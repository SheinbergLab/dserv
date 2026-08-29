#include "TclServer.h"
#include "SendGuard.h"
#include "TclCommands.h"
#include "TclInterpInit.h"
#include "ObjectRegistry.h"
#include "dserv.h"
#include "dservConfig.h"
#include "socket_keepalive.h"
#include "ListenerSocket.h"
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>

// JSON support
#include <jansson.h>

#include <fnmatch.h>  // pattern matching support

/* PHC (PTP hardware clock) access for dservPhcOffset. Linux-only: /dev/ptpN and
 * PTP_SYS_OFFSET_PRECISE do not exist elsewhere, and dserv also builds on
 * macOS. The command is registered on every platform regardless -- it FAILS
 * there rather than silently returning nothing, because a no-op would hand back
 * an absent offset that a caller cannot tell from a good one. */
#if defined(__linux__)
#include <linux/ptp_clock.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#endif

#include "TclCompletion.h"

extern "C" int TclHttps_RegisterCommands(Tcl_Interp *interp);
extern "C" int TclSha256_RegisterCommands(Tcl_Interp *interp);

static int process_requests(TclServer *tserv);
static Tcl_Interp *setup_tcl(TclServer *tserv);
static int dpoint_tcl_script(Tcl_Interp *interp, const char *script,
                             ds_datapoint_t *dpoint);
static void run_when_callbacks(TclServer *tserv, Tcl_Interp *interp,
                               ds_datapoint_t *dpoint);
static int dserv_when_command(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *const objv[]);
static int dserv_when_cancel_command(ClientData data, Tcl_Interp *interp,
                                     int objc, Tcl_Obj *const objv[]);

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

/*
 * Websocket startup latch.
 *
 *  A websocket server thread spends its first moments inside
 * uWS::SSLApp construction -- SSL_CTX_new, cipher loading, engine
 * lookups against OpenSSL's process-global locks.  Those locks are
 * freed by OpenSSL's atexit cleanup, so a process that exits while a
 * websocket thread is still starting (a --tscript that finishes in
 * ~150ms) segfaults in pthread_rwlock_rdlock on freed memory.  The
 * threads are deliberately detached (a blocked app.run() cannot be
 * joined), so exit paths instead wait -- bounded -- for every
 * websocket thread to get past that window: Tcl-level exit via the
 * exit proc in dserv.cpp, orderly shutdown via graceful_shutdown().
 */
static std::atomic<int> ws_threads_starting{0};

void TclServer::mark_websocket_started(void)
{
  if (!ws_started.exchange(true))
    ws_threads_starting.fetch_sub(1);
}

void TclServer::wait_websocket_startups(int timeout_ms)
{
  auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(timeout_ms);
  while (ws_threads_starting.load() > 0 &&
	 std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

    // counted before the thread exists so an exit can never miss it
    ws_threads_starting.fetch_add(1);

    // Start the WebSocket server thread
    websocket_thread = std::thread(&TclServer::start_websocket_server, this);
  }
   
  // the process thread
  process_thread = std::thread(&process_requests, this);

  // the dservAfter timer thread (one-shot deferred scripts)
  after_thread = std::thread(&TclServer::after_loop, this);
}

TclServer::~TclServer()
{
  delete eventDispatcher;

  shutdown();

  // stop the dservAfter timer thread
  { std::lock_guard<std::mutex> lk(after_mutex); after_stop = true; }
  after_cv.notify_all();
  if (after_thread.joinable()) after_thread.join();

  if (websocket_port() >= 0)
    websocket_thread.detach();

  if (message_port() >= 0)
    message_net_thread.detach();

  if (newline_port() >= 0)
    newline_net_thread.detach();

  process_thread.join();

  // Remove our SendClient from the Dataserver's send_table.
  // This pushes the shutdown_dpoint so the detached SendClient thread
  // exits.  That thread may still be draining dpoints into our queue,
  // and its final act is to push REQ_QUEUE_EOS — so drain until we see
  // it (bounded), guaranteeing the producer is finished before this
  // object (and the queue member) is destroyed.
  if (ds && !client_name.empty()) {
    if (ds->remove_send_client_by_id(client_name)) {
      auto deadline =
	std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < deadline) {
	if (queue.size() == 0) {
	  std::this_thread::sleep_for(std::chrono::milliseconds(1));
	  continue;
	}
	client_request_t req = queue.front();
	queue.pop_front();
	if (req.type == REQ_QUEUE_EOS) break;
	if (req.type == REQ_DPOINT_SCRIPT && req.dpoint)
	  dpoint_free(req.dpoint);
      }
    }
  }
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
  struct sockaddr client_address;
  socklen_t client_address_len = sizeof(client_address);
  int socket_fd;
  int new_socket_fd;        // client socket
  int on = 1;               // TCP_NODELAY on accepted sockets

  socket_fd = listener_socket_or_die(newline_port(), "newline listener");

  while (!m_bDone) {
    /* Accept connection to client. */
    new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
    if (new_socket_fd == -1) {
      perror("accept");
      continue;
    }
    accepted_socket_cloexec(new_socket_fd);

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
     std::cout << "Per-IP connection limit reached (" << per_ip_limit(client_ip)
           << "), rejecting client from " << client_ip
           << " (current: " << current_ip_connections << ")" << std::endl;
       }
       dump_ip_connections_locked(client_ip);
       close(new_socket_fd);
       continue;
    }

    register_connection(new_socket_fd, client_ip, get_client_port(client_address));
    
    setsockopt(new_socket_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    dserv_set_keepalive(new_socket_fd);   /* reap peers that vanish */
    
    std::thread thr(tcp_client_process, this, new_socket_fd, &queue);
    thr.detach();
  }
  
  close(socket_fd);
}


