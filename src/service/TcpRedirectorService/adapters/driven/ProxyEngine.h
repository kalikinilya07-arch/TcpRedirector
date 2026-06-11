#pragma once

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <unordered_map>
#include <shared_mutex>
#include <sstream>
#include <iomanip>
#include "../../domain/ports/IProxyConnector.h"
#include "../../domain/entities/ProxyConfig.h"

#pragma comment(lib, "ws2_32.lib")

namespace tcp_redirector {
namespace infrastructure {

class ProxySession : public domain::ports::IProxySession,
                     public std::enable_shared_from_this<ProxySession> {
public:
    ProxySession(const domain::ProxyConfig& config, uint64_t session_id)
        : m_config(config)
        , m_sessionId(session_id)
        , m_startTime(std::chrono::steady_clock::now())
        , m_state(domain::ConnectionState::Redirecting) {
        m_localSocket = INVALID_SOCKET;
        m_proxySocket = INVALID_SOCKET;
    }

    ~ProxySession() override {
        Close();
    }

    // Установить локальный сокет для бриджа (восстановлено после рефакторинга)
    void SetLocalSocket(SOCKET s) { m_localSocket = s; }

    bool Start(const domain::RedirectEvent& redirect) override {
        m_redirect = redirect;
        m_state = domain::ConnectionState::ConnectingToProxy;
        return ConnectToProxy();
    }

    void Close() override {
        if (m_localSocket != INVALID_SOCKET) {
            shutdown(m_localSocket, SD_BOTH);
            closesocket(m_localSocket);
            m_localSocket = INVALID_SOCKET;
        }
        if (m_proxySocket != INVALID_SOCKET) {
            shutdown(m_proxySocket, SD_BOTH);
            closesocket(m_proxySocket);
            m_proxySocket = INVALID_SOCKET;
        }
        m_state = domain::ConnectionState::Closed;
    }

    domain::ports::ProxySessionStats GetStats() const override {
        domain::ports::ProxySessionStats stats;
        stats.rx_bytes = m_rxBytes.load();
        stats.tx_bytes = m_txBytes.load();
        stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_startTime);
        stats.connected = (m_state == domain::ConnectionState::TunnelEstablished);
        stats.error_message = m_errorMessage;
        return stats;
    }

    bool IsActive() const override {
        return m_state == domain::ConnectionState::TunnelEstablished;
    }

    uint64_t GetSessionId() const override { return m_sessionId; }

    void SetCompletionCallback(std::function<void(bool, const std::string&)> cb) {
        m_callback = std::move(cb);
    }

    domain::ConnectionState GetState() const { return m_state; }

private:
    bool ConnectToProxy() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            HandleError("wsa_startup", "Failed to initialize Winsock");
            return false;
        }

        // Create socket to proxy
        m_proxySocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_proxySocket == INVALID_SOCKET) {
            HandleError("socket", "Failed to create socket");
            return false;
        }

        // Set timeout
        int timeout = 10000;
        setsockopt(m_proxySocket, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&timeout, sizeof(timeout));
        setsockopt(m_proxySocket, SOL_SOCKET, SO_SNDTIMEO,
                   (const char*)&timeout, sizeof(timeout));

        // Resolve proxy host
        std::string proxy_host(m_config.host.begin(), m_config.host.end());
        struct hostent* host = gethostbyname(proxy_host.c_str());
        if (!host) {
            HandleError("resolve", "Failed to resolve proxy host: " + proxy_host);
            return false;
        }

        // Connect to proxy
        sockaddr_in proxy_addr;
        proxy_addr.sin_family = AF_INET;
        proxy_addr.sin_port = htons(m_config.port);
        memcpy(&proxy_addr.sin_addr, host->h_addr, host->h_length);

        if (connect(m_proxySocket, (sockaddr*)&proxy_addr, sizeof(proxy_addr)) != 0) {
            HandleError("connect", "Failed to connect to proxy");
            return false;
        }

        // Send CONNECT request
        return SendConnectRequest();
    }

    bool SendConnectRequest() {
        std::string connect_request;

        struct in_addr addr;
        addr.s_addr = m_redirect.original_address_v4;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);

        connect_request = "CONNECT " + std::string(ip_str) + ":" +
            std::to_string(m_redirect.original_port) + " HTTP/1.1\r\n"
            "Host: " + std::string(ip_str) + ":" +
            std::to_string(m_redirect.original_port) + "\r\n";

        if (m_config.auth_required && m_config.has_password) {
            std::string credentials = "proxy_user:proxy_pass";
            connect_request += "Proxy-Authorization: Basic " +
                               Base64Encode(credentials) + "\r\n";
        }

        connect_request += "User-Agent: TcpRedirector/1.0\r\n"
                           "Proxy-Connection: Keep-Alive\r\n"
                           "\r\n";

        if (send(m_proxySocket, connect_request.c_str(),
                 (int)connect_request.length(), 0) == SOCKET_ERROR) {
            HandleError("send_connect", "Failed to send CONNECT request");
            return false;
        }

        return ReadConnectResponse();
    }

    bool ReadConnectResponse() {
        char buffer[4096];
        int bytes = recv(m_proxySocket, buffer, sizeof(buffer) - 1, 0);

        if (bytes <= 0) {
            HandleError("read_response", "No response from proxy");
            return false;
        }

        buffer[bytes] = '\0';
        std::string response(buffer);

        // Check for "200 Connection Established"
        if (response.find("200") != std::string::npos) {
            m_state = domain::ConnectionState::TunnelEstablished;

            // Start bridging in a background thread
            m_bridgeThread = std::thread([this]() { BridgeLoop(); });
            m_bridgeThread.detach();

            if (m_callback) m_callback(true, "");
            return true;
        }

        HandleError("bad_response", "Proxy returned: " + response.substr(0, 100));
        return false;
    }

    void BridgeLoop() {
        fd_set read_set;
        char buf[65536];

        while (m_state == domain::ConnectionState::TunnelEstablished) {
            FD_ZERO(&read_set);
            bool hasLocal = (m_localSocket != INVALID_SOCKET);
            if (hasLocal) FD_SET(m_localSocket, &read_set);
            FD_SET(m_proxySocket, &read_set);

            timeval tv = {1, 0};
            int ret = select(0, &read_set, NULL, NULL, &tv);
            if (ret <= 0) continue;

            if (hasLocal && FD_ISSET(m_localSocket, &read_set)) {
                int n = recv(m_localSocket, buf, sizeof(buf), 0);
                if (n > 0) {
                    m_txBytes += n;
                    send(m_proxySocket, buf, n, 0);
                } else break;
            }

            if (FD_ISSET(m_proxySocket, &read_set)) {
                int n = recv(m_proxySocket, buf, sizeof(buf), 0);
                if (n > 0) {
                    m_rxBytes += n;
                    if (hasLocal) send(m_localSocket, buf, n, 0);
                } else break;
            }
        }

        Close();
    }

    void HandleError(const std::string& context, const std::string& message) {
        m_errorMessage = context + ": " + message;
        m_state = domain::ConnectionState::Error;
        Close();
        if (m_callback) m_callback(false, m_errorMessage);
    }

    static std::string Base64Encode(const std::string& input) {
        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "abcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        int i = 0;
        unsigned char c3[3], c4[4];
        for (char ch : input) {
            c3[i++] = ch;
            if (i == 3) {
                c4[0] = (c3[0] & 0xfc) >> 2;
                c4[1] = ((c3[0] & 0x03) << 4) + ((c3[1] & 0xf0) >> 4);
                c4[2] = ((c3[1] & 0x0f) << 2) + ((c3[2] & 0xc0) >> 6);
                c4[3] = c3[2] & 0x3f;
                for (int j = 0; j < 4; j++) result += b64[c4[j]];
                i = 0;
            }
        }
        if (i > 0) {
            for (int j = i; j < 3; j++) c3[j] = 0;
            c4[0] = (c3[0] & 0xfc) >> 2;
            c4[1] = ((c3[0] & 0x03) << 4) + ((c3[1] & 0xf0) >> 4);
            c4[2] = ((c3[1] & 0x0f) << 2) + ((c3[2] & 0xc0) >> 6);
            for (int j = 0; j < i + 1; j++) result += b64[c4[j]];
            while (i++ < 3) result += '=';
        }
        return result;
    }

    SOCKET m_localSocket = INVALID_SOCKET;
    SOCKET m_proxySocket = INVALID_SOCKET;
    std::thread m_bridgeThread;
    domain::ProxyConfig m_config;
    domain::RedirectEvent m_redirect;
    uint64_t m_sessionId;
    std::chrono::steady_clock::time_point m_startTime;
    std::atomic<uint64_t> m_rxBytes{0};
    std::atomic<uint64_t> m_txBytes{0};
    std::string m_errorMessage;
    domain::ConnectionState m_state;
    std::function<void(bool, const std::string&)> m_callback;
};

class ProxyEngine : public domain::ports::IProxyConnector {
public:
    ProxyEngine() = default;

    ~ProxyEngine() override { Shutdown(); }

    bool Initialize(const domain::ProxyConfig& config) override {
        m_config = config;
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
        m_initialized = true;
        return true;
    }

    void Shutdown() override {
        std::unique_lock lock(m_mutex);
        for (auto& [id, session] : m_sessions) {
            session->Close();
        }
        m_sessions.clear();
        WSACleanup();
        m_initialized = false;
    }

    std::shared_ptr<domain::ports::IProxySession> CreateSession(
        const domain::RedirectEvent& redirect,
        std::function<void(bool, const std::string&)> callback) override
    {
        auto session = std::make_shared<ProxySession>(m_config, m_nextSessionId++);
        session->SetCompletionCallback(callback);

        // Start on a background thread
        std::thread([session, redirect]() {
            session->Start(redirect);
        }).detach();

        {
            std::unique_lock lock(m_mutex);
            m_sessions[session->GetSessionId()] = session;
        }

        return session;
    }

    domain::ServiceStats GetAggregatedStats() const override {
        domain::ServiceStats stats;
        std::shared_lock lock(m_mutex);
        stats.active_connections = static_cast<uint32_t>(m_sessions.size());
        return stats;
    }

    bool IsInitialized() const override { return m_initialized; }

private:
    domain::ProxyConfig m_config;
    mutable std::shared_mutex m_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<ProxySession>> m_sessions;
    std::atomic<uint64_t> m_nextSessionId{1};
    bool m_initialized = false;
};

} // namespace infrastructure
} // namespace tcp_redirector