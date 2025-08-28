#ifndef TCLSERVER_H
#define TCLSERVER_H

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>

#include <stdlib.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#endif

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>

#ifdef __QNX__
#include <sys/socket.h>
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "Dataserver.h"

#include <unordered_map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <sstream>
#include <arpa/inet.h>  // for inet_ntop

// Add uWebSockets support
#include <App.h>

// Add WebSocket per-socket data structure
struct WSPerSocketData {
  SharedQueue<std::string> *rqueue;
  std::string client_name;
  std::vector<std::string> subscriptions;
  
  // Add datapoint notification queue
  SharedQueue<client_request_t> *notification_queue;
  std::string dataserver_client_id;  // ID for Dataserver registration
};

// To help manage large WebSocket messages (stimdg -> ess/stiminfo)
struct ChunkedMessage {
    std::string messageId;
    size_t chunkIndex;
    size_t totalChunks;
    std::string data;
    bool isLastChunk;
};

class TclServerConfig
{
public:
  int newline_listener_port = -1;
  int message_listener_port = -1;
  int websocket_listener_port = -1;  // Add WebSocket port
  std::string name;
  
  TclServerConfig(std::string name, int newline_port, int message_port):
    name(name), newline_listener_port(newline_port), 
    message_listener_port(message_port), websocket_listener_port(-1) {};
    
  TclServerConfig(std::string name):
    name(name), newline_listener_port(-1), 
    message_listener_port(-1), websocket_listener_port(-1) {};
    
  TclServerConfig(std::string name, int port):
    name(name), newline_listener_port(port), 
    message_listener_port(-1), websocket_listener_port(-1) {};
    
  // New constructor for all three ports
  TclServerConfig(std::string name, int newline_port, int message_port, int websocket_port):
    name(name), newline_listener_port(newline_port), 
    message_listener_port(message_port), websocket_listener_port(websocket_port) {};
};
  
class TclServer
{
  std::thread newline_net_thread;
  std::thread message_net_thread;
  std::thread websocket_thread;      // Add WebSocket thread
  std::thread process_thread;
  
  std::mutex mutex;	      // ensure only one thread accesses table
  std::condition_variable cond;	// condition variable for sync

private:
  std::atomic<int> active_connections{0};
  static const int MAX_TOTAL_CONNECTIONS = 128;      // Total server limit
  static const int MAX_CONNECTIONS_PER_IP = 8;       // Per-IP limit
  std::mutex connection_mutex;
  std::set<int> active_sockets;
  std::unordered_map<int, std::string> socket_to_ip;     // socket -> IP mapping
  std::unordered_map<std::string, int> ip_connection_count;

  // WebSocket subscription support
  mutable std::mutex ws_connections_mutex;
  std::map<std::string, uWS::WebSocket<false, true, WSPerSocketData>*> ws_connections;
  uWS::Loop *ws_loop = nullptr;  // Store the loop reference

  static const size_t LARGE_MESSAGE_THRESHOLD = 2 * 1024 * 1024; // 2MB
  static const size_t CHUNK_SIZE = 512 * 1024; // 512KB chunks
  
  void sendLargeMessage(uWS::WebSocket<false, true, WSPerSocketData>* ws,
			const std::string& message,
			const std::string& client_name);

 using CommandRegistrationCallback = std::function<void(Tcl_Interp*, void*)>;
 CommandRegistrationCallback command_callback = nullptr;
 void* command_callback_data = nullptr;
    
public:
  int argc;
  char **argv;

  enum socket_t { SOCKET_LINE, SOCKET_MESSAGE, SOCKET_WEBSOCKET };
  
  std::atomic<bool> m_bDone;	// flag to close process loop

  std::string name;		// name of this TclServer
  
  // identify connection to send process
  std::string client_name;

  // our dataserver
  Dataserver *ds;
  
  // scripts attached to dpoints
  TriggerDict dpoint_scripts;
  
  // for client requests
  SharedQueue<client_request_t> queue;

  const char *PRINT_DPOINT_NAME = "print";

  int _newline_port;		// for CR/LF oriented communication
  int _message_port;		// for message oriented comm  
  int _websocket_port;		// for WebSocket communication

  int newline_port(void) { return _newline_port; }
  int message_port(void) { return _message_port; }
  int websocket_port(void) { return _websocket_port; }

	// Add this method to allow deferred execution on the WebSocket event loop
  void deferToWebSocketLoop(std::function<void()> func) {
	if (ws_loop) {
		ws_loop->defer(func);
	}
   }
	
	bool hasWebSocketLoop() const {
		return ws_loop != nullptr;
	}


  bool accept_new_connection(const std::string& client_ip) {
    std::lock_guard<std::mutex> lock(connection_mutex);
    
    // Check total server limit
    if (active_connections.load() >= MAX_TOTAL_CONNECTIONS) {
      return false;
    }
    
    // Check per-IP limit
    auto ip_count_it = ip_connection_count.find(client_ip);
    int current_ip_connections = (ip_count_it != ip_connection_count.end()) ? ip_count_it->second : 0;
    
    return current_ip_connections < MAX_CONNECTIONS_PER_IP;
    }
    