void TclServer::start_message_server(void)
{
  struct sockaddr client_address;
  socklen_t client_address_len = sizeof(client_address);
  int socket_fd;
  int new_socket_fd;
  int on = 1;               // TCP_NODELAY on accepted sockets

  socket_fd = listener_socket_or_die(message_port(), "message listener");

  while (!m_bDone) {
    /* Accept connection to client. */
    new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
    if (new_socket_fd == -1) {
      perror("accept");
      continue;
    }
    accepted_socket_cloexec(new_socket_fd);

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
    std::cout << "Message server: Per-IP connection limit reached (" << per_ip_limit(client_ip)
          << "), rejecting client from " << client_ip
          << " (current: " << current_ip_connections << ")" << std::endl;
      }
      dump_ip_connections_locked(client_ip);
      close(new_socket_fd);
      continue;
    }

    setsockopt(new_socket_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    dserv_set_keepalive(new_socket_fd);   /* reap peers that vanish */

    register_connection(new_socket_fd, client_ip, get_client_port(client_address));
    
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
    int limit = per_ip_limit(ip);
    stats << "  " << ip << ": " << count << "/" << limit;
    if (count >= limit * 0.8) {  // Warn at 80% of limit
      stats << " (WARNING: approaching limit)";
    }
    stats << "\n";
  }

  auto now = std::chrono::steady_clock::now();
  stats << "\nHeld connections (fd peer age):\n";
  for (int fd : active_sockets) {
    auto ip_it = socket_to_ip.find(fd);
    auto port_it = socket_to_port.find(fd);
    auto since_it = socket_since.find(fd);
    long age_s = (since_it != socket_since.end()) ?
      std::chrono::duration_cast<std::chrono::seconds>(now - since_it->second).count() : -1;
    stats << "  fd=" << fd << " "
          << ((ip_it != socket_to_ip.end()) ? ip_it->second : std::string("?"))
          << ":" << ((port_it != socket_to_port.end()) ? port_it->second : -1)
          << " age=" << age_s << "s\n";
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
  /* however this function is left (early return, exception, run()
     returning), never leave an exit path waiting on our startup */
  struct StartupLatch {
    TclServer *ts;
    ~StartupLatch() { ts->mark_websocket_started(); }
  } startup_latch{this};

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
    
    // Cache: no-cache for HTML and application code (js/css) so a rig
    // GUI picks up installed updates on a plain reload — a 1h-cached
    // SyncModal.js kept running stale code after installs. Images and
    // fonts keep the short cache; they're content, not code.
    const char* cache_control = "no-cache";
    std::string ext = file_path.substr(file_path.rfind('.') + 1);
    if (ext == "png" || ext == "jpg" ||
        ext == "gif" || ext == "svg" || ext == "woff" || ext == "woff2" || ext == "ico") {
        cache_control = "public, max-age=3600";  // 1 hour for static assets
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
        /* Settings. Payload and backpressure track DSERV_MAX_DATA_LEN:
           any datapoint dserv can hold (a large stimdg's stiminfo JSON
           runs to tens of MB) must fit through this transport in both
           directions. Backpressure is the per-connection outbound
           buffer cap for a slow client — sends beyond it drop (see
           .dropped), and idleTimeout reaps dead peers. */
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = DSERV_MAX_DATA_LEN,
        .idleTimeout = 120,
        .maxBackpressure = DSERV_MAX_DATA_LEN,
        
        .upgrade = [](auto *res, auto *req, auto *context) {
          res->template upgrade<WSPerSocketData>({
              .rqueue = new SharedQueue<std::string>(),
              .client_name = "",
              .channel = nullptr,
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
            // Channel outlives this socket: the notification thread holds a
            // shared_ptr to it, so uWS freeing userData cannot pull it away
            userData->channel = std::make_shared<WSClientChannel>();
            userData->channel->notification_queue =
	      new SharedQueue<client_request_t>();

            // Create async response queue for this client
            userData->async_responses = new SharedQueue<std::string>();

            // Register with Dataserver as a queue-based client
            userData->dataserver_client_id =
	      this->ds->add_new_send_client(userData->channel->notification_queue);

            if (userData->dataserver_client_id.empty()) {
              std::cerr <<
		"Failed to register WebSocket client with Dataserver" <<
		std::endl;
              delete userData->channel->notification_queue;
              userData->channel->notification_queue = nullptr;
              userData->channel.reset();
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

            // Start a thread to process notifications for this client.
            // It captures the channel (shared_ptr, by value) and the name --
            // deliberately NOT userData, which uWS frees at socket destruction
            // while this thread is still running.
            {
              auto channel = userData->channel;
              std::string client_name = userData->client_name;
              std::thread([this, ws, channel, client_name]() {
                this->process_websocket_client_notifications_template(ws, channel,
								      client_name);
              }).detach();
            }

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

                // If client provided a requestId, use async path:
                // push request and return immediately, freeing the uWS event loop.
                // The process thread will send the response back via ws_loop->defer().
                if (requestId_obj && json_is_string(requestId_obj)) {
                  client_request_t req;
                  req.type = REQ_SCRIPT_WS_ASYNC;
                  req.rqueue = nullptr;  // no blocking queue
                  req.script = std::string(script);
                  req.socket_fd = -1;
                  req.websocket_id = userData->client_name;
                  req.request_id = json_string_value(requestId_obj);

                  queue.push_back(req);
                  // Event loop is free — response sent from process_requests
                }
                else {
                  // Original blocking path: no requestId provided.
                  // Kept exactly as-is for backward compatibility.
                  client_request_t req;
                  req.type = REQ_SCRIPT;
                  req.rqueue = userData->rqueue;
                  req.script = std::string(script);
                  req.socket_fd = -1;
                  req.websocket_id = userData->client_name;

                  queue.push_back(req);

                  // Wait for response (blocks uWS event loop — legacy behavior)
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

                  char *response_str = json_dumps(response, 0);
                  ws->send(response_str, uWS::OpCode::TEXT);
                  free(response_str);
                  json_decref(response);
                }
              }
            }

	    else if (strcmp(cmd, "clear") == 0) {
	      // Handle datapoint clear (non-blocking, no Tcl eval)
	      json_t *names_obj = json_object_get(root, "names");
	      
	      if (names_obj && json_is_array(names_obj)) {
		size_t index;
		json_t *val;
		json_array_foreach(names_obj, index, val) {
		  if (json_is_string(val)) {
		    ds->clear((char *)json_string_value(val));
		  }
		}
	      }
	      // No response needed — fire and forget
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
	    
            else if (strcmp(cmd, "clear") == 0) {
              // Handle datapoint clear (non-blocking, no Tcl eval)
              json_t *names_obj = json_object_get(root, "names");

              if (names_obj && json_is_array(names_obj)) {
                size_t index;
                json_t *val;
                json_array_foreach(names_obj, index, val) {
                  if (json_is_string(val)) {
                    ds->clear((char *)json_string_value(val));
                  }
                }
              }
              // No response needed — fire and forget
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
                
                // Store the subscription for this WebSocket client.
                // Under subs_mutex: the notification thread iterates this
                // vector, and a reallocating push_back would invalidate it
                // mid-iteration.
                if (userData->channel) {
                  std::lock_guard<std::mutex> lock(userData->channel->subs_mutex);
                  userData->channel->subscriptions.push_back(std::string(match));
                }

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
                
                // Remove from local subscriptions (see subscribe: the
                // notification thread reads this under the same mutex)
                if (userData->channel) {
                  std::lock_guard<std::mutex> lock(userData->channel->subs_mutex);
                  auto &subs = userData->channel->subscriptions;
                  auto it = std::find(subs.begin(), subs.end(), match);
                  if (it != subs.end()) {
                    subs.erase(it);
                  }
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
              
              if (userData->channel) {
                std::lock_guard<std::mutex> lock(userData->channel->subs_mutex);
                for (const std::string& sub : userData->channel->subscriptions) {
                  json_array_append_new(subs_array, json_string(sub.c_str()));
                }
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

		if (dp && DPOINT_IS_PRIVATE(dp)) {
		  dpoint_free(dp);
		  dp = nullptr;	/* report as not found */
		}

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
          /* Drain fires while a buffered send is still flushing. Do NOT
             close here on ordinary backpressure: any reply larger than
             one TCP write (a multi-MB stimdg JSON to a browser on a real
             network) transiently buffers well past any small threshold,
             and closing mid-send silently killed every large-datapoint
             viewer. The 1MB guard that used to live here did exactly
             that. Slow/runaway clients are already bounded by
             maxBackpressure (24MB, drops new sends) and idleTimeout. */
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
            
            // Remove from Dataserver's send_table.  The SendClient
            // thread will push REQ_QUEUE_EOS into our notification
            // queue as its final act, and the notification thread
            // frees the queue only after seeing it — the producer
            // announces end-of-stream, so no sleep/guess is needed.
            bool producer_active = false;
            if (!userData->dataserver_client_id.empty()) {
              producer_active =
                this->ds->remove_send_client_by_id(userData->dataserver_client_id) != 0;
            }

	    // Cleanup linked subprocesses
	    this->cleanup_subprocesses_for_websocket(userData->client_name);

            // If no producer existed (registration failed or already
            // removed), unblock the notification thread ourselves
            if (userData->channel && userData->channel->notification_queue &&
		!producer_active) {
              client_request_t eos_req;
              eos_req.type = REQ_QUEUE_EOS;
              userData->channel->notification_queue->push_back(eos_req);
            }

            // Drop this socket's reference to the channel. The notification
            // thread holds the other one and frees the queue when it sees EOS,
            // so the channel outlives userData by exactly as long as needed.
            userData->channel.reset();

            // Clean up rqueue
            delete userData->rqueue;
            userData->rqueue = nullptr;

            // Clean up async response queue.
            // The ws_connections entry was already erased above (under mutex),
            // so no process thread can find this client's queue anymore.
            // But a process thread might have found it just before we erased,
            // so we still drain under the same mutex for safety.
            if (userData->async_responses) {
              while (userData->async_responses->size() > 0) {
                userData->async_responses->pop_front();
              }
              delete userData->async_responses;
              userData->async_responses = nullptr;
            }
	    
          } catch (const std::exception& e) {
            std::cerr << "Exception in WebSocket close handler: " << e.what() << std::endl;
          }
        }   
        
	  }).listen("0.0.0.0", websocket_port(), [this](auto *listen_socket) {
      if (listen_socket) {
        std::cout << "WebSocket server listening on port " << websocket_port() << std::endl;
        std::cout << "Web terminal available at http://localhost:" << websocket_port() << "/" << std::endl;
      } else {
        // Same policy as the text listeners (ListenerSocket.h): a server
        // that cannot bind one of its ports must not keep running half-alive.
        std::cerr << "Failed to start WebSocket server on port "
                  << websocket_port()
                  << " -- exiting so systemd can relaunch clean" << std::endl;
        _exit(1);
      }
    });

    /* SSL context, routes and listen socket all exist: past the
       OpenSSL startup window that process exit must not race */
    this->mark_websocket_started();

    app.run();
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
    WebSocketType* ws, std::shared_ptr<WSClientChannel> channel,
    std::string client_name) {
    if (!channel || !channel->notification_queue) {
        std::cerr << "ERROR: channel is null in notification thread" << std::endl;
        return;
    }

    /* Resolved ONCE. The old code re-read this pointer out of the socket's
       userData on every iteration, and uWS frees userData the moment the socket
       is destroyed -- so front() could succeed and the pop_front() on the very
       next line run against a freed (zeroed) pointer. The channel we hold is
       shared_ptr-owned and cannot be pulled out from under us. */
    SharedQueue<client_request_t> *queue = channel->notification_queue;

    bool done = false;

    while (!done) {
      try {
        client_request_t req = queue->front();
        queue->pop_front();

        // REQ_QUEUE_EOS is the producer's (SendClient's) final act;
        // after it, no further pushes can arrive, so the queue
        // deletion below cannot race the producer
        if (req.type == REQ_QUEUE_EOS || req.type == REQ_SHUTDOWN) {
            done = true;
            break;
        }
        
        if (req.type == REQ_DPOINT_SCRIPT && req.dpoint) {
            // Check if this datapoint matches any subscriptions
            bool matches = false;
            std::string dpoint_name(req.dpoint->varname);

            {
              /* the event loop mutates this vector on subscribe/unsubscribe */
              std::lock_guard<std::mutex> lock(channel->subs_mutex);
              for (const std::string& pattern : channel->subscriptions) {
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
                        
                        // Send using ws_loop->defer for thread safety.
                        // The liveness check is not optional: this lambda runs
                        // later, on the loop thread, and the socket may have
                        // been closed and destroyed in between -- ws would then
                        // dangle. Checking ws_connections IS sufficient because
                        // .close erases from it on this same loop thread before
                        // uWS frees the socket, so a hit here means alive.
                        // (sendLargeMessage already did this; this path didn't.)
                        if (ws_loop) {
                            ws_loop->defer([ws, message, client_name, this]() {
                                if (this->isWebSocketConnected(client_name, ws)) {
                                    ws->send(message, uWS::OpCode::TEXT);
                                }
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
    
    // Cleanup. Safe after EOS: that is the producer's final act, so nothing can
    // still push. We own the queue here -- the socket's reference to the
    // channel is already gone (dropped in .close).
    delete queue;
    channel->notification_queue = nullptr;
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

  /*
   * Send-cycle guard (see SendGuard.h for the protocol and why): refuse
   * loudly rather than deadlock every interpreter thread in the loop.
   * On success our sending_to is left published; cleared after the wait.
   */
  std::string cycle = send_cycle_check(this_server, tclserver);
  if (!cycle.empty()) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": send cycle detected (", cycle.c_str(),
                     ") -- a synchronous send around this loop would "
                     "deadlock every interpreter thread in it; use "
                     "send_noreply or restructure the call", NULL);
    return TCL_ERROR;
  }

  /*
   * The reply queue is shared-owned by the queued request, so if we time
   * out below the interp thread's late reply lands in a still-live queue
   * (and is discarded) instead of a dead stack frame.
   */
  auto rqueue = std::make_shared<SharedQueue<std::string>>();
  client_request_t client_request;
  client_request.type = REQ_SCRIPT;
  client_request.rqueue = rqueue.get();
  client_request.owned_rqueue = rqueue;
  client_request.script = concatenated_script; // Use the concatenated string

  tclserver->queue.push_back(client_request);

  /*
   * Timed backstop for wedges the cycle guard cannot see (a target thread
   * stuck outside Tcl, a transitive wait through non-send machinery).
   * Generous default: legitimate sends wrap multi-second work (system
   * loads, world generation).  Override with DSERV_SEND_TIMEOUT_MS.
   */
  static const int send_timeout_ms = [] {
    const char *e = getenv("DSERV_SEND_TIMEOUT_MS");
    int v = e ? atoi(e) : 0;
    return v > 0 ? v : 120000;
  }();

  std::string s;
  bool got = rqueue->wait_pop(s, send_timeout_ms);
  this_server->sending_to.store(nullptr);

  if (!got) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
                     ": send to \"", tclserver->name.c_str(),
                     "\" timed out (target interp busy or wedged; any "
                     "late reply will be discarded)", NULL);
    return TCL_ERROR;
  }

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
      /*
       * A failed config script must be LOUD but not fatal: deleting the
       * child here would also abort the rest of the caller's config
       * (dsconf.tcl stops at the first failing subprocess), and a
       * half-initialized interp is still reachable for diagnosis and
       * re-source ("send <name> {source .../config/<name>conf.tcl}").
       * So keep the subprocess, log the full errorInfo to stderr (the
       * systemd journal), and publish <name>/init_error so agents/GUIs
       * can surface it.  The dpoint survives because the child stays
       * alive: subprocess teardown clears every "name/" datapoint.
       */
      std::string msg = result.substr(11);
      auto einfo = child->eval("set ::errorInfo");
      if (einfo.starts_with("!TCL_ERROR ")) einfo = msg;
      std::cerr << "subprocess " << name << ": config script failed: "
                << einfo << std::endl;

      ds_datapoint_t dpoint;
      std::string varname = name + "/init_error";
      dpoint_set(&dpoint, (char *) varname.c_str(),
                 tclserver->ds->now(), DSERV_STRING, msg.size(),
                 (unsigned char *) msg.c_str());
      tclserver->ds->set(dpoint);
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



/*
 * dservTiming ?on|off|reset|stats?
 *   Report timing for this interpreter's serialized request path.
 *   With no argument, equivalent to "stats".
 */
static int dserv_timing_command(ClientData data, Tcl_Interp *interp,
				int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;

  const char *sub = (objc > 1) ? Tcl_GetString(objv[1]) : "stats";

  if (!strcmp(sub, "on")) {
    tclserver->timing.set_enabled(true);
  }
  else if (!strcmp(sub, "off")) {
    tclserver->timing.set_enabled(false);
  }
  else if (!strcmp(sub, "reset")) {
    tclserver->timing.reset();
  }
  else if (!strcmp(sub, "stats")) {
    std::string s = tclserver->timing.report();
    Tcl_SetObjResult(interp, Tcl_NewStringObj(s.c_str(), -1));
  }
  else if (!strcmp(sub, "slow")) {
    std::string s = tclserver->timing.slowest();
    Tcl_SetObjResult(interp, Tcl_NewStringObj(s.c_str(), -1));
  }
  else if (!strcmp(sub, "labels")) {
    std::string s = tclserver->timing.labels();
    Tcl_SetObjResult(interp, Tcl_NewStringObj(s.c_str(), -1));
  }
  else {
    Tcl_WrongNumArgs(interp, 1, objv,
		     "?on|off|reset|stats|slow|labels?");
    return TCL_ERROR;
  }

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

/*
 * dpointAddScript varname script
 *
 *   Register an ADDITIONAL script for varname, keeping any already there.
 *   dpointSetScript stays "this is now the script": callers re-register to
 *   change behaviour, and turning that into an append would silently
 *   double-fire every existing registration.
 *
 *   Scripts run in registration order on the process thread. Publishing
 *   from inside one enqueues a request that is drained on a LATER pass, so
 *   two consumers of one datapoint never nest -- and a consumer that errors
 *   does not stop the ones after it.
 */
static int dpoint_add_script_command (ClientData data, Tcl_Interp *interp,
                                      int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;

  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname script");
    return TCL_ERROR;
  }

  tclserver->dpoint_scripts.append(std::string(Tcl_GetString(objv[1])),
                                   std::string(Tcl_GetString(objv[2])));
  return TCL_OK;
}

static int dpoint_remove_script_command (ClientData data, Tcl_Interp *interp,
                         int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname ?script?");
    return TCL_ERROR;
  }

  /* With a script argument, remove just that one and leave its siblings --
     needed once a name can carry several. Without one, remove them all,
     which is what this command has always done. */
  if (objc > 2) {
    tclserver->dpoint_scripts.remove_script(std::string(Tcl_GetString(objv[1])),
                                            std::string(Tcl_GetString(objv[2])));
  }
  else {
    tclserver->dpoint_scripts.remove(std::string(Tcl_GetString(objv[1])));
  }

  return TCL_OK;
  }

/*
 * Introspection for the dpoint script registry.
 *
 * These exist because the registry was previously WRITE-ONLY: there was no way
 * to ask what was registered, so a leaked script (see dpointSetScript's empty-
 * script trap) was invisible and cost a Tcl dispatch per publish indefinitely.
 * `dpointScripts` lists registered datapoints; `dpointGetScript` returns one.
 */
/*
 * dservClockEpochOffset -- the constant in dserv's timebase, in microseconds.
 *
 *   Dataserver::now() = clock_epoch_offset_us() + steady_us()
 *
 * i.e. CLOCK_MONOTONIC plus a value captured once at startup. Exposed because
 * bridging an external hardware clock onto dserv's timeline needs it: a PTP
 * grandmaster's PHC can be related to CLOCK_MONOTONIC locally (see
 * extio-zephyr/host/phc_offset.c), but converting that into dserv time requires
 * this constant, which was previously reachable only from C
 * (tclserver_clock_epoch_offset_us, used by modules/gpio_input).
 *
 *   dserv_us = phc_us - (phc_minus_mono_us) + [dservClockEpochOffset]
 */
static int dserv_clock_epoch_offset_command (ClientData data, Tcl_Interp *interp,
                                             int objc, Tcl_Obj *objv[])
{
  Tcl_SetObjResult(interp,
                   Tcl_NewWideIntObj((Tcl_WideInt) Dataserver::clock_epoch_offset_us()));
  return TCL_OK;
}

/*
 * dservPhcOffset /dev/ptpN -- PHC minus CLOCK_MONOTONIC, the other half of the
 * constant dservClockEpochOffset provides. Together:
 *
 *     D = dserv_us - ptp_us = [dservClockEpochOffset] - (phc_us - mono_us)
 *
 * Lives here, beside dservClockEpochOffset, because dserv owns the clock this is
 * being related TO. It was previously an external helper
 * (extio-zephyr/host/phc_offset.c) that config/ptpconf.tcl had to exec: that
 * meant a binary outside the dserv install to locate and keep in sync, a ~2 s
 * fork+exec per measurement where the ioctl itself costs microseconds, and
 * stdout as the contract. The standalone tool REMAINS for bench work -- it does
 * the two-method cross-check, drift fit and residuals this does not.
 *
 * Returns a DICT, not a bare integer:
 *
 *     ns <n>  window <n>  method A|B  phc <device>
 *
 * deliberately, so a caller can judge how much to trust it and refuse to anchor
 * on a bad measurement. A bare number is a value that cannot say how good it is.
 *
 *   method B  PTP_SYS_OFFSET_PRECISE, a hardware cross-timestamp. `window` is
 *             the tiny CLOCK_MONOTONIC_RAW<->CLOCK_MONOTONIC pair (tens of ns);
 *             the PHC correlation itself happens in hardware. ~+/-11 ns
 *             measured on an Intel I226-V.
 *   method A  fallback where the driver has no getcrosststamp (the Pi 5 does
 *             not): sandwich a PHC read between two CLOCK_MONOTONIC reads, keep
 *             the least-interrupted of several. `window` is that read window and
 *             bounds the error at half of it. ~+/-704 ns on a Pi 5.
 */
#if defined(__linux__)
#define DSERV_PHC_FD_TO_CLOCKID(fd) ((~(clockid_t) (fd) << 3) | 3)
#define DSERV_PHC_SANDWICH_TRIES    21

static int64_t dserv_phc_ts_ns(const struct timespec *t)
{
  return (int64_t) t->tv_sec * 1000000000LL + t->tv_nsec;
}

static int64_t dserv_phc_pct_ns(const struct ptp_clock_time *t)
{
  return (int64_t) t->sec * 1000000000LL + t->nsec;
}
#endif

static int dserv_phc_offset_command (ClientData data, Tcl_Interp *interp,
                                     int objc, Tcl_Obj *objv[])
{
  if (objc != 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "/dev/ptpN");
    return TCL_ERROR;
  }

#if !defined(__linux__)
  Tcl_SetObjResult(interp,
    Tcl_NewStringObj("dservPhcOffset: PHC access is Linux-only", -1));
  return TCL_ERROR;
#else
  const char *dev = Tcl_GetString(objv[1]);
  int fd = open(dev, O_RDONLY);

  if (fd < 0) {
    Tcl_SetObjResult(interp, Tcl_ObjPrintf("dservPhcOffset: cannot open %s: %s",
                                           dev, strerror(errno)));
    return TCL_ERROR;
  }

  int64_t off_ns = 0, win_ns = 0;
  const char *method = NULL;

  /* Method B first: hardware cross-timestamp, if the driver implements it. */
  {
    struct ptp_sys_offset_precise sp;

    memset(&sp, 0, sizeof sp);
    if (ioctl(fd, PTP_SYS_OFFSET_PRECISE, &sp) == 0) {
      struct timespec r0, m, r1;

      /* PTP_SYS_OFFSET_PRECISE yields MONOTONIC_RAW, which is NOT NTP-slewed,
         while dserv's timebase follows CLOCK_MONOTONIC, which is. Relate them
         locally; this small pair is the only uncertainty method B carries. */
      if (clock_gettime(CLOCK_MONOTONIC_RAW, &r0) == 0 &&
          clock_gettime(CLOCK_MONOTONIC,     &m)  == 0 &&
          clock_gettime(CLOCK_MONOTONIC_RAW, &r1) == 0) {
        int64_t raw_minus_mono =
          (dserv_phc_ts_ns(&r0) + dserv_phc_ts_ns(&r1)) / 2 - dserv_phc_ts_ns(&m);

        off_ns = (dserv_phc_pct_ns(&sp.device) -
                  dserv_phc_pct_ns(&sp.sys_monoraw)) + raw_minus_mono;
        win_ns = dserv_phc_ts_ns(&r1) - dserv_phc_ts_ns(&r0);
        method = "B";
      }
    }
  }

  /* Method A: min-filtered sandwich. */
  if (!method) {
    clockid_t phc = DSERV_PHC_FD_TO_CLOCKID(fd);
    int got = 0;

    for (int i = 0; i < DSERV_PHC_SANDWICH_TRIES; i++) {
      struct timespec m0, p, m1;

      if (clock_gettime(CLOCK_MONOTONIC, &m0)) continue;
      if (clock_gettime(phc, &p))              continue;
      if (clock_gettime(CLOCK_MONOTONIC, &m1)) continue;

      int64_t w = dserv_phc_ts_ns(&m1) - dserv_phc_ts_ns(&m0);
      int64_t o = dserv_phc_ts_ns(&p) -
                  (dserv_phc_ts_ns(&m0) + dserv_phc_ts_ns(&m1)) / 2;

      if (!got || w < win_ns) { win_ns = w; off_ns = o; got = 1; }
    }
    if (got) method = "A";
  }

  close(fd);

  if (!method) {
    Tcl_SetObjResult(interp, Tcl_ObjPrintf("dservPhcOffset: no usable read from %s",
                                           dev));
    return TCL_ERROR;
  }

  Tcl_Obj *d = Tcl_NewDictObj();
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("ns", -1),     Tcl_NewWideIntObj(off_ns));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("window", -1), Tcl_NewWideIntObj(win_ns));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("method", -1), Tcl_NewStringObj(method, -1));
  Tcl_DictObjPut(interp, d, Tcl_NewStringObj("phc", -1),    Tcl_NewStringObj(dev, -1));
  Tcl_SetObjResult(interp, d);
  return TCL_OK;
#endif
}

static int dpoint_scripts_command (ClientData data, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Tcl_Obj *l = Tcl_NewListObj(0, NULL);

  for (auto const &k : tclserver->dpoint_scripts.keys()) {
    Tcl_ListObjAppendElement(interp, l,
                             Tcl_NewStringObj(k.c_str(), -1));
  }
  Tcl_SetObjResult(interp, l);
  return TCL_OK;
}

static int dpoint_get_script_command (ClientData data, Tcl_Interp *interp,
                                      int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }

  std::string script;
  if (!tclserver->dpoint_scripts.find_first(std::string(Tcl_GetString(objv[1])),
                                            script)) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj("", -1));
    return TCL_OK;                     /* not registered -> empty, not an error */
  }
  Tcl_SetObjResult(interp, Tcl_NewStringObj(script.c_str(), -1));
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

// =============================================================================
// Tcl command: tcp_probe
// =============================================================================

// tcp_probe host ?port? ?timeout_ms?
// Returns 1 if TCP connection succeeds, 0 otherwise
static int Tcp_Probe_Cmd(ClientData clientData, Tcl_Interp *interp,
                         int objc, Tcl_Obj *const objv[])
{
  if (objc < 2 || objc > 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "host ?port? ?timeout_ms?");
    return TCL_ERROR;
  }

  const char *host = Tcl_GetString(objv[1]);
  int port = 2560;
  int timeout_ms = 2000;

  if (objc >= 3 && Tcl_GetIntFromObj(interp, objv[2], &port) != TCL_OK)
    return TCL_ERROR;
  if (objc >= 4 && Tcl_GetIntFromObj(interp, objv[3], &timeout_ms) != TCL_OK)
    return TCL_ERROR;

  struct addrinfo hints = {}, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);

  if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
  }

  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock < 0) {
    freeaddrinfo(res);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
  }

  // Set non-blocking
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  int result = 0;
  int rc = connect(sock, res->ai_addr, res->ai_addrlen);

  if (rc == 0) {
    result = 1;
  } else if (errno == EINPROGRESS) {
    struct pollfd pfd = { sock, POLLOUT, 0 };
    if (poll(&pfd, 1, timeout_ms) > 0) {
      int err = 0;
      socklen_t len = sizeof(err);
      getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
      result = (err == 0) ? 1 : 0;
    }
  }

  close(sock);
  freeaddrinfo(res);

  Tcl_SetObjResult(interp, Tcl_NewIntObj(result));
  return TCL_OK;
}

/* ----------------------------------------------------------------------------
 * dservAfter: one-shot deferred scripts (the replacement for Tcl `after ms
 * script`, which never fires here since we don't spin the Tcl event loop).
 * The timer thread waits for the soonest entry and hands its script to the
 * process thread via the request queue -- the same path a module/timer uses --
 * so the script runs in normal process-thread context.
 * -------------------------------------------------------------------------- */
void TclServer::after_loop(void)
{
  std::unique_lock<std::mutex> lk(after_mutex);
  while (!after_stop) {
    if (after_entries.empty()) { after_cv.wait(lk); continue; }

    // soonest-firing entry
    auto soonest = after_entries.begin();
    for (auto it = after_entries.begin(); it != after_entries.end(); ++it)
      if (it->fire < soonest->fire) soonest = it;

    auto now = std::chrono::steady_clock::now();
    if (soonest->fire <= now) {
      std::string script = soonest->script;
      after_entries.erase(soonest);
      lk.unlock();                         // run outside the lock
      client_request_t req;
      req.type = REQ_SCRIPT_NOREPLY;
      req.script = script;
      queue.push_back(req);                // -> process thread evaluates it
      lk.lock();
    } else {
      after_cv.wait_until(lk, soonest->fire);   // wake when due (or on add/cancel)
    }
  }
}

