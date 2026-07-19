# Stub Kerberos Proxy — заглушка вышестоящего прокси

Самостоятельное тестовое приложение, эмулирующее вышестоящий HTTP(S)-прокси-сервер
с **имитацией Kerberos-аутентификации (SPNEGO/Negotiate)**. Предназначено
исключительно для тестирования основного приложения **TcpRedirector**,
расположенного в родительской папке.

> ⚠️ **Только для тестирования.** Заглушка принимает любые аутентификационные
> данные без реальной валидации и всегда пропускает запрос дальше. Не
> использовать в продакшене.

---

## 1. Назначение

Основное приложение (служба TcpRedirector) выступает **клиентом** вышестоящего
прокси: оно перехватывает TCP-соединения и туннелирует их через HTTP-прокси с
помощью метода `CONNECT`, добавляя заголовок `Proxy-Authorization` в режиме
**Basic** или **Negotiate/Kerberos** (через Windows SSPI).

Эта заглушка играет роль **того самого вышестоящего прокси**. Она:

1. Поднимает HTTP(S)-прокси-сервер и проксирует трафик на целевые адреса,
   извлекаемые из входящих запросов (`CONNECT host:port`, а также обычные
   HTTP-методы с абсолютным URI / заголовком `Host`).
2. Имитирует Kerberos (SPNEGO/Negotiate): принимает **любые** предоставленные
   аутентификационные данные без валидации и всегда пропускает запрос дальше.
   Опционально может отдавать `407 Proxy Authentication Required` с
   `Proxy-Authenticate: Negotiate <challenge>`, чтобы прогнать полный
   SSPI-цикл основного приложения.
3. Логирует данные авторизации (заголовки/токены/креденшелы) и сведения о каждом
   проксируемом запросе (метод, целевой адрес, статус ответа, временная метка).

Заглушка **полностью независима** от кода основного приложения, реализована
только на стандартной библиотеке Python 3 и размещается целиком внутри `/stub`.

### Совместимость с основным приложением

Основное приложение строго проверяет строку статуса ответа на `CONNECT`: она
должна начинаться ровно с `HTTP/1.x 200` (см. `TcpRelayServer.h`). Заглушка
всегда отвечает `HTTP/1.1 200 Connection established`, поэтому совместимость
гарантирована. Для Negotiate дополнительно возвращается финальный псевдо-токен
`Proxy-Authenticate: Negotiate <token>`.

---

## 2. Требования

- **Python 3.7+** (используется только стандартная библиотека).
- Windows / Linux / macOS (кроссплатформенно).
- Для HTTPS-режима (`--tls`) с автогенерацией self-signed сертификата нужен либо
  `openssl` в `PATH`, либо пакет `cryptography`, либо заранее подготовленные
  файлы `--cert`/`--key`. Для обычного HTTP-прокси это не требуется.

---

## 3. Установка и запуск

Установка не требуется — достаточно клонированного репозитория и Python 3.

### Windows

```bat
cd stub
run.bat
```

или напрямую:

```bat
cd stub
py -3 stub_proxy.py --port 8888 --auth-mode accept-any
```

### Linux / macOS

```bash
cd stub
python3 stub_proxy.py --port 8888 --auth-mode accept-any
```

Остановка — `Ctrl+C`.

### Быстрая связка с основным приложением

1. Запустите заглушку, например на `127.0.0.1:8888`.
2. В `config.json` основного приложения укажите:
   ```json
   "proxy": { "host": "127.0.0.1", "port": 8888, "enabled": true },
   "auth":  { "enabled": true, "kerberos": true }
   ```
   (или `"kerberos": false` с `username`/`password` для Basic).
3. Запустите службу — её `CONNECT`-запросы и заголовки авторизации появятся в
   логе заглушки, а трафик будет проксироваться на реальные целевые адреса.

---

## 4. Конфигурационные параметры

Настройки задаются через аргументы CLI и/или JSON-файл (`--config`).
**Приоритет:** значения CLI переопределяют JSON, JSON переопределяет значения по
умолчанию.

| CLI-аргумент | Ключ JSON | По умолчанию | Описание |
|---|---|---|---|
| `--host` | `host` | `127.0.0.1` | Адрес прослушивания (bind). `0.0.0.0` — все интерфейсы |
| `--port` | `port` | `8888` | Порт прокси-сервера |
| `--auth-mode` | `auth_mode` | `accept-any` | Режим имитации авторизации (см. ниже) |
| `--tls` | `tls` | `false` | Включить TLS — прокси работает по HTTPS |
| `--cert` | `cert` | `""` | Путь к TLS-сертификату (PEM) |
| `--key` | `key` | `""` | Путь к приватному ключу (PEM) |
| `--log-file` | `log_file` | `logs/stub_proxy.log` | Путь к файлу лога |
| `--log-format` | `log_format` | `text` | Формат лога: `text` или `json` |
| `--log-max-mb` | `log_max_mb` | `10` | Размер файла лога (МБ) до ротации |
| `--reveal-secrets` | `reveal_secrets` | `false` | Логировать пароли/токены полностью (иначе маскируются) |
| `--connect-timeout` | `connect_timeout` | `10.0` | Таймаут подключения к целевому серверу (сек) |
| `--idle-timeout` | `idle_timeout` | `0.0` | Таймаут простоя туннеля (сек); `0` — без таймаута |
| — | `buffer_size` | `65536` | Размер буфера ретрансляции (только через JSON) |
| `--config` | — | — | Путь к JSON-конфигу |

### Режимы авторизации (`--auth-mode`)

| Режим | Поведение |
|---|---|
| `accept-any` *(по умолчанию)* | Любой `Proxy-Authorization` (или его отсутствие) принимается сразу — немедленный `200`/проксирование. Заголовки авторизации логируются. |
| `challenge` | При первом запросе **без** токена возвращается `407 Proxy Authentication Required` с `Proxy-Authenticate: Negotiate <challenge>`. Клиент повторяет запрос уже с токеном, который принимается без валидации. Позволяет прогнать полный SSPI/Kerberos-цикл основного приложения. |
| `basic-log-only` | Как `accept-any`, но подразумевает акцент на логировании Basic-креденшелов (запрос всегда пропускается). |

> Во всех режимах валидация токенов/паролей **не выполняется** — запрос всегда
> в итоге пропускается дальше.

### Пример JSON-конфига

См. [`config.example.json`](config.example.json). Пути в JSON (например,
`log_file`) резолвятся относительно текущего рабочего каталога запуска.

---

## 5. Примеры использования

Прокси в режиме accept-any на порту 8888:

```bash
python3 stub_proxy.py
```

Прокси на всех интерфейсах, порт 3128, с Kerberos-challenge и JSON-логами:

```bash
python3 stub_proxy.py --host 0.0.0.0 --port 3128 --auth-mode challenge --log-format json
```

Запуск из JSON-конфига с полным раскрытием секретов в логах:

```bash
python3 stub_proxy.py --config config.example.json --reveal-secrets
```

HTTPS-прокси с самоподписанным сертификатом (генерируется автоматически):

```bash
python3 stub_proxy.py --tls --port 8443
```

Ручная проверка через `curl` (HTTP CONNECT через прокси):

```bash
curl -v -x http://127.0.0.1:8888 https://example.com
# с имитацией Basic-креденшелов (принимаются любые):
curl -v -x http://user:pass@127.0.0.1:8888 https://example.com
```

---

## 6. Формат логов

Логи пишутся **одновременно** в консоль (stdout) и в файл (`--log-file`, по
умолчанию `logs/stub_proxy.log`) с простой ротацией по размеру
(`stub_proxy.log` → `stub_proxy.log.1`).

### Текстовый формат (`text`)

```
<ISO-8601 timestamp> [<EVENT>] key1=value1 key2=value2 ...
```

Пример:

```
2026-07-20T02:30:11.482+03:00 [SERVER_START] scheme=http bind=127.0.0.1:8888 auth_mode=accept-any log_format=text log_file=logs/stub_proxy.log
2026-07-20T02:30:15.014+03:00 [AUTH] conn=1 client=127.0.0.1:54233 method=CONNECT target=93.184.216.34:443 auth_type=Negotiate negotiate_token_len=1456 negotiate_token_preview=YIIF... 
2026-07-20T02:30:15.061+03:00 [REQUEST] conn=1 client=127.0.0.1:54233 method=CONNECT target=93.184.216.34:443 auth_type=Negotiate status=200 latency_ms=47.2
2026-07-20T02:30:44.900+03:00 [TUNNEL_CLOSED] conn=1 client=127.0.0.1:54233 target=93.184.216.34:443 bytes_client_to_target=812 bytes_target_to_client=15630
```

### JSON-формат (`json`)

Каждая строка — отдельный JSON-объект:

```json
{"ts": "2026-07-20T02:30:15.014+03:00", "event": "AUTH", "conn": 1, "client": "127.0.0.1:54233", "method": "CONNECT", "target": "93.184.216.34:443", "auth_type": "Negotiate", "negotiate_token_len": 1456, "negotiate_token_preview": "YIIF..."}
{"ts": "2026-07-20T02:30:15.061+03:00", "event": "REQUEST", "conn": 1, "client": "127.0.0.1:54233", "method": "CONNECT", "target": "93.184.216.34:443", "auth_type": "Negotiate", "status": 200, "latency_ms": 47.2}
```

### События

| Событие | Когда | Ключевые поля |
|---|---|---|
| `SERVER_START` | Старт сервера | `scheme`, `bind`, `auth_mode`, `log_format`, `log_file` |
| `SERVER_STOP` | Остановка сервера | — |
| `AUTH` | Получен запрос — **лог данных авторизации** | `conn`, `client`, `method`, `target`, `auth_type`, `auth_raw`, `basic_user`, `basic_password`, `negotiate_token_len`, `negotiate_token_preview` (или полный токен при `--reveal-secrets`) |
| `CHALLENGE_SENT` | Отправлен `407` (режим `challenge`) | `conn`, `client`, `status=407`, `target` |
| `AUTH_RETRY` | Повторный запрос с токеном после `407` | те же, что у `AUTH` |
| `REQUEST` | **Итог проксируемого запроса** | `conn`, `client`, `method`, `target`, `auth_type`, `status`, `latency_ms` |
| `TUNNEL_CLOSED` | Закрыт CONNECT-туннель | `conn`, `target`, `bytes_client_to_target`, `bytes_target_to_client` |
| `UPSTREAM_ERROR` | Не удалось подключиться к целевому серверу | `conn`, `target`, `error` |
| `TLS_HANDSHAKE_ERROR` | Ошибка TLS-рукопожатия | `client`, `error` |
| `HANDLER_ERROR` | Непредвиденная ошибка обработки клиента | `conn`, `client`, `error` |
| `BAD_REQUEST` / `CONN_CLOSED_EARLY` | Некорректный/оборванный запрос | `conn`, `client` |
| `INFO` / `ERROR` | Служебные сообщения | `msg` |

### Логирование авторизации — детали

- **Basic:** декодируется base64, логируется `basic_user`; `basic_password`
  маскируется (`***`) по умолчанию либо выводится полностью при
  `--reveal-secrets`. Сырой заголовок доступен в `auth_raw`.
- **Negotiate/Kerberos:** логируется длина токена (`negotiate_token_len`) и
  превью (`negotiate_token_preview`); полный base64-токен — только при
  `--reveal-secrets`.
- **NTLM:** аналогично Negotiate.
- **Отсутствие авторизации:** `auth_type=none`.

---

## 7. Структура каталога

```
stub/
├── stub_proxy.py         # основной модуль (весь функционал)
├── config.example.json   # пример конфигурации
├── run.bat               # запуск под Windows
├── README.md             # этот файл
└── logs/                 # каталог логов (создаётся автоматически)
    └── stub_proxy.log
```

---

## 8. Ограничения

- Заглушка **не валидирует** аутентификацию — это осознанное поведение для
  тестирования.
- Обычные (не-CONNECT) HTTP-запросы проксируются в упрощённом режиме
  (`Connection: close`, без keep-alive пулинга). Основной сценарий
  TcpRedirector — `CONNECT`-туннели, которые поддерживаются полноценно.
- Self-signed сертификат для `--tls` требует `openssl` или пакет
  `cryptography`; при их отсутствии укажите `--cert`/`--key` вручную.
