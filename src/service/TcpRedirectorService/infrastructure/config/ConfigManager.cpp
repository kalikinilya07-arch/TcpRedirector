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
#include <cstdio>

namespace tcp_redirector {
namespace infrastructure {

// ====================================================================
// Конструктор ConfigManager
// ====================================================================

ConfigManager::ConfigManager() {
    wchar_t progData[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH) > 0) {
        m_configPath = std::filesystem::path(progData) / L"TcpRedirector" / L"config.json";
    } else {
        m_configPath = L"C:\\ProgramData\\TcpRedirector\\config.json";
    }
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
    pc.has_password = !m_config.auth.encryptedPassword.empty();
    if (pc.has_password) {
        pc.plain_password = DecryptPassword(m_config.auth.encryptedPassword);
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
    // v1.1.1: only update password when explicitly provided (set_password=true).
    // If set_password is false, preserve the existing encrypted password.
    if (config.has_password) {
        m_config.auth.encryptedPassword = EncryptPassword(config.plain_password);
    }
    // else: keep existing encryptedPassword unchanged
    NotifyListeners(oldCfg, m_config);
    return SaveImpl();  // Persist to config.json
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
    switch (m_config.log.level) {
        case 0: return domain::LogLevel::Trace;
        case 1: return domain::LogLevel::Debug;
        case 2: return domain::LogLevel::Info;
        case 3: return domain::LogLevel::Warn;
        case 4: return domain::LogLevel::Error;
        default: return domain::LogLevel::Info;
    }
}

bool ConfigManager::SetLogLevel(domain::LogLevel level) {
    std::unique_lock lock(m_mutex);
    Config oldCfg = m_config;
    switch (level) {
        case domain::LogLevel::Trace: m_config.log.level = 0; break;
        case domain::LogLevel::Debug: m_config.log.level = 1; break;
        case domain::LogLevel::Info:  m_config.log.level = 2; break;
        case domain::LogLevel::Warn:  m_config.log.level = 3; break;
        case domain::LogLevel::Error: m_config.log.level = 4; break;
        default: m_config.log.level = 2; break;
    }
    if (SaveImpl()) {
        NotifyListeners(oldCfg, m_config);
        return true;
    }
    return false;
}

std::filesystem::path ConfigManager::GetLogDirectory() const {
    wchar_t progData[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH) > 0) {
        return std::filesystem::path(progData) / L"TcpRedirector" / L"logs";
    }
    return L"C:\\ProgramData\\TcpRedirector\\logs";
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
    if (m_config.auth.encryptedPassword.empty())
        return {};
    return DecryptPassword(m_config.auth.encryptedPassword);
}

void ConfigManager::SetPassword(const std::wstring& plainPassword) {
    std::unique_lock lock(m_mutex);
    m_config.auth.encryptedPassword = EncryptPassword(plainPassword);
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

        // Загрузка Config
        if (j.contains("app")) {
            auto& a = j["app"];
            std::string tmpExe = a["exePath"].get<std::string>();
            m_config.app.exePath = Utf8ToWide(tmpExe);
        }
        if (j.contains("proxy")) {
            auto& p = j["proxy"];
            m_config.proxy.host = p.value("host", std::string("127.0.0.1"));
            m_config.proxy.port = p.value("port", 3128);
            m_config.proxy.enabled = p.value("enabled", true);
        }
        if (j.contains("auth")) {
            auto& a = j["auth"];
            m_config.auth.enabled = a.value("enabled", false);
            m_config.auth.username = a.value("username", std::string());
            m_config.auth.encryptedPassword = a.value("encryptedPassword", std::string());
            m_config.auth.kerberos = a.value("kerberos", false);
        }
        if (j.contains("log")) {
            auto& l = j["log"];
            m_config.log.level = l.value("level", 2);
            m_config.log.fileEnabled = l.value("fileEnabled", true);
            m_config.log.maxSizeMB = l.value("maxSizeMB", 10);
        }
        if (j.contains("stats")) {
            auto& s = j["stats"];
            m_config.stats.updateIntervalMs = s.value("updateIntervalMs", 2000);
        }
        if (j.contains("capture")) {
            m_config.capture_enabled = j["capture"].value("enabled", false);
        }

        if (j.contains("log_rotation")) {
            auto& lr = j["log_rotation"];
            m_config.log_rotation.enabled = lr.value("enabled", true);
            m_config.log_rotation.schedule = lr.value("schedule", std::string("daily"));
            m_config.log_rotation.hour = lr.value("hour", 3);
            m_config.log_rotation.minute = lr.value("minute", 0);
            m_config.log_rotation.max_age_days = lr.value("max_age_days", 30);
            {
                std::string tmp = lr.value("archive_dir", std::string());
                m_config.log_rotation.archive_dir = tmp;
            }
            m_config.log_rotation.compress = lr.value("compress", true);
        }

        // Загрузка правил (старый формат)
        if (j.contains("rules")) {
            m_rules = JsonToRules(j);
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool ConfigManager::SaveImpl() {
    try {
        std::filesystem::create_directories(m_configPath.parent_path());

        nlohmann::json j;
        j["app"]["exePath"] = WideToUtf8(m_config.app.exePath);
        j["proxy"]["host"] = m_config.proxy.host;
        j["proxy"]["port"] = m_config.proxy.port;
        j["proxy"]["enabled"] = m_config.proxy.enabled;
        j["auth"]["enabled"] = m_config.auth.enabled;
        j["auth"]["username"] = m_config.auth.username;
        j["auth"]["encryptedPassword"] = m_config.auth.encryptedPassword;
        j["auth"]["kerberos"] = m_config.auth.kerberos;
        j["log"]["level"] = m_config.log.level;
        j["log"]["fileEnabled"] = m_config.log.fileEnabled;
        j["log"]["maxSizeMB"] = m_config.log.maxSizeMB;
        j["capture"]["enabled"] = m_config.capture_enabled;

        j["log_rotation"]["enabled"] = m_config.log_rotation.enabled;
        j["log_rotation"]["schedule"] = m_config.log_rotation.schedule;
        j["log_rotation"]["hour"] = m_config.log_rotation.hour;
        j["log_rotation"]["minute"] = m_config.log_rotation.minute;
        j["log_rotation"]["max_age_days"] = m_config.log_rotation.max_age_days;
        j["log_rotation"]["archive_dir"] = m_config.log_rotation.archive_dir;
        j["log_rotation"]["compress"] = m_config.log_rotation.compress;
        j["stats"]["updateIntervalMs"] = m_config.stats.updateIntervalMs;

        // Правила (старый формат)
        j["rules"] = RulesToJson(m_rules);

        // Atomic write: write to temp file, then rename
        auto tmpPath = m_configPath;
        tmpPath += L".tmp";
        {
            std::ofstream file(tmpPath);
            if (!file.is_open()) return false;
            file << j.dump(4);
            if (!file.good()) return false;
        }
        std::filesystem::rename(tmpPath, m_configPath);
        return true;
    } catch (...) {
        return false;
    }
}

bool ConfigManager::CreateDefaultConfig() {
    m_config = Config();
    m_config.capture_enabled = false;  // безопасный старт: редирект выключен
    m_config.app.exePath = L"C:\\Projects\\china\\police_sec\\TransfersClient.exe";
    m_config.proxy.host = "127.0.0.1";
    m_config.proxy.port = 8888;
    m_config.proxy.enabled = true;

    domain::Rule defaultRule;
    defaultRule.id = "default";
    defaultRule.type = domain::RuleType::ProcessName;
    defaultRule.action = domain::RuleAction::Proxy;
    defaultRule.pattern = L"TransfersClient.exe";
    defaultRule.description = L"Redirect TcpRedirector traffic";
    defaultRule.priority = 1;
    defaultRule.enabled = true;
    m_rules.push_back(defaultRule);

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
    j["log"]["level"] = cfg.log.level;
    j["log"]["fileEnabled"] = cfg.log.fileEnabled;
    j["log"]["maxSizeMB"] = cfg.log.maxSizeMB;
    j["stats"]["updateIntervalMs"] = cfg.stats.updateIntervalMs;
    return j;
}

std::vector<domain::Rule> ConfigManager::JsonToRules(const nlohmann::json& j) const {
    std::vector<domain::Rule> rules;
    if (!j.contains("rules")) return rules;
    for (const auto& r : j["rules"]) {
        domain::Rule rule;
        rule.id = r.value("id", "");
        {
            std::string tmp = r["pattern"].get<std::string>();
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
// DPAPI: шифрование / дешифрование пароля
// ====================================================================

std::string ConfigManager::EncryptPassword(const std::wstring& plaintext) const {
    DATA_BLOB plainBlob;
    plainBlob.pbData = (BYTE*)plaintext.data();
    plainBlob.cbData = (DWORD)(plaintext.size() * sizeof(wchar_t));
    DATA_BLOB encryptedBlob = {0};
    // M12: align DPAPI flags with SecretsManager::Encrypt for interoperability.
    // CRYPTPROTECT_UI_FORBIDDEN + description string (consistent with SecretsManager).
    if (CryptProtectData(&plainBlob, L"TcpRedirector Proxy Password",
            NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &encryptedBlob)) {
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
    // M12: align DPAPI flags with encryption (CRYPTPROTECT_UI_FORBIDDEN).
    if (CryptUnprotectData(&encryptedBlob, NULL, NULL, NULL, NULL,
                           CRYPTPROTECT_UI_FORBIDDEN, &plainBlob)) {
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