static int dserv_after_command(ClientData data, Tcl_Interp *interp,
                               int objc, Tcl_Obj *const objv[])
{
  TclServer *tserv = (TclServer *) data;
  if (objc != 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "ms script");
    return TCL_ERROR;
  }
  int ms;
  if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK) return TCL_ERROR;
  if (ms < 0) ms = 0;

  int id;
  {
    std::lock_guard<std::mutex> lk(tserv->after_mutex);
    id = tserv->after_next_id++;
    TclServer::AfterEntry e;
    e.id     = id;
    e.fire   = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    e.script = Tcl_GetString(objv[2]);
    tserv->after_entries.push_back(std::move(e));
  }
  tserv->after_cv.notify_all();
  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

static int dserv_after_cancel_command(ClientData data, Tcl_Interp *interp,
                                      int objc, Tcl_Obj *const objv[])
{
  TclServer *tserv = (TclServer *) data;
  if (objc != 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "id");
    return TCL_ERROR;
  }
  int id;
  if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK) return TCL_ERROR;

  int removed = 0;
  {
    std::lock_guard<std::mutex> lk(tserv->after_mutex);
    for (auto it = tserv->after_entries.begin(); it != tserv->after_entries.end(); ++it)
      if (it->id == id) { tserv->after_entries.erase(it); removed = 1; break; }
  }
  tserv->after_cv.notify_all();
  Tcl_SetObjResult(interp, Tcl_NewIntObj(removed));
  return TCL_OK;
}

