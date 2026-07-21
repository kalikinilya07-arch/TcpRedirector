# Per-User Kerberos Auth Helper — E2E / Integration Verification Plan

**Feature:** Per-user SSPI/Kerberos auth via per-user helper (Variant 4b)
**Design doc:** [`plans/kerberos_per_user_auth_helper_plan.md`](../plans/kerberos_per_user_auth_helper_plan.md) (see §8)
**Applies to:** `TcpRedirectorService.exe` + `TcpRedirectorAuthHelper.exe`
**Audience:** QA/ops engineer running on a real **domain-joined** Windows test host.

---

## 0. Purpose & scope

This document is a **runnable** manual verification plan for the per-user Kerberos
auth feature. It is the human-executed complement to the automated unit tests
(`tests/CMakeLists.txt`, Catch2). The unit tests already cover the protocol
library, provider decision-table, broker client timeout/nonce logic, and manager
pure helpers. This plan covers what unit tests **cannot**: real Kerberos ticket
acquisition, cross-session isolation, WTS session lifecycle, and installer
delivery.

**Core acceptance criterion:** with the feature ON, the principal presented to
the proxy is the **interactive user** (e.g. `CORP\alice`), NOT the machine
account (`COMPUTERNAME$`). With the feature OFF, behavior is byte-for-byte the
legacy machine-account path.

---

## 1. Environment matrix

| Requirement | Detail |
|---|---|
| Host | Windows 10/11 or Server, **domain-joined** to a test AD |
| Privileges | Local admin (install service, read `%ProgramData%` logs) |
| Users | At least **two** distinct domain users (e.g. `CORP\alice`, `CORP\bob`) able to log on interactively / via RDP |
| Proxy (real path) | An HTTP proxy requiring `Negotiate`, with an SPN registered so Kerberos is chosen (FQDN, e.g. `HTTP/proxy.corp.example.com`) |
| Proxy (stub path) | `python stub/stub_proxy.py --auth-mode challenge` on the test host or a reachable host |
| Python | 3.x (for the stub) |
| Build outputs | `TcpRedirectorService.exe` **and** `TcpRedirectorAuthHelper.exe` co-located (installer ships both; see [`installer/setup.iss`](../installer/setup.iss:81)) |

### 1.1 Kerberos vs NTLM oracle rule (critical)

`Negotiate` picks **Kerberos** only when the target has a resolvable SPN — in
practice an **FQDN** proxy host with a registered `HTTP/<fqdn>` SPN. An
**IP-literal** proxy host forces **NTLM** fallback. The stub's
[`classify_negotiate_token`](../stub/stub_proxy.py:71) reports the wire subtype in
the `token_subtype` log field:

- `Kerberos/SPNEGO (GSS-API)` → real Kerberos (GSS/SPNEGO ASN.1 wrapper).
- `NTLM Type1/2/3` → NTLM leg.

**Always test the happy path against an FQDN proxy** or you will get NTLM and the
per-user identity assertion becomes ambiguous.

---

## 2. Config reference

The `auth` block lives in `config.json` (v2). Domain mirror:
[`ProxyConfig.h`](../src/service/TcpRedirectorService/domain/entities/ProxyConfig.h:32).

```json
"auth": {
  "enabled": true,
  "kerberos": true,
  "per_user_helper": true,
  "spn": "",                     // empty => auto HTTP/<proxy_host>
  "helper_timeout_ms": 5000,
  "fallback_policy": "drop"      // "drop" (default) | "system" | "error"
}
```

| Field | Code field | Notes |
|---|---|---|
| `per_user_helper` | `per_user_auth_enabled` | Master switch for this feature |
| `spn` | `auth_spn` | Empty → auto `HTTP/<host>` ([`MakeSpn`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:225)) |
| `helper_timeout_ms` | `helper_timeout_ms` | Broker call timeout (default 5000) |
| `fallback_policy` | `fallback_policy` | **Default = `drop`** ([`ProxyConfig.h`](../src/service/TcpRedirectorService/domain/entities/ProxyConfig.h:63)) — never silently machine-auth |

> **Note on the default:** the implementation defaults `fallback_policy` to
> **`drop`** (fail-closed), which is stricter than the `system` value discussed
> in plan §4.5. Regression tests that expect legacy behavior **must set
> `fallback_policy=system` explicitly** (see TC-R1).

### 2.1 Where to read logs

- **Service log:** `%ProgramData%\TcpRedirector\logs\` (per Logger config).
  Grep for `service`, `AuthHelperManager`, `dropping connection`, `fallback`.
- **Helper log:** written by [`HelperLog.h`](../src/service/AuthHelper/HelperLog.h)
  in the **user's** context (see the helper `README.md`); one file per session.
- **Proxy/stub log:** stub prints structured events incl. `token_subtype`,
  `CHALLENGE_SENT`, target, and (with `--reveal-secrets`) the raw principal.

---

## 3. Stub-proxy usage (verification oracle)

Start the stub in challenge mode so it drives a full Negotiate handshake and logs
the token subtype:

```bat
python stub\stub_proxy.py --port 8888 --auth-mode challenge --log-format json --log-file stub.log
```

Useful flags (see [`stub_proxy.py`](../stub/stub_proxy.py:825) argparse):

| Flag | Effect |
|---|---|
| `--auth-mode challenge` | Sends `407` and runs the 3-leg Negotiate/NTLM loop; logs `token_subtype` each leg |
| `--auth-mode accept-any` | Accepts first token (no 407 loop) — use only for smoke |
| `--port 8888` | Bind port |
| `--log-format json` | Machine-readable events (recommended for grep) |
| `--reveal-secrets` | Also logs decoded principal / raw blob (handle carefully) |

**Reading the oracle:** each `REQUEST`/challenge event carries
`token_subtype`. For the happy path you want to see
`token_subtype":"Kerberos/SPNEGO (GSS-API)"`. Point clients at the proxy **by
FQDN** to get Kerberos, by **IP** to force NTLM.

> The stub does not validate credentials — it is an **identity/observability
> oracle**, not an authz gate. Real-principal confirmation on a production proxy
> comes from that proxy's authenticated-user log.

---

## 4. Common setup / teardown

**Setup (once):**
1. Install the package (or `deploy.bat` output). Confirm **both** EXEs present in
   `{app}`: `TcpRedirectorService.exe`, `TcpRedirectorAuthHelper.exe`.
2. Configure a redirect rule for a test client app (e.g. `curl.exe` /
   `packet_generator.exe`) → proxy `HTTP CONNECT`.
3. Point the proxy target at either the real Kerberos proxy (FQDN) or the stub.

**Per-test config change procedure:**
1. Edit config via the GUI (Settings → "Per-user Kerberos (multi-user/RDP)" +
   fallback dropdown) **or** edit `config.json` directly.
2. **Restart the service** (`sc stop TcpRedirectorService && sc start
   TcpRedirectorService`) so the new `auth` block and helper-manager wiring load
   (see [`ServiceMain.h`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h:212)).
3. Confirm in the service log: `AuthHelperManager started (per-user Kerberos auth
   enabled)` when the feature is ON.

**Teardown:** stop service, kill any stray `TcpRedirectorAuthHelper.exe`, revert
config.

---

## 5. Test cases

### 5.1 Regression (feature OFF)

#### TC-R1 — Feature OFF = legacy machine-account behavior
- **Precondition:** `per_user_helper=false`. (`fallback_policy` irrelevant when OFF.)
- **Steps:**
  1. Restart service; confirm service log does **NOT** show `AuthHelperManager
     started` (manager only starts when `per_user_helper && kerberos`).
  2. Confirm **no** `TcpRedirectorAuthHelper.exe` process exists (Task Manager).
  3. Drive proxied traffic (client app → proxy FQDN, stub in `challenge`).
- **Expected:** Handshake succeeds via legacy [`SspiNegotiate`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:46) (machine account). Proxy/stub principal = `COMPUTERNAME$`. `token_subtype=Kerberos/SPNEGO` (FQDN) — same as before this feature.

#### TC-R2 — Basic-auth path untouched
- **Precondition:** `kerberos=false`, Basic auth configured.
- **Steps:** Drive traffic; observe proxy receives `Proxy-Authorization: Basic ...`.
- **Expected:** Identical to pre-feature Basic behavior; helper never involved.

#### TC-R3 — Existing capture flows unchanged (WinDivert + embedded Wintun)
- **Precondition:** `per_user_helper=false`, run the standard integration flow ([`tests/integration_test.bat`](integration_test.bat) / [`tests/run_test.bat`](run_test.bat)).
- **Expected:** All existing traffic tests pass; no behavioral delta.

---

### 5.2 Happy path (feature ON, domain, Kerberos)

#### TC-H1 — Interactive user principal via real Kerberos proxy
- **Precondition:** `per_user_helper=true`, `kerberos=true`, FQDN proxy with SPN,
  logged on interactively as `CORP\alice`.
- **Steps:**
  1. Enable feature (GUI/config), restart service.
  2. Confirm service log: `AuthHelperManager started`.
  3. Confirm `TcpRedirectorAuthHelper.exe` runs, **User column = `CORP\alice`**
     (Task Manager → Details → User name), one instance for alice's session.
  4. As alice, run the proxied client app to drive a CONNECT.
  5. Read the **proxy's** authenticated-principal log for that connection.
- **Expected:** Presented principal = **`CORP\alice`** (NOT `COMPUTERNAME$`).
  Helper log line records the acquiring identity as alice.

#### TC-H2 — Kerberos confirmed via stub `token_subtype`
- **Precondition:** As TC-H1 but target = stub `--auth-mode challenge` by **FQDN**.
- **Steps:** Drive traffic; grep stub log for `token_subtype`.
- **Expected:** `token_subtype":"Kerberos/SPNEGO (GSS-API)"` (proves Negotiate
  chose Kerberos, not NTLM). Handshake completes (stub returns 200 after the
  final token).

#### TC-H3 — NTLM vs Kerberos discrimination
- **Steps:** Repeat TC-H2 but point the client at the proxy by **IP literal**.
- **Expected:** `token_subtype":"NTLM Type1/2/3"` across the legs (documented
  NTLM fallback). Confirms the FQDN/SPN dependency is understood and the oracle
  discriminates correctly.

---

### 5.3 Helper lifecycle

#### TC-L1 — Launch on logon, runs as the user
- **Steps:** With feature ON, log a user on. Observe helper appears.
- **Expected:** Exactly one `TcpRedirectorAuthHelper.exe` per active interactive
  session, User column = that user; service log records the launch.

