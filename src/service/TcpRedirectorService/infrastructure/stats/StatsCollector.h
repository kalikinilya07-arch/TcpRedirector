#pragma once

//
// StatsCollector — lock-free сбор статистики перехвата пакетов.
// - Все счётчики: std::atomic (fetch_add relaxed)
// - Per-second rate: дифференциальное вычисление в GetStats (не в OnPacket)
// - Min/Max: compare_exchange_weak
// - Никаких мьютексов в OnPacket
// - GetStats(): атомарный снимок всех счётчиков
// - Reset(): store(0) на все счётчики
//

#include <cstdint>
#include <atomic>
#include <chrono>

namespace tcp_redirector {
namespace infrastructure {

// Информация о пакете, передаваемая в OnPacket
struct PacketInfo {
    uint32_t packet_size = 0;     // размер пакета в байтах
    uint8_t  protocol = 0;        // 6=TCP, 0=other (UDP не обрабатывается CaptureLoop)
    bool     redirected = false;  // пакет перенаправлен на прокси
    bool     error = false;       // ошибка отправки на прокси
    uint64_t flow_id = 0;         // ID потока (из RedirectEvent)
};

// Снимок статистики для GUI
struct StatsSnapshot {
    uint64_t totalPackets = 0;       // всего перехвачено пакетов
    uint64_t tcpPackets = 0;         // из них TCP
    uint64_t redirectedPackets = 0;  // перенаправлено на прокси
    uint64_t errors = 0;             // ошибок отправки

    uint64_t totalBytes = 0;         // суммарный объём трафика
    double   avgPacketSize = 0.0;    // средний размер пакета
    uint32_t minPacketSize = 0;      // минимальный размер
    uint32_t maxPacketSize = 0;      // максимальный размер

    double   packetsPerSecond = 0.0; // пакетов в секунду (дифференциально)
};

class StatsCollector {
public:
    StatsCollector();

    // Вызывается из CaptureLoop для каждого пакета (lock-free)
    void OnPacket(const PacketInfo& info);

    // Атомарный снимок всех счётчиков
    StatsSnapshot GetStats();

    // Сброс всех счётчиков в ноль
    void Reset();

private:
    // Атомарные счётчики (только fetch_add relaxed)
    std::atomic<uint64_t> m_totalPackets{0};
    std::atomic<uint64_t> m_tcpPackets{0};
    std::atomic<uint64_t> m_redirectedPackets{0};
    std::atomic<uint64_t> m_errors{0};
    std::atomic<uint64_t> m_totalBytes{0};

    // Min/Max через compare_exchange_weak
    std::atomic<uint32_t> m_minPacketSize{0xFFFFFFFFu};
    std::atomic<uint32_t> m_maxPacketSize{0};

    // Per-second rate (дифференциальный — вычисляется в GetStats)
    std::chrono::steady_clock::time_point m_lastStatsCall;
    uint64_t m_lastTotalPackets = 0;
};

} // namespace infrastructure
} // namespace tcp_redirector