# Анализ архитектуры перехвата и перенаправления TcpRedirector

## 1. Инициализация WinDivert

**Цепочка вызовов:**

| Шаг | Файл | Строка | Что происходит |
|-----|------|--------|----------------|
| 1 | `ServiceMain.h` | `m_capture = make_unique<WinDivertCapture>()` | Создание объекта |
| 2 | `ServiceMain.h` | `m_capture->Open()` | Запуск захвата |
| 3 | `WinDivertCapture.cpp` | `WinDivertCapture::Open()` | **Основная инициализация** |
| 3a | `WinDivertCapture.cpp` | `LoadWinDivertApi()` → `m_api.Load()` | Загрузка WinDivert.dll |
| 3b | `WinDivertCapture.cpp` | `WinDivertApi::Load()` | Динамическая загрузка 9 функций через `LoadLibraryW` + `GetProcAddress` |
| 3c | `WinDivertCapture.cpp` | `m_api.Open("true", WINDIVERT_LAYER_NETWORK, 0, 0)` | Открытие handle, фильтр "true" (все пакеты) |
| 3d | `WinDivertCapture.cpp` | `SetParam(QUEUE_LENGTH=16384, QUEUE_TIME=2000, QUEUE_SIZE=33553920)` | Настройка очереди |
| 3e | `WinDivertCapture.cpp` | Запуск `CaptureLoop()` | Старт потока захвата |

**Критические функции (НЕ ТРОГАТЬ):**
- `WinDivertApi::Load()` — динамическая загрузка DLL
- `WinDivertCapture::Open()` — открытие handle + настройка очереди

---

## 2. Главный цикл перехвата (CaptureLoop)

**Файл:** `WinDivertCapture.cpp`

```
while (m_running) {
    m_api.Recv(m_handle, packet, 0xFFFF, &recvLen, &addr)   // ← WinDivertRecv
    HelperParsePacket(packet, ...)                             // ← парсинг IP/TCP
    if (SYN && IsTargetProcess(pid)) → создать RedirectEvent   // ← фильтрация
    if (!shouldBlock) m_api.Send(m_handle, packet, ...)        // ← WinDivertSend
}
```

**Ключевые строки:**

| Файл | Строка | Вызов | Назначение |
|------|--------|-------|------------|
| `WinDivertCapture.cpp` | `Recv` | `m_api.Recv(...)` | **WinDivertRecv** — захват пакета |
| `WinDivertCapture.cpp` | `HelperParsePacket` | `HelperParsePacket(...)` | Разбор IP/TCP заголовков |
| `WinDivertCapture.cpp` | `FindPidBySourcePort` | TCP table lookup | Определение PID по src_port |
| `WinDivertCapture.cpp` | `IsTargetProcess` | Сравнение полного пути | Проверка, принадлежит ли PID целевому процессу |
| `WinDivertCapture.cpp` | `создание RedirectEvent` | `SetEvent(m_hEvent)` | Генерация события редиректа |
| `WinDivertCapture.cpp` | `Send` | `m_api.Send(...)` | **WinDivertSend** — пропуск пакета |

**Критические функции (НЕ ТРОГАТЬ):**
- `CaptureLoop()` — **весь цикл**, сердце программы
- `FindPidBySourcePort()` — маппинг порт→PID через TCP table
- `IsTargetProcess()` — проверка PID по полному пути процесса
- `FindTargetPid()` — поиск PID по имени (Toolhelp32Snapshot)

---

## 3. Логика перенаправления на HTTP прокси

**Цепочка вызовов:**

| Шаг | Файл | Строка | Что происходит |
|-----|------|--------|----------------|
| 1 | `ServiceMain.h` | `GetPendingRedirects(100)` | Polling очереди редиректов |
| 2 | `ServiceMain.h` | `HandleRedirect()` | Проверка правил + создание сессии |
| 3 | `ServiceMain.h` | `ShouldRedirect(process_name, path)` | Проверка, нужно ли редиректить |
| 4 | `ServiceMain.h` | `CreateSession(redirect, callback)` | Создание прокси-сессии |
| 5 | `ProxyEngine.h` | `ProxyEngine::CreateSession()` | Запуск `Start()` в фоновом потоке |
| 6 | `ProxyEngine.h` | `ProxySession::Start()` → `ConnectToProxy()` | Подключение к прокси |
| 7 | `ProxyEngine.h` | `ConnectToProxy()` | socket → gethostbyname → connect |
| 8 | `ProxyEngine.h` | `SendConnectRequest()` | Отправка `CONNECT host:port HTTP/1.1` |
| 9 | `ProxyEngine.h` | `ReadConnectResponse()` | Ожидание `HTTP/1.1 200 Connection Established` |
| 10 | `ProxyEngine.h` | Запуск `BridgeLoop()` | Bidirectional bridge |

