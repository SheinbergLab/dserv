/*
 * TclHttps.cpp - HTTPS client commands for Tcl using OpenSSL
 *
 * Provides:
 *   https_post $url $body ?-timeout ms?
 *   https_get $url ?-timeout ms?
 *
 * Uses OpenSSL which is already linked into dserv.
 * Add to TclServer.cpp's add_tcl_commands():
 *   TclHttps_RegisterCommands(interp);
 *
 * Build: Already links OpenSSL, just include this file in the build.
 */

#include <tcl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

#include <string>
#include <cstring>
#include <sstream>
#include <regex>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

// Parse URL into components
struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port;
    std::string path;
    bool valid;
};

static ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl result = {"", "", 0, "/", false};
    
    std::regex urlRegex(R"(^(https?)://([^/:]+)(?::(\d+))?(.*)$)");
    std::smatch match;
    
    if (!std::regex_match(url, match, urlRegex)) {
        return result;
    }
    
    result.scheme = match[1].str();
    result.host = match[2].str();
    result.port = match[3].length() > 0 ? std::stoi(match[3].str()) : 
                  (result.scheme == "https" ? 443 : 80);
    result.path = match[4].length() > 0 ? match[4].str() : "/";
    result.valid = true;
    
    return result;
}

// Simple HTTP response parser
struct HttpResponse {
    int statusCode;
    std::string body;
    bool success;
    std::string error;
};

static HttpResponse parseHttpResponse(const std::string& raw) {
    HttpResponse resp = {0, "", false, ""};
    
    // Find status line
    size_t statusEnd = raw.find("\r\n");
    if (statusEnd == std::string::npos) {
        resp.error = "Invalid HTTP response";
        return resp;
    }
    
    // Parse status code
    std::string statusLine = raw.substr(0, statusEnd);
    std::regex statusRegex(R"(^HTTP/[\d.]+ (\d+))");
    std::smatch match;
    if (!std::regex_search(statusLine, match, statusRegex)) {
        resp.error = "Cannot parse status line";
        return resp;
    }
    resp.statusCode = std::stoi(match[1].str());
    
    // Find body (after \r\n\r\n)
    size_t bodyStart = raw.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        resp.body = raw.substr(bodyStart + 4);
    }
    
    resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);
    return resp;
}

// Perform HTTPS request using OpenSSL
static HttpResponse doHttpsRequest(const std::string& method,
                                    const ParsedUrl& url,
                                    const std::string& body,
                                    int timeoutMs) {
    HttpResponse resp = {0, "", false, ""};
    
    // Initialize OpenSSL (safe to call multiple times)
    SSL_library_init();
    SSL_load_error_strings();
    
    // Create SSL context
    const SSL_METHOD* sslMethod = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(sslMethod);
    if (!ctx) {
        resp.error = "Failed to create SSL context";
        return resp;
    }
    
    // Use system CA certificates
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    
    // Create socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        resp.error = "Failed to create socket";
        SSL_CTX_free(ctx);
        return resp;
    }
    
    // Set non-blocking for timeout support
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    // Resolve hostname
    struct hostent* host = gethostbyname(url.host.c_str());
    if (!host) {
        resp.error = "Failed to resolve hostname: " + url.host;
        close(sockfd);
        SSL_CTX_free(ctx);
        return resp;
    }
    
    // Connect
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(url.port);
    memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
    
    int connectResult = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    if (connectResult < 0 && errno != EINPROGRESS) {
        resp.error = "Connection failed";
        close(sockfd);
        SSL_CTX_free(ctx);
        return resp;
    }
    
    // Wait for connection with timeout
    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLOUT;
    
    int pollResult = poll(&pfd, 1, timeoutMs);
    if (pollResult <= 0) {
        resp.error = pollResult == 0 ? "Connection timeout" : "Poll error";
        close(sockfd);
        SSL_CTX_free(ctx);
        return resp;
    }
    
    // Check for connection error
    int sockError;
    socklen_t sockErrorLen = sizeof(sockError);
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sockError, &sockErrorLen);
    if (sockError != 0) {
        resp.error = "Connection failed: " + std::string(strerror(sockError));
        close(sockfd);
        SSL_CTX_free(ctx);
        return resp;
    }
    
    // Set back to blocking for SSL
    fcntl(sockfd, F_SETFL, flags);
    
    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    // Create SSL connection
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sockfd);
    SSL_set_tlsext_host_name(ssl, url.host.c_str());  // SNI
    
    if (SSL_connect(ssl) <= 0) {
        unsigned long err = ERR_get_error();
        char errBuf[256];
        ERR_error_string_n(err, errBuf, sizeof(errBuf));
        resp.error = "SSL connection failed: " + std::string(errBuf);
        SSL_free(ssl);
        close(sockfd);
        SSL_CTX_free(ctx);
        return resp;
    }
    
    // Build HTTP request
    std::ostringstream reqStream;
    reqStream << method << " " << url.path << " HTTP/1.1\r\n";
    reqStream << "Host: " << url.host << "\r\n";
    reqStream << "Connection: close\r\n";
    reqStream << "User-Agent: dserv-tclhttps/1.0\r\n";
    
    if (method == "POST" && !body.empty()) {
        reqStream << "Content-Type: application/json\r\n";
        reqStream << "Content-Length: " << body.size() << "\r\n";
    }
    
    reqStream << "\r\n";
    
    if (method == "POST" && !body.empty()) {
        reqStream << body;
    }
    
    std::string request = reqStream.str();
    
    // Send request
    if (SSL_write(ssl, request.c_str(), request.size()) <= 0) {
        resp.error = "Failed to send request";
        SSL_free(ssl);
        close(sockfd);
        SSL_CTX_free(ctx);
        return resp;
    }
    
    // Read response
    std::string rawResponse;
    char buffer[4096];
    int bytesRead;
    
    while ((bytesRead = SSL_read(ssl, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        rawResponse += buffer;
    }
    
    // Cleanup
    SSL_free(ssl);
    close(sockfd);
    SSL_CTX_free(ctx);
    
    // Parse response
    resp = parseHttpResponse(rawResponse);
    return resp;
}

// Handle HTTP (non-SSL) requests
static HttpResponse doHttpRequest(const std::string& method,
                                   const ParsedUrl& url,
                                   const std::string& body,
                                   int timeoutMs) {
    HttpResponse resp = {0, "", false, ""};
    
    // Create socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        resp.error = "Failed to create socket";
        return resp;
    }
    
    // Set timeout
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    // Resolve and connect
    struct hostent* host = gethostbyname(url.host.c_str());
    if (!host) {
        resp.error = "Failed to resolve hostname";
        close(sockfd);
        return resp;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(url.port);
    memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        resp.error = "Connection failed";
        close(sockfd);
        return resp;
    }
    
    // Build and send HTTP request
    std::ostringstream reqStream;
    reqStream << method << " " << url.path << " HTTP/1.1\r\n";
    reqStream << "Host: " << url.host << "\r\n";
    reqStream << "Connection: close\r\n";
    
    if (method == "POST" && !body.empty()) {
        reqStream << "Content-Type: application/json\r\n";
        reqStream << "Content-Length: " << body.size() << "\r\n";
    }
    
    reqStream << "\r\n";
    
    if (method == "POST" && !body.empty()) {
        reqStream << body;
    }
    
    std::string request = reqStream.str();
    
    if (send(sockfd, request.c_str(), request.size(), 0) < 0) {
        resp.error = "Failed to send request";
        close(sockfd);
        return resp;
    }
    
    // Read response
    std::string rawResponse;
    char buffer[4096];
    ssize_t bytesRead;
    
    while ((bytesRead = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytesRead] = '\0';
        rawResponse += buffer;
    }
    
    close(sockfd);
    
    resp = parseHttpResponse(rawResponse);
    return resp;
}

