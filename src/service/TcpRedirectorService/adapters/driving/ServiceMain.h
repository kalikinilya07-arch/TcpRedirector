#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include "../../domain/ports/ICapture.h"
#include "../../domain/services/RuleEngine.h"
#include "../../domain/services/ConnectionTracker.h"
#include "../../infrastructure/capture/WinDivertCapture.h"
#include "../../infrastructure/ipc/PipeServer.h"
#include "../../infrastructure/config/ConfigManager.h"
#include "../../infrastructure/logging/Logger.h"
#include "../../adapters/driven/ProxyEngine.h"
#include "IpcHandler.h"

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

        // Default proxy config — restored from config.json on each start
        // REPLACED with config: читаем из ConfigManager вместо хардкода
        // ВАЖНО: НЕ вызываем SetProxyConfig — он затирает config.json через SaveImpl()
        // ProxyConfig передаётся напрямую в ProxyEngine::Initialize
        {
            auto cfg = m_configManager->GetConfig();
            domain::ProxyConfig proxyCfg;
            proxyCfg.host = std::wstring(cfg.proxy.host.begin(), cfg.proxy.host.end());
            proxyCfg.port = cfg.proxy.port;
            proxyCfg.auth_required = cfg.auth.enabled;
            if (cfg.auth.enabled) {
                proxyCfg.login = std::wstring(cfg.auth.username.begin(), cfg.auth.username.end());
                proxyCfg.has_password = !cfg.auth.encryptedPassword.empty();
            }
            // Прямая передача в ProxyEngine (без сохранения в JSON)
            m_configManager->UpdateConfigNoSave(cfg);
            m_logger->Info("service", "Proxy set from config: " + cfg.proxy.host + ":" + std::to_string(cfg.proxy.port));
        }
        // Старый хардкод (сохранён для совместимости):
        // domain::ProxyConfig hardcoded;
        // hardcoded.host = L"127.0.0.1";
        // hardcoded.port = 8888;
        // hardcoded.auth_required = false;
        // m_configManager->SetProxyConfig(hardcoded);
        // m_logger->Info("service", "Proxy set to 127.0.0.1:8888");

        // Initialize rule engine
        m_ruleEngine = std::make_unique<domain::services::RuleEngine>();
        m_ruleEngine->SetRules(m_configManager->GetRules());
        m_logger->Info("service", "Rules loaded: " +
            std::to_string(m_configManager->GetRules().size()) + " rules");

        // Default rule: создаётся из ConfigManager (полный путь, ProcessPath — как в оригинале)
        {
            auto cfg = m_configManager->GetConfig();
            std::vector<domain::Rule> configRules;
            domain::Rule rule;
            rule.id = "capture-target";
            rule.pattern = cfg.app.exePath; // полный путь, как в оригинальном хардкоде
            rule.description = L"Auto-generated from config: " + cfg.app.exePath;
            rule.priority = 1;
            rule.enabled = true;
            rule.type = domain::RuleType::ProcessPath;
            rule.action = cfg.proxy.enabled
                ? domain::RuleAction::Proxy
                : domain::RuleAction::Direct;
            configRules.push_back(rule);
            m_ruleEngine->SetRules(configRules);
            std::string exePath(cfg.app.exePath.begin(), cfg.app.exePath.end());
            m_logger->Info("service", "Rule set from config (ProcessPath): " + exePath +
                ", proxy=" + (cfg.proxy.enabled ? "enabled" : "disabled"));
        }
        // Старый хардкод (сохранён для совместимости):
        // std::vector<domain::Rule> defaultRules;
        // domain::Rule transfersRule;
        // transfersRule.id = "packet-gen-test";
        // transfersRule.pattern = L"packet_generator.exe";
        // ...

        // Initialize connection tracker
        m_connectionTracker = std::make_unique<domain::services::ConnectionTracker>();

        // Initialize proxy engine
        m_proxyEngine = std::make_unique<infrastructure::ProxyEngine>();
        if (!m_proxyEngine->Initialize(m_configManager->GetProxyConfig())) {
            m_logger->Error("service", "Failed to initialize proxy engine");
            return false;
        }
        m_logger->Info("service", "Proxy engine initialized");

        // Initialize WinDivert capture via ICapture port
        m_capture = std::make_unique<infrastructure::WinDivertCapture>();
        // Установить целевой процесс из конфига (замена хардкода)
        {
            auto cfg = m_configManager->GetConfig();
            m_capture->SetTargetProcess(cfg.app.exePath);
            m_logger->Info("service", "Target process set from config: " +
                std::string(cfg.app.exePath.begin(), cfg.app.exePath.end()));
        }
        if (m_capture->Open()) {
            m_logger->Info("service", "WinDivert capture started");
        } else {
            m_logger->Warn("service", "WinDivert not available (place WinDivert.dll and WinDivert64.sys next to exe)");
        }

        // Initialize IPC server with IpcHandler
        m_pipeServer = std::make_unique<infrastructure::PipeServer>();
        m_ipcHandler = std::make_unique<adapters::IpcHandler>(
            m_ruleEngine.get(), m_connectionTracker.get(),
            m_configManager.get(), m_logger.get(),
            &m_running, &m_initialized);
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
            // Poll for redirect events from capture
            if (m_capture && m_capture->IsOpen()) {
                auto redirects = m_capture->GetPendingRedirects(100);
                for (const auto& redirect : redirects) {
                    HandleRedirect(redirect);
                }
            }

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

        if (m_capture) {
            m_capture->Close();
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
        std::wstring process_name;
        std::wstring process_path = redirect.process_path;

        auto pos = process_path.find_last_of(L'\\');
        if (pos != std::wstring::npos) {
            process_name = process_path.substr(pos + 1);
        } else {
            process_name = process_path;
        }

        if (!m_ruleEngine->ShouldRedirect(process_name, process_path)) {
            if (m_capture) {
                m_capture->AckRedirect(redirect.redirect_id);
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
            domain::ConnectionRecord record;
            record.id = m_connectionTracker->GenerateId();
            record.pid = redirect.pid;
            record.process_path = redirect.process_path;
            record.destination_ip = std::to_string(redirect.original_address_v4);
            record.destination_port = redirect.original_port;
            record.start_time = std::chrono::steady_clock::now();
            record.state = domain::ConnectionState::Redirecting;
            m_connectionTracker->AddConnection(record);

            if (m_capture) {
                m_capture->AckRedirect(redirect.redirect_id);
            }

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

    void SetupIpcHandlers() {
        m_pipeServer->SetOnRequest(
            [this](const std::string& method,
                   const std::string& params,
                   std::string& response) {
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

    SERVICE_STATUS m_status = {0};
    SERVICE_STATUS_HANDLE m_statusHandle;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
};

// Global service instance
extern TcpRedirectorService g_Service;

} // namespace service
} // namespace tcp_redirector