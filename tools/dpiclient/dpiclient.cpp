#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>
#include "json.hpp"
using json = nlohmann::json;

#define DPOINT_BINARY_MSG_CHAR '>'
#define DPOINT_BINARY_FIXED_LENGTH (128)

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <netdb.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

class TCPForwarder {
private:
  SOCKET sock;
  bool initialized;
  
public:
  TCPForwarder() : sock(INVALID_SOCKET), initialized(false) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
      std::cerr << "TCP WSAStartup failed" << std::endl;
      return;
    }
#endif
    initialized = true;
  }
  
  ~TCPForwarder() {
    if (sock != INVALID_SOCKET) {
      closesocket(sock);
    }
#ifdef _WIN32
    if (initialized) {
      WSACleanup();
    }
#endif
  }
  
  bool connect(const std::string& host, int port) {
    if (!initialized) {
      return false;
    }
    
    // Create TCP socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
      std::cerr << "Failed to create TCP socket" << std::endl;
      return false;
    }
    
    // Enable TCP_NODELAY for fast transfers
    int flag = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)) == SOCKET_ERROR) {
      std::cerr << "Failed to set TCP_NODELAY" << std::endl;
      closesocket(sock);
      sock = INVALID_SOCKET;
      return false;
    }

    // Set socket timeout for receiving responses (5 seconds)
#ifdef _WIN32
    DWORD timeout = 5000; // milliseconds
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
      std::cerr << "Failed to set SO_RCVTIMEO" << std::endl;
      closesocket(sock);
      sock = INVALID_SOCKET;
      return false;
    }
#else
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == SOCKET_ERROR) {
      std::cerr << "Failed to set SO_RCVTIMEO" << std::endl;
      closesocket(sock);
      sock = INVALID_SOCKET;
      return false;
    }
#endif

    // Setup server address using getaddrinfo
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP
    
    std::string port_str = std::to_string(port);
    int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (status != 0) {
      std::cerr << "Failed to resolve TCP hostname: " << host << std::endl;
      closesocket(sock);
      sock = INVALID_SOCKET;
      return false;
    }
    
    struct sockaddr_in server_addr;
    memcpy(&server_addr, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    
    // Connect to server
    if (::connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
      std::cerr << "Failed to connect to TCP server " << host << ":" << port << std::endl;
      closesocket(sock);
      sock = INVALID_SOCKET;
      return false;
    }
    
    std::cout << "Connected to TCP server: " << host << ":" << port << std::endl;
    return true;
  }

  bool write_to_dataserver(const char *varname, int dtype, int len, void *data)
  {
    uint8_t cmd = DPOINT_BINARY_MSG_CHAR;
    static char buf[DPOINT_BINARY_FIXED_LENGTH];

    uint16_t varlen;
    uint64_t timestamp = 0; // Use 0 for now, can be replaced with actual timestamp if needed
    if (sock == INVALID_SOCKET || varname == nullptr || data == nullptr || len <= 0)
    {
      std::cerr << "Invalid parameters for write_to_dataserver" << std::endl;
      return false;
    }
    uint32_t datatype = dtype, datalen = len;

    uint16_t bufidx = 0;
    uint16_t total_bytes = 0;

    varlen = (int16_t) strlen(varname);

    // Start by seeing how much space we need
    total_bytes += sizeof(uint16_t); // varlen
    total_bytes += varlen;           // strlen(varname)
    total_bytes += sizeof(uint64_t); // timestamp
    total_bytes += sizeof(uint32_t); // datatype
    total_bytes += sizeof(uint32_t); // datalen
    total_bytes += len;              // data

    // data don't fit
    if (total_bytes > sizeof(buf) - 1)
    {
      return false;
    }

    memcpy(&buf[bufidx], &cmd, sizeof(uint8_t));
    bufidx += sizeof(uint8_t);

    memcpy(&buf[bufidx], &varlen, sizeof(uint16_t));
    bufidx += sizeof(uint16_t);

    memcpy(&buf[bufidx], varname, varlen);
    bufidx += varlen;

    memcpy(&buf[bufidx], &timestamp, sizeof(uint64_t));
    bufidx += sizeof(uint64_t);

    memcpy(&buf[bufidx], &datatype, sizeof(uint32_t));
    bufidx += sizeof(uint32_t);

    memcpy(&buf[bufidx], &datalen, sizeof(uint32_t));
    bufidx += sizeof(uint32_t);

    memcpy(&buf[bufidx], data, datalen);
    bufidx += datalen;

    int total_sent = 0;
    while (total_sent < DPOINT_BINARY_FIXED_LENGTH)
    {
      int sent = send(sock, buf + total_sent,
                      DPOINT_BINARY_FIXED_LENGTH - total_sent, 0);
      if (sent == SOCKET_ERROR)
      {
        std::cerr << "Failed to send TCP message" << std::endl;
        return false;
      }
      total_sent += sent;
    }

    return true;
  }

  bool sendMessage(const std::string& message, bool send_as_json_string = true) {
    if (sock == INVALID_SOCKET) {
      return false;
    }

    const char *name = "openiris/frameinfo";
    const int ts = 0;

    if (send_as_json_string)
    {
      const int dtype = 1; // DSERV_STRING
      std::ostringstream oss;
      oss << "%setdata " << name << " " << dtype << " " << ts << " "
          << message.length() << " {" << message << "}\r\n";
      std::string sendbuf = oss.str();

      int total_sent = 0;
      int message_len = static_cast<int>(sendbuf.length());

      while (total_sent < message_len)
      {
        int sent = send(sock, sendbuf.c_str() + total_sent,
                        message_len - total_sent, 0);
        if (sent == SOCKET_ERROR)
        {
          std::cerr << "Failed to send TCP message" << std::endl;
          return false;
        }
        total_sent += sent;
      }

      // Read status message back from TCP server
      char buffer[1024];
      int received = recv(sock, buffer, sizeof(buffer) - 1, 0);

      if (received == SOCKET_ERROR)
      {
        std::cerr << "Failed to receive status message from TCP server (timeout or error)" << std::endl;
        return false;
      }

      if (received == 0)
      {
        std::cerr << "TCP server closed connection without sending status" << std::endl;
        return false;
      }
      //    std::cout << "Forwarded JSON to TCP server: " << message << std::endl;
    }
    else {
      // Parse JSON
      json j = json::parse(message);
      // Create binary data structure
      if (!j.contains("Right") || !j["Right"].contains("Pupil") ||
          !j["Right"]["Pupil"].contains("Center") ||
          !j["Right"].contains("CRs")) {
        std::cerr << "Invalid JSON format" << std::endl;
        return false;
      }
      if (j["Right"]["CRs"].size() < 4) {
        std::cerr << "CRs array must contain at least 4 elements" << std::endl;
        return false;
      }
      if (!j["Right"]["CRs"][0].contains("X") || !j["Right"]["CRs"][0].contains("Y") ||
          !j["Right"]["CRs"][3].contains("X") || !j["Right"]["CRs"][3].contains("Y")) {
        std::cerr << "CRs must contain X and Y coordinates" << std::endl;
        return false;
      }
      if (!j["Right"]["Pupil"].contains("Center") ||
          !j["Right"]["Pupil"]["Center"].contains("X") ||
          !j["Right"]["Pupil"]["Center"].contains("Y")) {
        std::cerr << "Pupil center must contain X and Y coordinates" << std::endl;
        return false;
      }
      if (!j["Right"]["Seconds"].is_number() ||
          !j["Right"]["FrameNumber"].is_number()) {
        std::cerr << "Frame and Time must be numbers" << std::endl;
        return false;
      }
      double data[10] = {
          j["Right"]["FrameNumber"].get<double>(),
          j["Right"]["Seconds"].get<double>(),
          j["Right"]["Pupil"]["Center"]["X"].get<double>(),
          j["Right"]["Pupil"]["Center"]["Y"].get<double>(),
          j["Right"]["CRs"][0]["X"].get<double>(),
          j["Right"]["CRs"][0]["Y"].get<double>(),
          j["Right"]["CRs"][3]["X"].get<double>(),
          j["Right"]["CRs"][3]["Y"].get<double>(),
	  j["Extra"]["Int0"].get<double>(),
	  j["Extra"]["Int1"].get<double>()
	    
      };
      const int dtype = 3; // DSERV_DOUBLE

      // max payload is 128 bytes for fixed length message - fits barely
      return write_to_dataserver(name, dtype, 10*sizeof(double), data);
    }
    return true;
  }
};

