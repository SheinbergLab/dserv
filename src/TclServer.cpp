#include "TclServer.h"
#include "TclCommands.h"
#include "ObjectRegistry.h"
#include "dserv.h"
#include <vector>

// JSON support
#include <jansson.h>

#include <fnmatch.h>  // Add this include for pattern matching


// our minified html pages (in www/*.html)
#include "embedded_terminal.h"
#include "embedded_datapoint_explorer.h"
#include "embedded_welcome.h"

// Vue gui with sources in from essgui-web/dist
#include "embedded_essgui_index_html.h"
#include "embedded_essgui_index_js.h"
#include "embedded_essgui_index_css.h"


static int process_requests(TclServer *tserv);
static Tcl_Interp *setup_tcl(TclServer *tserv);

TclServer::TclServer(int argc, char **argv, Dataserver *dserv,
                     std::string name, int port)
  : TclServer(argc, argv, dserv, TclServerConfig(name, port, -1))
{
}

TclServer::TclServer(int argc, char **argv, Dataserver *dserv,
                     std::string name, int newline_port, int message_port)
  : TclServer(argc, argv, dserv, TclServerConfig(name, newline_port, message_port))
{
}

// Add new constructor with WebSocket port
TclServer::TclServer(int argc, char **argv, Dataserver *dserv,
                     std::string name, int newline_port, int message_port, int websocket_port)
  : TclServer(argc, argv, dserv, TclServerConfig(name, newline_port, message_port, websocket_port))
{
}

TclServer::TclServer(int argc, char **argv,
             Dataserver *dserv, TclServerConfig cfg):
  argc(argc), argv(argv)
{
  m_bDone = false;
  ds = dserv;

  name = cfg.name;
  _newline_port = cfg.newline_listener_port;
  _message_port = cfg.message_listener_port;
  _websocket_port = cfg.websocket_listener_port;
   
  // create a connection to dataserver so we can subscribe to datapoints
  client_name = ds->add_new_send_client(&queue);

  // create a CR/LF tcp/ip listener if port is not -1
  if (newline_port() >= 0)
    newline_net_thread = std::thread(&TclServer::start_tcp_server, this);
  
  // create a message tcp/ip listener if port is not -1
  if (message_port() >= 0)
    message_net_thread = std::thread(&TclServer::start_message_server, this);
  
  // create a WebSocket listener if port is not -1
  if (websocket_port() >= 0) {
    std::cout << "Starting WebSocket server on port " << websocket_port() << std::endl;
    
    // Start the WebSocket server thread
    websocket_thread = std::thread(&TclServer::start_websocket_server, this);
  }
   
  // the process thread
  process_thread = std::thread(&process_requests, this);
}

TclServer::~TclServer()
{
  shutdown();
  
  if (websocket_port() > 0)
    websocket_thread.detach();
  
  if (message_port() > 0) 
    message_net_thread.detach();
    
  if (newline_port() > 0) 
    newline_net_thread.detach();
  
  process_thread.join();
}


void TclServer::shutdown(void)
{
  m_bDone = true;
  shutdown_message(&queue);
}

bool TclServer::isDone()
{
  return m_bDone;
}

void TclServer::start_tcp_server(void)
{
  struct sockaddr_in address;
  struct sockaddr client_address;
  socklen_t client_address_len = sizeof(client_address);
  int socket_fd;
  int new_socket_fd;        // client socket
  int on = 1;
  
  /* Initialise IPv4 address. */
  memset(&address, 0, sizeof(struct sockaddr_in));
  address.sin_family = AF_INET;
  address.sin_port = htons(newline_port());
  address.sin_addr.s_addr = INADDR_ANY;
  
  
  /* Create TCP socket. */
  if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    return;
  }

  /* Allow this server to reuse the port immediately */
  setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  
  /* Bind address to socket. */
  if (bind(socket_fd, (const struct sockaddr *) &address,
       sizeof (struct sockaddr)) == -1) {
    perror("bind");
    return;
  }
  
  /* Listen on socket. */
  if (listen(socket_fd, 20) == -1) {
    perror("listen");
    return;
  }

  while (!m_bDone) {
    /* Accept connection to client. */
    new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
    if (new_socket_fd == -1) {
      perror("accept");
      continue;
    }

    // Get client IP address
    std::string client_ip = get_client_ip(client_address);
    
    if (!accept_new_connection(client_ip)) {
       std::lock_guard<std::mutex> lock(connection_mutex);
       auto ip_count_it = ip_connection_count.find(client_ip);
       int current_ip_connections = (ip_count_it != ip_connection_count.end()) ? ip_count_it->second : 0;
       
       if (active_connections.load() >= MAX_TOTAL_CONNECTIONS) {
     std::cout << "Total connection limit reached (" << MAX_TOTAL_CONNECTIONS 
           << "), rejecting client from " << client_ip << std::endl;
       } else {
     std::cout << "Per-IP connection limit reached (" << MAX_CONNECTIONS_PER_IP 
           << "), rejecting client from " << client_ip 
           << " (current: " << current_ip_connections << ")" << std::endl;
       }
       close(new_socket_fd);
       continue;      
    }
        
    register_connection(new_socket_fd, client_ip);
    
    setsockopt(new_socket_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    
    std::thread thr(tcp_client_process, this, new_socket_fd, &queue);
    thr.detach();
  }
  
  close(socket_fd);
}


void TclServer::start_message_server(void)
{
  struct sockaddr_in address;
  struct sockaddr client_address;
  socklen_t client_address_len = sizeof(client_address);
  int socket_fd;
  int new_socket_fd;
  int on = 1;
  
  memset(&address, 0, sizeof(struct sockaddr_in));
  address.sin_family = AF_INET;
  address.sin_port = htons(message_port());
  address.sin_addr.s_addr = INADDR_ANY;

  if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    return;
  }

  setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  
  if (bind(socket_fd, (const struct sockaddr *) &address,
       sizeof (struct sockaddr)) == -1) {
    perror("bind");
    return;
  }
  
  if (listen(socket_fd, 20) == -1) {
    perror("listen");
    return;
  }

  while (!m_bDone) {
    /* Accept connection to client. */
    new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
    if (new_socket_fd == -1) {
      perror("accept");
      continue;
    }

    // Get client IP address
    std::string client_ip = get_client_ip(client_address);

    if (!accept_new_connection(client_ip)) {
      std::lock_guard<std::mutex> lock(connection_mutex);
      auto ip_count_it = ip_connection_count.find(client_ip);
      int current_ip_connections = (ip_count_it != ip_connection_count.end()) ? ip_count_it->second : 0;
      
      if (active_connections.load() >= MAX_TOTAL_CONNECTIONS) {
    std::cout << "Message server: Total connection limit reached (" << MAX_TOTAL_CONNECTIONS 
          << "), rejecting client from " << client_ip << std::endl;
      } else {
    std::cout << "Message server: Per-IP connection limit reached (" << MAX_CONNECTIONS_PER_IP 
          << "), rejecting client from " << client_ip 
          << " (current: " << current_ip_connections << ")" << std::endl;
      }
      close(new_socket_fd);
      continue;
    }
    
    setsockopt(new_socket_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    
    register_connection(new_socket_fd, client_ip);
    
    std::thread thr(message_client_process, this, new_socket_fd, &queue);
    thr.detach();
  }
  
  close(socket_fd);
}

// A method to get connection statistics
std::string TclServer::get_connection_stats() {
  std::lock_guard<std::mutex> lock(connection_mutex);
  std::ostringstream stats;
  stats << "Active connections: " << active_connections.load() 
    << "/" << MAX_TOTAL_CONNECTIONS << "\n";  // Changed from MAX_CONNECTIONS
  stats << "Per-IP limit: " << MAX_CONNECTIONS_PER_IP << "\n\n";
  
  stats << "Connections by IP:\n";
  for (const auto& [ip, count] : ip_connection_count) {
    stats << "  " << ip << ": " << count << "/" << MAX_CONNECTIONS_PER_IP;
    if (count >= MAX_CONNECTIONS_PER_IP * 0.8) {  // Warn at 80% of limit
      stats << " (WARNING: approaching limit)";
    }
    stats << "\n";
  }
  
  return stats.str();
}

void TclServer::start_websocket_server(void)
{
  ws_loop = uWS::Loop::get();
  
  auto app = uWS::App();

  // Helper macros for stringification
#define STRINGIFY(x) #x
#define EXPAND_AND_STRINGIFY(x) STRINGIFY(x)
  
  // Welcome page as the root
  app.get("/", [](auto *res, auto *req) {
    res->writeHeader("Content-Type", "text/html; charset=utf-8")
      ->writeHeader("Cache-Control", "no-cache")
      ->end(embedded::essgui_index_html);
  });

 // Build route strings with actual hash values
  std::string js_route = "/assets/index-" + std::string(EXPAND_AND_STRINGIFY(ESSGUI_JS_HASH)) + ".js";
  std::string css_route = "/assets/index-" + std::string(EXPAND_AND_STRINGIFY(ESSGUI_CSS_HASH)) + ".css";
    
  // Register the exact routes
  app.get(js_route, [](auto *res, auto *req) {
    res->writeHeader("Content-Type", "application/javascript; charset=utf-8")
      ->writeHeader("Cache-Control", "public, max-age=31536000") // Long cache since hash changes
      ->end(embedded::essgui_index_js);
  });

  app.get(css_route, [](auto *res, auto *req) {
    res->writeHeader("Content-Type", "text/css; charset=utf-8")
      ->writeHeader("Cache-Control", "public, max-age=31536000") // Long cache since hash changes
      ->end(embedded::essgui_index_css);
  });

#if 0
  // Debug output to verify routes
  std::cout << "Registered asset routes:" << std::endl;
  std::cout << "  JS: " << js_route << std::endl;
  std::cout << "  CSS: " << css_route << std::endl;
#endif  
  
  app.get("/terminal", [](auto *res, auto *req) {
    res->writeHeader("Content-Type", "text/html; charset=utf-8")
      ->writeHeader("Cache-Control", "no-cache")
      ->end(embedded::terminal_html);
  });

  app.get("/explorer", [](auto *res, auto *req) {
    res->writeHeader("Content-Type", "text/html; charset=utf-8")
      ->writeHeader("Cache-Control", "no-cache")
      ->end(embedded::datapoint_explorer_html);
  });
  
  // Add some aliases
  app.get("/datapoints", [](auto *res, auto *req) {
    res->writeStatus("302 Found")
      ->writeHeader("Location", "/explorer")
      ->end();
  });
  
  app.get("/console", [](auto *res, auto *req) {
    res->writeStatus("302 Found")
      ->writeHeader("Location", "/terminal")
      ->end();
  });  
  
  app.get("/health", [](auto *res, auto *req) {
    res->writeHeader("Content-Type", "application/json")
      ->end("{\"status\":\"ok\",\"service\":\"dserv-tclserver\"}");
  });
  
  // Favicon to prevent 404s
  app.get("/favicon.ico", [](auto *res, auto *req) {
    res->writeStatus("204 No Content")->end();
  });
  
  // WebSocket endpoint
  app.ws<WSPerSocketData>("/ws", {
      /* Settings */
      .compression = uWS::SHARED_COMPRESSOR,
    .maxPayloadLength = 24 * 1024 * 1024,
    .idleTimeout = 120,
    .maxBackpressure = 24 * 1024 * 1024,
      
    .upgrade = [](auto *res, auto *req, auto *context) {
      res->template upgrade<WSPerSocketData>({
          .rqueue = new SharedQueue<std::string>(),
          .client_name = "",
          .subscriptions = std::vector<std::string>(),
          .notification_queue = nullptr,
          .dataserver_client_id = ""
        }, req->getHeader("sec-websocket-key"),
        req->getHeader("sec-websocket-protocol"),
        req->getHeader("sec-websocket-extensions"),
        context);
    },
    
    .open = [this](auto *ws) {
      WSPerSocketData *userData = (WSPerSocketData *) ws->getUserData();
      
      if (!userData || !userData->rqueue) {
        std::cerr << "ERROR: Invalid userData in WebSocket open handler" << std::endl;
        ws->close();
        return;
      }

      try {
        // Create notification queue for this client
        userData->notification_queue = new SharedQueue<client_request_t>();
        
        // Register with Dataserver as a queue-based client
        userData->dataserver_client_id = this->ds->add_new_send_client(userData->notification_queue);
        
        if (userData->dataserver_client_id.empty()) {
          std::cerr << "Failed to register WebSocket client with Dataserver" << std::endl;
          delete userData->notification_queue;
          userData->notification_queue = nullptr;
          ws->close();
          return;
        }
        
        // Create a unique client name for this WebSocket
        char client_id[32];
        snprintf(client_id, sizeof(client_id), "ws_%p", (void*)ws);
        userData->client_name = std::string(client_id);
        
        // Store this WebSocket connection
        {
          std::lock_guard<std::mutex> lock(this->ws_connections_mutex);
          this->ws_connections[userData->client_name] = ws;
        }
        
        // Start a thread to process notifications for this client
        std::thread([this, ws, userData]() {
          this->process_websocket_client_notifications(ws, userData);
        }).detach();
        
        //      std::cout << "WebSocket client connected: " << userData->client_name << std::endl;
        
      } catch (const std::exception& e) {
        std::cerr << "Exception in WebSocket open handler: " << e.what() << std::endl;
        ws->close();
      }
    },
    
    .message = [this](auto *ws, std::string_view message, uWS::OpCode opCode) {
      WSPerSocketData *userData = (WSPerSocketData *) ws->getUserData();
  
      if (!userData || !userData->rqueue) {
        std::cerr << "ERROR: Invalid userData in WebSocket message handler!" << std::endl;
        ws->close();
        return;
      }
  
      // Handle JSON protocol for web clients
      if (message.length() > 0 && message[0] == '{') {
        // Create null-terminated string for jansson
        std::string json_str(message.data(), message.length());
          
        json_error_t error;
        json_t *root = json_loads(json_str.c_str(), 0, &error);
          
        if (!root) {
          json_t *error_response = json_object();
          json_object_set_new(error_response, "error", json_string("Invalid JSON"));
          char *error_str = json_dumps(error_response, 0);
          ws->send(error_str, uWS::OpCode::TEXT);
          free(error_str);
          json_decref(error_response);
          return;
        }
          
        json_t *cmd_obj = json_object_get(root, "cmd");
        if (!cmd_obj || !json_is_string(cmd_obj)) {
          json_t *error_response = json_object();
          json_object_set_new(error_response, "error", json_string("Missing 'cmd' field"));
          char *error_str = json_dumps(error_response, 0);
          ws->send(error_str, uWS::OpCode::TEXT);
          free(error_str);
          json_decref(error_response);
          json_decref(root);
          return;
        }
          
        const char *cmd = json_string_value(cmd_obj);
          
        if (strcmp(cmd, "eval") == 0) {
          // Handle Tcl script evaluation
          json_t *script_obj = json_object_get(root, "script");
          json_t *requestId_obj = json_object_get(root, "requestId"); // Get requestId

          if (script_obj && json_is_string(script_obj)) {
        const char *script = json_string_value(script_obj);
              
        // Create request
        client_request_t req;
        req.type = REQ_SCRIPT;
        req.rqueue = userData->rqueue;
        req.script = std::string(script);
              
        // Push to queue
        queue.push_back(req);
              
        // Wait for response
        std::string result = userData->rqueue->front();
        userData->rqueue->pop_front();
              
        // Create JSON response
        json_t *response = json_object();
        if (result.starts_with("!TCL_ERROR ")) {
          json_object_set_new(response, "status", json_string("error"));
          json_object_set_new(response, "error", json_string(result.substr(11).c_str()));
        } else {
          json_object_set_new(response, "status", json_string("ok"));
          json_object_set_new(response, "result", json_string(result.c_str()));
        }

        // If a requestId was provided, include it in the response
        if (requestId_obj && json_is_string(requestId_obj)) {
            json_object_set(response, "requestId", requestId_obj);
        }
              
        char *response_str = json_dumps(response, 0);
        ws->send(response_str, uWS::OpCode::TEXT);
        free(response_str);
        json_decref(response);
          }
        }

        else if (strcmp(cmd, "touch") == 0) {
          // Handle datapoint touch
          json_t *name_obj = json_object_get(root, "name");
          if (name_obj && json_is_string(name_obj)) {
        const char *name = json_string_value(name_obj);
        
        // Call the Dataserver touch method
        int found = ds->touch((char *)name);
        
        // Send response
        json_t *response = json_object();
        if (found) {
          json_object_set_new(response, "status", json_string("ok"));
          json_object_set_new(response, "action", json_string("touched"));
          json_object_set_new(response, "name", json_string(name));
        } else {
          json_object_set_new(response, "status", json_string("error"));
          json_object_set_new(response, "error", json_string("Datapoint not found"));
          json_object_set_new(response, "name", json_string(name));
        }
        
        char *response_str = json_dumps(response, 0);
        ws->send(response_str, uWS::OpCode::TEXT);
        free(response_str);
        json_decref(response);
          } else {
        json_t *error_response = json_object();
        json_object_set_new(error_response, "error", json_string("Missing or invalid 'name' field"));
        char *error_str = json_dumps(error_response, 0);
        ws->send(error_str, uWS::OpCode::TEXT);
        free(error_str);
        json_decref(error_response);
          }
        }       
        
        else if (strcmp(cmd, "subscribe") == 0) {
          json_t *match_obj = json_object_get(root, "match");
          json_t *every_obj = json_object_get(root, "every");
          
          if (match_obj && json_is_string(match_obj)) {
        const char *match = json_string_value(match_obj);
        int every = 1;
        if (every_obj && json_is_integer(every_obj)) {
          every = json_integer_value(every_obj);
        }
        
        // Store the subscription for this WebSocket client
        userData->subscriptions.push_back(std::string(match));
        
        // Register the match with Dataserver so we get notifications
        ds->client_add_match(userData->dataserver_client_id, (char*)match, every);
        
        // Send confirmation
        json_t *response = json_object();
        json_object_set_new(response, "status", json_string("ok"));
        json_object_set_new(response, "action", json_string("subscribed"));
        json_object_set_new(response, "match", json_string(match));
        
        char *response_str = json_dumps(response, 0);
        ws->send(response_str, uWS::OpCode::TEXT);
        free(response_str);
        json_decref(response);
          }
        }

        else if (strcmp(cmd, "unsubscribe") == 0) {
          json_t *match_obj = json_object_get(root, "match");
          if (match_obj && json_is_string(match_obj)) {
        const char *match = json_string_value(match_obj);
        
        // Remove from local subscriptions
        auto it = std::find(userData->subscriptions.begin(), userData->subscriptions.end(), match);
        if (it != userData->subscriptions.end()) {
          userData->subscriptions.erase(it);
        }
        
        // Remove from Dataserver
        this->ds->client_remove_match(userData->dataserver_client_id, (char*)match);
        
        // Send confirmation
        json_t *response = json_object();
        json_object_set_new(response, "status", json_string("ok"));
        json_object_set_new(response, "action", json_string("unsubscribed"));
        json_object_set_new(response, "match", json_string(match));
        
        char *response_str = json_dumps(response, 0);
        ws->send(response_str, uWS::OpCode::TEXT);
        free(response_str);
        json_decref(response);
          }
        }

        else if (strcmp(cmd, "list_subscriptions") == 0) {
          json_t *response = json_object();
          json_t *subs_array = json_array();
          
          for (const std::string& sub : userData->subscriptions) {
        json_array_append_new(subs_array, json_string(sub.c_str()));
          }
          
          json_object_set_new(response, "status", json_string("ok"));
          json_object_set_new(response, "subscriptions", subs_array);
          
          char *response_str = json_dumps(response, 0);
          ws->send(response_str, uWS::OpCode::TEXT);
          free(response_str);
          json_decref(response);
        }
        
        else if (strcmp(cmd, "get") == 0) {
          // Handle datapoint get
          json_t *name_obj = json_object_get(root, "name");
          if (name_obj && json_is_string(name_obj)) {
        const char *name = json_string_value(name_obj);
        ds_datapoint_t *dp = ds->get_datapoint((char *)name);
              
        if (dp) {
          char *json_str = dpoint_to_json(dp);
          ws->send(json_str, uWS::OpCode::TEXT);
          free(json_str);
          dpoint_free(dp);
        } else {
          json_t *error_response = json_object();
          json_object_set_new(error_response, "error", json_string("Datapoint not found"));
          char *error_str = json_dumps(error_response, 0);
          ws->send(error_str, uWS::OpCode::TEXT);
          free(error_str);
          json_decref(error_response);
        }
          }
        }
        else if (strcmp(cmd, "set") == 0) {
          // Handle datapoint set
          json_t *name_obj = json_object_get(root, "name");
          json_t *value_obj = json_object_get(root, "value");
            
          if (name_obj && json_is_string(name_obj) && value_obj && json_is_string(value_obj)) {
        const char *name = json_string_value(name_obj);
        const char *value = json_string_value(value_obj);
              
        ds->set((char *)name, (char *)value);
              
        json_t *response = json_object();
        json_object_set_new(response, "status", json_string("ok"));
        json_object_set_new(response, "action", json_string("set"));
              
        char *response_str = json_dumps(response, 0);
        ws->send(response_str, uWS::OpCode::TEXT);
        free(response_str);
        json_decref(response);
          }
        }   
        json_decref(root);
      }

      else {
        // Handle legacy text protocol (newline-terminated commands)
        std::string script(message);
          
        // Remove trailing newline if present
        if (!script.empty() && script.back() == '\n') {
          script.pop_back();
        }
          
        // Process as Tcl command
        client_request_t req;
        req.type = REQ_SCRIPT;
        req.rqueue = userData->rqueue;
        req.script = script;
          
        queue.push_back(req);
          
        std::string result = userData->rqueue->front();
        userData->rqueue->pop_front();
          
        // For text protocol, send plain response
        ws->send(result, uWS::OpCode::TEXT);
      }
    },
      
    .dropped = [](auto *ws, std::string_view message, uWS::OpCode opCode) {
      std::cerr << "WebSocket message dropped due to backpressure" << std::endl;
    },
      
    .drain = [](auto *ws) {
      if (ws->getBufferedAmount() > 1024 * 1024) {
        ws->close();
      }
    },
      
    .ping = [](auto *ws, std::string_view) {
      /* Not used, uWS automatically handles pings */
    },
      
    .pong = [](auto *ws, std::string_view) {
      /* Not used */
    },

    .close = [this](auto *ws, int code, std::string_view message) {
      WSPerSocketData *userData = (WSPerSocketData *) ws->getUserData();
      
      if (!userData) {
        return;
      }

      try {

        // Remove from active connections
        {
          std::lock_guard<std::mutex> lock(this->ws_connections_mutex);
          this->ws_connections.erase(userData->client_name);
        }
        
        // Remove from Dataserver's send_table
        if (!userData->dataserver_client_id.empty()) {
          this->ds->remove_send_client_by_id(userData->dataserver_client_id);
        }
        
        // Signal shutdown to the notification processing thread
        if (userData->notification_queue) {
          client_request_t shutdown_req;
          shutdown_req.type = REQ_SHUTDOWN;
          userData->notification_queue->push_back(shutdown_req);
          
          // Give the thread a moment to process the shutdown
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Clean up rqueue
        delete userData->rqueue;
        userData->rqueue = nullptr;
        
        //      std::cout << "WebSocket client disconnected: " << userData->client_name << std::endl;
        
      } catch (const std::exception& e) {
        std::cerr << "Exception in WebSocket close handler: " << e.what() << std::endl;
      }
    }   
    
    }).listen(websocket_port(), [this](auto *listen_socket) {
      if (listen_socket) {
        std::cout << "WebSocket server listening on port " << websocket_port() << std::endl;
        std::cout << "Web terminal available at http://localhost:" << websocket_port() << "/" << std::endl;
      } else {
        std::cerr << "Failed to start WebSocket server on port " << websocket_port() << std::endl;
      }
    }).run();
}

void TclServer::process_websocket_client_notifications(uWS::WebSocket<false, true, WSPerSocketData>* ws, WSPerSocketData* userData) {
    if (!userData) {
        std::cerr << "ERROR: userData is null in notification thread" << std::endl;
        return;
    }
    
    std::string client_name = userData->client_name; // Copy for safety
    bool done = false;
    
    while (!done) {
      try {
    if (!userData || !userData->notification_queue) {
      break;
    }
        
    client_request_t req = userData->notification_queue->front();
    userData->notification_queue->pop_front();
            
            if (req.type == REQ_SHUTDOWN) {
                done = true;
                break;
            }
            
            if (req.type == REQ_DPOINT_SCRIPT && req.dpoint) {

                // Check if WebSocket is still connected BEFORE processing
                if (!isWebSocketConnected(client_name, ws)) {
                    dpoint_free(req.dpoint);
                    done = true;
                    break;
                }
          
                // Check if this datapoint matches any of the client's subscriptions
                bool matches = false;
                std::string dpoint_name(req.dpoint->varname);
                
                for (const std::string& pattern : userData->subscriptions) {
                    if (pattern == "*") {
                        matches = true;
                    } else if (pattern.back() == '*') {
                        std::string prefix = pattern.substr(0, pattern.length() - 1);
                        matches = (strncmp(dpoint_name.c_str(), prefix.c_str(), prefix.length()) == 0);
                    } else {
                        matches = (strcmp(dpoint_name.c_str(), pattern.c_str()) == 0);
                    }
                    
                    if (matches) break;
                }

		if (matches) {
		  // Convert to JSON
		  char *json_str = dpoint_to_json(req.dpoint);
		  if (json_str) {
		    size_t json_size = strlen(json_str);
		    
		    // Log large messages
		    if (json_size > 500000) {
		      //		      std::cout << "Large datapoint: " << (json_size/1024) << "KB for " 
		      //				<< req.dpoint->varname << std::endl;
		    }
		    
		    json_error_t error;
		    json_t *root = json_loads(json_str, 0, &error);
		    if (root) {
		      json_object_set_new(root, "type", json_string("datapoint"));
		      char *enhanced_json = json_dumps(root, 0);
		      
		      // Create string for the message
		      std::string message(enhanced_json);
		      
		      // Use chunking-aware send method (handles both small and large)
		      sendLargeMessage(ws, message, client_name);
		      
		      free(enhanced_json);
		      json_decref(root);
		    }
		    free(json_str);
		  }
		}
                dpoint_free(req.dpoint);
            }
        } catch (...) {
            // Queue empty or other error
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Cleanup
    if (userData && userData->notification_queue) {
        delete userData->notification_queue;
        userData->notification_queue = nullptr;
    }
}

void TclServer::sendLargeMessage(uWS::WebSocket<false, true, WSPerSocketData>* ws,
                               const std::string& message,
                               const std::string& client_name) {
    if (message.size() <= LARGE_MESSAGE_THRESHOLD) {
        // Small message, send directly
        if (ws_loop) {
            ws_loop->defer([ws, message, client_name, this]() {
                std::lock_guard<std::mutex> lock(this->ws_connections_mutex);
                auto it = this->ws_connections.find(client_name);
                if (it != this->ws_connections.end() && it->second == ws) {
                    ws->send(message, uWS::OpCode::TEXT);
                }
            });
        }
        return;
    }
    
    // Large message - chunk it
    //    std::cout << "Chunking large message: " << (message.size() / 1024) << "KB" << std::endl;
    
    // Generate unique message ID
    auto now = std::chrono::steady_clock::now();
    std::string messageId = std::to_string(now.time_since_epoch().count());
    
    size_t totalChunks = (message.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
    
    for (size_t i = 0; i < totalChunks; i++) {
        size_t start = i * CHUNK_SIZE;
        size_t end = std::min(start + CHUNK_SIZE, message.size());
        std::string chunk = message.substr(start, end - start);
        
        // Create chunked message
        json_t *chunked = json_object();
        json_object_set_new(chunked, "isChunkedMessage", json_true());
        json_object_set_new(chunked, "messageId", json_string(messageId.c_str()));
        json_object_set_new(chunked, "chunkIndex", json_integer(i));
        json_object_set_new(chunked, "totalChunks", json_integer(totalChunks));
        json_object_set_new(chunked, "data", json_string(chunk.c_str()));
        json_object_set_new(chunked, "isLastChunk", json_boolean(i == totalChunks - 1));
        
        char *chunk_str = json_dumps(chunked, 0);
        std::string chunk_message(chunk_str);
        
        // Send chunk with proper defer
        if (ws_loop) {
            ws_loop->defer([ws, chunk_message, client_name, this, i]() {
                std::lock_guard<std::mutex> lock(this->ws_connections_mutex);
                auto it = this->ws_connections.find(client_name);
                if (it != this->ws_connections.end() && it->second == ws) {
                    bool sent = ws->send(chunk_message, uWS::OpCode::TEXT);
                    if (!sent) {
                        std::cerr << "Failed to send chunk " << i << std::endl;
                    }
                }
            });
        }
        
        free(chunk_str);
        json_decref(chunked);
        
        // Small delay between chunks to prevent overwhelming
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    
    //    std::cout << "Sent " << totalChunks << " chunks for message " << messageId << std::endl;
}

/********************************* now *********************************/

static int now_command (ClientData data, Tcl_Interp *interp,
                int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;

  
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(ds->now()));
  return TCL_OK;
}

/***************************** eval_noreply ***************************/

static int eval_noreply_command (ClientData data, Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[])
{
  TclServer *this_server = (TclServer *) data;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "script");
    return TCL_ERROR;
  }

  /*
   * note: script should be contained in braces as single argument
   *       to prevent any unwanted Tcl parsing!
   */
  client_request_t client_request;
  client_request.type = REQ_SCRIPT_NOREPLY;
  client_request.script = std::string(Tcl_GetString(objv[1]));

  this_server->queue.push_back(client_request);

  /* don't wait for a reply to the message, just return */
  return TCL_OK;
}

/********************************* send ********************************/

static int send_command (ClientData data, Tcl_Interp *interp,
                 int objc, Tcl_Obj *objv[])
{
  TclServer *this_server = (TclServer *) data;

  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "server message");
    return TCL_ERROR;
  }

  auto tclserver = TclServerRegistry.getObject(Tcl_GetString(objv[1]));
  if (!tclserver) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": server \"", Tcl_GetString(objv[1]), "\" not found",
                     NULL);
    return TCL_ERROR;
  }

  if (tclserver == this_server) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": cannot send message to self", NULL);
    return TCL_ERROR;
  }

  // Concatenate all arguments from objv[2] to objv[objc-1] into a single string
  std::string concatenated_script;
  if (objc > 2) { // Only proceed if there are arguments to concatenate
    concatenated_script += Tcl_GetString(objv[2]); // Add the first argument without a leading space
    for (int i = 3; i < objc; ++i) {
      concatenated_script += " "; // Add a space
      concatenated_script += Tcl_GetString(objv[i]); // Then add the next argument
    }
  }

  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.type = REQ_SCRIPT;
  client_request.rqueue = &rqueue;
  client_request.script = concatenated_script; // Use the concatenated string

  tclserver->queue.push_back(client_request);

  /* rqueue will be available after command has been processed */
  /* NOTE: this can create a deadlock between two tclservers   */

  std::string s(client_request.rqueue->front());
  client_request.rqueue->pop_front();

  Tcl_SetObjResult(interp, Tcl_NewStringObj(s.c_str(), -1));
  return TCL_OK;
}

/***************************** send_noreply ****************************/

static int send_noreply_command (ClientData data, Tcl_Interp *interp,
                 int objc, Tcl_Obj *objv[])
{
  TclServer *this_server = (TclServer *) data;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "server message");
    return TCL_ERROR;
  }
    
  auto tclserver = TclServerRegistry.getObject(Tcl_GetString(objv[1]));
  if (!tclserver) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
             ": server \"", Tcl_GetString(objv[1]), "\" not found",
             NULL);
    return TCL_ERROR;
  }

  if (tclserver == this_server) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
             ": cannot send message to self", NULL);
    return TCL_ERROR;
  }

  client_request_t client_request;
  client_request.type = REQ_SCRIPT_NOREPLY;
  client_request.script = std::string(Tcl_GetString(objv[2]));

  tclserver->queue.push_back(client_request);

  /* don't wait for a reply to the message, just return */
  return TCL_OK;
}


/******************************* process *******************************/

static int subprocess_command (ClientData data, Tcl_Interp *interp,
                   int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  int port;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "name port [startup_script]");
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[2], &port) != TCL_OK) {
    return TCL_ERROR;
  }

  TclServer *child = new TclServer(tclserver->argc, tclserver->argv,
                   tclserver->ds,
                   Tcl_GetString(objv[1]), port);
  TclServerRegistry.registerObject(Tcl_GetString(objv[1]), child);

  if (objc > 2) {
    std::string script = std::string(Tcl_GetString(objv[3]));
    auto result = child->eval(script);
    if (result.starts_with("!TCL_ERROR ")) {
      Tcl_AppendResult(interp, result.c_str(), NULL);
      delete child;
      return TCL_ERROR;
    }
  }
  
  Tcl_SetObjResult(interp, Tcl_NewStringObj(child->client_name.c_str(), -1));
  
  return TCL_OK;
}


static int getsubprocesses_command(ClientData clientData, Tcl_Interp *interp, 
                   int objc, Tcl_Obj *const objv[])
{
  try {
    // Get all object names 
    auto allObjects = TclServerRegistry.getAllObjects();
    std::vector<std::string> names;
    names.reserve(allObjects.size());
    for (const auto& pair : allObjects) {
      names.push_back(pair.first);
    }    
    
    // Create a new Tcl dictionary
    Tcl_Obj *dictObj = Tcl_NewDictObj();
    
    // Iterate through all objects
    for (const std::string& name : names) {
      TclServer* obj = TclServerRegistry.getObject(name);
      if (obj != nullptr) {
    // Create key (object name)
    Tcl_Obj *keyObj = Tcl_NewStringObj(name.c_str(), -1);
    Tcl_Obj *valueObj = Tcl_NewIntObj(obj->newline_port());
        
    // Add key-value pair to dictionary
    if (Tcl_DictObjPut(interp, dictObj, keyObj, valueObj) != TCL_OK) {
      return TCL_ERROR;
    }
      }
    }  
    
    // Set the result
    Tcl_SetObjResult(interp, dictObj);
    return TCL_OK;
    
  } catch (const std::exception& e) {
    Tcl_SetResult(interp, const_cast<char*>(e.what()), TCL_VOLATILE);
    return TCL_ERROR;
  }
}


