#pragma once

#include <optional>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

class IConfigStore {
public:
    virtual ~IConfigStore() = default;

    virtual bool Load() = 0;
    virtual bool Save() = 0;

    virtual ProxyConfig GetProxyConfig() const = 0;
    virtual bool SetProxyConfig(const ProxyConfig& config) = 0;

    virtual std::vector<Rule> GetRules() const = 0;
    virtual bool SetRules(const std::vector<Rule>& rules) = 0;

    virtual LogLevel GetLogLevel() const = 0;
    virtual bool SetLogLevel(LogLevel level) = 0;

    virtual std::filesystem::path GetLogDirectory() const = 0;
    virtual uint32_t GetMaxLogFileSizeMB() const = 0;
    virtual uint32_t GetMaxLogFiles() const = 0;
};

class ISecretsManager {
public:
    virtual ~ISecretsManager() = default;

    // Encrypt plaintext password using DPAPI
    virtual std::vector<uint8_t> Encrypt(const std::wstring& plaintext) = 0;

    // Decrypt password using DPAPI
    virtual std::wstring Decrypt(const std::vector<uint8_t>& ciphertext) = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector