# Аудит: DST Modification vs ProxyBridge — что сделано не так

## Дата: 2026-06-18

## Источники:
1. **TcpRedirector_11_06** — версия от 11 июня (рабочая, shouldBlock=true)
2. **PACKET_INJECTION_TASK.md** — задание на DST modification (пережило потерю кода)

---

## ⚠️ ГЛАВНАЯ ПРОБЛЕМА: Архитектура перехвата

### Как работает ProxyBridge (ПРАВИЛЬНО):
1. Пакет SYN перехватывается WinDivert
2. **НЕ блокируется**, а **МОДИФИЦИРУЕТСЯ**:
   - `DstPort` → relayPort (34010)
   - Если src и dst НЕ loopback: `SrcAddr ↔ DstAddr` + `addr.Outbound = FALSE`
   - Если loopback→loopback: только порт, Outbound остаётся TRUE
3. Пакет **ОТПРАВЛЯЕТСЯ** обратно через `WinDivertSend`
4. **TCP-стека Windows** доставляет пакет relay-серверу
5. Клиент получает SYN-ACK от relay → **TCP handshake завершается**
6. Relay принимает connection, смотрит originalDst из таблицы, делает CONNECT к прокси
7. **Bidirectional bridge** между client и proxy

### Как работает версия от 11.06 (НЕПРАВИЛЬНО):
```
CaptureLoop:
  SYN от target → shouldBlock = true → пакет БЛОКИРУЕТСЯ
  RedirectEvent → очередь → ServiceMain.HandleRedirect()
  → ProxySession создаётся, но клиент НЕ получил SYN-ACK
  → У клиента НЕТ сокета → m_localSocket ПУСТОЙ
  → BridgeLoop не может передавать данные
  → На прокси пакетов нет!
```

### Почему в старой версии "обращения шли на прокси, но не было ответов":
- SYN блокировался, клиент не завершал handshake
- **НО если приложение (packet_generator) само создаёт сокет и подключается к 127.0.0.1:relayPort** (как было задумано), то:
  - bridgeLoop между m_localSocket (клиент→relay) и m_proxySocket (relay→proxy) работал
  - Responder не мог отправить ответ обратно через WinDivert, потому что SYN был заблокирован

---

## 🚨 Конкретные ошибки в DST-modification реализации

### 1. Relay сервер слушает на 127.0.0.1 (НЕПРАВИЛЬНО)

**В ProxyBridge:**
```c
addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 0.0.0.0
addr.sin_port = htons(g_local_relay_port);  // 34010
```

**Почему INADDR_ANY?** WinDivert для non-loopback меняет IP местами:
- Original: `192.168.1.5:54321 → 19.10.251.100:9300`
- После модификации: `19.10.251.100:9300 → 192.168.1.5:34010` + Inbound
- Пакет приходит на **192.168.1.5:34010**, а не на 127.0.0.1
- Если слушать только 127.0.0.1 → пакет будет отклонён!

### 2. Неправильный addr.Outbound (КРИТИЧЕСКИ)

**После модификации DST:**
- Non-loopback: SrcAddr и DstAddr нужно поменять местами, `addr.Outbound = FALSE`
- Loopback→loopback: только порт, Outbound = TRUE (как в ProxyBridge)

Без `addr.Outbound = FALSE` пакет не будет доставлен локально!

### 3. Отсутствие пересчёта контрольных сумм

```c
WinDivertHelperCalcChecksums(packet, packet_len, &addr, 0);
WinDivertSend(windivert_handle, packet, packet_len, NULL, &addr);
```

После модификации IP/TCP заголовков **обязательно** пересчитывать чексуммы!
Без этого TCP стек отбрасывает пакет.

### 4. Отсутствие connection_table (хэш src_port → originalDst)

ProxyBridge хранит для каждого src_port:
```c
typedef struct CONNECTION_INFO {
    UINT16 src_port;
    UINT32 src_ip;
    UINT32 orig_dest_ip;
    UINT16 orig_dest_port;
    BOOL   is_tracked;
    UINT32 proxy_config_id;
    CONNECTION_INFO *next;
} CONNECTION_INFO;
```

При первом SYN: `add_connection(src_port, src_ip, orig_dest_ip, orig_dest_port, proxy_config_id)`
При последующих пакетах: `is_connection_tracked(src_port)` → меняет DstPort на relayPort

### 5. Ответные пакеты от relay не восстанавливаются

Когда relay отправляет данные обратно:
- Исходящий пакет от relay: `localhost:34010 → client_ip:client_port`
- Его нужно перехватить и восстановить: `original_dest_ip:original_dest_port → client_ip:client_port`
- И поменять Outbound = FALSE (для non-loopback)

ProxyBridge делает это в `packet_processor`:
```c
if (tcp_header->SrcPort == htons(g_local_relay_port)) {
    // Restore original src port
    UINT16 dst_port = ntohs(tcp_header->DstPort);
    if (get_connection(dst_port, &orig_dest_ip, &orig_dest_port))
        tcp_header->SrcPort = htons(orig_dest_port);
    // Swap IPs for non-loopback
    if (!is_loopback) {
        // SrcAddr ↔ DstAddr
        addr.Outbound = FALSE;
    }
}
```

### 6. WinDivert фильтр (неэффективный)

Текущий: `"true"` — все пакеты. Правильный (как ProxyBridge):
```
(tcp and (outbound or loopback or (tcp.DstPort == 34010 or tcp.SrcPort == 34010))) or ...
```

Это снижает нагрузку на WinDivert — фильтруются только нужные пакеты.

### 7. Отсутствие PID cache и per-port decision bitmap

ProxyBridge кэширует решение на src_port:
```c
if (port_is_decided(sp)) {
    if (port_is_direct(sp)) {
        // fast path — отправить без изменений
        WinDivertSend(...);
        continue;
    }
    // PROXY/BLOCK — уже в connection_table, fall through
}
```

Это даёт огромный прирост производительности: на каждый последующий пакет от того же порта не вызывается `GetExtendedTcpTable` + `OpenProcess` + `QueryFullProcessImageName`.

---

## 📋 План исправления

### Фаза 1: Базовая DST modification (основная функциональность)

1. **Добавить `ConnectionTable`** в инфраструктуру:
   - Хэш-таблица `src_port → {originalDstIP, originalDstPort, proxyConfigId, is_tracked}`
   - Потокобезопасность (SRWLOCK как в ProxyBridge)

2. **Изменить `WinDivertCapture::CaptureLoop`**:
   - Убрать shouldBlock=true
   - На SYN target process: сохранить в ConnectionTable, модифицировать DST
   - На последующие пакеты от tracked src_port: менять DstPort на relayPort
   - На пакеты от relay: восстанавливать DST
   - Пересчитывать контрольные суммы
   - Добавить localhost bypass

3. **Изменить `TcpRelayServer`**:
   - Слушать на INADDR_ANY:relayPort (не 127.0.0.1!)
   - При accept: искать originalDst в ConnectionTable по src_port
   - Создавать подключение к proxy (http CONNECT / SOCKS5)
   - Запускать bidirectional bridge

4. **Обновить WinDivert фильтр** с `"true"` на специфичный

5. **Изменить ServiceMain.h**:
   - Убрать HandleRedirect / GetPendingRedirects / AckRedirect
   - В Run() не поллинг событий, а ожидание завершения (через relay)
   - TcpRelayServer создаётся в Initialize()

### Фаза 2: Оптимизация производительности

1. **Per-port decision bitmap** (port_is_direct / port_is_decided)
2. **PID cache** (src_ip+src_port → pid, TTL 30 сек)
3. **Локальный bridge через два one-way relay** (как в ProxyBridge)

### Фаза 3: IPv6 поддержка

1. IPv6 DST modification (аналогично IPv4)
2. IPv6 relay listener