/* Shadow Tcl's built-in `after`: its deferred forms schedule into the Tcl event
 * loop, which dserv never spins, so they would silently never run.  Make that a
 * loud error and steer callers to the working primitives.  The harmless forms
 * still work: `after cancel`/`after info` are no-ops, and a bare `after <ms>`
 * (no script) is a real blocking delay via Tcl_Sleep. */
static int dserv_after_shim_command(ClientData data, Tcl_Interp *interp,
                                    int objc, Tcl_Obj *const objv[])
{
  (void) data;
  if (objc >= 2) {
    std::string a1 = Tcl_GetString(objv[1]);
    if (a1 == "cancel" || a1 == "info") return TCL_OK;   // nothing was scheduled
    int ms;
    if (objc == 2 && Tcl_GetIntFromObj(NULL, objv[1], &ms) == TCL_OK) {
      if (ms > 0) Tcl_Sleep(ms);                          // bare `after ms` = blocking delay
      return TCL_OK;
    }
  }
  Tcl_AppendResult(interp,
    "after: deferred callbacks are not serviced in dserv (no Tcl event loop). "
    "Use `dservAfter ms script` (one-shot), dservWhen (reactive on a datapoint), "
    "or the timer module (periodic).", NULL);
  return TCL_ERROR;
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
  Tcl_CreateObjCommand(interp, "dservSetPrivate",
		       dserv_setprivate_command, tserv->ds, NULL);
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
  Tcl_CreateObjCommand(interp, "dservSendClients",
		       dserv_send_clients_command, tserv->ds, NULL);

  Tcl_CreateObjCommand(interp, "processGetParam",
               process_get_param_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "processSetParam",
               process_set_param_command, tserv->ds, NULL);

  /* these are specific to TclServers */
  Tcl_CreateObjCommand(interp, "now",
               (Tcl_ObjCmdProc *) now_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservClockEpochOffset",
               (Tcl_ObjCmdProc *) dserv_clock_epoch_offset_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservPhcOffset",
               (Tcl_ObjCmdProc *) dserv_phc_offset_command, tserv, NULL);

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
                                      
  Tcl_CreateObjCommand(interp, "dservTiming",
               (Tcl_ObjCmdProc *) dserv_timing_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservWhen",
               dserv_when_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservWhenCancel",
               dserv_when_cancel_command, tserv, NULL);

  Tcl_CreateObjCommand(interp, "dservAfter",
               dserv_after_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservAfterCancel",
               dserv_after_cancel_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "after",            // shadow Tcl's inert built-in
               dserv_after_shim_command, tserv, NULL);

  Tcl_CreateObjCommand(interp, "dservAddMatch",
               dserv_add_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservAddExactMatch",
               dserv_add_exact_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveMatch",
               dserv_remove_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveAllMatches",
               dserv_remove_all_matches_command, tserv, NULL);
  /* dservAddExactMatch existed with no symmetric remove, so the natural call
   * silently failed and every caller leaked a match (they were all wrapped in
   * `catch`).  Exact and glob matches share one dict keyed by the match string
   * -- client_add_match/client_add_exact_match both insert(match, spec) and
   * client_remove_match removes by that key -- so this is genuinely just the
   * missing name for the same operation. */
  Tcl_CreateObjCommand(interp, "dservRemoveExactMatch",
               dserv_remove_match_command, tserv, NULL);

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
  Tcl_CreateObjCommand(interp, "dpointAddScript",
               (Tcl_ObjCmdProc *) dpoint_add_script_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "dpointRemoveScript",
               (Tcl_ObjCmdProc *) dpoint_remove_script_command,
               tserv, NULL);
  Tcl_CreateObjCommand(interp, "dpointScripts",
               (Tcl_ObjCmdProc *) dpoint_scripts_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dpointGetScript",
               (Tcl_ObjCmdProc *) dpoint_get_script_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dpointRemoveAllScripts",
               (Tcl_ObjCmdProc *) dpoint_remove_all_scripts_command,
               tserv, NULL);

  Tcl_CreateObjCommand(interp, "www_path", Www_Path_Cmd,
		       (ClientData)tserv, NULL);

  Tcl_CreateObjCommand(interp, "print",
               (Tcl_ObjCmdProc *) print_command, tserv, NULL);

  Tcl_CreateObjCommand(interp, "tcp_probe",
               (Tcl_ObjCmdProc *) Tcp_Probe_Cmd, tserv, NULL);
  
  Tcl_LinkVar(interp, "tcpPort", (char *) &tserv->_newline_port,
          TCL_LINK_INT | TCL_LINK_READ_ONLY);


  // Completion support
  TclCompletion::RegisterCompletionCommands(interp);

  // TpoolMap Support
  extern int TpoolMap_Init(Tcl_Interp *interp, TclServer *tserv);
  TpoolMap_Init(interp, tserv);
  
  // HTTPS client commands
  TclHttps_RegisterCommands(interp);  

  // SHA256 client commands
  TclSha256_RegisterCommands(interp);  

  return;
}

