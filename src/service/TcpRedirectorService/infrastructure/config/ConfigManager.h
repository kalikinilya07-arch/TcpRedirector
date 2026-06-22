#pragma once

/**
 * @file ConfigManager.h
 * @brief Менеджер конфигурации приложения.
 *
 * Отвечает за загрузку, сохранение и уведомление об изменении
 * конфигурации. Реализует доменный порт IConfigStore.
 * - Загружает/сохраняет config.json в %ProgramData%\TcpRedirector\
 * - Потокобезопасен (shared_mutex)
 * - Уведомляет listener'ов при изменении конфига
 * - Шифрует пароль через DPAPI (SecretsManager)
 * - Совместим с IConfigStore интерфейсом
 */

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

/**
 * @brief Менеджер конфигурации приложения.
 *
 * Реализует IConfigStore. Обеспечивает потокобезопасный доступ
 * к конфигурации, сериализацию в JSON, шифрование паролей
 * через DPAPI и механизм уведомления подписчиков об изменениях.
 */
class ConfigManager : public domain::ports::IConfigStore {
public:
    ConfigManager();
    ~ConfigManager() override = default;

    // --- IConfigStore interface ---

    /**
     * @brief Загрузить конфигурацию из файла.
     * @return true, если загрузка успешна.
     */
    bool Load() override;

    /**
     * @brief Сохранить конфигурацию в файл.
     * @return true, если сохранение успешно.
     */
    bool Save() override;

    /**
     * @brief Получить конфигурацию прокси.
     * @return Структура ProxyConfig.
     */
    domain::ProxyConfig GetProxyConfig() const override;

    /**
     * @brief Установить конфигурацию прокси.
     * @param config Новые параметры прокси.
     * @return true, если сохранено.
     */
    bool SetProxyConfig(const domain::ProxyConfig& config) override;

    /**
     * @brief Получить список правил.
     * @return Вектор правил.
     */
    std::vector<domain::Rule> GetRules() const override;

    /**
     * @brief Установить список правил.
     * @param rules Вектор правил.
     * @return true, если сохранено.
     */
    bool SetRules(const std::vector<domain::Rule>& rules) override;

    /**
     * @brief Получить уровень логирования.
     * @return Текущий уровень.
     */
    domain::LogLevel GetLogLevel() const override;

    /**
     * @brief Установить уровень логирования.
     * @param level Новый уровень.
     * @return true, если сохранено.
     */
    bool SetLogLevel(domain::LogLevel level) override;

    /**
     * @brief Получить директорию логов.
     * @return Путь к директории.
     */
    std::filesystem::path GetLogDirectory() const override;

    /**
     * @brief Получить макс. размер лог-файла в МБ.
     * @return Размер в МБ.
     */
    uint32_t GetMaxLogFileSizeMB() const override;

    /**
     * @brief Получить макс. количество лог-файлов.
     * @return Количество файлов.
     */
    uint32_t GetMaxLogFiles() const override;

    // --- Новый API (Config-ориентированный) ---

    /**
     * @brief Получить полную конфигурацию.
     * @return Структура Config.
     */
    Config GetConfig() const;

    /**
     * @brief Обновить конфигурацию и сохранить на диск.
     * @param newConfig Новая конфигурация.
     * @return true, если успешно.
     */
    bool UpdateConfig(const Config& newConfig);

    /**
     * @brief Обновить конфигурацию без сохранения на диск.
     * @param newConfig Новая конфигурация.
     * @return true, если успешно.
     */
    bool UpdateConfigNoSave(const Config& newConfig);

    // --- DPAPI для пароля ---

    /**
     * @brief Получить пароль в открытом виде (расшифрованный).
     * @return Пароль или пустая строка.
     */
    std::wstring GetPlainPassword() const;

    /**
     * @brief Зашифровать и сохранить пароль.
     * @param plainPassword Пароль в открытом виде.
     */
    void SetPassword(const std::wstring& plainPassword);

    // --- Listener-механизм уведомлений ---

    /** Тип коллбэка уведомления об изменении конфигурации. */
    using ConfigChangeListener = std::function<void(const Config& oldConfig, const Config& newConfig)>;

    /**
     * @brief Добавить подписчика на изменения конфигурации.
     * @param callback Функция обратного вызова.
     * @return Идентификатор подписчика.
     */
    uint64_t AddListener(ConfigChangeListener callback);

