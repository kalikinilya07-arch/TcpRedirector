#pragma once

//
// ICapture — чистый доменный порт для захвата/перенаправления трафика.
// Содержит все методы, необходимые для управления WinDivertCapture.
// Является основным портом (IDriverCommunicator удалён как дублирующийся).
//

#include <string>
#include <vector>
#include <memory>
#include "../entities/ProxyConfig.h"
#include "IConnectionTable.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

class ICapture {
public:
    virtual ~ICapture() = default;

    // --- Жизненный цикл ---
    virtual bool Open() = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    // --- Конфигурация захвата ---
    virtual void SetTargetProcess(const std::wstring& exePath) = 0;
    virtual void SetRelayPort(uint16_t port) = 0;
    virtual void SetProxyConfig(const std::string& host, uint16_t port) = 0;
    virtual void SetConnectionTable(IConnectionTable* table) = 0;

    // --- Управление редиректами ---
    virtual std::vector<RedirectEvent> GetPendingRedirects(
        uint32_t timeout_ms = 1000) = 0;
    virtual bool AckRedirect(uint64_t redirect_id) = 0;

    // --- Статистика ---
    virtual DriverStats GetStats() = 0;

    // --- Системные ---
    virtual void* GetEventHandle() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector