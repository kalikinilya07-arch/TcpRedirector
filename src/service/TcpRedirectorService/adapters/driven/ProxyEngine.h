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

//
// ProxySession — manages one TCP tunnel through an HTTP proxy.
//
// Fixes applied:
//   H1  — uses m_config.login / m_config.plain_password instead of hardcoded
//         "proxy_user:proxy_pass".
//   M2  — checks the full "200 Connection established" phrase instead of
//         a bare substring search for "200".
//   H4  — bridge thread is stored and joined in Close() instead of detach().
//   M1  — half-close is handled: each direction is shut down independently,
//         data continues flowing on the other side until both are done.
//
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

    void SetLocalSocket(SOCKET s) { m_localSocket = s; }

    bool Start(const domain::RedirectEvent& redirect) override {
        m_redirect = redirect;
        m_state = domain::ConnectionState::ConnectingToProxy;
        return ConnectToProxy();
    }

    void Close() override {
        m_state = domain::ConnectionState::Closing;

        // If the bridge thread is still running, cancel its select()
        // by shutting down the sockets. Then join the thread.
        if (m_bridgeThread.joinable()) {
            // Shutdown both sockets to unblock select() in BridgeLoop.
            if (m_localSocket != INVALID_SOCKET) {
                shutdown(m_localSocket, SD_BOTH);
            }
            if (m_proxySocket != INVALID_SOCKET) {
                shutdown(m_proxySocket, SD_BOTH);
            }
            m_bridgeThread.join();
        }

        if (m_localSocket != INVALID_SOCKET) {
            closesocket(m_localSocket);
            m_localSocket = INVALID_SOCKET;
        }
        if (m_proxySocket != INVALID_SOCKET) {
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
    // ---- Connection setup -------------------------------------------------

    bool ConnectToProxy() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            HandleError("wsa_startup", "Failed to initialize Winsock");
            return false;
        }

        m_proxySocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_proxySocket == INVALID_SOCKET) {
            HandleError("socket", "Failed to create socket");
            return false;
        }

        int timeout = 10000;
        setsockopt(m_proxySocket, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&timeout, sizeof(timeout));
        setsockopt(m_proxySocket, SOL_SOCKET, SO_SNDTIMEO,
                   (const char*)&timeout, sizeof(timeout));

        std::string proxy_host(m_config.host.begin(), m_config.host.end());

        // Use getaddrinfo instead of deprecated gethostbyname
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        struct addrinfo* addrResult = nullptr;

        int rc = getaddrinfo(proxy_host.c_str(), nullptr, &hints, &addrResult);
        if (rc != 0 || !addrResult) {
            HandleError("resolve", "Failed to resolve proxy host: " + proxy_host);
            return false;
        }

        sockaddr_in proxy_addr;
        memcpy(&proxy_addr, addrResult->ai_addr, sizeof(proxy_addr));
        proxy_addr.sin_port = htons(m_config.port);
        freeaddrinfo(addrResult);

        if (connect(m_proxySocket, (sockaddr*)&proxy_addr, sizeof(proxy_addr)) != 0) {
            HandleError("connect", "Failed to connect to proxy");
            return false;
        }

        return SendConnectRequest();
    }

    // ---- H1: use real login/password from config --------------------------

    bool SendConnectRequest() {
        struct in_addr addr;
        addr.s_addr = m_redirect.original_address_v4;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);

        std::string connect_request =
            "CONNECT " + std::string(ip_str) + ":" +
            std::to_string(m_redirect.original_port) + " HTTP/1.1\r\n"
            "Host: " + std::string(ip_str) + ":" +
            std::to_string(m_redirect.original_port) + "\r\n";

        // H1: use configured credentials instead of hardcoded "proxy_user:proxy_pass"
        if (m_config.auth_required && m_config.has_password && !m_config.plain_password.empty()) {
            std::string login(m_config.login.begin(), m_config.login.end());
            std::string password(m_config.plain_password.begin(), m_config.plain_password.end());
            std::string credentials = login + ":" + password;
            connect_request += "Proxy-Authorization: Basic " +
                               Base64Encode(credentials) + "\r\n";
        } else if (m_config.auth_required && m_config.has_password) {
            // Fallback: password exists but wasn't decrypted — try empty password
            std::string login(m_config.login.begin(), m_config.login.end());
            std::string credentials = login + ":";
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

    // ---- M2: check full "200 Connection established" -----------------------

    bool ReadConnectResponse() {
        char buffer[4096];
        int bytes = recv(m_proxySocket, buffer, sizeof(buffer) - 1, 0);

        if (bytes <= 0) {
            HandleError("read_response", "No response from proxy");
            return false;
        }

        buffer[bytes] = '\0';
        std::string response(buffer);

        // M2: search for the full status line, not just bare "200"
        bool established =
            response.find("200 Connection established") != std::string::npos ||
            response.find("200 Connection Established") != std::string::npos ||
            response.find("200 OK") != std::string::npos;

        if (established) {
            m_state = domain::ConnectionState::TunnelEstablished;

            // H4: store and join the bridge thread instead of detach()
            m_bridgeThread = std::thread([this]() { BridgeLoop(); });

            if (m_callback) m_callback(true, "");
            return true;
        }

        HandleError("bad_response", "Proxy returned: " + response.substr(0, 100));
        return false;
    }

    // ---- M1 + H4: half-close + joinable bridge thread --------------------

    void BridgeLoop() {
        char buf[65536];
        // Track which directions have seen EOF.
        bool localDone = false;
        bool proxyDone = false;

        while (m_state == domain::ConnectionState::TunnelEstablished &&
               !(localDone && proxyDone)) {

            fd_set read_set;
            FD_ZERO(&read_set);
            timeval tv = {1, 0};
            int maxFd = 0;

            if (!localDone && m_localSocket != INVALID_SOCKET) {
                FD_SET(m_localSocket, &read_set);
                maxFd = (std::max)(maxFd, (int)m_localSocket);
            }
            if (!proxyDone && m_proxySocket != INVALID_SOCKET) {
                FD_SET(m_proxySocket, &read_set);
                maxFd = (std::max)(maxFd, (int)m_proxySocket);
            }

            int ret = select(maxFd + 1, &read_set, NULL, NULL, &tv);
            if (ret < 0) break;
            if (ret == 0) continue;

            // M1: half-close — when one side sends EOF, only shut down the
            // outgoing direction to the other side, keep reading the reverse.
            if (!localDone && m_localSocket != INVALID_SOCKET &&
                FD_ISSET(m_localSocket, &read_set)) {
                int n = recv(m_localSocket, buf, sizeof(buf), 0);
                if (n > 0) {
                    m_txBytes.fetch_add(n, std::memory_order_relaxed);
                    send(m_proxySocket, buf, n, 0);
                } else {
                    // local -> proxy direction done; shutdown proxy's write side.
                    localDone = true;
                    if (m_proxySocket != INVALID_SOCKET) {
                        shutdown(m_proxySocket, SD_SEND);
                    }
                }
            }

            if (!proxyDone && m_proxySocket != INVALID_SOCKET &&
                FD_ISSET(m_proxySocket, &read_set)) {
                int n = recv(m_proxySocket, buf, sizeof(buf), 0);
                if (n > 0) {
                    m_rxBytes.fetch_add(n, std::memory_order_relaxed);
                    if (m_localSocket != INVALID_SOCKET) {
                        send(m_localSocket, buf, n, 0);
                    }
                } else {
                    // proxy -> local direction done; shutdown local's write side.
                    proxyDone = true;
                    if (m_localSocket != INVALID_SOCKET) {
                        shutdown(m_localSocket, SD_SEND);
                    }
                }
            }
        }

        // Both directions done — close everything. We do NOT call Close()
        // here because Close() would try to join this very thread → deadlock.
        // Instead, we mark state and let Close() (called externally) handle it.
        m_state = domain::ConnectionState::Closing;
    }

    void HandleError(const std::string& context, const std::string& message) {
        m_errorMessage = context + ": " + message;
        m_state = domain::ConnectionState::Error;
        Close();
        if (m_callback) m_callback(false, m_errorMessage);
    }

    // ---- Base64 (kept local, duplicates exist elsewhere) ------------------

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

    // ---- Members ----------------------------------------------------------

    SOCKET m_localSocket = INVALID_SOCKET;
    SOCKET m_proxySocket = INVALID_SOCKET;
    std::thread m_bridgeThread;             // H4: joinable, NOT detached
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

//
// ProxyEngine — factory that creates ProxySession instances.
//
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

    // ---- M11: WSACleanup called AFTER all sessions are joined --------------

    void Shutdown() override {
        // 1. Close all sessions — this joins their bridge threads.
        {
            std::unique_lock lock(m_mutex);
            for (auto& [id, session] : m_sessions) {
                session->Close();
            }
            m_sessions.clear();
        }
        // 2. Only then clean up Winsock.
        WSACleanup();
        m_initialized = false;
    }

    std::shared_ptr<domain::ports::IProxySession> CreateSession(
        const domain::RedirectEvent& redirect,
        std::function<void(bool, const std::string&)> callback) override
    {
        auto session = std::make_shared<ProxySession>(m_config, m_nextSessionId++);
        session->SetCompletionCallback(callback);

        // H4: Start synchronously instead of detach().
        // The bridge thread is created inside ReadConnectResponse as needed.
        session->Start(redirect);

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