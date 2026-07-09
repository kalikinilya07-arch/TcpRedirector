# Статистика TcpRedirector

## Архитектура

Файл: [`StatsCollector.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/stats/StatsCollector.h), [`StatsCollector.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/stats/StatsCollector.cpp)

### Счётчики

Все счётчики — `std::atomic<uint64_t>`, lock-free:

| Счётчик | Описание |
|---------|----------|
| `m_totalPackets` | Всего захваченных пакетов |
| `m_totalRxBytes` | Всего принятых байт (client→relay) |
| `m_totalTxBytes` | Всего отправленных байт (relay→client) |
| `m_redirectsEmitted` | Количество редиректов |
| `m_activeConnections` | Текущие активные соединения |
| `m_proxyErrors` | Ошибки подключения к прокси |
| `m_uptimeSeconds` | Uptime сервиса в секундах |

### StatsSnapshot

```cpp
struct StatsSnapshot {
    uint64_t totalPackets;
    uint64_t totalRxBytes;
    uint64_t totalTxBytes;
    uint64_t totalRedirects;
    uint32_t activeConnections;
    uint32_t proxyErrors;
    uint64_t uptimeSeconds;
    double packetsPerSecond;
    double bytesPerSecondRx;
    double bytesPerSecondTx;
};
```

### Per-second rate

Вычисляется дифференциально в `GetStats()`: разница счётчиков между вызовами, делённая на прошедшее время. Не требует отдельного потока для сброса счётчиков.

### Интеграция с WinDivertCapture

Счётчики инкрементируются в `CaptureLoop` через `m_connectionMonitor`:
- `m_totalRxBytes` / `m_totalTxBytes` — при подсчёте байт в `RestoreFromRelay()` и `ModifyDstToRelay()`
- `m_activeConnections` — при добавлении/удалении из ConnectionTable
- `m_proxyErrors` — при ошибках CONNECT в TcpRelayServer
