# Developer Documentation — TcpRedirector v1.1.5

## 1. Codebase Structure

```
TcpRedirector/
├── src/
│   ├── service/TcpRedirectorService/    # C++ Windows Service
│   │   ├── main.cpp                      # Service entry point (install/run/uninstall)
│   │   ├── adapters/driving/             # Primary adapters (inbound)
│   │   │   ├── ServiceMain.h             # Windows Service lifecycle (SCM handler)
│   │   │   └── IpcHandler.h              # JSON-RPC over named pipe (GUI ↔ Service)
│   │   ├── adapters/driven/              # Secondary adapters (outbound)
│   │   │   └── ProxyEngine.h             # Legacy direct proxy connector (pre-relay)
│   │   ├── domain/
│   │   │   ├── ports/                    # Interfaces (I*)
│   │   │   │   ├── ICapture.h            # Packet capture interface
│   │   │   │   ├── IRelayServer.h        # Relay server interface
│   │   │   │   ├── IAuthenticationProvider.h  # Auth interface + result types
│   │   │   │   ├── ILogSink.h            # Logging interface
│   │   │   │   ├── IConnectionTable.h    # Connection tracking
│   │   │   │   └── IConnectionMonitor.h  # Monitoring interface
│   │   │   └── entities/
│   │   │       └── ProxyConfig.h         # DTOs: ProxyConfig, Rule, LogLevel, etc.
│   │   └── infrastructure/               # Implementations
│   │       ├── capture/
│   │       │   └── WinDivertCapture.*    # WinDivert driver integration
│   │       ├── relay/
│   │       │   ├── TcpRelayServer.h      # HTTP CONNECT relay (header-only)
│   │       │   └── ConnectionTable.h     # Connection hash table
│   │       ├── auth/
│   │       │   ├── KerberosAgentProvider.* # Kerberos via AuthAgent (pipe IPC)
│   │       │   ├── BasicAuthenticationProvider.* # Basic Auth
│   │       │   └── auth_sspi.*           # SSPI helpers (Parse407, Base64)
│   │       ├── config/
│   │       │   ├── Config.h              # Config structures
│   │       │   └── ConfigManager.*       # JSON config I/O + DPAPI
│   │       ├── ipc/
│   │       │   └── PipeServer.h          # Named pipe server (header-only)
│   │       ├── logging/
│   │       │   ├── Logger.*              # Async file logger
│   │       │   └── LogRotator.*          # Log rotation + compression
│   │       └── stats/
│   │           └── StatsCollector.*      # Connection statistics
│   │
│   ├── auth-agent/TcpRedirectorAuthAgent/ # C++ Auth Agent (user session)
│   │   ├── main.cpp                       # Named pipe server + JSON-RPC dispatch
│   │   ├── SspiEngine.*                   # SSPI context management
│   │   └── ContextStore.h                 # In-memory context storage
│   │
│   └── gui/TcpRedirectorGUI/             # C# WPF GUI
│       ├── Adapters/Driving/Wpf/
│       │   ├── ViewModels/
│       │   │   ├── ShellViewModel.cs      # Main window logic
│       │   │   ├── SettingsViewModel.cs   # Settings tab logic
│       │   │   └── StatsViewModel.cs      # Statistics tab logic
│       │   └── Controls/
│       │       └── TrafficGraph.cs        # Custom traffic visualization
│       └── Infrastructure/
│           ├── Ipc/IpcClient.cs           # Named pipe client (JSON-RPC)
│           └── Config/JsonConfigRepository.cs # Config file I/O
│
├── external/
│   └── ProxyBridge/                       # Legacy C code (reference only)
│
├── tests/
│   ├── mock_proxy.cpp                     # Mock HTTP proxy for testing
│   ├── packet_generator.cpp               # TCP packet generator
│   └── unit/service/RuleEngineTest.cpp    # Rule matching tests
│
└── docs/
    ├── ADMIN_GUIDE_RU.md / ADMIN_GUIDE_EN.md
    ├── SECURITY_RU.md / SECURITY_EN.md
    ├── ARCHITECTURE_RU.md / ARCHITECTURE_EN.md
    ├── DEVELOPER_RU.md / DEVELOPER_EN.md (this file)
    ├── BUILD.md, INSTALL.md, UPGRADE.md, UNINSTALL.md
    └── SMOKE_TEST.md
```

