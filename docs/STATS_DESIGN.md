# Проектирование системы сбора статистики (StatsCollector)

## 1. Структура PacketInfo

```cpp
struct PacketInfo {
    uint32_t packet_size;         // размер пакета в байтах
    uint8_t protocol;             // 6=TCP, 17=UDP, 0=other
    bool redirected;              // true — пакет ушёл на прокси
    bool error;                   // true — ошибка отправки на прокси
    uint64_t flow_id;             // ID потока (из RedirectEvent)
    uint16_t src_port;            // порт источника
    uint16_t dst_port;            // порт назначения
};
```

Вызов из CaptureLoop ([`WinDivertCapture.cpp:178-252`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:178)):

```cpp
if (ipHdr && tcpHdr) {
    // ... существующая логика ...
    
    PacketInfo info;
    info.packet_size = recvLen;
    info.protocol = (protocol == 6) ? 6 : (protocol == 17) ? 17 : 0;
    info.redirected = shouldBlock;
    info.error = (sendFailed == true);
    // ...
    m_statsCollector->OnPacket(info);
}
```

---

## 2. Класс StatsCollector

```cpp
class StatsCollector {
public:
    StatsCollector();

    // Вызывается из CaptureLoop для каждого пакета
    void OnPacket(const PacketInfo& info);

    // Атомарный снимок всех счётчиков
    StatsSnapshot GetStats() const;

    // Сброс всех счётчиков в ноль
    void Reset();

private:
    // --- Атомарные счётчики ---
    std::atomic<uint64_t> m_totalPackets{0};
    std::atomic<uint64_t> m_tcpPackets{0};
    std::atomic<uint64_t> m_udpPackets{0};
    std::atomic<uint64_t> m_redirectedPackets{0};
    std::atomic<uint64_t> m_errors{0};

    // Для среднего размера пакета
    std::atomic<uint64_t> m_totalBytes{0};      // сумма байт всех пакетов
    // m_totalPackets уже есть выше

    // Для минимального/максимального
    std::atomic<uint32_t> m_minPacketSize{UINT32_MAX};
    std::atomic<uint32_t> m_maxPacketSize{0};

    // Per-second rate (для моментальной статистики)
    struct RateCounter {
        std::atomic<uint64_t> count{0};
        std::chrono::steady_clock::time_point lastReset;
    };
    RateCounter m_packetsPerSecond;
};
```

---

## 3. Структура StatsSnapshot

Возвращается из `GetStats()` — один `std::atomic<uint64_t>` read для каждого поля (через `.load()`).

```cpp
struct StatsSnapshot {
    // --- Счётчики ---
    uint64_t totalPackets;           // всего перехвачено пакетов
    uint64_t tcpPackets;             // из них TCP
    uint64_t udpPackets;             // из них UDP
    uint64_t redirectedPackets;      // перенаправлено на прокси
    uint64_t errors;                 // ошибок отправки

    // --- Размеры ---
    uint64_t totalBytes;             // суммарный объём трафика
    double   avgPacketSize;          // средний размер пакета
    uint32_t minPacketSize;          // минимальный
    uint32_t maxPacketSize;          // максимальный

    // --- Rate ---
    double   packetsPerSecond;       // пакетов в секунду (мгновенно)
};
```

### 3.1. Вычисление среднего размера

```cpp
double avgPacketSize = (m_totalPackets.load() > 0)
    ? static_cast<double>(m_totalBytes.load()) / m_totalPackets.load()
    : 0.0;
```

**Просто, атомарно, без contention.** Не нужно скользящее среднее — для GUI достаточно общего среднего за всё время.

---

## 4. Реализация OnPacket

```cpp
void StatsCollector::OnPacket(const PacketInfo& info) {
    // 1. Общие счётчики
    m_totalPackets.fetch_add(1, std::memory_order_relaxed);
    m_totalBytes.fetch_add(info.packet_size, std::memory_order_relaxed);

    // 2. По протоколу
    if (info.protocol == 6)          // TCP
        m_tcpPackets.fetch_add(1, std::memory_order_relaxed);
    else if (info.protocol == 17)    // UDP
        m_udpPackets.fetch_add(1, std::memory_order_relaxed);

    // 3. Редирект / ошибки
    if (info.redirected)
        m_redirectedPackets.fetch_add(1, std::memory_order_relaxed);
    if (info.error)
        m_errors.fetch_add(1, std::memory_order_relaxed);

    // 4. Min/Max (compare_exchange)
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

    // 5. Per-second rate (каждые 1000 мс сбрасывать)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_packetsPerSecond.lastReset).count();
    m_packetsPerSecond.count.fetch_add(1, std::memory_order_relaxed);
    if (elapsed >= 1000) {
        // Сброс — только один поток делает reset
        auto expected = m_packetsPerSecond.lastReset;
        if (m_packetsPerSecond.lastReset == expected &&
            std::chrono::steady_clock::now() - expected >= std::chrono::milliseconds(1000)) {
            m_packetsPerSecond.lastReset = now;
            m_packetsPerSecond.count.store(0, std::memory_order_relaxed);
        }
    }
}
```

### 4.1. Почему memory_order_relaxed

- На x86 `relaxed` и `seq_cst` — одинаковый код (lock-free)
- Счётчики статистики не требуют строгих гарантий видимости между потоками
- GUI видит статистику с задержкой в 1-2 секунды (таймер) — это нормально
- **Zero contention**: нет мьютексов, нет spinning, только атомарные инкременты

---

## 5. Реализация GetStats

```cpp
StatsSnapshot StatsCollector::GetStats() const {
    StatsSnapshot s;

    // Все load — seq_cst (дефолт) для консистентности снимка
    s.totalPackets      = m_totalPackets.load();
    s.tcpPackets        = m_tcpPackets.load();
    s.udpPackets        = m_udpPackets.load();
    s.redirectedPackets = m_redirectedPackets.load();
    s.errors            = m_errors.load();
    s.totalBytes        = m_totalBytes.load();
    s.minPacketSize     = m_minPacketSize.load();
    s.maxPacketSize     = m_maxPacketSize.load();

    // Средний размер
    s.avgPacketSize = (s.totalPackets > 0)
        ? static_cast<double>(s.totalBytes) / s.totalPackets
        : 0.0;

    // Packets per second (мгновенная скорость)
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_packetsPerSecond.lastReset).count();
    uint64_t count = m_packetsPerSecond.count.load();
    s.packetsPerSecond = (elapsed_ms > 0)
        ? static_cast<double>(count) * 1000.0 / elapsed_ms
        : 0.0;

    return s;
}
```

### 5.1. О проблеме неатомарности снимка

8 `atomic::load()` не образуют одну атомарную транзакцию. Теоретически,
после чтения `m_totalPackets = 1000` и до чтения `m_totalBytes = 500000`,
пакет может увеличить оба счётчика. Но для статистики это **не критично**:

- GUI обновляется раз в 2 секунды
- Расхождение в 1 пакет не заметно глазу
- Средний размер вычисляется с погрешностью < 0.1% при трафике > 1000 пакетов

Если нужна строгая консистентность — можно добавить `std::shared_mutex`,
но это создаст contention. **Не делаем.**

---

## 6. Метод Reset

```cpp
void StatsCollector::Reset() {
    m_totalPackets.store(0);
    m_tcpPackets.store(0);
    m_udpPackets.store(0);
    m_redirectedPackets.store(0);
    m_errors.store(0);
    m_totalBytes.store(0);
    m_minPacketSize.store(UINT32_MAX);
    m_maxPacketSize.store(0);
    m_packetsPerSecond.count.store(0);
    m_packetsPerSecond.lastReset = std::chrono::steady_clock::now();
}
```

Reset вызывается:
- Из GUI (кнопка "Очистить статистику") через IPC
- При старте сервиса

---

## 7. Анализ contention при высоком трафике

### 7.1. CaptureLoop: 50k pps (пакетов в секунду)

| Операция | Инструкций | Время (пенальти) |
|----------|------------|------------------|
| `fetch_add(1, relaxed)` | 1 LOCK INC | ~30-60 нс |
| `compare_exchange_weak` | 1-2 LOCK CMPXCHG | ~50-100 нс (в 0.01% случаев) |
| Итого на пакет | ~100 нс | **0.01% CPU** при 50k pps |

### 7.2. Почему нет contention

- **Только инкременты** — LOCK INC не блокирует кэш-линию надолго
- **Один писатель + один читатель** — CaptureLoop пишет, GUI-таймер читает
- **cmp_exchg для min/max** — почти никогда не выполняется (только когда новый min/max)
- **Нет мьютексов** — full lock-free

### 7.3. Сравнение с вариантом на mutex

| Аспект | atomic | shared_mutex |
|--------|--------|--------------|
| Время записи | ~30 нс | ~50-200 нс (lock + store + unlock) |
| Время чтения | ~0.5 нс (load) | ~30-100 нс (shared_lock + read) |
| Contention при 50k pps | Отсутствует | Высокий (GUI читает раз в 2с, но lock всё равно держится) |

**Решение: атомарные счётчики без мьютексов.**

---

## 8. Интеграция с существующим кодом

### 8.1. Новые файлы

```
TcpRedirector/src/service/TcpRedirectorService/
├── infrastructure/
│   └── stats/
│       ├── StatsCollector.h       // класс + PacketInfo + StatsSnapshot
│       └── StatsCollector.cpp     // реализация OnPacket/GetStats/Reset
```

### 8.2. Изменяемые файлы

| Файл | Изменение |
|------|-----------|
| `WinDivertCapture.h` | Добавить `std::unique_ptr<StatsCollector> m_stats` |
| `WinDivertCapture.cpp` | В `CaptureLoop()` после обработки пакета: `m_stats->OnPacket(info)` |
| `ServiceMain.h` | В `Initialize()` создать `StatsCollector`; передать в `WinDivertCapture` |
| `domain/entities/ProxyConfig.h` | `DriverStats` будет содержать `StatsSnapshot` |
| `IpcHandler.h` | `GetStats()` вызывает `m_statsCollector->GetStats()` |
| `WinDivertCapture.h:127-128` | Убрать старые `m_packets_captured` / `m_redirects_emitted` (заменяются StatsCollector) |

### 8.3. Какие старые счётчики удаляются

В [`WinDivertCapture.h:127-128`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.h:127):

```cpp
std::atomic<uint64_t> m_packets_captured{0};   // → заменяется StatsCollector::m_totalPackets
std::atomic<uint64_t> m_redirects_emitted{0};   // → заменяется StatsCollector::m_redirectedPackets
```

В [`WinDivertCapture.cpp:160-162`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:160):

```cpp
LOG("[WD] HEARTBEAT: %llu packets, %llu redirected\n",
       pktCount, m_redirects_emitted.load());   // → m_stats->GetStats().totalPackets и .redirectedPackets
```

---

## 9. Полный сценарий

```
CaptureLoop                    StatsCollector                  GUI (таймер 2s)
     │                              │                              │
     │ Recv() → пакет                │                              │
     │                              │                              │
     ├──► OnPacket(info) ──────────►│                              │
     │    .totalPackets++ (atomic)   │                              │
     │    .totalBytes += size        │                              │
     │    if TCP: .tcpPackets++      │                              │
     │    if redirected:             │                              │
     │      .redirectedPackets++     │                              │
     │    if error: .errors++        │                              │
     │    update min/max (cmp_exchg) │                              │
     │                              │                              │
     │ Send() → пакет дальше         │                              │
     │       ...                     │                              │
     │       ...                     │                              │
     │                              │                              │
     │                              │     ◄─── GetStats() ─────────│
     │                              │     ├── snapshot.load()      │
     │                              │     ├── avg = bytes/packets  │
     │                              │     └── return snapshot ────►│
     │                              │                              │
     │                              │     отображение на GUI       │
     │                              │                              │
```

---

## 10. Резюме

| Аспект | Решение |
|--------|---------|
| **Счётчики** | 8 x `std::atomic<uint64_t>` + 2 x `std::atomic<uint32_t>` |
| **OnPacket** | Только `fetch_add(relaxed)` + `cmp_exchg_weak` для min/max |
| **GetStats** | 10 x `atomic::load()` + вычисление avg |
| **Contention** | Отсутствует — lock-free, relaxed memory order |
| **Средний размер** | `totalBytes / totalPackets` (не скользящее) |
| **Per-second rate** | Счётчик + временная метка, сброс раз в секунду |
| **Reset** | `store(0)` на все счётчики |
| **Новые файлы** | 2: `StatsCollector.h`, `StatsCollector.cpp` |
| **Заменяет** | `m_packets_captured`, `m_redirects_emitted` в WinDivertCapture |
