# План проверки DST modification

## Сценарий: Chrome через TcpRedirector

### Цель
Проверить, что весь трафик Chrome.exe маршрутизируется через test_proxy (127.0.0.1:3128).

### Шаги

#### 1. Настройка config.json
Файл: `C:\ProgramData\TcpRedirector\config.json`

```json
{
    "capture": {
        "driver": "windivert",
        "mode": "dst_modify"
    },
    "proxy": {
        "host": "127.0.0.1",
        "port": 3128,
        "auth": {
            "enabled": false
        }
    },
    "rules": [
        {
            "action": "redirect",
            "pattern": "",
            "description": ""
        }
    ],
    "process_path": "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    "process_name": "chrome.exe",
    "log": {
        "directory": "C:\\ProgramData\\TcpRedirector\\logs",
        "max_size_mb": 5,
        "max_files": 3
    }
}
```

**Важно:** Закройте Chrome перед тестом, чтобы PID обновился.

#### 2. Запуск test_proxy.py
```cmd
cd C:\Users\user\Desktop\proxy-redirector_old\proxy-redirector\test-proxy
python test_proxy.py
```
Ожидаем: `Test HTTP Proxy running on 127.0.0.1:3128`

#### 3. Запуск TcpRedirector
**От Администратора:**
```cmd
C:\Users\user\Desktop\TcpRedirector\build\TcpRedirectorService.exe --console
```
Ожидаем:
```
[RELAY] Listening on 0.0.0.0:34010
Proxy set from config: 127.0.0.1:3128
Target process: chrome.exe
CaptureLoop started (relayPort=34010, proxy=127.0.0.1:3128)
```

#### 4. Открыть Chrome и зайти на https://google.com
Chrome сделает SYN на google.com:443 → WinDivert перехватывает → DST modify на relay:34010.

Ожидаемые логи в TcpRedirector:
```
[PROXY] #1 srcPort=XXXXX -> relay:34010 (was XXX.XXX.XXX.XXX:443)
[RLY-RSP] #2 restore port=XXXXX
```

Ожидаемые логи в test_proxy.log:
```
CONNECT google.com:443 HTTP/1.1
```

#### 5. Ожидаемый результат
- DNS запросы от Chrome проходят напрямую (UDP, не перехватываются WinDivert)
- TCP SYN на google.com:443 перехватывается и перенаправляется на relay
- Relay делает CONNECT google.com:443 через test_proxy
- test_proxy делает реальное TCP соединение с google.com:443
- Ответ от google.com идёт обратно через test_proxy → relay → Chrome
- **Chrome показывает страницу google.com** — полный цикл работает

### Возможные проблемы

1. **Chrome использует QUIC (UDP)** — WinDivert фильтр ловит только TCP. QUIC UDP пакеты будут проходить напрямую, в обход прокси. Chrome может переключиться на QUIC и H3. Решение: Закрыть Chrome, запустить с флагом `--disable-quic`:
   ```cmd
   "C:\Program Files\Google\Chrome\Application\chrome.exe" --disable-quic
   ```

2. **Chrome использует DNS-over-HTTPS** — не влияет, т.к. DNS-over-HTTPS это HTTPS трафик, который перехватывается.

3. **Proxifier** — если включён, перехватывает трафик до WinDivert. Убедитесь, что он отключён.

### Альтернатива: packet_generator + mock_proxy

Если Chrome тест неудобен, можно использовать готовые заглушки:

**Терминал 1 (Таргет-заглушка):**
```cmd
C:\Users\user\Desktop\TcpRedirector\build\tests\Debug\mock_proxy.exe 8888
```
Слушает на 0.0.0.0:8888, принимает HTTP CONNECT, отвечает.

**Терминал 2 (test_proxy):**
```cmd
python C:\Users\user\Desktop\proxy-redirector_old\proxy-redirector\test-proxy\test_proxy.py
```

**Терминал 3 (TcpRedirector - админ):**
```cmd
C:\Users\user\Desktop\TcpRedirector\build\TcpRedirectorService.exe --console
```
Правило: `packet_generator.exe` → PROXY

**Терминал 4 (packet_generator):**
```cmd
C:\Users\user\Desktop\TcpRedirector\build\tests\Debug\packet_generator.exe 127.0.0.1 8888
```
Шлёт SYN на 127.0.0.1:8888 → DST modify на relay:34010 → relay делает CONNECT 127.0.0.1:8888 через test_proxy → test_proxy CONNECT → mock_proxy отвечает → ответ идёт обратно.

**Весь цикл:** generator → WinDivert → relay → test_proxy → mock_proxy → ответ обратно. ✅
