#pragma once

//
// IRelayServer — чистый доменный порт для relay сервера.
// TcpRelayServer (infrastructure) реализует этот интерфейс.
//

#include <string>
#include <functional>
#include <cstdint>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

// Коллбэки для уведомления о событиях
using RelayLogCallback = std::function<void(const std::string&)>;

class IRelayServer {
public:
    virtual ~IRelayServer() = default;

    virtual void SetProxyConfig(const ProxyConfig& config, uint32_t config_id = 1) = 0;
    virtual void SetLogCallback(RelayLogCallback cb) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual uint16_t GetPort() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector