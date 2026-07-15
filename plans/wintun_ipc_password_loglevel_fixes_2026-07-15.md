# План: Wintun-форвардинг, IPC-auth toggle, plaintext-пароль, лог-уровень (2026-07-15)

Дата: 15.07.2026
Область: служба `TcpRedirectorService` (C++), опционально GUI (C#).
Вход: `output/config.json` (`capture_mode=wintun`, `engine=embedded`, `log.level=3`),
`output/tcp_redirector.log` (несколько прогонов в разных режимах).

---

## 0. Ключевой вывод анализа (важно для порядка работ)

Все три запроса пользователя связаны, и **корневая причина №3 блокирует диагностику №1**:

- В конфиге стоит `log.level=3` (DEBUG в файловой шкале), но в логе **нет ни одной
  DEBUG/TRACE-строки** — только INFO/WARN/ERROR.
- Причина: [`ServiceMain::Initialize()`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h:74)
  инициализирует логгер **жёстко** уровнем `domain::LogLevel::Info` (строка 77) и
  после `ConfigManager::Load()` **никогда не вызывает** `m_logger->SetLevel(...)`.
  Применяется только `SetMaxFileSizeMB` (строка 108). То есть уровень из
  `config.json` полностью игнорируется — логгер навсегда остаётся на INFO.
- Следствие: вся диагностика Wintun-движка (rx-stats, PROXY-flow, DIRECT-drop,
  connect-fail — см. §T5 в [`ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md`](../docs/ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md))
  пишется на DEBUG/TRACE и **невидима**. Поэтому «в debug не видно доп. инфо»
  (запрос №3) и «не можем понять, почему трафик не выходит» (запрос №1) — это
  **один и тот же корень**.

**Порядок:** сначала №3 (лог-уровень) — он «включает свет», затем №1/№2 (toggles),
затем прогон диагностики №1 на стенде уже с видимыми DEBUG/TRACE.

---

## 1. Запрос №3 — лог-уровень не влияет (ROOT CAUSE найден)

### 1.1. Факты
- [`ConfigManager::GetLogLevel()`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:181)
  корректно маппит `0→Error,1→Warn,2→Info,3→Debug`.
- [`Logger::Log()`](../src/service/TcpRedirectorService/infrastructure/logging/Logger.cpp:76)
  фильтрует `if (level < m_currentLevel) return;` в доменной шкале
  `Trace=0 < Debug=1 < Info=2 < Warn=3 < Error=4`.
- `ServiceMain` **не переносит** значение из конфига в логгер.

### 1.2. Исправление (обязательное)
В [`ServiceMain::Initialize()`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h:105)
сразу после блока `SetMaxFileSizeMB` добавить:
```cpp
m_logger->SetLevel(m_configManager->GetLogLevel());
m_logger->Info("service", "Log level applied from config: " +
    std::to_string(m_configManager->GetConfig().log.level));
```
Одна строка логики, риск минимальный. Живой `set_log_level` через IPC уже работает
([`IpcHandler::SetLogLevel`](../src/service/TcpRedirectorService/adapters/driving/IpcHandler.h:258)),
проблема только в старте.

### 1.3. Проблема шкалы TRACE (нужно для диагностики №1)
Важный нюанс: rx-stats, DIRECT-drop и PROXY-трейс в Wintun-движке пишутся на
**`LogLevel::Trace`**, а `Trace < Debug`. Значит даже с `log.level=3` (=Debug) они
будут отфильтрованы. Файловая шкала (`0..3`) **не умеет выражать TRACE**.

Варианты (выбрать при реализации, рекомендация — B):
- **(A)** Понизить самые важные диагностические строки движка (`[rx-stats]`,
  `[DIRECT-drop]`) с `Trace` на `Debug`, чтобы они были видны при `level=3`.
- **(B, рекомендуется)** Расширить файловую шкалу: добавить `level=4 → Trace`
  (экспертный уровень). `GetLogLevel()`: `case 4: return Trace;`.
  `SetLogLevel()`: `Trace → 4`. GUI-шкалу `0..3` не трогаем (обратная
  совместимость); значение `4` доступно ручной правкой конфига для полной
  трассировки. Документировать в [`КОНФИГУРАЦИЯ.md`](../docs/КОНФИГУРАЦИЯ.md) §8.
- Можно совместить: A (несколько ключевых строк на Debug) + B (экспертный TRACE).

**Файлы:** `ServiceMain.h`, `ConfigManager.cpp` (`GetLogLevel`/`SetLogLevel`),
опц. `Tun2SocksEngineEmbedded.cpp` (уровни строк), `docs/КОНФИГУРАЦИЯ.md`.

---

## 2. Запрос №1 — Wintun: трафик входит в TUN, но не уходит на прокси

### 2.1. Что видно из лога (без DEBUG — ограниченно)
- `embedded, process_filter DISABLED` (Option 2b, весь TCP → прокси) — стартует
  штатно, но **нет** relay-строк `CONNECT ... 200 OK` и нет DEBUG-строк PROXY-flow
  (их не видно из-за бага №3).
- `external` (tun2socks.exe): процесс **падает сразу** (`exited code=1`),
  супервизор перезапускает 6 раз и сдаётся (`restart rate limit reached`).
  Внешний движок сломан отдельно (см. §2.4).

### 2.2. Запрошенный toggle: отключить IPC auth token
Пользователь просит попробовать «disable IPC auth token usage, allow all without
it and without privileges». Технически IPC-канал (GUI↔служба) **не влияет** на
форвардинг трафика — это канал управления. Но реализуем toggle как запрошенный
эксперимент и для упрощения подключения GUI:

- **Config.h:** новая секция/поле, напр. `struct IpcSettings { bool auth_enabled = true; };`
  (или `auth.ipc_auth_enabled`). Дефолт `true` (текущее поведение).
- **ConfigManager.cpp:** парсинг/сериализация `ipc.auth_enabled`.
- **ServiceMain.h:** вызывать
  [`SetAuthTokenFilePath`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h:330)
  **только** при `auth_enabled=true`. При `false` — не задавать путь токена;
  `TcpIpcServer` тогда оставляет `m_authToken` пустым, и проверка в
  [`ServerThread`](../src/service/TcpRedirectorService/infrastructure/ipc/TcpIpcServer.h:269)
  (`if (!m_authToken.empty() && ...)`) **не применяется** — сервер принимает всех.
  Механизм fail-safe уже существует, менять `TcpIpcServer` почти не нужно.
- Лог при старте: явно писать «IPC auth DISABLED by config (all local callers allowed)».
- **Документация:** отметить риск (любой локальный процесс сможет управлять
  службой) — это осознанный диагностический режим.

> Замечание: если реальная проблема была в том, что GUI не мог подключиться/
> применить настройки из-за токена (напр. не читался `.ipc_token`), этот toggle
> её снимет. Но на форвардинг Wintun он не влияет — параллельно идём по §2.3.

### 2.3. Диагностический прогон embedded (после фикса №3) — runbook для стенда
Цель: локализовать, где теряется трафик. С включённым DEBUG/TRACE (level=4 после
§1.3) и `process_filter_enabled=false`:

1. Запустить службу (`capture_mode=wintun`, `engine=embedded`,
   `process_filter_enabled=false`, `log.level=4`).
2. Запустить тестовое приложение (`TransfersClient.exe` → `:9080/:9300`).
3. Читать лог и `Get-NetAdapterStatistics TcpRedirector`, интерпретировать:
   - **`[wintun][rx-stats] ipv4=0`** и `ReceivedBytes=0` → пакеты **не доходят до
     TUN**: проблема МАРШРУТИЗАЦИИ (сосуществующий VPN/WireGuard забирает трафик
     более длинным префиксом, либо протёкший `/32`-bypass). См. предыдущий разбор
     [`wintun_no_traffic_debug_2026-07-14.md`](wintun_no_traffic_debug_2026-07-14.md).
     Действие: убедиться, что сторонний VPN выключен; проверить `Find-NetRoute <dst>`
     → должен указывать на ifIndex адаптера `TcpRedirector`.
   - **`ipv4>0`, но нет `[wintun][flow] PROXY ...`** → пакеты входят, но lwIP не
     терминирует/accept не срабатывает (или всё падает в DIRECT-drop при
     `process_filter_enabled=true`). При `false` DIRECT не должно быть.
   - **есть `PROXY ... connect=OK`, но нет relay `CONNECT ... 200 OK`** → проблема
     на стыке relay↔прокси (адрес/порт/доступность `192.168.1.80:8080`, авторизация).
   - **`[wintun][flow] connect to relay ... FAILED (WSA=...)`** → relay не слушает
     или занят порт `34010`.
   - **relay пишет `No connection record for port X`** → регрессия B8 (не должно
     после фикса, но проверить).
4. Проверить IPv6-утечку: если `block_ipv6=true` и приложение всё же коннектится
   по IPv6 — смотреть `[wintun][rx-stats] ipv6_rst`.

Этот прогон выполняет **пользователь на стенде** (автопрогон ломает сеть) — мы
даём точные критерии интерпретации.

### 2.4. Побочно: external tun2socks.exe падает `code=1`
- Проверить формируемую командную строку и аргументы в
  [`Tun2SocksEngineExternal.cpp`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineExternal.cpp:1)
  (binding к адаптеру по имени/LUID, `-device`, `-proxy socks5://127.0.0.1:1080`,
  версия `tun2socks.exe` в `.bin/tun2socks/`).
- Запустить `tun2socks.exe` вручную с теми же аргументами и прочитать stderr —
  `code=1` обычно означает неверный аргумент или не найден адаптер.
- Это отдельная ветка (embedded — основной путь пользователя); фиксить после
  локализации, чтобы не смешивать с №1-embedded.

**Файлы:** `Config.h`, `ConfigManager.cpp`, `ServiceMain.h`, опц.
`Tun2SocksEngineExternal.cpp`, `docs/*`.

---

## 3. Запрос №2 — не шифровать пароль прокси, хранить/использовать «как есть»

### 3.1. Текущее поведение
- [`ConfigManager::EncryptPassword`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:1053)
  шифрует через DPAPI `CRYPTPROTECT_LOCAL_MACHINE`, хранит Base64 в
  `auth.encryptedPassword`.
- [`GetProxyConfig`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:131)
  расшифровывает через `DecryptPassword`. Если DPAPI-blob не читается (перенос
  между аккаунтами/скоупами, «протухший» user-scope после миграции), функция
  возвращает **пустую строку** → Basic-auth молча не срабатывает.

### 3.2. Исправление: toggle plaintext-пароля (обратно совместимо)
- **Config.h `AuthSettings`:** добавить
  ```cpp
  bool        encryptPassword = true;  //!< false → хранить/использовать пароль как есть
  std::string password;                //!< plaintext-пароль (используется при encryptPassword=false)
  ```
- **ConfigManager.cpp:**
  - Парсинг: читать `auth.encryptPassword` (default `true`) и `auth.password`.
  - `GetProxyConfig()`: если `!encryptPassword` — брать `auth.password` **как есть**
    (без DPAPI); иначе — прежняя логика (`DecryptPassword(encryptedPassword)`).
  - Сериализация: при `encryptPassword=false` писать `auth.password` (plaintext) и
    **не** писать/не использовать `encryptedPassword`; при `true` — как сейчас.
  - `SetProxyPassword` / `SetProxyConfig`: уважать флаг (при `false` — сохранять
    plaintext, не звать `EncryptPassword`).
- **Документация:** явный **security-warning** в
  [`КОНФИГУРАЦИЯ.md`](../docs/КОНФИГУРАЦИЯ.md) §7: при `encryptPassword=false`
  пароль лежит в `config.json` открытым текстом — использовать только для
  диагностики/доверенного окружения.

> Это снимает класс проблем «пароль зашифрован не тем скоупом → не расшифровался
> → авторизация на прокси падает». Пользователь сможет вписать пароль руками.

**Файлы:** `Config.h`, `ConfigManager.cpp`, `docs/КОНФИГУРАЦИЯ.md`.
Опц. GUI (C#) — тумблер и plaintext-поле (follow-up).

---

## 4. Делегирование и порядок исполнения

1. **Code (C++)** — №3: применить лог-уровень при старте + TRACE-шкала. *(разблокирует диагностику)*
2. **Code (C++)** — №1: toggle отключения IPC-auth.
3. **Code (C++)** — №2: toggle plaintext-пароля.
4. **Code (C++)** — сборка Release/x64, юнит-тесты (`RuleEngineTest`,
   `ConnectionTableTest`), деплой.
5. **Пользователь (стенд)** — прогон runbook §2.3 с DEBUG/TRACE; сбор нового лога.
6. **Debug (C++)** — по результатам runbook: локализация форвардинга №1
   (маршрутизация vs accept vs relay) и/или external `tun2socks code=1` (§2.4).
7. **Code (C++/C#)** — документация + опц. GUI-тумблеры.

## 5. Критерии приёмки
- В `config.json` `log.level=3` даёт видимые DEBUG-строки; `level=4` — TRACE
  (rx-stats и т.д.). Смена уровня реально меняет детализацию файла.
- `ipc.auth_enabled=false` → в логе «IPC auth DISABLED», GUI/любой локальный
  клиент подключается без токена; при `true` — прежнее поведение.
- `auth.encryptPassword=false` + `auth.password="..."` → служба использует пароль
  как есть (проверяется по успешному Basic `CONNECT` в логе relay).
- Сборка Release/x64 без ошибок; юнит-тесты зелёные.
- По новому DEBUG/TRACE-логу однозначно определяется стадия потери трафика №1.

## 6. Риски
- Изменение лог-шкалы: не сломать существующую GUI-трансляцию `0..3` (значение `4`
  — только ручной/экспертный).
- Plaintext-пароль: явно предупредить о хранении секрета открытым текстом.
- IPC без auth: повышает локальную поверхность атаки — диагностический режим,
  задокументировать.
- Не менять горячий путь lwIP/relay без данных runbook (высокий риск, отложено).

---

## 7. РЕАЛИЗОВАНО (15.07.2026)

Все правки внесены (сборку/установку выполняет пользователь).

### Служба (C++)
- **№3 лог-уровень (B10):** [`ServiceMain.h`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h) —
  после `Load()` вызывается `m_logger->SetLevel(GetLogLevel())`.
  [`ConfigManager.cpp`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp) —
  `GetLogLevel/SetLogLevel` расширены уровнем `4`=TRACE.
  [`Tun2SocksEngineEmbedded.cpp`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp) —
  `[rx-stats]` и `[DIRECT-drop]` понижены Trace→Debug.
- **№1 IPC-toggle (B11):** [`Config.h`](../src/service/TcpRedirectorService/infrastructure/config/Config.h) —
  `IpcSettings{auth_enabled=true}` + поле `Config::ipc`; парсинг/сериализация в
  `ConfigManager.cpp`; `ServiceMain.h` пропускает `SetAuthTokenFilePath` при
  `auth_enabled=false` (+WARN в лог).
- **№2 plaintext-пароль (B12):** [`Config.h`](../src/service/TcpRedirectorService/infrastructure/config/Config.h) —
  `AuthSettings.encryptPassword=true` + `AuthSettings.password`; `ConfigManager.cpp`
  — `GetProxyConfig/GetPlainPassword/SetPassword` уважают флаг, парсинг/сериализация;
  [`IpcHandler.h`](../src/service/TcpRedirectorService/adapters/driving/IpcHandler.h) —
  лог-сообщение отражает режим (DPAPI/plaintext).

### GUI (C#)
- [`ProxyConfig.cs`](../src/gui/TcpRedirectorGUI/Domain/Entities/ProxyConfig.cs) —
  `EncryptPassword`, `IpcAuthEnabled`.
- [`JsonConfigRepository.cs`](../src/gui/TcpRedirectorGUI/Infrastructure/Config/JsonConfigRepository.cs) —
  `WriteFullV2` пишет `auth.encryptPassword`/`auth.password`/`ipc.auth_enabled`.
- [`SettingsViewModel.cs`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs) —
  свойства `EncryptPassword`/`IpcAuthEnabled`/`HasStoredPassword`/`PasswordPlaceholder`;
  чтение/запись; TRACE↔4 в `DomainToFileLogLevel`/`FileToDomainLogLevel`.
- [`MainWindow.xaml`](../src/gui/TcpRedirectorGUI/MainWindow.xaml) +
  [`MainWindow.xaml.cs`](../src/gui/TcpRedirectorGUI/MainWindow.xaml.cs) — чекбоксы
  «Шифровать пароль (DPAPI)» и «Требовать токен IPC»; overlay-плейсхолдер
  `••••••••` в поле пароля (показан, если пароль сохранён; скрыт при вводе).

### Находка (отложено): external tun2socks `code=1` (B13)
Служба сама держит Wintun-адаптер открытым, `tun2socks.exe` не может открыть тот
же `wintun://<name>` → `code=1`. Фикс — не открывать адаптер в external-режиме
(handoff владения); рантайм-риск, вынесено отдельно. Основной путь — embedded.

### Проверка на стенде (за пользователем)
1. `log.level=3` → появляются DEBUG-строки; `4` → TRACE (rx-stats и т.д.).
2. Прогон embedded с `process_filter_enabled=false` и чтение лога по runbook §2.3.
3. `ipc.auth_enabled=false` → в логе «IPC auth DISABLED».
4. `auth.encryptPassword=false` + `auth.password` → Basic-auth по plaintext.
5. В GUI: поле пароля показывает `••••••••` при сохранённом пароле.

---

## 8. КОРНЕВАЯ ПРИЧИНА «трафик не выходит на прокси» — B14 (найдено 15.07.2026, второй лог)

### Симптом (приложение реально работало)
- В каждой строке `[wintun][rx-stats]` — `active_flows=0`, хотя `ipv4` растёт до сотен.
- Нет ни одной строки пути `OnAccept`: ни `[wintun][flow] PROXY`, ни `[wintun][DIRECT-drop]`, ни `connect FAILED`.
- В режиме `process_filter_enabled=false` у приложения ПРОПАДАЕТ интернет — весь TCP уходит в туннель и теряется.

### Вывод: catch-all listener НЕ ловит SYN — порт-0 НЕ wildcard по порту
Движок в `Start()` (Tun2SocksEngineEmbedded.cpp:956) делает `tcp_bind(pcb, IP_ANY_TYPE, 0)` + `tcp_listen`,
рассчитывая принять SYN на ЛЮБОЙ dst-порт. Для стокового lwIP 2.2.0 это неверно:
1. `tcp_bind` (external/lwip/src/core/tcp.c:709): при `port==0` вызывается `tcp_new_port()` — listener получает конкретный эфемерный порт, а не «любой».
2. `tcp_input` (external/lwip/src/core/tcp_in.c:326): SYN матчится к listen-pcb ТОЛЬКО при `lpcb->local_port == tcphdr->dest`. `IP_ANY_TYPE` (строка 327) снимает только проверку IP, но НЕ порта.

Цепочка: SYN на dst 9080/9300 → в lwIP → хук IP4_INPUT переписывает netif ip на dst → ip4_input_accept проходит по IP → в tcp_input порт listener'а ≠ dst-порт → нет матча → SYN дропается/RST → OnAccept не вызывается → active_flows=0. В 2b это рвёт весь TCP приложения.

Хук `LWIP_HOOK_TCP_INPACKET_PCB` не спасает (вызывается ПОСЛЕ поиска pcb, tcp_in.c:375) и в проекте не определён (lwip_hooks.h:52 объявляет только IP4_INPUT).

Гипотеза про `Proxy bypass /32 route for 192.168.1.80` — отклонена: это /32 только для IP прокси (обход петли), на перехват трафика приложения не влияет.

### Варианты фикса (правка сетевого ядра, высокий рантайм-риск)
- A (рекомендуется): трактовать `lpcb->local_port==0` как «любой порт» в tcp_in.c:326 + не назначать порт в tcp_bind. Вынести правку в project-owned форк, т.к. external/lwip помечен DO NOT EDIT.
- B: `LWIP_HOOK_TCP_INPACKET_PCB` не подходит (после матча).
- C: флаг wildcard-port в listen-pcb + проверка в tcp_input (тоже правка vendored).

Это и есть настоящая причина «embedded никогда не форвардил»: предыдущие фиксы (B8, ladder, IPv6-RST) необходимы, но недостаточны — до OnAccept дело не доходило.

### Верификация после фикса
- В логе `[wintun][flow] PROXY ... connect=OK` и `active_flows>0`; в 2b интернет не пропадает.
