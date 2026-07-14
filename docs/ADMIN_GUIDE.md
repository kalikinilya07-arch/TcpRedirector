# Руководство администратора — TcpRedirector v1.1.0

## Обзор архитектуры

```
Windows
├── TcpRedirectorService.exe (LocalSystem, AutoStart)
│   ├── WinDivert — перехват пакетов
│   ├── TcpRelayServer — HTTP CONNECT к прокси
│   ├── IAuthenticationProvider — аутентификация
│   │   ├── KerberosAgentProvider → AuthAgent
│   │   └── BasicAuthenticationProvider
│   └── PipeServer — IPC (\\.\pipe\TcpRedirectorService)
│
├── TcpRedirectorAuthAgent.exe (User Session, At Logon)
│   └── SSPI/Kerberos в контексте пользователя
│
└── TcpRedirectorGUI.exe (User, без UAC)
    └── Named Pipe Client → только IPC
```

## Роли

| Компонент | Права | Когда запускается |
|---|---|---|
| TcpRedirectorService | LocalSystem | При загрузке Windows |
| TcpRedirectorAuthAgent | Пользователь | При входе пользователя |
| TcpRedirectorGUI | Пользователь | Вручную |

## Установка

См. [`INSTALL.md`](INSTALL.md)

## Конфигурация

Файл: `%ProgramData%\TcpRedirector\config.json`

### Режимы аутентификации (v1.1.0)

```json
{
  "auth": {
    "enabled": true,
    "username": "domain\\user",
    "encryptedPassword": "<DPAPI>",
    "kerberos": true,
    "authMode": "KerberosPreferred"
  }
}
```

| Режим | Описание |
|---|---|
| `KerberosOnly` | Только Kerberos. Ошибка = разрыв соединения |
| `KerberosPreferred` | Kerberos, fallback на Basic (по умолчанию) |
| `BasicOnly` | Только Basic Auth |

## Диагностика

### Служба не запускается

```bat
sc query TcpRedirectorService
:: Проверить STATE

:: Проверить логи
type "%ProgramData%\TcpRedirector\logs\service.log"
```

### WinDivert не работает

```bat
sc query WinDivert
:: Драйвер должен быть RUNNING

:: Переустановить драйвер
"C:\Program Files\TcpRedirector\install_windivert.bat"
```

### GUI не подключается

```bat
:: Проверить Named Pipe
dir \\.\pipe\TcpRedirectorService

:: Проверить ACL
icacls \\.\pipe\TcpRedirectorService
```

### Kerberos не работает

```bat
:: Проверить AuthAgent
dir \\.\pipe\TcpRedirectorAuth

:: Проверить Task Scheduler
schtasks /query /tn "TcpRedirectorAuthAgent"
```

## Обновление

См. [`UPGRADE.md`](UPGRADE.md)

## Удаление

См. [`UNINSTALL.md`](UNINSTALL.md)
