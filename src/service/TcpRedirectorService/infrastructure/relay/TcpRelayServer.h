#pragma once

//
// TcpRelayServer — local TCP relay (аналог local_proxy_server из ProxyBridge).
//
// Слушает на INADDR_ANY:relayPort, принимает перенаправленные SYN-пакеты,
// смотрит original destination из ConnectionTable, подключается к прокси,
// и запускает двунаправленный бридж данных (client ↔ proxy).
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>
#include <cstring>

#include "../../domain/ports/IRelayServer.h"
#include "../../domain/ports/IConnectionTable.h"
#include "../../domain/entities/ProxyConfig.h"
#include "../../infrastructure/auth/auth_sspi.h"

namespace tcp_redirector {
namespace infrastructure {

// Структура для двунаправленного бриджа (как RELAY_PAIR в ProxyBridge)
struct RelayPair {
    SOCKET sock_client;
    SOCKET sock_proxy;
    volatile LONG refs;
};

// Коллбэки для уведомления о событиях
using RelayLogCallback = std::function<void(const std::string&)>;
using RelayConnCallback = std::function<void(const std::string& process_name, uint32_t pid,
    const std::string& dest_ip, uint16_t dest_port, const std::string& proxy_info)>;

class TcpRelayServer : public domain::ports::IRelayServer {
public:
    TcpRelayServer(domain::ports::IConnectionTable& conn_table, uint16_t relay_port = 34010)
        : m_connTable(conn_table)
        , m_relayPort(relay_port)
        , m_listenSock(INVALID_SOCKET)
        , m_listenSock6(INVALID_SOCKET) {
    }

    ~TcpRelayServer() {
        Stop();
    }

    void SetProxyConfig(const domain::ProxyConfig& config, uint32_t config_id = 1) {
        m_proxyHost = std::string(config.host.begin(), config.host.end());
        m_proxyPort = config.port;
        m_proxyAuthRequired = config.auth_required;
        m_kerberosAuth = config.kerberos_auth;
        m_proxyConfigId = config_id;
        // H1: store decrypted password from ConfigManager
        m_proxyUser = std::string(config.login.begin(), config.login.end());
        m_proxyPassword = std::string(config.plain_password.begin(), config.plain_password.end());
    }

    void SetLogCallback(RelayLogCallback cb) { m_logCb = std::move(cb); }
    void SetConnCallback(RelayConnCallback cb) { m_connCb = std::move(cb); }

    bool Start() {
        if (m_running) return true;

        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            Log("[RELAY] WSAStartup failed");
            return false;
        }

        m_listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenSock == INVALID_SOCKET) {
            Log("[RELAY] socket() failed: " + std::to_string(WSAGetLastError()));
            WSACleanup();
            return false;
        }

        int on = 1;
        setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

        // INADDR_ANY — критически важно! WinDivert меняет IP местами для non-loopback,
        // поэтому пакеты приходят на реальный IP машины, а не на 127.0.0.1
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(m_relayPort);

