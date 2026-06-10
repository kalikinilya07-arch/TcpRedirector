#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <nlohmann/json.hpp>
#include "../../domain/services/RuleEngine.h"
#include "../../domain/services/ConnectionTracker.h"
#include "../../infrastructure/capture/NpcapCapture.h"
#include "../../infrastructure/ipc/PipeServer.h"
#include "../../infrastructure/config/ConfigManager.h"
#include "../../infrastructure/logging/Logger.h"
#include "../../adapters/driven/ProxyEngine.h"

namespace tcp_redirector {
namespace service {

class TcpRedirectorService {
public:
    TcpRedirectorService()
        : m_statusHandle(0) {
    }

    ~TcpRedirectorService() { Stop(); }

    bool Initialize() {
        // Initialize logging
        m_logger = std::make_unique<infrastructure::Logger>();
        if (!m_logger->Initialize(
                std::filesystem::path(getenv("ProgramData")) / "TcpRedirector" / "logs",
                domain::LogLevel::Info)) {
            return false;
        }

        m_logger->Info("service", "Initializing TcpRedirector Service...");

        // Load configuration
        m_configManager = std::make_unique<infrastructure::ConfigManager>();
        if (!m_configManager->Load()) {
            m_logger->Warn("service", "No config found, using defaults");
        }

        // HARDCODED PROXY: 127.0.0.1:3128 (IPC save is broken by JSON escaping)
        domain::ProxyConfig hardcoded;
        hardcoded.host = L"127.0.0.1";
        hardcoded.port = 3128;
        hardcoded.auth_required = false;
        m_configManager->SetProxyConfig(hardcoded);
        m_logger->Info("service", "Proxy set to 127.0.0.1:3128 (hardcoded)");

        // Initialize rule engine
        m_ruleEngine = std::make_unique<domain::services::RuleEngine>();
        m_ruleEngine->SetRules(m_configManager->GetRules());
        m_logger->Info("service", "Rules loaded: " +
            std::to_string(m_configManager->GetRules().size()) + " rules");

        // HARDCODED: redirect ALL processes (IPC set_rules broken)
        std::vector<domain::Rule> defaultRules;
        domain::Rule catchAll;
        catchAll.id = "default-catch-all";
        catchAll.pattern = L"*";
        catchAll.description = L"All processes";
        catchAll.priority = 999;
        catchAll.enabled = true;
        catchAll.type = domain::RuleType::Global;
        catchAll.action = domain::RuleAction::Proxy;
        defaultRules.push_back(catchAll);
        m_ruleEngine->SetRules(defaultRules);
        m_logger->Info("service", "Default catch-all rule added (hardcoded)");

        // Initialize connection tracker
        m_connectionTracker = std::make_unique<domain::services::ConnectionTracker>();

        // Initialize proxy engine
        m_proxyEngine = std::make_unique<infrastructure::ProxyEngine>();
        if (!m_proxyEngine->Initialize(m_configManager->GetProxyConfig())) {
            m_logger->Error("service", "Failed to initialize proxy engine");
            return false;
        }
        m_logger->Info("service", "Proxy engine initialized");

        // Initialize Npcap capture (replaces kernel driver)
        m_driverComm = std::make_unique<infrastructure::NpcapCapture>();
        if (m_driverComm->Open()) {
            m_logger->Info("service", "Npcap capture started");
        } else {
            m_logger->Warn("service", "Npcap not available (install Npcap from https://npcap.com)");
        }

        // Initialize IPC server
        m_pipeServer = std::make_unique<infrastructure::PipeServer>();
        SetupIpcHandlers();
        if (m_pipeServer->Start()) {
            m_logger->Info("service", "IPC server started");
        }

        m_initialized = true;
        m_logger->Info("service", "TcpRedirector Service initialized successfully");
        return true;
    }

    void Run() {
        m_running = true;
        m_logger->Info("service", "Service is running");

        while (m_running) {
            // Poll for redirect events from driver
            if (m_driverComm && m_driverComm->IsOpen()) {
                auto redirects = m_driverComm->GetPendingRedirects(100);
                for (const auto& redirect : redirects) {
                    HandleRedirect(redirect);
                }
            }

            // Update connection statistics (every second)
            UpdateStatistics();

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void Stop() {
        if (!m_running) return;
        m_running = false;

        m_logger->Info("service", "Stopping TcpRedirector Service...");

        if (m_proxyEngine) {
            m_proxyEngine->Shutdown();
        }

        if (m_pipeServer) {
            m_pipeServer->Stop();
        }

        if (m_driverComm) {
            m_driverComm->Close();
        }

        if (m_logger) {
            m_logger->Info("service", "Service stopped");
            m_logger->Shutdown();
        }

        m_initialized = false;
    }

    bool IsRunning() const { return m_running; }
    bool IsInitialized() const { return m_initialized; }

    // SCM integration
    void SetServiceStatusHandle(SERVICE_STATUS_HANDLE handle) {
        m_statusHandle = handle;
    }

    void ReportStatus(DWORD state, DWORD win32_exit_code = NO_ERROR,
                      DWORD wait_hint = 0) {
        if (!m_statusHandle) return;

        m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        m_status.dwCurrentState = state;
        m_status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                      SERVICE_ACCEPT_SHUTDOWN;
        m_status.dwWin32ExitCode = win32_exit_code;
        m_status.dwWaitHint = wait_hint;

        SetServiceStatus(m_statusHandle, &m_status);
    }

private:
    void HandleRedirect(const domain::RedirectEvent& redirect) {
        printf("[Redirect] PID=%u path=%.120ls\n", redirect.pid, redirect.process_path.c_str());

        // Check rules
        std::wstring process_name;
        std::wstring process_path = redirect.process_path;

        // Extract process name from path
        auto pos = process_path.find_last_of(L'\\');
        if (pos != std::wstring::npos) {
            process_name = process_path.substr(pos + 1);
        } else {
            process_name = process_path;
        }

        if (!m_ruleEngine->ShouldRedirect(process_name, process_path)) {
            printf("[Redirect] SKIP: %ls (not in rules)\n", process_name.c_str());
            if (m_driverComm) {
                m_driverComm->AckRedirect(redirect.redirect_id);
            }
            m_logger->Debug("redirect",
                "Skipped (direct): " + std::string(process_name.begin(), process_name.end()));
            return;
        }

        // Create proxy session
        auto session = m_proxyEngine->CreateSession(redirect,
            [this, redirect](bool success, const std::string& error) {
                if (success) {
                    m_logger->Debug("proxy", "Tunnel established: PID " +
                        std::to_string(redirect.pid) + " -> proxy");
                } else {
                    m_logger->Error("proxy", "Tunnel failed: " + error);
                }
            });

        if (session) {
            // Record connection
            domain::ConnectionRecord record;
            record.id = m_connectionTracker->GenerateId();
            record.pid = redirect.pid;
            record.process_path = redirect.process_path;
            record.destination_ip = std::to_string(redirect.original_address_v4);
            record.destination_port = redirect.original_port;
            record.start_time = std::chrono::steady_clock::now();
            record.state = domain::ConnectionState::Redirecting;
            m_connectionTracker->AddConnection(record);

            // Acknowledge to driver
            if (m_driverComm) {
                m_driverComm->AckRedirect(redirect.redirect_id);
            }

            printf("[Redirect] PROXY: %ls (%u) -> %u.%u.%u.%u:%u\n",
                   process_name.c_str(), redirect.pid,
                   (redirect.original_address_v4 >> 24) & 0xFF,
                   (redirect.original_address_v4 >> 16) & 0xFF,
                   (redirect.original_address_v4 >> 8) & 0xFF,
                   redirect.original_address_v4 & 0xFF,
                   redirect.original_port);

            m_logger->Info("redirect",
                "Redirected: " + std::string(process_name.begin(), process_name.end()) +
                " (" + std::to_string(redirect.pid) + ") -> " +
                std::to_string(redirect.original_address_v4) + ":" +
                std::to_string(redirect.original_port));
        } else {
            m_logger->Error("redirect", "Failed to create proxy session for PID " +
                std::to_string(redirect.pid));
        }
    }

    void UpdateStatistics() {
        // Push messages via pipe break the request/response protocol.
        // GUI polls via GetStatsAsync / GetConnectionsAsync every 2 seconds.
        // No push messages needed.
    }

    void SetupIpcHandlers() {
        m_pipeServer->SetOnRequest(
            [this](const std::string& method,
                   const std::string& params,
                   std::string& response) {
                HandleIpcRequest(method, params, response);
            });
    }

    void HandleIpcRequest(const std::string& method,
                          const std::string& params,
                          std::string& response) {
        nlohmann::json result;

        try {
            if (method == "get_config") {
                auto config = m_configManager->GetProxyConfig();
                result["status"] = "success";
                result["data"]["proxy"]["host"] = std::string(config.host.begin(), config.host.end());
                result["data"]["proxy"]["port"] = config.port;
                result["data"]["proxy"]["auth_required"] = config.auth_required;
                result["data"]["proxy"]["has_password"] = config.has_password;
            }
            else if (method == "set_config") {
                auto j = nlohmann::json::parse(params);
                domain::ProxyConfig config;
                std::string host = j["host"].get<std::string>();
                config.host = std::wstring(host.begin(), host.end());
                config.port = j["port"].get<uint16_t>();
                config.auth_required = j.value("auth_required", false);
                config.login = std::wstring(j.value("login", std::string()).begin(),
                                             j.value("login", std::string()).end());
                config.has_password = j.value("set_password", false);
                m_configManager->SetProxyConfig(config);
                result["status"] = "success";
            }
            else if (method == "get_rules") {
                auto rules = m_ruleEngine->GetRules();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& r : rules) {
                    arr.push_back({
                        {"id", r.id},
                        {"pattern", std::string(r.pattern.begin(), r.pattern.end())},
                        {"description", std::string(r.description.begin(), r.description.end())},
                        {"priority", r.priority},
                        {"enabled", r.enabled},
                        {"type", static_cast<int>(r.type)},
                        {"action", static_cast<int>(r.action)}
                    });
                }
                result["status"] = "success";
                result["data"]["rules"] = arr;
            }
            else if (method == "set_rules") {
                auto j = nlohmann::json::parse(params);
                std::vector<domain::Rule> rules;
                for (const auto& r : j["rules"]) {
                    domain::Rule rule;
                    rule.id = r.value("id", "");
                    rule.pattern = std::wstring(r["pattern"].get<std::string>().begin(),
                                                 r["pattern"].get<std::string>().end());
                    rule.description = std::wstring(r.value("description", std::string()).begin(),
                                                     r.value("description", std::string()).end());
                    rule.priority = r.value("priority", 0);
                    rule.enabled = r.value("enabled", true);
                    rule.type = static_cast<domain::RuleType>(r.value("type", 0));
                    rule.action = static_cast<domain::RuleAction>(r.value("action", 0));
                    rules.push_back(rule);
                }
                m_ruleEngine->SetRules(rules);
                m_configManager->SetRules(rules);
                result["status"] = "success";
            }
            else if (method == "get_connections") {
                auto connections = m_connectionTracker->GetActiveConnections();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& c : connections) {
                    arr.push_back({
                        {"id", c.id},
                        {"pid", c.pid},
                        {"process_name", std::string(c.process_name.begin(), c.process_name.end())},
                        {"destination_host", std::string(c.destination_host.begin(), c.destination_host.end())},
                        {"destination_ip", c.destination_ip},
                        {"destination_port", c.destination_port},
                        {"rx_bytes", c.rx_bytes},
                        {"tx_bytes", c.tx_bytes},
                        {"duration_ms", c.duration.count()},
                        {"state", static_cast<int>(c.state)},
                        {"proxy_enabled", c.proxy_enabled}
                    });
                }
                result["status"] = "success";
                result["data"]["connections"] = arr;
            }
            else if (method == "get_logs") {
                auto entries = m_logger->GetRecentEntries(500);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& e : entries) {
                    arr.push_back({
                        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                            e.timestamp.time_since_epoch()).count()},
                        {"level", static_cast<int>(e.level)},
                        {"logger", e.logger},
                        {"message", e.message}
                    });
                }
                result["status"] = "success";
                result["data"]["logs"] = arr;
            }
            else if (method == "get_stats") {
                auto stats = m_connectionTracker->GetAggregatedStats();
                result["status"] = "success";
                result["data"]["total_connections"] = stats.total_connections;
                result["data"]["active_connections"] = stats.active_connections;
                result["data"]["total_rx_bytes"] = stats.total_rx_bytes;
                result["data"]["total_tx_bytes"] = stats.total_tx_bytes;
            }
            else if (method == "service_status") {
                result["status"] = "success";
                result["data"]["running"] = m_running.load();
                result["data"]["initialized"] = m_initialized.load();
            }
            else if (method == "set_log_level") {
                auto j = nlohmann::json::parse(params);
                auto level = static_cast<domain::LogLevel>(j["level"].get<int>());
                m_logger->SetLevel(level);
                result["status"] = "success";
            }
            else {
                result["status"] = "error";
                result["error"] = "unknown_method";
            }
        }
        catch (const std::exception& e) {
            result["status"] = "error";
            result["error"] = e.what();
        }

        response = result.dump();
    }

    std::unique_ptr<infrastructure::Logger> m_logger;
    std::unique_ptr<infrastructure::ConfigManager> m_configManager;
    std::unique_ptr<domain::services::RuleEngine> m_ruleEngine;
    std::unique_ptr<domain::services::ConnectionTracker> m_connectionTracker;
    std::unique_ptr<infrastructure::ProxyEngine> m_proxyEngine;
    std::unique_ptr<infrastructure::NpcapCapture> m_driverComm;
    std::unique_ptr<infrastructure::PipeServer> m_pipeServer;

    SERVICE_STATUS m_status = {0};
    SERVICE_STATUS_HANDLE m_statusHandle;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
};

// Global service instance
extern TcpRedirectorService g_Service;

} // namespace service
} // namespace tcp_redirector