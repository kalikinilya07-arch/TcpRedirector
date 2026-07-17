# Архитектурный аудит цепочки Kerberos-аутентификации

## 1. Общая архитектура

```
┌─────────┐    ┌──────────────┐    ┌─────────────┐    ┌──────────────────┐
│ Chrome  │───▶│ WinDivert    │───▶│ TcpRelay    │───▶│ test_proxy_      │
│         │    │ Capture      │    │ Server.h    │    │ negotiate.py     │
└─────────┘    └──────────────┘    └──────┬──────┘    └────────┬─────────┘
                                          │                     │
                                          │ Named Pipe          │ SSPI
                                          ▼                     ▼
                                   ┌──────────────────┐  ┌───────────┐
                                   │ KerberosAgent    │  │ Windows   │
                                   │ Provider.cpp     │  │ SSPI      │
                                   └────────┬─────────┘  │ (Kerberos)│
                                            │             └───────────┘
                                            │ JSON-RPC
                                            ▼
                                   ┌──────────────────┐
                                   │ AuthAgent.exe    │
                                   │ (main.cpp)       │
                                   │ SspiEngine.cpp   │
                                   └──────────────────┘
```

## 2. Полный поток аутентификации (3-leg SPNEGO)

```
Client (TcpRelayServer)                    Proxy (test_proxy_negotiate.py)
───────────────────────                    ──────────────────────────────

CONNECT host:443 HTTP/1.1
(no auth header)
                                          → 407 Proxy Auth Required
                                            Proxy-Authenticate: Negotiate
                                            
[!authContextActive branch - NEW]
CreateContext("HTTP/127.0.0.1")
  → InitializeSecurityContext(NULL)
  ← initial SPNEGO token (base64)

CONNECT host:443 HTTP/1.1
Proxy-Authorization: Negotiate <token1>
                                          → accept_token(token1)
                                          ← 407 Proxy Auth Required
                                            Proxy-Authenticate: Negotiate <challenge>

[authContextActive && KerberosAgent]
Parse407Challenge(resp) → challenge
ContinueContext(id, challenge)
  → InitializeSecurityContext(challenge)
  ← response SPNEGO token (base64)

CONNECT host:443 HTTP/1.1
Proxy-Authorization: Negotiate <token2>
                                          → accept_token(token2) → OK
                                          ← 200 Connection Established

DATA RELAY ◀══════════════════════════════════════════════════▶ DATA RELAY
```

## 3. Оценка внесённых исправлений

### 3.1. TcpRelayServer.h: 407-обработчик для `!authContextActive`

**Файл**: [`TcpRelayServer.h:484-512`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:484)

**Статус**: ✅ КОРРЕКТНО

**Обоснование**: Когда превентивная аутентификация проваливается (AuthAgent не готов), `CreateContext()` возвращает `success=false`, `authContextActive` остаётся `false`. CONNECT уходит без авторизации. Прокси отвечает 407 с `Proxy-Authenticate: Negotiate` (без токена — только схема). Новая ветка ловит этот 407, вызывает `CreateContext(spn)` повторно (к этому моменту AuthAgent уже может быть готов), получает начальный SPNEGO-токен и повторяет CONNECT. Это правильное начало 3-leg handshake.

**Проверка совместимости с test_proxy_negotiate.py**: 
- Прокси при получении CONNECT без токена отправляет `_send_407(server_token=None)` — только схема Negotiate без токена
- `CreateContext(spn)` генерирует начальный токен через `InitializeSecurityContext(NULL)` — это правильный первый шаг SPNEGO

### 3.2. KerberosAgentProvider.cpp: `EnsureConnected()` 

**Файл**: [`KerberosAgentProvider.cpp:239-258`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:239)

**Статус**: ✅ КОРРЕКТНО

**Обоснование**: 
- Ранний возврат если уже подключены (`m_connected`) — правильно
- Попытка быстрого подключения без запуска агента — правильно
- Запуск AuthAgent + опрос 20×500ms = 10 секунд — достаточно для инициализации пайпа
- `ConnectToAgent()` внутри цикла корректно обрабатывает как отсутствие пайпа, так и занятость

### 3.3. KerberosAgentProvider.cpp: `LaunchAuthAgentInUserSession()` cooldown

**Файл**: [`KerberosAgentProvider.cpp:264-273`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:264)

**Статус**: ✅ КОРРЕКТНО

**Обоснование**: 
- 10-секундный冷却 предотвращает спам запусков (было 4-5 в секунду)
- `s_launchInProgress` защищает от параллельных запусков
- Двойная защита (cooldown + atomic flag) покрывает все сценарии

### 3.4. KerberosAgentProvider.cpp: `KeepaliveLoop()`

**Файл**: [`KerberosAgentProvider.cpp:207-237`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:207)

