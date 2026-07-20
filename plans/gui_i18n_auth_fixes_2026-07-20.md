# План: локализация GUI (RU/EN), фикс сохранения пароля, скрытие полей при Kerberos

**Дата:** 2026-07-20
**Область:** WPF GUI `TcpRedirectorGUI` (.NET 9) + минимальная правка службы `ConfigManager` (C++20).
**Автор плана:** Architect mode.

---

## Контекст (из изучения кода)

- GUI — единственный штатный писатель `config.json` ([`JsonConfigRepository.WriteFullV2`](../src/gui/TcpRedirectorGUI/Infrastructure/Config/JsonConfigRepository.cs:356)); служба дополнительно шифрует+сохраняет пароль через DPAPI после IPC-push (B4).
- Строки интерфейса **захардкожены** и **смешаны** (часть EN: `PROXY SETTINGS`; часть RU: `РЕЖИМ ЗАХВАТА ТРАФИКА`, `Сохранить всё`). Инфраструктуры локализации **нет** (в [`App.xaml`](../src/gui/TcpRedirectorGUI/App.xaml:1) только конвертеры и тема).
- Секция аутентификации ([`MainWindow.xaml:163-217`](../src/gui/TcpRedirectorGUI/MainWindow.xaml:163)) показывает Login + Password + чекбоксы Kerberos/«Шифровать пароль» вместе, видимость завязана только на `Settings.AuthRequired`.
- Пользовательские строки также в ViewModels: [`SettingsViewModel`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs:1) (валидация, `Msg`), [`ShellViewModel`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/ShellViewModel.cs:1) (`SvcStatus`, `SvcMsg`, `StatusText`), `StatsViewModel`, `TraceViewModel`.

---

## Задача 2 — Очистка неактивных полей auth при пересохранении

### Корень проблемы
В [`WriteFullV2`](../src/gui/TcpRedirectorGUI/Infrastructure/Config/JsonConfigRepository.cs:394) логика очистки `password`/`encryptedPassword` выполняется **только внутри** `if (!string.IsNullOrEmpty(config.Password))`. Если пользователь не вводит новый пароль (поле пустое после прошлого сохранения), при:
- переключении `encryptPassword` true↔false — противоположное поле НЕ чистится;
- переключении Basic↔Kerberos — `username`/`encryptedPassword`/`password` остаются;
- отключении auth (`enabled=false`) — секреты остаются.

### Решение (GUI)
В [`WriteFullV2`](../src/gui/TcpRedirectorGUI/Infrastructure/Config/JsonConfigRepository.cs:394) вынести нормализацию auth-полей в отдельный блок, выполняемый **всегда**, по правилам:
- `AuthRequired == false` → очистить `username=""`, `encryptedPassword=""`, `password=""`.
- `AuthRequired && KerberosEnabled` → очистить `username=""`, `encryptedPassword=""`, `password=""` (Kerberos через SSPI, креды не хранятся).
- `AuthRequired && !Kerberos && encryptPassword=true` → всегда `password=""`; `encryptedPassword` обновить, если введён новый пароль, иначе сохранить существующий.
- `AuthRequired && !Kerberos && encryptPassword=false` → всегда `encryptedPassword=""`; `password` обновить, если введён новый, иначе сохранить существующий.

### Решение (служба, для консистентности)
В [`ConfigManager::SetProxyConfig`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:160) и путях сохранения ([`SaveToFile`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:493)) применить те же правила очистки, чтобы после IPC-сохранения пароля служба не восстанавливала неактивные поля. Не менять поведение расшифровки, только зачистку неактуальных полей при записи.

### Файлы
- [`JsonConfigRepository.cs`](../src/gui/TcpRedirectorGUI/Infrastructure/Config/JsonConfigRepository.cs) — `WriteFullV2`.
- [`ConfigManager.cpp`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp) — `SetProxyConfig` / сохранение (консистентная зачистка).
- Проверка: [`SettingsViewModel.SaveAllAsync`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs:655) передаёт корректные `KerberosEnabled`/`AuthRequired`/`EncryptPassword` (уже так).

---

## Задача 3 — Скрытие Login/Password при Kerberos

### Решение
- Добавить в [`SettingsViewModel`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs) вычисляемое свойство `ShowBasicCredentials => AuthRequired && !KerberosEnabled`.
- Уведомлять о его изменении в [`OnKerberosEnabledChanged`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs:287) и [`OnAuthRequiredChanged`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs:293). Добавить имя в `s_nonContentProps` (не «контентное»).
- В [`MainWindow.xaml`](../src/gui/TcpRedirectorGUI/MainWindow.xaml:169) реструктурировать Grid секции auth: чекбокс «Use Kerberos» остаётся всегда видимым (когда AuthRequired), а Login (label+textbox), Password (label+box) и чекбокс «Шифровать пароль» обернуть в контейнер с `Visibility="{Binding Settings.ShowBasicCredentials, Converter={StaticResource BoolToVis}}"`.

### Файлы
- [`SettingsViewModel.cs`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs), [`MainWindow.xaml`](../src/gui/TcpRedirectorGUI/MainWindow.xaml).

---

## Задача 1 — Переключатель RU/EN + полная локализация

