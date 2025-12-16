/*
 *  WebSocketClientWrapper.h
 *  Wrapper for libwebsockets to replace libwsc WebSocketClient
 *  Author: TechGuyVN
 *  Copyright (c) 2025
 */

#ifndef WEBSOCKET_CLIENT_WRAPPER_H
#define WEBSOCKET_CLIENT_WRAPPER_H

#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <deque>

// Forward declarations
struct lws_context;
struct lws;
struct lws_client_connect_info;

// Include libwebsockets headers
#include <libwebsockets.h>

// Forward declare classes from libwsc
class WebSocketHeaders;
class WebSocketTLSOptions;

class WebSocketClientWrapper {
public:
    // Match WebSocketClient interface
    enum class MessageType {
        TEXT,
        BINARY,
        PING,
        PONG,
        CLOSE
    };

    enum class ConnectionState {
        DISCONNECTED,
        DISCONNECTING,
        CONNECTING,
        CONNECTED,
        FAILED
    };

    enum class ErrorCode {
        IO = 1,
        INVALID_HEADER,
        SERVER_MASKED,
        NOT_SUPPORTED,
        PING_TIMEOUT,
        CONNECT_FAILED,
        TLS_INIT_FAILED,
        SSL_HANDSHAKE_FAILED,
        SSL_ERROR,
    };

    enum class CloseCode {
        NORMAL = 1000,
        GOING_AWAY = 1001,
        PROTOCOL_ERROR = 1002,
        UNSUPPORTED = 1003,
        NO_STATUS = 1005,
        ABNORMAL = 1006,
        INVALID_PAYLOAD = 1007,
        POLICY_VIOLATION = 1008,
        MESSAGE_TOO_BIG = 1009,
        MANDATORY_EXTENSION = 1010,
        INTERNAL_ERROR = 1011,
        SERVICE_RESTART = 1012,
        TRY_AGAIN_LATER = 1013,
        TLS_HANDSHAKE = 1015,
        UNKNOWN = 0
    };

    // Callback types matching WebSocketClient
    using MessageCallback = std::function<void(const std::string&)>;
    using BinaryCallback = std::function<void(const void*, size_t)>;
    using CloseCallback = std::function<void(int code, const std::string& reason)>;
    using ErrorCallback = std::function<void(int error_code, const std::string& error_message)>;
    using OpenCallback = std::function<void()>;

    WebSocketClientWrapper();
    ~WebSocketClientWrapper();

    // Non-copyable
    WebSocketClientWrapper(const WebSocketClientWrapper&) = delete;
    WebSocketClientWrapper& operator=(const WebSocketClientWrapper&) = delete;

    void connect();
    void disconnect();
    bool isConnected();
    void setUrl(const std::string& url);
    bool sendMessage(const std::string& message);
    bool sendMessage(const char* msg, size_t len);
    bool sendData(const void* data, size_t length, MessageType type);
    bool sendBinary(const void* data, size_t length);
    void setMessageCallback(MessageCallback callback);
    void setBinaryCallback(BinaryCallback callback);
    void setCloseCallback(CloseCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void setOpenCallback(OpenCallback callback);
    bool close(int code = 1000, const std::string& reason = "Normal closure");
    bool close(CloseCode code, const std::string& reason);
    void enableCompression(bool enable = true);
    void setTLSOptions(const WebSocketTLSOptions& options);
    void setHeaders(const WebSocketHeaders& headers);
    void setPingInterval(int interval);
    void setConnectionTimeout(int timeout);

private:
    // libwebsockets context and wsi
    struct lws_context* m_context;
    struct lws* m_wsi;
    
    // Connection info
    std::string m_url;
    std::string m_host;
    std::string m_path;
    int m_port;
    bool m_secure;
    
    // State
    std::atomic<ConnectionState> m_state;
    std::atomic<bool> m_connected;
    std::atomic<bool> m_upgraded;
    
    // Callbacks
    MessageCallback m_message_callback;
    BinaryCallback m_binary_callback;
    CloseCallback m_close_callback;
    ErrorCallback m_error_callback;
    OpenCallback m_open_callback;
    
    // Threading
    std::thread* m_event_thread;
    std::mutex m_callback_mutex;
    std::mutex m_send_mutex;
    
    // Send queue
    struct PendingMessage {
        std::vector<uint8_t> data;
        MessageType type;
        bool is_text;
    };
    std::deque<PendingMessage> m_send_queue;
    std::mutex m_queue_mutex;
    
    // Options
    bool m_use_compression;
    int m_ping_interval;
    int m_connection_timeout;
    WebSocketTLSOptions m_tls_options;
    WebSocketHeaders m_headers;
    
    // Helper methods
    void parseUrl(const std::string& url);
    void createContext();
    void destroyContext();
    void serviceLoop();
    bool sendPendingMessages();
    void invokeMessageCallback(const std::string& message);
    void invokeBinaryCallback(const void* data, size_t len);
    void invokeCloseCallback(int code, const std::string& reason);
    void invokeErrorCallback(int code, const std::string& message);
    void invokeOpenCallback();
    
    // Instance pointer for callbacks
    static WebSocketClientWrapper* getInstance(struct lws* wsi);
    void setWsi(struct lws* wsi);
};

#endif // WEBSOCKET_CLIENT_WRAPPER_H

