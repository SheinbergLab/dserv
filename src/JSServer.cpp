/**
 * JSServer.cpp - QuickJS-NG based subprocess for dserv
 * 
 * Implementation follows TclServer patterns closely for consistency.
 * 
 * QuickJS-NG: https://github.com/quickjs-ng/quickjs
 */

#include "JSServer.h"

#include <fstream>
#include <iostream>
#include <cstring>
#include <zlib.h>

/***************************************************************************
 * Constructor / Destructor
 ***************************************************************************/

JSServer::JSServer(Dataserver *dserv, const std::string& name)
    : ds(dserv), name(name)
{
    // Register as a dataserver client (same as TclServer)
    client_name = ds->add_new_send_client(&queue);
    
    // Start the processing thread
    process_thread = std::thread(&JSServer::process_requests, this);
}

JSServer::~JSServer()
{
    shutdown();
    if (process_thread.joinable()) {
        process_thread.join();
    }
}

void JSServer::shutdown()
{
    m_bDone = true;
    // Send shutdown message to unblock queue
    client_request_t req;
    req.type = REQ_SHUTDOWN;
    queue.push_back(req);
}

/***************************************************************************
 * Evaluation Interface
 ***************************************************************************/

std::string JSServer::eval(const std::string& script)
{
    SharedQueue<std::string> rqueue;
    client_request_t req;
    req.type = REQ_SCRIPT;
    req.rqueue = &rqueue;
    req.script = script;
    
    queue.push_back(req);
    
    // Block until result is ready
    std::string result = rqueue.front();
    rqueue.pop_front();
    
    return result;
}

void JSServer::eval_noreply(const std::string& script)
{
    client_request_t req;
    req.type = REQ_SCRIPT_NOREPLY;
    req.script = script;
    queue.push_back(req);
}

/***************************************************************************
 * QuickJS Setup
 ***************************************************************************/

// Helper to get JSServer* from JSContext
static JSServer* get_jsserver(JSContext *ctx) {
    return (JSServer*)JS_GetContextOpaque(ctx);
}

// dserv.set(name, value)
static JSValue js_dserv_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSServer *srv = get_jsserver(ctx);
    if (!srv || argc < 2) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    
    ds_datapoint_t dpoint;
    
    // Determine type and convert value
    if (JS_IsNumber(argv[1])) {
        double d;
        JS_ToFloat64(ctx, &d, argv[1]);
        // Check if it's an integer
        if (d == (int)d) {
            int i = (int)d;
            dpoint_set(&dpoint, (char*)name, srv->ds->now(), 
                      DSERV_INT, sizeof(int), (unsigned char*)&i);
        } else {
            float f = (float)d;
            dpoint_set(&dpoint, (char*)name, srv->ds->now(),
                      DSERV_FLOAT, sizeof(float), (unsigned char*)&f);
        }
    }
    else if (JS_IsString(argv[1])) {
        const char *str = JS_ToCString(ctx, argv[1]);
        dpoint_set(&dpoint, (char*)name, srv->ds->now(),
                  DSERV_STRING, strlen(str), (unsigned char*)str);
        srv->ds->set(dpoint);  // set() copies the data
        JS_FreeCString(ctx, str);
        JS_FreeCString(ctx, name);
        return JS_UNDEFINED;
    }
    else if (JS_IsArray(argv[1])) {
        // Handle arrays - convert to float array (DSERV_FLOAT with len > sizeof(float))
        JSValue length_val = JS_GetPropertyStr(ctx, argv[1], "length");
        uint32_t len;
        JS_ToUint32(ctx, &len, length_val);
        JS_FreeValue(ctx, length_val);
        
        float *arr = (float*)malloc(len * sizeof(float));
        for (uint32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, argv[1], i);
            double d;
            JS_ToFloat64(ctx, &d, elem);
            arr[i] = (float)d;
            JS_FreeValue(ctx, elem);
        }
        
        dpoint_set(&dpoint, (char*)name, srv->ds->now(),
                  DSERV_FLOAT, len * sizeof(float), (unsigned char*)arr);
        srv->ds->set(dpoint);  // set() copies the data
        free(arr);
        JS_FreeCString(ctx, name);
        return JS_UNDEFINED;
    }
    else if (JS_IsObject(argv[1])) {
        // Convert object to JSON string
        JSValue json = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
        const char *str = JS_ToCString(ctx, json);
        dpoint_set(&dpoint, (char*)name, srv->ds->now(),
                  DSERV_JSON, strlen(str), (unsigned char*)str);
        srv->ds->set(dpoint);  // set() copies the data
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, json);
        JS_FreeCString(ctx, name);
        return JS_UNDEFINED;
    }
    
    srv->ds->set(dpoint);
    
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

// dserv.get(name) -> value
static JSValue js_dserv_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSServer *srv = get_jsserver(ctx);
    if (!srv || argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    
    ds_datapoint_t *dpoint = nullptr;
    int found = srv->ds->get((char*)name, &dpoint);
    JS_FreeCString(ctx, name);
    
    if (!found || !dpoint) return JS_NULL;
    
    JSValue result;
    
    switch (dpoint->data.type) {
        case DSERV_INT: {
            int n = dpoint->data.len / sizeof(int);
            if (n == 1) {
                result = JS_NewInt32(ctx, *(int*)dpoint->data.buf);
            } else {
                int *arr = (int*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewInt32(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_FLOAT: {
            int n = dpoint->data.len / sizeof(float);
            if (n == 1) {
                result = JS_NewFloat64(ctx, *(float*)dpoint->data.buf);
            } else {
                float *arr = (float*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewFloat64(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_DOUBLE: {
            int n = dpoint->data.len / sizeof(double);
            if (n == 1) {
                result = JS_NewFloat64(ctx, *(double*)dpoint->data.buf);
            } else {
                double *arr = (double*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewFloat64(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_SHORT: {
            int n = dpoint->data.len / sizeof(short);
            if (n == 1) {
                result = JS_NewInt32(ctx, *(short*)dpoint->data.buf);
            } else {
                short *arr = (short*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewInt32(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_BYTE: {
            int n = dpoint->data.len;
            if (n == 1) {
                result = JS_NewInt32(ctx, *(uint8_t*)dpoint->data.buf);
            } else {
                uint8_t *arr = (uint8_t*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewInt32(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_STRING:
        case DSERV_SCRIPT:
        case DSERV_JSON:
            result = JS_NewStringLen(ctx, (char*)dpoint->data.buf, dpoint->data.len);
            break;
        default:
            result = JS_NULL;
    }
    
    // get() returns a copy that must be freed
    dpoint_free(dpoint);
    return result;
}

// dserv.subscribe(pattern, every) - register for dpoint updates
static JSValue js_dserv_subscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSServer *srv = get_jsserver(ctx);
    if (!srv || argc < 1) return JS_UNDEFINED;
    
    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!pattern) return JS_EXCEPTION;
    
    int every = 1;
    if (argc > 1) {
        JS_ToInt32(ctx, &every, argv[1]);
    }
    
    srv->ds->client_add_match(srv->client_name, (char*)pattern, every);
    
    JS_FreeCString(ctx, pattern);
    return JS_UNDEFINED;
}

// dserv.unsubscribe(pattern)
static JSValue js_dserv_unsubscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSServer *srv = get_jsserver(ctx);
    if (!srv || argc < 1) return JS_UNDEFINED;
    
    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!pattern) return JS_EXCEPTION;
    
    srv->ds->client_remove_match(srv->client_name, (char*)pattern);
    
    JS_FreeCString(ctx, pattern);
    return JS_UNDEFINED;
}

// dserv.onDpoint(pattern, callback) - register a callback for dpoint updates
// pattern can be exact name or glob pattern (e.g., "eye/*")
// callback receives (name, value, timestamp)
static JSValue js_dserv_onDpoint(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSServer *srv = get_jsserver(ctx);
    if (!srv || argc < 2) return JS_UNDEFINED;
    
    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!pattern) return JS_EXCEPTION;
    
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, pattern);
        return JS_ThrowTypeError(ctx, "onDpoint: second argument must be a function");
    }
    
    // Check if pattern contains glob characters
    std::string pat_str(pattern);
    bool is_glob = (pat_str.find('*') != std::string::npos || 
                    pat_str.find('?') != std::string::npos);
    
    // Store the callback (dup the function to prevent GC)
    JSCallbackInfo info;
    info.func = JS_DupValue(ctx, argv[1]);
    info.pattern = pat_str;
    info.is_glob = is_glob;
    srv->dpoint_callbacks.push_back(info);
    
    // Subscribe to the pattern
    srv->ds->client_add_match(srv->client_name, (char*)pattern, 1);
    
    JS_FreeCString(ctx, pattern);
    return JS_UNDEFINED;
}

// dserv.offDpoint(pattern) - remove callbacks for a pattern
static JSValue js_dserv_offDpoint(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSServer *srv = get_jsserver(ctx);
    if (!srv || argc < 1) return JS_UNDEFINED;
    
    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!pattern) return JS_EXCEPTION;
    
    std::string pat_str(pattern);
    
    // Remove matching callbacks
    auto it = srv->dpoint_callbacks.begin();
    while (it != srv->dpoint_callbacks.end()) {
        if (it->pattern == pat_str) {
            JS_FreeValue(ctx, it->func);
            it = srv->dpoint_callbacks.erase(it);
        } else {
            ++it;
        }
    }
    
    // Unsubscribe
    srv->ds->client_remove_match(srv->client_name, (char*)pattern);
    
    JS_FreeCString(ctx, pattern);
    return JS_UNDEFINED;
}

// dserv.now() -> timestamp in microseconds
static JSValue js_dserv_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSServer *srv = get_jsserver(ctx);
    if (!srv) return JS_UNDEFINED;
    
    uint64_t timestamp = srv->ds->now();
    return JS_NewFloat64(ctx, (double)timestamp);
}

// console.log(...args) -> writes to subprocess/stdout dpoint
static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSServer *srv = get_jsserver(ctx);
    
    std::string output;
    for (int i = 0; i < argc; i++) {
        if (i > 0) output += " ";
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            output += str;
            JS_FreeCString(ctx, str);
        }
    }
    output += "\n";
    
    if (srv) {
        // Route to dpoint like TclServer does with puts
        std::string dpname = srv->name + "/stdout";
        ds_datapoint_t dpoint;
        dpoint_set(&dpoint, (char*)dpname.c_str(), srv->ds->now(),
                  DSERV_STRING, output.size(), (unsigned char*)output.c_str());
        srv->ds->set(dpoint);
    } else {
        // Fallback to stderr
        std::cerr << output;
    }
    
    return JS_UNDEFINED;
}

// dserv.readFile(path) -> Uint8Array
// Reads a file and returns its contents as a Uint8Array
static JSValue js_dserv_readFile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "readFile requires a path argument");
    }
    
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    
    // Open file in binary mode
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "Cannot open file: %s", path);
    }
    
    // Get file size
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Create Uint8Array
    JSValue array_buffer = JS_NewArrayBufferCopy(ctx, nullptr, size);
    if (JS_IsException(array_buffer)) {
        JS_FreeCString(ctx, path);
        return array_buffer;
    }
    
    // Get the buffer pointer and read directly into it
    size_t buf_size;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_size, array_buffer);
    if (!buf) {
        JS_FreeValue(ctx, array_buffer);
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "Failed to allocate buffer");
    }
    
    if (!file.read(reinterpret_cast<char*>(buf), size)) {
        JS_FreeValue(ctx, array_buffer);
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "Failed to read file");
    }
    
    JS_FreeCString(ctx, path);
    
    // Create Uint8Array view of the ArrayBuffer
    JSValue uint8_ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "Uint8Array");
    JSValue args[] = { array_buffer };
    JSValue result = JS_CallConstructor(ctx, uint8_ctor, 1, args);
    JS_FreeValue(ctx, uint8_ctor);
    JS_FreeValue(ctx, array_buffer);
    
    return result;
}

