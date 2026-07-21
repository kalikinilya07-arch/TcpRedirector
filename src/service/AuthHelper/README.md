# TcpRedirectorAuthHelper (Variant 4b — Phase 3)

Per-user Kerberos SSPI auth helper. A tiny, headless x64 executable launched
**by the service inside a user's logon session** (Phase 5). It hosts a per-session
named-pipe **server** and performs SSPI/Negotiate **in the user's own security
context**, so `AcquireCredentialsHandleW(NULL, L"Negotiate", …)` acquires the
**user's** Kerberos TGT instead of the machine account.

> Source of truth: [`plans/kerberos_per_user_auth_helper_plan.md`](../../../plans/kerberos_per_user_auth_helper_plan.md)
> — §1.4 (helper process), §2 (IPC), §3 (security).
> Wire protocol: [`AuthBrokerProtocol.h`](../../shared/auth_broker/AuthBrokerProtocol.h)
> (namespace `tcp_redirector::shared::auth_broker`).

## Files

| File | Purpose |
|---|---|
| [`AuthHelperMain.cpp`](AuthHelperMain.cpp) | Entry point: CLI parsing, session detection, session-0 refusal, stdin/parent watchdog, server bootstrap |
| [`AuthPipeServer.h`](AuthPipeServer.h) | Named-pipe **server**: SDDL, LocalSystem-caller check, `hello`, request loop, per-correlation `SspiContext` map + TTL GC, SPN allow-list, credential-handle cache |
| [`HelperLog.h`](HelperLog.h) | User-writable per-session logger (`%LOCALAPPDATA%\TcpRedirector\auth_helper_<sessionId>.log`) |
| [`TcpRedirectorAuthHelper.vcxproj`](TcpRedirectorAuthHelper.vcxproj) | x64 Release console project; links the **shared** `auth_sspi.cpp` verbatim |

The SSPI logic is **not** duplicated: the project compiles
[`auth_sspi.cpp`](../TcpRedirectorService/infrastructure/auth/auth_sspi.cpp)
directly from the service tree (`<ClCompile Include="..\TcpRedirectorService\...\auth_sspi.cpp" />`).

## Command-line contract (for Phase 5 `AuthHelperManager`)

```
TcpRedirectorAuthHelper.exe
    [--session <N>]        WTS session id. If omitted, derived via
                           ProcessIdToSessionId(GetCurrentProcessId()).
                           If given, MUST match own session (else exit 5).
    [--pipe <name>]        Full pipe name. If omitted, built from
                           MakeAuthPipeNameW(sessionId).
    --nonce <token>        One-time secret the manager recorded at launch;
                           returned in `hello` (§2.1/§3.2). Empty => warns.
    --spn <SPN>            Allowed SPN (repeatable).
    --proxy-host <host>    Alternative: derive "HTTP/<host>" into the allow-list.
    [--ttl <sec>]          TTL for abandoned SspiContexts (default 30).
    [--idle-exit <sec>]    Self-exit on idle (0 = never; default 0).
    [--console]            Also mirror log to stderr (interactive debugging).
    [--version <str>]      Helper version string surfaced in `hello`.
```

**Exit codes:** `2` bad args, `3` ProcessIdToSessionId failed, `4` refused
session 0, `5` session mismatch, `0` normal shutdown.

**SPN allow-list source:** `--spn`(×N) plus optional `--proxy-host`→`HTTP/<host>`.
Empty list ⇒ **fail-closed** (every `sspi_step` answered `denied`). Phase 5 must
pass the configured/expected proxy SPN(s) from `auth.spn` / proxy host.

## Security summary

- **Pipe SDDL:** `O:SYG:SYD:(A;;GA;;;SY)(A;;GA;;;<owner-user-SID>)`. Only the
  owning user (SID from `TokenUser`) and LocalSystem (`SY`) may connect.
  `BUILTIN\Administrators` (`BA`) is deliberately **not** granted, so an admin in
  another session cannot open this user's helper pipe. `FILE_FLAG_FIRST_PIPE_INSTANCE`
  on the first instance defeats pipe-name squatting.
- **Caller check:** after connect, `ImpersonateNamedPipeClient` +
  `GetTokenInformation(TokenUser)` require the client to be LocalSystem
  (`S-1-5-18`); anyone else is disconnected (defense-in-depth over the ACL).
- **Token-oracle scoping:** `sspi_step` is served only for SPNs on the allow-list;
  others → `denied` (never silently downgraded).

## Phase 4 notes (service-side `AuthBrokerClient` / `BrokeredAuthProvider`)

1. Connect to `MakeAuthPipeNameW(sessionId)` via `CreateFileW`, set
   `PIPE_READMODE_MESSAGE`.
2. First read is the helper's `hello` — parse with `ParseHelloMessage`, verify
   `session_id` matches and `nonce` equals the value the manager launched with
   (§3.2). Optionally verify the server PID via `GetNamedPipeServerProcessId`.
3. Drive steps: send `Serialize(SspiStepRequest{correlation_id, spn, proxy_host,
   server_token, session_id})`, read `ParseSspiStepResponse`. Map `AuthStatus`
   via `ToAuthStepStatus` (Denied/Error → `Failed`; NoCredentials → `NoCredentials`).
4. On connection end/success/failure send `Serialize(ReleaseMessage{correlation_id})`.
5. Apply `helper_timeout_ms`; on timeout/pipe error → §5 fallback policy.

## Phase 5 notes (launch) & Phase 8 notes (packaging)

- **Launch:** `WTSQueryUserToken` → `DuplicateTokenEx` (primary) →
  `CreateEnvironmentBlock` → `CreateProcessAsUserW` with
  `lpDesktop = L"winsta0\\default"`, `CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT`,
  passing `--session <N> --nonce <…> --spn <…>`. Attach to a kill-on-close Job
  Object. Optionally pass the child's `stdin` as a pipe the service holds open;
  closing it triggers the helper's graceful shutdown (stdin watchdog).
- **Packaging (Phase 8):** `build.bat` already has a minimal `[3b/3]` step that
  builds the vcxproj and copies `TcpRedirectorAuthHelper.exe` into `build\`.
  Phase 8 must additionally: stage it in `deploy.bat` next to the service, add it
  to `setup.iss` `[Files]` (`DestDir: "{app}"; Components: core`), and include it
  in `package.bat` staging. The service resolves the helper path next to its own
  EXE (via `AppPaths`).