## 2. Build from Source

### 2.1. Prerequisites

| Tool | Version | Path |
|------|---------|------|
| Visual Studio 2022+ | 18.x | `C:\Program Files\Microsoft Visual Studio\18\Community\` |
| .NET SDK | 9.0 | `dotnet` in PATH |
| Windows SDK | 10.0+ | Included with VS |

### 2.2. C++ Service

```bat
cd src\service\TcpRedirectorService
MSBuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build /m
:: Output: build\service\x64\Release\TcpRedirectorService.exe
```

### 2.3. C++ AuthAgent

```bat
cd src\auth-agent\TcpRedirectorAuthAgent
MSBuild TcpRedirectorAuthAgent.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build /m
:: Output: build\auth-agent\x64\Release\TcpRedirectorAuthAgent.exe
```

### 2.4. C# GUI

```bat
cd src\gui\TcpRedirectorGUI
dotnet build -c Release
:: Output: bin\Release\net9.0-windows\TcpRedirectorGUI.exe
```

## 3. Key Implementation Details

### 3.1. WinDivert Integration

**File**: [`WinDivertCapture.h`](../src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.h)

WinDivert is loaded via **Late Binding** (not import library):

```cpp
// Dynamic loading pattern
HMODULE hDivert = LoadLibraryW(L"WinDivert.dll");
pWinDivertOpen = (WinDivertOpenFn)GetProcAddress(hDivert, "WinDivertOpen");
```

**Filter**: `outbound && tcp && tcp.Syn && !tcp.Ack`

Only SYN packets are intercepted to determine the original destination. Once redirected, the relay server handles the full TCP stream. This avoids processing every packet in kernel mode.

**PID Resolution**: For each intercepted SYN packet, the process PID is determined via `WinDivertHelperParsePacket` + WinDivert PID cache. The PID is cached for subsequent packets from the same source port to avoid repeated lookups.

### 3.2. Connection Table

**File**: [`ConnectionTable.h`](../src/service/TcpRedirectorService/infrastructure/relay/ConnectionTable.h)

The connection table maps intercepted SYN packets to established relay connections:

```
Key: (src_port, src_ip) → Value: (orig_dest_ip, orig_dest_port, proxy_config_id)
```

- Hash table with linked-list collision resolution
- SRWLock (Slim Reader/Writer Lock) for thread safety
- **Known Issue**: compound key was added (v1.1.1) to prevent collisions when two connections share the same ephemeral source port

### 3.3. SSPI Negotiate Auth (Legacy)

**File**: [`auth_sspi.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp)

The legacy SSPI implementation is used by the test proxy scripts and AuthAgent:

```cpp
// Key steps:
1. AcquireCredentialsHandle()    → credentials
2. InitializeSecurityContext()   → token + context
3. Base64 encode token           → "Negotiate <token>"
4. Parse 407 challenge           → extract server token
5. InitializeSecurityContext()   → continue with challenge
```

**SSPI flags**: `ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_ALLOCATE_MEMORY`

### 3.4. Named Pipe IPC

Both IPC channels use **message-mode** named pipes with JSON-RPC 2.0 protocol:

```
Request:  {"jsonrpc":"2.0","method":"create_context","params":{...},"id":1}
Response: {"jsonrpc":"2.0","result":{...},"id":1}
```

**Service Pipe** (`\\.\pipe\TcpRedirectorService`):
- Server: `PipeServer.h` — creates pipe with `EVERYONE` (GENERIC_READ) security
- Client: `IpcClient.cs` (GUI, C#)
- Methods: `get_config`, `set_config`, `get_rules`, `set_rules`, `get_connections`, `get_logs`, `get_stats`, `ping`, `start_capture`, `stop_capture`, `reload_config`, `set_log_level`

**AuthAgent Pipe** (`\\.\pipe\TcpRedirectorAuth`):
- Server: AuthAgent `main.cpp` — creates pipe with `EVERYONE` (full access), **no sharing**
- Client: `KerberosAgentProvider.cpp` — service connects as client
- Methods: `hello`, `create_context`, `continue_context`, `close_context`, `ping`

### 3.5. Bidirectional Relay (Bridge)

**File**: [`TcpRelayServer.h`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h), lines 572-710

The relay uses two `OneWayRelay` functions (UP and DN) for bidirectional data transfer:

```cpp
struct RelayPair {
    SOCKET sock_client;   // Client-side socket
    SOCKET sock_proxy;    // Proxy-side socket
    LONG refs;            // Reference count (2 → 0)
};
```

**Reference Counting**:
- `pair->refs` starts at 2 (one for UP, one for DN)
- Each `OneWayRelay` decrements `refs` via `InterlockedDecrement()` on completion
- When `refs` reaches 0, sockets are closed and pair is deleted
- **M1 fix (half-close)**: on recv=0 or error, `shutdown(to, SD_SEND)` is called — the sibling thread can continue receiving until its own recv returns 0

**Socket Options** (v1.1.5):
- 4 MB buffer (`SO_RCVBUF`, `SO_SNDBUF`)
- `TCP_NODELAY` = 1 (disable Nagle's algorithm)
- `SO_KEEPALIVE` = 1 (after tunnel established)
- Timeout reset to 0 after CONNECT 200 (no SO_RCVTIMEO during data phase)

### 3.6. Pipe I/O Synchronization (v1.1.5 fix)

**File**: [`KerberosAgentProvider.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp), line 415

**Problem**: `ConnectionHandler` runs in a new thread for each connection. Multiple threads could call `CreateContext()` simultaneously, all calling `SendMessage()`/`ReadMessage()` on the **same** named pipe handle without synchronization. This caused message interleaving — thread A wrote a request, thread B wrote before A could read, then A read B's response. Affected threads hung forever in `ReadMessage()`.

**Fix**: Changed `m_pipeMutex` from `std::mutex` to `std::recursive_mutex` and added `lock_guard` in `Call()`:

```cpp
nlohmann::json KerberosAgentProvider::Call(const std::string& method,
                                            const nlohmann::json& params) {
    std::lock_guard<std::recursive_mutex> lock(m_pipeMutex);  // ← NEW
    // ... SendMessage + ReadMessage
}
```

**Why recursive_mutex**: `ConnectToAgent()` takes `m_pipeMutex`, then calls `Call("hello", ...)` which now also takes it. Without recursion, this would deadlock.

### 3.7. AuthAgent Auto-Launch

**File**: [`KerberosAgentProvider.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp), line 283

The service (running as `LocalSystem`) launches AuthAgent in the user's session using:

```cpp
1. WTSGetActiveConsoleSessionId()     → session ID
2. WTSQueryUserToken(sessionId)       → user token
3. DuplicateTokenEx()                 → primary token (minimal rights)
4. CreateEnvironmentBlock()           → user environment
5. CreateProcessAsUserW()             → launch AuthAgent.exe
```

**Rate limiting**: launch cooldown of 10 seconds, plus `s_launchInProgress` atomic flag for single-attempt gating.

### 3.8. DPAPI Password Encryption

**File**: [`ConfigManager.cpp`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp)

```cpp
// Encrypt: plaintext → base64(DPAPI_encrypted)
CryptProtectData(plaintext, entropy) → base64

// Decrypt: base64(DPAPI_encrypted) → plaintext
CryptUnprotectData(base64_decode(input), entropy) → plaintext
```

Entropy is a fixed byte string added to prevent rainbow table attacks on DPAPI.

### 3.9. Configuration Atomic Writes

```cpp
// Save sequence:
1. Write to config.json.tmp
2. Flush + close tmp file
3. ReplaceFile(tmp, config.json)  // atomic rename on NTFS
```

This prevents config corruption on power loss or process crash mid-write.

## 4. Threading Model

```
Main Thread
├── WinDivert capture loop (1 thread, blocking WinDivertRecv)
├── AcceptLoop (1 thread, select() on listen sockets)
│   └── ConnectionHandler (1 thread per connection, CreateThread)
│       ├── OneWayRelay DN (runs in ConnectionHandler thread)
│       └── OneWayRelay UP (1 thread, CreateThread)
├── KeepaliveLoop (1 thread, ping AuthAgent every 15s)
├── Logger worker (1 thread, async queue processing)
├── LogRotator (1 thread, periodic rotation check)
└── IPC PipeServer (accept loop + per-client thread)
```

**Thread Safety Notes**:
- `KerberosAgentProvider::Call()` is now thread-safe (serialized via `m_pipeMutex`)
- `ConnectionTable` protected by `SRWLOCK`
- `RelayPair::refs` uses `InterlockedDecrement` (lock-free)
- `m_activePairs`, `m_totalRxBytes`, `m_totalTxBytes` are `std::atomic`
- `ConfigManager::Load()/Save()` protected by `shared_mutex` (multiple readers, single writer)

## 5. Testing

### 5.1. Test Proxy Scripts

Located in `test-proxy/` (sibling directory):

- `test_proxy.py` — simple HTTP proxy (no auth)
- `test_proxy_negotiate.py` — Negotiate-auth proxy (SSPI)
- `test_proxy_direct.py` — direct proxy (no redirection)
- `test_proxy_https.py` — HTTPS testing

Run: `python test_proxy_negotiate.py [port]`

### 5.2. Mock Proxy (C++)

`tests/mock_proxy.cpp` — standalone TCP proxy for integration testing. Connects to real upstream servers.

### 5.3. Unit Tests

```
tests/unit/service/RuleEngineTest.cpp

cd tests/build2
ctest -C Release
```

### 5.4. Smoke Test

See [`SMOKE_TEST.md`](SMOKE_TEST.md) for the full test procedure.

## 6. Debugging Tips

### 6.1. Console Mode

```bat
TcpRedirectorService.exe --console
```
Runs the service as a console application instead of Windows Service — useful for printf-debugging and IDE debugging.

### 6.2. Log Levels

Set `"level": 0` in `config.json` for trace-level logging, then:

```bat
tail -f "%ProgramData%\TcpRedirector\logs\tcp_redirector.log"
```

### 6.3. WinDivert Debug

Enable WinDivert debug log:
```bat
set WINDIVERT_DEBUG=1
TcpRedirectorService.exe --console
```
Output goes to `%ProgramData%\TcpRedirector\logs\windivert_debug.log`.

### 6.4. AuthAgent Debug

Run AuthAgent manually in a console:
```bat
TcpRedirectorAuthAgent.exe
```
Writes diagnostics to `%ProgramData%\TcpRedirector\logs\auth_agent.log` and stderr.

### 6.5. Named Pipe Inspection

```bat
:: List all TcpRedirector pipes
dir \\.\pipe\TcpRedirector*

:: Check pipe security
icacls \\.\pipe\TcpRedirectorAuth
```

## 7. Common Pitfalls

### 7.1. Pipe Message Interleaving (FIXED in v1.1.5)

Never call `WriteFile`/`ReadFile` on the same named pipe from multiple threads without serialization. Message-mode pipes deliver complete messages, but concurrent writes interleave at the byte level.

### 7.2. FlushFileBuffers on Message Pipes (FIXED in v1.1.5)

`FlushFileBuffers()` is unnecessary on message-mode named pipes — `WriteFile` already ensures the message boundary. It only adds latency.

### 7.3. TCP_NODELAY (FIXED in v1.1.5)

Without `TCP_NODELAY`, Nagle's algorithm delays small packets (TLS handshake, HTTP requests) by up to 200ms. Always set `TCP_NODELAY` on relay sockets.

### 7.4. SSPI Buffer Allocation

`InitializeSecurityContext` may return `SEC_E_BUFFER_TOO_SMALL` if the output buffer is too small. Always use `SecBufferDesc` with sufficient buffer size and handle the retry.

### 7.5. WinDivert Filter

The filter `outbound && tcp && tcp.Syn && !tcp.Ack` only intercepts SYN. If you change the filter to intercept all packets, you MUST also handle the full TCP state machine — a much more complex undertaking.

### 7.6. DPAPI Scope

DPAPI encryption is scoped to the user account. A password encrypted by the GUI (user scope) CANNOT be decrypted by the service (SYSTEM scope) and vice versa. Both must use the same scope.

## 8. Version History

See [`CHANGELOG.md`](../CHANGELOG.md) for full details. Key milestones:

| Version | Changes |
|---------|---------|
| v1.1.0 | Hexagonal architecture, Kerberos auth, GUI |
| v1.1.1 | SSPI buffer fix, WarmUp(), bridge logging, ConnectionHandler diagnostics |
| v1.1.3 | Proactive AuthAgent launch |
| v1.1.4 | AuthProvider re-creation on config reload |
| v1.1.5 | TCP_NODELAY, recursive_mutex for pipe I/O, removed FlushFileBuffers, try-catch in StartBridge |