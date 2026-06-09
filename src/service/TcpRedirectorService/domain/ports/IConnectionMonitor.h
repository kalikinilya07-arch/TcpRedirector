#pragma once

#include <functional>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

class IConnectionMonitor {
public:
    virtual ~IConnectionMonitor() = default;

    virtual void AddConnection(const ConnectionRecord& record) = 0;
    virtual void UpdateConnection(uint64_t id, const ConnectionRecord& updates) = 0;
    virtual void RemoveConnection(uint64_t id) = 0;

    virtual std::vector<ConnectionRecord> GetActiveConnections() const = 0;
    virtual std::vector<ConnectionRecord> Search(const std::wstring& query) const = 0;
    virtual std::vector<ConnectionRecord> FilterByProcess(uint32_t pid) const = 0;

    virtual ServiceStats GetAggregatedStats() const = 0;

    // Callback for UI updates
    using ConnectionsCallback = std::function<void(const std::vector<ConnectionRecord>&)>;
    virtual void SetOnConnectionsChanged(ConnectionsCallback callback) = 0;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;

    virtual void Log(LogLevel level, const std::string& logger,
                     const std::string& message) = 0;
    virtual void SetLevel(LogLevel level) = 0;
    virtual LogLevel GetLevel() const = 0;

    using LogCallback = std::function<void(const LogEntry&)>;
    virtual void SetOnLogEntry(LogCallback callback) = 0;

    virtual std::vector<LogEntry> GetRecentEntries(size_t max_count = 100) const = 0;
};

class IGUIIpc {
public:
    virtual ~IGUIIpc() = default;

    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsConnected() const = 0;

    virtual void SendConnections(const std::vector<ConnectionRecord>& connections) = 0;
    virtual void SendLog(const LogEntry& entry) = 0;
    virtual void SendStats(const ServiceStats& stats) = 0;
    virtual void SendConfig(const ProxyConfig& config) = 0;
    virtual void SendRules(const std::vector<Rule>& rules) = 0;

    using RequestCallback = std::function<void(const std::string& method,
                                                const std::string& params,
                                                std::string& response)>;
    virtual void SetOnRequest(RequestCallback callback) = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector