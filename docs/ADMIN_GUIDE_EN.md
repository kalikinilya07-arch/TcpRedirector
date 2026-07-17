# Administrator's Guide — TcpRedirector v1.1.5

## 1. Product Purpose

TcpRedirector is a transparent TCP redirector for Windows 10/11 x64. It intercepts TCP traffic of selected applications at the kernel level (WinDivert) and redirects it through an upstream HTTP(S) proxy with support for Kerberos/Negotiate and Basic authentication.

**Key Features:**
- Kernel-level TCP packet interception (WinDivert) — no application configuration required
- Process-name-based routing rules (e.g., `chrome.exe → proxy`)
- Proxy authentication: Kerberos/Negotiate (SSPI) and Basic
- GUI management console (WPF, .NET 9.0)
- Windows Service (LocalSystem, auto-start)

## 2. Deployment Architecture

```
┌─────────────────────────────────────────────────┐
│ Windows 10/11 x64                                │
│                                                   │
│  ┌──────────────────────┐                        │
│  │ TcpRedirectorService  │ SYSTEM, AutoStart     │
│  │ (C++ service)         │                        │
│  │  ├─ WinDivert         │ packet interception    │
│  │  ├─ TcpRelayServer    │ HTTP CONNECT (34010)   │
│  │  ├─ AuthProvider      │ Kerberos/Basic         │
│  │  └─ PipeServer        │ IPC (named pipe)       │
│  └──────┬───────────────┘                        │
│         │ named pipe                              │
│  ┌──────┴───────────────┐                        │
│  │ TcpRedirectorAuthAgent│ User Session          │
│  │ (C++ agent)           │ SSPI/Kerberos         │
│  └──────────────────────┘                        │
│                                                   │
│  ┌──────────────────────┐                        │
│  │ TcpRedirectorGUI      │ User (non-admin)      │
│  │ (WPF, .NET 9.0)       │ Management via IPC    │
│  └──────────────────────┘                        │
└─────────────────────────────────────────────────┘
```

## 3. System Requirements

| Component | Requirement |
|-----------|-------------|
| OS | Windows 10 21H2+ / Windows 11 (x64) |
| .NET | Windows Desktop Runtime 9.0 (x64) |
| Privileges | Administrator (for driver and service installation) |
| RAM | ~50 MB (service) + ~100 MB (GUI) |
| Disk | ~100 MB |

## 4. Installation

### 4.1. Prerequisites
1. Install [.NET 9.0 Windows Desktop Runtime (x64)](https://dotnet.microsoft.com/download/dotnet/9.0)
2. Have Administrator privileges

### 4.2. Installation Steps

```bat
:: 1. Extract archive to C:\Program Files\TcpRedirector\
:: 2. Install WinDivert driver (once)
cd C:\Program Files\TcpRedirector\bin
install_windivert.bat

:: 3. Install Windows service (once)
install_service.bat

:: 4. Start the service
sc start TcpRedirectorService

:: 5. Auto-start AuthAgent for current user
::    (GUI performs this automatically when Kerberos is enabled)
```

### 4.3. Verification

```bat
:: Check service
sc query TcpRedirectorService
:: STATE should be: 4 RUNNING

:: Check driver
sc query WinDivert
:: STATE should be: 4 RUNNING

:: Check ports
netstat -ano | findstr "34010 34011"
:: 34010 — relay server (TCP)
:: 34011 — IPC (TCP)
```

## 5. Configuration

File: `%ProgramData%\TcpRedirector\config.json`

### 5.1. `proxy` Section

```json
{
  "proxy": {
    "host": "proxy.example.com",
    "port": 3128
  }
}
```

### 5.2. `auth` Section — Proxy Authentication

```json
{
  "auth": {
    "enabled": true,
    "username": "DOMAIN\\user",
    "encryptedPassword": "<DPAPI-encrypted>",
    "kerberos": true,
    "authMode": "KerberosOnly"
  }
}
```

| Parameter | Values | Description |
|-----------|--------|-------------|
| `enabled` | `true`/`false` | Enable authentication |
| `kerberos` | `true`/`false` | Use Kerberos (via AuthAgent) |
| `authMode` | `KerberosOnly`, `KerberosPreferred`, `BasicOnly` | Authentication mode |

**Authentication Modes:**
- `KerberosOnly` — Kerberos/Negotiate only. Failure → connection dropped.
- `KerberosPreferred` — Kerberos with Basic fallback (default).
- `BasicOnly` — Basic Auth only (username/password).

### 5.3. `rules` Section — Routing Rules

```json
{
  "rules": [
    {
      "type": "process",
      "action": "proxy",
      "pattern": "chrome.exe",
      "description": "Google Chrome"
    }
  ]
}
```

| Parameter | Values | Description |
|-----------|--------|-------------|
| `type` | `process`, `path`, `global` | Rule type |
| `action` | `proxy`, `direct`, `block` | Action |
| `pattern` | string | Process name or path |

### 5.4. `log` Section — Logging

```json
{
  "log": {
    "level": 2,
    "directory": "C:\\ProgramData\\TcpRedirector\\logs",
    "max_file_size_mb": 10,
    "max_files": 5
  }
}
```

| Level | Value | When to Use |
|-------|-------|-------------|
| 0 — Trace | Maximum detail | Debugging |
| 1 — Debug | Debug messages | Diagnostics |
| 2 — Info | Informational (default) | Production |
| 3 — Warn | Warnings | Production |
| 4 — Error | Errors only | Production |
| 5 — Off | Logging disabled | — |

## 6. Daily Operations

### 6.1. Starting/Stopping the Service

```bat
sc start TcpRedirectorService
sc stop TcpRedirectorService
```

### 6.2. Viewing Logs

```bat
:: Current log
type "%ProgramData%\TcpRedirector\logs\tcp_redirector.log"

:: Last 100 lines
powershell -Command "Get-Content '%ProgramData%\TcpRedirector\logs\tcp_redirector.log' -Tail 100"

:: Search for errors
findstr /i "error fail" "%ProgramData%\TcpRedirector\logs\tcp_redirector.log"
```

### 6.3. Connection Monitoring

```bat
:: Active connections through redirector
netstat -ano | findstr "34010"
```

## 7. Troubleshooting

### 7.1. Service Won't Start

```bat
:: Check status
sc query TcpRedirectorService

:: Check Windows Event Log
Eventvwr.msc → Windows Logs → System

:: Run in console mode for diagnostics
cd C:\Program Files\TcpRedirector\bin
TcpRedirectorService.exe --console
```

### 7.2. Traffic Not Being Redirected

```bat
:: Check WinDivert driver is loaded
sc query WinDivert

:: Check rules
type "%ProgramData%\TcpRedirector\config.json"

:: Check relay server is listening
netstat -ano | findstr "34010"
```

### 7.3. Kerberos Authentication Not Working

```bat
:: Check AuthAgent is running
tasklist | findstr TcpRedirectorAuthAgent

:: Check agent named pipe
dir \\.\pipe\TcpRedirectorAuth

:: Check agent log
type "%ProgramData%\TcpRedirector\logs\auth_agent.log"

:: Check scheduled task
schtasks /query /tn "TcpRedirectorAuthAgent"

:: Check domain controller availability
nltest /dsgetdc:DOMAIN
```

### 7.4. GUI Cannot Connect to Service

```bat
:: Check service is running
sc query TcpRedirectorService

:: Check service named pipe
dir \\.\pipe\TcpRedirectorService

:: Run GUI as Administrator
```

### 7.5. Slow Performance / Degradation

```bat
:: Check active connection count
netstat -ano | findstr "34010" | find /c "ESTABLISHED"

:: Check service memory usage
tasklist /fi "imagename eq TcpRedirectorService.exe"

:: Restart service
sc stop TcpRedirectorService && sc start TcpRedirectorService
```

## 8. Upgrade

See [`UPGRADE.md`](UPGRADE.md).

Brief steps:
```bat
sc stop TcpRedirectorService
taskkill /F /IM TcpRedirectorAuthAgent.exe
:: Replace .exe files in C:\Program Files\TcpRedirector\bin\
sc start TcpRedirectorService
:: GUI will start AuthAgent automatically
```

## 9. Uninstallation

See [`UNINSTALL.md`](UNINSTALL.md).

```bat
sc stop TcpRedirectorService
sc delete TcpRedirectorService
sc delete WinDivert
taskkill /F /IM TcpRedirectorAuthAgent.exe
schtasks /delete /tn "TcpRedirectorAuthAgent" /f
rmdir /s /q "C:\Program Files\TcpRedirector"
del /f "%ProgramData%\TcpRedirector\config.json"
```

## 10. Changelog

See [`CHANGELOG.md`](CHANGELOG.md).