static int Tcl_DservAppInit(Tcl_Interp *interp, TclServer *tserv)
{
  if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;

  add_tcl_commands(interp, tserv);

  /*
   * ::dserv_interp -- this interp's own name.
   *
   * An interp otherwise cannot discover it: the name lives out here, and
   * nothing inside the child reflects it.  That matters for anything a
   * PAGE has to route back: settings::put must run in the interp that
   * declared the knob, so settings-1.0.tm stamps this name into every
   * declaration and the schema datapoint carries it.
   *
   * The value is the REGISTRY name, i.e. exactly what `send <name> {...}`
   * takes -- with two values that are not send targets and must be read as
   * such: "dserv" is the main interp (send refuses it; evaluate directly),
   * and "" is a nameless one-off interp, unregistered and unaddressable.
   *
   * Set here rather than injected by dsconf.tcl's subprocess wrapper so
   * that it also covers -link children, interps spawned after boot
   * (virtual_subject, virtual_extio), and main itself.
   */
  Tcl_SetVar2Ex(interp, "dserv_interp", NULL,
                Tcl_NewStringObj(tserv->name.c_str(), -1), TCL_GLOBAL_ONLY);

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

  /* One interpreter under construction at a time, process-wide. dsconf starts
     its subprocesses in sequence, so this serialises nothing that was actually
     running in parallel. See TclInterpInit.h. */
  std::lock_guard<std::mutex> tcl_init_guard(tcl_interp_init_lock());

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

/*
 * dservWhen: non-blocking, predicate-gated datapoint callbacks.
 *
 *   dservWhen key predicate script ?-repeat? ?-body?   -> returns an id
 *   dservWhenCancel id | all
 *
 * When a datapoint matching `key` is delivered and satisfies `predicate` -- a
 * ::tcl::mathop fragment with the datapoint value as the implicit left operand
 * ({>= 143208}, {in {armed fail}}, {ne staging}); empty means "any update" --
 * `script` runs as `script name value`.  One-shot by default (auto-removed
 * after firing); -repeat keeps it live.  A level check at registration fires
 * immediately for the current value(s) that already satisfy.
 *
 * `key` may be a glob (*, ?, []): we then subscribe with a glob match and the
 * registration watches every matching datapoint (so one dservWhen can cover a
 * whole family, e.g. all of a box's di lines), and the level check seeds from
 * every currently-matching datapoint.  An exact key uses an exact subscription.
 *
 * By default `script` is a proc name (bytecode-cached by Tcl -> cheap to fire
 * repeatedly).  With -body, `script` is instead a code snippet: we compile it
 * once into a generated proc `{name value} {<snippet>}` and use that, so it is
 * just as fast as a hand-written proc but reads inline.  The snippet sees the
 * datapoint via `$name` and `$value`.  -body only affects dservWhen; the shared
 * dpointSetScript path is untouched.
 *
 * This is the reactive alternative to a blocking wait: the script runs from the
 * normal delivery path on the process thread, so it never holds that thread and
 * cannot deadlock the code that produces the value.  It keeps its own registry
 * (separate from dpoint_scripts) so it never dislodges a caller's dpointSetScript,
 * and refcounts any subscription it adds so cleanup never removes the caller's.
 */

// Evaluate a when predicate against a delivered dpoint.  1 true, 0 false, -1 err.
static int when_eval_predicate(Tcl_Interp *interp, const std::string &predicate,
                               ds_datapoint_t *dpoint)
{
  if (predicate.empty()) return 1;               // no predicate -> any update

  Tcl_Obj *v = dpoint_to_tclobj(interp, dpoint);
  if (!v) v = Tcl_NewObj();
  Tcl_IncrRefCount(v);
  Tcl_SetVar2Ex(interp, "__dservWhenVal", NULL, v, TCL_GLOBAL_ONLY);

  // Split the predicate into operator (first word) + remaining rhs, and run it
  // as `::tcl::mathop::<op> $val <rest>` so bareword operands stay strings and
  // $vars resolve in the caller's frame (same rationale as the value comparison
  // in a mathop expression, not an expr which rejects barewords).
  size_t p = 0, n = predicate.size();
  while (p < n && isspace((unsigned char) predicate[p])) p++;
  size_t opstart = p;
  while (p < n && !isspace((unsigned char) predicate[p])) p++;
  std::string op = predicate.substr(opstart, p - opstart);
  std::string rest = (p < n) ? predicate.substr(p) : std::string();
  std::string cmd = "::tcl::mathop::" + op + " $::__dservWhenVal " + rest;

  int b = 0;
  int rc = Tcl_EvalEx(interp, cmd.c_str(), -1, 0);
  if (rc == TCL_OK)
    rc = Tcl_GetBooleanFromObj(interp, Tcl_GetObjResult(interp), &b);
  Tcl_DecrRefCount(v);
  return (rc == TCL_OK) ? (b ? 1 : 0) : -1;
}

// Remove a when registration by id: delete its generated proc (if any), release
// its subscription refcount (dropping the underlying match once the last owner
// goes away), and erase it.
static void when_remove(TclServer *tserv, Tcl_Interp *interp, int id)
{
  auto it = std::find_if(tserv->when_callbacks.begin(),
                         tserv->when_callbacks.end(),
                         [&](const WhenCallback &c){ return c.id == id; });
  if (it == tserv->when_callbacks.end()) return;

  if (it->generated)
    Tcl_DeleteCommand(interp, it->script.c_str());

  if (it->owns_match) {
    auto rit = tserv->when_match_refs.find(it->pattern);
    if (rit != tserv->when_match_refs.end() && --rit->second <= 0) {
      tserv->ds->client_remove_match(tserv->client_name,
                                     (char *) it->pattern.c_str());
      tserv->when_match_refs.erase(rit);
    }
  }
  tserv->when_callbacks.erase(it);
}

// Dispatch delivered datapoint `dpoint` to matching when-callbacks.  Runs on the
// process thread from the REQ_DPOINT_SCRIPT path.
static void run_when_callbacks(TclServer *tserv, Tcl_Interp *interp,
                               ds_datapoint_t *dpoint)
{
  if (tserv->when_callbacks.empty()) return;

  const char *varname = dpoint->varname;

  // Collect ids to fire first: a firing script may add/remove callbacks, so we
  // must not iterate the live vector while firing.
  std::vector<int> fire_ids;
  for (auto &cb : tserv->when_callbacks) {
    if (Tcl_StringMatch(varname, cb.pattern.c_str())) {
      if (when_eval_predicate(interp, cb.predicate, dpoint) == 1)
        fire_ids.push_back(cb.id);
    }
  }

  for (int id : fire_ids) {
    auto it = std::find_if(tserv->when_callbacks.begin(),
                           tserv->when_callbacks.end(),
                           [&](const WhenCallback &c){ return c.id == id; });
    if (it == tserv->when_callbacks.end()) continue;   // cancelled meanwhile
    std::string script = it->script;                   // copy before firing
    bool once = it->once;
    dpoint_tcl_script(interp, script.c_str(), dpoint); // runs `script name value`
    if (once) when_remove(tserv, interp, id);
  }
}

static int dserv_when_command(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *const objv[])
{
  TclServer *tserv = (TclServer *) data;

  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "key predicate script ?-repeat? ?-body?");
    return TCL_ERROR;
  }
  std::string key = Tcl_GetString(objv[1]);
  std::string predicate = Tcl_GetString(objv[2]);
  std::string script = Tcl_GetString(objv[3]);
  bool once = true, body = false;
  for (int i = 4; i < objc; i++) {
    std::string opt = Tcl_GetString(objv[i]);
    if (opt == "-repeat") once = false;
    else if (opt == "-body") body = true;
    else {
      Tcl_AppendResult(interp, "bad option \"", opt.c_str(),
                       "\": must be -repeat or -body", NULL);
      return TCL_ERROR;
    }
  }

  int id = tserv->when_next_id++;

  // Resolve the callback command: a caller-supplied proc name, or -- with -body
  // -- a proc we generate once from the snippet (compiled body cached by Tcl,
  // so firing is as cheap as a proc while reading inline).  Do this before
  // touching subscription state so a bad snippet leaves nothing registered.
  std::string callback = script;
  bool generated = false;
  if (body) {
    callback = "::__dservWhen_cb_" + std::to_string(id);
    Tcl_Obj *def[4] = {
      Tcl_NewStringObj("proc", -1), Tcl_NewStringObj(callback.c_str(), -1),
      Tcl_NewStringObj("name value", -1), Tcl_NewStringObj(script.c_str(), -1)
    };
    for (int i = 0; i < 4; i++) Tcl_IncrRefCount(def[i]);
    int rc = Tcl_EvalObjv(interp, 4, def, 0);
    for (int i = 0; i < 4; i++) Tcl_DecrRefCount(def[i]);
    if (rc != TCL_OK) return TCL_ERROR;   // interp result holds the error
    generated = true;
  }

  // A key with glob metacharacters watches many datapoints; subscribe (and the
  // Tcl_StringMatch in run_when_callbacks) glob-match accordingly.
  bool glob = key.find_first_of("*?[") != std::string::npos;

  // Ensure a subscription so a value change to `key` is delivered to us.  If we
  // already own one for this pattern, bump its refcount; else add one only if
  // nothing already covers the key (never clobbering the caller's matches).
  bool owns = false;
  auto rit = tserv->when_match_refs.find(key);
  if (rit != tserv->when_match_refs.end()) {
    rit->second++;
    owns = true;
  }
  else if (!tserv->ds->client_covers(tserv->client_name, (char *) key.c_str())) {
    if (glob)
      tserv->ds->client_add_match(tserv->client_name, (char *) key.c_str(), 1);
    else
      tserv->ds->client_add_exact_match(tserv->client_name,
                                        (char *) key.c_str(), 1);
    tserv->when_match_refs[key] = 1;
    owns = true;
  }

  tserv->when_callbacks.push_back(
      WhenCallback{ id, key, predicate, callback, once, owns, generated });

  // Level check: fire immediately for the current value(s) already satisfying,
  // so a datapoint already present at registration isn't missed.  For a glob
  // key this seeds from every currently-matching datapoint (subsuming the old
  // `foreach [dservKeys]` seed); a -once registration stops after the first.
  bool done = false;
  auto level_try = [&](const char *k) {
    if (done) return;
    ds_datapoint_t *dp = tserv->ds->get_datapoint((char *) k);
    if (!dp) return;
    if (DPOINT_IS_PRIVATE(dp)) { dpoint_free(dp); return; }
    if (when_eval_predicate(interp, predicate, dp) == 1) {
      dpoint_tcl_script(interp, callback.c_str(), dp);
      if (once) { when_remove(tserv, interp, id); done = true; }
    }
    dpoint_free(dp);
  };
  if (glob) {
    char *keys = tserv->ds->get_table_keys();   // space-separated; we free it
    if (keys) {
      std::string ks(keys);
      free(keys);
      size_t pos = 0;
      while (pos < ks.size() && !done) {
        size_t sp = ks.find(' ', pos);
        std::string k = ks.substr(pos, sp == std::string::npos ? sp : sp - pos);
        pos = (sp == std::string::npos) ? ks.size() : sp + 1;
        if (!k.empty() && Tcl_StringMatch(k.c_str(), key.c_str()))
          level_try(k.c_str());
      }
    }
  }
  else {
    level_try(key.c_str());
  }

  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

static int dserv_when_cancel_command(ClientData data, Tcl_Interp *interp,
                                     int objc, Tcl_Obj *const objv[])
{
  TclServer *tserv = (TclServer *) data;

  if (objc != 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "id|all");
    return TCL_ERROR;
  }
  if (std::string(Tcl_GetString(objv[1])) == "all") {
    while (!tserv->when_callbacks.empty())
      when_remove(tserv, interp, tserv->when_callbacks.front().id);
    return TCL_OK;
  }
  int id;
  if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK) return TCL_ERROR;
  when_remove(tserv, interp, id);
  return TCL_OK;
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

    uint64_t t_dequeue = request_timing_now_ns();

    /*
     * Label the request so slow ones can be attributed to real code.
     * Must be built BEFORE the switch: the REQ_DPOINT_SCRIPT case frees
     * req.dpoint, so reading varname afterwards is a use-after-free.
     * Only built while timing is on, since it allocates.
     */
    std::string timing_label;
    if (tserv->timing.enabled()) {
      switch (req.type) {
      case REQ_SCRIPT:
      case REQ_SCRIPT_NOREPLY:
      case REQ_SCRIPT_WS_ASYNC:
	timing_label = request_timing_script_label(req.script);
	break;
      case REQ_DPOINT_SCRIPT:
	timing_label = "dpoint_script " +
	  std::string(req.dpoint && req.dpoint->varname ?
		      req.dpoint->varname : "?");
	break;
      case REQ_DPOINT:
	timing_label = "dpoint_set " +
	  std::string(req.dpoint && req.dpoint->varname ?
		      req.dpoint->varname : "?");
	break;
      case REQ_TIMER:
	timing_label = "timer";
	break;
      default:
	timing_label = "type_" + std::to_string((int) req.type);
	break;
      }
    }

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
    case REQ_SCRIPT_WS_ASYNC:
      {
        const char *script = req.script.c_str();
        retcode = Tcl_Eval(interp, script);
        const char *rcstr = Tcl_GetStringResult(interp);

        json_t *response = json_object();
        if (retcode == TCL_OK) {
          json_object_set_new(response, "status", json_string("ok"));
          json_object_set_new(response, "result",
                              json_string(rcstr ? rcstr : ""));
        } else {
          json_object_set_new(response, "status", json_string("error"));
          json_object_set_new(response, "error",
                              json_string(rcstr ? rcstr : "Error"));
        }

        if (!req.request_id.empty()) {
          json_object_set_new(response, "requestId",
                              json_string(req.request_id.c_str()));
        }

        char *response_str = json_dumps(response, 0);
        std::string msg(response_str);
        free(response_str);
        json_decref(response);

        tserv->sendAsyncResponse(req.websocket_id, msg);
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
	//
	// An EMPTY script is not evaluated. `dpointSetScript <dp> {}` reads like
	// "clear this" and was used that way throughout the harnesses, but it
	// stores "" rather than removing the entry -- and this path used to
	// evaluate it on EVERY publish of that datapoint, forever, which cost a
	// full Tcl dispatch per delivery and was invisible. Measured at ~130 us
	// per publish on a Pi 5 (see extio-zephyr/PORTING.md 2026-07-26).
	//
	// The entry is deliberately still LOOKED UP: an empty exact entry keeps
	// shadowing a wildcard script, so this change removes the cost without
	// changing which script wins. Use dpointRemoveScript to actually remove.
	// A name may carry several scripts (dpointAddScript). They run in
	// registration order, and each is dispatched independently:
	// dpoint_tcl_script's return code is deliberately not consulted, so
	// one consumer erroring cannot silence the consumers after it. That
	// matters now that two unrelated subsystems can share a datapoint --
	// a broken one must not take the other down with it.
	std::vector<std::string> scripts;
	if (tserv->dpoint_scripts.find(varname, scripts) ||
	    tserv->dpoint_scripts.find_match(varname, scripts)) {
	  for (auto const &script : scripts) {
	    if (!script.empty()) {
	      ds_datapoint_t *dpoint = req.dpoint;
	      const char *dpoint_script = script.c_str();
	      int retcode = dpoint_tcl_script(interp, dpoint_script, dpoint);
	    }
	  }
	}

	// fire any predicate-gated dservWhen callbacks for this point
	run_when_callbacks(tserv, interp, dpoint);

	dpoint_free(dpoint);
      }
    default:
      break;
    }

    if (tserv->timing.enabled())
      tserv->timing.record(req.type, timing_label, req.t_enqueue, t_dequeue,
			   request_timing_now_ns());

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

