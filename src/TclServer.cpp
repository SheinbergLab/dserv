#include "TclServer.h"
#include "TclCommands.h"
#include "ObjectRegistry.h"
#include "dserv.h"
#include "dservConfig.h"
#include <vector>
#include <filesystem>
#include <fstream>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

// JSON support
#include <jansson.h>

#include <fnmatch.h>  // pattern matching support

#include "TclCompletion.h"

static int process_requests(TclServer *tserv);
static Tcl_Interp *setup_tcl(TclServer *tserv);

// For one off subprocesses don't need name
TclServer::TclServer(int argc, char **argv, Dataserver *dserv)
  : TclServer(argc, argv, dserv, TclServerConfig("", -1, -1, -1))
{
}
// For no-network subprocess
TclServer::TclServer(int argc, char **argv, Dataserver *dserv, std::string name)
  : TclServer(argc, argv, dserv, TclServerConfig(name, -1, -1, -1))
{
}

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
  www_path = cfg.www_path;

  exports_path = cfg.exports_path;
  if (exports_path.empty()) {
    if (!www_path.empty()) {
      std::filesystem::path p(www_path);
      exports_path = (p.parent_path() / "exports").string();
    } else {
      exports_path = "/tmp/dserv_exports";
    }
  }
  std::filesystem::create_directories(exports_path); 
  
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
  delete eventDispatcher;
  
  shutdown();

  if (websocket_port() >= 0)
    websocket_thread.detach();
  
  if (message_port() >= 0) 
    message_net_thread.detach();
    
  if (newline_port() >= 0) 
    newline_net_thread.detach();
  
  process_thread.join();
}

void TclServer::setPriority(int priority) {
#ifdef __linux__
    struct sched_param param;
    param.sched_priority = priority;
    
    // Set priority for the process thread (most critical)
    if (process_thread.joinable()) {
        int result = pthread_setschedparam(process_thread.native_handle(), SCHED_FIFO, &param);
        if (result != 0) {
            std::cerr << "Warning: Failed to set process thread priority: " << strerror(result) << std::endl;
        }
    }
    
    // Lower priority for network threads (if they exist)
    param.sched_priority = std::max(1, priority - 1);  // Ensure priority >= 1
    if (newline_net_thread.joinable()) {
        pthread_setschedparam(newline_net_thread.native_handle(), SCHED_FIFO, &param);
    }
    if (message_net_thread.joinable()) {
        pthread_setschedparam(message_net_thread.native_handle(), SCHED_FIFO, &param);
    }
    
    std::cout << "Set thread priorities for " << name << " (priority: " << priority << ")" << std::endl;
    
#elif defined(_WIN32)
    // Windows implementation if needed
    std::cout << "Thread priority setting not implemented on Windows" << std::endl;
    
#else
    // macOS, other Unix variants
    std::cout << "Thread priority setting not implemented on this platform" << std::endl;
    
#endif
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


// Content-type mapping for static file serving
static const char* get_content_type(const std::string& path) {
  size_t dot_pos = path.rfind('.');
  if (dot_pos == std::string::npos) return "application/octet-stream";
  
  std::string ext = path.substr(dot_pos);
  
  // HTML
  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  
  // JavaScript
  if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
  
  // CSS
  if (ext == ".css") return "text/css; charset=utf-8";
  
  // JSON
  if (ext == ".json") return "application/json; charset=utf-8";
  
  // Images
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".webp") return "image/webp";
  
  // Fonts
  if (ext == ".woff") return "font/woff";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".ttf") return "font/ttf";
  
  // Other
  if (ext == ".txt") return "text/plain; charset=utf-8";
  if (ext == ".md") return "text/markdown; charset=utf-8";
  if (ext == ".pem") return "application/x-pem-file";
  if (ext == ".crt") return "application/x-x509-ca-cert";  
  if (ext == ".xml") return "application/xml";
  if (ext == ".wasm") return "application/wasm";
  
  return "application/octet-stream";
}

static bool is_safe_path(const std::string& path) {
  return path.find("..") == std::string::npos;
}

static std::string read_file_contents(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return "";
  
  auto size = file.tellg();
  if (size <= 0) return "";
  
  file.seekg(0);
  std::string content(static_cast<size_t>(size), '\0');
  file.read(content.data(), size);
  
  return content;
}


// Efficient file streaming for large downloads
// Uses uWebSockets async pattern to avoid loading entire file in memory
static void stream_file_response(auto *res, const std::string& file_path, 
                                  const std::string& content_type,
                                  const std::string& filename) {
    // Check file exists and get size
    struct stat st;
    if (stat(file_path.c_str(), &st) != 0) {
        res->writeStatus("404 Not Found")
           ->writeHeader("Content-Type", "text/plain")
           ->end("File not found");
        return;
    }
    
    size_t file_size = st.st_size;
    
    // For small files (< 10MB), read and send directly
    const size_t SMALL_FILE_THRESHOLD = 10 * 1024 * 1024;
    
    if (file_size < SMALL_FILE_THRESHOLD) {
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            res->writeStatus("500 Internal Server Error")
               ->end("Failed to open file");
            return;
        }
        
        std::string content(file_size, '\0');
        file.read(content.data(), file_size);
        
        res->writeHeader("Content-Type", content_type)
           ->writeHeader("Content-Disposition", 
                        "attachment; filename=\"" + filename + "\"")
           ->writeHeader("Content-Length", std::to_string(file_size))
           ->writeHeader("Cache-Control", "no-cache")
           ->end(content);
        return;
    }
    
    // For large files, use chunked streaming
    // Note: This is a simplified version. For production, you might want
    // to use uWS::HttpResponse::tryEnd() with onWritable callback for
    // true async streaming of very large files.
    
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        res->writeStatus("500 Internal Server Error")
           ->end("Failed to open file");
        return;
    }
    
    // Read in chunks
    const size_t CHUNK_SIZE = 64 * 1024;  // 64KB chunks
    std::string content;
    content.reserve(file_size);
    
    char buffer[CHUNK_SIZE];
    while (file.read(buffer, CHUNK_SIZE) || file.gcount() > 0) {
        content.append(buffer, file.gcount());
    }
    
    res->writeHeader("Content-Type", content_type)
       ->writeHeader("Content-Disposition", 
                    "attachment; filename=\"" + filename + "\"")
       ->writeHeader("Content-Length", std::to_string(file_size))
       ->writeHeader("Cache-Control", "no-cache")
       ->end(content);
}

