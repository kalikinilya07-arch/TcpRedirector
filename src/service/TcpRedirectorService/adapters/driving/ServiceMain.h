#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include "../../domain/ports/ICapture.h"
#include "../../domain/ports/ICapturePreflight.h"
#include "../../domain/ports/IRelayServer.h"
#include "../../domain/ports/IConnectionTable.h"
#include "../../domain/services/RuleEngine.h"
#include "../../domain/services/ConnectionTracker.h"
#include "../../infrastructure/capture/WinDivertCapture.h"
#include "../../infrastructure/capture/WintunCapture.h"
#include "../../infrastructure/preflight/WinDivertPreflight.h"
#include "../../infrastructure/preflight/WintunPreflight.h"
#include "../../infrastructure/relay/ConnectionTable.h"
#include "../../infrastructure/relay/TcpRelayServer.h"
#include "../../infrastructure/ipc/TcpIpcServer.h"
#include "../../infrastructure/config/ConfigManager.h"
#include "../../infrastructure/logging/Logger.h"
#include "../../infrastructure/paths/AppPaths.h"
#include "../../adapters/driven/ProxyEngine.h"
#include "../../infrastructure/utf8_convert.h"
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
        // WP1: перед всем прочим — однократная миграция legacy config
        // из %ProgramData%\TcpRedirector\config.json в <exeDir>\config.json.
        // Сообщение мы прологируем ПОСЛЕ инициализации логгера ниже.
        auto migration = infrastructure::ConfigManager::EnsureConfigMigrated();

        // WP1: логи теперь ложатся в <exeDir>\logs\; создаём директорию,
        // если её ещё нет. ERROR_ALREADY_EXISTS — не ошибка.
        std::filesystem::path logDir;
        try {
            logDir = std::filesystem::path(infrastructure::paths::GetLogDirectoryW());
            if (!CreateDirectoryW(logDir.c_str(), nullptr)) {
                DWORD gle = GetLastError();
                if (gle != ERROR_ALREADY_EXISTS) {
                    // Не фатально: Logger::Initialize сам напечатает предупреждение,
                    // если не сможет открыть файл лога.
                    fprintf(stderr,
                        "[WARN] ServiceMain: cannot create log directory (GLE=%lu)\n",
                        gle);
                }
            }
        } catch (const std::exception& e) {
            fprintf(stderr,
                "[WARN] ServiceMain: failed to resolve log directory: %s\n",
                e.what());
            // Последний рубеж, чтобы Logger::Initialize не упал на пустом пути.
            logDir = std::filesystem::current_path() / L"logs";
        }

        // Initialize logging (не фатально если папка логов недоступна).
        // Задача 3 (логирование): порог ротации основного лога — 50 МБ
        // (совпадает с дефолтом LogSettings::maxSizeMB и сид-конфигом).
        // Конфиг ещё не загружен на этом этапе (логгер нужен для сообщений
        // миграции), поэтому берём дефолт из LogSettings; после Load() ниже
        // порог переустанавливается фактическим значением из config.json.
        m_logger = std::make_unique<infrastructure::Logger>();
        {
            infrastructure::LogSettings defLog{};
            m_logger->Initialize(logDir, domain::LogLevel::Info,
                                 static_cast<size_t>(defLog.maxSizeMB));
        }

        m_logger->Info("service", "Initializing TcpRedirector Service...");

        // Прологируем результат миграции конфига (теперь, когда логгер поднят).
        if (!migration.message.empty()) {
            if (migration.succeeded) {
                m_logger->Info("service", migration.message);
            } else if (migration.attempted) {
                // Пытались, но не удалось — WARN, продолжаем работу с дефолтами.
                m_logger->Warn("service", migration.message);
            } else {
                // Только диагностический шум (skip-причина) — DEBUG.
                m_logger->Debug("service", migration.message);
            }
        }

        // Load configuration (путь резолвится внутри ConfigManager через AppPaths).
        m_configManager = std::make_unique<infrastructure::ConfigManager>();
        if (!m_configManager->Load()) {
            m_logger->Warn("service", "No config found, using defaults");
        }

        // Задача 3: применяем фактический порог ротации основного лога из
        // config.json (log.maxSizeMB, дефолт 50). Логгер уже поднят выше с
        // дефолтом; здесь переустанавливаем на значение из конфига.
        {
            const int cfgMaxMB = m_configManager->GetConfig().log.maxSizeMB;
            if (cfgMaxMB > 0) {
                m_logger->SetMaxFileSizeMB(static_cast<size_t>(cfgMaxMB));
                m_logger->Info("service",
                    "Log rotation threshold: " + std::to_string(cfgMaxMB) + " MB");
            }
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
                // Kerberos implies auth_required (mutually exclusive with Basic)
                if (proxyCfg.kerberos_auth && !proxyCfg.auth_required) {
                    proxyCfg.auth_required = true;
                    m_logger->Info("service", "Kerberos enabled — forcing auth_required=true");
                }
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

        // WP4: также загружаем v2 app-правила (порт-aware матчинг).
        // Legacy SetRules(...) остаётся рабочим для v1-конфигов; v2 app-правила
        // используются в новом MatchForFlow(name, path, dst_port).
        {
            auto appRules = m_configManager->GetAppRules();
            const size_t appRulesCount = appRules.size();
            m_ruleEngine->SetAppRules(std::move(appRules));
            m_logger->Info("service", "App rules loaded: " +
                std::to_string(appRulesCount) + " app rule(s)");
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
        uint16_t relayPort = 34010;
        m_relayServer = std::make_unique<infrastructure::TcpRelayServer>(*m_connTable, relayPort);
        m_relayServer->SetProxyConfig(proxyCfg, 1);
        m_relayServer->SetLogSink(m_logger.get());
        static_cast<infrastructure::TcpRelayServer*>(m_relayServer.get())
            ->SetConnectionMonitor(m_connectionTracker.get());
        m_relayServer->SetLogCallback([this](const std::string& msg) {
            m_logger->Debug("relay", msg);
        });

        // Start relay server
        if (!m_relayServer->Start()) {
            m_logger->Error("service", "Failed to start TcpRelayServer");
        } else {
            m_logger->Info("service", "TcpRelayServer started on port " +
                std::to_string(m_relayServer->GetPort()));
        }

        // WP7: mode-conditional preflight — validate ONLY the active engine's
        // dependencies BEFORE we construct the capture.  See §8 «Mode-conditional
        // dependency validation» in plans/WINTUN_INTEGRATION_PLAN.md and user
        // requirement #6: the inactive engine's files are never checked.
        {
            std::unique_ptr<domain::ports::ICapturePreflight> preflight;
            switch (m_configManager->GetCaptureMode()) {
                case infrastructure::CaptureMode::WinDivert:
                    preflight = std::make_unique<infrastructure::preflight::WinDivertPreflight>();
                    break;
                case infrastructure::CaptureMode::Wintun:
                    preflight = std::make_unique<infrastructure::preflight::WintunPreflight>(
                        m_configManager->GetWintunSettings());
                    break;
            }
            auto pf = preflight->Check();
            for (const auto& w : pf.warnings) {
                m_logger->Warn("preflight", "[" + pf.mode + "] " + w);
            }
            for (const auto& f : pf.failures) {
                m_logger->Error("preflight", "[" + pf.mode + "] " + f);
            }
            // Persist result for a future IPC GetStatus accessor (see
            // GetLastPreflightFailures()/GetLastPreflightMode() below).  We do
            // NOT extend the IPC schema in this WP.
            m_lastPreflightMode = pf.mode;
            m_lastPreflightFailures = pf.failures;
            m_lastPreflightWarnings = pf.warnings;
            m_lastPreflightOk = pf.ok;

            if (!pf.ok) {
                m_logger->Error("preflight",
                    "Refusing to start: " + std::to_string(pf.failures.size())
                    + " failure(s) for mode '" + pf.mode
                    + "'. Service will remain stopped.");
                m_logger->Shutdown();
                return false;
            }
        }

        // WP6: capture instantiation now dispatches on capture_mode.
        // The concrete implementation is chosen via ICapture (base pointer);
        // per-implementation setters are applied inside each branch, but
        // ICapture-level wiring (SetConnectionTable / SetRelayPort / SetProxyConfig
        // / SetRuleEngine) is called uniformly through m_capture.
        {
            auto appCfg = m_configManager->GetConfig();
            std::wstring exeName    = appCfg.GetExeName();
            std::string  exeNameUtf8 = infrastructure::WideToUtf8(exeName);

            switch (m_configManager->GetCaptureMode()) {
                case infrastructure::CaptureMode::WinDivert: {
                    m_logger->Info("service", "Capture mode: WinDivert");
                    auto capture = std::make_unique<infrastructure::WinDivertCapture>();
                    // WinDivert-specific (non-ICapture) hooks.
                    capture->SetLogSink(m_logger.get());
                    capture->SetConnectionMonitor(m_connectionTracker.get());
                    m_capture = std::move(capture);
                    break;
                }
                case infrastructure::CaptureMode::Wintun: {
                    const auto wintun = m_configManager->GetWintunSettings();
                    const bool external =
                        (wintun.engine == infrastructure::WintunEngineKind::External);
                    m_logger->Info("service",
                        std::string("Capture mode: Wintun (")
                        + (external ? "external engine)" : "embedded engine)"));

                    // WP12a — до старта capture'а поднимаем SOCKS5-слушатель
                    // на TcpRelayServer (только для external-движка).  Он
                    // будет upstream'ом для tun2socks.exe, которую WP12 запустит
                    // после Open()'а capture'а.
                    //
                    // ВНИМАНИЕ: НЕ активируем при embedded — движок ходит на
                    // relay напрямую как loopback TCP-клиент, SOCKS5 не нужен.
                    // Preflight (WP7) уже проверил формат socks5_listen и что
                    // порт свободен; здесь мы просто биндим.
                    if (external) {
                        auto hp = infrastructure::relay::Socks5Adapter::ParseHostPort(
                            wintun.external_engine.socks5_listen);
                        auto* relay =
                            static_cast<infrastructure::TcpRelayServer*>(m_relayServer.get());
                        std::string err;
                        if (!relay->EnableSocks5Listener(hp.first, hp.second, &err)) {
                            m_logger->Error("relay",
                                "Failed to enable SOCKS5 listener: " + err);
                            m_logger->Shutdown();
                            return false;
                        }
                    }

                    // WP10: реальный WintunCapture-фасад.  Порт релея
                    // передаём в конструктор — движок форвардит принятые
                    // из туннеля flow'ы именно на 127.0.0.1:relayPort.
                    auto capture = std::make_unique<infrastructure::WintunCapture>(
                        wintun,
                        relayPort,
                        m_logger.get());
                    // Non-ICapture hooks kept parallel to WinDivert branch.
                    capture->SetConnectionMonitor(m_connectionTracker.get());
                    m_capture = std::move(capture);
                    break;
                }
            }

            // Common wiring via the ICapture base pointer — no static_cast.
            m_capture->SetTargetProcess(appCfg.app.exePath);
            m_capture->SetConnectionTable(m_connTable.get());
            m_capture->SetRelayPort(relayPort);
            m_capture->SetProxyConfig(
                infrastructure::WideToUtf8(proxyCfg.host),
                proxyCfg.port);
            m_capture->SetRuleEngine(m_ruleEngine.get());

            m_logger->Info("service", "Target process: " + exeNameUtf8);
        }

        // Start capture (H7: critical — fail Initialize if capture fails).
        // WP6: message is engine-neutral; specific error string is up to
        // the concrete implementation (or, later, the preflight in WP7).
        if (m_capture->Open()) {
            m_logger->Info("service", "Capture started");
        } else {
            m_logger->Error("service",
                "CRITICAL: capture engine failed to open (see preceding log lines)");
            m_logger->Shutdown();
            return false;
        }

        // Record service start time for uptime tracking
        m_startTime = std::chrono::steady_clock::now();

        // Initialize IPC server with IpcHandler
        m_pipeServer = std::make_unique<infrastructure::TcpIpcServer>();
        // Route IPC-server diagnostics into the shared log (previously the sink
        // was never set, so IPC errors were silently discarded).
        m_pipeServer->SetLogSink(m_logger.get());
        // B1 (QA audit): the IPC channel is authenticated with a per-run token
        // written next to the EXE (same dir as config.json) with an
        // Administrators/SYSTEM-only DACL. The elevated GUI reads it and echoes
        // it in every request; unauthenticated local callers are rejected.
        try {
            std::filesystem::path tokenPath =
                std::filesystem::path(infrastructure::paths::GetExecutableDirectoryW())
                / L".ipc_token";
            m_pipeServer->SetAuthTokenFilePath(tokenPath.wstring());
        } catch (const std::exception& e) {
            m_logger->Warn("service",
                std::string("Could not resolve IPC token path: ") + e.what());
        }
        m_ipcHandler = std::make_unique<adapters::IpcHandler>(
            m_ruleEngine.get(), m_connectionTracker.get(),
            m_configManager.get(), m_logger.get(),
            &m_running, &m_initialized,
            [this]() -> std::pair<uint64_t, uint64_t> {
                // WP6: read byte counters through the ICapture base pointer
                // — no static_cast to a concrete implementation.
                return {m_capture->GetTotalRxBytes(), m_capture->GetTotalTxBytes()};
            },
            [this]() -> uint32_t {
                // WP6: same — go through ICapture.
                return m_capture->GetActiveConnections();
            },
            [this]() -> uint64_t {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - m_startTime).count());
            });
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

        // M11: correct shutdown order — capture first, then pipe, then relay.
        // 1. Stop packet capture (no new packets will be processed).
        if (m_capture) {
            m_capture->Close();
            m_logger->Info("service", "Capture closed");
        }

        // 2. Disconnect GUI clients.
        if (m_pipeServer) {
            m_pipeServer->Stop();
            m_logger->Info("service", "IPC server stopped");
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

    // WP7: lightweight accessors so a future IPC GetStatus handler (or unit
    // test) can surface the last preflight outcome.  We intentionally do NOT
    // extend the IPC binary/JSON schema in this WP.
    const std::vector<std::string>& GetLastPreflightFailures() const {
        return m_lastPreflightFailures;
    }
    const std::vector<std::string>& GetLastPreflightWarnings() const {
        return m_lastPreflightWarnings;
    }
    const std::string& GetLastPreflightMode() const { return m_lastPreflightMode; }
    bool GetLastPreflightOk() const { return m_lastPreflightOk; }

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
    std::unique_ptr<infrastructure::TcpIpcServer> m_pipeServer;
    std::unique_ptr<adapters::IpcHandler> m_ipcHandler;

    // DST modification relay (храним через порты для injectable тестов)
    std::unique_ptr<domain::ports::IConnectionTable> m_connTable;

    // Uptime tracking
    std::chrono::steady_clock::time_point m_startTime;
    std::unique_ptr<domain::ports::IRelayServer> m_relayServer;

    SERVICE_STATUS m_status = {0};
    SERVICE_STATUS_HANDLE m_statusHandle;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};

    // WP7: last preflight summary (populated in Initialize before m_capture->Open()).
    // Consumed by GetLastPreflight*() accessors above.
    std::string m_lastPreflightMode;
    std::vector<std::string> m_lastPreflightFailures;
    std::vector<std::string> m_lastPreflightWarnings;
    bool m_lastPreflightOk = false;
};

// Global service instance
extern TcpRedirectorService g_Service;

} // namespace service
} // namespace tcp_redirector