**Статус**: ✅ КОРРЕКТНО

**Обоснование**: Тот же паттерн опроса что в `EnsureConnected()`. Корректно переподключается при обрыве связи.

## 4. Выявленные проблемы (не внесённые нашими изменениями)

### 4.1. 🔴 Deadlock-риск: `ReadMessage()` → `DisconnectFromAgent()` 

**Файл**: [`KerberosAgentProvider.cpp:395-407`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:395)

```cpp
std::string KerberosAgentProvider::ReadMessage(HANDLE pipe) {
    // ...
    if (!ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            DisconnectFromAgent();  // ← берёт m_pipeMutex (строка 199)
        }
        throw std::runtime_error(...);
    }
}
```

`DisconnectFromAgent()` делает `std::lock_guard<std::mutex> lock(m_pipeMutex)`. Если `ReadMessage()` вызван из `ConnectToAgent()` (который уже держит `m_pipeMutex` на [строке 136](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:136)), происходит **deadlock** (Windows `std::mutex` не рекурсивный).

**Сценарий**: `ConnectToAgent()` → `Call("hello")` → `ReadMessage()` → ошибка `ERROR_BROKEN_PIPE` → `DisconnectFromAgent()` → `lock(m_pipeMutex)` → **DEADLOCK**

**Серьёзность**: Низкая на практике (ошибка `ERROR_BROKEN_PIPE` при version negotiation маловероятна), но баг присутствует.

### 4.2. 🟡 `Call()` использует `m_pipe` без мьютекса

**Файл**: [`KerberosAgentProvider.cpp:364-381`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:364)

Методы `CreateContext`, `ContinueContext`, `CloseContext` вызывают `EnsureConnected()` (без мьютекса), затем `Call()` (без мьютекса). Параллельный вызов `DisconnectFromAgent()` из keepalive-потока может закрыть `m_pipe` между `EnsureConnected()` и `Call()`.

**Серьёзность**: Средняя. Может привести к крашу при интенсивной нагрузке.

### 4.3. 🟡 `ConnectToAgent()` блокируется до 50 секунд

**Файл**: [`KerberosAgentProvider.cpp:142-153`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:142)

Внутренний цикл: 10 попыток × `kPipeTimeout` (5000ms) = до 50 секунд если пайп существует но все экземпляры заняты. В комбинации с внешним циклом `EnsureConnected()` (20×500ms) это даёт непредсказуемые таймауты.

**Серьёзность**: Низкая для одного клиента (пайп не будет занят), но может проявиться при множественных подключениях.

### 4.4. 🟢 Дублирование SSPI-логики

В проекте две реализации SSPI:
- [`auth_sspi.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp) — в процессе сервиса (SYSTEM)
- [`SspiEngine.cpp`](TcpRedirector/src/auth-agent/TcpRedirectorAuthAgent/SspiEngine.cpp) — в AuthAgent (пользовательская сессия)

Фактически используется только `SspiEngine.cpp` (через AuthAgent). `auth_sspi.cpp` — мёртвый код? Но `Parse407Challenge` из `auth_sspi.cpp` используется в `TcpRelayServer.h:441`.

## 5. Итоговая оценка

| Компонент | Статус | Комментарий |
|-----------|--------|-------------|
| 407 handler (`!authContextActive`) | ✅ Корректно | Правильно начинает SPNEGO handshake |
| 407 handler (`authContextActive`) | ✅ Корректно | Правильно продолжает SPNEGO |
| `EnsureConnected()` polling | ✅ Корректно | 10 сек достаточно для инициализации |
| `LaunchAuthAgent` cooldown | ✅ Корректно | Предотвращает спам запусков |
| `KeepaliveLoop()` polling | ✅ Корректно | Правильное переподключение |
| SPNEGO flow (SspiEngine) | ✅ Корректно | Правильные флаги, обработка ошибок |
| AuthAgent JSON-RPC | ✅ Корректно | Правильная обработка всех методов |
| `Parse407Challenge` | ✅ Корректно | 4 варианта регистра, обрезка пробелов |
| Deadlock `ReadMessage→Disconnect` | 🔴 Pre-existing | Не введено нами, низкий риск |
| `Call()` без мьютекса | 🟡 Pre-existing | Не введено нами, средний риск |
| `ConnectToAgent()` таймауты | 🟡 Pre-existing | Не введено нами, низкий риск |

## 6. Достаточность исправлений

**Исправления достаточны для успешной Kerberos-аутентификации.** Ключевая проблема (невозможность подключиться к AuthAgent после запуска) решена. Поток SPNEGO корректен на всех трёх шагах.

Оставшиеся проблемы (deadlock, отсутствие мьютекса в `Call()`) являются pre-existing и не блокируют работу Kerberos в нормальных условиях. Рекомендуется исправить их отдельно, но не в рамках текущего цикла.