### Подход (runtime-переключение без перезапуска)
1. **Строковые ресурсы** — две `ResourceDictionary`: `Strings.ru.xaml` и `Strings.en.xaml` с одинаковым набором ключей (`x:Key`), значения — `sys:String`. Разместить в `Adapters/Driving/Wpf/Localization/`.
2. **LocalizationService** (`Infrastructure/Localization/LocalizationService.cs`):
   - Хранит текущий язык; метод `SetLanguage(ru|en)` подменяет merged-словарь строк в `Application.Current.Resources.MergedDictionaries`.
   - Статический доступ `Loc.Get(key)` для VM-строк (валидация, статусы) из текущего словаря.
   - Реализует `INotifyPropertyChanged` через индексер (`this[key]`) для DynamicResource-независимых биндингов при необходимости.
3. **XAML** — все `Text="..."`, `Content="..."`, `Header="..."`, `ToolTip="..."` перевести на `{DynamicResource Loc.<Key>}` (DynamicResource обновляется при подмене словаря во время выполнения).
4. **ViewModels** — заменить захардкоженные RU-строки (`"✓ Сохранено"`, валидационные сообщения, `SvcMsg`/`SvcStatus`, и т.п.) на `Loc.Get("...")`. Учесть, что статусы служебного бара (`Stopped`/`Running`/`Started`) тоже локализуются.
5. **Переключатель языка** — `ComboBox` (RU/EN) в service-bar ([`MainWindow.xaml:51-97`](../src/gui/TcpRedirectorGUI/MainWindow.xaml:51)), привязан к `Language`-свойству в `ShellViewModel`, вызывающему `LocalizationService.SetLanguage`.
6. **Персист выбора** — сохранять язык в `config.json` под GUI-ключом (напр. `gui.language`), merge-writer его сохранит; служба игнорирует неизвестные ключи. Читать при старте в [`ShellViewModel`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/ShellViewModel.cs:26)/`LoadFromConfig`. Дефолт — `ru` (текущее де-факто поведение).
7. **DI** — зарегистрировать `LocalizationService` в [`App.xaml.cs`](../src/gui/TcpRedirectorGUI/App.xaml.cs:15); в `App.OnStartup` установить язык из конфига ДО показа окна; подключить дефолтный словарь в [`App.xaml`](../src/gui/TcpRedirectorGUI/App.xaml:10).
8. **csproj** — при необходимости явно включить новые `.xaml`/`.cs` (обычно авто-включаются SDK-стилем).

### Объём перевода
Полный перевод всех секций: PROXY SETTINGS, AUTHENTICATION, РЕЖИМ ЗАХВАТА ТРАФИКА (+ WinDivert/Wintun подписи), ПРИЛОЖЕНИЯ (+ колонки DataGrid, кнопки), ТРАССИРОВКА (+ колонки), ДОПОЛНИТЕЛЬНО (IPC), LOG LEVEL, STATISTICS, статус-бар, все сообщения ViewModels и валидации. Обеспечить единообразие: сейчас часть UI на EN — привести RU-словарь к полностью русскому, EN-словарь к полностью английскому.

### Файлы
- Новые: `Strings.ru.xaml`, `Strings.en.xaml`, `LocalizationService.cs` (+ `Loc` helper).
- Правки: [`App.xaml`](../src/gui/TcpRedirectorGUI/App.xaml), [`App.xaml.cs`](../src/gui/TcpRedirectorGUI/App.xaml.cs), [`MainWindow.xaml`](../src/gui/TcpRedirectorGUI/MainWindow.xaml), [`ShellViewModel.cs`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/ShellViewModel.cs), [`SettingsViewModel.cs`](../src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs), `StatsViewModel.cs`, `TraceViewModel.cs`, при необходимости [`TcpRedirectorGUI.csproj`](../src/gui/TcpRedirectorGUI/TcpRedirectorGUI.csproj).

---

## Порядок выполнения и делегирование

Все задачи — Code mode. Порядок выбран так, чтобы локализация делалась ПОСЛЕ реструктуризации auth-секции (иначе строки/контролы придётся переводить дважды):

1. **Subtask A (code):** Задача 2 + Задача 3 (обе затрагивают auth: `WriteFullV2`, `SettingsViewModel`, `MainWindow.xaml` auth-секцию, `ConfigManager.cpp`).
2. **Subtask B (code):** Задача 1 — инфраструктура локализации + словари RU/EN + переключатель + перевод всех строк.
3. **Сборка/проверка:** `dotnet build -c Release` (GUI) и `msbuild ... Release/x64` (служба).
4. **Документация:** отразить переключатель языка и корректную зачистку auth-полей в `README.md`/`docs/КОНФИГУРАЦИЯ.md`.

---

## Критерии приёмки

- **Задача 2:** после смены `encryptPassword` / Basic↔Kerberos / отключения auth без ввода пароля — в `config.json` неактивные поля (`username`/`encryptedPassword`/`password` по правилам) очищены; активный пароль не теряется.
- **Задача 3:** при включённом Kerberos поля Login/Password и чекбокс «Шифровать пароль» скрыты; при выключении — снова видны; чекбокс Kerberos всегда доступен (когда auth включён).
- **Задача 1:** переключатель RU/EN в GUI меняет весь интерфейс (включая сообщения VM) на лету; выбор сохраняется между запусками; оба языка полностью переведены и консистентны.
- Сборка GUI и службы — без ошибок.