class UDPClient {
private:
    SOCKET sock;
    struct sockaddr_in server_addr;
    bool initialized;

public:
    UDPClient() : sock(INVALID_SOCKET), initialized(false) {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed" << std::endl;
            return;
        }
#endif
        initialized = true;
    }

    ~UDPClient() {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
        }
#ifdef _WIN32
        if (initialized) {
            WSACleanup();
        }
#endif
    }

    bool connect(const std::string& host, int port) {
        if (!initialized) {
            return false;
        }

        // Create socket
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }

        // Set socket timeout (5 seconds)
#ifdef _WIN32
        DWORD timeout = 5000; // milliseconds
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
#else
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    // Setup server address using getaddrinfo
        struct addrinfo hints, *result;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;      // IPv4
        hints.ai_socktype = SOCK_DGRAM; // UDP
        
        std::string port_str = std::to_string(port);
        int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
        if (status != 0) {
            std::cerr << "Failed to resolve UDP hostname: " << host << std::endl;
            return false;
        }
        
        memcpy(&server_addr, result->ai_addr, result->ai_addrlen);
        freeaddrinfo(result);
        
        return true;
    }

    std::string sendAndReceive(const std::string& message) {
        if (sock == INVALID_SOCKET) {
            return "";
        }

        // Send message
        int sent = sendto(sock, message.c_str(), static_cast<int>(message.length()), 0,
                         (struct sockaddr*)&server_addr, sizeof(server_addr));
        
        if (sent == SOCKET_ERROR) {
            std::cerr << "Failed to send message" << std::endl;
            return "";
        }

    //        std::cout << "Sent: " << message << std::endl;

        // Receive response
        char buffer[4096];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                               (struct sockaddr*)&from_addr, &from_len);
        
        if (received == SOCKET_ERROR) {
            std::cerr << "Failed to receive response (timeout or error)" << std::endl;
            return "";
        }

        buffer[received] = '\0';
        return std::string(buffer);
    }
};


int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 9003;

    std::string tcp_host = "localhost";
    int tcp_port = 4620;
    
    // Parse command line arguments
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = std::stoi(argv[2]);
    }
    if (argc >= 4) {
        tcp_host = argv[3];
    }
    if (argc >= 5) {
        tcp_port = std::stoi(argv[4]);
    }

    std::cout << "UDP Client starting..." << std::endl;
    std::cout << "Target: " << host << ":" << port << std::endl;

    UDPClient client;
    
    if (!client.connect(host, port)) {
        std::cerr << "Failed to initialize client" << std::endl;
        return 1;
    }

    TCPForwarder forwarder;
    if (!forwarder.connect(tcp_host, tcp_port)) {
      std::cerr << "Failed to connect to TCP server" << std::endl;
      return 1;
    }
    
    while (true) {
      // Send request and wait for JSON response
      std::string response = client.sendAndReceive("WAITFORDATA");
      
      if (!response.empty()) {
    if (forwarder.sendMessage(response, false)) {
      //      std::cout << "Successfully forwarded JSON to TCP server" << std::endl;
    } else {
      std::cerr << "Failed to forward JSON to TCP server" << std::endl;
      return 1;
    }
      } 
    }

    return 0;
}
