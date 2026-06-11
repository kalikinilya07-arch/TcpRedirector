#include "StatsCollector.h"

namespace tcp_redirector {
namespace infrastructure {

StatsCollector::StatsCollector() {
    m_lastStatsCall = std::chrono::steady_clock::now();
}

void StatsCollector::OnPacket(const PacketInfo& info) {
    // 1. Общие счётчики
    m_totalPackets.fetch_add(1, std::memory_order_relaxed);
    m_totalBytes.fetch_add(info.packet_size, std::memory_order_relaxed);

    // 2. По протоколу (только TCP — UDP не обрабатывается CaptureLoop)
    if (info.protocol == 6) {
        m_tcpPackets.fetch_add(1, std::memory_order_relaxed);
    }

    // 3. Редирект / ошибки
    if (info.redirected) {
        m_redirectedPackets.fetch_add(1, std::memory_order_relaxed);
    }
    if (info.error) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
    }

    // 4. Min/Max через compare_exchange_weak (почти никогда не выполняется)
    uint32_t size = info.packet_size;
    uint32_t expectedMin = m_minPacketSize.load(std::memory_order_relaxed);
    while (size < expectedMin) {
        if (m_minPacketSize.compare_exchange_weak(expectedMin, size,
                std::memory_order_relaxed)) break;
    }
    uint32_t expectedMax = m_maxPacketSize.load(std::memory_order_relaxed);
    while (size > expectedMax) {
        if (m_maxPacketSize.compare_exchange_weak(expectedMax, size,
                std::memory_order_relaxed)) break;
    }
}

StatsSnapshot StatsCollector::GetStats() {
    StatsSnapshot s;

    // Атомарное чтение всех счётчиков (seq_cst по умолчанию)
    s.totalPackets      = m_totalPackets.load();
    s.tcpPackets        = m_tcpPackets.load();
    s.redirectedPackets = m_redirectedPackets.load();
    s.errors            = m_errors.load();
    s.totalBytes        = m_totalBytes.load();
    s.minPacketSize     = m_minPacketSize.load();
    s.maxPacketSize     = m_maxPacketSize.load();

    // Средний размер пакета
    s.avgPacketSize = (s.totalPackets > 0)
        ? static_cast<double>(s.totalBytes) / s.totalPackets
        : 0.0;

    // Per-second rate (дифференциальное вычисление)
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastStatsCall).count();
    uint64_t packetsSinceLast = s.totalPackets - m_lastTotalPackets;

    s.packetsPerSecond = (elapsed_ms > 0)
        ? static_cast<double>(packetsSinceLast) * 1000.0 / elapsed_ms
        : 0.0;

    // Обновить состояние для следующего вызова
    m_lastStatsCall = now;
    m_lastTotalPackets = s.totalPackets;

    return s;
}

void StatsCollector::Reset() {
    m_totalPackets.store(0);
    m_tcpPackets.store(0);
    m_redirectedPackets.store(0);
    m_errors.store(0);
    m_totalBytes.store(0);
    m_minPacketSize.store(0xFFFFFFFFu);
    m_maxPacketSize.store(0);

    m_lastStatsCall = std::chrono::steady_clock::now();
    m_lastTotalPackets = 0;
}

} // namespace infrastructure
} // namespace tcp_redirector