        if (bind(m_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            Log("[RELAY] bind(" + std::to_string(m_relayPort) + ") failed: " + std::to_string(WSAGetLastError()));
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
            WSACleanup();
            return false;
        }

        if (listen(m_listenSock, SOMAXCONN) == SOCKET_ERROR) {
            Log("[RELAY] listen() failed: " + std::to_string(WSAGetLastError()));
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
            WSACleanup();
            return false;
        }

        // IPv6 listen
        m_listenSock6 = socket(AF_INET6, SOCK_STREAM, 0);
        if (m_listenSock6 != INVALID_SOCKET) {
            int v6only = 1;
            setsockopt(m_listenSock6, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
            setsockopt(m_listenSock6, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
            sockaddr_in6 addr6 = {};
            addr6.sin6_family = AF_INET6;
            addr6.sin6_addr = in6addr_any;
            addr6.sin6_port = htons(m_relayPort);
            if (bind(m_listenSock6, (sockaddr*)&addr6, sizeof(addr6)) == SOCKET_ERROR ||
                listen(m_listenSock6, SOMAXCONN) == SOCKET_ERROR) {
                closesocket(m_listenSock6);
                m_listenSock6 = INVALID_SOCKET;
            }
        }

        m_running = true;
        m_acceptThread = std::thread(&TcpRelayServer::AcceptLoop, this);

        Log("[RELAY] Listening on 0.0.0.0:" + std::to_string(m_relayPort));
        return true;
    }

    void Stop() {
        if (!m_running) return;
        m_running = false;

        if (m_listenSock != INVALID_SOCKET) {
            shutdown(m_listenSock, SD_BOTH);
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }
        if (m_listenSock6 != INVALID_SOCKET) {
            shutdown(m_listenSock6, SD_BOTH);
            closesocket(m_listenSock6);
            m_listenSock6 = INVALID_SOCKET;
        }

        if (m_acceptThread.joinable())
            m_acceptThread.join();

        WSACleanup();
        Log("[RELAY] Stopped");
    }

    bool IsRunning() const override { return m_running; }

    uint16_t GetPort() const override { return m_relayPort; }

private:
    void AcceptLoop() {
        while (m_running) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(m_listenSock, &read_fds);
            if (m_listenSock6 != INVALID_SOCKET)
                FD_SET(m_listenSock6, &read_fds);

            timeval timeout = {1, 0};
            int ret = select(0, &read_fds, nullptr, nullptr, &timeout);
            if (ret <= 0) continue;

            if (FD_ISSET(m_listenSock, &read_fds)) {
                sockaddr_in client_addr;
                int addr_len = sizeof(client_addr);
                SOCKET client_sock = accept(m_listenSock, (sockaddr*)&client_addr, &addr_len);
                if (client_sock != INVALID_SOCKET) {
                    HandleNewConnection(client_sock, ntohs(client_addr.sin_port), false, nullptr);
                }
            }

            if (m_listenSock6 != INVALID_SOCKET && FD_ISSET(m_listenSock6, &read_fds)) {
                sockaddr_in6 client_addr6;
                int addr_len6 = sizeof(client_addr6);
                SOCKET client_sock6 = accept(m_listenSock6, (sockaddr*)&client_addr6, &addr_len6);
                if (client_sock6 != INVALID_SOCKET) {
                    HandleNewConnection(client_sock6, ntohs(client_addr6.sin6_port), true,
                        (const uint8_t*)&client_addr6.sin6_addr);
                }
            }
        }
    }

    void HandleNewConnection(SOCKET client_sock, uint16_t client_port,
                             bool is_ipv6, const uint8_t* client_addr6) {
        uint32_t orig_dest_ip = 0;
        uint16_t orig_dest_port = 0;

        if (!m_connTable.Get(client_port, &orig_dest_ip, &orig_dest_port)) {
            Log("[RELAY] No connection record for port " + std::to_string(client_port));
            closesocket(client_sock);
            return;
        }

        uint32_t proxy_config_id = m_connTable.GetProxyConfigId(client_port);

        auto* ctx = new RelayContext();
        ctx->server = this;
        ctx->client_sock = client_sock;
        ctx->orig_dest_ip = orig_dest_ip;
        ctx->orig_dest_port = orig_dest_port;
        ctx->proxy_config_id = proxy_config_id;
        ctx->is_ipv6 = is_ipv6;
        if (is_ipv6 && client_addr6)
            memcpy(ctx->client_addr6, client_addr6, 16);

        HANDLE hThread = CreateThread(nullptr, 0, &TcpRelayServer::ConnectionHandlerThunk,
                                       ctx, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            delete ctx;
            closesocket(client_sock);
        }
    }

    struct RelayContext {
        TcpRelayServer* server;
        SOCKET client_sock;
        uint32_t orig_dest_ip;
        uint16_t orig_dest_port;
        uint32_t proxy_config_id;
        bool is_ipv6;
        uint8_t client_addr6[16];
    };

    static DWORD WINAPI ConnectionHandlerThunk(LPVOID arg) {
        auto* ctx = static_cast<RelayContext*>(arg);
        ctx->server->ConnectionHandler(ctx);
        return 0;
    }

    void ConnectionHandler(RelayContext* ctx) {
        SOCKET client_sock = ctx->client_sock;
        uint32_t dest_ip = ctx->orig_dest_ip;
        uint16_t dest_port = ctx->orig_dest_port;
        delete ctx;

        // DEBUG: проверим, какие флаги реально приходят
        Log("[AUTH-DEBUG] m_proxyAuthRequired=" + std::to_string(m_proxyAuthRequired) +
            " m_kerberosAuth=" + std::to_string(m_kerberosAuth));

        SOCKET proxy_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (proxy_sock == INVALID_SOCKET) {
            Log("[RELAY] Failed to create proxy socket");
            closesocket(client_sock);
            return;
        }

        // Большие буферы (4 MB) как в ProxyBridge
        int bufsize = 4194304;
        int timeout = 30000;
        setsockopt(proxy_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));
        setsockopt(proxy_sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));
        setsockopt(proxy_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(proxy_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));
        setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));

        // Resolve proxy host via getaddrinfo
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        struct addrinfo* result = nullptr;
        if (getaddrinfo(m_proxyHost.c_str(), nullptr, &hints, &result) != 0 || !result) {
            Log("Failed to resolve proxy: " + m_proxyHost);
            closesocket(client_sock);
            closesocket(proxy_sock);
            if (result) freeaddrinfo(result);
            return;
        }

        sockaddr_in proxy_addr;
        memset(&proxy_addr, 0, sizeof(proxy_addr));
        proxy_addr.sin_family = AF_INET;
        proxy_addr.sin_port = htons(m_proxyPort);
        proxy_addr.sin_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
        freeaddrinfo(result);

        if (connect(proxy_sock, (sockaddr*)&proxy_addr, sizeof(proxy_addr)) != 0) {
            Log("connect to proxy failed: " + std::to_string(WSAGetLastError()));
            closesocket(client_sock);
            closesocket(proxy_sock);
            return;
        }

        // HTTP CONNECT
        char ip_str[INET_ADDRSTRLEN] = {};
        struct in_addr in;
        in.s_addr = dest_ip;
        inet_ntop(AF_INET, &in, ip_str, sizeof(ip_str));

        // ---- SSPI/Kerberos: инициализация (только при первом CONNECT) ----
        infrastructure::SspiContext sspiCtx;
        std::string sspiToken;
        bool sspiInitDone = false;
        bool sspiAvailable = true;   // локальный флаг, НЕ классовый m_kerberosAuth
        int authRetries = 0;

    retry_connect:

        std::string connect_req = "CONNECT " + std::string(ip_str) + ":" +
            std::to_string(dest_port) + " HTTP/1.1\r\nHost: " +
            std::string(ip_str) + ":" + std::to_string(dest_port) + "\r\n";

        if (m_proxyAuthRequired && !m_kerberosAuth) {
            // Basic Auth (оригинальное поведение — без изменений)
            // H1: use configured password instead of hardcoded ":proxy_pass"
            std::string basic = m_proxyUser + ":" + m_proxyPassword;
            connect_req += "Proxy-Authorization: Basic " + Base64Encode(basic) + "\r\n";
        } else if (m_kerberosAuth && sspiAvailable) {
            // Negotiate/Kerberos через SSPI
            if (!sspiInitDone) {
                Log("[SSPI] Acquiring credentials for " + m_proxyHost + "...");
                auto r = infrastructure::SspiNegotiate(sspiCtx, "", sspiToken,
                    infrastructure::MakeSpn(m_proxyHost));
                if (r == infrastructure::SspiResult::NoCredentials) {
                    Log("[SSPI] Kerberos/NTLM недоступен (SEC_E_NO_CREDENTIALS)");
                    sspiAvailable = false; // локальный флаг, НЕ классовый
                } else if (r == infrastructure::SspiResult::Error) {
                    Log("[SSPI] Ошибка инициализации SSPI");
                    sspiAvailable = false;
                } else {
                    Log("[SSPI] Token получен: " +
                        (sspiToken.empty() ? std::string("empty") :
                         std::to_string(sspiToken.size()) + " bytes"));
                }
                sspiInitDone = true;
            }
            if (!sspiToken.empty()) {
                connect_req += "Proxy-Authorization: Negotiate " + sspiToken + "\r\n";
            }
        }

        connect_req += "Proxy-Connection: Keep-Alive\r\n\r\n";

        if (send(proxy_sock, connect_req.c_str(), (int)connect_req.length(), 0) == SOCKET_ERROR) {
            Log("send CONNECT failed");
            closesocket(client_sock);
            closesocket(proxy_sock);
            return;
        }

        char resp_buf[4096];
        int bytes = recv(proxy_sock, resp_buf, sizeof(resp_buf) - 1, 0);
        if (bytes <= 0) {
            Log("no CONNECT response");
            closesocket(client_sock);
            closesocket(proxy_sock);
            return;
        }
        resp_buf[bytes] = '\0';

        // M2: check full "200 Connection established" phrase, not bare "200"
        if (strstr(resp_buf, "200 Connection established") != nullptr ||
            strstr(resp_buf, "200 Connection Established") != nullptr ||
            strstr(resp_buf, "200 OK") != nullptr) {
            // CONNECT успешен — выходим
        }
        else if (m_kerberosAuth && sspiAvailable && strstr(resp_buf, "407") != nullptr) {
            // ---- 407 Proxy Auth Required — SSPI-цикл ----
            std::string challenge = infrastructure::Parse407Challenge(resp_buf);
            if (challenge.empty()) {
                Log("CONNECT failed: 407 без Negotiate challenge");
                closesocket(client_sock);
                closesocket(proxy_sock);
                return;
            }
            Log("[SSPI] Got 407 challenge (" + std::to_string(challenge.size()) + " bytes), continuing...");
            auto r = infrastructure::SspiNegotiate(sspiCtx, challenge, sspiToken,
                infrastructure::MakeSpn(m_proxyHost));
            if (r == infrastructure::SspiResult::Error) {
                Log("SSPI error after 407 challenge: " + std::string(resp_buf, 100));
                closesocket(client_sock);
                closesocket(proxy_sock);
                return;
            }
            if (sspiToken.empty()) {
                Log("CONNECT failed: SSPI не дал токен после 407");
                closesocket(client_sock);
                closesocket(proxy_sock);
                return;
            }
            authRetries++;
            if (authRetries > 5) {
                Log("CONNECT failed: SSPI retry limit exceeded");
                closesocket(client_sock);
                closesocket(proxy_sock);
                return;
            }
            Log("[SSPI] Retry CONNECT with new token (attempt " + std::to_string(authRetries) + ")");
            goto retry_connect;
        }
        else {
            Log("CONNECT failed: " + std::string(resp_buf, 100));
            closesocket(client_sock);
            closesocket(proxy_sock);
            return;
        }

        Log("[RELAY] " + std::string(ip_str) + ":" +
            std::to_string(dest_port) + " -> " + m_proxyHost + ":" +
            std::to_string(m_proxyPort));

        StartBridge(client_sock, proxy_sock);
    }

    void StartBridge(SOCKET client_sock, SOCKET proxy_sock) {
        auto* pair = new RelayPair();
        pair->sock_client = client_sock;
        pair->sock_proxy = proxy_sock;
        pair->refs = 2;

        auto* up_cfg = new OneWayConfig();
        up_cfg->pair = pair;
        up_cfg->from = client_sock;
        up_cfg->to = proxy_sock;

        auto* dn_cfg = new OneWayConfig();
        dn_cfg->pair = pair;
        dn_cfg->from = proxy_sock;
        dn_cfg->to = client_sock;

        HANDLE up_thread = CreateThread(nullptr, 0, &TcpRelayServer::OneWayRelayThunk,
                                         up_cfg, 0, nullptr);
        if (!up_thread) {
            delete up_cfg;
            delete dn_cfg;
            delete pair;
            closesocket(client_sock);
            closesocket(proxy_sock);
            return;
        }

        OneWayRelay(dn_cfg);

        WaitForSingleObject(up_thread, INFINITE);
        CloseHandle(up_thread);
    }

    struct OneWayConfig {
        RelayPair* pair;
        SOCKET from;
        SOCKET to;
    };

    static DWORD WINAPI OneWayRelayThunk(LPVOID arg) {
        OneWayRelay(static_cast<OneWayConfig*>(arg));
        return 0;
    }

    static void OneWayRelay(OneWayConfig* cfg) {
        RelayPair* pair = cfg->pair;
        SOCKET from = cfg->from;
        SOCKET to = cfg->to;
        delete cfg;

        char buf[131072];

        int len;
        while ((len = recv(from, buf, sizeof(buf), 0)) > 0) {
            int sent = 0;
            while (sent < len) {
                int n = send(to, buf + sent, len - sent, 0);
                if (n == SOCKET_ERROR) {
                    shutdown(pair->sock_client, SD_BOTH);
                    shutdown(pair->sock_proxy, SD_BOTH);
                    if (InterlockedDecrement(&pair->refs) == 0) {
                        closesocket(pair->sock_client);
                        closesocket(pair->sock_proxy);
                        delete pair;
                    }
                    return;
                }
                sent += n;
            }
        }

        shutdown(pair->sock_client, SD_BOTH);
        shutdown(pair->sock_proxy, SD_BOTH);

        if (InterlockedDecrement(&pair->refs) == 0) {
            closesocket(pair->sock_client);
            closesocket(pair->sock_proxy);
            delete pair;
        }
    }

    static std::string Base64Encode(const std::string& in) {
        static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.length() + 2) / 3) * 4);
        for (size_t i = 0; i < in.length(); i += 3) {
            unsigned int b = ((unsigned char)in[i]) << 16;
            if (i + 1 < in.length()) b |= ((unsigned char)in[i + 1]) << 8;
            if (i + 2 < in.length()) b |= (unsigned char)in[i + 2];
            out += chars[(b >> 18) & 0x3F];
            out += chars[(b >> 12) & 0x3F];
            out += (i + 1 < in.length()) ? chars[(b >> 6) & 0x3F] : '=';
            out += (i + 2 < in.length()) ? chars[b & 0x3F] : '=';
        }
        return out;
    }

    void Log(const std::string& msg) {
        printf("[%s] %s\n", "RELAY", msg.c_str());
        if (m_logCb) m_logCb(msg);
    }

    domain::ports::IConnectionTable& m_connTable;
    uint16_t m_relayPort;
    uint32_t m_proxyConfigId = 1;

    std::string m_proxyHost;
    uint16_t m_proxyPort = 8888;
    bool m_proxyAuthRequired = false;
    bool m_kerberosAuth = false;
    std::string m_proxyUser;
    std::string m_proxyPassword;

    SOCKET m_listenSock;
    SOCKET m_listenSock6;
    std::thread m_acceptThread;
    std::atomic<bool> m_running{false};

    RelayLogCallback m_logCb;
    RelayConnCallback m_connCb;
};

} // namespace infrastructure
} // namespace tcp_redirector