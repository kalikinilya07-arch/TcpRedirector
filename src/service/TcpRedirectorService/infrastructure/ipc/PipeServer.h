#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include "../../domain/ports/IConnectionMonitor.h"

namespace tcp_redirector {
namespace infrastructure {

class PipeServer : public domain::ports::IGUIIpc {
public:
    PipeServer() : m_hPipe(INVALID_HANDLE_VALUE) {}

    ~PipeServer() override {
        Stop();
    }

    bool Start() override {
        m_running = true;
        m_thread = std::thread([this]() { PipeThread(); });
        return true;
    }

    void Stop() override {
        m_running = false;
        if (m_hPipe != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(m_hPipe);
            CloseHandle(m_hPipe);
            m_hPipe = INVALID_HANDLE_VALUE;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    bool IsConnected() const override { return m_connected; }

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
            {"total_connections", stats.total_connections},
            {"active_connections", stats.active_connections},
            {"total_rx_bytes", stats.total_rx_bytes},
            {"total_tx_bytes", stats.total_tx_bytes},
            {"proxy_errors", stats.proxy_errors}
        };
        SendMessage("push", "stats", j.dump());
    }

    void SendConfig(const domain::ProxyConfig& config) override {}
    void SendRules(const std::vector<domain::Rule>& rules) override {}

    void SetOnRequest(RequestCallback callback) override {
        m_requestCallback = callback;
    }

private:
    void PipeThread() {
        while (m_running) {
            m_hPipe = CreateNamedPipeW(
                L"\\\\.\\pipe\\TcpRedirectorService",
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                65536, 65536,
                5000,
                NULL);

            if (m_hPipe == INVALID_HANDLE_VALUE) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            m_connected = ConnectNamedPipe(m_hPipe, NULL) ||
                          GetLastError() == ERROR_PIPE_CONNECTED;

            if (!m_connected) {
                CloseHandle(m_hPipe);
                m_hPipe = INVALID_HANDLE_VALUE;
                continue;
            }

            // Handle client communication
            char buffer[65536];
            DWORD bytes_read;

            while (m_running && m_connected) {
                BOOL success = ReadFile(m_hPipe, buffer, sizeof(buffer) - 1,
                                       &bytes_read, NULL);

                if (!success || bytes_read == 0) {
                    m_connected = false;
                    break;
                }

                buffer[bytes_read] = '\0';
                std::string request(buffer, bytes_read);

                try {
                    auto j = nlohmann::json::parse(request);
                    std::string method = j.value("method", "");
                    std::string params = j.value("params", "");
                    std::string response;

                    if (m_requestCallback) {
                        m_requestCallback(method, params, response);
                    } else {
                        response = "{\"status\":\"error\",\"error\":\"not_ready\"}";
                    }

                    DWORD bytes_written;
                    WriteFile(m_hPipe, response.c_str(),
                             (DWORD)response.size(), &bytes_written, NULL);
                    FlushFileBuffers(m_hPipe);
                }
                catch (...) {
                    std::string error = "{\"status\":\"error\",\"error\":\"parse_error\"}";
                    DWORD bytes_written;
                    WriteFile(m_hPipe, error.c_str(),
                             (DWORD)error.size(), &bytes_written, NULL);
                }
            }

            DisconnectNamedPipe(m_hPipe);
            CloseHandle(m_hPipe);
            m_hPipe = INVALID_HANDLE_VALUE;
        }
    }

    void SendMessage(const std::string& type, const std::string& event,
                     const std::string& data) {
        if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return;

        nlohmann::json j = {
            {"type", type},
            {"event", event},
            {"data", nlohmann::json::parse(data)}
        };

        std::string msg = j.dump();
        DWORD bytes_written;
        WriteFile(m_hPipe, msg.c_str(), (DWORD)msg.size(), &bytes_written, NULL);
    }

    HANDLE m_hPipe;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    RequestCallback m_requestCallback;
};

} // namespace infrastructure
} // namespace tcp_redirector