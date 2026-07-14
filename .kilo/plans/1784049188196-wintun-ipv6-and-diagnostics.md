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
- Пункт 3.1/3.2 (локинг/однократное решение flow) из аудита — не трогаем здесь.
