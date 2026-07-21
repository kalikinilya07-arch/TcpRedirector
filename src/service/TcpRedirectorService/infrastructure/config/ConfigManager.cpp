/**
 * @file ConfigManager.cpp
 * @brief Реализация менеджера конфигурации приложения.
 *
 * Содержит логику загрузки/сохранения JSON-конфигурации,
 * шифрования/дешифрования паролей через DPAPI,
 * сериализации правил и уведомления подписчиков.
 *
 * @author TcpRedirector Team
 */

#include "ConfigManager.h"
#include "../utf8_convert.h"
#include "../paths/AppPaths.h"
#include <cstdio>
#include <algorithm>
#include <set>
#include <shlobj.h>       // SHGetKnownFolderPath, FOLDERID_ProgramData
#include <knownfolders.h> // FOLDERID_ProgramData GUID

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace tcp_redirector {
namespace infrastructure {

// ====================================================================
// Конструктор ConfigManager
// ====================================================================

ConfigManager::ConfigManager() {
    // WP1: путь резолвится через AppPaths, единый способ для всего сервиса.
    // Наличие %ProgramData%-fallback удалено — legacy-миграция выполняется
    // в EnsureConfigMigrated() отдельным разовым шагом при старте сервиса.
    m_configPath = std::filesystem::path(paths::GetConfigPathW());
}

ConfigManager::ConfigManager(std::filesystem::path explicitConfigPath)
    : m_configPath(std::move(explicitConfigPath)) {
    // Явный путь используется только тестами; никакой fallback-логики нет.
}

// ====================================================================
// EnsureConfigMigrated — однократная миграция из %ProgramData% (WP1 §2.3)
// ====================================================================

ConfigManager::MigrationResult ConfigManager::EnsureConfigMigrated() {
    MigrationResult result;

    try {
        // 1. newPath = <exeDir>\config.json
        std::filesystem::path newPath;
        try {
            newPath = std::filesystem::path(paths::GetConfigPathW());
        } catch (const std::exception& e) {
            result.message = std::string("Config migration skipped: cannot resolve exe dir: ") + e.what();
            return result;
        }

        // 2. legacyPath = <%ProgramData%>\TcpRedirector\config.json
        PWSTR programData = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData,
                                          KF_FLAG_DEFAULT, nullptr, &programData);
        if (FAILED(hr) || !programData) {
            if (programData) CoTaskMemFree(programData);
            result.message = "Config migration skipped: SHGetKnownFolderPath(FOLDERID_ProgramData) failed, HRESULT=0x"
                             + std::to_string(static_cast<unsigned long>(hr));
            return result;
        }
        std::filesystem::path legacyPath =
            std::filesystem::path(programData) / L"TcpRedirector" / L"config.json";
        CoTaskMemFree(programData);

        std::error_code ec;

        // Быстрый выход: новый файл уже существует.
        if (std::filesystem::exists(newPath, ec)) {
            // Ничего не логируем — не хочется засорять лог на каждом старте.
            return result;
        }

        // Legacy отсутствует или не файл — нечего мигрировать.
        if (!std::filesystem::exists(legacyPath, ec) ||
            !std::filesystem::is_regular_file(legacyPath, ec)) {
            return result;
        }

        // Миграция.
        result.attempted = true;
        // CopyFileW с bFailIfExists=TRUE. Дубликат existence-проверки выше нам не мешает —
        // защита от гонки на случай, если другой процесс создал файл в промежутке.
        BOOL ok = CopyFileW(legacyPath.c_str(), newPath.c_str(), TRUE /*bFailIfExists*/);
        if (ok) {
            result.succeeded = true;
            result.message = "Migrated legacy config from %ProgramData% to "
                             + WideToUtf8(newPath.wstring());
        } else {
            DWORD gle = GetLastError();
            result.message = "Config migration failed: CopyFileW('"
                             + WideToUtf8(legacyPath.wstring()) + "' -> '"
                             + WideToUtf8(newPath.wstring())
                             + "'), GLE=" + std::to_string(gle);
        }
    } catch (const std::exception& e) {
        result.message = std::string("Config migration threw exception: ") + e.what();
    } catch (...) {
        result.message = "Config migration threw unknown exception";
    }

    return result;
}

// ====================================================================
// IConfigStore: Load / Save
// ====================================================================

bool ConfigManager::Load() {
    std::unique_lock lock(m_mutex);
    return LoadImpl();
}

bool ConfigManager::Save() {
    std::unique_lock lock(m_mutex);
    return SaveImpl();
}

// ====================================================================
// IConfigStore: ProxyConfig
// ====================================================================

domain::ProxyConfig ConfigManager::GetProxyConfig() const {
    std::shared_lock lock(m_mutex);
    domain::ProxyConfig pc;
    pc.host = Utf8ToWide(m_config.proxy.host);
    pc.port = m_config.proxy.port;
    pc.auth_required = m_config.auth.enabled;
    pc.kerberos_auth = m_config.auth.kerberos;
    pc.login = Utf8ToWide(m_config.auth.username);

    // (Задача №2) Пароль: два режима хранения.
    //  • encryptPassword=false → пароль хранится/используется «как есть» в
    //    открытом поле auth.password (в обход DPAPI). Снимает класс проблем
    //    «DPAPI-blob не расшифровался под учёткой службы → Basic-auth падает».
    //  • encryptPassword=true (по умолчанию) → прежняя логика: расшифровка
    //    auth.encryptedPassword через DPAPI.
    if (!m_config.auth.encryptPassword) {
        pc.has_password = !m_config.auth.password.empty();
        if (pc.has_password) {
            pc.plain_password = Utf8ToWide(m_config.auth.password);
        }
    } else {
        pc.has_password = !m_config.auth.encryptedPassword.empty();
        if (pc.has_password) {
            pc.plain_password = DecryptPassword(m_config.auth.encryptedPassword);
        }
    }

    // --- Per-user Kerberos auth helper (Variant 4b, Phase 0) ---
    // Переносим в доменный ProxyConfig. На Phase 0 эти поля никем не читаются
    // в auth/relay-логике (проводка — в поздних фазах), но уже доступны через
    // единый доменный интерфейс конфигурации.
    pc.per_user_auth_enabled = m_config.auth.per_user_enabled;
    pc.auth_spn = Utf8ToWide(m_config.auth.spn);
    pc.helper_timeout_ms = m_config.auth.helper_timeout_ms;
    switch (m_config.auth.fallback_policy) {
        case AuthFallbackPolicy::System:
            pc.fallback_policy = domain::AuthFallbackPolicy::System; break;
        case AuthFallbackPolicy::Error:
            pc.fallback_policy = domain::AuthFallbackPolicy::Error; break;
        case AuthFallbackPolicy::Drop:
        default:
            pc.fallback_policy = domain::AuthFallbackPolicy::Drop; break;
    }
    return pc;
}

bool ConfigManager::SetProxyConfig(const domain::ProxyConfig& config) {
    std::unique_lock lock(m_mutex);
    Config oldCfg = m_config;
    m_config.proxy.host = WideToUtf8(config.host);
    m_config.proxy.port = config.port;
    m_config.proxy.enabled = true;
    m_config.auth.enabled = config.auth_required;
    m_config.auth.kerberos = config.kerberos_auth;
    m_config.auth.username = WideToUtf8(config.login);
    if (config.has_password && !m_config.auth.encryptedPassword.empty()) {
        // пароль уже зашифрован — оставляем
    }
    NotifyListeners(oldCfg, m_config);
    return true;  // config.json is persisted by GUI via WriteFull()
}

// ====================================================================
// IConfigStore: Rules
// ====================================================================

std::vector<domain::Rule> ConfigManager::GetRules() const {
    std::shared_lock lock(m_mutex);
    return m_rules;
}

bool ConfigManager::SetRules(const std::vector<domain::Rule>& rules) {
    std::unique_lock lock(m_mutex);
    m_rules = rules;
    return true;  // config.json is persisted by GUI via WriteFull()
}

// ====================================================================
// IConfigStore: Logging
// ====================================================================

domain::LogLevel ConfigManager::GetLogLevel() const {
    std::shared_lock lock(m_mutex);
    // Файловая шкала службы: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=TRACE.
    // Уровень 4 (TRACE) — экспертный: включает максимально подробную
    // диагностику (в т.ч. Wintun rx-stats/PROXY-трейс). GUI пишет 0..3;
    // значение 4 доступно ручной правкой config.json.
    switch (m_config.log.level) {
        case 0: return domain::LogLevel::Error;
        case 1: return domain::LogLevel::Warn;
        case 2: return domain::LogLevel::Info;
        case 3: return domain::LogLevel::Debug;
        case 4: return domain::LogLevel::Trace;
        default: return domain::LogLevel::Info;
    }
}

bool ConfigManager::SetLogLevel(domain::LogLevel level) {
    std::unique_lock lock(m_mutex);
    Config oldCfg = m_config;
    switch (level) {
        case domain::LogLevel::Error: m_config.log.level = 0; break;
        case domain::LogLevel::Warn:  m_config.log.level = 1; break;
        case domain::LogLevel::Info:  m_config.log.level = 2; break;
        case domain::LogLevel::Debug: m_config.log.level = 3; break;
        case domain::LogLevel::Trace: m_config.log.level = 4; break;
        default: m_config.log.level = 2; break;
    }
    if (SaveImpl()) {
        NotifyListeners(oldCfg, m_config);
        return true;
    }
    return false;
}

std::filesystem::path ConfigManager::GetLogDirectory() const {
    // WP1: логи теперь рядом с EXE, в <exeDir>\logs\.
    // %ProgramData%-fallback удалён.
    try {
        return std::filesystem::path(paths::GetLogDirectoryW());
    } catch (...) {
        // Крайне маловероятно: если GetModuleFileNameW сломан, возвращаем
        // текущую рабочую директорию + logs как последний рубеж, чтобы
        // логгер не крашнулся. Ошибка уже была прологирована выше по стеку.
        return std::filesystem::current_path() / L"logs";
    }
}

uint32_t ConfigManager::GetMaxLogFileSizeMB() const {
    return MAX_LOG_SIZE_MB;
}

uint32_t ConfigManager::GetMaxLogFiles() const {
    return MAX_LOG_FILES;
}

// ====================================================================
// Новый Config-ориентированный API
// ====================================================================

Config ConfigManager::GetConfig() const {
    std::shared_lock lock(m_mutex);
    return m_config;
}

bool ConfigManager::UpdateConfig(const Config& newConfig) {
    std::unique_lock lock(m_mutex);
    Config oldCfg = m_config;

    // Если exePath изменился — пересчитывать exeName не нужно (вычисляется на лету)
    // Если пароль изменился — шифруем
    if (newConfig.auth.encryptedPassword.empty() &&
        !m_config.auth.encryptedPassword.empty()) {
        // пароль не передан — сохраняем старый зашифрованный
        Config tmp = newConfig;
        tmp.auth.encryptedPassword = m_config.auth.encryptedPassword;
        m_config = tmp;
    } else {
        m_config = newConfig;
    }

    if (SaveImpl()) {
        NotifyListeners(oldCfg, m_config);
        return true;
    }
    return false;
}

bool ConfigManager::UpdateConfigNoSave(const Config& newConfig) {
    std::unique_lock lock(m_mutex);
    Config oldCfg = m_config;
    m_config = newConfig;
    NotifyListeners(oldCfg, m_config);
    return true;
}

// ====================================================================
// DPAPI для пароля
// ====================================================================

std::wstring ConfigManager::GetPlainPassword() const {
    std::shared_lock lock(m_mutex);
    // (Задача №2) При encryptPassword=false пароль лежит открытым текстом.
    if (!m_config.auth.encryptPassword) {
        return Utf8ToWide(m_config.auth.password);
    }
    if (m_config.auth.encryptedPassword.empty())
        return {};
    return DecryptPassword(m_config.auth.encryptedPassword);
}

void ConfigManager::SetPassword(const std::wstring& plainPassword) {
    std::unique_lock lock(m_mutex);
    // (Задача №2) Уважаем режим хранения пароля.
    if (!m_config.auth.encryptPassword) {
        // Храним «как есть» (plaintext). Зашифрованное поле очищаем, чтобы
        // не путать источники истины.
        m_config.auth.password = WideToUtf8(plainPassword);
        m_config.auth.encryptedPassword.clear();
    } else {
        m_config.auth.encryptedPassword = EncryptPassword(plainPassword);
        m_config.auth.password.clear();
    }
    SaveImpl();
}

// ====================================================================
// Listener-механизм уведомлений
// ====================================================================

uint64_t ConfigManager::AddListener(ConfigChangeListener callback) {
    std::lock_guard lock(m_listenersMutex);
    uint64_t id = m_nextListenerId++;
    m_listeners[id] = std::move(callback);
    return id;
}

void ConfigManager::RemoveListener(uint64_t listenerId) {
    std::lock_guard lock(m_listenersMutex);
    m_listeners.erase(listenerId);
}

// ====================================================================
// Приватные методы: LoadImpl / SaveImpl / CreateDefaultConfig
// ====================================================================

bool ConfigManager::LoadImpl() {
    try {
        if (!std::filesystem::exists(m_configPath)) {
            return CreateDefaultConfig();
        }
        std::ifstream file(m_configPath);
        if (!file.is_open()) return false;

        nlohmann::json j;
        file >> j;

        // -------- WP3: определение версии схемы --------
        // v1 не содержит поля "config_version"; в v2 оно == 2.
        // Промежуточные/будущие значения обрабатываем как «не хуже v2».
        int schema = j.value("config_version", 0);
        const bool isV1 = (schema < 2);

        // Сброс v2-полей на дефолты — LoadImpl может вызываться повторно.
        // Дефолт capture_mode — Wintun (embedded): применяется, если ключ
        // отсутствует в config.json.
        m_config.config_version = 2;
        m_config.capture_mode   = CaptureMode::Wintun;
        m_config.wintun         = WintunSettings{};
        m_config.apps.clear();
        m_config.ipc            = IpcSettings{};   // (Задача №1) сброс IPC-настроек

        // -------- Загрузка v1-секций (без регресса) --------
        if (j.contains("app") && j["app"].is_object()) {
            auto& a = j["app"];
            std::string tmpExe = a.value("exePath", std::string());
            m_config.app.exePath = Utf8ToWide(tmpExe);
        }
        if (j.contains("proxy") && j["proxy"].is_object()) {
            auto& p = j["proxy"];
            m_config.proxy.host = p.value("host", std::string("127.0.0.1"));
            m_config.proxy.port = static_cast<uint16_t>(p.value("port", 3128));
            m_config.proxy.enabled = p.value("enabled", true);
        }
        if (j.contains("auth") && j["auth"].is_object()) {
            auto& a = j["auth"];
            m_config.auth.enabled = a.value("enabled", false);
            m_config.auth.username = a.value("username", std::string());
            m_config.auth.encryptedPassword = a.value("encryptedPassword", std::string());
            m_config.auth.kerberos = a.value("kerberos", false);
            // (Задача №2) Режим хранения пароля. Дефолт encryptPassword=true —
            // обратная совместимость: старые конфиги без поля продолжают
            // использовать DPAPI-encryptedPassword.
            m_config.auth.encryptPassword = a.value("encryptPassword", true);
            m_config.auth.password = a.value("password", std::string());

            // --- Per-user Kerberos auth helper (Variant 4b, Phase 0) ---
            // Все поля опциональны: старые конфиги без них загружаются с
            // безопасными дефолтами (per_user_enabled=false, fallback=drop,
            // helper_timeout_ms=5000, spn=""). Поведение auth/relay не меняется.
            m_config.auth.per_user_enabled = a.value("per_user_enabled", false);
            m_config.auth.spn = a.value("spn", std::string());
            m_config.auth.helper_timeout_ms = a.value("helper_timeout_ms", 5000);
            {
                // fallback_policy: строка "drop" | "system" | "error".
                // Неизвестное/отсутствующее значение → безопасный дефолт Drop.
                std::string fp = a.value("fallback_policy", std::string("drop"));
                AuthFallbackPolicy parsed = AuthFallbackPolicy::Drop;
                if (!AuthFallbackPolicyFromString(fp, parsed)) {
                    std::fprintf(stderr,
                        "[WARN] ConfigManager: unknown auth.fallback_policy '%s', coercing to 'drop'\n",
                        fp.c_str());
                    parsed = AuthFallbackPolicy::Drop;
                }
                m_config.auth.fallback_policy = parsed;
            }
        }
        if (j.contains("log") && j["log"].is_object()) {
            auto& l = j["log"];
            m_config.log.level = l.value("level", 2);
            m_config.log.fileEnabled = l.value("fileEnabled", true);
            m_config.log.maxSizeMB = l.value("maxSizeMB", 50);
        }
        if (j.contains("stats") && j["stats"].is_object()) {
            auto& s = j["stats"];
            m_config.stats.updateIntervalMs = s.value("updateIntervalMs", 2000);
        }
        // (Задача №1) IPC-настройки. Дефолт auth_enabled=true — обратная
        // совместимость: без секции ipc поведение прежнее (токен требуется).
        if (j.contains("ipc") && j["ipc"].is_object()) {
            auto& i = j["ipc"];
            m_config.ipc.auth_enabled = i.value("auth_enabled", true);
        }

        // -------- Legacy rules[] --------
        // В обеих версиях парсим legacy-массив (в v2 он «зеркало», хранимое
        // ради обратной совместимости со старыми читателями).
        if (j.contains("rules")) {
            m_rules = JsonToRules(j);
        } else {
            m_rules.clear();
        }

        // -------- v2 новые секции --------
        // capture_mode
        if (j.contains("capture_mode")) {
            if (j["capture_mode"].is_string()) {
                CaptureMode cm;
                if (CaptureModeFromString(j["capture_mode"].get<std::string>(), cm)) {
                    m_config.capture_mode = cm;
                } else {
                    std::fprintf(stderr,
                        "[WARN] ConfigManager: unknown capture_mode '%s', coercing to 'wintun'\n",
                        j["capture_mode"].get<std::string>().c_str());
                    m_config.capture_mode = CaptureMode::Wintun;
                }
            }
        }
        // wintun{...}
        if (j.contains("wintun") && j["wintun"].is_object()) {
            m_config.wintun = ParseWintunSettings(j["wintun"]);
        }
        // apps[]
        if (j.contains("apps") && j["apps"].is_array()) {
            for (const auto& ja : j["apps"]) {
                if (!ja.is_object()) continue;
                AppRule rule;
                if (ParseAppRule(ja, rule)) {
                    m_config.apps.push_back(std::move(rule));
                }
            }
        }

        // -------- v1 → v2 in-memory upgrade --------
        // Если это v1 (нет поля config_version) — синтезируем apps[] из legacy rules[].
        // Условие «apps не задан» проверяем по итоговому вектору, а не по факту
        // отсутствия ключа: v1-файл его иметь не может.
        if (isV1 && m_config.apps.empty()) {
            m_config.apps = UpgradeLegacyRulesToApps(j.value("rules", nlohmann::json::array()));
        }

        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[ERROR] ConfigManager::LoadImpl exception: %s\n", e.what());
        return false;
    } catch (...) {
        std::fprintf(stderr,
            "[ERROR] ConfigManager::LoadImpl: unknown exception\n");
        return false;
    }
}

void ConfigManager::NormalizeAuthFields() {
    // (Задача №2) Единая нормализация auth-полей перед персистом. Держим в
    // config.json только актуальные для текущего режима значения, очищая
    // «мусор» от деактивированных настроек (иначе при пересохранении, напр.
    // после переключения Basic↔Kerberos или смены encryptPassword без
    // повторного ввода пароля, старые поля оставались в файле).
    auto& auth = m_config.auth;
    if (!auth.enabled || auth.kerberos) {
        // Авторизация выключена, либо Kerberos (SSPI, креды не хранятся).
        auth.username.clear();
        auth.encryptedPassword.clear();
        auth.password.clear();
    } else if (auth.encryptPassword) {
        // Basic + шифрование: plaintext-поле неактуально.
        auth.password.clear();
    } else {
        // Basic + plaintext: DPAPI-поле неактуально.
        auth.encryptedPassword.clear();
    }
}

bool ConfigManager::SaveImpl() {
    // WP1: не «глотать» ошибки записи молча — печатаем причину в stderr.
    // Логгер здесь недоступен (ConfigManager может быть создан до Logger).
    // (Задача №2) Перед записью очищаем неактуальные auth-поля.
    NormalizeAuthFields();
    try {
        std::error_code ec;
        auto parent = m_configPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::fprintf(stderr,
                    "[ERROR] ConfigManager: cannot create config directory '%s': %s\n",
                    parent.string().c_str(), ec.message().c_str());
                // create_directories возвращает ec, но директория могла уже существовать —
                // не бросаем, а пытаемся писать; if truly missing, ofstream ниже упадёт.
            }
        }

        // WP3: v2 file layout — version marker первым полем.
        // nlohmann::json неупорядочен, но dump() пишет в порядке insertion,
        // так что порядок ключей ниже определяет порядок в файле.
        nlohmann::json j;
        j["config_version"] = 2;
        j["capture_mode"]   = CaptureModeToString(m_config.capture_mode);

        // v1-совместимые секции
        j["app"]["exePath"] = WideToUtf8(m_config.app.exePath);
        j["proxy"]["host"] = m_config.proxy.host;
        j["proxy"]["port"] = m_config.proxy.port;
        j["proxy"]["enabled"] = m_config.proxy.enabled;
        j["auth"]["enabled"] = m_config.auth.enabled;
        j["auth"]["username"] = m_config.auth.username;
        j["auth"]["encryptedPassword"] = m_config.auth.encryptedPassword;
        j["auth"]["kerberos"] = m_config.auth.kerberos;
        // (Задача №2) Режим хранения пароля.
        j["auth"]["encryptPassword"] = m_config.auth.encryptPassword;
        j["auth"]["password"] = m_config.auth.password;
        // Per-user Kerberos auth helper (Variant 4b, Phase 0).
        j["auth"]["per_user_enabled"] = m_config.auth.per_user_enabled;
        j["auth"]["spn"] = m_config.auth.spn;
        j["auth"]["fallback_policy"] = AuthFallbackPolicyToString(m_config.auth.fallback_policy);
        j["auth"]["helper_timeout_ms"] = m_config.auth.helper_timeout_ms;
        j["log"]["level"] = m_config.log.level;
        j["log"]["fileEnabled"] = m_config.log.fileEnabled;
        j["log"]["maxSizeMB"] = m_config.log.maxSizeMB;
        j["stats"]["updateIntervalMs"] = m_config.stats.updateIntervalMs;
        // (Задача №1) IPC-настройки.
        j["ipc"]["auth_enabled"] = m_config.ipc.auth_enabled;

        // v2: wintun{...} и apps[]
        j["wintun"] = WintunSettingsToJson(m_config.wintun);
        j["apps"]   = AppsToJson(m_config.apps);

        // Legacy mirror: rules[] строится из apps[] для обратной совместимости
        // со старыми (v1-only) читателями конфига.  Приоритет отдаём авто-«зеркалу»,
        // если apps[] непусты; иначе оставляем то, что уже лежит в m_rules
        // (unmodified v1 запись).
        if (!m_config.apps.empty()) {
            j["rules"] = BuildLegacyRulesMirror(m_config.apps);
        } else {
            j["rules"] = RulesToJson(m_rules);
        }

        // Atomic write: write to temp file, then rename
        auto tmpPath = m_configPath;
        tmpPath += L".tmp";
        {
            std::ofstream file(tmpPath);
            if (!file.is_open()) {
                std::fprintf(stderr,
                    "[ERROR] ConfigManager: cannot open '%s' for writing (errno=%d)\n",
                    tmpPath.string().c_str(), errno);
                return false;
            }
            file << j.dump(4);
            if (!file.good()) {
                std::fprintf(stderr,
                    "[ERROR] ConfigManager: write to '%s' failed (errno=%d)\n",
                    tmpPath.string().c_str(), errno);
                return false;
            }
        }
        std::filesystem::rename(tmpPath, m_configPath, ec);
        if (ec) {
            std::fprintf(stderr,
                "[ERROR] ConfigManager: rename '%s' -> '%s' failed: %s\n",
                tmpPath.string().c_str(), m_configPath.string().c_str(),
                ec.message().c_str());
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[ERROR] ConfigManager::SaveImpl exception: %s\n", e.what());
        return false;
    } catch (...) {
        std::fprintf(stderr, "[ERROR] ConfigManager::SaveImpl: unknown exception\n");
        return false;
    }
}

bool ConfigManager::CreateDefaultConfig() {
    // WP3: дефолты v2.  Все новые поля инициализированы конструкторами struct'ов;
    // здесь только те, что должны отличаться от dev-нейтральных значений.
    m_config = Config();
    m_config.config_version = 2;
    // Дефолт после установки/создания конфига — Wintun (embedded).
    m_config.capture_mode   = CaptureMode::Wintun;
    m_config.app.exePath = L"C:\\Projects\\china\\police_sec\\TransfersClient.exe";
    m_config.proxy.host = "127.0.0.1";
    m_config.proxy.port = 8888;
    m_config.proxy.enabled = true;
    // WintunSettings и ExternalEngineSettings берут дефолты из Config.h.

    // Legacy rules[] — одно правило для дефолтного приложения.
    domain::Rule defaultRule;
    defaultRule.id = "default";
    defaultRule.type = domain::RuleType::ProcessName;
    defaultRule.action = domain::RuleAction::Proxy;
    defaultRule.pattern = L"TransfersClient.exe";
    defaultRule.description = L"Redirect TcpRedirector traffic";
    defaultRule.priority = 1;
    defaultRule.enabled = true;
    m_rules.clear();
    m_rules.push_back(defaultRule);

    // v2 apps[] — синтезируем из того же правила, чтобы новый читатель
    // (RuleEngine, WP4) имел непустой список.
    AppRule defaultApp;
    defaultApp.pattern           = "TransfersClient.exe";
    defaultApp.proxy_id          = "default";
    defaultApp.route_all_traffic = false;
    defaultApp.ports.clear();       // Порты не заданы — правило будет warn'ить в LoadImpl.
    m_config.apps.clear();
    m_config.apps.push_back(std::move(defaultApp));

    return SaveImpl();
}

void ConfigManager::NotifyListeners(const Config& oldCfg, const Config& newCfg) {
    std::lock_guard lock(m_listenersMutex);
    for (auto& [id, cb] : m_listeners) {
        if (cb) cb(oldCfg, newCfg);
    }
}

// ====================================================================
// Сериализация / Десериализация (JSON ↔ структуры)
// ====================================================================

Config ConfigManager::JsonToConfig(const nlohmann::json& j) const {
    Config cfg;
    (void)j;
    // используется в LoadImpl напрямую
    return cfg;
}

nlohmann::json ConfigManager::ConfigToJson(const Config& cfg) const {
    nlohmann::json j;
    j["app"]["exePath"] = WideToUtf8(cfg.app.exePath);
    j["proxy"]["host"] = cfg.proxy.host;
    j["proxy"]["port"] = cfg.proxy.port;
    j["proxy"]["enabled"] = cfg.proxy.enabled;
    j["auth"]["enabled"] = cfg.auth.enabled;
    j["auth"]["username"] = cfg.auth.username;
    j["auth"]["encryptedPassword"] = cfg.auth.encryptedPassword;
    j["auth"]["kerberos"] = cfg.auth.kerberos;
    j["auth"]["encryptPassword"] = cfg.auth.encryptPassword;  // (Задача №2)
    j["auth"]["password"] = cfg.auth.password;                // (Задача №2)
    // Per-user Kerberos auth helper (Variant 4b, Phase 0).
    j["auth"]["per_user_enabled"] = cfg.auth.per_user_enabled;
    j["auth"]["spn"] = cfg.auth.spn;
    j["auth"]["fallback_policy"] = AuthFallbackPolicyToString(cfg.auth.fallback_policy);
    j["auth"]["helper_timeout_ms"] = cfg.auth.helper_timeout_ms;
    j["log"]["level"] = cfg.log.level;
    j["log"]["fileEnabled"] = cfg.log.fileEnabled;
    j["log"]["maxSizeMB"] = cfg.log.maxSizeMB;
    j["stats"]["updateIntervalMs"] = cfg.stats.updateIntervalMs;
    j["ipc"]["auth_enabled"] = cfg.ipc.auth_enabled;          // (Задача №1)
    return j;
}

std::vector<domain::Rule> ConfigManager::JsonToRules(const nlohmann::json& j) const {
    std::vector<domain::Rule> rules;
    if (!j.contains("rules")) return rules;
    for (const auto& r : j["rules"]) {
        domain::Rule rule;
        rule.id = r.value("id", "");
        {
            // Tolerant name read: prefer "pattern", fall back to the legacy
            // "exe" key the GUI mirror writes. A hard r["pattern"].get<string>()
            // throws type_error.302 when the key is absent (null), which used to
            // abort LoadImpl and drop the service onto WinDivert defaults.
            std::string tmp = r.value("pattern", std::string());
            if (tmp.empty()) tmp = r.value("exe", std::string());
            if (tmp.empty()) continue;  // skip malformed/nameless entries
            rule.pattern = Utf8ToWide(tmp);
        }
        {
            std::string tmp = r.value("description", std::string());
            rule.description = Utf8ToWide(tmp);
        }
        rule.priority = r.value("priority", 0);
        rule.enabled = r.value("enabled", true);
        std::string type_str = r.value("type", "process_name");
        if (type_str == "process_path") rule.type = domain::RuleType::ProcessPath;
        else if (type_str == "global") rule.type = domain::RuleType::Global;
        else rule.type = domain::RuleType::ProcessName;
        std::string action_str = r.value("action", "proxy");
        if (action_str == "direct") rule.action = domain::RuleAction::Direct;
        else if (action_str == "block") rule.action = domain::RuleAction::Block;
        else rule.action = domain::RuleAction::Proxy;
        rules.push_back(rule);
    }
    return rules;
}

nlohmann::json ConfigManager::RulesToJson(const std::vector<domain::Rule>& rules) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rule : rules) {
        nlohmann::json r;
        r["id"] = rule.id;
        r["pattern"] = WideToUtf8(rule.pattern);
        r["description"] = WideToUtf8(rule.description);
        r["priority"] = rule.priority;
        r["enabled"] = rule.enabled;
        switch (rule.type) {
            case domain::RuleType::ProcessPath: r["type"] = "process_path"; break;
            case domain::RuleType::Global: r["type"] = "global"; break;
            default: r["type"] = "process_name"; break;
        }
        switch (rule.action) {
            case domain::RuleAction::Direct: r["action"] = "direct"; break;
            case domain::RuleAction::Block: r["action"] = "block"; break;
            default: r["action"] = "proxy"; break;
        }
        arr.push_back(r);
    }
    return arr;
}

// ====================================================================
// WP3: schema v2 helpers — accessors, parse/serialize, upgrader
// ====================================================================

// --- Публичные accessors (thread-safe копии под shared_lock) ---

std::vector<AppRule> ConfigManager::GetAppRules() const {
    std::shared_lock lock(m_mutex);
    return m_config.apps;
}

CaptureMode ConfigManager::GetCaptureMode() const {
    std::shared_lock lock(m_mutex);
    return m_config.capture_mode;
}

WintunSettings ConfigManager::GetWintunSettings() const {
    std::shared_lock lock(m_mutex);
    return m_config.wintun;
}

// --- Валидаторы (внутренние, без state) --------------------------------------

namespace {

// Лёгкая проверка IPv4-CIDR: "A.B.C.D/N", A..D ∈ [0,255], N ∈ [0,32].
// Возвращает true при валидности; строгую валидацию делает WP7 preflight.
bool IsLikelyValidIpv4Cidr(const std::string& s) {
    auto slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) return false;
    std::string addr = s.substr(0, slash);
    std::string pfx  = s.substr(slash + 1);
    // Prefix: 1-2 цифры, 0..32.
    if (pfx.empty() || pfx.size() > 2) return false;
    for (char c : pfx) if (c < '0' || c > '9') return false;
    int prefix = std::atoi(pfx.c_str());
    if (prefix < 0 || prefix > 32) return false;
    // Address: ровно 4 октета.
    int octets = 0;
    size_t pos = 0;
    while (pos <= addr.size()) {
        size_t dot = addr.find('.', pos);
        std::string oct = addr.substr(pos, (dot == std::string::npos) ? std::string::npos : (dot - pos));
        if (oct.empty() || oct.size() > 3) return false;
        for (char c : oct) if (c < '0' || c > '9') return false;
        int v = std::atoi(oct.c_str());
        if (v < 0 || v > 255) return false;
        ++octets;
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return octets == 4;
}

// Очень лёгкая проверка IPv6-CIDR: содержит ':' и '/'; префикс — число 0..128.
bool IsLikelyValidIpv6Cidr(const std::string& s) {
    auto slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) return false;
    if (s.find(':') == std::string::npos) return false;
    std::string pfx = s.substr(slash + 1);
    if (pfx.empty() || pfx.size() > 3) return false;
    for (char c : pfx) if (c < '0' || c > '9') return false;
    int prefix = std::atoi(pfx.c_str());
    return prefix >= 0 && prefix <= 128;
}

// Проверка формы "host:port"; port ∈ [1..65535].  Host не валидируется.
bool IsLikelyValidHostPort(const std::string& s) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return false;
    std::string portStr = s.substr(colon + 1);
    if (portStr.empty() || portStr.size() > 5) return false;
    for (char c : portStr) if (c < '0' || c > '9') return false;
    int port = std::atoi(portStr.c_str());
    return port >= 1 && port <= 65535;
}

} // anonymous namespace

// --- Парсинг WintunSettings из JSON ------------------------------------------

WintunSettings ConfigManager::ParseWintunSettings(const nlohmann::json& jw) const {
    WintunSettings w;  // start from defaults
    if (!jw.is_object()) return w;

    w.adapter_name = jw.value("adapter_name", w.adapter_name);
    w.adapter_guid = jw.value("adapter_guid", w.adapter_guid);

    // tunnel_ipv4_cidr — light validate; fallback на дефолт при провале.
    {
        std::string v4 = jw.value("tunnel_ipv4_cidr", w.tunnel_ipv4_cidr);
        if (!v4.empty() && !IsLikelyValidIpv4Cidr(v4)) {
            std::fprintf(stderr,
                "[WARN] ConfigManager: wintun.tunnel_ipv4_cidr '%s' invalid, using default '%s'\n",
                v4.c_str(), w.tunnel_ipv4_cidr.c_str());
        } else {
            w.tunnel_ipv4_cidr = v4;
        }
    }

    // tunnel_ipv6_cidr — пусто ОК; иначе лёгкая проверка.
    {
        std::string v6 = jw.value("tunnel_ipv6_cidr", std::string());
        if (!v6.empty() && !IsLikelyValidIpv6Cidr(v6)) {
            std::fprintf(stderr,
                "[WARN] ConfigManager: wintun.tunnel_ipv6_cidr '%s' invalid, disabling IPv6 tunnel\n",
                v6.c_str());
            w.tunnel_ipv6_cidr.clear();
        } else {
            w.tunnel_ipv6_cidr = v6;
        }
    }

    // mtu — clamp в [576; 65535].
    {
        int mtu = jw.value("mtu", w.mtu);
        if (mtu < 576)   mtu = 576;
        if (mtu > 65535) mtu = 65535;
        w.mtu = mtu;
    }

    // engine — enum + fallback.
    if (jw.contains("engine") && jw["engine"].is_string()) {
        WintunEngineKind e;
        if (WintunEngineKindFromString(jw["engine"].get<std::string>(), e)) {
            w.engine = e;
        } else {
            std::fprintf(stderr,
                "[WARN] ConfigManager: wintun.engine '%s' unknown, coercing to 'embedded'\n",
                jw["engine"].get<std::string>().c_str());
            w.engine = WintunEngineKind::Embedded;
        }
    }

    // external_engine{...}
    if (jw.contains("external_engine") && jw["external_engine"].is_object()) {
        const auto& je = jw["external_engine"];
        w.external_engine.executable   = je.value("executable",   w.external_engine.executable);
        // extra_args (массив строк) — берём как есть, невалидные элементы пропускаем.
        w.external_engine.extra_args.clear();
        if (je.contains("extra_args") && je["extra_args"].is_array()) {
            for (const auto& a : je["extra_args"]) {
                if (a.is_string()) w.external_engine.extra_args.push_back(a.get<std::string>());
            }
        }
        // socks5_listen — light validate; на провале фолбэк на 127.0.0.1:1080.
        {
            std::string sl = je.value("socks5_listen", w.external_engine.socks5_listen);
            if (!IsLikelyValidHostPort(sl)) {
                std::fprintf(stderr,
                    "[WARN] ConfigManager: wintun.external_engine.socks5_listen '%s' invalid, using '127.0.0.1:1080'\n",
                    sl.c_str());
                w.external_engine.socks5_listen = "127.0.0.1:1080";
            } else {
                w.external_engine.socks5_listen = sl;
            }
        }
        w.external_engine.restart_on_crash    = je.value("restart_on_crash",    w.external_engine.restart_on_crash);
        w.external_engine.restart_backoff_ms  = je.value("restart_backoff_ms",  w.external_engine.restart_backoff_ms);
    }

    // Задача 2: process_filter_enabled — по умолчанию true (фильтрация по
    // процессу в embedded-движке включена, поведение консистентно с WinDivert).
    w.process_filter_enabled = jw.value("process_filter_enabled", w.process_filter_enabled);

    // route_ladder_prefix — глубина лестницы split-tunnel маршрутов (1..8).
    // Значения вне диапазона clamp'ятся; всё, что <1, коэрсится к 1.
    {
        int rlp = jw.value("route_ladder_prefix", w.route_ladder_prefix);
        if (rlp < 1) rlp = 1;
        if (rlp > 8) rlp = 8;
        if (rlp != jw.value("route_ladder_prefix", w.route_ladder_prefix)) {
            std::fprintf(stderr,
                "[WARN] ConfigManager: wintun.route_ladder_prefix out of [1..8], clamped to %d\n",
                rlp);
        }
        w.route_ladder_prefix = rlp;
    }

    // block_ipv6 — по умолчанию true (нейтрализация IPv6-утечки в Wintun-режиме).
    w.block_ipv6 = jw.value("block_ipv6", w.block_ipv6);

    // Задача 3: direct_passthrough — по умолчанию false (сохраняет прежнее
    // fail-fast/drop поведение для DIRECT-flow'ов в embedded-режиме).
    w.direct_passthrough = jw.value("direct_passthrough", w.direct_passthrough);

    // direct_fallback — enum "drop" | "proxy" (default drop).  Что делать, если
    // физический egress для DIRECT-flow'а установить не удалось.
    if (jw.contains("direct_fallback") && jw["direct_fallback"].is_string()) {
        DirectFallback f;
        if (DirectFallbackFromString(jw["direct_fallback"].get<std::string>(), f)) {
            w.direct_fallback = f;
        } else {
            std::fprintf(stderr,
                "[WARN] ConfigManager: wintun.direct_fallback '%s' unknown, using 'drop'\n",
                jw["direct_fallback"].get<std::string>().c_str());
            w.direct_fallback = DirectFallback::Drop;
        }
    }

    // direct_egress_interface — ручное переопределение физ. интерфейса egress
    // (имя/ifIndex/IP).  Пусто (default) = авто через GetBestRoute2.
    w.direct_egress_interface = jw.value("direct_egress_interface", w.direct_egress_interface);

    // direct_route_optimization (Option 2a) — динамические <dst>/32 bypass-
    // маршруты; по умолчанию true, но активна только при direct_passthrough=true.
    w.direct_route_optimization = jw.value("direct_route_optimization", w.direct_route_optimization);

    return w;
}

// --- Парсинг одного AppRule ---------------------------------------------------

bool ConfigManager::ParseAppRule(const nlohmann::json& ja, AppRule& out) const {
    if (!ja.is_object()) return false;

    out = AppRule{};  // reset to defaults
    out.exe_path         = ja.value("exe_path", std::string());
    out.pattern          = ja.value("pattern",  std::string());
    out.proxy_id         = ja.value("proxy_id", std::string());
    out.route_all_traffic = ja.value("route_all_traffic", false);

    // pattern — обязателен. Пустой — пропустить с warn'ом.
    if (out.pattern.empty()) {
        std::fprintf(stderr,
            "[WARN] ConfigManager: apps[] entry skipped: empty 'pattern'\n");
        return false;
    }

    // ports[] — деduplicate + валидация 1..65535.
    if (ja.contains("ports") && ja["ports"].is_array()) {
        std::set<uint16_t> seen;
        for (const auto& p : ja["ports"]) {
            if (!p.is_number_integer() && !p.is_number_unsigned()) {
                std::fprintf(stderr,
                    "[WARN] ConfigManager: apps[%s] non-integer port entry skipped\n",
                    out.pattern.c_str());
                continue;
            }
            long long v = p.get<long long>();
            if (v < 1 || v > 65535) {
                std::fprintf(stderr,
                    "[WARN] ConfigManager: apps[%s] port %lld out of range, skipped\n",
                    out.pattern.c_str(), v);
                continue;
            }
            uint16_t port = static_cast<uint16_t>(v);
            if (seen.insert(port).second) {
                out.ports.push_back(port);
            }
        }
    }

    // port_ranges[] — валидация from<=to и обоих в 1..65535.
    if (ja.contains("port_ranges") && ja["port_ranges"].is_array()) {
        for (const auto& r : ja["port_ranges"]) {
            if (!r.is_object()) continue;
            long long from = r.value("from", (long long)0);
            long long to   = r.value("to",   (long long)0);
            if (from < 1 || from > 65535 || to < 1 || to > 65535 || from > to) {
                std::fprintf(stderr,
                    "[WARN] ConfigManager: apps[%s] invalid port_range {from=%lld,to=%lld}, skipped\n",
                    out.pattern.c_str(), from, to);
                continue;
            }
            PortRange pr;
            pr.from = static_cast<uint16_t>(from);
            pr.to   = static_cast<uint16_t>(to);
            out.port_ranges.push_back(pr);
        }
    }

    // Semantic warning: правило не отматчит ничего.
    if (!out.route_all_traffic && out.ports.empty() && out.port_ranges.empty()) {
        std::fprintf(stderr,
            "[WARN] ConfigManager: app '%s' has no ports and route_all_traffic=false; will match nothing\n",
            out.pattern.c_str());
    }

    return true;
}

// --- v1 → v2 in-memory upgrader ----------------------------------------------

std::vector<AppRule> ConfigManager::UpgradeLegacyRulesToApps(const nlohmann::json& jrules) const {
    std::vector<AppRule> apps;
    if (!jrules.is_array()) return apps;

    apps.reserve(jrules.size());
    for (const auto& r : jrules) {
        if (!r.is_object()) continue;

        AppRule a;
        // v1 rules[] могут иметь поле "exe" ИЛИ "pattern" (в разных исторических
        // ревизиях).  Пробуем оба; предпочтение "exe" — как в WP3 спеке.
        std::string exeName = r.value("exe", std::string());
        if (exeName.empty()) exeName = r.value("pattern", std::string());
        if (exeName.empty()) continue;   // пустой pattern — правило неопределено, пропускаем

        a.pattern  = exeName;
        // proxyId (легаси) / proxy_id (нейтральный ключ) — оба принимаются.
        a.proxy_id = r.value("proxyId", r.value("proxy_id", std::string("default")));
        a.route_all_traffic = false;

        // v1 хранил один порт в поле "port".  0 или отсутствие ⇒ ports[]=[].
        if (r.contains("port") && r["port"].is_number_integer()) {
            long long v = r["port"].get<long long>();
            if (v >= 1 && v <= 65535) {
                a.ports.push_back(static_cast<uint16_t>(v));
            }
        }

        // port_ranges в v1 отсутствовал.
        apps.push_back(std::move(a));
    }
    return apps;
}

// --- Сериализация WintunSettings → JSON --------------------------------------

nlohmann::json ConfigManager::WintunSettingsToJson(const WintunSettings& w) const {
    nlohmann::json j;
    j["adapter_name"]     = w.adapter_name;
    j["adapter_guid"]     = w.adapter_guid;
    j["tunnel_ipv4_cidr"] = w.tunnel_ipv4_cidr;
    j["tunnel_ipv6_cidr"] = w.tunnel_ipv6_cidr;
    j["mtu"]              = w.mtu;
    j["engine"]           = WintunEngineKindToString(w.engine);

    nlohmann::json je;
    je["executable"]         = w.external_engine.executable;
    je["extra_args"]         = w.external_engine.extra_args;
    je["socks5_listen"]      = w.external_engine.socks5_listen;
    je["restart_on_crash"]   = w.external_engine.restart_on_crash;
    je["restart_backoff_ms"] = w.external_engine.restart_backoff_ms;
    j["external_engine"]     = je;

    // Задача 2: фильтрация по процессу внутри Wintun-движка.
    j["process_filter_enabled"] = w.process_filter_enabled;

    // Глубина лестницы split-tunnel маршрутов.
    j["route_ladder_prefix"] = w.route_ladder_prefix;

    // Блокировка IPv6 на время Wintun-захвата.
    j["block_ipv6"] = w.block_ipv6;

    // Задача 3: DIRECT-passthrough (Option 1 + Option 2a).
    j["direct_passthrough"]        = w.direct_passthrough;
    j["direct_fallback"]           = DirectFallbackToString(w.direct_fallback);
    j["direct_egress_interface"]   = w.direct_egress_interface;
    j["direct_route_optimization"] = w.direct_route_optimization;

    return j;
}

// --- Сериализация apps[] → JSON ----------------------------------------------

nlohmann::json ConfigManager::AppsToJson(const std::vector<AppRule>& apps) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : apps) {
        nlohmann::json j;
        j["exe_path"]          = a.exe_path;
        j["pattern"]           = a.pattern;
        j["proxy_id"]          = a.proxy_id;
        j["route_all_traffic"] = a.route_all_traffic;
        j["ports"]             = a.ports;

        nlohmann::json ranges = nlohmann::json::array();
        for (const auto& r : a.port_ranges) {
            nlohmann::json jr;
            jr["from"] = r.from;
            jr["to"]   = r.to;
            ranges.push_back(jr);
        }
        j["port_ranges"] = ranges;

        arr.push_back(j);
    }
    return arr;
}

// --- Legacy mirror: apps[] → rules[] (best-effort, cap 128) ------------------

nlohmann::json ConfigManager::BuildLegacyRulesMirror(const std::vector<AppRule>& apps) const {
    // Cap total flat entries; сверх cap'а — пропускаем расширение диапазонов
    // и логируем debug-сообщение.  Cap = 128 (см. WP3 spec).
    constexpr size_t kLegacyCap = 128;

    nlohmann::json arr = nlohmann::json::array();
    size_t emitted = 0;
    bool truncated = false;

    auto emit = [&](const std::string& pattern, uint16_t port, const std::string& proxyId) {
        if (emitted >= kLegacyCap) { truncated = true; return; }
        nlohmann::json r;
        r["exe"]     = pattern;
        r["port"]    = port;
        r["proxyId"] = proxyId.empty() ? std::string("default") : proxyId;
        arr.push_back(r);
        ++emitted;
    };

    for (const auto& a : apps) {
        if (a.pattern.empty()) continue;

        // route_all_traffic — НЕ эмитим в legacy: старый читатель применил бы
        // это некорректно (port=0 не совпадает ни с чем), а «any-port» v1
        // выразить нельзя.
        if (a.route_all_traffic) continue;

        // Дискретные порты
        for (uint16_t p : a.ports) {
            emit(a.pattern, p, a.proxy_id);
            if (truncated) break;
        }
        if (truncated) break;

        // Диапазоны — расширяем ТОЛЬКО если после расширения общее число
        // записей остаётся ≤ cap.
        for (const auto& r : a.port_ranges) {
            // Сколько записей добавит этот диапазон.
            size_t span = (r.to >= r.from) ? (size_t)(r.to - r.from) + 1 : 0;
            if (span == 0) continue;
            if (emitted + span > kLegacyCap) { truncated = true; break; }
            for (uint32_t p = r.from; p <= r.to; ++p) {
                emit(a.pattern, static_cast<uint16_t>(p), a.proxy_id);
            }
        }
        if (truncated) break;
    }

    if (truncated) {
        // Debug-канал: логгер здесь ещё может быть неинициализирован;
        // stderr достаточно для диагностики.
        std::fprintf(stderr,
            "[DEBUG] ConfigManager: legacy rules[] mirror truncated at %zu entries\n",
            emitted);
    }
    return arr;
}

// ====================================================================
// DPAPI: шифрование / дешифрование пароля
// ====================================================================

std::string ConfigManager::EncryptPassword(const std::wstring& plaintext) const {
    DATA_BLOB plainBlob;
    plainBlob.pbData = (BYTE*)plaintext.data();
    plainBlob.cbData = (DWORD)(plaintext.size() * sizeof(wchar_t));
    DATA_BLOB encryptedBlob = {0};
    // Machine-scope DPAPI: the GUI runs as an admin user, the service as
    // LocalSystem. Only CRYPTPROTECT_LOCAL_MACHINE lets one account encrypt a
    // blob the other can decrypt, so both sides must use it. The .NET GUI writes
    // auth.encryptedPassword with DataProtectionScope.LocalMachine to match.
    if (CryptProtectData(&plainBlob, L"TcpRedirector Proxy Password",
            NULL, NULL, NULL,
            CRYPTPROTECT_UI_FORBIDDEN | CRYPTPROTECT_LOCAL_MACHINE, &encryptedBlob)) {
        std::vector<uint8_t> data(encryptedBlob.pbData,
                                  encryptedBlob.pbData + encryptedBlob.cbData);
        LocalFree(encryptedBlob.pbData);
        return Base64Encode(data);
    }
    return {};
}

std::wstring ConfigManager::DecryptPassword(const std::string& ciphertext) const {
    auto raw = Base64Decode(ciphertext);
    if (raw.empty()) return {};
    DATA_BLOB encryptedBlob;
    encryptedBlob.pbData = raw.data();
    encryptedBlob.cbData = (DWORD)raw.size();
    DATA_BLOB plainBlob = {0};
    // Machine-scope DPAPI: must match EncryptPassword and the GUI's
    // DataProtectionScope.LocalMachine so a GUI-written blob decrypts here.
    if (CryptUnprotectData(&encryptedBlob, NULL, NULL, NULL, NULL,
                           CRYPTPROTECT_UI_FORBIDDEN | CRYPTPROTECT_LOCAL_MACHINE, &plainBlob)) {
        std::wstring result((wchar_t*)plainBlob.pbData,
                            plainBlob.cbData / sizeof(wchar_t));
        LocalFree(plainBlob.pbData);
        return result;
    }
    return {};
}

std::string ConfigManager::Base64Encode(const std::vector<uint8_t>& data) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             "abcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int i = 0;
    unsigned char c3[3], c4[4];
    for (auto byte : data) {
        c3[i++] = byte;
        if (i == 3) {
            c4[0] = (c3[0] & 0xfc) >> 2;
            c4[1] = ((c3[0] & 0x03) << 4) + ((c3[1] & 0xf0) >> 4);
            c4[2] = ((c3[1] & 0x0f) << 2) + ((c3[2] & 0xc0) >> 6);
            c4[3] = c3[2] & 0x3f;
            for (int j = 0; j < 4; j++) result += b64[c4[j]];
            i = 0;
        }
    }
    if (i > 0) {
        for (int j = i; j < 3; j++) c3[j] = 0;
        c4[0] = (c3[0] & 0xfc) >> 2;
        c4[1] = ((c3[0] & 0x03) << 4) + ((c3[1] & 0xf0) >> 4);
        c4[2] = ((c3[1] & 0x0f) << 2) + ((c3[2] & 0xc0) >> 6);
        for (int j = 0; j < i + 1; j++) result += b64[c4[j]];
        while (i++ < 3) result += '=';
    }
    return result;
}

std::vector<uint8_t> ConfigManager::Base64Decode(const std::string& data) {
    static unsigned char D[256] = {0};
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) D[i] = 0xFF;
        for (int i = 'A'; i <= 'Z'; i++) D[i] = i - 'A';
        for (int i = 'a'; i <= 'z'; i++) D[i] = i - 'a' + 26;
        for (int i = '0'; i <= '9'; i++) D[i] = i - '0' + 52;
        D['+'] = 62; D['/'] = 63; D['='] = 0xFF;  // H2: padding must be skipped, not decoded as data
        init = true;
    }

    std::vector<uint8_t> result;
    int val = 0, valb = -8;
    for (char c : data) {
        if (D[(unsigned char)c] == 0xFF) continue;  // skips padding '=' and non-base64 chars
        val = (val << 6) + D[(unsigned char)c];
        valb += 6;
        if (valb >= 0) {
            result.push_back((unsigned char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

// ====================================================================
// SecretsManager — реализация доменного порта ISecretsManager
// ====================================================================

std::vector<uint8_t> SecretsManager::Encrypt(const std::wstring& plaintext) {
    DATA_BLOB plainBlob;
    plainBlob.pbData = (BYTE*)plaintext.data();
    plainBlob.cbData = (DWORD)(plaintext.size() * sizeof(wchar_t));
    DATA_BLOB encryptedBlob = {0};
    if (CryptProtectData(&plainBlob, L"TcpRedirector Proxy Password",
            NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &encryptedBlob)) {
        std::vector<uint8_t> result(encryptedBlob.pbData,
                                    encryptedBlob.pbData + encryptedBlob.cbData);
        LocalFree(encryptedBlob.pbData);
        return result;
    }
    return {};
}

std::wstring SecretsManager::Decrypt(const std::vector<uint8_t>& ciphertext) {
    DATA_BLOB encryptedBlob;
    encryptedBlob.pbData = const_cast<BYTE*>(ciphertext.data());
    encryptedBlob.cbData = (DWORD)ciphertext.size();
    DATA_BLOB plainBlob = {0};
    if (CryptUnprotectData(&encryptedBlob, NULL, NULL, NULL, NULL,
                           CRYPTPROTECT_UI_FORBIDDEN, &plainBlob)) {
        std::wstring result((wchar_t*)plainBlob.pbData,
                            plainBlob.cbData / sizeof(wchar_t));
        LocalFree(plainBlob.pbData);
        return result;
    }
    return {};
}

} // namespace infrastructure
} // namespace tcp_redirector
