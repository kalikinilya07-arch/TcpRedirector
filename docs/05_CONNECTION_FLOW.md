# Последовательность прохождения соединения (DST modification)

**Разработчик:** Kalikin Iliya

## Полный поток: от приложения до HTTP Proxy

```
Приложение (chrome.exe)
═══════════════════════════

Шаг 1: Приложение вызывает connect()
────────────────────────────────────────
chrome.exe выполняет:
    socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
    connect(sock, (sockaddr*)&addr, sizeof(addr))
    
    где addr.sin_addr = 142.250.185.46  (google.com)
        addr.sin_port = htons(443)

    ▼
    SYN-пакет отправляется в сеть


Шаг 2: WinDivert перехватывает SYN
────────────────────────────────────
WinDivertCapture::CaptureLoop() (в потоке CaptureLoop):
    WinDivertRecv(packet, 0xFFFF, &recvLen, &addr)
    HelperParsePacket(packet, ...) → IP/TCP заголовки

    Определяем, что это SYN: tcpHdr->SYN и !tcpHdr->ACK
    
    Данные пакета:
    - src_ip: 192.168.1.100 (ipHdr->SrcAddr)
    - src_port: 49152 (tcpHdr->SrcPort)
    - dst_ip: 142.250.185.46 (ipHdr->DstAddr)
    - dst_port: 443 (tcpHdr->DstPort)


Шаг 3: Определение PID и проверка правил
──────────────────────────────────────────
WinDivertCapture:
    1. FindPidBySourcePort(src_port=49152):
       - GetExtendedTcpTable(...) → ищет LocalPort=49152
       - Находит: PID=1234, process=chrome.exe
       - Возвращает PID и полный путь процесса
    
    2. Если PID уже закэширован в bitmap → пропуск шага 1
    
    3. CheckProcessRule(pid=1234, procPath="...chrome.exe"):
       - Проверяет по битмапу: порт уже известен?
       - Если нет → вызывает RuleEngine:
         Rule #1: chrome.exe → Proxy (match!)
         Action: Redirect to proxy

    4. Сохраняет в ConnectionTable:
       srcPort=49152 → { pid=1234, path="...", bytes_up=0, bytes_down=0 }


Шаг 4: DST modification
────────────────────────
WinDivertCapture модифицирует SYN-пакет:
    // Сохраняем оригинальные адрес и порт
    original_dst_ip   = ipHdr->DstAddr    // 142.250.185.46
    original_dst_port = tcpHdr->DstPort   // 443

    // Меняем DST на relay-сервер
    ipHdr->DstAddr    = 127.0.0.1
    tcpHdr->DstPort   = relayPort (34010)

    // Пересчитываем контрольные суммы
    tcpHdr->Checksum  = 0  (WinDivert пересчитает)
    ipHdr->Checksum   = 0  (WinDivert пересчитает)

    WinDivertSend(m_handle, packet, ...)  ← изменённый SYN отправлен


Шаг 5: Приложение получает SYN-ACK
────────────────────────────────────
chrome.exe получает SYN-ACK от "сервера" (на самом деле от relay):
    SYN-ACK от 127.0.0.1:34010
    
    chrome.exe отправляет ACK (TCP handshake завершён)
    ▼
    TCP-соединение установлено с локальным relay-сервером


Шаг 6: TcpRelayServer принимает соединение
───────────────────────────────────────────
TcpRelayServer::ConnectionHandler (в отдельном потоке):

    1. accept() → новый клиентский сокет
    2. ConnectionTable::Lookup(srcPort=49152):
       - Находит: pid=1234, path="...chrome.exe"
       - Возвращает оригинальный dst: 142.250.185.46:443
    
    3. Получает данные для CONNECT:
       - Original host: 142.250.185.46
       - Original port: 443
       - PID: 1234, process: chrome.exe


Шаг 7: Подключение к HTTP Proxy
────────────────────────────────
TcpRelayServer::ConnectionHandler:

    1. Создаёт сокет для прокси:
       socket(AF_INET, SOCK_STREAM, 0)
    
    2. resolve proxy_host:
       gethostbyname("proxy.example.com") или 127.0.0.1
    
    3. connect(proxy_socket, proxy_addr, proxy_port):
       TCP → proxy.example.com:3128
       SYN → SYN-ACK → ACK (TCP handshake с прокси)


Шаг 8: Отправка CONNECT запроса
────────────────────────────────
TcpRelayServer::ConnectionHandler:

    Отправляет CONNECT запрос к прокси:
    
    CONNECT 142.250.185.46:443 HTTP/1.1\r\n
    Host: 142.250.185.46:443\r\n
    
    ─── Если Basic auth: ──────────────────────
    Proxy-Authorization: Basic base64(login:password)\r\n
    
    ─── Если Kerberos/Negotiate auth: ─────────
    // SSPI: AcquireCredentialsHandle("Negotiate")
    //       InitializeSecurityContext() → token
    Proxy-Authorization: Negotiate <base64_token>\r\n
    
    ───────────────────────────────────────────
    \r\n


Шаг 9: Обработка ответа прокси
───────────────────────────────
TcpRelayServer::ConnectionHandler:

    Читает ответ прокси:
    
    HTTP/1.1 200 Connection Established\r\n
    \r\n

    ─── Если 407 с Negotiate challenge: ──────
    HTTP/1.1 407 Proxy Auth Required\r\n
    Proxy-Authenticate: Negotiate <challenge>\r\n
    \r\n
    
    // SSPI: Parse407Challenge(challenge)
    //       SspiNegotiate(challenge) → новый токен
    //       goto Шаг 8 → повторный CONNECT с новым токеном
    //       → 200 OK → tunnel established
    ────────────────────────────────────────────


Шаг 10: Установка туннеля
───────────────────────────
Туннель установлен!

    [chrome.exe] ←→ [127.0.0.1:34010] ←→ [relay:proxy_socket] ←→ [HTTP Proxy] ←→ [google.com:443]

    Relay логирует:
    [RELAY] [PROXIED] #1 chrome.exe(49152) → 142.250.185.46:443 → 127.0.0.1:3128


Шаг 11: Двусторонняя передача данных
──────────────────────────────────────
TcpRelayServer::ConnectionHandler:

    // Bridge loop — select() на обоих сокетах
    while (running && !error) {
        select(client_socket, proxy_socket, ...)
        
        if (client_socket readable) {
            recv(client_socket, buf, size)
            send(proxy_socket, buf, bytes_read)
            ConnectionTable::AddBytes(up=bytes_read)
        }
        
        if (proxy_socket readable) {
            recv(proxy_socket, buf, size)
            send(client_socket, buf, bytes_read)
            ConnectionTable::AddBytes(down=bytes_read)
        }
    }


Шаг 12: Закрытие соединения
─────────────────────────────
При закрытии (FIN или RST от любой стороны):

    1. ConnectionHandler обнаруживает закрытие:
       - recv возвращает 0 (FIN) или ошибку (RST)
    
    2. Логирует статистику соединения:
       [RELAY] [PROXIED] closed: up=X bytes down=Y bytes total=Z bytes
    
    3. ConnectionTable::Remove(srcPort):
       - Удаляет запись из таблицы
       - Очищает битмап для порта
    
    4. Закрывает оба сокета:
       closesocket(client_socket)
       closesocket(proxy_socket)


Шаг 13: GUI обновляет отображение
───────────────────────────────────
GUI (таймер 2 секунды):
    IPC: get_connections → список активных соединений
    IPC: get_stats → статистика (всего пакетов, байт)
    
    DataGrid обновляется через ObservableCollection
```

