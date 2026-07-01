#pragma once

//
// IpcHandler — driving adapter для IPC-запросов от GUI.
// Выделен из ServiceMain.h для соблюдения SRP.
// Содержит только маршрутизацию IPC-методов, без бизнес-логики.
//

#include <string>
#include <nlohmann/json.hpp>
#include "../../domain/services/RuleEngine.h"
#include "../../domain/services/ConnectionTracker.h"
#include "../../infrastructure/config/ConfigManager.h"
#include "../../infrastructure/logging/Logger.h"

namespace tcp_redirector {
namespace adapters {

// Callback to get relay byte counters: returns {rx_bytes, tx_bytes}
using GetRelayBytesCallback = std::function<std::pair<uint64_t, uint64_t>()>;

// Callback to get active connection count from capture (ConnectionTable)
using GetActiveCountCallback = std::function<uint32_t()>;

// Callback to get service uptime in seconds
using GetUptimeCallback = std::function<uint64_t()>;

class IpcHandler {
public:
    IpcHandler(
        domain::services::RuleEngine* ruleEngine,
        domain::services::ConnectionTracker* connectionTracker,
        infrastructure::ConfigManager* configManager,
        infrastructure::Logger* logger,
        const std::atomic<bool>* running,
        const std::atomic<bool>* initialized,
        GetRelayBytesCallback getRelayBytes = nullptr,
        GetActiveCountCallback getActiveCount = nullptr,
        GetUptimeCallback getUptime = nullptr)
        : m_ruleEngine(ruleEngine)
        , m_connectionTracker(connectionTracker)
        , m_configManager(configManager)
        , m_logger(logger)
        , m_running(running)
        , m_initialized(initialized)
        , m_getRelayBytes(std::move(getRelayBytes))
        , m_getActiveCount(std::move(getActiveCount))
        , m_getUptime(std::move(getUptime)) {
    }

    void Handle(const std::string& method,
                const std::string& params,
                std::string& response) {
        nlohmann::json result;

        try {
            if (method == "get_config") {
                GetConfig(result);
            }
            else if (method == "set_config") {
                SetConfig(params, result);
            }
            else if (method == "get_rules") {
                GetRules(result);
            }
            else if (method == "set_rules") {
                SetRules(params, result);
            }
            else if (method == "get_connections") {
                GetConnections(result);
            }
            else if (method == "get_logs") {
                GetLogs(result);
            }
            else if (method == "get_stats") {
                GetStats(result);
            }
            else if (method == "service_status") {
                GetStatus(result);
            }
            else if (method == "set_log_level") {
                SetLogLevel(params, result);
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

private:
    void GetConfig(nlohmann::json& result) {
        auto config = m_configManager->GetProxyConfig();
        result["status"] = "success";
        result["data"]["proxy"]["host"] = std::string(config.host.begin(), config.host.end());
        result["data"]["proxy"]["port"] = config.port;
        result["data"]["proxy"]["auth_required"] = config.auth_required;
        result["data"]["proxy"]["has_password"] = config.has_password;
    }

    void SetConfig(const std::string& params, nlohmann::json& result) {
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

    void GetRules(nlohmann::json& result) {
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

    void SetRules(const std::string& params, nlohmann::json& result) {
        auto j = nlohmann::json::parse(params);
        std::vector<domain::Rule> rules;
        for (const auto& r : j["rules"]) {
            domain::Rule rule;
            rule.id = r.value("id", "");
            {
                std::string tmp = r["pattern"].get<std::string>();
                rule.pattern = std::wstring(tmp.begin(), tmp.end());
            }
            {
                std::string tmp = r.value("description", std::string());
                rule.description = std::wstring(tmp.begin(), tmp.end());
            }
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

    void GetConnections(nlohmann::json& result) {
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

    void GetLogs(nlohmann::json& result) {
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

    void GetStats(nlohmann::json& result) {
        auto stats = m_connectionTracker->GetAggregatedStats();
        uint64_t rx = stats.total_rx_bytes;
        uint64_t tx = stats.total_tx_bytes;
        // Include capture byte counters (WinDivertCapture tracks TCP-level bytes)
        if (m_getRelayBytes) {
            auto rb = m_getRelayBytes();
            if (rb.first > rx) rx = rb.first;
            if (rb.second > tx) tx = rb.second;
        }
        // active_connections from capture (ConnectionTable) — authoritative source
        uint32_t active = m_getActiveCount ? m_getActiveCount() : stats.active_connections;
        // Push uptime to ConnectionTracker before reading stats
        if (m_getUptime) {
            m_connectionTracker->SetUptimeSeconds(m_getUptime());
        }
        result["status"] = "success";
        result["data"]["active_connections"] = active;
        result["data"]["total_rx_bytes"] = rx;
        result["data"]["total_tx_bytes"] = tx;
        result["data"]["proxy_errors"] = stats.proxy_errors;
        result["data"]["avg_latency_ms"] = stats.avg_latency_ms;
        result["data"]["uptime_seconds"] = stats.uptime_seconds;
    }

    void GetStatus(nlohmann::json& result) {
        result["status"] = "success";
        result["data"]["running"] = m_running->load();
        result["data"]["initialized"] = m_initialized->load();
    }

    void SetLogLevel(const std::string& params, nlohmann::json& result) {
        auto j = nlohmann::json::parse(params);
        auto level = static_cast<domain::LogLevel>(j["level"].get<int>());
        m_logger->SetLevel(level);
        result["status"] = "success";
    }

    domain::services::RuleEngine* m_ruleEngine;
    domain::services::ConnectionTracker* m_connectionTracker;
    infrastructure::ConfigManager* m_configManager;
    infrastructure::Logger* m_logger;
    const std::atomic<bool>* m_running;
    const std::atomic<bool>* m_initialized;
    GetRelayBytesCallback m_getRelayBytes;
    GetActiveCountCallback m_getActiveCount;
    GetUptimeCallback m_getUptime;
};

} // namespace adapters
} // namespace tcp_redirector