#### TC-L2 — Relaunch on kill
- **Steps:** `taskkill /f /im TcpRedirectorAuthHelper.exe` (that session's PID).
- **Expected:** Manager relaunches it within the backoff window (bounded restart
  rate); new PID appears; service log notes the relaunch. Subsequent connections
  authenticate normally.

#### TC-L3 — Survives RDP disconnect, killed on logoff
- **Steps:** RDP-disconnect (not logoff) the user → check helper still present.
  Then log the user off → check helper gone.
- **Expected:** `WTS_SESSION_DISCONNECT` keeps the helper (session still logged
  on); `WTS_SESSION_LOGOFF` kills it and closes its pipe. (See SESSIONCHANGE
  forwarding in [`main.cpp`](../src/service/TcpRedirectorService/main.cpp:25) →
  [`OnSessionChange`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h:597).)

#### TC-L4 — No orphans on service crash
- **Steps:** Force-kill the service process.
- **Expected:** Job Object kills all helpers (no orphan `TcpRedirectorAuthHelper.exe`
  remain). On SCM auto-restart, helpers relaunch for active sessions.

---

### 5.4 Multi-user / RDP

#### TC-M1 — Two concurrent sessions → two helpers, no cross-talk
- **Precondition:** alice on the console, bob via RDP; both drive proxied traffic.
- **Steps:**
  1. Confirm **two** helpers, one as alice, one as bob.
  2. Each user drives a CONNECT concurrently.
  3. Read proxy/stub principal per connection.
- **Expected:** alice's connections present `CORP\alice`; bob's present
  `CORP\bob`. **No** session's token appears on the other's connection.

#### TC-M2 — Cross-session pipe ACL denial
- **Precondition:** TC-M1 running (both helpers up).
- **Steps:** As bob, attempt to open alice's helper pipe
  `\\.\pipe\TcpRedirectorAuth_<aliceSessionId>` with a small probe tool
  (`CreateFile`).
- **Expected:** **Access denied.** The pipe ACL grants only the owner-user SID +
  `SY` (LocalSystem); `BA` is intentionally not granted (plan §3.1).

---

### 5.5 Fallback policy

#### TC-F1 — `fallback_policy=drop`: helper unavailable ⇒ connection DROPPED
- **Precondition:** `per_user_helper=true`, `fallback_policy=drop`.
- **Steps:** Kill the helper (or deny it) so `TryGetHelperForSession` fails; drive traffic.
- **Expected:** Connection is **dropped** — never machine-account-authenticated.
  Service log WARN indicating drop (`dropping connection` / `fallback_policy=drop`).
  Proxy log shows **no** `COMPUTERNAME$` auth for that attempt.

#### TC-F2 — `fallback_policy=drop`: external tun2socks mode (no PID attribution)
- **Precondition:** `capture_mode=wintun`, `wintun.engine=external`,
  `per_user_helper=true`, `fallback_policy=drop`.
- **Steps:** Start service; drive traffic through the external engine.
- **Expected:** Startup log warns ONCE that per-user auth is unavailable in
  external mode; connections are dropped (cannot resolve user → fail-closed).

#### TC-F3 — `fallback_policy=system`: helper unavailable ⇒ legacy machine auth
- **Precondition:** `per_user_helper=true`, `fallback_policy=system`, no/killed helper.
- **Steps:** Drive traffic.
- **Expected:** Falls back to `LocalSspiProvider` (machine account); tunnel works;
  proxy principal = `COMPUTERNAME$`. Service log notes the fallback.

#### TC-F4 — `fallback_policy=error`: helper unavailable ⇒ close + error count
- **Precondition:** `per_user_helper=true`, `fallback_policy=error`, no/killed helper.
- **Steps:** Drive traffic.
- **Expected:** Connection closed, `proxy_errors` incremented, service WARN. No
  machine-account auth.

#### TC-F5 — Helper timeout mid-handshake
- **Precondition:** `per_user_helper=true`; simulate a slow/hung helper (or set a
  very low `helper_timeout_ms`).
- **Steps:** Drive traffic; observe broker call exceed `helper_timeout_ms`.
- **Expected:** Treated as helper-unavailable → behaves per `fallback_policy`
  (drop/system/error). Under `system`, tunnel still works.

#### TC-F6 — Local (non-domain) account ⇒ no_credentials
- **Precondition:** Log on with a **local** (non-domain) account; feature ON.
- **Steps:** Drive traffic to an FQDN proxy.
- **Expected:** Helper returns `no_credentials` (no Kerberos TGT) → handled per
  `fallback_policy` (same semantics as legacy NTLM-fallback).

---

### 5.6 Security

#### TC-S1 — SPN allow-list denies unexpected SPN
- **Precondition:** feature ON; craft/inject a broker `sspi_step` for an SPN
  **not** on the helper's allow-list (e.g. `CIFS/dc`, `LDAP/dc`).
- **Steps:** Send the request (test harness / instrumented client to the pipe as
  LocalSystem).
- **Expected:** Helper responds `status:"denied"`; relay treats as hard fail
  (WARN); **never** downgrades to machine auth. (Allow-list per plan §3.3.)

#### TC-S2 — Non-LocalSystem caller rejected
- **Precondition:** feature ON, helper running.
- **Steps:** From a non-privileged process **in the same session**, open the
  helper pipe (if ACL admitted) and send `sspi_step`.
- **Expected:** Helper `ImpersonateNamedPipeClient` → caller ≠ `S-1-5-18` →
  **rejected**. (plan §3.2.)

#### TC-S3 — Pipe-squat defeated
- **Precondition:** feature ON.
- **Steps:** Pre-create `\\.\pipe\TcpRedirectorAuth_<sessionId>` before the helper
  launches (squat), then log the user on.
- **Expected:** `FILE_FLAG_FIRST_PIPE_INSTANCE` on the real helper + hello-nonce
  mismatch + server-PID check cause the service to reject the squatted pipe.
  (plan §3.2.)

---

## 6. Checklist table

| Test ID | Precondition | Steps (summary) | Expected result | Pass/Fail |
|---|---|---|---|---|
| TC-R1 | `per_user_helper=false` | Restart, drive traffic | Legacy machine auth; principal `COMPUTERNAME$`; no helper | ☐ |
| TC-R2 | `kerberos=false`, Basic | Drive traffic | `Basic` auth unchanged; no helper | ☐ |
| TC-R3 | feature OFF | Run existing integration flow | All existing traffic tests pass | ☐ |
| TC-H1 | ON, FQDN proxy, alice | Enable, restart, drive as alice | Proxy principal = `CORP\alice`; helper runs as alice | ☐ |
| TC-H2 | ON, stub `challenge` FQDN | Drive, grep stub | `token_subtype=Kerberos/SPNEGO (GSS-API)` | ☐ |
| TC-H3 | ON, proxy by IP | Drive, grep stub | `token_subtype=NTLM` (fallback) | ☐ |
| TC-L1 | ON | Log user on | 1 helper/session, runs as user | ☐ |
| TC-L2 | ON | Kill helper | Relaunched within backoff | ☐ |
| TC-L3 | ON | Disconnect, then logoff | Survives disconnect; killed on logoff | ☐ |
| TC-L4 | ON | Kill service | Job Object kills all helpers; no orphans | ☐ |
| TC-M1 | ON, alice+bob | Concurrent traffic | Each principal matches its session; no bleed | ☐ |
| TC-M2 | ON, both up | bob opens alice's pipe | Access denied (ACL) | ☐ |
| TC-F1 | ON, `drop`, no helper | Drive traffic | Connection DROPPED; WARN; no machine auth | ☐ |
| TC-F2 | ON, `drop`, external tun2socks | Drive traffic | Startup warn once; connections dropped | ☐ |
| TC-F3 | ON, `system`, no helper | Drive traffic | Legacy machine auth; tunnel works | ☐ |
| TC-F4 | ON, `error`, no helper | Drive traffic | Close + `proxy_errors`++ + WARN | ☐ |
| TC-F5 | ON, low timeout / hung helper | Drive traffic | Timeout → fallback per policy | ☐ |
| TC-F6 | ON, local account | Drive traffic (FQDN) | `no_credentials` → policy fallback | ☐ |
| TC-S1 | ON, unlisted SPN | Send `sspi_step` | Helper `denied`; hard fail; no downgrade | ☐ |
| TC-S2 | ON, non-SYSTEM caller | Call helper pipe | Rejected (caller ≠ LocalSystem) | ☐ |
| TC-S3 | ON, squatted pipe | Pre-create pipe, logon | Squat defeated (FIRST_PIPE_INSTANCE + nonce + PID) | ☐ |

---

## 7. Automated unit tests (reference)

Built and run via CMake + CTest (Catch2 v3):

```bat
cmake -S tests -B tests\build -G "Visual Studio 17 2022" -A x64
cmake --build tests\build --config Release
ctest --test-dir tests\build -C Release --output-on-failure
```

Auth-related unit test files ([`tests/CMakeLists.txt`](CMakeLists.txt:40)):
`AuthBrokerProtocolTest.cpp`, `BrokeredAuthProviderTest.cpp`,
`AuthHelperManagerTest.cpp`, `SelectingAuthProviderFactoryTest.cpp` (compiled with
`AuthHelperManager.cpp` + shared `auth_sspi.cpp`).

> **Known harness quirk:** several auth `TEST_CASE` names contain the `§`
> (U+00A7) character. `catch_discover_tests` registers each test by name and
> re-invokes the exe with a name filter; the non-ASCII byte does not survive the
> CMake→console codepage round-trip, so CTest reports **`No test cases matched`**
> for those 3 tests even though the bodies pass. Verify them by **tag** instead:
> ```bat
> tests\build\Release\tcp_redirector_tests.exe "[authbroker]"
> tests\build\Release\tcp_redirector_tests.exe "[brokered],[mapping],[drop]"
> ```
> (Recommended follow-up: rename those `TEST_CASE`s to ASCII, e.g. `(sec 2.1)`,
> so CTest reports them green.)

**Test-integration gap:** the legacy batch scripts ([`build.bat`](../build.bat:70)
step `[1/3]`, [`run_test.bat`](run_test.bat), [`prepare_test_build.bat`](prepare_test_build.bat))
do **not** run the Catch2 unit suite — they compile only the two legacy
standalone tests or drive traffic stubs. The full unit suite (including all auth
tests) is run **only** via the CMake/CTest path above. CI should invoke CTest
explicitly.
