# Wintun «нет трафика через прокси» — диагностика (2026-07-14)

## Симптом
`capture_mode=wintun` (engine=embedded): трафик тестового приложения не
перехватывается и не идёт через прокси. `capture_mode=windivert` — работает.

## Стенд
- Прокси: mitmproxy в docker `9dd04693840a`, слушает `0.0.0.0:8080` → доступен как `127.0.0.2:8080`.
- Служба: `C:\Program Files\TcpRedirector\`, логи в `.logs\` (NB: каталог назван `.logs`, не `logs`).
- Тестовое приложение: `E:\...\second_system\1\TransfersClient.exe`, коннектится к `19.10.251.100:9080`.
- Служба собрана Release/x64 из текущего дерева и развёрнута.

## Ход диагностики

### Служба стартует корректно
Лог (`.logs\tcp_redirector.log`, старт 16:54:28):
```
Proxy set from config: 127.0.0.2:8080
Listening on 0.0.0.0:34010            (relay)
Capture mode: Wintun (embedded engine)
Adapter ready: name='TcpRedirector'
Process filter ENABLED for embedded engine
WintunCapture opened: tunnel=10.6.7.1/24 gateway=10.6.7.1 relay_port=34010 engine=embedded
Service is running
```
→ Гипотеза «proxy_configured=false» опровергнута: прокси сконфигурирован.

### Гипотеза 4 (RX/TX) — РЕШАЮЩИЙ ИНСТРУМЕНТ: RX=0
`Get-NetAdapterStatistics` по адаптеру `TcpRedirector`:
- До и после запуска приложения: **ReceivedBytes = 0, ReceivedUnicastPackets = 0**.
- Т.е. пакеты НЕ попадают в TUN-адаптер вообще → проблема в МАРШРУТИЗАЦИИ/ПРИЁМЕ,
  а НЕ в lwIP/DecideFlow. Это сразу снимает гипотезы 2 (DecideFlow→DIRECT) и
  3 (DNS/UDP), т.к. до них дело не доходит.

### Соединение уходит мимо туннеля
`Get-NetTCPConnection` для TransfersClient:
```
192.168.1.147:50835 -> 19.10.251.100:9080  SynSent
```
Источник `192.168.1.147` — физический интерфейс, НЕ tunnel (10.6.7.x). SYN идёт
наружу физическим путём, соединение висит в SynSent.

### Гипотеза 1 (метрики) — уточнена
Интерфейс-метрика Wintun = 5 (AutomaticMetric=Enabled — код её НЕ задаёт).
Наши split-маршруты стоят и по метрике выигрывают у дефолта:
```
0.0.0.0/1   ifIndex 78 (Wintun) RouteMetric 1 + IfMetric 5 = 6
128.0.0.0/1 ifIndex 78 (Wintun) RouteMetric 1 + IfMetric 5 = 6
0.0.0.0/0   ifIndex 24 (Wi-Fi)  eff = 35
```
НО метрика не решает: побеждает самый длинный префикс (longest-prefix-match).

### КОРНЕВАЯ ПРИЧИНА: наши /1 проигрывают более специфичным чужим маршрутам
`Find-NetRoute -RemoteIPAddress 19.10.251.100` (тестовый dst):

1. Сначала выигрывал **`19.10.251.100/32` на ifIndex 24 (Wi-Fi)**, NextHop `192.168.1.80`,
   RouteMetric 1, Protocol=NETMGMT. Это **устаревший proxy-bypass /32**, оставшийся
   от ПРЕДЫДУЩЕГО запуска, когда `proxy.host = 19.10.251.100:9080` (см. старые 502-логи).
   `InstallHostBypass` ставит его, а снимает только `UninstallHostBypass` на чистом
   стопе. При смене proxy.host / крэше — /32 «протекает» и навсегда уводит этот dst мимо туннеля.
   /32 — максимально специфичный префикс, бьёт наш /1 всегда.

2. После ручного удаления /32 маршрут к `19.10.251.100` упал на
   **`16.0.0.0/4` на ifIndex 76 (WireGuard Tunnel)** — на машине активен посторонний
   always-on WireGuard VPN, который ставит полную «лестницу» маршрутов `/2../8`
   (0.0.0.0/1 ему не конкурент: /4 длиннее /1). Т.е. весь публичный IPv4 забирает WireGuard.

Подтверждение приёмом: с приложением-генератором `TcpRedirector.ReceivedBytes`
остаётся ровно **0**, а `redlinkqxlp9gip` (WireGuard) RX растёт (111 MB) — пакеты
уходят в WireGuard, не в наш TUN.

## Вывод
Wintun-перехват основан на маршрутизации. Split `0.0.0.0/1 + 128.0.0.0/1`
(префикс /1) проигрывает по longest-prefix-match любому сосуществующему туннелю/маршруту
с более длинным префиксом:
- собственный «протёкший» proxy-bypass `/32` от старого запуска;
- сторонний VPN (WireGuard), раскладывающий `/2../8`.

WinDivert работает, потому что перехватывает на уровне WFP независимо от таблицы маршрутов.

## Кандидаты на исправление (обсудить перед реализацией)
- (A) Понижать interface-metric адаптера — НЕ поможет: проблема в длине префикса, не в метрике.
- (B) Гигиена proxy-bypass: снимать «протёкшие» /32 (стартовая очистка чужих NETMGMT /32
  к прежним прокси; фикс утечки при смене proxy.host / грязном стопе).
- (C) Устойчивость к сосуществующим full-tunnel VPN — принципиально не решается split-/1
  через route table (нужен либо WFP-hook, либо более специфичные маршруты, что бесконечная гонка).
- (D) Для стенда: убедиться, что посторонний WireGuard VPN выключен на время теста Wintun,
  иначе он всегда забирает весь публичный IPv4.

## РЕАЛИЗОВАННОЕ ИСПРАВЛЕНИЕ (кардинальный ladder-подход, согласовано)

Дизайн: [`plans/wintun_route_ladder_design_2026-07-14.md`](wintun_route_ladder_design_2026-07-14.md).

Заменили одиночную пару `0.0.0.0/1 + 128.0.0.0/1` на «лестницу» более специфичных
маршрутов `/D` (по умолчанию `D=5`, конфигурируемо `wintun.route_ladder_prefix`
1..8), покрывающую `0.0.0.0/0` за вычетом carve-out диапазонов. Плюс сопутствующие
фиксы F1/F2/F3.

Изменённые файлы:
- [`Config.h`](../src/service/TcpRedirectorService/infrastructure/config/Config.h:205) —
  поле `int route_ladder_prefix = 5` в `WintunSettings`.
- [`ConfigManager.cpp`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:805) —
  парсинг+clamp `route_ladder_prefix` (1..8); сериализация.
- [`RouteInstaller.h`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/RouteInstaller.h:71) /
  [`RouteInstaller.cpp`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/RouteInstaller.cpp:185):
  - `InstallSplitTunnel(..., int ladder_prefix, ...)` — генерация complement-лестницы
    через `ExpandLadder` с carve-outs (`127/8`, `0/8`, `169.254/16`), on-link NextHop=0.
  - `UninstallSplitTunnel` — перечисляет IPv4-таблицу и сносит ВСЕ on-link NETMGMT
    маршруты `/1../8` на LUID (teardown любой глубины) — фикс F3.
  - `CleanupStaleBypass(keep_ipv4_be)` — сносит наши «протёкшие» `/32` NETMGMT
    proxy-bypass, не входящие в keep-множество — фикс F1 (главный баг).
  - `SetInterfaceMetric(luid, 1)` — `AutomaticMetric=off`, метрика 1 — фикс F2.
- [`WintunCapture.cpp`](../src/service/TcpRedirectorService/infrastructure/capture/WintunCapture.cpp:296):
  - `CleanupStaleBypass(proxyIps)` ВСЕГДА до install (в т.ч. для loopback-прокси —
    keep пусто → сносятся все старые /32).
  - `SetInterfaceMetric(luid, 1)` до install лестницы.
  - `InstallSplitTunnel(..., m_settings.route_ladder_prefix, ...)`.

Сборка Release/x64 — успешно (exit 0). Бинарь развёрнут в `C:\Program Files\TcpRedirector\`.
Config стенда: добавлен `wintun.route_ladder_prefix = 5`.

Проверка teardown (после чистого стопа службы): адаптер `TcpRedirector` удалён,
на его LUID НЕ осталось on-link `/1../8` маршрутов, старый `19.10.251.100/32` отсутствует.

## Верификация прохождения трафика — ЗА ПОЛЬЗОВАТЕЛЕМ
По просьбе пользователя прогон трафика через прокси на стенде НЕ выполняется
автоматически (ломает сторонние сетевые вещи). Пользователь проверяет вручную:
сборкой приложения и ручным запуском. Ожидаемое подтверждение фикса:
1. При запущенной службе (wintun, D=5) `Get-NetAdapterStatistics TcpRedirector`
   `ReceivedBytes > 0` при трафике тестового приложения.
2. `Find-NetRoute <dst>` → ifIndex адаптера `TcpRedirector` (а не WireGuard),
   ЕСЛИ сторонний WireGuard выключен ИЛИ его префикс короче нашего /D.
3. В `docker logs 9dd04693840a` появляется `CONNECT` от нашего relay.
ВАЖНО: при одновременно активном стороннем WireGuard с равной/большей глубиной
лестницы победа не гарантирована (см. ограничение в дизайне) — на время теста его
лучше выключить.
