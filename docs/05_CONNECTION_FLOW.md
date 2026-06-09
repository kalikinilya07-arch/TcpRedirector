# Последовательность прохождения соединения

## Полный поток: от приложения до HTTP Proxy

```
Приложение (chrome.exe)
═══════════════════════════

Шаг 1: Приложение вызывает connect()
──────────────────────────────────────
chrome.exe выполняет:
    socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
    connect(sock, (sockaddr*)&addr, sizeof(addr))
    
    где addr.sin_addr = 142.250.185.46  (google.com)
        addr.sin_port = htons(443)

    ▼
    TCP-стек Windows
    ▼
    WFP ALE Layer: FWPM_LAYER_ALE_AUTH_CONNECT_V4


Шаг 2: WFP вызывает callout драйвера
──────────────────────────────────────
TcpRedirectClassify() вызывается с параметрами:
    inFixedValues[FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS] = 142.250.185.46
    inFixedValues[FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_PORT] = 443 (big-endian)
    inMetaValues->remoteId = 1234  (PID chrome.exe)

    ▼
    Драйвер:
    1. Извлекает PID из inMetaValues->remoteId
    2. Вызывает SeLocateProcessImageName(PID) → получает полный путь
    3. Извлекает имя процесса: chrome.exe
    4. Проверяет кэш правил:
       - chrome.exe → Proxy (match!)
    5. Сохраняет оригинальный адрес в REDIRECT_INFO:
       - redirectId = 1001
       - PID = 1234
       - originalAddressV4 = 142.250.185.46
       - originalPort = 443
       - processPath = "C:\Program Files\Google\Chrome\Application\chrome.exe"
       - timestamp = ...


Шаг 3: Выполнение Connection Redirect
──────────────────────────────────────
Драйвер:
    1. FwpsAcquireClassifyHandle0(...) → получает classifyHandle
    2. FwpsAcquireWritableLayerDataPointer0(...) → получает writableLayerData
    3. Заполняет ALE_CONNECT_REDIRECT:
       - redirectHandle = g_redirectHandle (создан при инициализации)
       - redirectContext = &redirectInfo
       - flags |= FWP_ALE_FLAG_REDIRECT_TCP_CONNECTION
       - redirectPort = 0  (динамический порт)
    4. FwpsApplyModifiedLayerData0(...) → применяет изменения
    5. classsifyOut->actionType = FWP_ACTION_PERMIT
       (разрешаем, но с активным редиректом)


Шаг 4: WFP выполняет редирект
──────────────────────────────
После classify return:
    1. WFP создаёт локальный сокет (127.0.0.1:random_port)
    2. WFP связывает этот сокет с исходным запросом chrome.exe
    3. chrome.exe получает SYN-ACK как от реального сервера
       (на самом деле это локальный сокет)
    4. Срабатывает слой ALE_CONNECT_REDIRECT_V4
       (подтверждение редиректа)


Шаг 5: Драйвер уведомляет сервис
───────────────────────────────────
Драйвер:
    1. Помещает REDIRECT_INFO в очередь (под спин-локом)
    2. KeSetEvent(&g_redirectEvent, IO_NO_INCREMENT, FALSE)
       → событие сигнализировано

    ▼
    (контекст переключается в user mode)


Шаг 6: Сервис получает событие
───────────────────────────────────
Service::DriverCommunicator:
    1. WaitForSingleObject(hEvent, INFINITE) → WAIT_OBJECT_0
    2. DeviceIoControl(hDevice, IOCTL_REDIRECTOR_GET_PENDING, ...)
       → получает REDIRECT_INFO из драйвера
    3. DeviceIoControl(hDevice, IOCTL_REDIRECTOR_ACK_REDIRECT, redirectId, ...)

    Данные редиректа:
    {
        redirectId: 1001,
        PID: 1234,
        processName: "chrome.exe",
        processPath: "C:\Program Files\Google\Chrome\Application\chrome.exe",
        originalIp: 142.250.185.46,
        originalPort: 443,
        redirectLocalPort: 54321  (локальный порт, куда сделан редирект)
    }


Шаг 7: Сервис проверяет правила
───────────────────────────────────
Service::RuleEngine:
    1. ProcessInfo = { pid: 1234, name: "chrome.exe", path: "..." }
    2. Match(processInfo):
       - Rule #1: chrome.exe → Proxy (wildcard match)
       - Action: Proxy
    3. ShouldRedirect → true


Шаг 8: Proxy Engine начинает обработку
────────────────────────────────────────
Service::ProxyEngine:
    1. Создаёт ProxySession
    2. Синтаксический анализ оригинального адреса:
       - Destination: 142.250.185.46:443
       - Reverse DNS lookup (async): google.com
    3. Сохраняет DNS имя (если получено)
    4. Начинает последовательность CONNECT


Шаг 9: Подключение к HTTP Proxy
───────────────────────────────────
ProxySession::ConnectToProxy():
    1. ResolveProxy():
       - Разрешаем адрес прокси (async)
       - proxy.example.com → 10.20.30.40:3128
    
    2. ConnectToProxy(endpoints):
       - TCP connect → proxy.example.com:3128
       - SYN → SYN-ACK → ACK (TCP handshake)
       - Соединение с прокси установлено


Шаг 10: Отправка CONNECT запроса
───────────────────────────────────
ProxySession::SendConnectRequest():
    Отправляет:
    CONNECT 142.250.185.46:443 HTTP/1.1\r\n
    Host: 142.250.185.46:443\r\n
    Proxy-Authorization: Basic base64(login:password)\r\n
    User-Agent: TcpRedirector/1.0\r\n
    \r\n

    (Если DNS дал имя, используем его вместо IP:
     CONNECT google.com:443 HTTP/1.1\r\n)


Шаг 11: Получение ответа прокси
───────────────────────────────────
ProxySession::ReadConnectResponse():
    HTTP/1.1 200 Connection Established\r\n
    \r\n

    HttpConnectParser::Parse():
    - statusCode = 200
    - success = true


Шаг 12: Установка туннеля
───────────────────────────
ProxySession::StartBridging():
    Туннель установлен!
    
    [chrome.exe] ←→ [лок.сокет:54321] ←→ [proxy.socket] ←→ [HTTP Proxy] ←→ [google.com:443]

    State = TunnelEstablished
    Время начала отсчёта статистики
    ConnectionTracker обновлён


Шаг 13: Двусторонняя передача данных
──────────────────────────────────────
ProxySession::StartBridging():
    // Асинхронное чтение с обоих сокетов
    auto self = shared_from_this();
    
    // Read from local (chrome) → write to proxy
    m_localSocket.async_read_some(
        boost::asio::buffer(m_localBuffer),
        [this, self](error_code ec, size_t bytesRead) {
            if (!ec) {
                m_txBytes += bytesRead;
                boost::asio::async_write(
                    m_proxySocket,
                    boost::asio::buffer(m_localBuffer, bytesRead),
                    [this, self](error_code ec, size_t) {
                        if (!ec) ReadFromLocal();  // continue reading
                        else HandleError(ec, "write_to_proxy");
                    });
            } else HandleError(ec, "read_from_local");
        });
    
    // Read from proxy → write to local (chrome)
    m_proxySocket.async_read_some(
        boost::asio::buffer(m_proxyBuffer),
        [this, self](error_code ec, size_t bytesRead) {
            if (!ec) {
                m_rxBytes += bytesRead;
                boost::asio::async_write(
                    m_localSocket,
                    boost::asio::buffer(m_proxyBuffer, bytesRead),
                    [this, self](error_code ec, size_t) {
                        if (!ec) ReadFromProxy();  // continue reading
                        else HandleError(ec, "write_to_local");
                    });
            } else HandleError(ec, "read_from_proxy");
        });


Шаг 14: Закрытие соединения
──────────────────────────────
При закрытии (любой стороной):
    1. error_code = boost::asio::error::eof или connection_reset
    2. HandleError:
       a. Закрываем оба сокета (m_localSocket, m_proxySocket)
       b. Вычисляем длительность: duration = now - m_startTime
       c. Обновляем ConnectionTracker:
          - State = Closed
          - Duration = ...
       d. Удаляем из списка активных соединений в тракере
       e. Лог: DEBUG "Connection closed: chrome.exe(1234) → google.com:443, 45s, RX: 1.2MB, TX: 45KB"
       f. Уведомление GUI через PipeServer::SendConnections()


Шаг 15: GUI обновляет отображение
────────────────────────────────────
GUI::ConnectionsViewModel:
    1. PipeServer шлёт push:connections
    2. IpcClient вызывает OnConnectionsUpdated(list)
    3. ConnectionsViewModel.UpdateConnections(list)
    4. DataGrid обновляется (через ObservableCollection)
    5. Статистика в статус-баре обновляется
```

