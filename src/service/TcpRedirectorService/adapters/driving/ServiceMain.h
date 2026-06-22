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
#include "../../infrastructure/relay/ConnectionTable.h"
#include "../../infrastructure/relay/TcpRelayServer.h"
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
        // Initialize logging (не фатально если папка логов недоступна)
        m_logger = std::make_unique<infrastructure::Logger>();
        m_logger->Initialize(
            std::filesystem::path(getenv("ProgramData")) / "TcpRedirector" / "logs",
            domain::LogLevel::Info);

        m_logger->Info("service", "Initializing TcpRedirector Service...");

        // Load configuration
        m_configManager = std::make_unique<infrastructure::ConfigManager>();
        if (!m_configManager->Load()) {
            m_logger->Warn("service", "No config found, using defaults");
        }

        // Proxy config from ConfigManager
        domain::ProxyConfig proxyCfg;
        {
            auto cfg = m_configManager->GetConfig();
            proxyCfg.host = std::wstring(cfg.proxy.host.begin(), cfg.proxy.host.end());
            proxyCfg.port = cfg.proxy.port;
            proxyCfg.auth_required = cfg.auth.enabled;
            if (cfg.auth.enabled) {
                proxyCfg.login = std::wstring(cfg.auth.username.begin(), cfg.auth.username.end());
                proxyCfg.has_password = !cfg.auth.encryptedPassword.empty();
            }
            m_configManager->UpdateConfigNoSave(cfg);
            m_logger->Info("service", "Proxy set from config: " + cfg.proxy.host + ":" + std::to_string(cfg.proxy.port));
        }

        // Initialize rule engine
        m_ruleEngine = std::make_unique<domain::services::RuleEngine>();
        m_ruleEngine->SetRules(m_configManager->GetRules());
        m_logger->Info("service", "Rules loaded: " +
            std::to_string(m_configManager->GetRules().size()) + " rules");

        // Default rule from config
        {
            auto cfg = m_configManager->GetConfig();
            std::vector<domain::Rule> configRules;
            domain::Rule rule;
            rule.id = "capture-target";
            rule.pattern = cfg.app.exePath;
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
        uint16_t relayPort = 34010; // порт локального relay сервера
        m_relayServer = std::make_unique<infrastructure::TcpRelayServer>(*m_connTable, relayPort);
        m_relayServer->SetProxyConfig(proxyCfg, 1);
        m_relayServer->SetLogCallback([this](const std::string& msg) {
            m_logger->Debug("relay", msg);
        });

        if (!m_relayServer->Start()) {
            m_logger->Error("service", "Failed to start TcpRelayServer");
        } else {
            m_logger->Info("service", "TcpRelayServer started on port " + std::to_string(relayPort));
        }

        // Initialize WinDivert capture (DST-modification mode)
        auto capture = std::make_unique<infrastructure::WinDivertCapture>();
        {
            auto appCfg = m_configManager->GetConfig();
            capture->SetTargetProcess(appCfg.app.exePath);
            capture->SetConnectionTable(m_connTable.get());
            capture->SetRelayPort(relayPort);
            capture->SetProxyConfig(
                std::string(proxyCfg.host.begin(), proxyCfg.host.end()),
                proxyCfg.port);
            // exeName извлекается из exePath (последний компонент после \)
            std::wstring exeName = appCfg.GetExeName();
            std::string exeNameUtf8(exeName.begin(), exeName.end());
            m_logger->Info("service", "Target process: " + exeNameUtf8);
        }
        m_capture = std::move(capture);
        if (m_capture->Open()) {
            m_logger->Info("service", "WinDivert capture started (DST-modification mode)");
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

        // Остановить relay сервер
        if (m_relayServer) {
            m_relayServer->Stop();
            m_logger->Info("service", "TcpRelayServer stopped");
        }

        // Очистить таблицу соединений
        if (m_connTable) {
            m_connTable->Clear();
        }

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
    // Удалён старый HandleRedirect — не используется в DST-modification архитектуре
    // Relay сам обрабатывает перенаправление

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

    // DST modification relay
    std::unique_ptr<infrastructure::ConnectionTable> m_connTable;
    std::unique_ptr<infrastructure::TcpRelayServer> m_relayServer;

    SERVICE_STATUS m_status = {0};
    SERVICE_STATUS_HANDLE m_statusHandle;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
};

// Global service instance
extern TcpRedirectorService g_Service;

} // namespace service
} // namespace tcp_redirector