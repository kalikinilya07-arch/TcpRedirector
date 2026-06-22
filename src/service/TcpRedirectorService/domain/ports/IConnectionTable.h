#pragma once

//
// IConnectionTable — чистый доменный порт для таблицы соединений.
// ConnectionTable (infrastructure) реализует этот интерфейс.
//

#include <cstdint>

namespace tcp_redirector {
namespace domain {
namespace ports {

class IConnectionTable {
public:
    virtual ~IConnectionTable() = default;

    virtual void Add(uint16_t src_port, uint32_t src_ip,
                     uint32_t orig_dest_ip, uint16_t orig_dest_port,
                     uint32_t proxy_config_id) = 0;

    virtual bool Get(uint16_t src_port, uint32_t* out_orig_dest_ip,
                     uint16_t* out_orig_dest_port) = 0;

    virtual uint32_t GetProxyConfigId(uint16_t src_port) = 0;

    virtual bool IsTracked(uint16_t src_port) = 0;

    virtual void Remove(uint16_t src_port) = 0;

    virtual void Clear() = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector