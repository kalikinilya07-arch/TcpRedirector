#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include "../../domain/ports/ICapture.h"
#include "../../domain/ports/IRelayServer.h"
#include "../../domain/ports/IConnectionTable.h"
#include "../../domain/services/RuleEngine.h"
#include "../../domain/services/ConnectionTracker.h"
#include "../../infrastructure/capture/WinDivertCapture.h"
#include "../../infrastructure/relay/ConnectionTable.h"
#include "../../infrastructure/relay/TcpRelayServer.h"
#include "../../infrastructure/ipc/PipeServer.h"
#include "../../infrastructure/config/ConfigManager.h"
#include "../../infrastructure/logging/Logger.h"
#include "../../adapters/driven/ProxyEngine.h"
#include "../../infrastructure/utf8_convert.h"
#include "IpcHandler.h"
#include "../../infrastructure/auth/KerberosAgentProvider.h"
#include "../../infrastructure/auth/BasicAuthenticationProvider.h"

namespace tcp_redirector {
namespace service {

class TcpRedirectorService {
public:
    TcpRedirectorService()
        : m_statusHandle(0) {
    }

    ~TcpRedirectorService() { Stop(); }

    bool Initialize() {
        // Initialize logging (не фатально если папка логов недоступна)
        m_logger = std::make_unique<infrastructure::Logger>();
        {
            // M4: getenv may return nullptr — fall back to default path
            const char* progData = getenv("ProgramData");
            std::filesystem::path logDir = progData
                ? std::filesystem::path(progData) / "TcpRedirector" / "logs"
                : std::filesystem::path("C:\\ProgramData\\TcpRedirector\\logs");
            m_logger->Initialize(logDir, domain::LogLevel::Info);
        }

        m_logger->Info("service", "Initializing TcpRedirector Service...");

        // Load configuration
        m_configManager = std::make_unique<infrastructure::ConfigManager>();
        if (!m_configManager->Load()) {
            m_logger->Warn("service", "No config found, using defaults");
        }

        // Proxy config from ConfigManager — uses GetProxyConfig() to get decrypted password
        domain::ProxyConfig proxyCfg = m_configManager->GetProxyConfig();
        {
            auto cfg = m_configManager->GetConfig();
            if (proxyCfg.host.empty()) {
                proxyCfg.host = infrastructure::Utf8ToWide(cfg.proxy.host);
                proxyCfg.port = cfg.proxy.port;
                proxyCfg.auth_required = cfg.auth.enabled;
                proxyCfg.kerberos_auth = cfg.auth.kerberos;
                proxyCfg.login = infrastructure::Utf8ToWide(cfg.auth.username);
            }
            m_configManager->UpdateConfigNoSave(cfg);
            m_logger->Info("service", "Proxy set from config: " + cfg.proxy.host + ":" + std::to_string(cfg.proxy.port));
        }

        // Initialize rule engine
        m_ruleEngine = std::make_unique<domain::services::RuleEngine>();
        m_ruleEngine->SetRules(m_configManager->GetRules());
        m_logger->Info("service", "Rules loaded: " +
            std::to_string(m_configManager->GetRules().size()) + " rules");

        // Initialize connection tracker
        m_connectionTracker = std::make_unique<domain::services::ConnectionTracker>();

        // ProxyEngine не используется в DST-modification режиме,
        // relay сам делает HTTP CONNECT. Оставлен для совместимости.
        m_proxyEngine = std::make_unique<infrastructure::ProxyEngine>();
        if (!m_proxyEngine->Initialize(m_configManager->GetProxyConfig())) {
            m_logger->Warn("service", "Proxy engine init skipped (not needed in DST-modification mode)");
        } else {
            m_logger->Info("service", "Proxy engine initialized");
        }

        // Initialize ConnectionTable для DST modification
        m_connTable = std::make_unique<infrastructure::ConnectionTable>();

        // Initialize TcpRelayServer
        uint16_t relayPort = 34010;
        m_relayServer = std::make_unique<infrastructure::TcpRelayServer>(*m_connTable, relayPort);
        m_relayServer->SetProxyConfig(proxyCfg, 1);
        m_relayServer->SetLogSink(m_logger.get());
        static_cast<infrastructure::TcpRelayServer*>(m_relayServer.get())
            ->SetConnectionMonitor(m_connectionTracker.get());
        m_relayServer->SetLogCallback([this](const std::string& msg) {
            m_logger->Debug("relay", msg);
        });

        // Initialize authentication provider based on AuthenticationMode
        // Only set up auth if enabled in config
        {
            auto cfg = m_configManager->GetConfig();

            if (cfg.auth.enabled) {
                auto authMode = cfg.auth.authMode;

                if (authMode == infrastructure::AuthenticationMode::KerberosOnly ||
                    authMode == infrastructure::AuthenticationMode::KerberosPreferred) {
                    auto kerberosProvider = std::make_unique<infrastructure::KerberosAgentProvider>();
                    // v1.1.1: set log callback for AuthAgent launch diagnostics
                    infrastructure::KerberosAgentProvider::SetLogCallback(
                        [this](const std::string& msg) {
                            m_logger->Info("auth-agent", msg);
                        });
                    m_authProvider = std::move(kerberosProvider);
                    m_logger->Info("service", "Authentication: KerberosAgent (mode=" +
                        std::to_string(static_cast<int>(authMode)) + ")");
                }

                if (authMode == infrastructure::AuthenticationMode::BasicOnly ||
                    (authMode == infrastructure::AuthenticationMode::KerberosPreferred && !m_authProvider)) {
                    // Decrypt password via DPAPI
                    auto proxyCfg = m_configManager->GetProxyConfig();
                    std::string password = infrastructure::WideToUtf8(proxyCfg.plain_password);
                    auto basicProvider = std::make_unique<infrastructure::BasicAuthenticationProvider>(
                        cfg.auth.username, password);
                    if (!m_authProvider) {
                        m_authProvider = std::move(basicProvider);
                    } else {
                        m_fallbackAuthProvider = std::move(basicProvider);
                    }
                    m_logger->Info("service", "Authentication: Basic (mode=" +
                        std::to_string(static_cast<int>(authMode)) + ")");
                }

                if (m_authProvider) {
                    static_cast<infrastructure::TcpRelayServer*>(m_relayServer.get())
                        ->SetAuthenticationProvider(m_authProvider.get());
                }
            } else {
                m_logger->Info("service", "Authentication: disabled (auth.enabled=false)");
            }
        }

        // Start relay server
        if (!m_relayServer->Start()) {
            m_logger->Error("service", "Failed to start TcpRelayServer");
        } else {
            m_logger->Info("service", "TcpRelayServer started on port " +
                std::to_string(m_relayServer->GetPort()));
        }

        // Initialize WinDivert capture (DST-modification mode)
        {
            auto capture = std::make_unique<infrastructure::WinDivertCapture>();
            auto appCfg = m_configManager->GetConfig();
            capture->SetTargetProcess(appCfg.app.exePath);
            capture->SetConnectionTable(m_connTable.get());
            capture->SetRelayPort(relayPort);
            capture->SetProxyConfig(
                infrastructure::WideToUtf8(proxyCfg.host),
                proxyCfg.port);
            std::wstring exeName = appCfg.GetExeName();
            std::string exeNameUtf8 = infrastructure::WideToUtf8(exeName);
            m_logger->Info("service", "Target process: " + exeNameUtf8);
            capture->SetLogSink(m_logger.get());
            capture->SetConnectionMonitor(m_connectionTracker.get());
            capture->SetRuleEngine(m_ruleEngine.get());
            m_capture = std::move(capture);
        }

        // Start WinDivert capture (H7: critical — fail Initialize if capture fails)
        if (m_capture->Open()) {
            m_logger->Info("service", "WinDivert capture started (DST-modification mode)");
        } else {
            m_logger->Error("service", "CRITICAL: WinDivert not available — install WinDivert driver first");
            m_logger->Shutdown();
            return false;
        }

        // Enable scheduled log rotation (config.json only, not in UI)
        {
            auto cfg = m_configManager->GetConfig();
            infrastructure::LogRotator::Settings rotSettings;
            rotSettings.enabled = cfg.log_rotation.enabled;
            rotSettings.schedule = cfg.log_rotation.schedule;
            rotSettings.hour = cfg.log_rotation.hour;
            rotSettings.minute = cfg.log_rotation.minute;
            rotSettings.max_age_days = cfg.log_rotation.max_age_days;
            if (!cfg.log_rotation.archive_dir.empty()) {
                rotSettings.archive_dir = std::wstring(
                    cfg.log_rotation.archive_dir.begin(),
                    cfg.log_rotation.archive_dir.end());
            }
            rotSettings.compress = cfg.log_rotation.compress;
            m_logger->EnableScheduledRotation(rotSettings);
        }

        // Record service start time for uptime tracking
        m_startTime = std::chrono::steady_clock::now();

        // Initialize IPC server with IpcHandler (C2: Named Pipe with ACL)
        m_pipeServer = std::make_unique<infrastructure::PipeServer>();
        m_pipeServer->SetLogSink(m_logger.get());
        m_ipcHandler = std::make_unique<adapters::IpcHandler>(
            m_ruleEngine.get(), m_connectionTracker.get(),
            m_configManager.get(), m_logger.get(),
            &m_running, &m_initialized,
            [this]() -> std::pair<uint64_t, uint64_t> {
                auto* capture = static_cast<infrastructure::WinDivertCapture*>(m_capture.get());
                return {capture->GetTotalRxBytes(), capture->GetTotalTxBytes()};
            },
            [this]() -> uint32_t {
                auto* capture = static_cast<infrastructure::WinDivertCapture*>(m_capture.get());
                return capture->GetActiveConnections();
            },
            [this]() -> uint64_t {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - m_startTime).count());
            },
            [this](bool start) -> bool {
                if (!m_capture) return false;
                if (start) {
                    if (!m_capture->IsOpen()) {
                        return m_capture->Open();
                    }
                    return true; // already open
                } else {
                    if (m_capture->IsOpen()) {
                        m_capture->Close();
                    }
                    return true; // already closed
                }
            },
            [this]() -> bool {
                if (!m_configManager) return false;
                if (!m_configManager->Load()) {
                    m_logger->Warn("service", "Config reload failed");
                    return false;
                }
                // Re-apply rules to engine
                m_ruleEngine->SetRules(m_configManager->GetRules());
                // Update proxy config on capture and relay
                auto proxyCfg = m_configManager->GetProxyConfig();
                auto* capture = static_cast<infrastructure::WinDivertCapture*>(m_capture.get());
                if (capture) {
                    capture->SetProxyConfig(
                        infrastructure::WideToUtf8(proxyCfg.host),
                        proxyCfg.port);
                }
                if (m_relayServer) {
                    m_relayServer->SetProxyConfig(proxyCfg, 1);
                }
                m_logger->Info("service", "Configuration reloaded");
                return true;
            });
        SetupIpcHandlers();
        if (m_pipeServer->Start()) {
            if (m_pipeServer->WaitForPipe(std::chrono::seconds(5))) {
                m_logger->Info("service", "IPC server started");
            } else {
                m_logger->Error("service", "IPC pipe creation timed out");
            }
        }