    /**
     * @brief Удалить подписчика.
     * @param listenerId Идентификатор подписчика.
     */
    void RemoveListener(uint64_t listenerId);

private:
    /**
     * @brief Внутренняя реализация загрузки.
     * @return true, если успешно.
     */
    bool LoadImpl();

    /**
     * @brief Внутренняя реализация сохранения.
     * @return true, если успешно.
     */
    bool SaveImpl();

    /**
     * @brief Создать конфигурацию по умолчанию.
     * @return true, если успешно.
     */
    bool CreateDefaultConfig();

    /**
     * @brief Уведомить всех подписчиков об изменении.
     * @param oldCfg Старая конфигурация.
     * @param newCfg Новая конфигурация.
     */
    void NotifyListeners(const Config& oldCfg, const Config& newCfg);

    // --- Сериализация/десериализация ---

    /**
     * @brief Преобразовать JSON в Config.
     * @param j JSON-объект.
     * @return Структура Config.
     */
    Config JsonToConfig(const nlohmann::json& j) const;

    /**
     * @brief Преобразовать Config в JSON.
     * @param cfg Структура конфигурации.
     * @return JSON-объект.
     */
    nlohmann::json ConfigToJson(const Config& cfg) const;

    /**
     * @brief Преобразовать JSON в список правил.
     * @param j JSON-объект с правилами.
     * @return Вектор правил.
     */
    std::vector<domain::Rule> JsonToRules(const nlohmann::json& j) const;

    /**
     * @brief Преобразовать список правил в JSON.
     * @param rules Вектор правил.
     * @return JSON-массив.
     */
    nlohmann::json RulesToJson(const std::vector<domain::Rule>& rules) const;

    // --- DPAPI helpers ---

    /**
     * @brief Зашифровать пароль через DPAPI и закодировать в Base64.
     * @param plaintext Пароль в открытом виде.
     * @return Base64-строка зашифрованных данных.
     */
    std::string EncryptPassword(const std::wstring& plaintext) const;

    /**
     * @brief Расшифровать пароль из Base64 через DPAPI.
     * @param ciphertext Base64-строка зашифрованных данных.
     * @return Пароль в открытом виде.
     */
    std::wstring DecryptPassword(const std::string& ciphertext) const;

    /**
     * @brief Закодировать бинарные данные в Base64.
     * @param data Вектор байт.
     * @return Строка в Base64.
     */
    static std::string Base64Encode(const std::vector<uint8_t>& data);

    /**
     * @brief Декодировать строку из Base64.
     * @param data Строка в Base64.
     * @return Вектор байт.
     */
    static std::vector<uint8_t> Base64Decode(const std::string& data);

    mutable std::shared_mutex m_mutex;   //!< Мьютекс для потокобезопасного доступа

    std::filesystem::path m_configPath;  //!< Путь к файлу конфигурации
    Config m_config;                     //!< Текущая конфигурация
    std::vector<domain::Rule> m_rules;   //!< Текущие правила

    // Listener'ы
    std::unordered_map<uint64_t, ConfigChangeListener> m_listeners; //!< Карта подписчиков
    uint64_t m_nextListenerId = 1;       //!< Счётчик ID подписчиков
    mutable std::mutex m_listenersMutex; //!< Мьютекс для listener'ов

    // Константы
    static constexpr size_t MAX_LOG_FILES = 10;    //!< Максимум файлов лога
    static constexpr uint32_t MAX_LOG_SIZE_MB = 50; //!< Макс. размер лога (МБ)
};

/**
 * @brief Менеджер секретов с использованием DPAPI.
 *
 * Реализует доменный порт ISecretsManager.
 * Использует CryptProtectData / CryptUnprotectData
 * для шифрования и дешифрования паролей.
 */
class SecretsManager : public domain::ports::ISecretsManager {
public:
    /**
     * @brief Зашифровать пароль через DPAPI.
     * @param plaintext Пароль.
     * @return Зашифрованные данные.
     */
    std::vector<uint8_t> Encrypt(const std::wstring& plaintext) override;

    /**
     * @brief Расшифровать пароль через DPAPI.
     * @param ciphertext Зашифрованные данные.
     * @return Пароль в открытом виде.
     */
    std::wstring Decrypt(const std::vector<uint8_t>& ciphertext) override;
};

} // namespace infrastructure
} // namespace tcp_redirector
