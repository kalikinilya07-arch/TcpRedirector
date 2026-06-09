# Риски, ограничения и производительность

## 1. Технические риски

### Риск 1: WFP Redirect Handle управление

**Описание**: `FwpsRedirectHandleCreate0` создаёт системный ресурс, который должен быть уничтожен. В Windows 10/11 существуют ограничения на количество redirect handle'ов.

**Вероятность**: Средняя

**Влияние**: Критическое — невозможность выполнять редирект

**Митигация**:
- Создавать один handle на ядро процессора (или один глобальный)
- Использовать `FwpsRedirectHandleDestroy0` при выгрузке драйвера
- Обрабатывать STATUS_INSUFFICIENT_RESOURCES
- Тестирование с 1000+ одновременных редиректов

---

### Риск 2: Driver Verifier — DPC контекст

**Описание**: Callout классификации может выполняться в DPC контексте, где доступны ограниченные API. Нельзя использовать:
- `ZwQueryInformationProcess` в DPC
- `SeLocateProcessImageName` в DPC
- Pageable memory

**Вероятность**: Высокая

**Влияние**: BSOD при первом же редиректе

**Митигация**:
- Проверить `inMetaValues->currentMetadata & FWPS_METADATA_FIELD_SYSTEM_FLAGS` для определения контекста
- Если DPC: использовать `ExAllocatePool2` с NonPagedPool
- Отложить получение полного пути процесса в work item
- В callout получать только PID (безопасно), остальное — в work queue
- Обязательное тестирование с Driver Verifier (Special Pool + DDI compliance)

---

### Риск 3: Совместимость ALE слоёв в разных версиях Windows

**Описание**: Поведение WFP ALE слоёв менялось между Windows 10, 11, Server. Некоторые поля метаданных могут отсутствовать.

**Вероятность**: Низкая

**Влияние**: Среднее — некорректная работа на некоторых версиях

**Митигация**:
- Проверять версию ОС через `RtlGetVersion`
- Использовать conditional fields: проверять `FWPS_IS_METADATA_FIELD_PRESENT`
- Тестировать на Windows 10 22H2, Windows 11 24H2, Windows Server 2022

---

### Риск 4: Утечка памяти в драйвере

**Описание**: Каждый редирект выделяет REDIRECT_INFO. Если user-mode сервис не обрабатывает события (упал/завис), очередь растёт.

**Вероятность**: Средняя

**Влияние**: Высокое — утечка non-paged pool, BSOD

**Митигация**:
- Ограничение очереди: 4096 элементов
- Timeout: 30 секунд на обработку редиректа
- Если очередь полна → блокировка новых соединений
- Если сервис не отвечает → FWP_ACTION_BLOCK
- Watchdog в драйвере: проверка активности сервиса

---

### Риск 5: Производительность Bridging при 1000 соединений

**Описание**: Каждое соединение требует 2 асинхронных чтения (local + proxy). При 1000 соединений это 2000 одновременных async_read операций на Boost.Asio.

**Вероятность**: Средняя

**Влияние**: Высокое — рост CPU, падение пропускной способности

**Митигация**:
- Использовать `io_context` с `SO_REUSEPORT` или несколько io_context
- Увеличить io_context::run() до числа ядер
- Увеличить буферы до 128KB (меньше системных вызовов)
- Рассмотреть IOCP напрямую вместо Boost.Asio для high-throughput
- Профилирование с 1000 соединений

---

### Риск 6: Проблемы с подписью драйвера

**Описание**: Windows 10/11 требуют подписанные драйверы для загрузки. Self-signed не работает на production системах.

**Вероятность**: Высокая

**Влияние**: Критическое — драйвер не загружается

**Митигация**:
- Для разработки: включить тестовый режим подписи (`bcdedit /set testsigning on`)
- Для production: EV-сертификат + Hardware Dev Center submission
- Альтернатива: Windows Hardware Compatibility Program

---

### Риск 7: Антивирусы и блокировка WFP

**Описание**: Некоторые антивирусы перехватывают ALE слои раньше, блокируя наш callout.

**Вероятность**: Средняя

**Влияние**: Среднее — редирект не срабатывает

**Митигация**:
- Поддержка разных подслоев (sublayer weight)
- Логирование конфликтов с другими WFP фильтрами
- Документация: известные конфликты

---

## 2. Ограничения решения

### Функциональные ограничения

| Ограничение | Причина | Возможное решение в будущем |
|-------------|---------|---------------------------|
| Только HTTP CONNECT Proxy | Архитектурное решение | Добавить SOCKS5 |
| Нет fallback при недоступности прокси | Требование безопасности | Опциональный fallback |
| Только TCP | WFP ALE не предназначен для UDP redirect | WinDivert (если допустить) |
| Нет QUIC/HTTP3 | QUIC использует UDP | — |
| Нет TLS MITM | Требование безопасности | — |
| Один прокси | MVP ограничение | Proxy chaining |
| Нет балансировки | MVP ограничение | Round-robin |
| Нет кэширования DNS | Требование не указано | DNS cache |
| Нет WebSocket/SSE специфики | Прозрачный туннель | — |

### Системные ограничения

| Ограничение | Значение | Примечание |
|-------------|----------|------------|
| Max редиректов в очереди | 4096 | Ограничение non-paged pool |
| Max одновременных соединений | 1000 (MVP) → 5000 (production) | Ограничение по RAM |
| Max правил | 1024 | Размер кэша в драйвере |
| Max размер конфига | 1 MB | — |
| Max размер лог-файла | 50 MB | Настраивается |
| Timeout CONNECT ответа | 30 сек | Настраивается |
| Idle timeout | 300 сек (5 мин) | Настраивается |
| Требуемые права | Администратор | Для загрузки драйвера |

### Совместимость с прокси

Требования к HTTP Proxy:
- Обязательно: HTTP CONNECT метод
- Обязательно: HTTP/1.1 200 Connection Established
- Опционально: Proxy-Authorization Basic
- Опционально: Keep-Alive

**Проверенные прокси**:
- [ ] Squid (необходимо тестирование)
- [ ] HAProxy (необходимо тестирование)
- [ ] NGINX (stream module) (необходимо тестирование)
- [ ] mitmproxy (для отладки)
- [ ] CCProxy (необходимо тестирование)
- [ ] 3proxy (необходимо тестирование)

---

## 3. Производительность

### Целевые показатели MVP

| Метрика | Цель | Метод измерения |
|---------|------|----------------|
| Одновременных TCP | 1000 | Мониторинг сервиса |
| CPU (средний) | <5% | Performance counters |
| RAM (общая) | <200 MB | Process Private Bytes |
| Задержка (дополнительная) | <1 ms | Локальный тест ping-pong |

### Оценка использования памяти

```
На одно соединение:
---------------------
REDIRECT_INFO (kernel):       ~280 bytes
ProxySession object:          ~1 KB
Socket buffers (2 x 64KB):    ~128 KB
Boost.Asio internal:          ~4 KB
ConnectionTracker record:     ~512 bytes
------------------------------------------
Итого на соединение:          ~134 KB

Для 1000 соединений:
---------------------
RAM на соединения:            ~134 MB
Service overhead:             ~30 MB
Driver (WFP + очередь):      ~10 MB
GUI:                          ~20 MB
spdlog buffers:               ~5 MB
------------------------------------------
Итого:                        ~199 MB ✅ (в пределах 200 MB)
```

### Узкие места

1. **Kernel Callout (ALE_AUTH_CONNECT)**
   - Потенциальная проблема: callout выполняется в DPC
   - Решение: минимум работы в callout, только сбор PID и базовых метаданных
   - Оценка: <5 мкс на вызов

2. **Kernel→User mode переход**
   - Потенциальная проблема: каждый редирект требует переключения контекста
   - Решение: батчинг — драйвер накапливает до 64 событий перед уведомлением
   - Оценка: <100 мкс на событие (амортизировано)

3. **Boost.Asio async_read цикл**
   - Потенциальная проблема: 2 async_read на соединение = 2000 чтений
   - Решение: `io_context` с несколькими потоками, `SO_RCVBUF` оптимизация
   - Оценка: <10% CPU при 1000 idle соединений

4. **HTTP Proxy latency**
   - Потенциальная проблема: дополнительный RTT к прокси при CONNECT
   - Решение: нет (требование — CONNECT обязателен)
   - Оценка: +1-3 RTT к начальной задержке соединения

5. **Named Pipe IPC (Service↔GUI)**
   - Потенциальная проблема: push-уведомления каждую секунду
   - Решение: diff-based updates, не полный список
   - Оценка: <1% CPU

### Масштабирование

| Метод масштабирования | Эффект | Сложность |
|-----------------------|--------|-----------|
| Увеличение потоков io_context | +20% throughput | Низкая |
| Увеличение буферов (128KB → 256KB) | +10% throughput | Низкая |
| Multiple redirect handles (per CPU) | +30% на многоядерных | Средняя |
| Memory-mapped buffer для KM→UM | -50% latency | Высокая |
| TCP chimney offload | +15% throughput | Средняя |
| RSS (Receive Side Scaling) настройка | +20% throughput | Средняя |
| Event batching в драйвере | +50% throughput (пиковый) | Низкая |

### Мониторинг производительности

```cpp
// Структура для сбора статистики производительности
struct PerformanceSnapshot {
    uint64_t redirectsTotal;
    uint64_t redirectsPerSecond;
    uint64_t activeConnections;
    double avgRedirectLatencyMs;    // время от connect() до CONNECT
    double avgTunnelLatencyMs;      // время от CONNECT до 200 OK
    uint64_t totalRxBytes;
    uint64_t totalTxBytes;
    uint64_t proxyErrors;
    uint64_t queuedRedirects;       // текущая длина очереди драйвера
    uint64_t driverPoolUsage;       // NonPagedPool usage
    uint64_t failedRedirects;       // ошибки редиректа
};
```

**Рекомендуемые Performance Counters:**

```
\TcpRedirector\Active Connections
\TcpRedirector\Redirects/sec
\TcpRedirector\Total RX Bytes
\TcpRedirector\Total TX Bytes
\TcpRedirector\Proxy Errors/sec
\TcpRedirector\Queue Length
```

## 4. Аудит безопасности

### Хранение пароля

```json
// %ProgramData%\TcpRedirector\config.json
{
    "proxy": {
        "host": "proxy.example.com",
        "port": 3128,
        "login": "user123",
        "password_encrypted": "AQAAANCMnd8BFdERjHoAwE/Cl+sBAAAA...",
        "password_nonce": "base64==="
    },
    "rules": [...],
    "logging": {
        "level": "INFO",
        "max_file_size_mb": 50,
        "max_files": 10
    }
}
```

**Механизм DPAPI:**

```cpp
// Шифрование
DATA_BLOB plainBlob = { (DWORD)password.size() * 2, (BYTE*)password.data() };
DATA_BLOB encryptedBlob = {0};
CryptProtectData(
    &plainBlob,
    L"TcpRedirector Proxy Password",
    NULL,           // optional entropy
    NULL,           // reserved
    NULL,           // prompt struct
    CRYPTPROTECT_UI_FORBIDDEN,
    &encryptedBlob
);
// encryptedBlob.pbData → сохраняем в JSON (base64 encoded)

// Расшифровка
DATA_BLOB encryptedBlob = { (DWORD)base64decoded.size(), base64decoded.data() };
DATA_BLOB plainBlob = {0};
CryptUnprotectData(
    &encryptedBlob,
    NULL,
    NULL,           // optional entropy
    NULL,           // reserved
    NULL,           // prompt struct
    CRYPTPROTECT_UI_FORBIDDEN,
    &plainBlob
);
// plainBlob.pbData → пароль (wchar_t*)
```

### Security Checklist

- [ ] Пароль не логируется ни на каком уровне
- [ ] Пароль не передаётся в GUI (только булево has_password)
- [ ] Named pipe DACL: только Administrators
- [ ] IOCTL буферы проверяются на корректный размер
- [ ] Driver: probe for user-mode buffers (ProbeForRead/ProbeForWrite)
- [ ] Config file: NTFS permissions (Administrators only)
- [ ] Логи: не содержат чувствительных данных
- [ ] Установщик: запрос UAC для прав администратора