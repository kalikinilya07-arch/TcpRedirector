#pragma once

/**
 * @file StatsCollector.h
 * @brief Lock-free сборщик статистики перехвата пакетов.
 *
 * Все счётчики используют std::atomic (fetch_add relaxed).
 * Per-second rate вычисляется дифференциально в GetStats (не в OnPacket).
 * Min/Max — через compare_exchange_weak.
 * Никаких мьютексов в OnPacket.
 * GetStats() — атомарный снимок всех счётчиков.
 * Reset() — store(0) на все счётчики.
 */

#include <cstdint>
#include <atomic>
#include <chrono>

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Информация о пакете, передаваемая в OnPacket.
 *
 * Содержит размер, протокол, флаги перенаправления и ошибки,
 * а также ID потока для отслеживания.
 */
struct PacketInfo {
    uint32_t packet_size = 0;     //!< Размер пакета в байтах
    uint8_t  protocol = 0;        //!< Протокол: 6=TCP, 0=other
    bool     redirected = false;  //!< Пакет перенаправлен на прокси
    bool     error = false;       //!< Ошибка отправки на прокси
    uint64_t flow_id = 0;         //!< ID потока (из RedirectEvent)
};

/**
 * @brief Снимок статистики для GUI.
 *
 * Содержит счётчики пакетов, объём трафика, мин/макс/средний размер
 * и скорость (пакетов в секунду, дифференциально).
 */
struct StatsSnapshot {
    uint64_t totalPackets = 0;       //!< Всего перехвачено пакетов
    uint64_t tcpPackets = 0;         //!< Из них TCP
    uint64_t redirectedPackets = 0;  //!< Перенаправлено на прокси
    uint64_t errors = 0;             //!< Ошибок отправки

    uint64_t totalBytes = 0;         //!< Суммарный объём трафика
    double   avgPacketSize = 0.0;    //!< Средний размер пакета
    uint32_t minPacketSize = 0;      //!< Минимальный размер
    uint32_t maxPacketSize = 0;      //!< Максимальный размер

    double   packetsPerSecond = 0.0; //!< Пакетов в секунду (дифференциально)
};

/**
 * @brief Lock-free сборщик статистики.
 *
 * Предназначен для вызова из CaptureLoop без блокировок.
 * OnPacket — lock-free (только fetch_add relaxed).
 * GetStats — атомарный снимок с дифференциальным расчётом скорости.
 */
class StatsCollector {
public:
    StatsCollector();

    /**
     * @brief Зарегистрировать пакет (lock-free).
     *
     * Вызывается из CaptureLoop для каждого пакета.
     * Использует только atomic fetch_add relaxed и compare_exchange_weak.
     * @param info Информация о пакете.
     */
    void OnPacket(const PacketInfo& info);

    /**
     * @brief Получить атомарный снимок статистики.
     *
     * Читает все счётчики, вычисляет средний размер пакета
     * и скорость пакетов в секунду (дифференциально от предыдущего вызова).
     * @return Снимок StatsSnapshot.
     */
    StatsSnapshot GetStats();

    /**
     * @brief Сбросить все счётчики в ноль.
     */
    void Reset();

private:
    // Атомарные счётчики (только fetch_add relaxed)
    std::atomic<uint64_t> m_totalPackets{0};       //!< Всего пакетов
    std::atomic<uint64_t> m_tcpPackets{0};          //!< TCP-пакетов
    std::atomic<uint64_t> m_redirectedPackets{0};   //!< Перенаправлено
    std::atomic<uint64_t> m_errors{0};              //!< Ошибок
    std::atomic<uint64_t> m_totalBytes{0};          //!< Всего байт

    // Min/Max через compare_exchange_weak
    std::atomic<uint32_t> m_minPacketSize{0xFFFFFFFFu}; //!< Мин. размер
    std::atomic<uint32_t> m_maxPacketSize{0};           //!< Макс. размер

    // Per-second rate (дифференциальный — вычисляется в GetStats)
    std::chrono::steady_clock::time_point m_lastStatsCall; //!< Время последнего вызова GetStats
    uint64_t m_lastTotalPackets = 0;  //!< Счётчик на момент последнего вызова
};

} // namespace infrastructure
} // namespace tcp_redirector