static int dserv_add_match_command(ClientData data, Tcl_Interp * interp,
                       int objc,
                       Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  Tcl_Obj *obj;
  int every = 1;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname [every]");
    return TCL_ERROR;
  }
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &every) != TCL_OK) {
      return TCL_ERROR;
    }
  }

  ds->client_add_match(tclserver->client_name, Tcl_GetString(objv[1]), every);
  return TCL_OK;
}

static int dserv_add_exact_match_command(ClientData data, Tcl_Interp * interp,
                         int objc,
                         Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  Tcl_Obj *obj;
  int every = 1;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname [every]");
    return TCL_ERROR;
  }
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &every) != TCL_OK) {
      return TCL_ERROR;
    }
  }
  
  ds->client_add_exact_match(tclserver->client_name, Tcl_GetString(objv[1]), every);

  return TCL_OK;
}

static int dserv_remove_match_command(ClientData data, Tcl_Interp * interp,
                      int objc,
                      Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  Tcl_Obj *obj;
  int every = 1;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  ds->client_remove_match(tclserver->client_name, Tcl_GetString(objv[1]));
  return TCL_OK;
}

static int dserv_remove_all_matches_command(ClientData data,
                        Tcl_Interp * interp,
                        int objc,
                        Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  ds->client_remove_all_matches(tclserver->client_name);
  return TCL_OK;
}


static int dserv_logger_clients_command(ClientData data, Tcl_Interp *interp,
                        int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  std::string clients;
  
  clients = ds->get_logger_clients();
  Tcl_SetObjResult(interp, Tcl_NewStringObj(clients.data(), clients.size()));
    
  return TCL_OK;
}


static int dserv_log_open_command(ClientData data, Tcl_Interp *interp,
                 int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  int overwrite = 0;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "path [overwrite]");
    return TCL_ERROR;
  }
  
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &overwrite) != TCL_OK)
      return TCL_ERROR;
  }
  
  status = ds->logger_client_open(Tcl_GetString(objv[1]), overwrite);
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

static int dserv_log_close_command(ClientData data, Tcl_Interp *interp,
                       int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "path");
    return TCL_ERROR;
  }
  
  status = ds->logger_client_close(Tcl_GetString(objv[1]));
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

static int dserv_log_pause_command(ClientData data, Tcl_Interp *interp,
                       int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "path");
    return TCL_ERROR;
  }
  
  status = ds->logger_client_pause(Tcl_GetString(objv[1]));
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

static int dserv_log_start_command(ClientData data, Tcl_Interp *interp,
                       int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "path");
    return TCL_ERROR;
  }
  
  status = ds->logger_client_start(Tcl_GetString(objv[1]));
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

static int dserv_log_add_match_command(ClientData data, Tcl_Interp *interp,
                       int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  int obs_limited = 0;
  int buffer_size = 0;
  int every = 1;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv,
             "path match [obs_limited buffer_size every]");
    return TCL_ERROR;
  }
  
  if (objc > 3) {
    if (Tcl_GetIntFromObj(interp, objv[3], &obs_limited) != TCL_OK)
      return TCL_ERROR;
  }
  
  if (objc > 4) {
    if (Tcl_GetIntFromObj(interp, objv[4], &buffer_size) != TCL_OK)
      return TCL_ERROR;
  }
  
  if (objc > 5) {
    if (Tcl_GetIntFromObj(interp, objv[5], &every) != TCL_OK)
      return TCL_ERROR;
  }

  if (every <= 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
             ": invalid \"every\" argument",
             NULL);
    return TCL_ERROR;
  }
  if (buffer_size < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
             ": invalid buffer_size argument",
             NULL);
    return TCL_ERROR;
  }
  status = ds->logger_add_match(Tcl_GetString(objv[1]),
                Tcl_GetString(objv[2]),
                every, obs_limited, buffer_size);
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

static int dpoint_set_script_command (ClientData data, Tcl_Interp *interp,
                      int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname script");
    return TCL_ERROR;
  }
  
  tclserver->dpoint_scripts.insert(std::string(Tcl_GetString(objv[1])),
                   std::string(Tcl_GetString(objv[2])));
  
  return TCL_OK;
}

static int dpoint_remove_script_command (ClientData data, Tcl_Interp *interp,
                         int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  
  tclserver->dpoint_scripts.remove(std::string(Tcl_GetString(objv[1])));
  
  return TCL_OK;
  }

static int dpoint_remove_all_scripts_command (ClientData data,
                          Tcl_Interp *interp,
                          int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  tclserver->dpoint_scripts.clear();
  return TCL_OK;
}

static int print_command (ClientData data, Tcl_Interp *interp,
              int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;

  ds_datapoint_t dpoint;
  char *s;
  Tcl_Size len;
  int rc;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "string");
    return TCL_ERROR;
  }
  
  s = Tcl_GetStringFromObj(objv[1], &len);
  if (!s) return TCL_ERROR;
  
  /* fill the data point */
  dpoint_set(&dpoint, (char *) tclserver->PRINT_DPOINT_NAME, 
         tclserver->ds->now(), DSERV_STRING, len, (unsigned char *) s);
  
  /* send to dserv */
  tclserver->ds->set(dpoint);
  
  return TCL_OK;
}

static void add_tcl_commands(Tcl_Interp *interp, TclServer *tserv)
{
  /* use the generic Dataserver commands for these */
  Tcl_CreateObjCommand(interp, "dpointExists",
		       dserv_exists_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservExists",
		       dserv_exists_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dpointGet",
		       dserv_get_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservGet",
		       dserv_get_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservCopy",
		       dserv_copy_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSet",
		       dserv_set_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservTouch",
		       dserv_touch_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservTimestamp",
		       dserv_timestamp_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSetData",
		       dserv_setdata_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSetData64",
		       dserv_setdata64_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservClear",
		       dserv_clear_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservEval",
		       dserv_eval_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservKeys",
		       dserv_keys_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservDGDir",
		       dserv_dgdir_command, tserv->ds, NULL);

  Tcl_CreateObjCommand(interp, "processGetParam",
               process_get_param_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "processSetParam",
               process_set_param_command, tserv->ds, NULL);

  /* these are specific to TclServers */
  Tcl_CreateObjCommand(interp, "now",
               (Tcl_ObjCmdProc *) now_command,
               tserv, NULL);

  Tcl_CreateObjCommand(interp, "subprocess",
               (Tcl_ObjCmdProc *) subprocess_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "subprocessInfo",
               (Tcl_ObjCmdProc *) getsubprocesses_command,
               tserv, NULL);

  Tcl_CreateObjCommand(interp, "evalNoReply",
               (Tcl_ObjCmdProc *) eval_noreply_command,
               tserv, NULL);
  
  Tcl_CreateObjCommand(interp, "send",
               (Tcl_ObjCmdProc *) send_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "sendNoReply",
               (Tcl_ObjCmdProc *) send_noreply_command,
               tserv, NULL);
                  
  Tcl_CreateObjCommand(interp, "dservAddMatch",
               dserv_add_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservAddExactMatch",
               dserv_add_exact_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveMatch",
               dserv_remove_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveAllMatches",
               dserv_remove_all_matches_command, tserv, NULL);
  
  Tcl_CreateObjCommand(interp, "dservLoggerClients",
               dserv_logger_clients_command, tserv, NULL);

  Tcl_CreateObjCommand(interp, "dservLoggerOpen",
               dserv_log_open_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerClose",
               dserv_log_close_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerPause",
               dserv_log_pause_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerStart",
               dserv_log_start_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerResume",
               dserv_log_start_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerAddMatch",
               dserv_log_add_match_command, tserv, NULL);
  
  Tcl_CreateObjCommand(interp, "dpointSetScript",
               (Tcl_ObjCmdProc *) dpoint_set_script_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "dpointRemoveScript",
               (Tcl_ObjCmdProc *) dpoint_remove_script_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "dpointRemoveAllScripts",
               (Tcl_ObjCmdProc *) dpoint_remove_all_scripts_command,
               tserv, NULL);

  Tcl_CreateObjCommand(interp, "print",
               (Tcl_ObjCmdProc *) print_command, tserv, NULL);
  
  Tcl_LinkVar(interp, "tcpPort", (char *) &tserv->_newline_port,
          TCL_LINK_INT | TCL_LINK_READ_ONLY);
  return;
}

static int Tcl_DservAppInit(Tcl_Interp *interp, TclServer *tserv)
{
  if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;
  
  add_tcl_commands(interp, tserv);
  
  if (tserv->hasCommandCallback()) {
      tserv->callCommandCallback(interp);
  }
    
  return TCL_OK;
}

static Tcl_Interp *setup_tcl(TclServer *tserv)
{
  Tcl_Interp *interp;
  
  Tcl_FindExecutable(tserv->argv[0]);
  interp = Tcl_CreateInterp();
  if (!interp) {
    std::cerr << "Error initialializing tcl interpreter" << std::endl;
    return interp;
  }
#if 0
  if (TclZipfs_Mount(interp, "/usr/local/dserv/tclserver.zip", "app", NULL) != TCL_OK) {
    //    std::cerr << "Tclserver: error mounting zipfs" << std::endl;
  }
  else {
    //    std::cerr << "Mounted zipfs" << std::endl;
  }
#endif
  
  TclZipfs_AppHook(&tserv->argc, &tserv->argv);

  
  /*
   * Invoke application-specific initialization.
   */
  
  if (Tcl_DservAppInit(interp, tserv) != TCL_OK) {
    std::cerr << "application-specific initialization failed: ";
    std::cerr << Tcl_GetStringResult(interp) << std::endl;
  }
  else {
    Tcl_SourceRCFile(interp);
  }
  
  return interp;
}

/* queue up a point to be set from other threads */
void TclServer::set_point(ds_datapoint_t *dp)
{
  client_request_t req;
  req.type = REQ_DPOINT;
  req.dpoint = dp;
  queue.push_back(req);
}

/*
 * run a tcl script for give datapoint
 */
static int dpoint_tcl_script(Tcl_Interp *interp,
                 const char *script,
                 ds_datapoint_t *dpoint)
{
  Tcl_Obj *commandArray[3];
  commandArray[0] = Tcl_NewStringObj(script, -1);
      
  /* name of dpoint (special for DSERV_EVTs */
  if (dpoint->data.e.dtype != DSERV_EVT) {
    commandArray[1] = Tcl_NewStringObj(dpoint->varname,
                       dpoint->varlen);
    /* data as Tcl_Obj */
    commandArray[2] = dpoint_to_tclobj(interp, dpoint);
  }
  else {
    /* convert eventlog/events -> evt:TYPE:SUBTYPE notation */
    char evt_namebuf[32];
    snprintf(evt_namebuf, sizeof(evt_namebuf), "evt:%d:%d",
         dpoint->data.e.type, dpoint->data.e.subtype);
    commandArray[1] = Tcl_NewStringObj(evt_namebuf, -1);
    
    /* create a placeholder for repackaged dpoint */
    ds_datapoint_t e_dpoint;
    e_dpoint.data.type = (ds_datatype_t) dpoint->data.e.puttype;
    e_dpoint.data.len = dpoint->data.len;
    e_dpoint.data.buf = dpoint->data.buf;
    
    /* data as Tcl_Obj */
    commandArray[2] = dpoint_to_tclobj(interp, &e_dpoint);
  }
  /* incr ref count on command arguments */
  for (int i = 0; i < 3; i++) { Tcl_IncrRefCount(commandArray[i]); }
  
  /* call command */
  int retcode = Tcl_EvalObjv(interp, 3, commandArray, 3);
  
  /* decr ref count on command arguments */
  for (int i = 0; i < 3; i++) { Tcl_DecrRefCount(commandArray[i]); }
  return retcode;
}

static int process_requests(TclServer *tserv)
{
  int retcode;
  client_request_t req;

  // create a private interpreter for this process
  Tcl_Interp *interp = setup_tcl(tserv);
  
  /* process until receive a message saying we are done */
  while (!tserv->m_bDone) {
    
    req = tserv->queue.front();
    tserv->queue.pop_front();
    
    switch (req.type) {
    case REQ_SCRIPT:
      {
    const char *script = req.script.c_str();

    retcode = Tcl_Eval(interp, script);
    const char *rcstr = Tcl_GetStringResult(interp);
    
    if (retcode == TCL_OK) {
      if (rcstr) {
        req.rqueue->push_back(std::string(rcstr));
      }
      else {
        req.rqueue->push_back("");
      }
    }
    else {
      if (rcstr) {
        req.rqueue->push_back("!TCL_ERROR "+std::string(rcstr));
        //      std::cout << "Error: " + std::string(rcstr) << std::endl;
        
      }
      else {
        req.rqueue->push_back("Error:");
      }
    }
      }

      break;
    case REQ_SCRIPT_NOREPLY:
      {
    const char *script = req.script.c_str();
    retcode = Tcl_Eval(interp, script);
      }
      break;
    case REQ_DPOINT:
      {
    tserv->ds->set(req.dpoint);
      }
      break;
    case REQ_DPOINT_SCRIPT:
      {
    ds_datapoint_t *dpoint = req.dpoint;
    std::string script;
    std::string varname(dpoint->varname);
    // evaluate a dpoint script

    if (tserv->dpoint_scripts.find(varname, script)) {
      ds_datapoint_t *dpoint = req.dpoint;
      const char *dpoint_script = script.c_str();
      int retcode = dpoint_tcl_script(interp, dpoint_script, dpoint);
    }
    else if (tserv->dpoint_scripts.find_match(varname, script)) {
      ds_datapoint_t *dpoint = req.dpoint;
      const char *dpoint_script = script.c_str();
      int retcode = dpoint_tcl_script(interp, dpoint_script, dpoint);
    }

    dpoint_free(dpoint);
      }
    default:
      break;
    }
  }

  Tcl_DeleteInterp(interp);
  //  std::cout << "TclServer process thread ended" << std::endl;

  return 0;
}
  
int TclServer::queue_size(void)
{
  return queue.size();
}

void TclServer::shutdown_message(SharedQueue<client_request_t> *q)
{
  client_request_t client_request;
  client_request.type = REQ_SHUTDOWN;
  q->push_back(client_request);
}

std::string TclServer::eval(char *s)
{
  std::string script(s);
  return eval(script);
}

std::string TclServer::eval(std::string script)
{
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.type = REQ_SCRIPT;
  client_request.rqueue = &rqueue;
  client_request.script = script;
  
  queue.push_back(client_request);

  /* rqueue will be available after command has been processed */
  std::string s(client_request.rqueue->front());
  client_request.rqueue->pop_front();
  
  return s;
}

void TclServer::eval_noreply(char *s)
{
  std::string script(s);
  eval_noreply(script);
}

void TclServer::eval_noreply(std::string script)
{
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.type = REQ_SCRIPT_NOREPLY;
  client_request.script = script;
  
  queue.push_back(client_request);
}

/*
 * tcp_client_process is CR/LF oriented
 *  incoming messages are terminated by newlines and responses append these
 */
void
TclServer::tcp_client_process(TclServer *tserv,
                  int sockfd,
                  SharedQueue<client_request_t> *queue)
{
  int rval;
  int wrval;
  char buf[1024];
  
  // each client has its own request structure and reply queue
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.rqueue = &rqueue;
  client_request.type = REQ_SCRIPT;

  std::string script;  
    
  while ((rval = recv(sockfd, buf, sizeof(buf), 0)) > 0) {
    for (int i = 0; i < rval; i++) {
      char c = buf[i];
      if (c == '\n') {
    // shutdown if main server has shutdown
    if (tserv->m_bDone) break;
    
    if (script.length() > 0) {
      std::string s;
      client_request.script = std::string(script);

      // ignore certain commands, especially exit...
      if (!client_request.script.compare(0, 4, "exit")) {
        s = std::string("");
      } else {
        // push request onto queue for main thread to retrieve
        queue->push_back(client_request);
        
        // rqueue will be available after command has been processed
        s = client_request.rqueue->front();
        client_request.rqueue->pop_front();
        
        //    std::cout << "TCL Result: " << s << std::endl;
      }
      // Add a newline, and send the buffer including the null termination
      s = s+"\n";
#ifndef _MSC_VER
      wrval = write(sockfd, s.c_str(), s.size());
#else
      wrval = send(sockfd, s.c_str(), s.size(), 0);
#endif
      if (wrval < 0) {      // couldn't send to client
        break;
      }
    }
    script = "";
      }
      else {
    script += c;
      }
    }
  }

  // close and unregister for proper limit tracking
  tserv->unregister_connection(sockfd);
}

static  void sendMessage(int socket, const std::string& message) {
  // Convert size to network byte order
  uint32_t msgSize = htonl(message.size()); 

  send(socket, (char *) &msgSize, sizeof(msgSize), 0);
  send(socket, message.c_str(), message.size(), 0);
}

static std::pair<char*, size_t> receiveMessage(int socket) {
    uint32_t msgSize;
    // Receive the size of the message
    ssize_t bytesReceived = recv(socket, (char *) &msgSize,
                 sizeof(msgSize), 0);
    if (bytesReceived <= 0) return {nullptr, 0};

    // Convert size from network byte order to host byte order
    msgSize = ntohl(msgSize); 

    // Allocate and zero buffer for the message
    char* buffer = new char[msgSize+1]{};
    size_t totalBytesReceived = 0;
    while (totalBytesReceived < msgSize) {
        bytesReceived = recv(socket, buffer + totalBytesReceived,
                 msgSize - totalBytesReceived, 0);
        if (bytesReceived <= 0) {
      delete[] buffer;
      return {nullptr, 0}; // Connection closed or error
        }
        totalBytesReceived += bytesReceived;
    }
    
    return {buffer, msgSize};
}

/*
 * message_client_process is frame oriented with 32 size following by bytes
 *  response is similarly organized
 */
void
TclServer::message_client_process(TclServer *tserv,
                    int sockfd,
                  SharedQueue<client_request_t> *queue)
{
  int rval;
  int wrval;
  
  // each client has its own request structure and reply queue
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.rqueue = &rqueue;
  client_request.type = REQ_SCRIPT;
  
  std::string script;  

  while (true) {
    auto [buffer, msgSize] = receiveMessage(sockfd);
    if (buffer == nullptr) break;
    if (msgSize) {

      // shutdown if main server has shutdown
      if (tserv->m_bDone) break;

      client_request.script = std::string(buffer);
      std::string s;
      
      // ignore certain commands, especially exit...
      if (!client_request.script.compare(0, 4, "exit")) {
    s = std::string("");
      } else {
    // push request onto queue for main thread to retrieve
    queue->push_back(client_request);
    
    // rqueue will be available after command has been processed
    s = client_request.rqueue->front();
    client_request.rqueue->pop_front();
    //  std::cout << "TCL Result: " << s << std::endl;
      }

      // Send a response back to the client
      sendMessage(sockfd, s);
      
      delete[] buffer;
    }
  }
  tserv->unregister_connection(sockfd);
}

