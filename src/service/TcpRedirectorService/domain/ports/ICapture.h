#pragma once

//
// ICapture — чистый порт для захвата/редиректа трафика.
// В отличие от IDriverCommunicator (IOCTL-ориентированного), содержит
// только методы, реально используемые WinDivertCapture-адаптером.
// Соблюдает Interface Segregation Principle.
//

#include <vector>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

class ICapture {
public:
    virtual ~ICapture() = default;

    virtual bool Open() = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    virtual std::vector<RedirectEvent> GetPendingRedirects(
        uint32_t timeout_ms = 1000) = 0;
    virtual bool AckRedirect(uint64_t redirect_id) = 0;
    virtual DriverStats GetStats() = 0;
    virtual void* GetEventHandle() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector