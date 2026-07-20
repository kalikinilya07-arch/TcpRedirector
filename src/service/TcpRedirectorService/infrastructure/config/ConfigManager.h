#pragma once

/**
 * @file ConfigManager.h
 * @brief Менеджер конфигурации приложения.
 *
 * Отвечает за загрузку, сохранение и уведомление об изменении
 * конфигурации. Реализует доменный порт IConfigStore.
 * - Загружает/сохраняет config.json РЯДОМ С EXE сервиса
 *   (см. infrastructure/paths/AppPaths.h — WP1).
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
    /**
     * @brief Конструктор по умолчанию: путь к config.json резолвится
     *        через AppPaths::GetConfigPathW() — рядом с EXE сервиса.
     */
    ConfigManager();

    /**
     * @brief Конструктор с явным путём к файлу конфигурации.
     *        Используется только в тестах — обычный код должен использовать
     *        конструктор по умолчанию, чтобы путь резолвился рядом с EXE.
     * @param explicitConfigPath Абсолютный путь к config.json.
     */
    explicit ConfigManager(std::filesystem::path explicitConfigPath);

    ~ConfigManager() override = default;

    /**
     * @brief Результат однократной миграции legacy-конфига.
     */
    struct MigrationResult {
        bool         attempted = false;   //!< Была ли попытка миграции (legacy existed, new missing).
        bool         succeeded = false;   //!< Копирование удалось.
        std::string  message;             //!< Человекочитаемое сообщение для лога (утф-8).
    };

    /**
     * @brief Однократная миграция legacy config из %ProgramData%\TcpRedirector\
     *        в директорию рядом с EXE сервиса.
     *
     * Логика (WP1 §2.3):
     *  1. Резолвит newPath = <exeDir>\config.json и
     *     legacyPath = <%ProgramData%>\TcpRedirector\config.json
     *     (через SHGetKnownFolderPath(FOLDERID_ProgramData)).
     *  2. Если newPath НЕ существует, а legacyPath существует и является
     *     регулярным файлом — копирует legacyPath → newPath
     *     (CopyFileW с bFailIfExists=TRUE). Legacy-файл НЕ удаляется
     *     (сохраняем возможность отката).
     *  3. При любой ошибке — не бросает исключений, только формирует
     *     сообщение в MigrationResult::message. Сервис должен стартовать
     *     даже если миграция не удалась.
     *
     * Должна вызываться один раз при старте сервиса ПЕРЕД созданием
     * ConfigManager с путём по умолчанию. Возвращаемое сообщение вызывающая
     * сторона обязана прологировать после инициализации логгера.
     */
    static MigrationResult EnsureConfigMigrated();

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

    // --- WP3: schema v2 accessors ---

    /**
     * @brief Получить копию списка пер-приложение правил (schema v2 apps[]).
     * @return Вектор AppRule (под shared_lock).
     */
    std::vector<AppRule> GetAppRules() const;

    /**
     * @brief Получить активный режим захвата трафика.
     * @return CaptureMode (WinDivert по умолчанию).
     */
    CaptureMode GetCaptureMode() const;

    /**
     * @brief Получить настройки Wintun-адаптера / tun2socks-движка.
     * @return Копия WintunSettings.
     */
    WintunSettings GetWintunSettings() const;

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
     * @brief (Задача №2) Очистить неактуальные поля секции auth перед записью.
     *
     * Гарантирует, что в config.json не остаются «мусорные» значения для
     * деактивированных настроек авторизации:
     *  • auth выключена            → username/encryptedPassword/password очищаются;
     *  • Kerberos                  → username/encryptedPassword/password очищаются
     *                                (SSPI использует контекст текущей учётной записи);
     *  • Basic + encryptPassword   → password (plaintext) очищается;
     *  • Basic + !encryptPassword  → encryptedPassword (DPAPI) очищается.
     *
     * Вызывается из SaveImpl() (единая точка персиста), поэтому применяется ко
     * всем путям записи конфигурации службой.
     */
    void NormalizeAuthFields();

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

    // --- WP3: schema v2 helpers ---

    /**
     * @brief Разобрать WintunSettings из JSON-объекта c валидацией и fallback-ами.
     *        Никогда не бросает — некорректные поля заменяются дефолтами.
     * @param jw JSON-объект секции "wintun" (может быть null/пустым).
     * @return Заполненная структура WintunSettings.
     */
    WintunSettings ParseWintunSettings(const nlohmann::json& jw) const;

    /**
     * @brief Разобрать один AppRule из JSON-объекта.
     * @param ja JSON-объект элемента "apps[]".
     * @param out Результирующий AppRule (заполняется частично при частично-невалидных полях).
     * @return true, если правило пригодно к использованию; false — пропустить.
     */
    bool ParseAppRule(const nlohmann::json& ja, AppRule& out) const;

    /**
     * @brief Сериализовать WintunSettings в JSON.
     */
    nlohmann::json WintunSettingsToJson(const WintunSettings& w) const;

    /**
     * @brief Сериализовать вектор AppRule в JSON-массив.
     */
    nlohmann::json AppsToJson(const std::vector<AppRule>& apps) const;

    /**
     * @brief Построить legacy-массив rules[] как «зеркало» apps[] для
     *        обратной совместимости со старыми читателями конфига (v1).
     *        route_all_traffic-правила НЕ отражаются: старый читатель их
     *        всё равно применит некорректно (нулевой порт не совпадает
     *        ни с чем).  Диапазоны разворачиваются в отдельные записи
     *        только если суммарное количество записей ≤ 128.
     * @param apps Источник (v2).
     * @return JSON-массив в legacy-формате { exe, port, proxyId }.
     */
    nlohmann::json BuildLegacyRulesMirror(const std::vector<AppRule>& apps) const;

    /**
     * @brief Разобрать legacy-массив rules[] (v1) и синтезировать соответствующие
     *        AppRule для in-memory upgrade v1 → v2.
     *
     * Правило маппинга (см. §3.3 плана и WP3-спеку):
     *   pattern           = r.exe (или пусто);
     *   proxy_id          = r.proxyId (или "default");
     *   route_all_traffic = false;
     *   ports             = [r.port] если port валиден, иначе [];
     *   port_ranges       = [].
     *
     * @param jrules Массив rules[] из v1-файла.
     * @return Синтезированные AppRule (могут быть пустыми).
     */
    std::vector<AppRule> UpgradeLegacyRulesToApps(const nlohmann::json& jrules) const;

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
    Config m_config;                     //!< Текущая конфигурация (schema v2)
    std::vector<domain::Rule> m_rules;   //!< Legacy rules[] (в v2 остаётся как «зеркало»)

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
