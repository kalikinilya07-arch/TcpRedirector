#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dpapi.h>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "../../domain/ports/IConfigStore.h"

namespace tcp_redirector {
namespace infrastructure {

class ConfigManager : public domain::ports::IConfigStore {
public:
    ConfigManager() {
        m_configPath = std::filesystem::path(getenv("ProgramData"))
            / "TcpRedirector" / "config.json";
    }

    bool Load() override { /* same as before */ return LoadImpl(); }
    bool Save() override { /* same as before */ return SaveImpl(); }
    domain::ProxyConfig GetProxyConfig() const override { return m_config; }
    bool SetProxyConfig(const domain::ProxyConfig& config) override { m_config = config; return Save(); }
    std::vector<domain::Rule> GetRules() const override { return m_rules; }
    bool SetRules(const std::vector<domain::Rule>& rules) override { m_rules = rules; return Save(); }
    domain::LogLevel GetLogLevel() const override { return m_logLevel; }
    bool SetLogLevel(domain::LogLevel level) override { m_logLevel = level; return Save(); }
    std::filesystem::path GetLogDirectory() const override {
        return std::filesystem::path(getenv("ProgramData")) / "TcpRedirector" / "logs";
    }
    uint32_t GetMaxLogFileSizeMB() const override { return 50; }
    uint32_t GetMaxLogFiles() const override { return 10; }

private:
    bool LoadImpl() {
        try {
            if (!std::filesystem::exists(m_configPath)) {
                return CreateDefaultConfig();
            }
            std::ifstream file(m_configPath);
            if (!file.is_open()) return false;
            nlohmann::json j;
            file >> j;
            if (j.contains("proxy")) {
                auto& p = j["proxy"];
                m_config.host = std::wstring(p["host"].get<std::string>().begin(),
                                              p["host"].get<std::string>().end());
                m_config.port = p.value("port", 3128);
                m_config.auth_required = p.value("auth_required", false);
                m_config.login = std::wstring(p.value("login", std::string()).begin(),
                                               p.value("login", std::string()).end());
                m_config.has_password = p.value("has_password", false);
            }
            if (j.contains("rules")) {
                m_rules.clear();
                for (const auto& r : j["rules"]) {
                    domain::Rule rule;
                    rule.id = r.value("id", "");
                    rule.pattern = std::wstring(r["pattern"].get<std::string>().begin(),
                                                 r["pattern"].get<std::string>().end());
                    rule.description = std::wstring(r.value("description", std::string()).begin(),
                                                     r.value("description", std::string()).end());
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
                    m_rules.push_back(rule);
                }
            }
            return true;
        } catch (...) { return false; }
    }

    bool SaveImpl() {
        try {
            std::filesystem::create_directories(m_configPath.parent_path());
            nlohmann::json j;
            j["proxy"]["host"] = std::string(m_config.host.begin(), m_config.host.end());
            j["proxy"]["port"] = m_config.port;
            j["proxy"]["auth_required"] = m_config.auth_required;
            j["proxy"]["login"] = std::string(m_config.login.begin(), m_config.login.end());
            j["proxy"]["has_password"] = m_config.has_password;
            j["rules"] = nlohmann::json::array();
            for (const auto& rule : m_rules) {
                nlohmann::json r;
                r["id"] = rule.id;
                r["pattern"] = std::string(rule.pattern.begin(), rule.pattern.end());
                r["description"] = std::string(rule.description.begin(), rule.description.end());
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
                j["rules"].push_back(r);
            }
            std::ofstream file(m_configPath);
            file << j.dump(4);
            return true;
        } catch (...) { return false; }
    }

    bool CreateDefaultConfig() {
        m_config.host = L"proxy.example.com";
        m_config.port = 3128;
        domain::Rule default_rule;
        default_rule.id = "default";
        default_rule.type = domain::RuleType::Global;
        default_rule.action = domain::RuleAction::Direct;
        default_rule.pattern = L"*";
        default_rule.description = L"Default: pass through";
        default_rule.priority = 999;
        default_rule.enabled = true;
        m_rules.push_back(default_rule);
        return Save();
    }

    std::filesystem::path m_configPath;
    domain::ProxyConfig m_config;
    std::vector<domain::Rule> m_rules;
    domain::LogLevel m_logLevel = domain::LogLevel::Info;
};

class SecretsManager : public domain::ports::ISecretsManager {
public:
    std::vector<uint8_t> Encrypt(const std::wstring& plaintext) override {
        DATA_BLOB plain_blob;
        plain_blob.pbData = (BYTE*)plaintext.data();
        plain_blob.cbData = (DWORD)(plaintext.size() * sizeof(wchar_t));
        DATA_BLOB encrypted_blob = {0};
        if (CryptProtectData(&plain_blob, L"TcpRedirector Proxy Password",
                NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &encrypted_blob)) {
            std::vector<uint8_t> result(encrypted_blob.pbData,
                                        encrypted_blob.pbData + encrypted_blob.cbData);
            LocalFree(encrypted_blob.pbData);
            return result;
        }
        return {};
    }

    std::wstring Decrypt(const std::vector<uint8_t>& ciphertext) override {
        DATA_BLOB encrypted_blob;
        encrypted_blob.pbData = const_cast<BYTE*>(ciphertext.data());
        encrypted_blob.cbData = (DWORD)ciphertext.size();
        DATA_BLOB plain_blob = {0};
        if (CryptUnprotectData(&encrypted_blob, NULL, NULL, NULL, NULL,
                               CRYPTPROTECT_UI_FORBIDDEN, &plain_blob)) {
            std::wstring result((wchar_t*)plain_blob.pbData,
                                plain_blob.cbData / sizeof(wchar_t));
            LocalFree(plain_blob.pbData);
            return result;
        }
        return {};
    }
};

} // namespace infrastructure
} // namespace tcp_redirector