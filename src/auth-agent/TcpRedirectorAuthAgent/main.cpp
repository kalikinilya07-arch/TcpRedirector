#include <windows.h>
#include <sddl.h>
#include <string>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "SspiEngine.h"
#include "ContextStore.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "secur32.lib")

using namespace tcp_redirector::auth_agent;

// ============================================================================
// File logger — writes to %ProgramData%\TcpRedirector\logs\auth_agent.log
// ============================================================================

static std::ofstream g_logFile;
static std::mutex g_logMutex;

void AgentLog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    
    // Timestamp
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_s(&tm_info, &now);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_info);
    
    // Format message
    char msgBuf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);
    
    // Write to file and stderr
    if (g_logFile.is_open()) {
        g_logFile << "[" << timeBuf << "] " << msgBuf << std::endl;
        g_logFile.flush();
    }
    fprintf(stderr, "[%s] %s\n", timeBuf, msgBuf);
    fflush(stderr);
}

static void InitLogFile() {
    wchar_t progData[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH) > 0) {
        std::filesystem::path logDir = std::filesystem::path(progData) / L"TcpRedirector" / L"logs";
        std::filesystem::create_directories(logDir);
        std::filesystem::path logPath = logDir / L"auth_agent.log";
        g_logFile.open(logPath, std::ios::app);
        if (g_logFile.is_open()) {
            AgentLog("=== AuthAgent started (PID=%lu) ===", GetCurrentProcessId());
        }
    }
}

// ============================================================================
// Named Pipe Server для AuthAgent
// ============================================================================

static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\TcpRedirectorAuth";
static constexpr int kPipeTimeout = 5000;

static SspiEngine g_engine;
static ContextStore g_store;
static std::atomic<bool> g_running{true};

// Создать SECURITY_ATTRIBUTES с ACL Everyone (Read/Write)
static SECURITY_ATTRIBUTES MakeEveryoneSA(PSECURITY_DESCRIPTOR* outSd) {
    static SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;

    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GRGW;;;WD)",  // Everyone: Generic Read + Generic Write
            SDDL_REVISION_1,
            &raw,
            nullptr)) {
        *outSd = nullptr;
        return sa;
    }

    *outSd = raw;
    sa.lpSecurityDescriptor = raw;
    return sa;
}

// Чтение JSON-сообщения из pipe
static std::string ReadMessage(HANDLE pipe) {
    char buf[65536];
    DWORD bytesRead = 0;

    if (!ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
        throw std::runtime_error("ReadFile failed: " +
                                 std::to_string(GetLastError()));
    }

    buf[bytesRead] = '\0';
    return std::string(buf, bytesRead);
}

// Отправка JSON-сообщения в pipe
static void SendMessage(HANDLE pipe, const std::string& msg) {
    DWORD written = 0;
    if (!WriteFile(pipe, msg.c_str(),
                   static_cast<DWORD>(msg.length()),
                   &written, nullptr)) {
        throw std::runtime_error("WriteFile failed: " +
                                 std::to_string(GetLastError()));
    }
    FlushFileBuffers(pipe);
}