## Диаграмма последовательности (sequence)

```
chrome.exe    WFP Driver         Service          Proxy Engine      HTTP Proxy
    │             │                 │                  │                │
    │──connect()──►                 │                  │                │
    │             │──ALE classify──►│                  │                │
    │             │◄──redirect──────│                  │                │
    │◄──SYN-ACK───│                 │                  │                │
    │             │──event─────────►│                  │                │
    │             │                 │──get_pending────►│                │
    │             │                 │──ack────────────►│                │
    │             │                 │──check rules────►│                │
    │             │                 │──resolve───────►│                │
    │             │                 │──connect─────────►───────────────►│
    │             │                 │──CONNECT────────►───────────────►│
    │             │                 │◄──200 OK────────◄────────────────│
    │             │                 │──start bridging                 │
    │◄═══data══════════════════════►│◄══data════════►◄══data═════════►│
    │◄═══data══════════════════════►│◄══data════════►◄══data═════════►│
    │             │                 │                  │                │
    │──close()────►│                 │                  │                │
    │             │                 │──close──────────►│                │
    │             │                 │──cleanup────────►│                │
    │             │                 │──push_to_gui────►│                │
```

## Временные характеристики

| Этап | Типичное время | Критично? |
|------|---------------|-----------|
| WFP classify callout | < 1 мкс | Да — в контексте DPC |
| Redirect setup | < 100 мкс | Да |
| Уведомление сервиса | < 1 мс (event) | Нет |
| Обработка в сервисе | < 100 мкс | Нет |
| TCP connect к прокси | 10-100 мс (RTT) | Зависит от сети |
| CONNECT запрос/ответ | 1 RTT + обработка прокси | Зависит от сети |
| Bridging (задержка) | < 1 мс (дополнительно) | Маловажно |

**Ключевой вывод:** callout функция должна быть максимально быстрой. Вся тяжёлая работа (проверка правил, резолвинг DNS) делается в user-mode сервисе. Драйвер только кэширует решение (redirect/direct/block) и выполняет redirect.