#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <sddl.h>
#include "../../domain/ports/IConnectionMonitor.h"

#pragma comment(lib, "advapi32.lib")

namespace tcp_redirector {
namespace infrastructure {

//
// RAII deleter for SECURITY_DESCRIPTOR allocated by
// ConvertStringSecurityDescriptorToSecurityDescriptorW (LocalAlloc).
//
struct LocalFreeDeleter {
    void operator()(PSECURITY_DESCRIPTOR p) const noexcept {
        if (p) ::LocalFree(p);
    }
};
using SdPtr = std::unique_ptr<std::remove_pointer_t<PSECURITY_DESCRIPTOR>, LocalFreeDeleter>;

//
// Named-pipe server that handles exactly ONE GUI client at a time.
//   - Security: only BUILTIN\Administrators and LOCAL_SYSTEM may connect.
//   - Re-entrant: after a client disconnects, a new pipe instance is created
//     automatically so the GUI can reconnect.
//   - Thread-safe: m_hPipe is protected by a mutex; all Send*() methods
//     are safe to call from any thread (e.g. the capture or logger thread).
//
class PipeServer : public domain::ports::IGUIIpc {
public:
    PipeServer() noexcept : m_hPipe(INVALID_HANDLE_VALUE) {}

    void SetLogSink(domain::ports::ILogSink* sink) { m_logSink = sink; }

    ~PipeServer() override {
        Stop();
    }

    // ---- IGUIIpc ----

    bool Start() override {
        m_running = true;
        m_pipeReady = false;
        m_thread = std::thread([this]() { PipeThread(); });
        return true;
    }

    /// Wait until the pipe is created and ready to accept connections.
    bool WaitForPipe(std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!m_pipeReady.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    }

    void Stop() override {
        m_running = false;

        // Cancel any pending I/O on the pipe handle so PipeThread can exit.
        HANDLE pipeToCancel;
        {
            std::lock_guard<std::mutex> lock(m_pipeMutex);
            pipeToCancel = m_hPipe;
        }
        if (pipeToCancel != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipeToCancel, nullptr);
        }

        if (m_thread.joinable()) {
            m_thread.join();
        }

        // Close the handle under the mutex.
        std::lock_guard<std::mutex> lock(m_pipeMutex);
        if (m_hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hPipe);
            m_hPipe = INVALID_HANDLE_VALUE;
        }
    }

    bool IsConnected() const override {
        return m_connected.load(std::memory_order_acquire);
    }

    void SendConnections(const std::vector<domain::ConnectionRecord>& connections) override {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& c : connections) {
            j.push_back({
                {"id", c.id},
                {"pid", c.pid},
                {"process_name", std::string(c.process_name.begin(), c.process_name.end())},
                {"destination_host", std::string(c.destination_host.begin(), c.destination_host.end())},
                {"destination_ip", c.destination_ip},
                {"destination_port", c.destination_port},
                {"rx_bytes", c.rx_bytes},
                {"tx_bytes", c.tx_bytes},
                {"state", static_cast<int>(c.state)},
                {"duration_ms", c.duration.count()}
            });
        }
        SendMessage("push", "connections", j.dump());
    }

    void SendLog(const domain::LogEntry& entry) override {
        nlohmann::json j = {
            {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.timestamp.time_since_epoch()).count()},
            {"level", static_cast<int>(entry.level)},
            {"logger", entry.logger},
            {"message", entry.message}
        };
        SendMessage("push", "log", j.dump());
    }

    void SendStats(const domain::ServiceStats& stats) override {
        nlohmann::json j = {
            {"active_connections", stats.active_connections},
            {"total_rx_bytes", stats.total_rx_bytes},
            {"total_tx_bytes", stats.total_tx_bytes},
            {"proxy_errors", stats.proxy_errors}
        };
        SendMessage("push", "stats", j.dump());
    }

    void SendConfig(const domain::ProxyConfig& /*config*/) override {}
    void SendRules(const std::vector<domain::Rule>& /*rules*/) override {}

    void SetOnRequest(RequestCallback callback) override {
        m_requestCallback = std::move(callback);
    }

