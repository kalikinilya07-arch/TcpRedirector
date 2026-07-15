# Wintun: устранение IPv6-утечки + диагностическое логирование (2026-07-14)

Связано с прошлой диагностикой:
[`plans/wintun_no_traffic_debug_2026-07-14.md`](../../plans/wintun_no_traffic_debug_2026-07-14.md),
[`plans/wintun_route_ladder_design_2026-07-14.md`](../../plans/wintun_route_ladder_design_2026-07-14.md),
`docs/ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md` §0.5 (B8), §4 («IPv6 в туннеле не проброшен»).

## Симптом (со слов пользователя)
`capture_mode=wintun`, `engine=embedded`: WinDivert работает, Wintun — нет.
Трафик доходит до TUN-адаптера, но «из внутренней сети не выходит». При включённом
проксировании `ping` и утилиты работают через **IPv6**. В логах ошибок нет.
Диагностика `ipconfig`: у Wi-Fi активны глобальные IPv6 (`2a02:2168:...`), у адаптера
`TcpRedirector` — IPv4 `10.6.7.1` + fe80 link-local.

## Корневой диагноз
1. **IPv6-утечка (главная причина).** Туннель, route-ladder (`RouteInstaller`),
   embedded lwIP, relay и HTTP `CONNECT` — **только IPv4**. Хост имеет глобальный
   IPv6 с целым default-маршрутом на Wi-Fi. По RFC 6724 Windows предпочитает
   глобальный IPv6 IPv4 для dual-stack адресатов → трафик уходит по IPv6 мимо
   IPv4-TUN. Это и есть «работает через IPv6».
2. **Отсутствие видимости.** В embedded при неудаче PROXY-flow (или DIRECT/BLOCK)
   соединение молча `tcp_abort`ится, а установленные маршруты/байпассы почти не
   логируются на DEBUG → «ошибок в логах нет» даже при сбое. Пользователь просит
   добавить отладочную информацию о маршрутизации и подключениях.
3. **Остаточные адаптеры/маршруты** от прошлых запусков могут мешать (частично уже
   закрыто F1/F3, но стоит добавить диагностику и подстраховку по дублям адаптера).

## Согласованные решения (с пользователем)
- **IPv6-стратегия:** заворачивать весь IPv6 в TUN (маршруты `::/1` + `8000::/1`
  на Wintun-адаптере) и **отвечать TCP RST на IPv6 SYN** внутри embedded-движка,
  чтобы приложение мгновенно откатывалось на IPv4 (который идёт в туннель и
  проксируется). Не-TCP IPv6 — дропать. Всё **полностью снимается на Close**.
- **Гейт конфигом:** новое поле `wintun.block_ipv6` (bool, **default `true`**).
  При `false` — старое поведение (IPv6 не трогаем).

---

## Задачи (по порядку реализации)

> ⚠️ Требуется агент с правами на правку исходников (C++/сервис) — этот план
> создан в режиме планирования. Сборка: Release/x64 (msbuild), затем деплой в
> `C:\Program Files\TcpRedirector\`. Рантайм-проверку трафика выполняет
> пользователь вручную (ломает сторонние сетевые вещи).

### T1. Конфиг: флаг `wintun.block_ipv6`
- [`Config.h`](../../src/service/TcpRedirectorService/infrastructure/config/Config.h) —
  в `WintunSettings` добавить `bool block_ipv6 = true;` (рядом с
  `route_ladder_prefix`).
- [`ConfigManager.cpp`](../../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp) —
  парсинг (default `true`) + сериализация в секции `wintun` (по образцу
  `process_filter_enabled` / `route_ladder_prefix`).
- GUI: **необязательно** для фикса (default в C++ = `true`). Если добавлять —
  чтение/запись в `JsonConfigRepository.cs` (`ReadWintunSettings`/`WintunToJson`)
  через merge, чтобы не потерять поле (см. B6). Пометить как отдельный low-prio
  пункт; функционально не блокирует.
- Обновить `docs/КОНФИГУРАЦИЯ.md` §10 (таблица полей `wintun`) и §10.2 (описание
  поведения IPv6).

### T2. Маршруты IPv6 в TUN (`RouteInstaller`)
Цель: завернуть весь IPv6 в Wintun-адаптер, чтобы IPv6-пакеты попадали в
embedded-движок (где мы ответим RST), а не уходили на Wi-Fi.
- [`RouteInstaller.h`](../../src/service/TcpRedirectorService/infrastructure/capture/wintun/RouteInstaller.h) /
  [`.cpp`](../../src/service/TcpRedirectorService/infrastructure/capture/wintun/RouteInstaller.cpp):
  - `InstallIpv6CatchAll(NET_LUID luid, uint32_t metric, std::string* outError)` —
    ставит `::/1` и `8000::/1` (AF_INET6) on-link (NextHop `::`) с `Protocol=NETMGMT`,
    `Origin=NlroManual`. Пары `/1` достаточно (никакой сторонний IPv6-VPN на стенде
    не конкурирует; при желании — параметризовать глубину позже).
  - `UninstallIpv6CatchAll(NET_LUID luid, std::string* outError)` — перечислить
    `GetIpForwardTable2(AF_INET6)` и снести наши on-link NETMGMT `/1`-маршруты на
    LUID (по образцу `UninstallSplitTunnel`).
  - Дополнительно `SetInterfaceMetric` расширить/продублировать на `AF_INET6`
    (низкая метрика для IPv6-интерфейса), либо добавить `SetInterfaceMetricV6`.
  - **Carve-out для IPv6-прокси:** если `proxy.host` резолвится в IPv6 (редкий
    кейс) — bypass /128 на физ. путь (аналог `InstallHostBypass`, семейство v6).
    Для типового IPv4-прокси не требуется; зафиксировать TODO/лог-предупреждение.
- Проверить, что у Wintun-интерфейса IPv6 включён (иначе `CreateIpForwardEntry2`
  AF_INET6 вернёт ошибку). Обычно fe80 link-local уже есть; при необходимости
  включить IPv6 на интерфейсе в `WintunAdapter` (сейчас только `ConfigureIpv4`).

### T3. Интеграция IPv6-маршрутов в `WintunCapture::Open`/`TearDown`
- [`WintunCapture.cpp`](../../src/service/TcpRedirectorService/infrastructure/capture/WintunCapture.cpp):
  - После установки IPv4-лестницы (шаг 7) и **только при `m_settings.block_ipv6`**:
    вызвать `InstallIpv6CatchAll(luid, 1, &err)`; при провале — WARN, не фатал
    (IPv6-утечка возможна, но IPv4 продолжит работать). Обернуть в существующий
    паттерн (флаг `m_ipv6RoutesInstalled`).
  - В `TearDown` — `UninstallIpv6CatchAll` до удаления адаптера (по образцу IPv4).
  - Пробросить `block_ipv6` в embedded-движок (см. T4) через `EmbeddedProcessFilter`
    или отдельный сеттер.

### T4. Embedded: RST на IPv6-SYN (`Tun2SocksEngineEmbedded`)
Точка перехвата — единственный дренаж пакетов из TUN:
[`Tun2SocksEngineEmbedded.cpp:671-683`](../../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:671)
(`EngineThreadMain`, после `TryReceiveInto` в `rxbuf`, ДО `nf->input`).
- Проверить версию IP по первому нибблу: `(rxbuf[0] >> 4) == 6` → это IPv6.
  Такие пакеты **не** передавать в lwIP (`nf->input`), а обрабатывать отдельно:
  - Если IPv6 + `NextHeader==TCP(6)` + флаг **SYN** (и не ACK) → сформировать
    ответный **IPv6 TCP RST** (swap src/dst IPv6 и портов; `seq=0`,
    `ack=incoming_seq+1`, флаги `RST|ACK`; корректная TCP-контрольная сумма с
    IPv6-псевдозаголовком) и записать обратно в туннель `m_session->Send(pkt, len)`.
  - Прочий IPv6 (не-SYN TCP, UDP, ICMPv6, extension headers) — **дропать**
    (просто не форвардить). Extension-headers у SYN на практике нет; если
    NextHeader != 6 — дроп без RST.
  - Учитывать в статистике/логах (см. T5), но НЕ в TX-байтах прокси.
- Новый хелпер `HandleIpv6Packet(const uint8_t* pkt, int len)` в движке; вызывается
  только при `m_block_ipv6==true`. При `false` — текущее поведение (IPv6 уйдёт в
  lwIP, который его проигнорирует/уронит; допустимо).
- Реализация без внешних либ: ручной разбор фиксированного IPv6-заголовка (40 байт)
  + TCP-заголовка; RST-пакет — статический буфер 40+20 байт. Контрольная сумма TCP
  по RFC 2460 псевдозаголовку (src/dst 16б, upper-layer len, next-header=6).

### T5. Диагностическое логирование Wintun (явный запрос пользователя)
Цель: видеть «что и куда маршрутизируется» и «кто/когда подключается». Уровень —
DEBUG (маршруты/старт) и TRACE (пер-пакет/пер-flow), под общий тег `"wintun"`,
подчиняется `log.level` и ротации.
- **`WintunCapture::Open` (DEBUG):** уже логирует часть; добавить:
  - LUID и ifIndex адаптера, назначенный IPv4/CIDR, MTU;
  - каждый установленный лист IPv4-лестницы (`prefix/len`) и IPv6 `::/1,8000::/1`
    (сводно: количество + список на TRACE);
  - установленные/снятые proxy-bypass `/32`, interface-metric;
  - явный итог: сколько маршрутов поставлено, какие carve-outs.
  - Для этого расширить `RouteInstaller` возвратом списка поставленных префиксов
    (out-параметр `std::vector<std::pair<...>>* installed`) ИЛИ логировать внутри
    `RouteInstaller` через переданный `ILogSink*` (предпочтительно: добавить
    опциональный `domain::ports::ILogSink*` в статические методы и логировать
    каждую строку на DEBUG/TRACE).
- **`EngineThreadMain` дренаж (TRACE, троттлить):** периодически (напр. раз в N мс
  или каждые M пакетов) логировать счётчики: получено IPv4/IPv6 пакетов, отвечено
  RST на IPv6, дропнуто. Не логировать каждый пакет в проде (спам).
- **`OnAccept` (DEBUG на flow):** для каждого нового TCP-flow из туннеля логировать:
  `source_ip:source_port`, резолв `pid`/имя процесса, **решение** (PROXY/DIRECT/BLOCK),
  `original_dst_ip:port`, назначенный `relay_src_port`, результат `connect()` к relay.
  Сейчас connect-fail и DIRECT-drop почти немы — добавить явные строки:
  - PROXY: `"[wintun][flow] pid=.. proc=.. src=..:.. dst=..:.. relay_port=.. connect=OK"`.
  - connect-fail (строка ~322): WARN с `WSAGetLastError()` и dst — сейчас
    отсутствует (молчаливый `tcp_abort`).
  - `ConnectionTable::Add`/`Remove` — TRACE с портом и dst.
- **IPv6-RST (TRACE):** `"[wintun][ipv6-rst] src=[..]:.. dst=[..]:.. -> RST (forcing IPv4 fallback)"`.
- Убедиться, что ничего секретного (пароль прокси) не логируется.

### T6. Гигиена/диагностика остаточных адаптеров и маршрутов
- На старте `WintunCapture::Open` (DEBUG): перечислить существующие адаптеры/
  интерфейсы с именем `adapter_name` (дубли от прошлых крэшей) и залогировать
  предупреждение при обнаружении >1. Полное авто-удаление дублей — опционально
  (риск), по умолчанию только диагностика.
- Подтвердить, что `UninstallSplitTunnel` + новый `UninstallIpv6CatchAll` +
  `UninstallHostBypass` вызываются на всех путях выхода (Close и деструктор) —
  уже так для IPv4; добавить IPv6.
- Задокументировать в `docs/ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md` новый раздел (аналог §0.5)
  с описанием IPv6-фикса и остаточной уборки.

---

## Порядок работ и делегирование
1. T1 (конфиг) → T2 (RouteInstaller IPv6) → T3 (WintunCapture wiring) →
   T4 (embedded RST) → T5 (логирование) → T6 (гигиена+доки).
2. T2+T4 — самые рискованные (сеть/ручной разбор пакетов); делать вместе одним
   исполнителем, покрыть юнит-тестом разбора/чек-суммы IPv6-RST если возможно.
3. T5 может частично идти параллельно (не зависит от IPv6-логики).

## Проверка (после сборки Release/x64, деплой; трафик — вручную пользователем)
1. Служба стартует в `wintun/embedded`, в логе на DEBUG видны: LUID/ifIndex,
   IPv4-CIDR, список IPv4-листьев лестницы, `::/1`+`8000::/1`, bypass /32,
   interface-metric.
2. `Get-NetRoute -AddressFamily IPv6` показывает `::/1` и `8000::/1` на ifIndex
   адаптера `TcpRedirector`; после чистого стопа они исчезают.
3. `ping -6 <dual-stack-host>` / `curl -6`: соединение мгновенно отклоняется
   (RST) → приложение падает на IPv4; `ping <host>` (IPv4) идёт через туннель.
4. Тестовое приложение (proxy `127.0.0.2:8080`, правило под него ИЛИ
   `process_filter_enabled=false`):
   - `Get-NetAdapterStatistics TcpRedirector` RX>0;
   - в логе на DEBUG видны flow-строки с PROXY/dst/relay_port и `connect=OK`;
   - НЕТ `No connection record`;
   - на прокси виден `CONNECT`.
5. При `block_ipv6=false` — старое поведение (IPv6 не заворачивается), регресса нет.
6. Юнит-тесты `RuleEngineTest` 16/16, `ConnectionTableTest` 8/8 — PASS (не должны
   деградировать).

## Открытые/отложенные вопросы (не блокируют v1)
- IPv6-прокси (`proxy.host` = IPv6-литерал/AAAA) — сейчас не поддерживается relay;
  при block_ipv6 нужен /128-bypass. Зафиксировать как отдельную доработку.
- Полноценный IPv6-туннель в embedded (реальное проксирование IPv6→прокси) —
  большой объём (IPv6 в lwIP + relay). Вне рамок; RST-фолбэк — прагматичный v1.
- Авто-удаление дублирующих адаптеров — оставлено как диагностика (риск).

---

## Runtime follow-up (2026-07-15) — «трафик приходит в TUN, но не выходит наружу»

Пользователь: в GUI видно, что трафик приходит в виртуальную сеть, но наружу не
идёт. Просьба проверить маршрутизацию virtual↔physical и полноту конфига
(`output/config.json`).

### Ключевая находка: тест идёт на СТАРОМ бинаре, лог устарел
- Пересборка (этот план, T1–T6) легла в
  `src/service/TcpRedirectorService/build/service/x64/Release/TcpRedirectorService.exe`,
  а **НЕ** в `C:\Program Files\TcpRedirector\` — развёрнутый бинарь **не обновлён**.
- Развёрнутый лог `C:\Program Files\TcpRedirector\.logs\tcp_redirector.log`
  заканчивается `2026-07-14 21:15:49` строкой «Service is running» и **не содержит
  НИ ОДНОЙ** пер-соединенческой строки (`CONNECT`, `No connection record`,
  `connect to proxy failed`). То есть сегодняшний тест этим бинарём/логом не
  фиксируется, и новые диагностические строки (T5) физически не могут появиться.
- **Вывод:** первый и обязательный шаг — **пере-деплой свежей сборки + рестарт
  службы**. Уже одно это может починить egress, если развёрнутый бинарь старше
  фикса B8 (регистрация original-dst в `ConnectionTable`): без B8 embedded-flow
  порождает `No connection record for port X` (DEBUG) и relay молча закрывает
  соединение — ровно «пришло в TUN, но не вышло».

### Анализ маршрутизации virtual↔physical для ТЕКУЩЕГО конфига
Конфиг: proxy `192.168.1.80:8080` (в LAN `192.168.1.0/24`), Wi-Fi хоста
`192.168.1.147/24`, `route_ladder_prefix=5`, `process_filter_enabled=false`
(весь IPv4-TCP → PROXY).
- Лестница `/5` (on-link, metric 1) на Wintun покрывает `0.0.0.0/0` минус
  carve-outs (`127/8,0/8,169.254/16`).
- `InstallHostBypass` ставит `192.168.1.80/32` через физ. путь (GetBestRoute2)
  ДО лестницы; плюс connected-маршрут LAN `192.168.1.0/24` (/24 > /5) на Wi-Fi.
- **Итог:** relay→proxy (`192.168.1.80`) уходит физически, НЕ заворачивается в
  TUN. Для этого конфига «петли virtual↔physical» на пути к прокси НЕТ — маршрутизация
  прокси-байпаса корректна. Значит корень egress-проблемы, скорее всего, **не в
  route table**, а на прикладном уровне relay→proxy (см. ниже) либо в устаревшем
  бинаре.
- Побочно: при `route_all_traffic=true` любой LAN-адресат приложения (не только
  прокси) затягивается в TUN и проксируется — ожидаемо для «весь трафик».

### Список подозреваемых для egress (подтвердить НОВЫМИ логами после деплоя)
1. **Устаревший бинарь без B8** (самый вероятный) → `No connection record` →
   relay закрывает flow. Лечится деплоем свежей сборки.
2. **Basic-auth 407.** `auth.enabled=true`, Basic, `username=admin`,
   `encryptedPassword` задан. Если прокси требует авторизацию, а DPAPI-пароль не
   расшифровывается под аккаунтом службы (LocalSystem) — CONNECT получает **407**,
   relay пишет `CONNECT failed: <...407...>` (WARN) и закрывает. Симптом ровно
   «внутрь пришло, наружу не пошло». Проверка: в новом логе искать `407`/
   `CONNECT failed`. Лечение: заново ввести пароль в GUI (после миграции DPAPI на
   LocalMachine старые user-scope пароли нечитаемы — см. `ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md` §0.4).
3. **Достижимость прокси.** `192.168.1.80:8080` реально слушает? Проверить
   `Test-NetConnection 192.168.1.80 -Port 8080` с хоста; в логе — `connect to
   proxy failed: <WSA>`.
4. **relay→proxy connect уходит в TUN?** Только если `/32`-bypass не встал
   (в логе будет соответствующий WARN из `WintunCapture`); для loopback-прокси
   не применимо, но здесь прокси remote — bypass обязателен и логируется.

### Пробел конфига/GUI (реальный, добавить)
- **T7 (новое): GUI не пишет `block_ipv6`.** `JsonConfigRepository.cs`
  (`ReadWintunSettings`/`WintunToJson`) читает/пишет `process_filter_enabled` и
  `route_ladder_prefix`, но **не** `block_ipv6`. Поэтому сгенерированный
  `config.json` его не содержит. Функционально не блокирует (C++ default=`true`),
  но пользователь не может управлять флагом и его не видно в файле.
  Добавить: поле `BlockIpv6` в C#-модель `WintunSettings`, чтение
  (`jw["block_ipv6"]`, default true) и запись (`["block_ipv6"] = w.BlockIpv6`)
  через merge (не терять поле). Опционально — чекбокс в GUI. Низкий приоритет.
- Прочие поля секции `wintun` в присланном `output/config.json` присутствуют и
  валидны; ничего критичного больше не отсутствует.

### Протокол деплоя и верификации (обязательный порядок)
1. Собрать (уже сделано, 0 ошибок) и **развернуть** свежий сервис в
   `C:\Program Files\TcpRedirector\`: остановить службу
   (`sc stop TcpRedirectorService`), скопировать новый
   `build\service\x64\Release\TcpRedirectorService.exe` поверх развёрнутого
   (или прогнать `deploy.bat`), затем `sc start TcpRedirectorService`.
   ⚠️ Требуется агент с правами записи в `Program Files` + управления службой
   (этот шаг — вне plan-режима).
2. Убедиться, что `log.level=3` (DEBUG) — уже так.
3. Воспроизвести трафик тестовым приложением.
4. Прочитать НОВЫЙ хвост `C:\Program Files\TcpRedirector\.logs\tcp_redirector.log`
   и классифицировать по подозреваемым 1–4:
   - есть `[wintun][flow] PROXY ... connect=OK` + relay `CONNECT response: 200 OK`
     → egress работает;
   - `No connection record` → всё ещё старый бинарь/не B8;
   - `CONNECT failed: ...407...` → проблема авторизации (п.2);
   - `connect to proxy failed` → прокси недостижим (п.3);
   - `[wintun][ipv6-rst] ...` и рост `ipv6_rst` в `[wintun][rx-stats]` →
     IPv6-нейтрализация работает.
5. Если egress ОК по IPv4 — проверить, что `ping`/утилиты больше не уходят по
   IPv6 (см. основной раздел «Проверка»).

### Подтверждение по полному логу (`output/tcp_redirector.log`, 340 строк, до 21:15:49)
- **Relay не обработал НИ ОДНОГО соединения** во всех запусках: `ConnectionHandler`
  на КАЖДОЕ соединение пишет DEBUG `m_proxyAuthRequired=… m_kerberosAuth=…`
  (`TcpRelayServer.h:392`), плюс `No connection record` / `CONNECT response`.
  В логе НЕТ ни одной подстроки «connect»/«connection». Значит на
  `127.0.0.1:34010` соединения не приходили → лог заморожен на инициализации,
  сегодняшний тест им не фиксируется. Redeploy нового бинаря обязателен.
- Лог заканчивается embedded-запуском 21:15:49 (`process_filter DISABLED`,
  Option 2b), после «Service is running» — тишина.

### T8 (новое, РЕАЛЬНЫЙ БАГ) — external-движок роняет tun2socks кривыми аргументами
Из лога (строки ~90–320) видно детерминированный краш external-движка:
```
unknown shorthand flag: 'l' in -loglevel
tun2socks exited (code=2); supervisor will restart …
child restart rate limit reached (6/60s); giving up
```
- **Причина.** `Tun2SocksEngineExternal.cpp:184-189` передаёт длинные опции с
  ОДИНАРНЫМ дефисом: `-device`, `-proxy`, `-loglevel`. Бинарь
  `xjasonlyu/tun2socks` использует Go `pflag`: длинные опции требуют ДВОЙНОЙ дефис
  (`--device`, `--proxy`, `--loglevel`), а одиночный дефис — только шорткаты
  (`-d`, `-p`). Поэтому `-loglevel` парсится как шорткат `-l` → «unknown shorthand
  flag 'l'» → exit code 2; `-device`/`-proxy` тоже молча искажаются.
- **Фикс.** В `Tun2SocksEngineExternal::…` заменить на двойной дефис:
  ```
  cfg.args.push_back(L"--device");  cfg.args.push_back(L"wintun://" + m_adapterName);
  cfg.args.push_back(L"--proxy");   cfg.args.push_back(L"socks5://" + …socks5_listen);
  cfg.args.push_back(L"--loglevel");cfg.args.push_back(L"info");
  ```
  (или шорткаты `-d`/`-p` + `--loglevel`). Обновить и комментарий-пример
  (строки 178-181). После фикса external-режим станет рабочей альтернативой
  embedded (полезно как обходной путь, пока embedded диагностируется).
- **Приоритет.** Высокий как отдельный дефект, но НЕ на пути текущего теста
  (конфиг `engine=embedded`). Для пользователя это второй рабочий вариант.
- Пункт 3.1/3.2 (локинг/однократное решение flow) из аудита — не трогаем здесь.

---

## Статус применения (2026-07-15) — ВСЕ ЗАДАЧИ ПРИМЕНЕНЫ
- **T1–T6 ПРИМЕНЕНЫ** к рабочему дереву. Файлы: `Config.h`, `ConfigManager.cpp`,
  `RouteInstaller.h/.cpp`, `WintunCapture.h/.cpp`, `Tun2SocksEngineEmbedded.h/.cpp`,
  `docs/*`.
- **T8 ПРИМЕНЕН** — `Tun2SocksEngineExternal.cpp`: `--device/--proxy/--loglevel`.
- **T7 ПРИМЕНЕН** (включая опциональный UI):
  - `Domain/Entities/AppRule.cs` — поле `BlockIpv6` (default true);
  - `JsonConfigRepository.cs` — чтение `block_ipv6` + запись в `WintunSettingsToJson`;
  - `SettingsViewModel.cs` — свойство `WintunBlockIpv6` + `OnPropertyChanged` в reload;
  - `MainWindow.xaml` — чекбокс «Блокировать IPv6…».
- **Верификация сборки (2026-07-15):** сервис `msbuild Release/x64` → 0 ошибок;
  GUI `dotnet build -c Release` → 0 ошибок/0 предупреждений; юнит-тесты
  `RuleEngineTest` и `ConnectionTableTest` → exit 0 (PASS).
- **Установку/деплой/рестарт службы и сбор логов пользователь делает сам**
  (по его просьбе — здесь не выполнялось). Свежий сервис:
  `src/service/TcpRedirectorService/build/service/x64/Release/TcpRedirectorService.exe`;
  GUI: `src/gui/TcpRedirectorGUI/bin/Release/net9.0-windows/`.
- Ниже сохранены точные снимки правок (для истории/ревью).

## Готовые к применению правки (T8 + T7)

### T8 — `src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineExternal.cpp`
Заменить блок ~184-189 (одинарный → двойной дефис):
```cpp
    cfg.args.push_back(L"--device");
    cfg.args.push_back(L"wintun://" + m_adapterName);
    cfg.args.push_back(L"--proxy");
    cfg.args.push_back(L"socks5://" + Utf8ToWide(m_ext.socks5_listen));
    cfg.args.push_back(L"--loglevel");
    cfg.args.push_back(L"info");
```
(Комментарий-пример ~178-181 привести к `--device/--proxy/--loglevel`.)

### T7a — `src/gui/TcpRedirectorGUI/Domain/Entities/AppRule.cs`
После `RouteLadderPrefix` (строка ~94) добавить свойство в `WintunSettings`:
```csharp
    /// <summary>
    /// Neutralize IPv6 while Wintun capture is active. Mirrors C++
    /// <c>wintun.block_ipv6</c> (default <c>true</c>). Round-tripped so the GUI
    /// does not drop it when rewriting the wintun section.
    /// </summary>
    public bool BlockIpv6 { get; set; } = true;
```

### T7b — `JsonConfigRepository.cs` `ReadWintunSettings` (после строки ~179)
```csharp
            // block_ipv6 — neutralize IPv6 in Wintun mode. Missing → default true.
            if (jw["block_ipv6"] is JsonValue biVal && biVal.TryGetValue<bool>(out var bi))
                w.BlockIpv6 = bi;
```

### T7c — `JsonConfigRepository.cs` `WintunSettingsToJson` (после строки ~610)
```csharp
            ["block_ipv6"]             = w.BlockIpv6,
```

### T7d (опционально) — GUI-контрол
- `SettingsViewModel.cs`: свойство `WintunBlockIpv6` (get/set `Wintun.BlockIpv6`
  + `OnPropertyChanged`), продублировать `OnPropertyChanged(nameof(WintunBlockIpv6))`
  рядом со строкой ~513-514; чекбокс в XAML настроек Wintun. Низкий приоритет —
  round-trip (T7a–c) уже сохраняет поле, даже без UI.

## Проверка после ручной пересборки (без установки — на усмотрение пользователя)
- Сервис: `msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64`
  → 0 ошибок (пред-существующие C4005 — не в счёт).
- GUI: `dotnet build TcpRedirectorGUI.csproj -c Release` → 0 ошибок; при сохранении
  из GUI в `config.json` появляется `block_ipv6`.
- Юнит-тесты не затрагиваются T7/T8.
- После самостоятельного деплоя нового сервиса — классифицировать egress по
  «Протоколу деплоя и верификации» (см. выше): `[wintun][flow] … connect=OK`,
  `CONNECT failed …407…`, `connect to proxy failed`, `No connection record`.
