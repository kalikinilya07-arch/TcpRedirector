#include "KerberosAgentProvider.h"
#include <sstream>
#include <cstdio>

namespace tcp_redirector {
namespace infrastructure {

KerberosAgentProvider::KerberosAgentProvider() {
    // Запускаем keepalive-поток
    m_keepaliveThread = std::thread([this]() { KeepaliveLoop(); });
}

KerberosAgentProvider::~KerberosAgentProvider() {
    m_running = false;
    if (m_keepaliveThread.joinable()) {
        m_keepaliveThread.join();
    }
    DisconnectFromAgent();
}

// ============================================================================
// IAuthenticationProvider
// ============================================================================

domain::ports::CreateContextResult KerberosAgentProvider::CreateContext(
    std::string_view spn) {

    domain::ports::CreateContextResult result{};
    result.success = false;

    EnsureConnected();
    if (!m_connected.load(std::memory_order_acquire)) {
        result.error_message = "AuthAgent not available";
        return result;
    }

    try {
        nlohmann::json params;
        params["spn"] = std::string(spn);

        auto resp = Call("create_context", params);

        if (resp.contains("error")) {
            result.error_message = resp["error"]["message"].get<std::string>();
            return result;
        }

        auto& r = resp["result"];
        result.success = true;
        result.context_id = r["context_id"].get<uint64_t>();
        result.token = r["token"].get<std::string>();
        result.needs_continue = r.value("continue", false);

    } catch (const std::exception& e) {
        result.error_message = std::string("create_context failed: ") + e.what();
    }

    return result;
}

domain::ports::ContinueContextResult KerberosAgentProvider::ContinueContext(
    uint64_t context_id,
    std::string_view challenge) {

    domain::ports::ContinueContextResult result{};
    result.success = false;

    EnsureConnected();
    if (!m_connected.load(std::memory_order_acquire)) {
        result.error_message = "AuthAgent not available";
        return result;
    }

    try {
        nlohmann::json params;
        params["context_id"] = context_id;
        params["challenge"] = std::string(challenge);

        auto resp = Call("continue_context", params);

        if (resp.contains("error")) {
            result.error_message = resp["error"]["message"].get<std::string>();
            return result;
        }

        auto& r = resp["result"];
        result.success = true;
        result.token = r["token"].get<std::string>();
        result.needs_continue = r.value("continue", false);

    } catch (const std::exception& e) {
        result.error_message = std::string("continue_context failed: ") + e.what();
    }

    return result;
}

void KerberosAgentProvider::CloseContext(uint64_t context_id) {
    EnsureConnected();
    if (!m_connected.load(std::memory_order_acquire)) {
        return;  // Agent недоступен — контекст всё равно умрёт по TTL
    }

    try {
        nlohmann::json params;
        params["context_id"] = context_id;
        Call("close_context", params);
    } catch (...) {
        // Игнорируем ошибки закрытия — контекст умрёт по TTL в AuthAgent
    }
}

void KerberosAgentProvider::Reset() {
    // Все контексты будут закрыты при переподключении
    // (AuthAgent очищает их при разрыве pipe)
    DisconnectFromAgent();
}

// ============================================================================
// Pipe management
// ============================================================================

bool KerberosAgentProvider::ConnectToAgent() {
    std::lock_guard<std::mutex> lock(m_pipeMutex);

    if (m_pipe != INVALID_HANDLE_VALUE) {
        return true;  // Уже подключены
    }

    // Ожидаем доступности pipe (до 5 секунд)
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!WaitNamedPipeW(kPipeName, kPipeTimeout)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) {
                continue;  // Pipe существует, но занят — ждём
            }
            // Pipe не существует — ждём и пробуем снова
            Sleep(500);
            continue;
        }
        break;
    }

    HANDLE pipe = CreateFileW(
        kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,              // No sharing
        nullptr,        // Default security
        OPEN_EXISTING,
        0,              // Default attributes
        nullptr);       // No template

    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Установить режим сообщений
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        CloseHandle(pipe);
        return false;
    }

    m_pipe = pipe;

    // Version negotiation
    try {
        nlohmann::json helloParams;
        helloParams["version"] = 1;
        auto resp = Call("hello", helloParams);

        if (resp.contains("result") && resp["result"].contains("version")) {
            m_version.store(resp["result"]["version"].get<int>(),
                           std::memory_order_release);
            m_connected.store(true, std::memory_order_release);
            return true;
        }
    } catch (...) {
        // Version negotiation failed
    }

    CloseHandle(m_pipe);
    m_pipe = INVALID_HANDLE_VALUE;
    return false;
}

void KerberosAgentProvider::DisconnectFromAgent() {
    std::lock_guard<std::mutex> lock(m_pipeMutex);
    m_connected.store(false, std::memory_order_release);
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

void KerberosAgentProvider::KeepaliveLoop() {
    while (m_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(kKeepaliveInterval);

        if (!m_connected.load(std::memory_order_acquire)) {
            // Попытка переподключения
            ConnectToAgent();
            continue;
        }

        // Ping
        try {
            auto resp = Call("ping");
            if (resp.contains("error")) {
                // Ошибка — переподключаемся
                DisconnectFromAgent();
            }
        } catch (...) {
            DisconnectFromAgent();
        }
    }
}

void KerberosAgentProvider::EnsureConnected() {
    if (!m_connected.load(std::memory_order_acquire)) {
        ConnectToAgent();
    }
}

// ============================================================================
// JSON-RPC helpers
// ============================================================================

nlohmann::json KerberosAgentProvider::Call(
    const std::string& method,
    const nlohmann::json& params) {

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    req["id"] = m_nextId.fetch_add(1, std::memory_order_relaxed);
    if (!params.is_null()) {
        req["params"] = params;
    }

    std::string reqStr = req.dump();
    SendMessage(m_pipe, reqStr);
    std::string respStr = ReadMessage(m_pipe);

    return nlohmann::json::parse(respStr);
}

void KerberosAgentProvider::SendMessage(HANDLE pipe, const std::string& msg) {
    DWORD written = 0;
    if (!WriteFile(pipe, msg.c_str(),
                   static_cast<DWORD>(msg.length()),
                   &written, nullptr)) {
        throw std::runtime_error("WriteFile failed: " +
                                 std::to_string(GetLastError()));
    }
    // Flush to ensure message boundary
    FlushFileBuffers(pipe);
}

std::string KerberosAgentProvider::ReadMessage(HANDLE pipe) {
    char buf[65536];
    DWORD bytesRead = 0;

    if (!ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            DisconnectFromAgent();
        }
        throw std::runtime_error("ReadFile failed: " + std::to_string(err));
    }

    buf[bytesRead] = '\0';
    return std::string(buf, bytesRead);
}

} // namespace infrastructure
} // namespace tcp_redirector