// Обработка одного JSON-RPC запроса
static nlohmann::json HandleRequest(const nlohmann::json& req) {
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    // Fix: req.value("id", nullptr) throws type_error.302 when id is numeric.
    // Use contains() check instead to handle both numeric and null ids.
    resp["id"] = req.contains("id") ? req["id"] : nlohmann::json(nullptr);

    std::string method = req.value("method", "");

    AgentLog("REQ method=%s id=%s", method.c_str(),
             req.contains("id") ? req["id"].dump().c_str() : "null");

    try {
        if (method == "hello") {
            int clientVersion = req["params"].value("version", 0);
            nlohmann::json result;
            result["version"] = 1;  // Поддерживаемая версия протокола
            result["agent"] = "TcpRedirectorAuthAgent/1.1.0";
            // Report the user account under which the agent runs (Kerberos context)
            wchar_t userName[256] = {0};
            DWORD userNameLen = 256;
            if (GetUserNameW(userName, &userNameLen)) {
                char userNameUtf8[512];
                WideCharToMultiByte(CP_UTF8, 0, userName, -1,
                                    userNameUtf8, sizeof(userNameUtf8), nullptr, nullptr);
                result["user"] = userNameUtf8;
                AgentLog("  hello: agent user=%s", userNameUtf8);
            }
            resp["result"] = result;
        }
        else if (method == "ping") {
            nlohmann::json result;
            result["pong"] = true;
            resp["result"] = result;
        }
        else if (method == "create_context") {
            std::string spn = req["params"]["spn"].get<std::string>();
            AgentLog("  create_context: SPN=%s", spn.c_str());

            CredHandle credentials;
            CtxtHandle context;
            SecInvalidateHandle(&credentials);
            SecInvalidateHandle(&context);

            auto stepResult = g_engine.CreateContext(spn, credentials, context);

            if (!stepResult.success) {
                nlohmann::json error;
                error["code"] = -1;
                error["message"] = stepResult.error_message;
                resp["error"] = error;
            } else {
                uint64_t contextId = g_store.Create(
                    credentials, context, spn);

                nlohmann::json result;
                result["context_id"] = contextId;
                result["token"] = stepResult.token;
                result["continue"] = stepResult.needs_continue;
                resp["result"] = result;
            }
        }
        else if (method == "continue_context") {
            uint64_t contextId = req["params"]["context_id"].get<uint64_t>();
            std::string challenge = req["params"]["challenge"].get<std::string>();

            auto* entry = g_store.Get(contextId);
            if (!entry) {
                nlohmann::json error;
                error["code"] = -2;
                error["message"] = "Context not found or expired (id=" +
                                    std::to_string(contextId) + ")";
                resp["error"] = error;
            } else {
                auto stepResult = g_engine.ContinueContext(
                    entry->context, challenge);

                if (!stepResult.success) {
                    g_store.Close(contextId);
                    nlohmann::json error;
                    error["code"] = -1;
                    error["message"] = stepResult.error_message;
                    resp["error"] = error;
                } else {
                    nlohmann::json result;
                    result["token"] = stepResult.token;
                    result["continue"] = stepResult.needs_continue;
                    resp["result"] = result;

                    // Если аутентификация завершена — закрываем контекст
                    if (!stepResult.needs_continue) {
                        g_store.Close(contextId);
                    }
                }
            }
        }
        else if (method == "close_context") {
            uint64_t contextId = req["params"]["context_id"].get<uint64_t>();
            g_store.Close(contextId);

            nlohmann::json result;
            result["closed"] = true;
            resp["result"] = result;
        }
        else {
            nlohmann::json error;
            error["code"] = -32601;
            error["message"] = "Method not found: " + method;
            resp["error"] = error;
        }
    } catch (const std::exception& e) {
        AgentLog("  ERROR in %s: %s", method.c_str(), e.what());
        nlohmann::json error;
        error["code"] = -32603;
        error["message"] = std::string("Internal error: ") + e.what();
        resp["error"] = error;
    }

    AgentLog("RESP method=%s success=%s", method.c_str(),
             resp.contains("result") ? "true" : "false");
    return resp;
}

// Основной цикл обработки одного клиента (Service)
static void HandleClient(HANDLE pipe) {
    // Установить режим сообщений
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    while (g_running.load(std::memory_order_relaxed)) {
        try {
            std::string reqStr = ReadMessage(pipe);
            auto req = nlohmann::json::parse(reqStr);
            auto resp = HandleRequest(req);
            std::string respStr = resp.dump();
            SendMessage(pipe, respStr);
        } catch (const std::exception& e) {
            // Ошибка чтения/записи — клиент отключился
            AgentLog("Client disconnected: %s", e.what());
            break;
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // Отключить буферизацию stdout/stderr
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    // Init file logger FIRST
    InitLogFile();

    wchar_t userName[256] = {0};
    DWORD userNameLen = 256;
    if (GetUserNameW(userName, &userNameLen)) {
        AgentLog("TcpRedirectorAuthAgent v1.1.9 starting as user: %S", userName);
    } else {
        AgentLog("TcpRedirectorAuthAgent v1.1.9 starting as user: <unknown>");
    }

    // Периодическая очистка просроченных контекстов (каждые 5 минут)
    std::thread cleanupThread([]() {
        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::minutes(5));
            g_store.CleanupExpired();
        }
    });

    // Основной цикл: создаём pipe, ждём подключения, обрабатываем
    while (g_running.load(std::memory_order_relaxed)) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        SECURITY_ATTRIBUTES sa = MakeEveryoneSA(&sd);

        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536, 65536,
            kPipeTimeout,
            &sa);

        if (sd) {
            LocalFree(sd);
        }

        if (pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "[AuthAgent] CreateNamedPipe failed: %lu\n",
                    GetLastError());
            Sleep(1000);
            continue;
        }

        printf("[AuthAgent] Waiting for service connection...\n");

        if (ConnectNamedPipe(pipe, nullptr) ||
            GetLastError() == ERROR_PIPE_CONNECTED) {
            printf("[AuthAgent] Service connected\n");
            HandleClient(pipe);
        }

        CloseHandle(pipe);
        printf("[AuthAgent] Pipe closed, waiting for next connection...\n");
    }

    g_running = false;
    if (cleanupThread.joinable()) {
        cleanupThread.join();
    }

    g_store.Clear();
    printf("[AuthAgent] Shutdown complete\n");
    return 0;
}