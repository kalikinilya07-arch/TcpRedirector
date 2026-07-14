#pragma once

//
// TcpIpcServer — замена PipeServer на TCP socket (127.0.0.1:34011).
// Протокол: JSON-строки разделённые '\n' (request/response).
// Потокобезопасность: m_clientSocket защищён мьютексом.
//

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>       // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <wincrypt.h>   // CryptGenRandom (token entropy)

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <fstream>
#include <nlohmann/json.hpp>

#include "../../domain/ports/IConnectionMonitor.h"

#pragma comment(lib, "advapi32.lib")

namespace tcp_redirector {
namespace infrastructure {

class TcpIpcServer : public domain::ports::IGUIIpc {
public:
    static constexpr uint16_t kDefaultPort = 34011;

    TcpIpcServer() noexcept
        : m_listenSocket(INVALID_SOCKET)
        , m_clientSocket(INVALID_SOCKET) {}

    void SetLogSink(domain::ports::ILogSink* sink) { m_logSink = sink; }
    void SetPort(uint16_t port) { m_port = port; }

    // B1 (QA audit): path where Start() writes the freshly generated per-run
    // auth token, restricted by DACL to Administrators + LOCAL_SYSTEM. The GUI
    // (which runs elevated) reads the token from here and echoes it in every
    // request. If the path is empty, or the token file cannot be created, the
    // server falls back to NOT enforcing auth (logged) so a benign filesystem
    // problem never bricks the only control channel.
    void SetAuthTokenFilePath(const std::wstring& path) { m_tokenFilePath = path; }

    ~TcpIpcServer() override { Stop(); }

    // ---- IGUIIpc ----

    bool Start() override {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            TcpLog(domain::LogLevel::Error, "WSAStartup failed");
            return false;
        }

        m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSocket == INVALID_SOCKET) {
            TcpLog(domain::LogLevel::Error, "socket() failed: " + std::to_string(WSAGetLastError()));
            WSACleanup();
            return false;
        }

        int opt = 1;
        setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (bind(m_listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            TcpLog(domain::LogLevel::Error, "bind() failed: " + std::to_string(WSAGetLastError()));
            closesocket(m_listenSocket);
            WSACleanup();
            return false;
        }

        if (listen(m_listenSocket, 1) == SOCKET_ERROR) {
            TcpLog(domain::LogLevel::Error, "listen() failed: " + std::to_string(WSAGetLastError()));
            closesocket(m_listenSocket);
            WSACleanup();
            return false;
        }

        // B1: establish the per-run auth token before accepting clients.
        EstablishAuthToken();

        m_running = true;
        m_thread = std::thread([this]() { ServerThread(); });
        TcpLog(domain::LogLevel::Info, "TCP IPC server started on 127.0.0.1:" + std::to_string(m_port));
        return true;
    }

    void Stop() override {
        if (!m_running) return;
        m_running = false;

        // Interrupt accept() by closing the listen socket
        if (m_listenSocket != INVALID_SOCKET) {
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
        }

        // Close client socket under lock
        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            if (m_clientSocket != INVALID_SOCKET) {
                closesocket(m_clientSocket);
                m_clientSocket = INVALID_SOCKET;
            }
        }

        if (m_thread.joinable()) {
            m_thread.join();
        }

        WSACleanup();
        TcpLog(domain::LogLevel::Info, "TCP IPC server stopped");
    }

    bool IsConnected() const override {
        return m_connected.load(std::memory_order_acquire);
    }

    // ---- Push-уведомления (не используются в request/response модели) ----

    void SendConnections(const std::vector<domain::ConnectionRecord>&) override {}
    void SendLog(const domain::LogEntry&) override {}
    void SendStats(const domain::ServiceStats&) override {}
    void SendConfig(const domain::ProxyConfig&) override {}
    void SendRules(const std::vector<domain::Rule>&) override {}

    void SetOnRequest(RequestCallback callback) override {
        m_requestCallback = std::move(callback);
    }

private:
    void TcpLog(domain::LogLevel level, const std::string& msg) {
        if (m_logSink) {
            m_logSink->Log(level, "tcp_ipc", msg);
        }
    }

    // B1: generate a 256-bit random token (hex) and write it to m_tokenFilePath
    // with a DACL that grants access only to Administrators + LOCAL_SYSTEM. On
    // any failure, leaves m_authToken empty (auth NOT enforced) and logs a
    // warning, so a filesystem/ACL problem can never brick the control channel.
    void EstablishAuthToken() {
        m_authToken.clear();
        if (m_tokenFilePath.empty()) {
            TcpLog(domain::LogLevel::Warn, "IPC auth disabled: no token path configured");
            return;
        }
        std::string token = GenerateTokenHex(32);
        if (token.empty()) {
            TcpLog(domain::LogLevel::Warn, "IPC auth disabled: token generation failed");
            return;
        }
        if (!WriteTokenFileSecured(m_tokenFilePath, token)) {
            TcpLog(domain::LogLevel::Warn, "IPC auth disabled: could not write secured token file");
            return;
        }
        m_authToken = token;
        TcpLog(domain::LogLevel::Info, "IPC auth token established (Administrators/SYSTEM only)");
    }