        m_initialized = true;
        m_logger->Info("service", "TcpRedirector Service initialized successfully");
        return true;
    }

    void Run() {
        m_running = true;
        m_logger->Info("service", "Service is running (DST-modification mode)");

        // DST modification relay работает самостоятельно:
        // - CaptureLoop модифицирует SYN и отправляет на relay
        // - TcpRelayServer принимает соединения, делает CONNECT к прокси
        // - Bidirectional bridge передаёт данные
        // ServiceMain просто ждёт сигнала остановки.
        while (m_running) {
            // Периодическая проверка — не делаем поллинг событий,
            // relay и capture работают в своих потоках
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void Stop() {
        if (!m_running) return;
        m_running = false;

        m_logger->Info("service", "Stopping TcpRedirector Service...");

        // M11: correct shutdown order — capture first, then pipe, then relay.
        // 1. Stop packet capture (no new packets will be processed).
        if (m_capture) {
            m_capture->Close();
            m_logger->Info("service", "Capture closed");
        }

        // 2. Disconnect GUI clients.
        if (m_pipeServer) {
            m_pipeServer->Stop();
            m_logger->Info("service", "PipeServer stopped");
        }

        // 3. Stop relay (no more connections will be proxied).
        if (m_relayServer) {
            m_relayServer->Stop();
            m_logger->Info("service", "TcpRelayServer stopped");
        }

        // 4. Clear connection table.
        if (m_connTable) {
            m_connTable->Clear();
        }

        // 5. Shutdown proxy engine.
        if (m_proxyEngine) {
            m_proxyEngine->Shutdown();
        }

        // 6. Logger last.
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
    // Удалён старый HandleRedirect — не используется в DST-modification архитектуре
    // Relay сам обрабатывает перенаправление

    void SetupIpcHandlers() {
        m_pipeServer->SetOnRequest(
            [this](const std::string& method,
                   const std::string& params,
                   std::string& response) {
                // v1.1.0: новые IPC-команды
                if (method == "ping") {
                    response = R"({"jsonrpc":"2.0","result":{"pong":true},"id":null})";
                    return;
                }
                if (method == "get_version") {
                    response = R"({"jsonrpc":"2.0","result":{"version":"1.1.0"},"id":null})";
                    return;
                }
                if (method == "get_status") {
                    nlohmann::json j;
                    j["jsonrpc"] = "2.0";
                    j["result"]["service_state"] = m_running ? "running" : "stopped";
                    j["result"]["driver_loaded"] = m_capture && m_capture->IsOpen();
                    j["result"]["capture_enabled"] = m_capture && m_capture->IsOpen();
                    j["result"]["active_connections"] = m_capture
                        ? static_cast<infrastructure::WinDivertCapture*>(m_capture.get())->GetActiveConnections()
                        : 0;
                    j["result"]["relay_connections"] = 0; // TODO: from m_connTable
                    j["result"]["version"] = "1.1.0";
                    j["id"] = nullptr;
                    response = j.dump();
                    return;
                }
                m_ipcHandler->Handle(method, params, response);
            });
    }

    std::unique_ptr<infrastructure::Logger> m_logger;
    std::unique_ptr<infrastructure::ConfigManager> m_configManager;
    std::unique_ptr<domain::services::RuleEngine> m_ruleEngine;
    std::unique_ptr<domain::services::ConnectionTracker> m_connectionTracker;
    std::unique_ptr<infrastructure::ProxyEngine> m_proxyEngine;
    std::unique_ptr<domain::ports::ICapture> m_capture;
    std::unique_ptr<infrastructure::PipeServer> m_pipeServer;
    std::unique_ptr<adapters::IpcHandler> m_ipcHandler;

    // DST modification relay (храним через порты для injectable тестов)
    std::unique_ptr<domain::ports::IConnectionTable> m_connTable;

    // Uptime tracking
    std::chrono::steady_clock::time_point m_startTime;
    std::unique_ptr<domain::ports::IRelayServer> m_relayServer;

    // Authentication provider (KerberosAgent or Basic)
    std::unique_ptr<domain::ports::IAuthenticationProvider> m_authProvider;
    std::unique_ptr<domain::ports::IAuthenticationProvider> m_fallbackAuthProvider;

    SERVICE_STATUS m_status = {0};
    SERVICE_STATUS_HANDLE m_statusHandle;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
};

// Global service instance
extern TcpRedirectorService g_Service;

} // namespace service
} // namespace tcp_redirector