std::string TclServer::eval(const char *s)
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

void TclServer::eval_noreply(const char *s)
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

  // Each client has its own request structure and reply queue.  The queue
  // is shared-owned by every queued copy of the request, so a handler
  // that abandons a wait (client vanished mid-eval) leaves a live queue
  // for the interp thread's late reply to land in harmlessly.
  auto rqueue = std::make_shared<SharedQueue<std::string>>();
  client_request_t client_request;
  client_request.rqueue = rqueue.get();
  client_request.owned_rqueue = rqueue;
  client_request.type = REQ_SCRIPT;
  client_request.socket_fd = sockfd;
  client_request.websocket_id = "";
  std::string script;
  bool client_gone = false;

  while (!client_gone && (rval = recv(sockfd, buf, sizeof(buf), 0)) > 0) {
    for (int i = 0; i < rval; i++) {
      char c = buf[i];
      if (c == '\n') {
    // shutdown if main server has shutdown
    if (tserv->m_bDone) break;

    if (script.length() > 0) {
      std::string s;
      client_request.script = std::string(script);

      // this struct is reused for the life of the connection, so the
      // construction-time stamp is stale; re-stamp at each enqueue
      client_request.t_enqueue = request_timing_now_ns();

      // push request onto queue for main thread to retrieve
      queue->push_back(client_request);

      // Wait for the reply, but keep noticing the socket: while parked
      // here this thread neither reads nor frees its connection slot, so
      // a wedged interp used to pin the slot forever and lock the per-IP
      // budget shut (2026-08-29 outage).  If the client hangs up while
      // we wait, give the slot back; the eventual reply is discarded via
      // the shared queue.
      bool got = false;
      while (!(got = rqueue->wait_pop(s, 2000))) {
#ifndef _MSC_VER
        char probe;
        ssize_t pr = recv(sockfd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (pr == 0 ||
            (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          client_gone = true;
          break;
        }
#endif
      }
      if (client_gone) break;

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

  // Shared-owned reply queue -- see tcp_client_process for the rationale
  // (abandoning a wait must leave a live queue for the late reply).
  auto rqueue = std::make_shared<SharedQueue<std::string>>();
  client_request_t client_request;
  client_request.rqueue = rqueue.get();
  client_request.owned_rqueue = rqueue;
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

      // reused struct (see tcp_client_process): re-stamp at each enqueue
      client_request.t_enqueue = request_timing_now_ns();

      // push request onto queue for main thread to retrieve
      queue->push_back(client_request);

      // Wait for the reply while watching for a vanished client, so a
      // wedged interp cannot pin this connection slot forever (see
      // tcp_client_process).
      bool got = false;
      bool client_gone = false;
      while (!(got = rqueue->wait_pop(s, 2000))) {
#ifndef _MSC_VER
        char probe;
        ssize_t pr = recv(sockfd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (pr == 0 ||
            (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          client_gone = true;
          break;
        }
#endif
      }
      if (client_gone) {
        delete[] buffer;
        break;
      }
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

void TclServer::sendAsyncResponse(const std::string& client_name,
                                   const std::string& message) {
    if (!ws_loop) return;

    bool pushed = false;
    {
        std::lock_guard<std::mutex> lock(ws_connections_mutex);
        auto it = ws_connections.find(client_name);
        if (it != ws_connections.end()) {
            WSPerSocketData *ud = nullptr;
            if (websocket_ssl_enabled) {
                auto *ws = (uWS::WebSocket<true, true, WSPerSocketData>*)it->second;
                ud = (WSPerSocketData *)ws->getUserData();
            } else {
                auto *ws = (uWS::WebSocket<false, true, WSPerSocketData>*)it->second;
                ud = (WSPerSocketData *)ws->getUserData();
            }
            if (ud && ud->async_responses) {
                ud->async_responses->push_back(message);
                pushed = true;
            }
        }
    }

    if (pushed) {
        ws_loop->defer([this, client_name]() {
            std::lock_guard<std::mutex> lock(ws_connections_mutex);
            auto it = ws_connections.find(client_name);
            if (it == ws_connections.end()) return;

            if (websocket_ssl_enabled) {
                auto *ws = (uWS::WebSocket<true, true, WSPerSocketData>*)it->second;
                WSPerSocketData *ud = (WSPerSocketData *)ws->getUserData();
                if (ud && ud->async_responses) {
                    while (ud->async_responses->size() > 0) {
                        std::string resp = ud->async_responses->front();
                        ud->async_responses->pop_front();
                        ws->send(resp, uWS::OpCode::TEXT);
                    }
                }
            } else {
                auto *ws = (uWS::WebSocket<false, true, WSPerSocketData>*)it->second;
                WSPerSocketData *ud = (WSPerSocketData *)ws->getUserData();
                if (ud && ud->async_responses) {
                    while (ud->async_responses->size() > 0) {
                        std::string resp = ud->async_responses->front();
                        ud->async_responses->pop_front();
                        ws->send(resp, uWS::OpCode::TEXT);
                    }
                }
            }
        });
    }
}