// Unified request handler
static HttpResponse doRequest(const std::string& method,
                               const std::string& urlStr,
                               const std::string& body,
                               int timeoutMs) {
    ParsedUrl url = parseUrl(urlStr);
    if (!url.valid) {
        HttpResponse resp = {0, "", false, "Invalid URL: " + urlStr};
        return resp;
    }
    
    if (url.scheme == "https") {
        return doHttpsRequest(method, url, body, timeoutMs);
    } else {
        return doHttpRequest(method, url, body, timeoutMs);
    }
}

/*
 * https_post $url $body ?-timeout ms?
 */
static int HttpsPostCmd(ClientData clientData, Tcl_Interp *interp,
                        int objc, Tcl_Obj *const objv[]) {
    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "url body ?-timeout ms?");
        return TCL_ERROR;
    }
    
    const char* url = Tcl_GetString(objv[1]);
    const char* body = Tcl_GetString(objv[2]);
    int timeoutMs = 10000;  // Default 10 seconds
    
    // Parse options
    for (int i = 3; i < objc; i++) {
        const char* opt = Tcl_GetString(objv[i]);
        if (strcmp(opt, "-timeout") == 0 && i + 1 < objc) {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &timeoutMs) != TCL_OK) {
                return TCL_ERROR;
            }
            i++;
        }
    }
    
    HttpResponse resp = doRequest("POST", url, body, timeoutMs);
    
    if (!resp.error.empty()) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(resp.error.c_str(), -1));
        return TCL_ERROR;
    }
    
    if (!resp.success) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("HTTP %d: %s", 
            resp.statusCode, resp.body.c_str()));
        return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(resp.body.c_str(), resp.body.size()));
    return TCL_OK;
}

/*
 * https_get $url ?-timeout ms?
 */
static int HttpsGetCmd(ClientData clientData, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "url ?-timeout ms?");
        return TCL_ERROR;
    }
    
    const char* url = Tcl_GetString(objv[1]);
    int timeoutMs = 10000;
    
    for (int i = 2; i < objc; i++) {
        const char* opt = Tcl_GetString(objv[i]);
        if (strcmp(opt, "-timeout") == 0 && i + 1 < objc) {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &timeoutMs) != TCL_OK) {
                return TCL_ERROR;
            }
            i++;
        }
    }
    
    HttpResponse resp = doRequest("GET", url, "", timeoutMs);
    
    if (!resp.error.empty()) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(resp.error.c_str(), -1));
        return TCL_ERROR;
    }
    
    if (!resp.success) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("HTTP %d: %s", 
            resp.statusCode, resp.body.c_str()));
        return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(resp.body.c_str(), resp.body.size()));
    return TCL_OK;
}

/*
 * Register commands with interpreter
 * Call from add_tcl_commands() in TclServer.cpp:
 *   TclHttps_RegisterCommands(interp);
 */
extern "C" {
    int TclHttps_RegisterCommands(Tcl_Interp *interp) {
        Tcl_CreateObjCommand(interp, "https_post", HttpsPostCmd, NULL, NULL);
        Tcl_CreateObjCommand(interp, "https_get", HttpsGetCmd, NULL, NULL);
        return TCL_OK;
    }
}
