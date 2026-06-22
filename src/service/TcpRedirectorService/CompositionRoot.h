#pragma once

//
// CompositionRoot — точка сборки графа зависимостей.
//
// Вынесен из ServiceMain.h для соблюдения принципа голого composition root
// и улучшения тестируемости. В конструктор можно передавать mock-зависимости.
//
// Все доменные объекты создаются здесь через порты (ICapture, IConnectionTable,
// IRelayServer). Конкретные реализации импортируются только здесь.
//

#include <memory>
#include <string>
#include <filesystem>

#include "domain/ports/ICapture.h"
#include "domain/ports/IConnectionTable.h"
#include "domain/ports/IRelayServer.h"
#include "domain/services/RuleEngine.h"
#include "domain/services/ConnectionTracker.h"
#include "domain/entities/ProxyConfig.h"
#include "infrastructure/logging/Logger.h"
#include "infrastructure/config/ConfigManager.h"
#include "infrastructure/ipc/PipeServer.h"
#include "infrastructure/relay/ConnectionTable.h"
#include "infrastructure/relay/TcpRelayServer.h"
#include "infrastructure/capture/WinDivertCapture.h"

namespace tcp_redirector {
namespace service {

struct ServiceDependencies {
    // Domain services
    std::unique_ptr<domain::services::RuleEngine> ruleEngine;
    std::unique_ptr<domain::services::ConnectionTracker> connectionTracker;

    // Infrastructure (с системными зависимостями)
    std::unique_ptr<infrastructure::Logger> logger;
    std::unique_ptr<infrastructure::ConfigManager> configManager;
    std::unique_ptr<infrastructure::PipeServer> pipeServer;

    // Ports (могут быть замоканы в тестах)
    std::unique_ptr<domain::ports::ICapture> capture;
    std::unique_ptr<domain::ports::IConnectionTable> connTable;
    std::unique_ptr<domain::ports::IRelayServer> relayServer;

    // Proxy config (из config manager)
    domain::ProxyConfig proxyConfig;
};

class CompositionRoot {
public:
    CompositionRoot() = default;
    ~CompositionRoot() = default;

    // Создать все зависимости из config.json.
    // В будущем: перегрузить для принятия mock-зависимостей.
    ServiceDependencies CreateFromConfig() {
        ServiceDependencies deps;

        // 1. Logging
        deps.logger = std::make_unique<infrastructure::Logger>();
        deps.logger->Initialize(
            std::filesystem::path(getenv("ProgramData")) / "TcpRedirector" / "logs",
            domain::LogLevel::Info);

        // 2. Config
        deps.configManager = std::make_unique<infrastructure::ConfigManager>();
        if (!deps.configManager->Load()) {
            deps.logger->Warn("service", "No config found, using defaults");
        }

        // 3. Proxy config
        {
            auto cfg = deps.configManager->GetConfig();
            deps.proxyConfig.host = std::wstring(cfg.proxy.host.begin(), cfg.proxy.host.end());
            deps.proxyConfig.port = cfg.proxy.port;
            deps.proxyConfig.auth_required = cfg.auth.enabled;
            if (cfg.auth.enabled) {
                deps.proxyConfig.login = std::wstring(cfg.auth.username.begin(), cfg.auth.username.end());
                deps.proxyConfig.has_password = !cfg.auth.encryptedPassword.empty();
            }
        }

        // 4. Domain services
        deps.ruleEngine = std::make_unique<domain::services::RuleEngine>();
        deps.connectionTracker = std::make_unique<domain::services::ConnectionTracker>();

        // 5. ConnectionTable (infrastructure, через порт)
        deps.connTable = std::make_unique<infrastructure::ConnectionTable>();

        // 6. Relay server (infrastructure, через порт)
        uint16_t relayPort = 34010;
        deps.relayServer = std::make_unique<infrastructure::TcpRelayServer>(
            *deps.connTable, relayPort);
        deps.relayServer->SetProxyConfig(deps.proxyConfig, 1);

        // 7. Capture (WinDivert, через порт ICapture)
        auto capture = std::make_unique<infrastructure::WinDivertCapture>();
        {
            auto cfg = deps.configManager->GetConfig();
            capture->SetTargetProcess(cfg.app.exePath);
            capture->SetConnectionTable(deps.connTable.get());
            capture->SetRelayPort(relayPort);
            capture->SetProxyConfig(
                std::string(deps.proxyConfig.host.begin(), deps.proxyConfig.host.end()),
                deps.proxyConfig.port);
        }
        deps.capture = std::move(capture);

        // 8. IPC server
        deps.pipeServer = std::make_unique<infrastructure::PipeServer>();

        return deps;
    }
};

} // namespace service
} // namespace tcp_redirector