private:
    // ---- SECURITY ----------------------------------------------------------
    // Create a SECURITY_ATTRIBUTES that only allows Administrators and
    // LOCAL_SYSTEM to access the named pipe.
    // SDDL: D:(A;;GA;;;BA)(A;;GA;;;SY)
    //   BA = Built-in Administrators, SY = Local System, GA = GENERIC_ALL
    // The returned SdPtr owns the memory (LocalFree on destruction).
    static std::pair<SECURITY_ATTRIBUTES, SdPtr> MakeAdminOnlySA() noexcept {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;

        PSECURITY_DESCRIPTOR raw = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;BA)(A;;GA;;;SY)",
                SDDL_REVISION_1,
                &raw,
                nullptr)) {
            return {sa, nullptr};
        }

        SdPtr sd(raw);
        sa.lpSecurityDescriptor = sd.get();
        return {sa, std::move(sd)};
    }

    // ---- Pipe thread (runs for the lifetime of the server) -----------------
    void PipeThread() {
        bool isFirstInstance = true;

        while (m_running.load(std::memory_order_relaxed)) {
            auto [secAttr, sdOwner] = MakeAdminOnlySA();
            DWORD pipeFlags = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT;
            if (isFirstInstance) {
                pipeFlags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
            }

            HANDLE newPipe = CreateNamedPipeW(
                L"\\\\.\\pipe\\TcpRedirectorService",
                PIPE_ACCESS_DUPLEX,
                pipeFlags,
                PIPE_UNLIMITED_INSTANCES,
                65536, 65536,
                5000,
                secAttr.lpSecurityDescriptor ? &secAttr : nullptr);

            // sdOwner is kept alive until after CreateNamedPipeW returns
            // (the SECURITY_DESCRIPTOR must remain valid for the call).
            // It will be destroyed by the unique_ptr at the end of this scope.

            if (newPipe == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                IpcLog(domain::LogLevel::Warn,
                    "CreateNamedPipe failed: " + std::to_string(err));
                // Clear FIRST_PIPE_INSTANCE on ANY error — retrying with
                // the flag set is guaranteed to fail again for the same reason.
                isFirstInstance = false;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // Swap the new handle into place under the mutex.
            HANDLE oldPipe;
            {
                std::lock_guard<std::mutex> lock(m_pipeMutex);
                oldPipe = m_hPipe;
                m_hPipe = newPipe;
            }
            if (oldPipe != INVALID_HANDLE_VALUE) {
                CancelIoEx(oldPipe, nullptr);
                DisconnectNamedPipe(oldPipe);
                CloseHandle(oldPipe);
            }

            isFirstInstance = false;
            m_pipeReady.store(true, std::memory_order_release);
            IpcLog(domain::LogLevel::Info, "Named pipe created, waiting for GUI...");

            // Wait for a client to connect.
            BOOL connected = ConnectNamedPipe(newPipe, nullptr);
            if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }

            if (!connected) {
                // Client failed to connect — close and retry.
                {
                    std::lock_guard<std::mutex> lock(m_pipeMutex);
                    if (m_hPipe == newPipe) {
                        m_hPipe = INVALID_HANDLE_VALUE;
                    }
                }
                DisconnectNamedPipe(newPipe);
                CloseHandle(newPipe);
                m_connected.store(false, std::memory_order_release);
                continue;
            }

            m_connected.store(true, std::memory_order_release);
            IpcLog(domain::LogLevel::Info, "GUI client connected via Named Pipe");

            // ---- Client I/O loop -------------------------------------------
            // M13: Buffer sized to detect oversized messages (> 1 MB).
            // If a read fills the buffer, the message is truncated → reject.
            static constexpr size_t IPC_READ_BUFFER = 65536;
            char buffer[IPC_READ_BUFFER];
            DWORD bytesRead = 0;

            while (m_running.load(std::memory_order_relaxed)) {
                BOOL ok = ReadFile(newPipe, buffer, sizeof(buffer) - 1,
                                   &bytesRead, nullptr);
                if (!ok || bytesRead == 0) {
                    break;  // client disconnected or error
                }

                // M13: If buffer is full, the message may be truncated.
                // Reject to prevent partial JSON parse.
                if (bytesRead >= sizeof(buffer) - 1) {
                    IpcLog(domain::LogLevel::Warn,
                           "IPC message too large, rejecting");
                    std::string err =
                        R"({"status":"error","error":"message_too_large"})";
                    DWORD bw = 0;
                    {
                        std::lock_guard<std::mutex> lock(m_pipeMutex);
                        if (m_hPipe == newPipe) {
                            WriteFile(m_hPipe, err.data(),
                                      static_cast<DWORD>(err.size()), &bw,
                                      nullptr);
                        }
                    }
                    continue;
                }

                buffer[bytesRead] = '\0';
                std::string request(buffer, bytesRead);

                try {
                    auto j = nlohmann::json::parse(request);
                    std::string method = j.value("method", "");
                    std::string params = j.value("params", "");

                    std::string response;
                    if (m_requestCallback) {
                        m_requestCallback(method, params, response);
                    } else {
                        response = R"({"status":"error","error":"not_ready"})";
                    }

                    // M7: hold pipe mutex during write to prevent interleaving
                    // with concurrent SendMessage() push notifications.
                    {
                        std::lock_guard<std::mutex> lock(m_pipeMutex);
                        if (m_hPipe == newPipe) {
                            DWORD bytesWritten = 0;
                            WriteFile(m_hPipe, response.data(),
                                      static_cast<DWORD>(response.size()),
                                      &bytesWritten, nullptr);
                            FlushFileBuffers(m_hPipe);
                        }
                    }
                }
                catch (...) {
                    std::string err = R"({"status":"error","error":"parse_error"})";
                    DWORD bytesWritten = 0;
                    WriteFile(newPipe, err.data(),
                              static_cast<DWORD>(err.size()),
                              &bytesWritten, nullptr);
                }
            }

            // Client gone — clean up the pipe instance.
            m_connected.store(false, std::memory_order_release);
            IpcLog(domain::LogLevel::Debug, "GUI client disconnected");

            {
                std::lock_guard<std::mutex> lock(m_pipeMutex);
                if (m_hPipe == newPipe) {
                    m_hPipe = INVALID_HANDLE_VALUE;
                }
            }
            DisconnectNamedPipe(newPipe);
            CloseHandle(newPipe);

            // Allow the next iteration to create a fresh pipe instance
            // with FILE_FLAG_FIRST_PIPE_INSTANCE again.
            isFirstInstance = true;
        }
    }

    // ---- Send a push notification to the connected client ------------------
    void SendMessage(const std::string& type, const std::string& event,
                     const std::string& data) {
        if (!m_connected.load(std::memory_order_acquire)) {
            return;
        }

        nlohmann::json j = {
            {"type", type},
            {"event", event},
            {"data", nlohmann::json::parse(data)}
        };
        std::string msg = j.dump();

        std::lock_guard<std::mutex> lock(m_pipeMutex);
        if (m_hPipe == INVALID_HANDLE_VALUE) {
            return;
        }

        DWORD bytesWritten = 0;
        WriteFile(m_hPipe, msg.data(), static_cast<DWORD>(msg.size()),
                  &bytesWritten, nullptr);
    }

    // ---- Members -----------------------------------------------------------
    HANDLE m_hPipe;                                  // guarded by m_pipeMutex
    std::mutex m_pipeMutex;                          // protects m_hPipe
    void IpcLog(domain::LogLevel level, const std::string& msg) {
        if (m_logSink) {
            m_logSink->Log(level, "pipe", msg);
        }
        // Always write warnings/errors to stderr so GUI can capture
        // them via RedirectStandardError for diagnostics.
        if (level == domain::LogLevel::Warn || level == domain::LogLevel::Error) {
            fprintf(stderr, "[PIPE] %s\n", msg.c_str());
            fflush(stderr);
        }
    }

    domain::ports::ILogSink* m_logSink = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_pipeReady{false};
    RequestCallback m_requestCallback;
};

} // namespace infrastructure
} // namespace tcp_redirector