// dserv.gunzip(data) -> Uint8Array
// Decompresses gzip data using zlib
static JSValue js_dserv_gunzip(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "gunzip requires data argument");
    }
    
    // Get input data - could be Uint8Array, ArrayBuffer, or array-like
    size_t input_len;
    uint8_t *input_data = nullptr;
    JSValue array_buffer = JS_UNDEFINED;
    bool free_input = false;
    
    // Check if it's a TypedArray or ArrayBuffer
    size_t offset, elem_size;
    JSValue underlying_buffer = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &input_len, &elem_size);
    
    if (!JS_IsException(underlying_buffer)) {
        // It's a TypedArray - get the underlying buffer
        size_t buf_len;
        uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_len, underlying_buffer);
        input_data = buf + offset;
        JS_FreeValue(ctx, underlying_buffer);
    } else {
        // Clear the exception and try ArrayBuffer directly
        JS_FreeValue(ctx, JS_GetException(ctx));
        input_data = JS_GetArrayBuffer(ctx, &input_len, argv[0]);
        
        if (!input_data) {
            return JS_ThrowTypeError(ctx, "gunzip requires Uint8Array or ArrayBuffer");
        }
    }
    
    // Estimate decompressed size (start with 4x, can grow)
    size_t output_capacity = input_len * 4;
    if (output_capacity < 1024) output_capacity = 1024;
    
    std::vector<uint8_t> output;
    output.resize(output_capacity);
    
    // Initialize zlib for gzip decompression
    z_stream strm = {};
    strm.next_in = input_data;
    strm.avail_in = input_len;
    
    // 15 + 16 = gzip format, 15 + 32 = auto-detect gzip or zlib
    if (inflateInit2(&strm, 15 + 32) != Z_OK) {
        return JS_ThrowTypeError(ctx, "Failed to initialize zlib");
    }
    
    size_t total_out = 0;
    int ret;
    
    do {
        // Grow buffer if needed
        if (total_out >= output.size()) {
            output.resize(output.size() * 2);
        }
        
        strm.next_out = output.data() + total_out;
        strm.avail_out = output.size() - total_out;
        
        ret = inflate(&strm, Z_NO_FLUSH);
        
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return JS_ThrowTypeError(ctx, "Decompression failed: %s", strm.msg ? strm.msg : "unknown error");
        }
        
        total_out = strm.total_out;
        
    } while (ret != Z_STREAM_END);
    
    inflateEnd(&strm);
    
    // Create result Uint8Array
    JSValue result_buffer = JS_NewArrayBufferCopy(ctx, output.data(), total_out);
    if (JS_IsException(result_buffer)) {
        return result_buffer;
    }
    
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue uint8_ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JSValue args[] = { result_buffer };
    JSValue result = JS_CallConstructor(ctx, uint8_ctor, 1, args);
    JS_FreeValue(ctx, uint8_ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, result_buffer);
    
    return result;
}