// Get content type for data files
static const char* get_download_content_type(const std::string& path) {
    size_t dot_pos = path.rfind('.');
    if (dot_pos == std::string::npos) return "application/octet-stream";
    
    std::string ext = path.substr(dot_pos);
    
    if (ext == ".dgz") return "application/gzip";
    if (ext == ".dg") return "application/octet-stream";
    if (ext == ".ess") return "application/octet-stream";
    if (ext == ".zip") return "application/zip";
    if (ext == ".json") return "application/json";
    if (ext == ".csv") return "text/csv";
    
    return "application/octet-stream";
}

void TclServer::start_websocket_server(void)
{
  ws_loop = uWS::Loop::get();
  
  // Check for SSL certificates
  bool use_ssl = false;

  // cert_path and key_path are strings pointing to certificate/key
  struct stat buffer;
  if (stat(cert_path.c_str(), &buffer) == 0 && 
      stat(key_path.c_str(), &buffer) == 0) {
    use_ssl = true;
    websocket_ssl_enabled = true;    
    std::cout << "SSL certificates found - starting HTTPS/WSS server on port " 
              << websocket_port() << std::endl;
  } else {
    std::cout << "SSL certificates not found - starting HTTP/WS server on port " 
              << websocket_port() << std::endl;
    std::cout << "(To enable HTTPS, place cert.pem and key.pem in /etc/dserv/ssl/)" 
              << std::endl;
  }

  ds->set((char *)"system/ssl", (char *)(use_ssl ? "1" : "0"));
  std::string portStr = std::to_string(websocket_port());
  ds->set((char *)"system/webport", (char *)portStr.c_str());
  ds->set((char *)"system/www_path", (char *)www_path.c_str());
  ds->set((char *)"system/exports_path", (char *)exports_path.c_str());
 
  auto setup_routes = [this](auto &app) {
    
    // Health check - always available, no www_path needed
    app.get("/health", [](auto *res, auto *req) {
        res->writeHeader("Content-Type", "application/json")
            ->end("{\"status\":\"ok\",\"service\":\"dserv\"}");
    });
    
    // Favicon - prevent 404 noise in logs
    app.get("/favicon.ico", [](auto *res, auto *req) {
        res->writeStatus("204 No Content")->end();
    });

    // Static file serving
    if (!this->www_path.empty()) {
      // Explicit root handler      
      app.get("/", [this](auto *res, auto *req) {
        std::string file_path = this->www_path + "/index.html";
        std::string content = read_file_contents(file_path);
        
        if (content.empty()) {
	  res->writeStatus("404 Not Found")
	    ->writeHeader("Content-Type", "text/plain")
	    ->end("index.html not found");
	  return;
        }
        
        res->writeHeader("Content-Type", "text/html; charset=utf-8")
	  ->writeHeader("Cache-Control", "no-cache")
	  ->end(content);
      });

      // Redirect /essgui/ to main control page
      app.get("/essgui/", [](auto *res, auto *req) {
	res->writeStatus("302 Found")
	  ->writeHeader("Location", "/ess_control.html")
	  ->end();
      });      
      
    // ------------------------------------------------------------------
    // Download route - serves exported data files
    // ------------------------------------------------------------------
    app.get("/download/*", [this](auto *res, auto *req) {
        std::string url_path(req->getUrl());
        
        // Strip "/download" prefix (9 chars)
        if (url_path.length() <= 9) {
            res->writeStatus("400 Bad Request")
               ->end("No file specified");
            return;
        }
        std::string file_name = url_path.substr(10);  // Skip "/download/"
        
        // Security: reject path traversal
        if (!is_safe_path(file_name)) {
            res->writeStatus("403 Forbidden")
               ->writeHeader("Content-Type", "text/plain")
               ->end("Forbidden: invalid path");
            return;
        }
        
        // Only allow certain extensions for security
        size_t dot_pos = file_name.rfind('.');
        if (dot_pos == std::string::npos) {
            res->writeStatus("403 Forbidden")
               ->end("Forbidden: no file extension");
            return;
        }
        
        std::string ext = file_name.substr(dot_pos);
        if (ext != ".dgz" && ext != ".dg" && ext != ".zip" && 
            ext != ".json" && ext != ".csv" && ext != ".ess") {
            res->writeStatus("403 Forbidden")
               ->end("Forbidden: file type not allowed");
            return;
        }
        
        // Build full path
        std::string file_path = this->exports_path + "/" + file_name;
        
        // Stream the file
        stream_file_response(res, file_path, 
                            get_download_content_type(file_name),
                            file_name);
    });
    
    // ------------------------------------------------------------------
    // List available exports
    // ------------------------------------------------------------------
    app.get("/api/exports", [this](auto *res, auto *req) {
        json_t *response = json_array();
        
        DIR *dir = opendir(this->exports_path.c_str());
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_name[0] == '.') continue;  // Skip hidden files
                
                std::string name = entry->d_name;
                std::string full_path = this->exports_path + "/" + name;
                
                struct stat st;
                if (stat(full_path.c_str(), &st) == 0) {
                    json_t *file_obj = json_object();
                    json_object_set_new(file_obj, "name", json_string(name.c_str()));
                    json_object_set_new(file_obj, "size", json_integer(st.st_size));
                    json_object_set_new(file_obj, "mtime", json_integer(st.st_mtime));
                    json_object_set_new(file_obj, "url", 
                        json_string(("/download/" + name).c_str()));
                    json_array_append_new(response, file_obj);
                }
            }
            closedir(dir);
        }
        
        char *json_str = json_dumps(response, JSON_INDENT(2));
        res->writeHeader("Content-Type", "application/json")
           ->writeHeader("Cache-Control", "no-cache")
           ->end(json_str);
        free(json_str);
        json_decref(response);
    });
    
    // ------------------------------------------------------------------
    // Trigger export via POST (calls Tcl export functions)
    // ------------------------------------------------------------------
    app.post("/api/export", [this](auto *res, auto *req) {
        // Buffer the request body
        std::string buffer;
        
        res->onData([this, res, buffer = std::move(buffer)]
                   (std::string_view data, bool last) mutable {
            buffer.append(data.data(), data.length());
            
            if (last) {
                // Parse JSON request
                json_error_t error;
                json_t *root = json_loads(buffer.c_str(), 0, &error);
                
                if (!root) {
                    json_t *err_response = json_object();
                    json_object_set_new(err_response, "error", 
                                       json_string("Invalid JSON"));
                    char *err_str = json_dumps(err_response, 0);
                    res->writeHeader("Content-Type", "application/json")
                       ->end(err_str);
                    free(err_str);
                    json_decref(err_response);
                    return;
                }
                
                // Extract parameters
                json_t *files_obj = json_object_get(root, "files");
                json_t *level_obj = json_object_get(root, "level");
                
                if (!files_obj || !json_is_array(files_obj)) {
                    json_t *err_response = json_object();
                    json_object_set_new(err_response, "error", 
                                       json_string("Missing 'files' array"));
                    char *err_str = json_dumps(err_response, 0);
                    res->writeHeader("Content-Type", "application/json")
                       ->end(err_str);
                    free(err_str);
                    json_decref(err_response);
                    json_decref(root);
                    return;
                }
                
                const char *level = "extracted";  // default
                if (level_obj && json_is_string(level_obj)) {
                    level = json_string_value(level_obj);
                }
                
                // Build Tcl command to execute export
                // This will be sent to the df subprocess
                std::string tcl_cmd = "send df {export_batch {";
                
                size_t n_files = json_array_size(files_obj);
                for (size_t i = 0; i < n_files; i++) {
                    json_t *file = json_array_get(files_obj, i);
                    if (json_is_string(file)) {
                        if (i > 0) tcl_cmd += " ";
                        tcl_cmd += json_string_value(file);
                    }
                }
                tcl_cmd += "} ";
                tcl_cmd += level;
                tcl_cmd += "}";
                
                // Execute via queue
                SharedQueue<std::string> rqueue;
                client_request_t client_request;
                client_request.type = REQ_SCRIPT;
                client_request.rqueue = &rqueue;
                client_request.script = tcl_cmd;
                client_request.socket_fd = -1;
                client_request.websocket_id = "";
                
                this->queue.push_back(client_request);
                
                // Wait for response
                std::string result = rqueue.front();
                rqueue.pop_front();
                
                // Return result (should be JSON from dfconf.tcl)
                res->writeHeader("Content-Type", "application/json")
                   ->writeHeader("Cache-Control", "no-cache")
                   ->end(result);
                
                json_decref(root);
            }
        });
        
        res->onAborted([]() {
            std::cerr << "Export request aborted" << std::endl;
        });
    });
    
    // ------------------------------------------------------------------
    // Delete export file (cleanup)
    // ------------------------------------------------------------------
    app.del("/api/export/*", [this](auto *res, auto *req) {
        std::string url_path(req->getUrl());
        
        // Strip "/api/export/" prefix
        if (url_path.length() <= 12) {
            res->writeStatus("400 Bad Request")
               ->end("{\"error\":\"No file specified\"}");
            return;
        }
        std::string file_name = url_path.substr(12);
        
        // Security checks
        if (!is_safe_path(file_name)) {
            res->writeStatus("403 Forbidden")
               ->end("{\"error\":\"Invalid path\"}");
            return;
        }
        
        std::string file_path = this->exports_path + "/" + file_name;
        
        if (unlink(file_path.c_str()) == 0) {
            res->writeHeader("Content-Type", "application/json")
               ->end("{\"status\":\"deleted\"}");
        } else {
            res->writeStatus("404 Not Found")
               ->writeHeader("Content-Type", "application/json")
               ->end("{\"error\":\"File not found\"}");
        }
    });
      
      // Serve all other files from www_path
