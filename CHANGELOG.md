# TcpRedirector v1.1.1

## Ключевые изменения

### Исправления (v1.1.1)

#### Сохранение конфигурации
- **SetProxyConfig**: исправлено сохранение флага `kerberos` в config.json
- **SetProxyConfig**: пароль больше не очищается при сохранении без повторного ввода (`set_password=false`)
- **IpcHandler::SetConfig**: теперь проверяет результат `SetProxyConfig` и `Save`, возвращает ошибку при провале

#### Стабильность AuthAgent
- **KerberosAgentProvider**: авто-запуск AuthAgent через `CreateProcessAsUser` (WTS API) при недоступности pipe
- **LaunchAuthAgentInUserSession**: полное логирование всех точек отказа (session, token, duplicate, create process) с кодами ошибок
- **DuplicateTokenEx**: `TOKEN_ALL_ACCESS` → `TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY` (минимальные права)
- **Динамический путь**: `GetModuleFileNameW` вместо хардкода `C:\Program Files\TcpRedirector\`
- **KeepaliveLoop**: теперь тоже вызывает `LaunchAuthAgentInUserSession` при обрыве соединения (не только `EnsureConnected`)

#### Устойчивость прокси-соединений
- **TcpRelayServer**: retry при 504/502 Gateway Error (до 3 попыток с пересозданием сокета)
- **TcpRelayServer**: rate-limit предупреждений "Auth provider failed" — не чаще 1 раза в 30 секунд

#### Инсталлятор
- TcpRedirectorAuthAgent.exe включён в пакет, добавлена верификация после копирования

---

# TcpRedirector v1.1.0

## Ключевые изменения (v1.1.0)

### Non-Admin GUI
- GUI больше не требует прав администратора для запуска
- Служба запускается автоматически (Windows Service, StartupType=Automatic)
- GUI — исключительно IPC-клиент, не управляет службой
- Named Pipe ACL изменён на Everyone (Read/Write)

### Kerberos через AuthAgent
- Новый компонент `TcpRedirectorAuthAgent.exe` — SSPI/Kerberos в пользовательской сессии
- `IAuthenticationProvider` — абстракция аутентификации (CreateContext/ContinueContext/CloseContext)
- `KerberosAgentProvider` — получение токенов через AuthAgent
- `BasicAuthenticationProvider` — fallback/изолированный
- `AuthenticationMode`: KerberosOnly, KerberosPreferred, BasicOnly
- Security Context живёт ровно один HTTP CONNECT (одноразовый)

### Новые IPC-команды
- `ping` — проверка доступности службы
- `get_status` — расширенный статус (service_state, driver_loaded, capture_enabled, connections)
- `get_version` — версия службы

### Инсталлятор
- Inno Setup Installer с поддержкой Install/Upgrade/Repair/Uninstall
- Автоматическая установка и настройка службы
- Task Scheduler для AuthAgent (At Logon)
- Backup config.json при обновлении

### Удалено
- ServiceController.cs (GUI больше не управляет службой)
- IServiceController интерфейс
- StartService/StopService в ShellViewModel
- WriteFull/WriteInt из JsonConfigRepository (GUI не пишет config.json)

### Исправления
- Исправлена опечатка `WinDivirt` → `WinDivert` в install_windivert.bat
- TcpRelayServer больше не содержит SSPI-логики
