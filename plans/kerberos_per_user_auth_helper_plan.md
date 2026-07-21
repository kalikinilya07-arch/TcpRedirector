# Per-User Kerberos Auth Helper — Implementation Plan (Variant 4b)

**Status:** Design / implementation-ready
**Date:** 2026-07-21
**Feature:** Per-user SSPI auth helper + IPC token brokering
**Author:** Architect mode (grounded in current source)

---

## 0. Problem statement & ground truth

The relay [`TcpRelayServer::ConnectionHandler`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:385) runs inside `TcpRedirectorService.exe`, which the SCM starts as **LocalSystem** (see [`main.cpp`](../src/service/TcpRedirectorService/main.cpp:93) `SERVICE_WIN32_OWN_PROCESS`). All SSPI runs there:

- [`SspiNegotiate`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:46) calls [`AcquireCredentialsHandleW(NULL, L"Negotiate", SECPKG_CRED_OUTBOUND, ...)`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:71) — "current user" = **LocalSystem = the machine account `COMPUTERNAME$`**.
- Confirmed by docs [`КОНФИГУРАЦИЯ.md`](../docs/КОНФИГУРАЦИЯ.md:133): *"Служба по умолчанию работает под LocalSystem (identity = машинный аккаунт домена)"*.

**Consequence:** Kerberos authenticates as the computer, not the human user who launched the proxied app. Variant 4b fixes this by acquiring the SSPI token **inside the user's logon session** in a per-user helper process, and brokering the token blobs to the LocalSystem relay over a secure named pipe.

### Key facts discovered from the code (drive the whole design)

| Fact | Source | Design impact |
|---|---|---|
| Relay auth is per-connection, stateless; a fresh `SspiContext` lives on the stack and is discarded at handler exit | [`TcpRelayServer.h:452`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:452) | Each connection = one brokered SSPI handshake; helper must hold context across the multi-step round trip |
| The relay does **NOT** resolve PID/user today — `ConnectionHandler` only has `client_port`, `orig_dest_ip/port` | [`TcpRelayServer.h:385-390`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:385) | Must ADD PID→session→SID resolution in the relay before auth |
| PID/proc_path are stored in `ConnectionTable` by capture via [`SetProcessInfo`](../src/service/TcpRedirectorService/infrastructure/relay/ConnectionTable.h:202) | [`WinDivertCapture.cpp:474`](../src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:474) | `ConnectionTable::GetInfo` gives PID; reuse it. Add `GetInfo`-by-port on the relay hot path |
| `ProcessResolver::ResolvePidBySourcePort` already exists (GetExtendedTcpTable, 30s cache) | [`ProcessResolver.h:81`](../src/service/TcpRedirectorService/infrastructure/process/ProcessResolver.h:81) | Fallback PID resolver if ConnectionTable has no PID (e.g. embedded wintun path) |
| Named-pipe + SDDL ACL + JSON is the established IPC idiom | [`PipeServer.h:139`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h:139) (`D:(A;;GA;;;BA)(A;;GA;;;SY)`) | Mirror it (reversed roles) for the broker channel |
| `ChildProcessSupervisor` spawns children **in the same session** via CreateProcess | [`ChildProcessSupervisor.h:115`](../src/service/TcpRedirectorService/infrastructure/process/ChildProcessSupervisor.h:115) | Cannot reuse as-is for cross-session launch; need a new WTS/CreateProcessAsUser launcher |
| `auth_sspi.cpp` is dependency-free (only secur32) and already a `.cpp` in the vcxproj | [`vcxproj:81`](../src/service/TcpRedirectorService/TcpRedirectorService.vcxproj:81) | Compile the SAME file verbatim into the helper — zero SSPI logic duplication |
| Config is `config.json` v2, `auth` block | [`config.default.json:15`](../installer/config.default.json:15) | Extend `auth` with helper options |
| tun2socks **external** engine cannot attribute a flow to a PID | [`config.default.json:38`](../installer/config.default.json:38) `engine`, task background | Explicit fallback branch for that mode |
| Stub proxy supports `--auth-mode challenge` and logs `token_subtype` (NTLM vs Kerberos/SPNEGO) | [`stub_proxy.py:71`](../stub/stub_proxy.py:71) `classify_negotiate_token` | Primary verification oracle |

---

## 1. Target architecture

### 1.1 Component overview

```
                          LocalSystem process (TcpRedirectorService.exe)
  +---------------------------------------------------------------------------------+
  |  TcpRelayServer.ConnectionHandler                                               |
  |     | resolve client_port -> PID (ConnectionTable.GetInfo / ProcessResolver)    |
  |     | PID -> sessionId (ProcessIdToSessionId) -> user SID (OpenProcessToken)    |
  |     v                                                                           |
  |  IAuthBroker  --------------------------------------------------------------+   |
  |     | routes SSPI steps to the helper owning sessionId                      |   |
  |     v                                                                       |   |
  |  AuthHelperManager                                                          |   |
  |     - enumerates sessions (WTSEnumerateSessions)                            |   |
  |     - launches one helper per active user session                          |   |
  |       (WTSQueryUserToken + CreateProcessAsUser, correct sessionId)          |   |
  |     - tracks {sessionId -> HelperEntry{pid, sid, pipeName, state}}          |   |
  |     - relaunch on logon (SERVICE_CONTROL_SESSIONCHANGE), cleanup on logoff  |   |
  +--------------------------------|------------------------------------------------+
                                   | secure named pipe (per-session):
                                   |   \\.\pipe\TcpRedirectorAuth_<sessionId>
                                   |   service = CLIENT, helper = SERVER
                                   v
                 Interactive user session N (winsta0\Default, sessionId=N)
  +---------------------------------------------------------------------------------+
  |  TcpRedirectorAuthHelper.exe  (runs AS the user)                                |
  |    - named-pipe SERVER, ACL: owner-user + LocalSystem only                      |
  |    - on each request: SspiNegotiate(...) with the USER's Kerberos TGT           |
  |    - holds SspiContext per correlation-id across the multi-step handshake       |
  |    - SPN allow-list = configured proxy SPN(s) ONLY (token-oracle scoping)       |
  +---------------------------------------------------------------------------------+
```

### 1.2 New service-side component: `AuthHelperManager`

Location: `src/service/TcpRedirectorService/infrastructure/auth/AuthHelperManager.{h,cpp}`

Responsibilities:

1. **Session enumeration & tracking.** On start and on session-change events, call `WTSEnumerateSessionsW` and keep only `WTSActive`/`WTSConnected` **interactive user** sessions (skip session 0 — the services session — see §6 session-0 isolation).
2. **Per-session helper launch.** For each qualifying session:
   - `WTSQueryUserToken(sessionId, &hUserToken)` → primary token of the logged-in user.
   - `DuplicateTokenEx(hUserToken, TOKEN_ALL_ACCESS, ..., SecurityImpersonation→TokenPrimary)`.
   - `CreateEnvironmentBlock(hUserToken)` so the child gets the user's `%USERPROFILE%`, Kerberos ticket cache location, etc.
   - `STARTUPINFOW.lpDesktop = L"winsta0\\default"`.
   - `CreateProcessAsUserW(hDupToken, helperExePath, cmdline, ..., CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, env, ...)`.
   - Pass the target session id + expected pipe name on the command line, e.g. `--session <N> --pipe TcpRedirectorAuth_<N>`.
   - Attach the child to a **kill-on-close Job Object** (same guarantee as [`ChildProcessSupervisor`](../src/service/TcpRedirectorService/infrastructure/process/ChildProcessSupervisor.h:16)) so no orphan helpers survive a service crash.
3. **Lifecycle.** Relaunch a helper if its process dies while the session is still active (bounded restart rate, reuse the rolling-window logic pattern from `ChildProcessSupervisor`). Cleanup (kill helper, close pipe client) on logoff/disconnect.
4. **Lookup for the relay.** `bool TryGetHelperForSession(DWORD sessionId, HelperEntry* out)`.

### 1.3 Relay-side connection→helper selection

New helper used inside [`ConnectionHandler`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:385), executed **only when** `m_kerberosAuth && perUserHelperEnabled`:

```
resolve:
  pid = connTable.GetInfo(client_port).pid            // primary
  if pid == 0: pid = processResolver.ResolvePidBySourcePort(client_port)   // fallback
  if pid == 0: -> FALLBACK per policy (see §5)
  ProcessIdToSessionId(pid, &sessionId)
  sid = tokenUserSid(pid)                              // OpenProcess + OpenProcessToken + GetTokenInformation(TokenUser)
  helper = authHelperManager.TryGetHelperForSession(sessionId)
  if !helper: -> FALLBACK per policy (see §5)
```

The relay then drives the SSPI handshake **through the broker** instead of calling `SspiNegotiate` locally. The existing `goto retry_connect` loop ([`TcpRelayServer.h:458`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:458)) is preserved; only the token-production calls at lines [482](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:482) and [616](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:616) are swapped for broker calls (see §4.3).

### 1.4 New helper process: `TcpRedirectorAuthHelper.exe`

Location: `src/service/AuthHelper/` (new project `TcpRedirectorAuthHelper.vcxproj`).

- Tiny console/GUI-less EXE. No WinDivert / wintun / lwIP dependencies.
- Compiles the **shared** [`auth_sspi.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp) (added to its vcxproj as a linked source — no copy).
- Runs a named-pipe **server** at the name given on the command line.
- Per request: runs `SspiNegotiate` using the user's own credentials (because the process token IS the user's), keyed by a `correlation_id` so a multi-step handshake reuses the same `SspiContext`.
- Idle-exits after N seconds with no requests AND no session (defensive; the manager also kills it on logoff).

---

## 2. IPC design — token-brokering protocol

### 2.1 Transport

- **Named pipe**, one per session: `\\.\pipe\TcpRedirectorAuth_<sessionId>` (session id in the name avoids collisions across concurrent RDP users).
- **Roles reversed vs the GUI pipe:** the **helper is the server**, the **service is the client**. Rationale: the helper runs as the user and owns "its" pipe namespace object; the service (LocalSystem) can always open any pipe. This lets the pipe ACL be authored by the helper to admit exactly `{owner user SID, LocalSystem}`.
- **Message framing:** JSON, `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE` (identical to [`PipeServer.h:164`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h:164)). One request → one response.
- **Handshake nonce:** on connect, the helper sends a one-time `hello` with the session id and a random nonce that the manager recorded at launch time (passed on the helper command line), so the service can confirm it is talking to the helper it launched (defends against a squatted pipe name — see §3).

### 2.2 Message schema

Request (service → helper):

```json
{
  "v": 1,
  "op": "sspi_step",
  "correlation_id": "conn-<uint64>",     // unique per relay connection
  "spn": "HTTP/proxy.corp.example.com",  // MUST be on helper's allow-list
  "proxy_host": "proxy.corp.example.com",
  "server_token": "<base64 challenge or empty for first step>",
  "session_id": 3
}
```

Response (helper → service):

```json
{
  "v": 1,
  "status": "continue | complete | no_credentials | denied | error",
  "out_token": "<base64 outbound Negotiate token or empty>",
  "detail": "optional human text for logs"
}
```

Auxiliary ops: `{"op":"hello", ...}` (helper→service on connect), `{"op":"ping"}` (liveness), `{"op":"release","correlation_id":...}` (relay tells helper to free the `SspiContext` when a connection ends or fails).

### 2.3 Correlation of the multi-step handshake

- The relay generates a `correlation_id` at the top of `ConnectionHandler`.
- **Step 1:** `sspi_step` with empty `server_token` → helper creates a new `SspiContext`, stores it in a `map<correlation_id, SspiContext>` under a mutex, returns the first token + `status:"continue"` (or `complete`).
- **Step N (after 407):** `sspi_step` with the parsed challenge → helper looks up the stored context, calls `SspiNegotiate` again, returns the next token.
- On tunnel established, failure, or handler exit: relay sends `release`. Helper also GC's contexts older than a TTL (e.g. 30 s) to bound memory.
- This maps 1:1 onto the existing relay flow: step 1 replaces [`TcpRelayServer.h:482`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:482); step N replaces [`TcpRelayServer.h:616`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:616). The retry cap (`authRetries > 5`, [line 633](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:633)) is unchanged.

### 2.4 Timeouts & errors

- Broker call timeout: configurable `auth.helper_timeout_ms` (default 4000). Implemented with overlapped I/O or `WaitForSingleObject` on the pipe-read; on timeout → treat as helper-unavailable → §5 fallback.
- Error mapping: `no_credentials` → same handling as [`SspiResult::NoCredentials`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.h:37) today (log + fall back per policy); `denied` (SPN not allow-listed) → hard fail + WARN (never silently downgrade to machine auth); `error` → drop or fallback per policy.
- No-helper case: `TryGetHelperForSession` returns false → §5 fallback.
- PID→user resolution failure: → §5 fallback (`pid_resolution_failed`).

---

## 3. Security model

### 3.1 Pipe ACL (prevent cross-user token theft)

- The helper creates its pipe with an SDDL granting **only**:
  - the **owner user SID** (obtained from its own token via `GetTokenInformation(TokenUser)`), and
  - `SY` (LocalSystem) — the service.
  - SDDL shape: `O:SYG:SYD:(A;;GA;;;SY)(A;;GA;;;<user-SID>)` (deny-by-omission of everyone else; `BA` intentionally NOT granted so a local admin in another session cannot open a different user's helper pipe).
- Because the pipe is created *in the user's session by the user*, and named with the session id, a malicious user in session B cannot request tokens from the helper in session A: they lack ACL access AND they cannot name a helper they didn't launch.

### 3.2 Authenticating the service ↔ helper channel

- **Service verifies helper:** after connecting, the service calls `GetNamedPipeServerProcessId`/impersonates and checks the server side is the helper PID it launched (tracked in `AuthHelperManager`), and validates the `hello` nonce (§2.1). This defends against pipe-name squatting created *before* the real helper (mitigated further by `FILE_FLAG_FIRST_PIPE_INSTANCE` on the helper, matching [`PipeServer.h:166`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h:166)).
- **Helper verifies caller:** the helper calls `ImpersonateNamedPipeClient` + `GetTokenInformation(TokenUser)` and requires the caller to be **LocalSystem** (SID `S-1-5-18`). It refuses any other caller even if the ACL somehow admitted them.

### 3.3 Minimizing the token-oracle risk

The helper is a *potential Negotiate-token oracle* for the user's identity. Scope it down:

1. **SPN allow-list.** The helper accepts `sspi_step` only for SPNs on a configured allow-list (the proxy SPN(s) derived from `auth.spn` / proxy host). Any other SPN → `status:"denied"`. This prevents an attacker who somehow reaches the pipe from minting tokens for arbitrary services (e.g. `CIFS/dc`, `LDAP/dc`).
2. **Caller must be LocalSystem** (§3.2) — a non-privileged process in the same session cannot use the helper as an oracle.
3. **No raw credential export.** Only opaque Negotiate blobs for the allowed SPN cross the pipe — never passwords, never TGTs.
4. **Rate/GC limits.** Bounded concurrent contexts + TTL GC (§2.3) limit abuse and memory.

### 3.4 Interaction with the existing `.ipc_token` GUI auth

- The GUI↔service channel ([`PipeServer.h`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h) / `TcpIpcServer.h`, `.ipc_token`) is **unchanged and orthogonal**. Different pipe name, different direction, different purpose (config/stats vs auth brokering).
- The helper does **not** use `.ipc_token`. The auth pipe's security is SID-based (§3.1), not token-file-based, because the trust anchor here is the OS-verified process identity, not a shared secret file.

---

## 4. Affected modules / new files

### 4.1 New files

| Path | Purpose |
|---|---|
| `src/service/AuthHelper/TcpRedirectorAuthHelper.vcxproj` | New helper EXE project (x64, links shared `auth_sspi.cpp`) |
| `src/service/AuthHelper/main_helper.cpp` | Helper entry point: parse `--session/--pipe`, run pipe server loop |
| `src/service/AuthHelper/AuthPipeServer.h` | Helper-side named-pipe server + per-correlation `SspiContext` map + SPN allow-list |
| `src/service/TcpRedirectorService/infrastructure/auth/AuthHelperManager.h/.cpp` | Session enumeration, per-session launch (WTS + CreateProcessAsUser), tracking, relaunch/cleanup |
| `src/service/TcpRedirectorService/infrastructure/auth/AuthBrokerClient.h` | Service-side pipe **client**: `sspi_step`/`release`, timeout, hello-nonce verify |
| `src/service/TcpRedirectorService/infrastructure/auth/IAuthProvider.h` | Small interface abstracting "produce next Negotiate token" so the relay can be given EITHER the local-SSPI provider (today's behavior) OR the brokered provider |
| `src/service/TcpRedirectorService/infrastructure/auth/LocalSspiProvider.h` | Wraps existing `SspiNegotiate` as `IAuthProvider` (used for fallback + non-helper mode) |
| `src/service/TcpRedirectorService/infrastructure/auth/BrokeredAuthProvider.h` | Implements `IAuthProvider` via `AuthBrokerClient` |
| `tests/unit/service/AuthBrokerProtocolTest.cpp` | Unit tests for JSON schema encode/decode, SPN allow-list, correlation GC |

### 4.2 Shared SSPI split (`auth_sspi.cpp/.h`)

- **No behavioral change** to [`SspiNegotiate`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:46). It already takes `(ctx, serverToken, outToken, spn)` and is context-carrying — perfect for both processes.
- Add the file to the helper vcxproj (`<ClCompile Include="..\TcpRedirectorService\infrastructure\auth\auth_sspi.cpp" />`). It stays compiled in the service too ([`vcxproj:81`](../src/service/TcpRedirectorService/TcpRedirectorService.vcxproj:81)).
- Minor: replace the `printf` diagnostics ([`auth_sspi.cpp:58`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:58)) with an injectable log callback so the helper can route them to its own log without stdout noise (optional, low priority).

### 4.3 Relay changes (`TcpRelayServer.h`)

- Add setters: `SetAuthProvider(IAuthProvider*)`, `SetAuthHelperManager(AuthHelperManager*)`, `SetProcessResolver(ProcessResolver*)`, and a `bool m_perUserHelper` flag from config.
- In [`ConnectionHandler`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:385): before the `retry_connect` label, resolve PID→session→SID (§1.3) and select the provider:
  - helper available → `BrokeredAuthProvider` bound to that session;
  - else → policy: `LocalSspiProvider` (legacy machine auth) OR drop.
- Replace the two `infrastructure::SspiNegotiate(...)` call sites ([482](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:482), [616](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:616)) with `provider->NextToken(correlationId, serverToken, spn, outToken)`.
- On handler exit / error / success paths that `return`, call `provider->Release(correlationId)` (helper GC also covers leaks).
- Keep `Basic` auth path ([line 464](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:464)) untouched.

### 4.4 Service lifecycle wiring (`ServiceMain.h`, `main.cpp`)

- In [`ServiceMain.h Initialize()`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h:39): construct `AuthHelperManager` when `auth.kerberos && auth.per_user_helper`, start it, and wire it + a `ProcessResolver` + the chosen `IAuthProvider` into the relay (near the existing relay wiring at [ServiceMain.h:184-193](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h:184)).
- In [`main.cpp`](../src/service/TcpRedirectorService/main.cpp:9): the service control handler must register for **session-change notifications**. Change `RegisterServiceCtrlHandlerExW` usage and, in `ServiceControlHandlerEx`, handle `SERVICE_CONTROL_SESSIONCHANGE` (`WTS_SESSION_LOGON`, `WTS_SESSION_LOGOFF`, `WTS_SESSION_UNLOCK`, `WTS_SESSION_REMOTE_CONNECT`) by forwarding to `AuthHelperManager`. Also set `SERVICE_ACCEPT_SESSIONCHANGE` in the reported controls-accepted mask.
- Deploy path: the service must know the helper EXE path (next to the service EXE, resolved via `AppPaths`).

### 4.5 Config (`ProxyConfig.h`, `Config.h`/`ConfigManager`, `config.default.json`)

Add to the `auth` block:

```json
"auth": {
  "enabled": false,
  "kerberos": false,
  "per_user_helper": false,
  "spn": "",                     // optional explicit SPN; empty => HTTP/<proxy_host>
  "helper_timeout_ms": 4000,
  "fallback_policy": "system"    // "system" | "drop" | "error"
}
```

- Mirror these into `domain::ProxyConfig` (extend [`ProxyConfig.h:29`](../src/service/TcpRedirectorService/domain/entities/ProxyConfig.h:29)) and the `ConfigManager` parse/serialize.
- GUI: add a "Per-user Kerberos (multi-user/RDP)" checkbox + fallback dropdown in `SettingsViewModel`/config repository (out of scope for the C++ plan but noted for completeness).

### 4.6 Build files

- `build.bat`: add a step to build `src/service/AuthHelper/TcpRedirectorAuthHelper.vcxproj` (same msbuild invocation pattern as the service at [`build.bat:104`](../build.bat:104)) and copy `TcpRedirectorAuthHelper.exe` into `%ROOT%\build\`.
- `deploy.bat`: stage the helper EXE alongside the service.

### 4.7 Installer (`setup.iss`, `install.bat`/`package.bat`)

- `setup.iss` [`[Files]`](../installer/setup.iss:65): add `Source: "TcpRedirectorAuthHelper.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: core`.
- No scheduled task is strictly required (the service launches the helper via CreateProcessAsUser), which is simpler than a logon-triggered task and avoids per-user task registration. **A logon-triggered scheduled task is documented as an alternative** for environments where CreateProcessAsUser is restricted; if used, register it under `[Run]` with `schtasks /create`.
- `package.bat`: include the helper EXE in the obfuscated staging tree (note: obfuscar targets .NET; the C++ helper is copied verbatim like the service).

---

## 5. Fallback & compatibility

All fallbacks are governed by `auth.fallback_policy` (`system` | `drop` | `error`), evaluated whenever the brokered path cannot produce a user token.

| Situation | Detection | Behavior |
|---|---|---|
| No helper in session (not yet launched / crashed) | `TryGetHelperForSession` == false | policy: `system` → legacy `LocalSspiProvider` (machine account, today's behavior); `drop` → close connection; `error` → close + increment `proxy_errors` + WARN |
| External tun2socks mode (no PID attribution) | `capture_mode==wintun && wintun.engine==external` (known at startup, [`config.default.json:38`](../installer/config.default.json:38)) | Cannot resolve user → force policy fallback; log ONCE at startup that per-user auth is unavailable in external mode |
| Non-domain / local account | helper returns `no_credentials` (Negotiate → NTLM/none), same as [`SspiResult::NoCredentials`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.h:37) | policy fallback; behavior matches current NTLM-fallback semantics in [`КОНФИГУРАЦИЯ.md:127`](../docs/КОНФИГУРАЦИЯ.md:127) |
| PID→user resolution fails | pid==0 after ConnectionTable + ProcessResolver | policy fallback |
| Helper timeout / pipe error | broker call exceeds `helper_timeout_ms` | policy fallback (default `system`) |
| Feature disabled (`per_user_helper=false`) | config | exact current behavior — relay uses local `SspiNegotiate`; helper never launched. **Full backward compatibility.** |

Default `fallback_policy = "system"` guarantees the feature is a strict superset: turning it on can only *improve* auth (real user Kerberos when possible) and never breaks a working machine-account deployment.

---

## 6. Risks & mitigations

| Risk | Mitigation |
|---|---|
| **Cross-session IPC security** (user A steals user B's token) | Session-id-named pipe + SID-scoped ACL (§3.1) + LocalSystem-only caller check (§3.2) + SPN allow-list (§3.3) |
| **Helper lifecycle / crash recovery** | Kill-on-close Job Object (pattern from [`ChildProcessSupervisor.h:16`](../src/service/TcpRedirectorService/infrastructure/process/ChildProcessSupervisor.h:16)); bounded restart rate; relaunch on session-change; idle self-exit |
| **Per-connection latency on the hot path** | The costly step is `AcquireCredentialsHandle`; cache a **per-session credential handle** in the helper (acquire once, reuse). Optionally cache a **completed context** per (session, SPN) if the proxy accepts pre-emptive tokens. Pipe round-trip is local (µs–low ms). Reuse `ProcessResolver`'s 30 s PID cache ([`ProcessResolver.h:245`](../src/service/TcpRedirectorService/infrastructure/process/ProcessResolver.h:245)) |
| **Session 0 isolation** | Never launch a helper in session 0 (services). Only enumerate interactive sessions; skip `WTSGetActiveConsoleSessionId()==0` edge; the helper explicitly refuses if its own session id is 0 |
| **CreateProcessAsUser privilege** | LocalSystem holds `SE_ASSIGNPRIMARYTOKEN`/`SE_INCREASE_QUOTA` implicitly; document requirement. Provide the scheduled-task alternative (§4.7) if group policy blocks it |
| **Deployment/installer changes** | Additive only: one new EXE in `[Files]`; service starts helper at runtime; no new service, no driver. Config defaults keep feature OFF |
| **Backward compatibility** | `per_user_helper=false` (default) = byte-for-byte current behavior; `fallback_policy=system` preserves machine-auth path |
| **Token oracle** (helper minting tokens) | SPN allow-list + LocalSystem-only caller + opaque-blob-only transport (§3.3) |
| **Pipe-name squatting** | `FILE_FLAG_FIRST_PIPE_INSTANCE` on helper + hello-nonce + server-PID verification (§3.2) |
| **Orphan helpers on RDP disconnect vs logoff** | Distinguish `WTS_SESSION_DISCONNECT` (keep helper — session still logged on) from `WTS_SESSION_LOGOFF` (kill helper) |

---

## 7. Step-by-step implementation phases

Each phase is a self-contained Code-mode subtask.

- **Phase 0 — Config plumbing.** Extend `auth` in [`config.default.json`](../installer/config.default.json:15), `domain::ProxyConfig` ([`ProxyConfig.h`](../src/service/TcpRedirectorService/domain/entities/ProxyConfig.h:29)), and `ConfigManager` parse/serialize. Feature stays OFF. *Tests:* config round-trip unit test.
- **Phase 1 — IAuthProvider abstraction.** Add `IAuthProvider.h` + `LocalSspiProvider.h` wrapping existing `SspiNegotiate`. Refactor [`ConnectionHandler`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:385) to call `provider->NextToken(...)` instead of `SspiNegotiate` directly, wired to `LocalSspiProvider`. **Behavior identical.** *Tests:* existing WinDivert/wintun flows regress-tested against stub.
- **Phase 2 — Broker protocol library.** Implement JSON schema encode/decode, SPN allow-list, correlation map + TTL GC as a pure library. *Tests:* `AuthBrokerProtocolTest.cpp` (no I/O).
- **Phase 3 — Helper EXE.** New `AuthHelper` project + `AuthPipeServer` (server side). Links shared `auth_sspi.cpp`. Standalone test: run helper interactively, drive it with a small client harness against `stub_proxy.py --auth-mode challenge`, confirm token production. *Tests:* manual harness + protocol unit tests.
- **Phase 4 — AuthBrokerClient + BrokeredAuthProvider.** Service-side pipe client with timeout, hello-nonce verify, LocalSystem caller assertions on the helper side. *Tests:* loopback integration (service-as-client ↔ helper) in a single interactive session.
- **Phase 5 — AuthHelperManager.** Session enumeration, CreateProcessAsUser launch, Job Object, tracking, relaunch. *Tests:* launch/track/kill lifecycle in one session; verify helper runs as the user (`whoami` in helper log).
- **Phase 6 — Session-change wiring.** `main.cpp` + `ServiceMain.h`: accept `SERVICE_CONTROL_SESSIONCHANGE`, forward logon/logoff/disconnect to the manager. Wire relay PID→session→SID resolution + provider selection. *Tests:* logon/logoff drive helper create/destroy.
- **Phase 7 — Fallback policy.** Implement the §5 matrix incl. external-tun2socks branch and `fallback_policy`. *Tests:* each fallback path unit/integration-tested.
- **Phase 8 — Build & installer.** `build.bat`/`deploy.bat` build+stage helper; `setup.iss` ships it. *Tests:* clean install → helper present next to service.
- **Phase 9 — Docs & GUI.** Update [`КОНФИГУРАЦИЯ.md`](../docs/КОНФИГУРАЦИЯ.md), [`АРХИТЕКТУРА.md`](../docs/АРХИТЕКТУРА.md), [`УСТАНОВКА.md`](../docs/УСТАНОВКА.md); add GUI toggle.
- **Phase 10 — E2E verification.** Multi-user/RDP scenarios per §8.

---

## 8. Testing & verification plan

### 8.1 Token-identity verification (the core acceptance test)

- Run [`stub_proxy.py --auth-mode challenge`](../stub/stub_proxy.py:45) which logs `token_subtype` via [`classify_negotiate_token`](../stub/stub_proxy.py:71).
- **Machine-account regression (baseline):** with `per_user_helper=false`, capture the presented principal — expect `COMPUTERNAME$` (machine) when domain-joined + FQDN SPN.
- **Per-user (target):** with `per_user_helper=true`, the presented Kerberos ticket must be for the **logged-in user**, not `COMPUTERNAME$`. Verify by:
  1. On a real domain proxy: inspect the proxy's authenticated-principal log.
  2. With the stub: confirm `token_subtype=Kerberos/SPNEGO (GSS-API)` (proves Negotiate chose Kerberos, requires FQDN SPN per [`КОНФИГУРАЦИЯ.md:130`](../docs/КОНФИГУРАЦИЯ.md:130)); and confirm via the helper's own log line (replacing [`auth_sspi.cpp:57`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:57) `GetUserNameW`) that the acquiring identity is the user, not the machine account.
- **NTLM vs Kerberos distinction:** point the app at an IP-literal proxy → expect `token_subtype=NTLM` (documented fallback, [`КОНФИГУРАЦИЯ.md:138`](../docs/КОНФИГУРАЦИЯ.md:138)); at an FQDN proxy → expect Kerberos.

### 8.2 Multi-user / RDP scenarios

- Two concurrent interactive sessions (console + RDP), each running the proxied app as a **different** domain user. Assert:
  - two helpers exist (one per session), each running as its own user;
  - each connection's presented principal matches the session owner (no cross-talk);
  - user A cannot open user B's helper pipe (ACL test with a small probe tool).

### 8.3 Lifecycle tests

- Logon → helper appears; logoff → helper killed; RDP disconnect → helper survives; helper crash → relaunched within backoff; service crash → helpers killed by Job Object (no orphans).

### 8.4 Fallback tests

- external tun2socks mode → per-user disabled, `fallback_policy` honored.
- local (non-domain) account → `no_credentials` → policy fallback.
- helper timeout (kill helper mid-handshake) → policy fallback, tunnel still works under `system`.

### 8.5 Regression

- Full existing WinDivert and Wintun (embedded) flows with `per_user_helper=false` — must be byte-for-byte unchanged (Phase 1 guarantees this via the identical `LocalSspiProvider`).
- Basic-auth path unchanged.
- Existing unit tests ([`ConnectionTableTest.cpp`](../tests/unit/service/ConnectionTableTest.cpp), RuleEngine) still pass; add the new `AuthBrokerProtocolTest`.

### 8.6 Security tests

- SPN not on allow-list → helper returns `denied`; relay never downgrades to machine auth silently.
- Non-LocalSystem caller against the helper pipe → rejected.
- Pipe-squat attempt (pre-create the pipe name) → `FILE_FLAG_FIRST_PIPE_INSTANCE` + nonce mismatch defeats it.

---

## 9. Key open decisions (need product/ops confirmation)

1. **Default `fallback_policy`.** Recommended `system` (safe superset). Ops may prefer `drop`/`error` in strict environments where machine-account auth must never be used.
2. **Launch mechanism.** CreateProcessAsUser (recommended, self-contained) vs logon-triggered scheduled task (needed only if GPO blocks primary-token assignment). Decide default; possibly ship both with a config switch.
3. **SPN source.** Auto-derive `HTTP/<proxy_host>` (matches [`MakeSpn`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:225)) vs explicit `auth.spn`. Affects allow-list authoring.
4. **Credential/context caching aggressiveness.** Per-session credential handle only (safe) vs cached completed contexts / pre-emptive tokens (lower latency, larger oracle surface).
5. **RDP disconnect policy.** Keep helper alive on disconnect (recommended, faster reconnect) vs kill on disconnect (lower footprint).
6. **Obfuscation/signing of the helper EXE.** The .NET obfuscar path doesn't apply to C++; decide whether the helper needs Authenticode signing for enterprise deployment.