app.get("/*", [this](auto *res, auto *req) {
    std::string url_path(req->getUrl());
    
    // Security: reject path traversal attempts
    if (!is_safe_path(url_path)) {
        res->writeStatus("403 Forbidden")
            ->writeHeader("Content-Type", "text/plain")
            ->end("Forbidden");
        return;
    }
    
    // Build filesystem path
    std::string file_path = this->www_path + url_path;
    std::string content = read_file_contents(file_path);
    
    // If path ends with /, try index.html in that directory
    if (content.empty() && url_path.back() == '/') {
        file_path = this->www_path + url_path + "index.html";
        content = read_file_contents(file_path);
    }
    
    // Clean URLs for paths without extension
    if (content.empty() && url_path.find('.') == std::string::npos) {
        // First try as directory with index.html (e.g., /essgui -> /essgui/index.html)
        file_path = this->www_path + url_path + "/index.html";
        content = read_file_contents(file_path);
        
        // Then try as .html file (e.g., /terminal -> /terminal.html)
        if (content.empty()) {
            file_path = this->www_path + url_path + ".html";
            content = read_file_contents(file_path);
        }
    }
    
    if (content.empty()) {
        res->writeStatus("404 Not Found")
            ->writeHeader("Content-Type", "text/html; charset=utf-8")
            ->end(R"(<!DOCTYPE html>
<html><head><title>404 - Not Found</title>
<style>
body { font-family: system-ui, sans-serif; background: #0d1117; color: #e6edf3; 
       display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
.container { text-align: center; }
h1 { color: #f85149; }
a { color: #58a6ff; }
</style></head>
<body><div class="container">
<h1>404 - Not Found</h1>
<p>The requested page was not found.</p>
<p><a href="/">Return to Home</a></p>
</div></body></html>)");
        return;
    }
    
    // Cache: no-cache for HTML, short cache for assets
    const char* cache_control = "no-cache";
    std::string ext = file_path.substr(file_path.rfind('.') + 1);
    if (ext == "js" || ext == "css" || ext == "png" || ext == "jpg" || 
        ext == "gif" || ext == "svg" || ext == "woff" || ext == "woff2" || ext == "ico") {
        cache_control = "public, max-age=3600";  // 1 hour for assets
    }
    
    res->writeHeader("Content-Type", get_content_type(file_path))
        ->writeHeader("Cache-Control", cache_control)
        ->end(content);
});        
        std::cout << "Web interface enabled at: " << this->www_path << std::endl;
    } 
    else {
        // No www_path: show helpful setup message
        app.get("/*", [](auto *res, auto *req) {
            res->writeHeader("Content-Type", "text/html; charset=utf-8")
                ->end(R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>dserv - Web Interface Not Configured</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #0d1117 0%, #161b22 100%);
            color: #e6edf3;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            max-width: 600px;
            background: #21262d;
            border: 1px solid #30363d;
            border-radius: 12px;
            padding: 40px;
            text-align: center;
        }
        .logo {
            font-size: 48px;
            margin-bottom: 20px;
        }
        h1 {
            color: #58a6ff;
            margin-bottom: 16px;
            font-size: 24px;
        }
        .status {
            background: #1a4d1a;
            border: 1px solid #238636;
            color: #3fb950;
            padding: 12px 20px;
            border-radius: 6px;
            margin: 20px 0;
            font-weight: 500;
        }
        .warning {
            background: #3d2a00;
            border: 1px solid #9e6a03;
            color: #d29922;
            padding: 12px 20px;
            border-radius: 6px;
            margin: 20px 0;
        }
        p {
            color: #8b949e;
            line-height: 1.6;
            margin: 12px 0;
        }
        code {
            background: #161b22;
            padding: 2px 8px;
            border-radius: 4px;
            font-family: 'SF Mono', Monaco, 'Consolas', monospace;
            font-size: 14px;
            color: #79c0ff;
        }
        .code-block {
            background: #161b22;
            border: 1px solid #30363d;
            border-radius: 6px;
            padding: 16px;
            margin: 16px 0;
            text-align: left;
            overflow-x: auto;
        }
        .code-block code {
            background: none;
            padding: 0;
            display: block;
            white-space: pre;
        }
        .section {
            margin: 24px 0;
            text-align: left;
        }
        .section h3 {
            color: #e6edf3;
            font-size: 14px;
            margin-bottom: 8px;
        }
        a {
            color: #58a6ff;
            text-decoration: none;
        }
        a:hover {
            text-decoration: underline;
        }
        .endpoints {
            margin-top: 24px;
            padding-top: 24px;
            border-top: 1px solid #30363d;
        }
        .endpoint {
            display: flex;
            justify-content: space-between;
            padding: 8px 0;
            border-bottom: 1px solid #21262d;
        }
        .endpoint:last-child {
            border-bottom: none;
        }
        .endpoint-name {
            color: #8b949e;
        }
        .endpoint-status {
            color: #3fb950;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="logo">⚡</div>
        <h1>dserv is running</h1>
        
        <div class="status">
            ✓ Data server operational
        </div>
        
        <div class="warning">
            ⚠ Web interface not configured
        </div>
        
        <div class="section">
            <h3>To enable the web interface:</h3>
            <div class="code-block">
                <code># Development (serve from source)
dserv -w /path/to/dserv/www

# Production (default install location)
dserv -w /usr/local/dserv/www</code>
            </div>
        </div>
        
        <div class="section">
            <h3>Or set www_path in your config:</h3>
            <div class="code-block">
                <code># In dsconf.tcl or config.tcl
set www_path /usr/local/dserv/www</code>
            </div>
        </div>
        
        <p>
            If you installed via package (.deb/.pkg), the web files should be at
            <code>/usr/local/dserv/www</code>
        </p>
        
        <div class="endpoints">
            <h3 style="color: #e6edf3; margin-bottom: 12px;">Available Endpoints</h3>
            <div class="endpoint">
                <span class="endpoint-name">/health</span>
                <span class="endpoint-status">✓ Available</span>
            </div>
            <div class="endpoint">
                <span class="endpoint-name">/ws</span>
                <span class="endpoint-status">✓ Available</span>
            </div>
            <div class="endpoint">
                <span class="endpoint-name">TCP :2570</span>
                <span class="endpoint-status">✓ Available</span>
            </div>
        </div>
    </div>
</body>
</html>)");
        });
        
        std::cout << "Web interface: not configured (use -w flag or set www_path)" << std::endl;
    }

    // WebSocket endpoint - NOTE the "template" keyword and "auto *ws" for type flexibility
    app.template ws<WSPerSocketData>("/ws", {
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
            std::cerr <<
	      "ERROR: Invalid userData in WebSocket open handler" << std::endl;
            ws->close();
            return;
          }

          try {
            // Create notification queue for this client
            userData->notification_queue = new SharedQueue<client_request_t>();
            
            // Register with Dataserver as a queue-based client
            userData->dataserver_client_id =
	      this->ds->add_new_send_client(userData->notification_queue);
            
            if (userData->dataserver_client_id.empty()) {
              std::cerr <<
		"Failed to register WebSocket client with Dataserver" <<
		std::endl;
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
              this->ws_connections[userData->client_name] = (void*)ws;
            }
            
            // Start a thread to process notifications for this client
            // Pass 'ws' by value (as void*) to the thread
            std::thread([this, ws, userData]() {
              this->process_websocket_client_notifications_template(ws, userData);
            }).detach();
            
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
              json_t *requestId_obj = json_object_get(root, "requestId");

              if (script_obj && json_is_string(script_obj)) {
                const char *script = json_string_value(script_obj);
                  
                // Create request
                client_request_t req;
                req.type = REQ_SCRIPT;
                req.rqueue = userData->rqueue;
                req.script = std::string(script);
		req.socket_fd = -1;
		req.websocket_id = userData->client_name;
		
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
                json_object_set_new(error_response, "error",
				    json_string("Missing or invalid 'name' field"));
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
                auto it = std::find(userData->subscriptions.begin(),
				    userData->subscriptions.end(), match);
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
		  if (json_str) {
		    ws->send(json_str, uWS::OpCode::TEXT);
		    free(json_str);
		  } else {
		    // Send error about unsupported datatype
		    json_t *error_response = json_object();
		    json_object_set_new(error_response, "error",
					json_string("Unsupported datapoint type"));
		    char *error_str = json_dumps(error_response, 0);
		    ws->send(error_str, uWS::OpCode::TEXT);
		    free(error_str);
		    json_decref(error_response);
		  }
		  dpoint_free(dp);
		}
                else {
                  json_t *error_response = json_object();
                  json_object_set_new(error_response, "error",
				      json_string("Datapoint not found"));
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
	    req.socket_fd = -1;
	    req.websocket_id = userData->client_name;
  
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

	    // Cleanup linked subprocesses
	    this->cleanup_subprocesses_for_websocket(userData->client_name);	    
            
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
            
          } catch (const std::exception& e) {
            std::cerr << "Exception in WebSocket close handler: " << e.what() << std::endl;
          }
        }   
        
	  }).listen("0.0.0.0", websocket_port(), [this](auto *listen_socket) {
      if (listen_socket) {
        std::cout << "WebSocket server listening on port " << websocket_port() << std::endl;
        std::cout << "Web terminal available at http://localhost:" << websocket_port() << "/" << std::endl;
      } else {
        std::cerr << "Failed to start WebSocket server on port " << websocket_port() << std::endl;
      }
    }).run();
  }; // End of setup_routes lambda
  
  // Create appropriate app type and call setup_routes
  if (use_ssl) {
    auto app = uWS::SSLApp({
      .key_file_name = key_path.c_str(),
      .cert_file_name = cert_path.c_str(),
      .passphrase = "",
    });
    setup_routes(app);
  } else {
    auto app = uWS::App();
    setup_routes(app);
  }
}

template<typename WebSocketType>
void TclServer::process_websocket_client_notifications_template(
    WebSocketType* ws, WSPerSocketData* userData) {
    if (!userData) {
        std::cerr << "ERROR: userData is null in notification thread" << std::endl;
        return;
    }
    
    std::string client_name = userData->client_name;
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
            // Check if this datapoint matches any subscriptions
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
                    json_error_t error;
                    json_t *root = json_loads(json_str, 0, &error);
                    if (root) {
                        json_object_set_new(root, "type", json_string("datapoint"));
                        char *enhanced_json = json_dumps(root, 0);
                        
                        std::string message(enhanced_json);
                        
                        // Send using ws_loop->defer for thread safety
                        if (ws_loop) {
                            ws_loop->defer([ws, message]() {
                                ws->send(message, uWS::OpCode::TEXT);
                            });
                        }
                        
                        free(enhanced_json);
                        json_decref(root);
                    }
                    free(json_str);
                }
            }
            dpoint_free(req.dpoint);
        }
      } catch (...) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    
    // Cleanup
    if (userData && userData->notification_queue) {
        delete userData->notification_queue;
        userData->notification_queue = nullptr;
    }
}

template<typename WebSocketType>
void TclServer::sendLargeMessage(WebSocketType* ws,
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

/********************* link subprocess to connection support *******************************/

void TclServer::link_subprocess_to_current_connection(const std::string& subprocess_name) {
  if (!current_request) {
    std::cerr << "Warning: link_subprocess called but no current request context" << std::endl;
    return;
  }
  
  std::lock_guard<std::mutex> lock(subprocess_ownership_mutex);
  
  if (current_request->socket_fd >= 0) {
    subprocess_to_socket[subprocess_name] = current_request->socket_fd;
    //    std::cout << "Linked subprocess '" << subprocess_name 
    //              << "' to socket " << current_request->socket_fd << std::endl;
  } 
  else if (!current_request->websocket_id.empty()) {
    subprocess_to_websocket[subprocess_name] = current_request->websocket_id;
    //    std::cout << "Linked subprocess '" << subprocess_name 
    //              << "' to websocket " << current_request->websocket_id << std::endl;
  }
  else {
    std::cerr << "Warning: link_subprocess called but request has no connection info" << std::endl;
  }
}

void TclServer::cleanup_datapoints_for_subprocess(const std::string& subprocess_name) {
  // Get all datapoint keys
  std::string keys_str = ds->get_table_keys();
  if (keys_str.empty()) {
    return;
  }
  
  // Parse space-separated keys
  std::istringstream iss(keys_str);
  std::string key;
  
  // Patterns to match: "subprocess_name/*" and "error/subprocess_name"
  std::string prefix = subprocess_name + "/";
  std::string error_key = "error/" + subprocess_name;
  
  std::vector<std::string> to_delete;
  
  while (iss >> key) {
    if (key.rfind(prefix, 0) == 0 || key == error_key) {
      to_delete.push_back(key);
    }
  }
  
  // Delete matching datapoints
  for (const auto& key : to_delete) {
    ds->clear(const_cast<char*>(key.c_str()));
  }
  
  if (!to_delete.empty()) {
    //    std::cout << "  Cleaned up " << to_delete.size() << " datapoints for " 
    //              << subprocess_name << std::endl;
  }
}

void TclServer::cleanup_subprocesses_for_socket(int sockfd) {
  std::lock_guard<std::mutex> lock(subprocess_ownership_mutex);
  
  std::vector<std::string> to_cleanup;
  for (const auto& [name, sock] : subprocess_to_socket) {
    if (sock == sockfd) {
      to_cleanup.push_back(name);
    }
  }
  
  for (const auto& name : to_cleanup) {
    auto subprocess = TclServerRegistry.getObject(name);
    if (subprocess) {
      //      std::cout << "Socket " << sockfd << " closed, shutting down subprocess: " 
      //        << name << std::endl;
      subprocess->shutdown();
      delete subprocess;
    }
    TclServerRegistry.unregisterObject(name);
    subprocess_to_socket.erase(name);

    // remove private dpoints created for this subprocess
    cleanup_datapoints_for_subprocess(name);  
  }

}

void TclServer::cleanup_subprocesses_for_websocket(const std::string& ws_id) {
  std::lock_guard<std::mutex> lock(subprocess_ownership_mutex);
  
  std::vector<std::string> to_cleanup;
  for (const auto& [name, id] : subprocess_to_websocket) {
    if (id == ws_id) {
      to_cleanup.push_back(name);
    }
  }
  
  for (const auto& name : to_cleanup) {
    auto subprocess = TclServerRegistry.getObject(name);
    if (subprocess) {
      //      std::cout << "WebSocket " << ws_id << " closed, shutting down subprocess: " 
      //                << name << std::endl;
      subprocess->shutdown();
      delete subprocess;
    }
    TclServerRegistry.unregisterObject(name);
    subprocess_to_websocket.erase(name);

    // remove private dpoints created for this subprocess
    cleanup_datapoints_for_subprocess(name);    
  }
}

static int subprocess_eval_command(ClientData data, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script");
        return TCL_ERROR;
    }
    
    TclServer *tclserver = (TclServer *) data;
    
    TclServer *child = new TclServer(tclserver->argc,
                                     tclserver->argv,
                                     tclserver->ds);

    std::string script = Tcl_GetString(objv[1]);
    auto result = child->eval(script);
    
    delete child;
    
    if (result.starts_with("!TCL_ERROR ")) {
        Tcl_AppendResult(interp, result.c_str() + 11, NULL);
        return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(result.c_str(), result.size()));
    return TCL_OK;
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

  if (!strcmp(Tcl_GetString(objv[1]), "dserv")) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": cannot send directly to dserv", NULL);
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