## Диаграмма последовательности

```
chrome.exe    WinDivertCapture    TcpRelayServer      HTTP Proxy
    │               │                  │                  │
    │──SYN──────────►                  │                  │
    │  (google:443) │                  │                  │
    │               │──PID lookup      │                  │
    │               │──Check rules     │                  │
    │               │──DST modify      │                  │
    │               │──Send (to relay) │                  │
    │◄──SYN-ACK─────│                  │                  │
    │──ACK──────────►                  │                  │
    │               │                  │                  │
    │               │                  │──accept────────  │
    │               │                  │──table.Lookup──  │
    │               │                  │──connect────────►│
    │               │                  │──CONNECT────────►│
    │               │                  │◄──200 OK─────────│
    │               │                  │                  │
    │◄══data══════════════════════════►│══data══════════►│
    │══data═══════════════════════════►│◄═data═══════════│
    │               │                  │                  │
    │──close()──────►                  │                  │
    │               │                  │──close──────────►│
    │               │                  │──log stats─────  │
```

## Временные характеристики

| Этап | Типичное время | Критично? |
|------|---------------|-----------|
| WinDivert Recv | < 10 мкс | Нет |
| PID lookup через TCP table | 100-500 мкс | Нет |
| DST modification | < 1 мкс | Нет |
| WinDivert Send | < 10 мкс | Нет |
| TCP connect к прокси | 10-100 мс (RTT) | Зависит от сети |
| CONNECT запрос/ответ | 1 RTT + обработка прокси | Зависит от сети |
| SSPI handshake (Kerberos) | +1-2 RTT + 50-200 мс | Дополнительно к CONNECT |
| Bridging (задержка) | < 1 мс (дополнительно) | Маловажно |

**Ключевой вывод:** Вся обработка (PID lookup, проверка правил, DST modification) происходит в user-mode. WinDivert работает в kernel-mode только для перехвата и модификации пакетов, но callback-функции вызываются в контексте пользовательского потока, что исключает проблемы DPC.