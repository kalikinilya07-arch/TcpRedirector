#pragma once

#include "../../domain/ports/IAuthenticationProvider.h"
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <string>
#include <string_view>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace tcp_redirector {
namespace infrastructure {

/// Провайдер Kerberos-аутентификации через AuthAgent.exe.
///
/// Подключается к AuthAgent через Named Pipe (\\.\pipe\TcpRedirectorAuth).
/// Выполняет version negotiation (hello), keepalive (ping каждые 15с),
/// автоматический reconnect при обрыве pipe.
///
/// ContextId живёт ровно один HTTP CONNECT.
/// После CloseContext() или ошибки — повторное использование запрещено.
class KerberosAgentProvider : public domain::ports::IAuthenticationProvider {
public:
    KerberosAgentProvider();
    ~KerberosAgentProvider() override;

    // ---- IAuthenticationProvider ----

    domain::ports::CreateContextResult CreateContext(
        std::string_view spn) override;

    domain::ports::ContinueContextResult ContinueContext(
        uint64_t context_id,
        std::string_view challenge) override;

    void CloseContext(uint64_t context_id) override;
    void Reset() override;
    domain::ports::AuthProviderType GetType() const override {
        return domain::ports::AuthProviderType::KerberosAgent;
    }

private:
    static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\TcpRedirectorAuth";
    static constexpr auto kKeepaliveInterval = std::chrono::seconds(15);
    static constexpr auto kReconnectDelay = std::chrono::seconds(2);
    static constexpr int kPipeTimeout = 5000;  // ms

    // ---- Pipe management ----
    bool ConnectToAgent();
    void DisconnectFromAgent();
    void KeepaliveLoop();
    void EnsureConnected();

    // ---- AuthAgent auto-launch ----
    static bool LaunchAuthAgentInUserSession();
    static std::atomic<bool> s_launchInProgress;

    // ---- JSON-RPC helpers ----
    nlohmann::json Call(const std::string& method,
                        const nlohmann::json& params = nullptr);
    std::string ReadMessage(HANDLE pipe);
    void SendMessage(HANDLE pipe, const std::string& msg);

    // ---- State ----
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::mutex m_pipeMutex;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{true};
    std::atomic<int> m_version{0};  // согласованная версия протокола

    // Keepalive thread
    std::thread m_keepaliveThread;

    // Последовательный ID для запросов
    std::atomic<uint64_t> m_nextId{1};
};

} // namespace infrastructure
} // namespace tcp_redirector