  void register_connection(int sockfd, const std::string& client_ip) {
    std::lock_guard<std::mutex> lock(connection_mutex);
    active_sockets.insert(sockfd);
    socket_to_ip[sockfd] = client_ip;
    ip_connection_count[client_ip]++;
    active_connections++;
#ifdef LOG_CONNECTIONS    
    std::cout << "Client connected from " << client_ip 
	      << " (IP connections: " << ip_connection_count[client_ip] 
	      << "/" << MAX_CONNECTIONS_PER_IP 
	      << ", total: " << active_connections.load() 
	      << "/" << MAX_TOTAL_CONNECTIONS << ")" << std::endl;
#endif
  }
  
  void unregister_connection(int sockfd) {
    std::lock_guard<std::mutex> lock(connection_mutex);
    auto ip_it = socket_to_ip.find(sockfd);
    std::string client_ip = (ip_it != socket_to_ip.end()) ? ip_it->second : "unknown";
    
    active_sockets.erase(sockfd);
    socket_to_ip.erase(sockfd);
    
    if (ip_it != socket_to_ip.end()) {
      ip_connection_count[client_ip]--;
      if (ip_connection_count[client_ip] <= 0) {
	ip_connection_count.erase(client_ip);  // Clean up zero counts
      }
    }
    
    active_connections--;

#ifndef _MSC_VER
    close(sockfd);
#else
    closesocket(sockfd);
#endif    
    
    auto remaining_count = ip_connection_count.find(client_ip);
    int remaining = (remaining_count != ip_connection_count.end()) ? remaining_count->second : 0;
    
#ifdef LOG_CONNECTIONS    
    std::cout << "Client disconnected from " << client_ip 
	      << " (IP connections: " << remaining 
	      << "/" << MAX_CONNECTIONS_PER_IP 
	      << ", total: " << active_connections.load() 
	      << "/" << MAX_TOTAL_CONNECTIONS << ")" << std::endl;
#endif    
  }
  
  std::string get_client_ip(const struct sockaddr& addr) {
    struct sockaddr_in* addr_in = (struct sockaddr_in*)&addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr_in->sin_addr), ip_str, INET_ADDRSTRLEN);
    return std::string(ip_str);
  }  
  
  // socket type can be SOCKET_LINE (newline oriented) or SOCKET_MESSAGE
  socket_t socket_type;

  bool isWebSocketConnected(const std::string& client_name, uWS::WebSocket<false, true, WSPerSocketData>* ws) {
    std::lock_guard<std::mutex> lock(ws_connections_mutex);
    auto it = ws_connections.find(client_name);
    return it != ws_connections.end() && it->second == ws;
  }
  
  static void
  tcp_client_process(TclServer *tserv,
		     int sock,
		     SharedQueue<client_request_t> *queue);
  
  static void
  message_client_process(TclServer *tserv,
			 int sock,
			 SharedQueue<client_request_t> *queue);
  
  TclServer(int argc, char **argv, Dataserver *dserv, TclServerConfig cfg);
  TclServer(int argc, char **argv, Dataserver *dserv, std::string name);
  TclServer(int argc, char **argv, Dataserver *dserv,
	    std::string name, int port);
  TclServer(int argc, char **argv, Dataserver *dserv,
	    std::string name, int newline_port, int message_port);
  TclServer(int argc, char **argv, Dataserver *dserv,
	    std::string name, int newline_port, int message_port, int websocket_port);
  ~TclServer();
   
  void shutdown(void);
  bool isDone();
  void start_tcp_server(void);
  void start_message_server(void);
  void start_websocket_server(void);  // Add WebSocket server
  std::string get_connection_stats(void);

  void setCommandCallback(CommandRegistrationCallback callback, void* data) {
        command_callback = callback;
        command_callback_data = data;
    }
  bool hasCommandCallback() const { return command_callback != nullptr; }
  void callCommandCallback(Tcl_Interp* interp) { 
        if (command_callback) command_callback(interp, command_callback_data); 
  }
    
  void process_websocket_client_notifications(uWS::WebSocket<false, true, WSPerSocketData>* ws, WSPerSocketData* userData);
  
  int sourceFile(const char *filename);
  uint64_t now(void) { return ds->now(); }
  
  // supported on some platforms
  void setPriority(int priority);
  
  void set_point(ds_datapoint_t *dp);
  int queue_size(void);
  void shutdown_message(SharedQueue<client_request_t> *queue);
  std::string eval(char *s);
  std::string eval(std::string script);
  void eval_noreply(char *s);
  void eval_noreply(std::string script);
};

#endif  // TCLSERVER_H