// dserv.readDGZ(path) -> Uint8Array (decompressed)
// Convenience function: reads and decompresses a .dgz file in one step
static JSValue js_dserv_readDGZ(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "readDGZ requires a path argument");
    }
    
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    
    // Open file
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::string err = std::string("Cannot open file: ") + path;
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "%s", err.c_str());
    }
    
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read compressed data
    std::vector<uint8_t> compressed(size);
    if (!file.read(reinterpret_cast<char*>(compressed.data()), size)) {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "Failed to read file");
    }
    JS_FreeCString(ctx, path);
    
    // Decompress - estimate 10x expansion for typical DG files
    size_t output_capacity = size * 10;
    if (output_capacity < 4096) output_capacity = 4096;
    
    std::vector<uint8_t> output;
    output.resize(output_capacity);
    
    z_stream strm = {};
    strm.next_in = compressed.data();
    strm.avail_in = size;
    
    if (inflateInit2(&strm, 15 + 32) != Z_OK) {
        return JS_ThrowTypeError(ctx, "Failed to initialize zlib");
    }
    
    size_t total_out = 0;
    int ret;
    
    do {
        if (total_out >= output.size()) {
            output.resize(output.size() * 2);
        }
        
        strm.next_out = output.data() + total_out;
        strm.avail_out = output.size() - total_out;
        
        ret = inflate(&strm, Z_NO_FLUSH);
        
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return JS_ThrowTypeError(ctx, "Decompression failed: %s", strm.msg ? strm.msg : "unknown error");
        }
        
        total_out = strm.total_out;
        
    } while (ret != Z_STREAM_END);
    
    inflateEnd(&strm);
    
    // Create result Uint8Array
    JSValue result_buffer = JS_NewArrayBufferCopy(ctx, output.data(), total_out);
    if (JS_IsException(result_buffer)) {
        return result_buffer;
    }
    
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue uint8_ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JSValue args[] = { result_buffer };
    JSValue result = JS_CallConstructor(ctx, uint8_ctor, 1, args);
    JS_FreeValue(ctx, uint8_ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, result_buffer);
    
    return result;
}



/**
 * JavaScript polyfills and standard globals for QuickJS-NG
 */

// Polyfill JavaScript code to evaluate at startup
static const char *js_polyfills = R"JS(

// TextEncoder / TextDecoder (Web API for string <-> bytes)
globalThis.TextDecoder = class TextDecoder {
    constructor(encoding = 'utf-8') {
        this.encoding = encoding.toLowerCase();
    }
    decode(bytes) {
        if (!bytes) return '';
        // Handle TypedArray or ArrayBuffer
        if (bytes.buffer) bytes = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        else if (bytes instanceof ArrayBuffer) bytes = new Uint8Array(bytes);
        
        // UTF-8 decoding
        let str = '';
        let i = 0;
        while (i < bytes.length) {
            let c = bytes[i++];
            if (c < 128) {
                str += String.fromCharCode(c);
            } else if (c < 224) {
                str += String.fromCharCode(((c & 31) << 6) | (bytes[i++] & 63));
            } else if (c < 240) {
                str += String.fromCharCode(((c & 15) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63));
            } else {
                // 4-byte UTF-8 -> surrogate pair
                let cp = ((c & 7) << 18) | ((bytes[i++] & 63) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
                cp -= 0x10000;
                str += String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
            }
        }
        return str;
    }
};

globalThis.TextEncoder = class TextEncoder {
    constructor() {
        this.encoding = 'utf-8';
    }
    encode(str) {
        const bytes = [];
        for (let i = 0; i < str.length; i++) {
            let c = str.charCodeAt(i);
            // Handle surrogate pairs
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length) {
                const c2 = str.charCodeAt(i + 1);
                if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                    c = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
                    i++;
                }
            }
            if (c < 128) {
                bytes.push(c);
            } else if (c < 2048) {
                bytes.push(192 | (c >> 6), 128 | (c & 63));
            } else if (c < 65536) {
                bytes.push(224 | (c >> 12), 128 | ((c >> 6) & 63), 128 | (c & 63));
            } else {
                bytes.push(240 | (c >> 18), 128 | ((c >> 12) & 63), 128 | ((c >> 6) & 63), 128 | (c & 63));
            }
        }
        return new Uint8Array(bytes);
    }
};

// atob / btoa (Base64 encoding - useful for binary data handling)
globalThis.btoa = function(str) {
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    let result = '';
    let i = 0;
    while (i < str.length) {
        const a = str.charCodeAt(i++);
        const b = i < str.length ? str.charCodeAt(i++) : 0;
        const c = i < str.length ? str.charCodeAt(i++) : 0;
        const triplet = (a << 16) | (b << 8) | c;
        result += chars[(triplet >> 18) & 63];
        result += chars[(triplet >> 12) & 63];
        result += i > str.length + 1 ? '=' : chars[(triplet >> 6) & 63];
        result += i > str.length ? '=' : chars[triplet & 63];
    }
    return result;
};

globalThis.atob = function(str) {
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    str = str.replace(/=+$/, '');
    let result = '';
    let i = 0;
    while (i < str.length) {
        const a = chars.indexOf(str[i++]);
        const b = chars.indexOf(str[i++]);
        const c = chars.indexOf(str[i++]);
        const d = chars.indexOf(str[i++]);
        const triplet = (a << 18) | (b << 12) | (c << 6) | d;
        result += String.fromCharCode((triplet >> 16) & 255);
        if (c !== -1) result += String.fromCharCode((triplet >> 8) & 255);
        if (d !== -1) result += String.fromCharCode(triplet & 255);
    }
    return result;
};

// structuredClone (deep copy - useful for data manipulation)
globalThis.structuredClone = function(obj) {
    return JSON.parse(JSON.stringify(obj));
};

// queueMicrotask (for async scheduling)
if (typeof queueMicrotask === 'undefined') {
    globalThis.queueMicrotask = function(fn) {
        Promise.resolve().then(fn);
    };
}

// Performance timing (useful for benchmarking)
if (typeof performance === 'undefined') {
    const startTime = Date.now();
    globalThis.performance = {
        now: function() {
            return Date.now() - startTime;
        },
        timeOrigin: startTime
    };
}

// URL parsing (basic implementation)
if (typeof URL === 'undefined') {
    globalThis.URL = class URL {
        constructor(url, base) {
            if (base) {
                // Simple base resolution
                if (!url.match(/^[a-z]+:/i)) {
                    url = base.replace(/\/[^/]*$/, '/') + url;
                }
            }
            const match = url.match(/^([a-z]+):\/\/([^/:]+)(?::(\d+))?(\/[^?#]*)?(\?[^#]*)?(#.*)?$/i);
            if (match) {
                this.protocol = match[1] + ':';
                this.hostname = match[2];
                this.port = match[3] || '';
                this.pathname = match[4] || '/';
                this.search = match[5] || '';
                this.hash = match[6] || '';
                this.host = this.hostname + (this.port ? ':' + this.port : '');
                this.origin = this.protocol + '//' + this.host;
                this.href = url;
            } else {
                throw new TypeError('Invalid URL: ' + url);
            }
        }
        toString() { return this.href; }
    };
}

// Object.hasOwn (ES2022)
if (!Object.hasOwn) {
    Object.hasOwn = function(obj, prop) {
        return Object.prototype.hasOwnProperty.call(obj, prop);
    };
}

// Array.prototype.at (ES2022)
if (!Array.prototype.at) {
    Array.prototype.at = function(index) {
        index = Math.trunc(index) || 0;
        if (index < 0) index += this.length;
        if (index < 0 || index >= this.length) return undefined;
        return this[index];
    };
}

// String.prototype.at (ES2022)
if (!String.prototype.at) {
    String.prototype.at = function(index) {
        index = Math.trunc(index) || 0;
        if (index < 0) index += this.length;
        if (index < 0 || index >= this.length) return undefined;
        return this[index];
    };
}

// Array.prototype.findLast / findLastIndex (ES2023)
if (!Array.prototype.findLast) {
    Array.prototype.findLast = function(fn, thisArg) {
        for (let i = this.length - 1; i >= 0; i--) {
            if (fn.call(thisArg, this[i], i, this)) return this[i];
        }
        return undefined;
    };
}

if (!Array.prototype.findLastIndex) {
    Array.prototype.findLastIndex = function(fn, thisArg) {
        for (let i = this.length - 1; i >= 0; i--) {
            if (fn.call(thisArg, this[i], i, this)) return i;
        }
        return -1;
    };
}

// Array.prototype.toSorted / toReversed / toSpliced (ES2023 - non-mutating)
if (!Array.prototype.toSorted) {
    Array.prototype.toSorted = function(compareFn) {
        return [...this].sort(compareFn);
    };
}

if (!Array.prototype.toReversed) {
    Array.prototype.toReversed = function() {
        return [...this].reverse();
    };
}

if (!Array.prototype.toSpliced) {
    Array.prototype.toSpliced = function(start, deleteCount, ...items) {
        const copy = [...this];
        copy.splice(start, deleteCount, ...items);
        return copy;
    };
}

// Object.groupBy (ES2024)
if (!Object.groupBy) {
    Object.groupBy = function(items, callback) {
        const result = {};
        let i = 0;
        for (const item of items) {
            const key = callback(item, i++);
            if (!result[key]) result[key] = [];
            result[key].push(item);
        }
        return result;
    };
}

// Map.groupBy (ES2024)
if (!Map.groupBy) {
    Map.groupBy = function(items, callback) {
        const result = new Map();
        let i = 0;
        for (const item of items) {
            const key = callback(item, i++);
            if (!result.has(key)) result.set(key, []);
            result.get(key).push(item);
        }
        return result;
    };
}

)JS";


/**
 * Initialize JavaScript polyfills
 * Call this from setup_js() after register_js_functions()
 */
static void init_js_polyfills(JSContext *ctx)
{
    JSValue result = JS_Eval(ctx, js_polyfills, strlen(js_polyfills),
                             "<polyfills>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        std::cerr << "Failed to initialize JS polyfills: " << (str ? str : "unknown") << std::endl;
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
}


// Register all dserv.* and console.* functions
void JSServer::register_js_functions(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    
    // Create dserv object
    JSValue dserv_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, dserv_obj, "set", 
        JS_NewCFunction(ctx, js_dserv_set, "set", 2));
    JS_SetPropertyStr(ctx, dserv_obj, "get",
        JS_NewCFunction(ctx, js_dserv_get, "get", 1));
    JS_SetPropertyStr(ctx, dserv_obj, "subscribe",
        JS_NewCFunction(ctx, js_dserv_subscribe, "subscribe", 2));
    JS_SetPropertyStr(ctx, dserv_obj, "unsubscribe",
        JS_NewCFunction(ctx, js_dserv_unsubscribe, "unsubscribe", 1));
    JS_SetPropertyStr(ctx, dserv_obj, "onDpoint",
        JS_NewCFunction(ctx, js_dserv_onDpoint, "onDpoint", 2));
    JS_SetPropertyStr(ctx, dserv_obj, "offDpoint",
        JS_NewCFunction(ctx, js_dserv_offDpoint, "offDpoint", 1));
    JS_SetPropertyStr(ctx, dserv_obj, "now",
        JS_NewCFunction(ctx, js_dserv_now, "now", 0));
    JS_SetPropertyStr(ctx, global, "dserv", dserv_obj);

    JS_SetPropertyStr(ctx, dserv_obj, "readFile",
		      JS_NewCFunction(ctx, js_dserv_readFile, "readFile", 1));
    JS_SetPropertyStr(ctx, dserv_obj, "gunzip",
		      JS_NewCFunction(ctx, js_dserv_gunzip, "gunzip", 1));
    JS_SetPropertyStr(ctx, dserv_obj, "readDGZ",
		      JS_NewCFunction(ctx, js_dserv_readDGZ, "readDGZ", 1));    

    // Create console object
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, js_console_log, "error", 1));  // Same for now
    JS_SetPropertyStr(ctx, global, "console", console);
    
    JS_FreeValue(ctx, global);
}

JSContext* JSServer::setup_js()
{
    // Create runtime and context
    rt = JS_NewRuntime();
    if (!rt) {
        std::cerr << "Failed to create QuickJS runtime" << std::endl;
        return nullptr;
    }
    
    ctx = JS_NewContext(rt);
    if (!ctx) {
        std::cerr << "Failed to create QuickJS context" << std::endl;
        JS_FreeRuntime(rt);
        rt = nullptr;
        return nullptr;
    }
    
    // Store JSServer pointer for callbacks
    JS_SetContextOpaque(ctx, this);
    
    // Register our functions
    register_js_functions(ctx);

    init_js_polyfills(ctx);
    
    return ctx;
}

/***************************************************************************
 * Main Processing Loop (mirrors TclServer::process_requests)
 ***************************************************************************/

// Convert dpoint data to JSValue
JSValue JSServer::dpoint_to_jsvalue(ds_datapoint_t *dpoint)
{
    JSValue result;
    
    switch (dpoint->data.type) {
        case DSERV_INT: {
            int n = dpoint->data.len / sizeof(int);
            if (n == 1) {
                result = JS_NewInt32(ctx, *(int*)dpoint->data.buf);
            } else {
                int *arr = (int*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewInt32(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_FLOAT: {
            int n = dpoint->data.len / sizeof(float);
            if (n == 1) {
                result = JS_NewFloat64(ctx, *(float*)dpoint->data.buf);
            } else {
                float *arr = (float*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewFloat64(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_DOUBLE: {
            int n = dpoint->data.len / sizeof(double);
            if (n == 1) {
                result = JS_NewFloat64(ctx, *(double*)dpoint->data.buf);
            } else {
                double *arr = (double*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewFloat64(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_SHORT: {
            int n = dpoint->data.len / sizeof(short);
            if (n == 1) {
                result = JS_NewInt32(ctx, *(short*)dpoint->data.buf);
            } else {
                short *arr = (short*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewInt32(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_BYTE: {
            int n = dpoint->data.len;
            if (n == 1) {
                result = JS_NewInt32(ctx, *(uint8_t*)dpoint->data.buf);
            } else {
                uint8_t *arr = (uint8_t*)dpoint->data.buf;
                result = JS_NewArray(ctx);
                for (int i = 0; i < n; i++) {
                    JS_SetPropertyUint32(ctx, result, i, JS_NewInt32(ctx, arr[i]));
                }
            }
            break;
        }
        case DSERV_STRING:
        case DSERV_SCRIPT:
            result = JS_NewStringLen(ctx, (char*)dpoint->data.buf, dpoint->data.len);
            break;
        case DSERV_JSON:
            // Parse JSON string into JS object
            result = JS_ParseJSON(ctx, (char*)dpoint->data.buf, dpoint->data.len, "<dpoint>");
            if (JS_IsException(result)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                result = JS_NewStringLen(ctx, (char*)dpoint->data.buf, dpoint->data.len);
            }
            break;
        default:
            result = JS_NULL;
    }
    
    return result;
}

// Simple glob pattern matching
bool JSServer::pattern_matches(const std::string& pattern, const std::string& name, bool is_glob)
{
    if (!is_glob) {
        return pattern == name;
    }
    
    // Simple glob matching - supports * and ?
    size_t pi = 0, ni = 0;
    size_t plen = pattern.length(), nlen = name.length();
    size_t star_pi = std::string::npos, star_ni = 0;
    
    while (ni < nlen) {
        if (pi < plen && (pattern[pi] == '?' || pattern[pi] == name[ni])) {
            pi++;
            ni++;
        } else if (pi < plen && pattern[pi] == '*') {
            star_pi = pi++;
            star_ni = ni;
        } else if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ni = ++star_ni;
        } else {
            return false;
        }
    }
    
    while (pi < plen && pattern[pi] == '*') pi++;
    return pi == plen;
}

// Dispatch callbacks for a dpoint
void JSServer::dispatch_dpoint_callbacks(ds_datapoint_t *dpoint)
{
    std::string varname(dpoint->varname, dpoint->varlen);
    
    for (auto& cb : dpoint_callbacks) {
        if (pattern_matches(cb.pattern, varname, cb.is_glob)) {
            // Build arguments: (name, value, timestamp)
            JSValue args[3];
            args[0] = JS_NewStringLen(ctx, dpoint->varname, dpoint->varlen);
            args[1] = dpoint_to_jsvalue(dpoint);
            args[2] = JS_NewFloat64(ctx, (double)dpoint->timestamp);
            
            // Call the callback
            JSValue result = JS_Call(ctx, cb.func, JS_UNDEFINED, 3, args);
            
            // Check for exceptions
            if (JS_IsException(result)) {
                JSValue exc = JS_GetException(ctx);
                const char *str = JS_ToCString(ctx, exc);
                std::cerr << "JS callback error for " << varname << ": " 
                          << (str ? str : "unknown") << std::endl;
                JS_FreeCString(ctx, str);
                JS_FreeValue(ctx, exc);
            }
            
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
            JS_FreeValue(ctx, args[2]);
        }
    }
}

void JSServer::process_requests()
{
    // Initialize JS interpreter
    JSContext *local_ctx = setup_js();
    if (!local_ctx) {
        std::cerr << "JSServer: failed to initialize interpreter" << std::endl;
        return;
    }
    
    client_request_t req;
    
    while (!m_bDone) {
        req = queue.front();
        queue.pop_front();
        
        switch (req.type) {
            case REQ_SHUTDOWN:
                m_bDone = true;
                break;
                
            case REQ_SCRIPT: {
                JSValue result = JS_Eval(ctx, req.script.c_str(), req.script.size(),
                                        "<eval>", JS_EVAL_TYPE_GLOBAL);
                
                std::string result_str;
                if (JS_IsException(result)) {
                    JSValue exc = JS_GetException(ctx);
                    const char *str = JS_ToCString(ctx, exc);
                    result_str = std::string("!JS_ERROR ") + (str ? str : "unknown error");
                    JS_FreeCString(ctx, str);
                    JS_FreeValue(ctx, exc);
                } else {
                    const char *str = JS_ToCString(ctx, result);
                    result_str = str ? str : "";
                    JS_FreeCString(ctx, str);
                }
                JS_FreeValue(ctx, result);
                
                if (req.rqueue) {
                    req.rqueue->push_back(result_str);
                }
                break;
            }
            
            case REQ_SCRIPT_NOREPLY: {
                JSValue result = JS_Eval(ctx, req.script.c_str(), req.script.size(),
                                        "<eval>", JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(result)) {
                    // Log error to dpoint
                    JSValue exc = JS_GetException(ctx);
                    const char *str = JS_ToCString(ctx, exc);
                    std::cerr << "JS Error in " << name << ": " << (str ? str : "") << std::endl;
                    JS_FreeCString(ctx, str);
                    JS_FreeValue(ctx, exc);
                }
                JS_FreeValue(ctx, result);
                break;
            }
            
            case REQ_DPOINT:
            case REQ_DPOINT_SCRIPT: {
                ds_datapoint_t *dpoint = req.dpoint;
                dispatch_dpoint_callbacks(dpoint);
                dpoint_free(dpoint);
                break;
            }
            
            default:
                break;
        }
        
        // Run pending jobs (promises, etc.)
        JSContext *ctx2;
        while (JS_ExecutePendingJob(rt, &ctx2) > 0) {
            // Process pending async work
        }
    }
    
    // Cleanup
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    ctx = nullptr;
    rt = nullptr;
}

/***************************************************************************
 * Cleanup
 ***************************************************************************/

// Called from destructor to free JS callback functions
void JSServer::cleanup_callbacks()
{
    // Note: ctx may already be null if process_requests finished
    // The callbacks are freed when the context is freed, so we just clear our list
    dpoint_callbacks.clear();
}
