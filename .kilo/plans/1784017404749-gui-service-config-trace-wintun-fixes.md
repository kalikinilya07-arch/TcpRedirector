# TcpRedirector — план исправлений: config-путь, Save all, валидация, трассировка, Wintun

Дата: 2026-07-14. Основано на анализе `README.md`, `docs/*`, `docs/ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md`
и исходников службы (C++20) и GUI (WPF/.NET 9).

> Ограничение окружения: полноценный рантайм-тест (админ-права, `WinDivert64.sys`,
> `wintun.dll`, `tun2socks.exe`, реальная сеть) в среде разработки недоступен. Для
> каждого пункта дан ручной чек-лист проверки на стенде + сборочные проверки.

Реализация выполняется агентом с правами на правку исходников (C++ + C#). Этот
агент (планировщик) исходники не меняет.

---

## Сводка подтверждённых проблем

| # | Задача | Root cause (файл) | Статус |
|---|--------|-------------------|--------|
| 1 | Путь config.json | Служба — OK (`AppPaths::GetConfigPathW`, `ConfigManager()` exe-dir). GUI — расходящийся резолвер: предпочитает `<gui>\config.json` (`AppPaths.cs:52-76`) | Исправить GUI |
| 2 | Save all + сброс режима | Единственная кнопка «Сохранить» в секции «ПРИЛОЖЕНИЯ»; `LoadFromConfig()` в Start/Stop затирает несохранённые правки (`ShellViewModel.cs:97,146`); нет verify после записи | Исправить GUI |
| 3 | Валидация перед сохранением | Валидируется только `PortSpec` (`SettingsViewModel.AreAppsValid`); host/port/CIDR/MTU/socks5/adapter_name не проверяются | Расширить GUI |
| 4 | Трассировка пакетов | `get_connections` есть в IPC, но GUI не опрашивает его, нет UI, `ConnectionsUpdated` не вызывается; трасса заполняется только в WinDivert (`WinDivertCapture.cpp:487`), в Wintun — пусто; `GetConnectionsAsync` падает на `.GetProperty`; график статистики скачет на первом сэмпле и при сбросе счётчиков | Исправить GUI + служба |
| 5 | Перехват в Wintun | Критические баги стека: `netif_set_ipaddr` рвёт параллельные flow (`lwip_hooks_impl.c`), нет bypass-маршрута к прокси/DNS (петля), DNS/UDP чёрная дыра, таймеры lwIP голодают (блокирующий `ReceiveInto`), next-hop маршрута = собственный IP | Исправить службу |

---

## Задача 1 — config.json из папки установки во всех режимах

### Что уже корректно (не трогать)
- Служба: `ConfigManager()` (`ConfigManager.cpp:31-36`) резолвит путь через
  `paths::GetConfigPathW()` = `GetModuleFileNameW` → `<exeDir>\config.json`.
  `ServiceMain::Initialize` использует дефолтный ctor (`ServiceMain.h:97`). Ни
  `GetCurrentDirectory`, ни `SetCurrentDirectory`, ни `_pgmptr` не используются.
  Работает одинаково для `--console` и SCM, для windivert и wintun. Fallback на
  `%ProgramData%` удалён; миграция — разовая (`EnsureConfigMigrated`, только если
  целевого файла нет).

### Проблема (GUI)
`Infrastructure/Config/AppPaths.cs:52-76` `GetConfigPath()` сначала проверяет
`<BaseDir>\config.json` (= `...\gui\config.json`) и только потом родительский
каталог (install root, где лежит служба). При установленной раскладке служба
всегда читает `<installRoot>\config.json`, а GUI при наличии/создании файла в
`gui\` начнёт писать в другой файл → GUI и служба расходятся (это же — вероятная
причина «сохранение режима не подхватывается»).

### Изменения
1. В `AppPaths.cs` сделать **install root авторитетным**, зеркально службе:
   - Определять каталог службы так же, как `ShellViewModel.FindBackendExe`
     (`ShellViewModel.cs:274-295`): искать `TcpRedirectorService.exe` рядом и на
     уровень выше; каталог найденного EXE = install root.
   - `GetConfigPath()`:
     - если служба найдена → `<serviceDir>\config.json` (единственный источник истины);
     - иначе (dev/portable, служба рядом) → `<BaseDir>\config.json`;
     - **убрать приоритет `<gui>\config.json`**, если существует родительский
       (install) `config.json`. Порядок: serviceDir → parent → base.
2. Добавить публичный `GetResolvedConfigPathForDisplay()` (или свойство), чтобы
   задача 2 могла показать пользователю фактический путь и делать read-back.
3. Не менять `%ProgramData%`-миграцию — оставить как one-shot.

### Проверка
- Установленная раскладка (`Program Files\TcpRedirector\` + `gui\`): GUI и служба
  используют один `<installRoot>\config.json`. Подложить фиктивный
  `gui\config.json` — GUI его **не** должен предпочесть.
- Dev-раскладка (GUI и служба в одном каталоге `build\`): используется локальный.
- `git grep -n "GetCurrentDirectory\|SpecialFolder\|CurrentDirectory"` в GUI — не
  должно остаться путей в `%AppData%`/`%ProgramData%`/cwd для основного config.

---

## Задача 2 — кнопка «Save all» + фикс сброса режима + verify записи

### Root cause сброса режима (подтверждено статически)
- Единственная кнопка сохранения — «Сохранить» в секции «ПРИЛОЖЕНИЯ»
  (`MainWindow.xaml:308`), вызывает `SaveAsyncCommand`. `capture_mode`/`wintun`
  пишутся в `WriteFullV2` симметрично для обоих режимов — асимметрии в самой
  записи нет.
- Реальная причина «переключение на wintun сохраняется, а обратно — нет»:
  `ShellViewModel.StartService`/`StopService` вызывают `Settings.LoadFromConfig()`
  (`ShellViewModel.cs:97,146`), который **перечитывает `capture_mode` с диска и
  затирает несохранённый выбор пользователя** в комбобоксе. Плюс расхождение
  путей из Задачи 1: запись могла уходить в `gui\config.json`, а служба читала
  install-root. Нет проверки, что записанное значение действительно на диске.

### Изменения (GUI)
1. **Кнопка «Save all»** на видном месте (например, в сервис-баре сверху,
   `MainWindow.xaml` Grid.Row=0, или отдельной секцией над «ПРИЛОЖЕНИЯ»).
   Привязать к новой команде `SaveAllCommand` в `SettingsViewModel`, которая
   сохраняет **все** секции: proxy, auth (+пароль через IPC), capture_mode,
   wintun (вкл. `process_filter_enabled`, external_engine), apps, log level.
2. `SaveAllAsync`:
   - Запуск полной валидации (Задача 3) — при ошибках прервать с сообщением.
   - `WriteFullV2(...)` (уже пишет `capture_mode` и все поля wintun).
   - **Verify (read-back):** после `Save` перечитать файл по тому же пути
     (`AppPaths.GetConfigPath`) и сравнить ключевые поля (`capture_mode`,
     `wintun.engine`, `proxy.host/port`, число `apps`) с тем, что записывали.
     Показать в `Msg`: `✓ Сохранено в <путь>` или
     `✗ Проверка не пройдена: на диске <факт> вместо <ожид>`.
   - Сообщать явные ошибки записи (сейчас `catch` глушит в общий текст) — включить
     `ex.Message` и путь.
3. **Не затирать несохранённые правки:** в `ShellViewModel.StartService`/`StopService`
   убрать безусловный `LoadFromConfig()` или добавить флаг «есть несохранённые
   изменения» (dirty). Варианты:
   - отслеживать dirty в `SettingsViewModel` (любой сеттер → `IsDirty=true`,
     сброс после успешного Save);
   - при перезагрузке из-за Start/Stop, если `IsDirty` — не перечитывать, либо
     спросить пользователя.
4. Отметить визуально несохранённые изменения (например, звёздочка/подсветка
   кнопки Save all), чтобы поведение было предсказуемым.
5. Проверить, что `process_filter_enabled` и `maxSizeMB` сохраняются корректно
   (сейчас `WintunToJson` пишет `process_filter_enabled` — ок; `maxSizeMB=50`
   хардкод — оставить 50, синхронно с `Config.h`).

### Проверка
- Переключить windivert→wintun, Save all → verify OK; перезапустить службу;
  вернуться wintun→windivert, Save all → verify OK; убедиться, что после
  Start/Stop выбор не «прыгает» обратно.
- Симулировать недоступность файла (снять права на запись) → GUI показывает
  явную ошибку с путём, а не «тихо сохранил».
- Открыть `config.json` на диске и сверить `capture_mode` с выбором в GUI.

---

## Задача 3 — валидация всех настроек перед сохранением

### Текущее состояние
Валидируется только `PortSpec` каждой строки приложения (`AppRuleViewModel`
реализует `INotifyDataErrorInfo`; стиль `PortSpecTextBox` подсвечивает и даёт
tooltip). `AreAppsValid` блокирует Save только по портам. Остальные поля — без
проверки.

### Изменения (GUI)
1. `SettingsViewModel` реализует `INotifyDataErrorInfo` (или ввести per-field
   валидацию) для:
   - **proxy.host** — непустой; валидный hostname/IPv4/IPv6.
   - **proxy.port** — целое 1..65535.
   - **auth**: при Basic (не Kerberos) и `AuthRequired` — `Login` непустой;
     предупреждение, если пароль не задан и на диске нет `encryptedPassword`.
   - **wintun.adapter_name** — непустой (иначе preflight fail).
   - **wintun.tunnel_ipv4_cidr** — формат `a.b.c.d/N`, `N`∈[0..32]
     (зеркалит `WintunPreflight`/`ConfigManager`).
   - **wintun.mtu** — [576..65535].
   - **external_engine.socks5_listen** (при engine=external) — `host:port`,
     порт∈[1..65535]; **executable** — непустой.
   - **apps** — существующая проверка портов; плюс предупреждение, когда
     `route_all_traffic=false` и порты пусты (правило ничего не отматчит).
2. Привязать каждый проблемный TextBox к стилю с подсветкой (обобщить
   `PortSpecTextBox` в общий `ValidatedTextBox` в `MainWindow.xaml`/`AppTheme.xaml`)
   с красной рамкой + tooltip = текст ошибки; для CIDR/MTU/socks5 добавить
   `ValidatesOnNotifyDataErrors=True, NotifyOnValidationError=True`,
   `UpdateSourceTrigger=PropertyChanged`.
3. Рядом с полем — краткая подсказка проблемы (tooltip уже есть; при желании —
   `TextBlock` с текстом ошибки под полем, скрытый до появления ошибки).
4. `SaveAllCommand.CanExecute` / начало `SaveAllAsync` — **блокировать сохранение**,
   пока есть любые ошибки; кнопка Save all неактивна при `HasErrors`.
5. Валидацию делать регистронезависимой к режиму: поля wintun валидируются, только
   когда `CaptureMode=Wintun`; external — только при engine=external.

### Проверка
- Ввести некорректный порт прокси / пустой adapter_name / кривой CIDR / плохой
  socks5 → поле краснеет, tooltip с описанием, Save all заблокирован.
- Исправить → ошибки исчезают, Save all доступен, verify проходит.

---

## Задача 4 — блок трассировки перенаправляемых пакетов

### Root causes (подтверждено)
1. **Не опрашивается и не отображается.** `PollLoopAsync`
   (`ShellViewModel.cs:313-349`) вызывает только `GetStatsAsync` и
   `GetServiceStatusAsync`; `GetConnectionsAsync` не вызывается нигде;
   `ConnectionsUpdated` объявлен, но не вызывается; в `MainWindow.xaml` нет
   таблицы соединений/трассы (только график и сводка). Т.е. «блок трассировки»
   фактически не подключён.
2. **Пусто в Wintun.** Per-connection записи (`ConnectionTracker::AddConnection`)
   добавляются только в `WinDivertCapture.cpp:487`. `WintunCapture` их не
   заполняет → `get_connections` в wintun-режимах отдаёт пустой список
   (счётчик `active_connections` при этом ненулевой — берётся из
   `engine->ActiveFlows()`), отсюда «нестабильность»/расхождение.
3. **Хрупкий парсинг.** `IpcClient.GetConnectionsAsync` (`IpcClient.cs:153-178`)
   и часть `GetStatsAsync` используют `.GetProperty(...)` без `TryGetProperty` —
   отсутствие любого поля бросает исключение → весь список гасится в `catch`
   (пустая трасса, мигание).
4. **Скачки графика.** `StatsViewModel.PushStats` (`StatsViewModel.cs:88-99`):
   на первом сэмпле `_prevRx=0` → дельта = полный total (пик); после
   рестарта/сброса счётчиков службы `TotalRxBytes` уменьшается → `ulong`-дельта
   underflow → гигантский ложный всплеск; `dt` считается «≈1с», хотя интервал
   опроса берётся из конфига и может отличаться.

### Изменения (GUI + служба)
GUI:
1. Ввести VM трассы (например `TraceViewModel`) c `ObservableCollection<ConnectionRecord>`;
   в `PollLoopAsync` вызывать `GetConnectionsAsync`, обновлять коллекцию через
   `Dispatcher` (обновлять по `Id`, не пересоздавать список целиком — меньше
   мигания). При `!IsConnected` — очищать/замораживать без ошибок.
2. Добавить в `MainWindow.xaml` секцию/таблицу трассы: PID, процесс, назначение
   (`DisplayDestination`), порт, состояние, RX/TX, длительность, «через прокси».
3. `IpcClient.GetConnectionsAsync`/`GetStatsAsync`: заменить `.GetProperty` на
   `TryGetProperty` с дефолтами (не терять весь батч из-за одного поля).
4. `StatsViewModel.PushStats`: убрать стартовый пик (первый сэмпл — только
   инициализация `_prev*`, без точки или с нулём); при `TotalRxBytes < _prevRx`
   (сброс) — трактовать дельту как 0/рестарт; нормировать rate на фактический
   `dt`, а не на «1с».
5. При смене режима/рестарте службы сбрасывать состояние трассы и `_prev*`.

Служба (чтобы трасса работала и в Wintun):
6. Заполнять `ConnectionTracker` per-connection записями в Wintun. Минимальный
   вариант — в `Tun2SocksEngineEmbedded` при accept'е flow (там уже есть
   `dst_ip:port`, резолв PID/процесса для фильтрации) вызывать
   `AddConnection(rec)`/обновлять байты/удалять при закрытии, по аналогии с
   `WinDivertCapture`. Для `external` — задокументировать ограничение (нет
   per-connection данных; см. `ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md` 3.9) и показывать в трассе
   пустой список с пояснением, а не «мигание».

### Проверка
- WinDivert: трасса заполняется активными соединениями целевого приложения,
  обновляется без мигания; при остановке службы очищается без ошибок.
- Wintun embedded: соединения появляются в трассе (после п.6); счётчики RX/TX
  растут; график не даёт стартового/рестартового пика.
- Отключить поле в ответе службы (тест) — GUI не роняет весь список.

---

## Задача 5 — корректность перехвата в Wintun-режимах

Полный анализ стека (subagent) выявил критические баги (embedded lwIP). Порядок
исправления — по влиянию.

### C1 (critical). `netif_set_ipaddr` рвёт параллельные flow
- Файл: `.../capture/wintun/lwip/lwip_hooks_impl.c` (IP4-input hook, ~строки 48-50):
  используется `netif_set_ipaddr(input_netif, &new_ip)` для «catch-all» подмены
  dst. В lwIP 2.2.0 смена адреса вызывает `tcp_netif_ip_addr_changed` →
  `tcp_abort()` всех PCB со старым `local_ip`. При параллельных соединениях к
  разным IP flow'ы взаимно абортятся; выживает трафик к одному dst за раз (одиночный
  smoke-тест это маскирует).
- Фикс: писать поле напрямую (безопасно при `NO_SYS=1`, один engine-поток):
  `ip4_addr_copy(*ip_2_ip4(&input_netif->ip_addr), iphdr->dest);` — не вызывать
  `netif_set_ipaddr`.

### C2 (critical). DNS/UDP — чёрная дыра (embedded)
- `/1`-маршруты (`RouteInstaller.cpp`) захватывают весь IPv4, включая UDP/53, но
  embedded lwIP собран с `LWIP_UDP 0` (`lwipopts.h:49`) и не обрабатывает DNS →
  резолв имён падает, «ничего не работает».
- Фикс (один из): bypass-маршрут `/32` на DNS-сервер(ы) через прежний физический
  шлюз; **или** локальный DNS-форвардер; **или** включить UDP+DNS в движке.
  Рекомендация: bypass-маршруты на текущие DNS до установки `/1`.

### C3 (critical). Нет bypass к прокси/direct → петля
- `/1`-маршруты захватывают всё без исключений. Исходящее соединение relay к
  вышестоящему прокси (`m_proxyHost:m_proxyPort`) и «Direct»-сокеты движка
  (`Tun2SocksEngineEmbedded.cpp:247-251`) сами попадают в туннель → рекурсия/
  амплификация; с удалённым прокси наружу ничего не выходит. Loop-protection по
  PID неэффективна, т.к. маршруты перехватывают независимо от процесса.
- Фикс: до установки `/1` добавить host-route `/32` к IP прокси (и DNS) через
  существующий дефолтный шлюз/физический интерфейс; и/или биндить upstream-сокет
  relay и Direct-сокеты движка к адресу физического интерфейса.

### H1 (high). Голодание таймеров lwIP (блокирующий `ReceiveInto`)
- `WintunSession::ReceiveInto` (`WintunSession.cpp:139-141`) при пустом кольце
  делает `WaitForMultipleObjects(..., INFINITE)` и возвращает `<=0` только на
  stop/error. В `Tun2SocksEngineEmbedded` (`~554-579`) внутренний `for(;;)` при
  этом никогда не выходит в штатном трафике → `sys_check_timeouts()` и внешний
  500 мс `WaitForMultipleObjects` фактически мёртвы → нет ретрансмиссий/
  delayed-ACK → соединения зависают.
- Фикс: добавить неблокирующий `TryReceiveInto` (возврат 0 при
  `ERROR_NO_MORE_ITEMS`) для дренажа кольца; ожидание/периодику отдать внешнему
  `WaitForMultipleObjects({readEvent, stop, timer}, 500)` с регулярным
  `sys_check_timeouts()`.

### M1 (medium). Next-hop маршрута = собственный IP адаптера
- `tunnel_ipv4_cidr` (по умолчанию `10.6.7.1/24`) используется и как адрес
  адаптера (`WintunAdapter.cpp:~236`), и как next-hop `/1`-маршрутов
  (`RouteInstaller.cpp:62-64`). Для L3-адаптера Wintun (без ARP) корректнее
  on-link маршрут: `NextHop = 0.0.0.0` с LUID туннеля. Иначе доставка пакетов в
  TUN может не происходить вовсе. **Проверить в первую очередь** — если резолв
  next-hop не удаётся, перехвата нет совсем.

### M2 (medium). Маршруты ставятся до подъёма интерфейса
- Маршруты создаются (`WintunCapture.cpp:~222-234`) до `WintunSession::Start`
  (`~240-254`). Интерфейс до старта сессии «down» → маршруты могут быть неактивны.
  Особенно критично для **external** (службой сессия не открывается вовсе — её
  открывает `tun2socks.exe`): окно, когда трафик утекает мимо туннеля.
- Фикс: поднимать сессию/интерфейс до установки `/1`-маршрутов; для external —
  либо кратковременная сессия для link-up, либо отложить маршруты до сигнала от
  дочернего процесса.

### M3 (medium). Блокирующие connect/send под `m_core_lock`
- `OnAccept` (`Tun2SocksEngineEmbedded.cpp:254-263`, blocking `::connect`) и
  `OnRecv` (`:315`, blocking `::send`) выполняются в `tcp_input` под
  `m_core_lock` → на время setup/переполнения буфера простаивает весь стек
  (см. также `ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md` 3.1). Плюс `Sleep(20)` при промахе PID под
  локом.
- Фикс: неблокирующие сокеты; вынести `connect`/back-pressure `send` и резолв PID
  из-под core-lock (accept → отложенный `DecideFlow`/`connect` в отдельном
  потоке).

### Дополнительно (низкий приоритет / документировать)
- IPv6 не перехватывается (нет `::/1`-маршрутов, `LWIP_IPV6 0`) — на dual-stack
  часть трафика уходит мимо (Happy Eyeballs). Задокументировать/добавить `::/1`.
- MTU интерфейса Windows-стороны не выставляется (`WintunAdapter.ConfigureIpv4`);
  сейчас совпадает с дефолтом 1500 — синхронизировать с `wintun.mtu`.
- `ERR_WOULDBLOCK` на записи в TUN (`:209-214`) теряет сегмент безвозвратно
  (усугубляется H1) — восстановить после фикса таймеров.

### Проверка (стенд, админ + `wintun.dll`)
- Embedded: адаптер `TcpRedirector` поднят, IPv4 из CIDR, `/1`-маршруты активны;
  параллельно открыть много соединений к разным хостам (браузер) — соединения не
  рвутся (C1); резолв DNS работает (C2); удалённый прокси достижим, нет петли
  (C3); нет зависаний/таймаутов под нагрузкой (H1); в логах видно PROXY/DIRECT/
  BLOCK по `apps[]`.
- Снять дампы маршрутов (`route print`) — проверить bypass `/32` к прокси/DNS и
  on-link `/1`.
- External: интерфейс поднят до появления трафика (M2); трафик идёт через
  SOCKS5→HTTP-прокси; авто-рестарт `tun2socks.exe`.

---

## Порядок выполнения

1. **Задача 1** (GUI config path) — фундамент для Задачи 2 (verify пишет туда же,
   куда читает служба).
2. **Задача 3** (валидация) — нужна как гейт для Save all.
3. **Задача 2** (Save all + verify + фикс dirty/reload) — опирается на 1 и 3.
4. **Задача 4** (трассировка) — GUI-опрос/UI + заполнение трекера в Wintun.
5. **Задача 5** (Wintun-стек) — самостоятельный крупный блок; начать с M1
   (проверка доставки пакетов), затем C1/C2/C3, затем H1/M2/M3.

## Сборка и общая проверка
- Служба: `MSBuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64`.
- GUI: `dotnet build TcpRedirectorGUI.csproj -c Release`.
- Юнит-тесты: `tests/unit/service/RuleEngineTest.cpp` (должно оставаться 16/16).
- Ручные чек-листы — по каждому пункту выше (см. также
  `docs/ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md` §6).
- После правок обновить `docs/ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md` (закрыть 3.5/3.6 и связанные
  находки Wintun).

## Открытые вопросы (уточнить при реализации)
- Задача 4: восстановить полноценную таблицу трассы соединений, или достаточно
  улучшить существующий график + сводку? (План закладывает таблицу.)
- Задача 5/C2-C3: способ bypass DNS/прокси — статические `/32`-маршруты
  (рекомендуется) vs bind-to-interface. Требует выбора при реализации.
- Задача 2: поведение при несохранённых изменениях во время Start/Stop —
  молча не перезагружать vs спрашивать пользователя.