/******************************* get_var *******************************/

static int get_var_command(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *objv[])
{
    TclServer *this_server = (TclServer *) data;
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "server_name var_name");
        return TCL_ERROR;
    }
    
    std::string server_name = Tcl_GetString(objv[1]);
    std::string var_name = Tcl_GetString(objv[2]);
    
    // Check if target server exists
    auto target_server = TclServerRegistry.getObject(server_name);
    if (!target_server) {
        Tcl_AppendResult(interp, "server \"", server_name.c_str(), "\" not found", NULL);
        return TCL_ERROR;
    }
    
    if (target_server == this_server) {
        Tcl_AppendResult(interp, "cannot get variable from self", NULL);
        return TCL_ERROR;
    }
    
    // Build a Tcl command to get the variable value
    std::string get_command = "set " + var_name;
    
    // Use the existing send infrastructure
    SharedQueue<std::string> rqueue;
    client_request_t client_request;
    client_request.type = REQ_SCRIPT;
    client_request.rqueue = &rqueue;
    client_request.script = get_command;
    
    target_server->queue.push_back(client_request);
    
    // Wait for response
    std::string result = client_request.rqueue->front();
    client_request.rqueue->pop_front();
    
    // Handle errors
    if (result.starts_with("!TCL_ERROR ")) {
        Tcl_AppendResult(interp, result.substr(11).c_str(), NULL);
        return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(result.c_str(), -1));
    return TCL_OK;
}