    static std::string GenerateTokenHex(size_t nbytes) {
        std::string hex;
        HCRYPTPROV prov = 0;
        if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL,
                                  CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            return hex;
        }
        std::vector<BYTE> buf(nbytes);
        BOOL ok = CryptGenRandom(prov, static_cast<DWORD>(nbytes), buf.data());
        CryptReleaseContext(prov, 0);
        if (!ok) return hex;
        static const char* kHex = "0123456789abcdef";
        hex.reserve(nbytes * 2);
        for (BYTE b : buf) { hex.push_back(kHex[b >> 4]); hex.push_back(kHex[b & 0x0F]); }
        return hex;
    }

    // Writes the token with a protected DACL: only Administrators + LOCAL_SYSTEM
    // may read it. A non-admin local process therefore cannot learn the token
    // and cannot drive the service over IPC.
    static bool WriteTokenFileSecured(const std::wstring& path, const std::string& token) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;FA;;;BA)(A;;FA;;;SY)", SDDL_REVISION_1, &sd, nullptr)) {
            return false;
        }
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        sa.lpSecurityDescriptor = sd;

        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &sa,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        LocalFree(sd);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(h, token.data(), static_cast<DWORD>(token.size()), &written, nullptr);
        CloseHandle(h);
        return ok && written == token.size();
    }

    void ServerThread() {
        while (m_running.load(std::memory_order_relaxed)) {
            // Blocking accept
            SOCKET client = accept(m_listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (m_running.load(std::memory_order_relaxed))
                    TcpLog(domain::LogLevel::Error, "accept() failed: " + std::to_string(WSAGetLastError()));
                break;
            }

            // Set recv timeout so we don't hang on disconnected clients
            DWORD timeout = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                m_clientSocket = client;
            }
            m_connected.store(true, std::memory_order_release);
            TcpLog(domain::LogLevel::Debug, "GUI client connected via TCP");

            // ---- Client I/O loop ----
            char buf[65536];
            std::string accum;

            while (m_running.load(std::memory_order_relaxed)) {
                int n = recv(client, buf, (int)sizeof(buf) - 1, 0);
                if (n <= 0) {
                    if (n < 0 && m_running.load(std::memory_order_relaxed))
                        TcpLog(domain::LogLevel::Debug, "recv() error: " + std::to_string(WSAGetLastError()));
                    break;  // client disconnected or error
                }

                buf[n] = '\0';
                accum.append(buf, n);

                // Process complete lines (delimited by '\n')
                size_t pos;
                while ((pos = accum.find('\n')) != std::string::npos) {
                    std::string line = accum.substr(0, pos);
                    accum.erase(0, pos + 1);

                    // Trim trailing '\r'
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();

                    if (line.empty()) continue;

                    try {
                        auto j = nlohmann::json::parse(line);
                        std::string method = j.value("method", "");
                        std::string params = j.value("params", "");

                        std::string response;
                        // B1: reject unauthenticated callers. Enforced only when a
                        // token was successfully established (see EstablishAuthToken).
                        if (!m_authToken.empty() &&
                            j.value("token", std::string()) != m_authToken) {
                            response = R"({"status":"error","error":"unauthorized"})";
                        } else if (m_requestCallback) {
                            m_requestCallback(method, params, response);
                        } else {
                            response = R"({"status":"error","error":"not_ready"})";
                        }

                        response += '\n';
                        send(client, response.data(), (int)response.size(), 0);
                    }
                    catch (const std::exception& e) {
                        std::string err = R"({"status":"error","error":"parse_error","detail":")" + std::string(e.what()) + R"("})";
                        err += '\n';
                        send(client, err.data(), (int)err.size(), 0);
                    }
                }
            }

            // Client disconnected
            m_connected.store(false, std::memory_order_release);
            TcpLog(domain::LogLevel::Debug, "GUI client disconnected");

            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                if (m_clientSocket == client) {
                    m_clientSocket = INVALID_SOCKET;
                }
            }
            closesocket(client);
        }
    }

    domain::ports::ILogSink* m_logSink = nullptr;
    RequestCallback m_requestCallback;
    uint16_t m_port = kDefaultPort;

    // B1: IPC authentication token (hex). Empty = auth not enforced.
    std::wstring m_tokenFilePath;
    std::string  m_authToken;

    SOCKET m_listenSocket;
    SOCKET m_clientSocket;
    std::mutex m_socketMutex;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
};

} // namespace infrastructure
} // namespace tcp_redirector
