#include "KerberosAgentProvider.h"
#include <sstream>
#include <cstdio>

namespace tcp_redirector {
namespace infrastructure {

// Static members
std::atomic<bool> KerberosAgentProvider::s_launchInProgress{false};
std::atomic<int64_t> KerberosAgentProvider::s_lastLaunchTime{0};
std::function<void(const std::string&)> KerberosAgentProvider::s_logFn;

// Helper: log via static callback if set
static void StaticLog(const std::string& msg) {
    if (KerberosAgentProvider::s_logFn) {
        KerberosAgentProvider::s_logFn(msg);
    }
}

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
    std::lock_guard<std::recursive_mutex> lock(m_pipeMutex);

    if (m_pipe != INVALID_HANDLE_VALUE) {
        return true;  // Уже подключены
    }

    // Ожидаем доступности pipe (до 5 секунд)
    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!WaitNamedPipeW(kPipeName, kPipeTimeout)) {
            lastErr = GetLastError();
            if (lastErr == ERROR_SEM_TIMEOUT) {
                StaticLog("[ConnectToAgent] attempt " + std::to_string(attempt) + ": pipe exists but busy, retrying...");
                continue;  // Pipe существует, но занят — ждём
            }
            // Pipe не существует — ждём и пробуем снова
            StaticLog("[ConnectToAgent] attempt " + std::to_string(attempt) + ": pipe not found (err=" + std::to_string(lastErr) + "), sleeping 500ms...");
            Sleep(500);
            continue;
        }
        StaticLog("[ConnectToAgent] attempt " + std::to_string(attempt) + ": WaitNamedPipeW OK");
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
        DWORD err = GetLastError();
        StaticLog("[ConnectToAgent] CreateFileW failed: err=" + std::to_string(err));
        return false;
    }
    StaticLog("[ConnectToAgent] CreateFileW OK");

    // Установить режим сообщений
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        StaticLog("[ConnectToAgent] SetNamedPipeHandleState failed: err=" + std::to_string(GetLastError()));
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
            StaticLog("[ConnectToAgent] version negotiation OK, connected");
            return true;
        }
        StaticLog("[ConnectToAgent] version negotiation: unexpected response");
    } catch (const std::exception& e) {
        StaticLog("[ConnectToAgent] version negotiation exception: " + std::string(e.what()));
    } catch (...) {
        StaticLog("[ConnectToAgent] version negotiation: unknown exception");
    }

    CloseHandle(m_pipe);
    m_pipe = INVALID_HANDLE_VALUE;
    return false;
}

void KerberosAgentProvider::DisconnectFromAgent() {
    std::lock_guard<std::recursive_mutex> lock(m_pipeMutex);
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
            if (!ConnectToAgent()) {
                // Launch AuthAgent and poll for pipe readiness
                LaunchAuthAgentInUserSession();
                for (int attempt = 0; attempt < 20; ++attempt) {
                    Sleep(500);
                    if (ConnectToAgent()) {
                        break;  // Connected!
                    }
                }
            }
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
    if (m_connected.load(std::memory_order_acquire)) {
        return;  // Already connected
    }

    StaticLog("[EnsureConnected] not connected, trying ConnectToAgent...");
    if (ConnectToAgent()) {
        StaticLog("[EnsureConnected] ConnectToAgent succeeded immediately");
        return;  // Connected successfully
    }

    // Launch AuthAgent and poll for pipe readiness (up to 10 seconds)
    StaticLog("[EnsureConnected] launching AuthAgent...");
    LaunchAuthAgentInUserSession();

    for (int attempt = 0; attempt < 20; ++attempt) {
        Sleep(500);
        StaticLog("[EnsureConnected] poll attempt " + std::to_string(attempt + 1) + "/20...");
        if (ConnectToAgent()) {
            StaticLog("[EnsureConnected] connected on poll attempt " + std::to_string(attempt + 1));
            return;  // Connected!
        }
    }
    // After 10 seconds of retries, give up — m_connected stays false
    StaticLog("[EnsureConnected] FAILED after 20 poll attempts (10 seconds)");
}

// ============================================================================
// AuthAgent auto-launch (WTS API) — v1.1.1 with full diagnostics
// ============================================================================

bool KerberosAgentProvider::LaunchAuthAgentInUserSession() {
    // Cooldown: don't launch more than once every 10 seconds
    {
        auto now = std::chrono::steady_clock::now();
        int64_t nowNs = now.time_since_epoch().count();
        int64_t lastNs = s_lastLaunchTime.load(std::memory_order_relaxed);
        if (lastNs > 0 && (nowNs - lastNs) < 10'000'000'000LL) {  // 10 seconds in ns
            return false;  // Cooldown active — skip silently
        }
    }

    // Rate-limit: only one launch attempt at a time
    bool expected = false;
    if (!s_launchInProgress.compare_exchange_strong(expected, true,
                                                     std::memory_order_acquire)) {
        return false;
    }

    bool result = false;
    HANDLE userToken = nullptr;
    HANDLE dupToken = nullptr;
    LPVOID envBlock = nullptr;

    // Step 1: Get active console session
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        StaticLog("[AuthAgent] launch failed: no active console session (WTSGetActiveConsoleSessionId returned 0xFFFFFFFF)");
        goto cleanup;
    }
    StaticLog("[AuthAgent] active console session: " + std::to_string(sessionId));

    // Step 2: Query user token
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        DWORD err = GetLastError();
        StaticLog("[AuthAgent] launch failed: WTSQueryUserToken error " + std::to_string(err));
        goto cleanup;
    }

    // Step 3: Duplicate token with minimal required rights
    // (was TOKEN_ALL_ACCESS — too broad, may fail on restricted tokens)
    if (!DuplicateTokenEx(userToken,
                          TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY,
                          nullptr, SecurityImpersonation, TokenPrimary, &dupToken)) {
        DWORD err = GetLastError();
        StaticLog("[AuthAgent] launch failed: DuplicateTokenEx error " + std::to_string(err));
        goto cleanup;
    }

    // Step 4: Create environment block (non-fatal if fails)
    if (!CreateEnvironmentBlock(&envBlock, dupToken, FALSE)) {
        DWORD err = GetLastError();
        StaticLog("[AuthAgent] CreateEnvironmentBlock failed (non-fatal): " + std::to_string(err));
        envBlock = nullptr;  // proceed without custom environment
    }

    // Step 5: Determine agent path dynamically (relative to service executable)
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dir(exePath);
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            dir = dir.substr(0, pos + 1);
        }
        std::wstring agentPath = dir + L"TcpRedirectorAuthAgent.exe";

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");

        PROCESS_INFORMATION pi = {};

        if (CreateProcessAsUserW(
                dupToken,
                nullptr,           // app name (use cmdLine)
                agentPath.data(),  // command line
                nullptr,           // process attributes
                nullptr,           // thread attributes
                FALSE,             // inherit handles
                CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS,
                envBlock,          // environment
                dir.c_str(),       // working directory
                &si,
                &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            result = true;

            // Convert wide path to UTF-8 for logging
            int len = WideCharToMultiByte(CP_UTF8, 0, agentPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string agentPathUtf8(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, agentPath.c_str(), -1, &agentPathUtf8[0], len, nullptr, nullptr);
            while (!agentPathUtf8.empty() && agentPathUtf8.back() == '\0') agentPathUtf8.pop_back();

            StaticLog("[AuthAgent] launched successfully in session " + std::to_string(sessionId) +
                      ": " + agentPathUtf8);
        } else {
            DWORD err = GetLastError();
            StaticLog("[AuthAgent] launch failed: CreateProcessAsUserW error " + std::to_string(err));
        }
    }

cleanup:
    if (envBlock) {
        DestroyEnvironmentBlock(envBlock);
    }
    if (dupToken) {
        CloseHandle(dupToken);
    }
    if (userToken) {
        CloseHandle(userToken);
    }

    s_launchInProgress.store(false, std::memory_order_release);
    if (result) {
        s_lastLaunchTime.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
    }
    return result;
}

// ============================================================================
// JSON-RPC helpers
// ============================================================================

nlohmann::json KerberosAgentProvider::Call(
    const std::string& method,
    const nlohmann::json& params) {

    // Serialize all pipe I/O: multiple ConnectionHandler threads may call
    // CreateContext/ContinueContext concurrently, and without this lock their
    // SendMessage/ReadMessage pairs would interleave on the shared pipe,
    // causing JSON parse errors and hung threads.
    std::lock_guard<std::recursive_mutex> lock(m_pipeMutex);

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
    // Message-mode pipe ensures message boundary; FlushFileBuffers is unnecessary
    // and adds latency to every IPC call (create_context, continue_context).
}

std::string KerberosAgentProvider::ReadMessage(HANDLE pipe) {
    char buf[65536];
    DWORD bytesRead = 0;
    if (!ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
        DWORD err = GetLastError();
        // NOTE: Do NOT call DisconnectFromAgent() here — it takes m_pipeMutex
        // which may already be held by ConnectToAgent(). The caller is
        // responsible for cleanup (ConnectToAgent closes m_pipe on failure).
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            m_connected.store(false, std::memory_order_release);
        }
        throw std::runtime_error("ReadFile failed: " + std::to_string(err));
    }
    buf[bytesRead] = '\0';
    return std::string(buf, bytesRead);
}

} // namespace infrastructure
} // namespace tcp_redirector