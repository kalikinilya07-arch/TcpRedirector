#pragma once

/**
 * @file RuleEngine.h
 * @brief Потокобезопасный движок проверки правил фильтрации трафика.
 *
 * Реализован header-only для простоты. Использует shared_mutex
 * для разделяемого доступа (читатели не блокируют друг друга).
 * Поддерживает правила типов: по имени процесса, по пути процесса,
 * глобальные. Сортировка по приоритету.
 */

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace services {

/**
 * @brief Движок проверки правил фильтрации трафика.
 *
 * Позволяет добавлять, удалять, обновлять правила, а также
 * выполнять сопоставление (match) процесса с правилами для
 * определения действия: Proxy, Direct или Block.
 */
class RuleEngine {
public:
    RuleEngine() = default;

    /**
     * @brief Добавить правило.
     * @param rule Правило для добавления.
     * @return true (всегда успешно).
     */
    bool AddRule(const Rule& rule) {
        std::unique_lock lock(m_mutex);
        m_rules.push_back(rule);
        SortRules();
        return true;
    }

    /**
     * @brief Удалить правило по идентификатору.
     * @param rule_id Идентификатор правила.
     * @return true, если правило найдено и удалено.
     */
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

    /**
     * @brief Обновить существующее правило.
     * @param rule Правило с обновлёнными полями (id должно совпадать).
     * @return true, если правило найдено и обновлено.
     */
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

    /**
     * @brief Установить все правила (заменяет текущий список).
     * @param rules Вектор новых правил.
     */
    void SetRules(const std::vector<Rule>& rules) {
        std::unique_lock lock(m_mutex);
        m_rules = rules;
        SortRules();
    }

    /**
     * @brief Получить все правила.
     * @return Копия вектора правил.
     */
    std::vector<Rule> GetRules() const {
        std::shared_lock lock(m_mutex);
        return m_rules;
    }

    /**
     * @brief Сопоставить процесс с правилами и определить действие.
     * @param process_name Имя процесса.
     * @param process_path Полный путь к процессу.
     * @return Действие (Proxy / Direct / Block).
     */
    RuleAction Match(const std::wstring& process_name,
                     const std::wstring& process_path) const {
        std::shared_lock lock(m_mutex);

        // Проходим по правилам в порядке приоритета
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

        // По умолчанию: не перенаправлять (Direct)
        return RuleAction::Direct;
    }

    /**
     * @brief Проверить, нужно ли перенаправлять трафик процесса.
     * @param process_name Имя процесса.
     * @param process_path Полный путь к процессу.
     * @return true, если действие правила — Proxy.
     */
    bool ShouldRedirect(const std::wstring& process_name,
                        const std::wstring& process_path) const {
        return Match(process_name, process_path) == RuleAction::Proxy;
    }

private:
    mutable std::shared_mutex m_mutex;  //!< Мьютекс для разделяемого доступа
    std::vector<Rule> m_rules;          //!< Список правил, отсортированный по приоритету

    /**
     * @brief Отсортировать правила по приоритету (возрастание).
     */
    void SortRules() {
        std::sort(m_rules.begin(), m_rules.end(),
            [](const Rule& a, const Rule& b) {
                return a.priority < b.priority;
            });
    }

    /**
     * @brief Проверка соответствия шаблона тексту с поддержкой wildcard (*).
     *
     * Поддерживаются шаблоны:
     * - * — любая строка
     * - текст* — префикс
     * - *текст — суффикс
     * - *текст* — вхождение подстроки
     * - текст — точное совпадение
     *
     * @param pattern Шаблон для поиска.
     * @param text Текст, в котором выполняется поиск.
     * @return true, если текст соответствует шаблону.
     */
    static bool WildcardMatch(const std::wstring& pattern, const std::wstring& text) {
        // "*" — любая строка
        if (pattern == L"*") return true;

        // Точное вхождение шаблона как подстроки
        size_t pos = text.find(pattern);
        if (pos != std::wstring::npos) return true;

        // *текст* — вхождение подстроки
        if (pattern.size() >= 2 && pattern.front() == L'*' && pattern.back() == L'*') {
            std::wstring mid = pattern.substr(1, pattern.size() - 2);
            return text.find(mid) != std::wstring::npos;
        }
        // текст* — префикс
        if (pattern.back() == L'*') {
            std::wstring prefix = pattern.substr(0, pattern.size() - 1);
            return text.find(prefix) == 0;
        }
        // *текст — суффикс
        if (pattern.front() == L'*') {
            std::wstring suffix = pattern.substr(1);
            if (text.size() >= suffix.size()) {
                return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
            }
        }

        // Точное совпадение
        return pattern == text;
    }
};

} // namespace services
} // namespace domain
} // namespace tcp_redirector