/******************************* process *******************************/

static void update_subprocess_dpoint(TclServer *tclserver)
{
  ds_datapoint_t dpoint;
  auto allObjects = TclServerRegistry.getAllObjects();
  
  // Separate regular and linked subprocesses
  std::string interpList;      // For regular subprocesses (UI)
  std::string sandboxList;     // For linked/sandbox subprocesses (monitoring)
  
  for (const auto& [name, server] : allObjects) {
    if (server->is_linked()) {
      // Add to sandbox list
      if (!sandboxList.empty()) sandboxList += " ";
      sandboxList += name;
    } else {
      // Add to regular interp list
      if (!interpList.empty()) interpList += " ";
      interpList += name;
    }
  }
  
  // Update dserv/interps (regular subprocesses - for UI)
  dpoint_set(&dpoint, (char *) tclserver->INTERPS_DPOINT_NAME,
             tclserver->ds->now(), DSERV_STRING, interpList.size(),
             (unsigned char *) interpList.c_str());
  tclserver->ds->set(dpoint);
  
  // Update dserv/sandboxes (linked subprocesses - for monitoring)
  dpoint_set(&dpoint, (char *) tclserver->SANDBOXES_DPOINT_NAME,
             tclserver->ds->now(), DSERV_STRING, sandboxList.size(),
             (unsigned char *) sandboxList.c_str());
  tclserver->ds->set(dpoint);
}

static int subprocess_command (ClientData data, Tcl_Interp *interp,
                               int objc, Tcl_Obj *objv[])
{
  static std::atomic<int> link_counter{0};
  
  TclServer *tclserver = (TclServer *) data;
  int port = -1;
  std::string script;
  bool link_connection = false;
  
  int arg_idx = 1;
  
  // Parse -link option
  while (arg_idx < objc && Tcl_GetString(objv[arg_idx])[0] == '-') {
    std::string opt = Tcl_GetString(objv[arg_idx]);
    if (opt == "-link") {
      link_connection = true;
      arg_idx++;
    } else {
      Tcl_AppendResult(interp, "unknown option: ", opt.c_str(), NULL);
      return TCL_ERROR;
    }
  }
  
  std::string name;
  
  // If -link was specified and no name provided, generate one
  if (link_connection && arg_idx >= objc) {
    // Generate unique name for linked subprocess
    do {
      name = "linked_" + std::to_string(link_counter.fetch_add(1));
    } while (TclServerRegistry.exists(name));
  } else {
    // Name was explicitly provided
    if (arg_idx >= objc) {
      Tcl_WrongNumArgs(interp, 1, objv, "?-link? ?name? ?port? ?script?");
      return TCL_ERROR;
    }
    name = Tcl_GetString(objv[arg_idx++]);
  }
  
  // Parse remaining args (port and/or script)
  if (arg_idx < objc) {
    // Try to parse as port number
    if (Tcl_GetIntFromObj(interp, objv[arg_idx], &port) == TCL_OK) {
      arg_idx++;
      if (arg_idx < objc) {
        script = std::string(Tcl_GetString(objv[arg_idx]));
      }
    } else {
      // Not an int, must be script
      script = std::string(Tcl_GetString(objv[arg_idx]));
    }
  }
  
  if (TclServerRegistry.exists(name)) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": child process \"",
                     name.c_str(), "\" already exists", NULL);
    return TCL_ERROR;
  }
  
  TclServer *child = new TclServer(tclserver->argc,
                                   tclserver->argv,
                                   tclserver->ds,
                                   name.c_str(), port);
  
  
  // link to current connection if requested, o.w. add to registry
  if (link_connection) {
    tclserver->link_subprocess_to_current_connection(name);
    child->set_linked(true);    
  }
  TclServerRegistry.registerObject(name, child);
    
  if (!script.empty()) {
    auto result = child->eval(script);
    if (result.starts_with("!TCL_ERROR ")) {
      Tcl_AppendResult(interp, result.c_str(), NULL);
      delete child;
      return TCL_ERROR;
    }
  }
  
  // update list of current subprocesses
  update_subprocess_dpoint(tclserver);
  
  Tcl_SetObjResult(interp, Tcl_NewStringObj(child->name.c_str(), -1));
  
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


// Custom exit command for subprocess control
static int subprocess_exit_cmd(ClientData clientData, Tcl_Interp *interp,
			       int objc, Tcl_Obj *const objv[])
{
  TclServer *tserv = (TclServer *) clientData;
  
  // Check if this is the main dserv process
  if (tserv->name == "dserv" || tserv->name == "") {
    Tcl_SetResult(interp, 
		  (char *)"Cannot exit main dserv process. Use 'shutdown' instead.", 
		  TCL_STATIC);
    return TCL_ERROR;
  }
  
  // For subprocesses, allow exit with optional code
  int exitCode = 0;
  if (objc > 1) {
    if (Tcl_GetIntFromObj(interp, objv[1], &exitCode) != TCL_OK) {
      return TCL_ERROR;
    }
  }
  
  // Store exit code in a datapoint for monitoring
  //  std::string dp_name = tserv->name + "/exit_code";
  //  tserv->ds->set(dp_name.c_str(), exitCode);
  
  //  std::cout << "Subprocess '" << tserv->name 
  //	    << "' exiting with code " << exitCode << std::endl;
  
  // Trigger clean shutdown
  tserv->shutdown();
  
  return TCL_OK;
}



static int set_priority_command(ClientData data, Tcl_Interp *interp,
                                int objc, Tcl_Obj *objv[])
{
    TclServer *tclserver = (TclServer *) data;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "priority");
        return TCL_ERROR;
    }
    
    int priority;
    if (Tcl_GetIntFromObj(interp, objv[1], &priority) != TCL_OK) {
        return TCL_ERROR;
    }
    
    // Validate priority range (1-99 for SCHED_FIFO on Linux)
    if (priority < 1 || priority > 99) {
        Tcl_AppendResult(interp, "Priority must be between 1 and 99", NULL);
        return TCL_ERROR;
    }
    
    tclserver->setPriority(priority);
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(priority));
    return TCL_OK;
}

static int dserv_version_command(ClientData data, Tcl_Interp *interp,
                                int objc, Tcl_Obj *objv[]) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj(dserv_VERSION, -1));
    return TCL_OK;
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

static int evt_set_script_command(ClientData data, Tcl_Interp *interp,
                                  int objc, Tcl_Obj *objv[])
{
    TclServer *tclserver = (TclServer *) data;
    
    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "type subtype script");
        return TCL_ERROR;
    }
    
    int type, subtype;
    if (Tcl_GetIntFromObj(interp, objv[1], &type) != TCL_OK ||
        Tcl_GetIntFromObj(interp, objv[2], &subtype) != TCL_OK) {
        return TCL_ERROR;
    }
    
    std::string script = Tcl_GetString(objv[3]);
    
    try {
        tclserver->eventDispatcher->registerEventHandler(type, subtype, script);
    } catch (const std::exception& e) {
        Tcl_AppendResult(interp, "evtSetScript: ", e.what(), NULL);
        return TCL_ERROR;
    }
    
    return TCL_OK;
}

static int evt_remove_script_command(ClientData data, Tcl_Interp *interp,
                                     int objc, Tcl_Obj *objv[])
{
    TclServer *tclserver = (TclServer *) data;
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "type subtype");
        return TCL_ERROR;
    }
    
    int type, subtype;
    if (Tcl_GetIntFromObj(interp, objv[1], &type) != TCL_OK ||
        Tcl_GetIntFromObj(interp, objv[2], &subtype) != TCL_OK) {
        return TCL_ERROR;
    }
    
    tclserver->eventDispatcher->removeEventHandler(type, subtype);
    return TCL_OK;
}

static int evt_remove_all_scripts_command(ClientData data, Tcl_Interp *interp,
                                          int objc, Tcl_Obj *objv[])
{
    TclServer *tclserver = (TclServer *) data;
    tclserver->eventDispatcher->removeAllEventHandlers();
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


// =============================================================================
// Tcl command: www_path
// =============================================================================

// www_path ?path?
// With no argument: returns current www path
// With argument: sets the www path (validates directory exists)
static int Www_Path_Cmd(ClientData clientData, Tcl_Interp *interp,
                        int objc, Tcl_Obj *const objv[])
{
  TclServer *tserv = (TclServer *)clientData;
  
  // Return current path
  Tcl_SetObjResult(interp, Tcl_NewStringObj(tserv->getWwwPath().c_str(), -1));
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
  Tcl_CreateObjCommand(interp, "dservInfo",
		       dserv_info_command, tserv->ds, NULL);
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
  Tcl_CreateObjCommand(interp, "subprocessEval",
               (Tcl_ObjCmdProc *) subprocess_eval_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "subprocessInfo",
               (Tcl_ObjCmdProc *) getsubprocesses_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "setPriority",
               (Tcl_ObjCmdProc *) set_priority_command, tserv, NULL);
                     
  Tcl_CreateObjCommand(interp, "evalNoReply",
               (Tcl_ObjCmdProc *) eval_noreply_command,
               tserv, NULL);
 
  Tcl_CreateObjCommand(interp, "dservVersion",
                    (Tcl_ObjCmdProc *) dserv_version_command,
                    tserv, NULL);
                     
  Tcl_CreateObjCommand(interp, "send",
               (Tcl_ObjCmdProc *) send_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "sendNoReply",
               (Tcl_ObjCmdProc *) send_noreply_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "sendAsync",
               (Tcl_ObjCmdProc *) send_noreply_command,
               tserv, NULL);

  Tcl_CreateObjCommand(interp, "getVar",
                    (Tcl_ObjCmdProc *) get_var_command, tserv, NULL);
                                      
  Tcl_CreateObjCommand(interp, "dservAddMatch",
               dserv_add_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservAddExactMatch",
               dserv_add_exact_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveMatch",
               dserv_remove_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveAllMatches",
               dserv_remove_all_matches_command, tserv, NULL);

  Tcl_CreateObjCommand(interp, "evtSetScript",
			 (Tcl_ObjCmdProc *) evt_set_script_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "evtRemoveScript", 
			 (Tcl_ObjCmdProc *) evt_remove_script_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "evtRemoveAllScripts",
			 (Tcl_ObjCmdProc *) evt_remove_all_scripts_command, tserv, NULL);
  
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

  Tcl_CreateObjCommand(interp, "www_path", Www_Path_Cmd,
		       (ClientData)tserv, NULL);
  
  Tcl_CreateObjCommand(interp, "print",
               (Tcl_ObjCmdProc *) print_command, tserv, NULL);
  
  Tcl_LinkVar(interp, "tcpPort", (char *) &tserv->_newline_port,
          TCL_LINK_INT | TCL_LINK_READ_ONLY);


  // Completion support
  TclCompletion::RegisterCompletionCommands(interp);

  return;
}

static int Tcl_DservAppInit(Tcl_Interp *interp, TclServer *tserv)
{
  if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;
  
  add_tcl_commands(interp, tserv);
  
  if (tserv->hasCommandCallback()) {
      tserv->callCommandCallback(interp);
  }

  // Common initialization for all interpreters
  const char *init_script = R"(
    set dspath [file dir [info nameofexecutable]]
    set base [file join [zipfs root] dlsh]
    set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

    proc remoteEval {host script {port 2560}} {
        set sock [socket $host $port]
        fconfigure $sock -translation binary -buffering full
        
        set msg "subprocessEval [list $script]"
        set len [string length $msg]
        puts -nonewline $sock [binary format I $len]$msg
        flush $sock
        
        set lenbuf [read $sock 4]
        binary scan $lenbuf I rlen
        set result [read $sock $rlen]
        
        close $sock
        return $result
    }
    proc remoteSend {host script {port 2560}} {
        set sock [socket $host $port]
        fconfigure $sock -translation binary -buffering full
        
        set len [string length $script]
        puts -nonewline $sock [binary format I $len]$script
        flush $sock
        
        set lenbuf [read $sock 4]
        binary scan $lenbuf I rlen
        set result [read $sock $rlen]
        
        close $sock
        return $result
    }
  )";
  
  if (Tcl_Eval(interp, init_script) != TCL_OK) {
    return TCL_ERROR;
  }
  
  // Redirect puts to datapoint for subprocesses (not main dserv)
  if (tserv->name != "dserv") {
    std::string puts_redirect = R"(
      rename puts _puts
      proc puts {args} {
        switch [llength $args] {
          1 {
            dservSet )" + tserv->name + R"(/stdout "[lindex $args 0]\n"
          }
          2 {
            if {[lindex $args 0] eq "-nonewline"} {
              dservSet )" + tserv->name + R"(/stdout [lindex $args 1]
            } else {
              _puts {*}$args
            }
          }
          default {
            _puts {*}$args
          }
        }
      }
    )";
    
    if (Tcl_Eval(interp, puts_redirect.c_str()) != TCL_OK) {
      return TCL_ERROR;
    }
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

  // initialize specialize event dispatcher
  tserv->eventDispatcher = new EventDispatcher(interp);
  
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

// event-specific script execution 
static int event_tcl_script(Tcl_Interp *interp,
                           const char *script,
                           ds_datapoint_t *dpoint)
{
    // Event handling - pass script, type, subtype, params
    Tcl_Obj *commandArray[4];
    commandArray[0] = Tcl_NewStringObj(script, -1);
    commandArray[1] = Tcl_NewIntObj(dpoint->data.e.type);
    commandArray[2] = Tcl_NewIntObj(dpoint->data.e.subtype);
    
    // Create temporary datapoint for parameters
    ds_datapoint_t e_dpoint;
    e_dpoint.data.type = (ds_datatype_t) dpoint->data.e.puttype;
    e_dpoint.data.len = dpoint->data.len;
    e_dpoint.data.buf = dpoint->data.buf;
    
    commandArray[3] = dpoint_to_tclobj(interp, &e_dpoint);
    
    for (int i = 0; i < 4; i++) { Tcl_IncrRefCount(commandArray[i]); }
    int retcode = Tcl_EvalObjv(interp, 4, commandArray, 0);
    for (int i = 0; i < 4; i++) { Tcl_DecrRefCount(commandArray[i]); }
    return retcode;
}

void EventDispatcher::registerEventHandler(int type, int subtype, const std::string& script) {
    if (type < 0 || type > 255 || subtype < -1 || subtype > 255) {
        throw std::invalid_argument("Invalid event type/subtype");
    }
    eventHandlers[{type, subtype}] = script;
}

void EventDispatcher::processEvent(ds_datapoint_t *dpoint) {
    if (dpoint->data.e.dtype != DSERV_EVT) return;
    
    int eventType = dpoint->data.e.type;
    int eventSubtype = dpoint->data.e.subtype;
    
    // Check specific type/subtype first
    auto specificKey = std::make_pair(eventType, eventSubtype);
    auto it = eventHandlers.find(specificKey);
    if (it != eventHandlers.end()) {
        event_tcl_script(interp, it->second.c_str(), dpoint);  // Use event_tcl_script
        return;
    }
    
    // Check wildcard subtype (-1)
    auto wildcardKey = std::make_pair(eventType, EVT_SUBTYPE_ALL);
    it = eventHandlers.find(wildcardKey);
    if (it != eventHandlers.end()) {
        event_tcl_script(interp, it->second.c_str(), dpoint);  // Use event_tcl_script
    }
}

void EventDispatcher::removeEventHandler(int type, int subtype) {
    eventHandlers.erase({type, subtype});
}

void EventDispatcher::removeAllEventHandlers() {
    eventHandlers.clear();
}

static int process_requests(TclServer *tserv)
{
  int retcode;
  client_request_t req;

  // create a private interpreter for this process
  Tcl_Interp *interp = setup_tcl(tserv);
  
  // store our interp in the TclServer instance
  tserv->setInterp(interp); 
  
  // Set the association data for modules to find
  Tcl_SetAssocData(interp, "tclserver_instance", NULL, (ClientData)tserv);

  // Create ErrorMonitor
  ErrorMonitor* errorMonitor = new ErrorMonitor(tserv);
  ErrorMonitor::registerCommand(interp, errorMonitor);

  // Override the default exit command with our subprocess-aware version
  Tcl_CreateObjCommand(interp, "exit", subprocess_exit_cmd, 
		       (ClientData)tserv, NULL);
  
  /* process until receive a message saying we are done */
  while (!tserv->m_bDone) {
    
    req = tserv->queue.front();
    tserv->queue.pop_front();

    // set current request context so Tcl commands can access
    tserv->set_current_request(&req);
 
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
	std::string varname(dpoint->varname);
	
	// Process events through EventDispatcher first
	if (varname == "eventlog/events" && tserv->eventDispatcher) {
	  tserv->eventDispatcher->processEvent(dpoint);
	}
        
	// evaluate a dpoint script
	std::string script;
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

    // clear context
    tserv->set_current_request(nullptr);
  }

  // Clean up ErrorMonitor
  delete errorMonitor;

  // Clean up datapoints for this subprocess before unregistering
  tserv->cleanup_datapoints_for_subprocess(tserv->name);
  
  TclServerRegistry.unregisterObject(tserv->name);
  update_subprocess_dpoint(tserv);
  
  // Call a shutdown handler if it exists
  Tcl_Eval(interp, "if {[info procs ::on_shutdown] ne {}} {::on_shutdown}");
   
  tserv->setInterp(nullptr);
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
  client_request.socket_fd = sockfd;
  client_request.websocket_id = "";
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

      // push request onto queue for main thread to retrieve
      queue->push_back(client_request);
      
      // rqueue will be available after command has been processed
      s = client_request.rqueue->front();
      client_request.rqueue->pop_front();
      
      //    std::cout << "TCL Result: " << s << std::endl;

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

  // cleanup any linked subprocesses
  tserv->cleanup_subprocesses_for_socket(sockfd);
  
  // close and unregister for proper limit tracking
  tserv->unregister_connection(sockfd);
}

static void sendMessage(int socket, const std::string& message) {
  // Convert size to network byte order
  uint32_t msgSize = htonl(message.size()); 
  send(socket, (char *) &msgSize, sizeof(msgSize), 0);
  
  size_t totalSent = 0;
  size_t remaining = message.size();
  const char* data = message.c_str();
  
  while (totalSent < message.size()) {
    ssize_t sent = send(socket, data + totalSent, remaining, 0);
    if (sent <= 0) break;  // error or connection closed
    totalSent += sent;
    remaining -= sent;
  }
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
  client_request.socket_fd = sockfd;
  client_request.websocket_id = "";
  
  std::string script;  

  while (true) {
    auto [buffer, msgSize] = receiveMessage(sockfd);
    if (buffer == nullptr) break;
    if (msgSize) {

      // shutdown if main server has shutdown
      if (tserv->m_bDone) break;

      client_request.script = std::string(buffer);
      std::string s;
      
      // push request onto queue for main thread to retrieve
      queue->push_back(client_request);
      
      // rqueue will be available after command has been processed
      s = client_request.rqueue->front();
      client_request.rqueue->pop_front();
      //  std::cout << "TCL Result: " << s << std::endl;
      
      // Send a response back to the client
      sendMessage(sockfd, s);
      
      delete[] buffer;
    }
  }

  // cleanup linked subprocesses when connection closes
  tserv->cleanup_subprocesses_for_socket(sockfd);
  
  tserv->unregister_connection(sockfd);
}

