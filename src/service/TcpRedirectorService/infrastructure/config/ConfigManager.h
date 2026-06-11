#pragma once

//
// ConfigManager — управляет конфигурацией приложения.
// - Загружает/сохраняет config.json в %ProgramData%\TcpRedirector\config.json
// - Потокобезопасен (shared_mutex)
// - Уведомляет listener'ов при изменении конфига
// - Шифрует пароль через DPAPI (SecretsManager)
// - Совместим с IConfigStore интерфейсом
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dpapi.h>

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <shared_mutex>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "Config.h"
#include "../../domain/ports/IConfigStore.h"

namespace tcp_redirector {
namespace infrastructure {

class ConfigManager : public domain::ports::IConfigStore {
public:
    ConfigManager();
    ~ConfigManager() override = default;

    // --- IConfigStore interface ---
    bool Load() override;
    bool Save() override;

    domain::ProxyConfig GetProxyConfig() const override;
    bool SetProxyConfig(const domain::ProxyConfig& config) override;

    std::vector<domain::Rule> GetRules() const override;
    bool SetRules(const std::vector<domain::Rule>& rules) override;

    domain::LogLevel GetLogLevel() const override;
    bool SetLogLevel(domain::LogLevel level) override;

    std::filesystem::path GetLogDirectory() const override;
    uint32_t GetMaxLogFileSizeMB() const override;
    uint32_t GetMaxLogFiles() const override;

    // --- Новый API (Config-ориентированный) ---
    Config GetConfig() const;
    bool UpdateConfig(const Config& newConfig);
    bool UpdateConfigNoSave(const Config& newConfig); // без сохранения на диск

    // --- DPAPI для пароля ---
    std::wstring GetPlainPassword() const;
    void SetPassword(const std::wstring& plainPassword);

    // --- Listener-механизм уведомлений ---
    using ConfigChangeListener = std::function<void(const Config& oldConfig, const Config& newConfig)>;
    uint64_t AddListener(ConfigChangeListener callback);
    void RemoveListener(uint64_t listenerId);

private:
    bool LoadImpl();
    bool SaveImpl();
    bool CreateDefaultConfig();
    void NotifyListeners(const Config& oldCfg, const Config& newCfg);

    // Сериализация/десериализация
    Config JsonToConfig(const nlohmann::json& j) const;
    nlohmann::json ConfigToJson(const Config& cfg) const;
    std::vector<domain::Rule> JsonToRules(const nlohmann::json& j) const;
    nlohmann::json RulesToJson(const std::vector<domain::Rule>& rules) const;

    // DPAPI helpers
    std::string EncryptPassword(const std::wstring& plaintext) const;
    std::wstring DecryptPassword(const std::string& ciphertext) const;
    static std::string Base64Encode(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> Base64Decode(const std::string& data);

    mutable std::shared_mutex m_mutex;

    std::filesystem::path m_configPath;
    Config m_config;
    std::vector<domain::Rule> m_rules;

    // Listener'ы
    std::unordered_map<uint64_t, ConfigChangeListener> m_listeners;
    uint64_t m_nextListenerId = 1;
    mutable std::mutex m_listenersMutex;

    // Константы
    static constexpr size_t MAX_LOG_FILES = 10;
    static constexpr uint32_t MAX_LOG_SIZE_MB = 50;
};

// --- SecretsManager (DPAPI) ---
class SecretsManager : public domain::ports::ISecretsManager {
public:
    std::vector<uint8_t> Encrypt(const std::wstring& plaintext) override;
    std::wstring Decrypt(const std::vector<uint8_t>& ciphertext) override;
};

} // namespace infrastructure
} // namespace tcp_redirector
