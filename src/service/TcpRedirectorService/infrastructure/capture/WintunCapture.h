#pragma once

/**
 * @file WintunCapture.h
 * @brief WP6 stub-реализация Wintun-based capture-движка.
 *
 * ЭТОТ ФАЙЛ — ЗАГЛУШКА, ВНЕСЁННАЯ В РАМКАХ WP6.
 * Полная реализация появится в WP8..WP10 (см. plans/WINTUN_INTEGRATION_PLAN.md
 * §6, §11).  Здесь мы:
 *   • реализуем весь ICapture-контракт минимальными безопасными no-op-ами,
 *   • при выборе режима capture_mode="wintun" сервис компилируется и запускается,
 *   • Open() ЛОГИРУЕТ INFO-строку и возвращает true, чтобы preflight WP7
 *     (а не эта заглушка) отвечал за отказ, когда рантайм-поддержка отсутствует,
 *   • НЕ грузит wintun.dll, НЕ создаёт адаптер, НЕ поднимает потоки/сокеты.
 *
 * Сохраняем WintunSettings как член класса — WP10 будет из них читать
 * adapter_name / tunnel_ipv4_cidr / mtu / engine и т.д.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>
#include <cstdint>

#include "../../domain/ports/ICapture.h"
#include "../../domain/ports/IConnectionTable.h"
#include "../../domain/ports/IConnectionMonitor.h"
#include "../../domain/services/RuleEngine.h"
#include "../config/Config.h"

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Заглушка Wintun-based capture — WP6.
 *
 * Реализует ICapture так, чтобы жизненный цикл сервиса корректно проходил
 * с capture_mode="wintun".  Единственное побочное действие — одна INFO-запись
 * при Open().  Остальные операции — безопасные no-op-ы, возвращающие нули.
 */
class WintunCapture : public domain::ports::ICapture {
public:
    /**
     * @brief Конструктор.
     * @param settings Настройки Wintun-секции (сохраняются для WP10).
     * @param logSink  Опциональный ILogSink — если задан, при Open() пишется
     *                 INFO-строка о выборе Wintun-режима.
     */
    explicit WintunCapture(WintunSettings settings,
                           domain::ports::ILogSink* logSink = nullptr)
        : m_settings(std::move(settings)),
          m_logSink(logSink) {
    }

    ~WintunCapture() override = default;

    // --- ICapture: жизненный цикл ---

    /**
     * @brief WP6 stub: логирует INFO и возвращает true.
     *
     * НЕ грузит wintun.dll, НЕ создаёт адаптер, НЕ поднимает потоки.
     * Реальный отказ ("wintun.dll не найден", "нет админ-прав" и т.п.)
     * — задача preflight WP7, а не этой заглушки.
     */
    bool Open() override {
        if (m_logSink) {
            m_logSink->Log(
                domain::LogLevel::Info,
                "wintun",
                "[WintunCapture] Selected \xE2\x80\x94 full implementation lands in WP10.");
        }
        m_opened = true;
        return true;
    }

    void Close() override {
        // No-op: ничего не открывали.
        m_opened = false;
    }

    bool IsOpen() const override { return m_opened; }

    // --- ICapture: конфигурация захвата (setters сохраняют, ничего не делают) ---

    void SetTargetProcess(const std::wstring& exePath) override {
        // Wintun-режим матчит через RuleEngine (apps[]); поле сохраняется
        // только для совместимости.
        m_targetProcessPath = exePath;
    }

    void SetRelayPort(uint16_t port) override { m_relayPort = port; }

    void SetProxyConfig(const std::string& host, uint16_t port) override {
        m_proxyHost = host;
        m_proxyPort = port;
    }

    void SetConnectionTable(domain::ports::IConnectionTable* table) override {
        m_connTable = table;
    }

    // --- ICapture: события перенаправления (заглушки) ---

    std::vector<domain::RedirectEvent> GetPendingRedirects(
        uint32_t /*timeout_ms*/ = 1000) override {
        return {};
    }

    bool AckRedirect(uint64_t /*redirect_id*/) override { return true; }

    // --- ICapture: статистика/системные ---

    domain::DriverStats GetStats() override { return {}; }

    void* GetEventHandle() const override { return nullptr; }

    // --- WP6: полиморфные accessor-ы (переопределяют ICapture) ---

    bool SetRuleEngine(domain::services::RuleEngine* engine) override {
        m_ruleEngine = engine;
        return true;
    }

    uint64_t GetTotalRxBytes()       const override { return 0; }
    uint64_t GetTotalTxBytes()       const override { return 0; }
    uint32_t GetActiveConnections()  const override {
        return m_connTable
            ? static_cast<uint32_t>(m_connTable->GetTrackedCount())
            : 0u;
    }

    // --- Инфраструктурные setters (не в ICapture, но повторяют форму WinDivertCapture) ---

    void SetLogSink(domain::ports::ILogSink* sink) { m_logSink = sink; }
    void SetConnectionMonitor(domain::ports::IConnectionMonitor* mon) {
        m_connectionMonitor = mon;
    }

    // Доступ к сохранённым настройкам (WP10 подхватит).
    const WintunSettings& GetSettings() const { return m_settings; }

private:
    // Сохранённые настройки Wintun (WP10 consume).
    WintunSettings m_settings;

    // Логирование (может быть nullptr — тогда Open() тихо возвращает true).
    domain::ports::ILogSink* m_logSink = nullptr;

    // ICapture wiring.
    domain::ports::IConnectionTable*        m_connTable         = nullptr;
    domain::ports::IConnectionMonitor*      m_connectionMonitor = nullptr;
    domain::services::RuleEngine*           m_ruleEngine        = nullptr;

    // Сохранённая конфигурация (для WP10).
    std::wstring m_targetProcessPath;
    std::string  m_proxyHost;
    uint16_t     m_proxyPort = 0;
    uint16_t     m_relayPort = 0;

    // Псевдо-состояние жизненного цикла.
    bool m_opened = false;
};

} // namespace infrastructure
} // namespace tcp_redirector
