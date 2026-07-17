# Application Architecture — TcpRedirector v1.1.5

## 1. Overall Architecture

TcpRedirector implements the "Hexagonal Architecture" (Ports & Adapters) pattern with clear layer separation:

```
┌─────────────────────────────────────────────────────────┐
│                    GUI (WPF .NET 9.0)                    │
│  Adapters/Driving/Wpf/                                  │
│    ├── ViewModels/  (MVVM: ShellViewModel, SettingsVM)  │
│    └── Controls/    (TrafficGraph)                      │
│  Infrastructure/                                         │
│    ├── Ipc/         (IpcClient → named pipe)            │
│    └── Config/      (JsonConfigRepository)              │
├─────────────────────────────────────────────────────────┤
│              C++ Service (TcpRedirectorService.exe)      │
│                                                          │
│  ┌─ adapters/driving/ ─────────────────────────────┐    │
│  │  ServiceMain.h     (Windows Service lifecycle)   │    │
│  │  IpcHandler.h      (JSON-RPC over named pipe)    │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─ domain/ports/ ─────────────────────────────────┐    │
│  │  ICapture.h            (packet capture interface)│    │
│  │  IRelayServer.h        (relay server interface)  │    │
│  │  IAuthenticationProvider.h  (auth interface)     │    │
│  │  ILogSink.h            (logging interface)       │    │
│  │  IConnectionMonitor.h  (monitoring interface)    │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─ infrastructure/ ───────────────────────────────┐    │
│  │  capture/WinDivertCapture  (WinDivert driver)    │    │
│  │  relay/TcpRelayServer      (HTTP CONNECT relay)  │    │
│  │  auth/KerberosAgentProvider (Kerberos via pipe)  │    │
│  │  auth/BasicAuthProvider     (Basic auth)         │    │
│  │  config/ConfigManager       (config.json I/O)    │    │
│  │  ipc/PipeServer             (named pipe server)  │    │
│  │  logging/Logger             (async file logger)  │    │
│  │  stats/StatsCollector       (connection stats)   │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─ adapters/driven/ ──────────────────────────────┐    │
│  │  ProxyEngine.h    (legacy direct proxy connector)│    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│           AuthAgent (TcpRedirectorAuthAgent.exe)         │
│  SspiEngine     (SSPI InitializeSecurityContext)        │
│  ContextStore   (in-memory context storage, TTL=5min)  │
│  Named Pipe     (\\.\pipe\TcpRedirectorAuth)            │
└─────────────────────────────────────────────────────────┘
```

## 2. Data Flow

### 2.1. Packet Interception

```
Application (chrome.exe)
  │ TCP SYN → 1.2.3.4:443
  ▼
WinDivert (kernel)
  │ intercepts outbound TCP packet
  │ resolves process PID
  │ checks rules (CheckProcessRule)
  ▼
Redirect to 127.0.0.1:34010
```

### 2.2. Connection Handling

```
Client Application
  │ TCP connect → 127.0.0.1:34010
  ▼
AcceptLoop (TcpRelayServer)
  │ accept() → client_sock
  ▼
HandleNewConnection
  │ ConnectionTable lookup by src_port
  │ create RelayContext
  ▼
ConnectionHandler (new thread)
  │ connect() → upstream proxy
  │ HTTP CONNECT → proxy
  │ ├─ Basic Auth (single request)
  │ └─ Negotiate Auth (CreateContext → 407 → ContinueContext → 200)
  ▼
StartBridge (two threads)
  ├─ OneWayRelay UP   (client → proxy)  [RX bytes]
  └─ OneWayRelay DN   (proxy → client)  [TX bytes]
```

### 2.3. Kerberos Authentication Flow

```
TcpRelayServer (SYSTEM)
  │ CreateContext("HTTP/proxy")
  ▼
KerberosAgentProvider
  │ Call("create_context", {spn})
  ▼
Named Pipe (\\.\pipe\TcpRedirectorAuth)
  ▼
AuthAgent (User Session)
  │ SspiEngine::CreateContext()
  │ InitializeSecurityContext() → SSPI
  │ Returns: token (base64), context_id
  ▼
TcpRelayServer
  │ CONNECT + "Proxy-Authorization: Negotiate <token>"
  ▼
Upstream Proxy
  │ 407 Proxy Auth Required
  │ "Proxy-Authenticate: Negotiate <challenge>"
  ▼
TcpRelayServer
  │ ContinueContext(context_id, challenge)
  ▼
AuthAgent
  │ InitializeSecurityContext(challenge) → SSPI
  │ Returns: final_token
  ▼
TcpRelayServer
  │ CONNECT + "Proxy-Authorization: Negotiate <final_token>"
  ▼
Upstream Proxy
  │ 200 Connection Established ✓
```

## 3. Key Components

### 3.1. WinDivertCapture

**File**: [`WinDivertCapture.h`](../src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.h)

- Loads `WinDivert.dll` dynamically (Late Binding)
- Filter: `outbound && tcp && tcp.Syn && !tcp.Ack` (SYN packets only)
- For each SYN: resolve process PID, check rules
- On rule match: redirect to `127.0.0.1:34010`
- PID cache (PID → process name) for performance

### 3.2. TcpRelayServer

**File**: [`TcpRelayServer.h`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h)

- Listens on `127.0.0.1:34010` (and [::1]:34010 for IPv6)
- `AcceptLoop`: main connection accept thread
- `ConnectionHandler`: per-connection thread — connects to upstream proxy, sends HTTP CONNECT, starts bridge
- `StartBridge`: creates two `OneWayRelay` threads (UP/DN) for bidirectional data transfer
- Retry support for 502/504 (up to 3 attempts)
- `TCP_NODELAY` on all sockets (v1.1.5)

### 3.3. KerberosAgentProvider

**File**: [`KerberosAgentProvider.h`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.h)

- Connects to AuthAgent via `\\.\pipe\TcpRedirectorAuth`
- JSON-RPC protocol: `create_context`, `continue_context`, `close_context`, `ping`
- Automatic agent launch via `CreateProcessAsUserW()` (WTS API)
- Keepalive: ping every 15 seconds
- **v1.1.5**: `std::recursive_mutex` protects pipe I/O from thread races
- **v1.1.3**: `WarmUp()` — proactive agent launch at service startup

### 3.4. AuthAgent

**File**: [`SspiEngine.cpp`](../src/auth-agent/TcpRedirectorAuthAgent/SspiEngine.cpp)

- Separate process in user session (not SYSTEM)
- Uses SSPI (`InitializeSecurityContext`) to obtain Kerberos/Negotiate tokens
- In-memory context storage with 5-minute TTL
- Named pipe server: `\\.\pipe\TcpRedirectorAuth`

### 3.5. IPC (Inter-Process Communication)

```
GUI ←→ Service:  \\.\pipe\TcpRedirectorService  (JSON-RPC)
Service ←→ Agent: \\.\pipe\TcpRedirectorAuth     (JSON-RPC)
```

### 3.6. GUI (WPF)

**Files**: `ShellViewModel.cs`, `SettingsViewModel.cs`

- MVVM pattern (CommunityToolkit.Mvvm)
- Sole writer of `config.json`
- Displays: service status, statistics, traffic graph, logs
- Manages rules and authentication settings

## 4. Multithreading

| Component | Model | Notes |
|-----------|-------|-------|
| AcceptLoop | 1 thread | select() on two sockets |
| ConnectionHandler | 1 thread per connection | CreateThread() |
| OneWayRelay (UP) | 1 thread per bridge | Blocking recv/send |
| OneWayRelay (DN) | 1 thread per bridge | Runs in ConnectionHandler thread |
| KeepaliveLoop | 1 thread | AuthAgent ping |
| Logger | Queue + 1 thread | Async write |
| WinDivert | 1 thread | Blocking recv |

**Synchronization:**
- `m_pipeMutex` (recursive) — pipe I/O protection in KerberosAgentProvider
- `pair->refs` (interlocked) — RelayPair reference counting
- `m_activePairs` (atomic) — active bridge tracking
- `m_connected` (atomic) — AuthAgent connection status

## 5. Configuration

File: `%ProgramData%\TcpRedirector\config.json`

```json
{
  "proxy": { "host": "...", "port": 3128 },
  "auth": { "enabled": true, "kerberos": true, "authMode": "KerberosOnly" },
  "log": { "level": 2, "directory": "...", "max_file_size_mb": 10 },
  "log_rotation": { "max_files": 5, "compress": true },
  "rules": [ { "type": "process", "action": "proxy", "pattern": "chrome.exe" } ]
}
```

## 6. Build Pipeline

```
C++ Service:    MSBuild /p:Configuration=Release /p:Platform=x64
                → TcpRedirectorService.exe

C++ AuthAgent:  MSBuild /p:Configuration=Release /p:Platform=x64
                → TcpRedirectorAuthAgent.exe

C# GUI:         dotnet build -c Release
                → TcpRedirectorGUI.exe + dependencies/

Package:        ZIP archive with bin/, docs/, config.json
```

## 7. Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| WinDivert | 2.2 | Packet interception (kernel) |
| nlohmann/json | 3.x | JSON parsing (header-only) |
| .NET 9.0 | 9.0.x | GUI runtime |
| CommunityToolkit.Mvvm | 8.x | MVVM for GUI |
| Windows SDK | 10.0+ | Win32 API, SSPI, WTS |

## 8. Architecture Changelog

| Version | Change |
|---------|--------|
| v1.1.0 | Hexagonal architecture, Kerberos authentication |
| v1.1.1 | SSPI buffer fix, WarmUp(), bridge logging |
| v1.1.3 | Proactive AuthAgent launch (WarmUp) |
| v1.1.4 | AuthProvider re-creation on config reload |
| v1.1.5 | TCP_NODELAY, recursive_mutex for pipe, try-catch in StartBridge |