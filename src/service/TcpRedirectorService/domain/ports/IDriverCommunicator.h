#pragma once

#include <vector>
#include <optional>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

struct ProcessInfo {
    uint32_t pid = 0;
    std::wstring name;
    std::wstring path;
};

class IDriverCommunicator {
public:
    virtual ~IDriverCommunicator() = default;

    virtual bool Open() = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    virtual std::vector<RedirectEvent> GetPendingRedirects(uint32_t timeout_ms = 1000) = 0;
    virtual bool AckRedirect(uint64_t redirect_id) = 0;
    virtual bool UpdateRules(const std::vector<Rule>& rules) = 0;
    virtual DriverStats GetStats() = 0;
    virtual std::optional<ProcessInfo> QueryProcess(uint32_t pid) = 0;
    virtual void* GetEventHandle() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector