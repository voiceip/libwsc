/*
 *  WebSocketClientWrapper.cpp
 *  Wrapper for libwebsockets to replace libwsc WebSocketClient
 *  Author: TechGuyVN
 *  Copyright (c) 2025
 */

#include "WebSocketClientWrapper.h"
#include "WebSocketHeaders.h"
#include "WebSocketTLSOptions.h"
#include "Logger.h"
#include <cstring>
#include <sstream>
#include <algorithm>
#include <map>

// Map wsi to instance for callbacks
static std::map<struct lws*, WebSocketClientWrapper*> g_wsi_to_instance;
static std::mutex g_wsi_map_mutex;

WebSocketClientWrapper::WebSocketClientWrapper()
    : m_context(nullptr)
    , m_wsi(nullptr)
    , m_port(80)
    , m_secure(false)
    , m_state(ConnectionState::DISCONNECTED)
    , m_connected(false)
    , m_upgraded(false)
    , m_event_thread(nullptr)
    , m_use_compression(false)
    , m_ping_interval(0)
    , m_connection_timeout(30)
{
    log_info("WebSocketClientWrapper: created");
}

WebSocketClientWrapper::~WebSocketClientWrapper() {
    log_info("WebSocketClientWrapper: destroying");
    disconnect();
    destroyContext();
}

void WebSocketClientWrapper::setUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_url = url;
    parseUrl(url);
    log_info("WebSocketClientWrapper: setUrl %s", url.c_str());
}

void WebSocketClientWrapper::parseUrl(const std::string& url) {
    // Parse ws://host:port/path or wss://host:port/path
    m_secure = (url.find("wss://") == 0 || url.find("WSS://") == 0);
    
    std::string protocol = m_secure ? "wss://" : "ws://";
    size_t proto_len = protocol.length();
    
    if (url.find(protocol) != 0) {
        log_error("WebSocketClientWrapper: Invalid URL protocol: %s", url.c_str());
        return;
    }
    
    size_t start = proto_len;
    size_t slash_pos = url.find('/', start);
    size_t colon_pos = url.find(':', start);
    
    if (slash_pos == std::string::npos) {
        slash_pos = url.length();
    }
    
    if (colon_pos != std::string::npos && colon_pos < slash_pos) {
        // Has port
        m_host = url.substr(start, colon_pos - start);
        size_t port_start = colon_pos + 1;
        std::string port_str = url.substr(port_start, slash_pos - port_start);
        m_port = std::stoi(port_str);
    } else {
        // No port, use default
        m_host = url.substr(start, slash_pos - start);
        m_port = m_secure ? 443 : 80;
    }
    
    if (slash_pos < url.length()) {
        m_path = url.substr(slash_pos);
    } else {
        m_path = "/";
    }
    
    log_info("WebSocketClientWrapper: parsed URL - host: %s, port: %d, path: %s, secure: %d",
             m_host.c_str(), m_port, m_path.c_str(), m_secure ? 1 : 0);
}

// Static protocol definition for libwebsockets (must be defined before use)
static struct lws_protocols protocols[] = {
    {
        "ws",
        WebSocketClientWrapper::callback_client,
        0,
        4096,
    },
    { nullptr, nullptr, 0, 0 }
};

void WebSocketClientWrapper::createContext() {
    if (m_context) {
        return;
    }
    
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    
    // TLS options
    if (m_secure) {
        if (!m_tls_options.caFile.empty() && m_tls_options.caFile != "SYSTEM" && m_tls_options.caFile != "NONE") {
            info.client_ssl_ca_filepath = m_tls_options.caFile.c_str();
        }
        if (m_tls_options.hasCertAndKey()) {
            info.client_ssl_cert_filepath = m_tls_options.certFile.c_str();
            info.client_ssl_private_key_filepath = m_tls_options.keyFile.c_str();
        }
        if (m_tls_options.disableHostnameValidation || m_tls_options.isPeerVerifyDisabled()) {
            info.options |= LWS_SERVER_OPTION_SKIP_SERVER_CANONICAL_NAME;
        }
    }
    
    m_context = lws_create_context(&info);
    if (!m_context) {
        log_error("WebSocketClientWrapper: Failed to create lws context");
        m_state = ConnectionState::FAILED;
    } else {
        log_info("WebSocketClientWrapper: Created lws context");
    }
}

void WebSocketClientWrapper::destroyContext() {
    if (m_context) {
        lws_context_destroy(m_context);
        m_context = nullptr;
        log_info("WebSocketClientWrapper: Destroyed lws context");
    }
}

void WebSocketClientWrapper::connect() {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    
    if (m_state == ConnectionState::CONNECTED || m_state == ConnectionState::CONNECTING) {
        log_info("WebSocketClientWrapper: Already connected or connecting");
        return;
    }
    
    m_state = ConnectionState::CONNECTING;
    m_connected = false;
    m_upgraded = false;
    
    createContext();
    if (!m_context) {
        m_state = ConnectionState::FAILED;
        invokeErrorCallback(static_cast<int>(ErrorCode::CONNECT_FAILED), "Failed to create context");
        return;
    }
    
    // Create connection info
    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    
    ccinfo.context = m_context;
    ccinfo.address = m_host.c_str();
    ccinfo.port = m_port;
    ccinfo.path = m_path.c_str();
    ccinfo.host = m_host.c_str();
    ccinfo.origin = m_host.c_str();
    ccinfo.protocol = "ws";
    ccinfo.ssl_connection = m_secure ? LCCSCF_USE_SSL : 0;
    
    // Add custom headers
    std::string extra_headers;
    if (!m_headers.empty()) {
        for (const auto& pair : m_headers.all()) {
            extra_headers += pair.first + ": " + pair.second + "\r\n";
        }
    }
    if (!extra_headers.empty()) {
        ccinfo.ietf_version_or_minus_one = -1;
        // Note: libwebsockets handles headers differently, we'll need to set them in callback
    }
    
    m_wsi = lws_client_connect_via_info(&ccinfo);
    if (!m_wsi) {
        log_error("WebSocketClientWrapper: Failed to initiate connection");
        m_state = ConnectionState::FAILED;
        invokeErrorCallback(static_cast<int>(ErrorCode::CONNECT_FAILED), "Failed to initiate connection");
        return;
    }
    
    // Store instance pointer
    {
        std::lock_guard<std::mutex> map_lock(g_wsi_map_mutex);
        g_wsi_to_instance[m_wsi] = this;
    }
    
    // Start service thread
    if (!m_event_thread) {
        m_event_thread = new std::thread(&WebSocketClientWrapper::serviceLoop, this);
    }
    
    log_info("WebSocketClientWrapper: Connection initiated");
}

void WebSocketClientWrapper::disconnect() {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    
    if (m_state == ConnectionState::DISCONNECTED || m_state == ConnectionState::DISCONNECTING) {
        return;
    }
    
    m_state = ConnectionState::DISCONNECTING;
    m_connected = false;
    
    if (m_wsi) {
        // Remove from map
        {
            std::lock_guard<std::mutex> map_lock(g_wsi_map_mutex);
            g_wsi_to_instance.erase(m_wsi);
        }
        
        // Close connection
        lws_close_reason(m_wsi, LWS_CLOSE_STATUS_GOING_AWAY, (unsigned char*)"Normal closure", 15);
        m_wsi = nullptr;
    }
    
    // Stop service thread
    if (m_event_thread) {
        if (m_context) {
            lws_cancel_service(m_context);
        }
        if (m_event_thread->joinable()) {
            m_event_thread->join();
        }
        delete m_event_thread;
        m_event_thread = nullptr;
    }
    
    m_state = ConnectionState::DISCONNECTED;
    log_info("WebSocketClientWrapper: Disconnected");
}

bool WebSocketClientWrapper::isConnected() {
    return m_connected.load() && m_state == ConnectionState::CONNECTED;
}

bool WebSocketClientWrapper::sendMessage(const std::string& message) {
    return sendMessage(message.c_str(), message.length());
}

bool WebSocketClientWrapper::sendMessage(const char* msg, size_t len) {
    return sendData(msg, len, MessageType::TEXT);
}

bool WebSocketClientWrapper::sendBinary(const void* data, size_t length) {
    return sendData(data, length, MessageType::BINARY);
}

bool WebSocketClientWrapper::sendData(const void* data, size_t length, MessageType type) {
    if (!isConnected() || !m_wsi) {
        log_error("WebSocketClientWrapper: Cannot send - not connected");
        return false;
    }
    
    std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
    
    PendingMessage msg;
    msg.data.resize(length);
    memcpy(msg.data.data(), data, length);
    msg.type = type;
    msg.is_text = (type == MessageType::TEXT);
    
    m_send_queue.push_back(msg);
    
    // Trigger writeable callback
    lws_callback_on_writable(m_wsi);
    
    return true;
}

bool WebSocketClientWrapper::close(int code, const std::string& reason) {
    (void)code;   // Suppress unused parameter warning
    (void)reason; // Suppress unused parameter warning
    if (!m_wsi) {
        return false;
    }
    
    disconnect();
    return true;
}

bool WebSocketClientWrapper::close(CloseCode code, const std::string& reason) {
    return close(static_cast<int>(code), reason);
}

void WebSocketClientWrapper::enableCompression(bool enable) {
    m_use_compression = enable;
    log_info("WebSocketClientWrapper: Compression %s", enable ? "enabled" : "disabled");
}

void WebSocketClientWrapper::setTLSOptions(const WebSocketTLSOptions& options) {
    m_tls_options = options;
}

void WebSocketClientWrapper::setHeaders(const WebSocketHeaders& headers) {
    m_headers = headers;
}

void WebSocketClientWrapper::setPingInterval(int interval) {
    m_ping_interval = interval;
    log_info("WebSocketClientWrapper: Ping interval set to %d seconds", interval);
}

void WebSocketClientWrapper::setConnectionTimeout(int timeout) {
    m_connection_timeout = timeout;
}

void WebSocketClientWrapper::setMessageCallback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_message_callback = callback;
}

void WebSocketClientWrapper::setBinaryCallback(BinaryCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_binary_callback = callback;
}

void WebSocketClientWrapper::setCloseCallback(CloseCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_close_callback = callback;
}

void WebSocketClientWrapper::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_error_callback = callback;
}

void WebSocketClientWrapper::setOpenCallback(OpenCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_open_callback = callback;
}

void WebSocketClientWrapper::serviceLoop() {
    log_info("WebSocketClientWrapper: Service loop started");
    
    while (m_state != ConnectionState::DISCONNECTED && m_context) {
        int ret = lws_service(m_context, 0);
        if (ret < 0) {
            log_error("WebSocketClientWrapper: lws_service returned error: %d", ret);
            break;
        }
        
        // Process send queue
        sendPendingMessages();
    }
    
    log_info("WebSocketClientWrapper: Service loop ended");
}

bool WebSocketClientWrapper::sendPendingMessages() {
    if (!m_wsi || !isConnected()) {
        return false;
    }
    
    std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
    
    while (!m_send_queue.empty()) {
        const auto& msg = m_send_queue.front();
        
        unsigned char* buf = (unsigned char*)malloc(LWS_PRE + msg.data.size());
        if (!buf) {
            log_error("WebSocketClientWrapper: Failed to allocate send buffer");
            break;
        }
        
        memcpy(buf + LWS_PRE, msg.data.data(), msg.data.size());
        
        int flags = msg.is_text ? LWS_WRITE_TEXT : LWS_WRITE_BINARY;
        int ret = lws_write(m_wsi, buf + LWS_PRE, msg.data.size(), (enum lws_write_protocol)flags);
        
        free(buf);
        
        if (ret < 0) {
            log_error("WebSocketClientWrapper: Failed to send message");
            break;
        }
        
        m_send_queue.pop_front();
    }
    
    return true;
}

void WebSocketClientWrapper::invokeMessageCallback(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    if (m_message_callback) {
        try {
            m_message_callback(message);
        } catch (...) {
            log_error("WebSocketClientWrapper: Exception in message callback");
        }
    }
}

void WebSocketClientWrapper::invokeBinaryCallback(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    if (m_binary_callback) {
        try {
            m_binary_callback(data, len);
        } catch (...) {
            log_error("WebSocketClientWrapper: Exception in binary callback");
        }
    }
}

void WebSocketClientWrapper::invokeCloseCallback(int code, const std::string& reason) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    if (m_close_callback) {
        try {
            m_close_callback(code, reason);
        } catch (...) {
            log_error("WebSocketClientWrapper: Exception in close callback");
        }
    }
}

void WebSocketClientWrapper::invokeErrorCallback(int code, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    if (m_error_callback) {
        try {
            m_error_callback(code, message);
        } catch (...) {
            log_error("WebSocketClientWrapper: Exception in error callback");
        }
    }
}

void WebSocketClientWrapper::invokeOpenCallback() {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    if (m_open_callback) {
        try {
            m_open_callback();
        } catch (...) {
            log_error("WebSocketClientWrapper: Exception in open callback");
        }
    }
}

WebSocketClientWrapper* WebSocketClientWrapper::getInstance(struct lws* wsi) {
    std::lock_guard<std::mutex> lock(g_wsi_map_mutex);
    auto it = g_wsi_to_instance.find(wsi);
    return (it != g_wsi_to_instance.end()) ? it->second : nullptr;
}

void WebSocketClientWrapper::setWsi(struct lws* wsi) {
    m_wsi = wsi;
}

// libwebsockets protocol callback
int WebSocketClientWrapper::callback_client(struct lws *wsi, enum lws_callback_reasons reason,
                                            void *user, void *in, size_t len) {
    WebSocketClientWrapper* instance = getInstance(wsi);
    if (!instance) {
        return 0;
    }
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            instance->m_upgraded = true;
            instance->m_connected = true;
            instance->m_state = ConnectionState::CONNECTED;
            instance->invokeOpenCallback();
            log_info("WebSocketClientWrapper: Connection established");
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE:
            {
                // In libwebsockets, we need to check the frame type
                // The protocol buffer contains the frame data
                // For now, we'll treat all received data as text messages
                // Binary frames will be handled separately if needed
                std::string message((const char*)in, len);
                instance->invokeMessageCallback(message);
            }
            break;
            
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            instance->sendPendingMessages();
            break;
            
        case LWS_CALLBACK_CLIENT_CLOSED:
            instance->m_connected = false;
            instance->m_state = ConnectionState::DISCONNECTED;
            instance->invokeCloseCallback(1000, "Connection closed");
            log_info("WebSocketClientWrapper: Connection closed");
            break;
            
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            instance->m_state = ConnectionState::FAILED;
            instance->m_connected = false;
            {
                const char* err_msg = in ? (const char*)in : "Connection error";
                instance->invokeErrorCallback(static_cast<int>(ErrorCode::CONNECT_FAILED), err_msg);
            }
            log_error("WebSocketClientWrapper: Connection error");
            break;
            
        default:
            break;
    }
    
    return 0;
}


