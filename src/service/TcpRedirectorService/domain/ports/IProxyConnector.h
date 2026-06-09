#pragma once

#include <functional>
#include <memory>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

struct ProxySessionStats {
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    std::chrono::milliseconds duration{0};
    bool connected = false;
    std::string error_message;
};

class IProxySession {
public:
    virtual ~IProxySession() = default;
    virtual bool Start(const RedirectEvent& redirect) = 0;
    virtual void Close() = 0;
    virtual ProxySessionStats GetStats() const = 0;
    virtual bool IsActive() const = 0;
    virtual uint64_t GetSessionId() const = 0;
};

class IProxyConnector {
public:
    virtual ~IProxyConnector() = default;

    virtual bool Initialize(const ProxyConfig& config) = 0;
    virtual void Shutdown() = 0;

    // Create a new proxy session for a redirect event
    virtual std::shared_ptr<IProxySession> CreateSession(
        const RedirectEvent& redirect,
        std::function<void(bool success, const std::string& error)> callback) = 0;

    virtual ServiceStats GetAggregatedStats() const = 0;
    virtual bool IsInitialized() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector