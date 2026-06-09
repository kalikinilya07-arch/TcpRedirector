#pragma once

#include <memory>
#include <shared_mutex>
#include <algorithm>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace services {

class RuleEngine {
public:
    RuleEngine() = default;

    bool AddRule(const Rule& rule) {
        std::unique_lock lock(m_mutex);
        m_rules.push_back(rule);
        SortRules();
        return true;
    }

    bool RemoveRule(const std::string& rule_id) {
        std::unique_lock lock(m_mutex);
        auto it = std::remove_if(m_rules.begin(), m_rules.end(),
            [&](const Rule& r) { return r.id == rule_id; });
        if (it != m_rules.end()) {
            m_rules.erase(it, m_rules.end());
            return true;
        }
        return false;
    }

    bool UpdateRule(const Rule& rule) {
        std::unique_lock lock(m_mutex);
        for (auto& r : m_rules) {
            if (r.id == rule.id) {
                r = rule;
                SortRules();
                return true;
            }
        }
        return false;
    }

    void SetRules(const std::vector<Rule>& rules) {
        std::unique_lock lock(m_mutex);
        m_rules = rules;
        SortRules();
    }

    std::vector<Rule> GetRules() const {
        std::shared_lock lock(m_mutex);
        return m_rules;
    }

    RuleAction Match(const std::wstring& process_name,
                     const std::wstring& process_path) const {
        std::shared_lock lock(m_mutex);

        for (const auto& rule : m_rules) {
            if (!rule.enabled) continue;

            switch (rule.type) {
                case RuleType::Global:
                    return rule.action;

                case RuleType::ProcessName:
                    if (WildcardMatch(rule.pattern, process_name)) {
                        return rule.action;
                    }
                    break;

                case RuleType::ProcessPath:
                    if (WildcardMatch(rule.pattern, process_path)) {
                        return rule.action;
                    }
                    break;
            }
        }

        // Default: no redirect (direct)
        return RuleAction::Direct;
    }

    bool ShouldRedirect(const std::wstring& process_name,
                        const std::wstring& process_path) const {
        return Match(process_name, process_path) == RuleAction::Proxy;
    }

private:
    mutable std::shared_mutex m_mutex;
    std::vector<Rule> m_rules;

    void SortRules() {
        std::sort(m_rules.begin(), m_rules.end(),
            [](const Rule& a, const Rule& b) {
                return a.priority < b.priority;
            });
    }

    static bool WildcardMatch(const std::wstring& pattern, const std::wstring& text) {
        if (pattern == L"*") return true;

        size_t pos = text.find(pattern);
        if (pos != std::wstring::npos) return true;

        // Simple wildcard: support * at start, end, or both
        if (pattern.size() >= 2 && pattern.front() == L'*' && pattern.back() == L'*') {
            std::wstring mid = pattern.substr(1, pattern.size() - 2);
            return text.find(mid) != std::wstring::npos;
        }
        if (pattern.back() == L'*') {
            std::wstring prefix = pattern.substr(0, pattern.size() - 1);
            return text.find(prefix) == 0;
        }
        if (pattern.front() == L'*') {
            std::wstring suffix = pattern.substr(1);
            if (text.size() >= suffix.size()) {
                return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
            }
        }

        return pattern == text;
    }
};

} // namespace services
} // namespace domain
} // namespace tcp_redirector