**BridgeLoop (`ProxyEngine.h`):**
```
while (TunnelEstablished) {
    select() на m_localSocket + m_proxySocket
    recv(m_localSocket) → send(m_proxySocket)
    recv(m_proxySocket) → send(m_localSocket)
}
```

**Критические функции (НЕ ТРОГАТЬ):**
- `HandleRedirect()` — оркестратор редиректа
- `ConnectToProxy()` — CONNECT-рукопожатие
- `BridgeLoop()` — bidirectional forwarding

---

## 4. Жёстко зашитые (hardcoded) параметры

### 4a. Путь к целевому EXE (target process)

| Файл | Строка | Значение |
|------|--------|----------|
| `WinDivertCapture.h` | `m_targetProcessPath` | `C:\Projects\china\police_sec\TransfersClient.exe` |

Используется в:
- `FindTargetPid()` — сравнение `_wcsicmp(path, m_targetProcessPath)`
- `IsTargetProcess()` — то же сравнение

### 4b. Адрес и порт прокси

| Файл | Строка | Значение |
|------|--------|----------|
| `ServiceMain.h` | `host = L"127.0.0.1"` | `port = 8888` |

Перезаписывает config.json при каждом старте. Используется в:
- `m_proxyEngine->Initialize(m_configManager->GetProxyConfig())`
- `gethostbyname(proxy_host)` + `connect()`

### 4c. Правило фильтрации

| Файл | Строка | Значение |
|------|--------|----------|
| `ServiceMain.h` | `pattern = L"packet_generator.exe"` | `type = ProcessName` |

Оригинал: `TransfersClient.exe` с `type = ProcessPath`.

---

## 5. Сводка: что НЕЛЬЗЯ МЕНЯТЬ

### CORE CAPTURE (НЕ ТРОГАТЬ)
| Функция | Файл | Причина |
|---------|------|---------|
| `WinDivertCapture::Open()` | `WinDivertCapture.cpp:69` | Инициализация WinDivert, загрузка DLL, настройка очереди |
| `WinDivertCapture::CaptureLoop()` | `WinDivertCapture.cpp:130` | Главный цикл Recv/Send — сердце программы |
| `WinDivertCapture::FindPidBySourcePort()` | `WinDivertCapture.cpp:261` | Маппинг порт→PID через таблицу TCP |
| `WinDivertCapture::IsTargetProcess()` | `WinDivertCapture.cpp:305` | Валидация PID по полному пути процесса |
| `WinDivertCapture::FindTargetPid()` | `WinDivertCapture.cpp:282` | Поиск PID процесса по имени |
| `WinDivertApi::Load()` | `WinDivertCapture.cpp:28` | Динамическая загрузка WinDivert.dll |

### PROXY ENGINE (НЕ ТРОГАТЬ)
| Функция | Файл | Причина |
|---------|------|---------|
| `ProxySession::ConnectToProxy()` | `ProxyEngine.h:84` | CONNECT-рукопожатие с HTTP-прокси |
| `ProxySession::SendConnectRequest()` | `ProxyEngine.h:128` | Отправка CONNECT запроса |
| `ProxySession::ReadConnectResponse()` | `ProxyEngine.h:160` | Чтение ответа 200 |
| `ProxySession::BridgeLoop()` | `ProxyEngine.h:188` | Bidirectional data forwarding |

### МОЖНО МЕНЯТЬ (HARDCODED CONFIG)
| Параметр | Файл | Строка |
|----------|------|--------|
| `m_targetProcessPath` | `WinDivertCapture.h:85` | Путь к целевому EXE |
| `host` / `port` прокси | `ServiceMain.h:48-49` | Адрес и порт HTTP-прокси |
| Правило фильтрации | `ServiceMain.h:60-72` | Какое перехватывать |
