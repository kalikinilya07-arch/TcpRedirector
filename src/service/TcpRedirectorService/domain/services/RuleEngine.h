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
#include <cstdint>
#include <string>
#include <vector>
#include "../entities/ProxyConfig.h"
#include "../../infrastructure/config/Config.h"

namespace tcp_redirector {
namespace domain {
namespace services {

/**
 * @brief Результат сопоставления правила потока (WP4).
 *
 * Возвращается функцией RuleEngine::MatchForFlow. Если matched=false,
 * остальные поля не имеют смысла (значения по умолчанию).
 */
struct AppRuleMatch {
    bool        matched = false;             //!< true, если какое-либо правило сработало.
    std::string proxy_id;                    //!< Идентификатор прокси из сработавшего правила.
    bool        route_all_traffic = false;   //!< Флаг «маршрутизировать весь трафик» (порты игнор.).
};

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

    // ------------------------------------------------------------------
    // WP4: port-aware matching API (schema v2 AppRule).
    // ------------------------------------------------------------------

    /**
     * @brief Заменить список пер-приложение правил (schema v2).
     *
     * Приоритет — порядок вставки (первое совпавшее правило побеждает).
     * Существующий legacy-API SetRules/Match/ShouldRedirect остаётся
     * рабочим и независимым от m_app_rules.
     *
     * @param app_rules Вектор AppRule из ConfigManager::GetAppRules().
     */
    void SetAppRules(std::vector<infrastructure::AppRule> app_rules) {
        std::unique_lock lock(m_mutex);
        m_app_rules = std::move(app_rules);
    }

    /**
     * @brief Получить копию текущего списка v2 app-правил.
     */
    std::vector<infrastructure::AppRule> GetAppRules() const {
        std::shared_lock lock(m_mutex);
        return m_app_rules;
    }

    /**
     * @brief Сопоставить исходящий поток с v2 app-правилами.
     *
     * Итерация — в порядке вставки. Для каждого правила:
     *   1. Если задан exe_path — сверяем регистро-независимо (равенство
     *      либо суффиксное совпадение) с process_path; при несовпадении
     *      правило пропускается.
     *   2. Если задан pattern — матчим по process_name через ту же
     *      wildcard-семантику, что и legacy Match (WildcardMatch).
     *   3. При успешном совпадении процесса:
     *      - route_all_traffic=true → возврат (порты игнорируются);
     *      - иначе если dst_port есть в ports[] или входит в один из
     *        port_ranges[] (from ≤ dst_port ≤ to) → возврат;
     *      - иначе пробуем следующее правило (не останавливаемся).
     *   4. Если ни одно правило не сработало — matched=false.
     *
     * @param process_name Имя процесса (например "firefox.exe").
     * @param process_path Полный путь к процессу или "".
     * @param dst_port     Порт назначения TCP-соединения (host byte order).
     * @return AppRuleMatch с результатом.
     */
    AppRuleMatch MatchForFlow(const std::string& process_name,
                              const std::string& process_path,
                              uint16_t dst_port) const {
        std::shared_lock lock(m_mutex);

        for (const auto& rule : m_app_rules) {
            // (1) exe_path (опционально): суффикс/равенство, case-insensitive.
            if (!rule.exe_path.empty()) {
                if (!PathMatchesCaseInsensitive(rule.exe_path, process_path)) {
                    continue;
                }
            }

            // (2) pattern (обязательно по схеме, но допускаем пустой как
            //     «wildcard по имени», если exe_path уже сматчился).
            if (!rule.pattern.empty()) {
                if (!WildcardMatchUtf8(rule.pattern, process_name)) {
                    continue;
                }
            } else if (rule.exe_path.empty()) {
                // Оба пустые — нечего матчить.
                continue;
            }

            // (3) Процесс сматчился — проверяем порт.
            if (rule.route_all_traffic) {
                AppRuleMatch r;
                r.matched = true;
                r.proxy_id = rule.proxy_id;
                r.route_all_traffic = true;
                return r;
            }

            bool port_ok = false;
            for (uint16_t p : rule.ports) {
                if (p == dst_port) { port_ok = true; break; }
            }
            if (!port_ok) {
                for (const auto& range : rule.port_ranges) {
                    if (range.from <= dst_port && dst_port <= range.to) {
                        port_ok = true;
                        break;
                    }
                }
            }
            if (port_ok) {
                AppRuleMatch r;
                r.matched = true;
                r.proxy_id = rule.proxy_id;
                r.route_all_traffic = false;
                return r;
            }
            // Порт не подошёл — продолжаем: другое правило может поймать.
        }

        return AppRuleMatch{}; // matched=false
    }

private:
    mutable std::shared_mutex m_mutex;  //!< Мьютекс для разделяемого доступа
    std::vector<Rule> m_rules;          //!< Список правил, отсортированный по приоритету
    std::vector<infrastructure::AppRule> m_app_rules; //!< v2 app-правила (порядок = приоритет).

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

    /**
     * @brief UTF-8 (narrow) вариант WildcardMatch — та же семантика, что
     *        и wide-версия (см. выше): "*", префикс, суффикс, подстрока,
     *        точное совпадение.
     *
     * Используется MatchForFlow, потому что v2 AppRule.pattern — std::string.
     */
    static bool WildcardMatchUtf8(const std::string& pattern, const std::string& text) {
        if (pattern == "*") return true;

        // Прямое вхождение как подстрока (совпадает с легаси-веткой).
        size_t pos = text.find(pattern);
        if (pos != std::string::npos) return true;

        if (pattern.size() >= 2 && pattern.front() == '*' && pattern.back() == '*') {
            std::string mid = pattern.substr(1, pattern.size() - 2);
            return text.find(mid) != std::string::npos;
        }
        if (!pattern.empty() && pattern.back() == '*') {
            std::string prefix = pattern.substr(0, pattern.size() - 1);
            return text.find(prefix) == 0;
        }
        if (!pattern.empty() && pattern.front() == '*') {
            std::string suffix = pattern.substr(1);
            if (text.size() >= suffix.size()) {
                return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
            }
        }

        return pattern == text;
    }

    /**
     * @brief Case-insensitive сравнение пути процесса против rule.exe_path.
     *
     * Возвращает true если process_path равен exe_path, либо оканчивается
     * на exe_path с разделителем перед ним (суффиксный матч по компонентам
     * пути). Регистр игнорируется по ASCII (path в Windows — ANSI/UTF-8).
     */
    static bool PathMatchesCaseInsensitive(const std::string& exe_path,
                                           const std::string& process_path) {
        if (exe_path.empty()) return true; // пустой exe_path не ограничивает.
        if (process_path.empty()) return false;

        auto tolower_ascii = [](unsigned char c) -> unsigned char {
            return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
        };
        auto ieq = [&](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (tolower_ascii(static_cast<unsigned char>(a[i])) !=
                    tolower_ascii(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        };

        if (ieq(exe_path, process_path)) return true;

        // Суффиксный матч: process_path заканчивается на exe_path и
        // перед ним есть разделитель (или exe_path сам стартует с разделителя).
        if (process_path.size() > exe_path.size()) {
            size_t off = process_path.size() - exe_path.size();
            // сравниваем хвост
            for (size_t i = 0; i < exe_path.size(); ++i) {
                if (tolower_ascii(static_cast<unsigned char>(process_path[off + i])) !=
                    tolower_ascii(static_cast<unsigned char>(exe_path[i]))) {
                    return false;
                }
            }
            // граница по разделителю
            char boundary = process_path[off - 1];
            if (boundary == '\\' || boundary == '/') return true;
            if (!exe_path.empty() && (exe_path.front() == '\\' || exe_path.front() == '/')) {
                return true;
            }
        }
        return false;
    }
};

} // namespace services
} // namespace domain
} // namespace tcp_redirector