# Security Measures — TcpRedirector v1.1.5

## 1. Scope

This document describes the information security measures implemented in TcpRedirector for handling network traffic, authentication data, and configuration information.

## 2. Threat Model

### 2.1. Protected Assets

| Asset | Classification | Storage Location |
|-------|---------------|------------------|
| Proxy credentials (login/password) | Confidential | `config.json` (DPAPI-encrypted) |
| Kerberos tokens (Negotiate) | Confidential | Memory only, lifetime = one CONNECT |
| Routing rules configuration | Internal | `config.json` |
| Connection logs | Internal | `%ProgramData%\TcpRedirector\logs\` |
| User application traffic | Confidential | Transit (not stored) |

### 2.2. Threat Actors

| Actor | Access Level | Attack Vector |
|-------|-------------|---------------|
| Local non-admin user | User | Named pipe access, GUI |
| Local administrator | Administrator | Full system access |
| Malware in user session | User | Named pipe access, GUI interception |
| Network attacker | Network | Traffic interception between service and proxy |

## 3. Implemented Security Measures

### 3.1. Credential Protection

#### 3.1.1. DPAPI Password Encryption

Proxy password is stored in `config.json` in encrypted form using Windows Data Protection API (DPAPI):

```
config.json → auth.encryptedPassword → DPAPI (user scope, LocalSystem)
```

- **Scope**: current user (GUI) or LocalSystem (service)
- **Algorithm**: AES-256 (provided by DPAPI)
- **Entropy**: additional entropy from a fixed string
- **Password in memory**: stored in plaintext only in `ProxyConfig::plain_password`, cleared on object destruction

**Code**: [`ConfigManager.cpp`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp) — `EncryptPassword()` / `DecryptPassword()` methods.

#### 3.1.2. Kerberos Tokens

- Negotiate tokens are generated **in the user session** via AuthAgent (not in SYSTEM context)
- SSPI context lives exactly one HTTP CONNECT
- After successful CONNECT or error, context is immediately destroyed: [`CloseContext()`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:110)
- Tokens are **never written** to disk or logged

### 3.2. IPC Access Control

#### 3.2.1. Named Pipe: Service

- Name: `\\.\pipe\TcpRedirectorService`
- Security Descriptor: `EVERYONE` (GENERIC_READ) — read for all, write blocked for non-admins
- GUI connects as client, service only sends data
- **Code**: [`PipeServer.h`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h:183)

#### 3.2.2. Named Pipe: AuthAgent

- Name: `\\.\pipe\TcpRedirectorAuth`
- Security Descriptor: `EVERYONE` (full access) — **only one session can connect** (no sharing)
- Service (SYSTEM) connects as client
- **Code**: [`KerberosAgentProvider.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:160)

#### 3.2.3. Limitations

- **Critical**: AuthAgent named pipe is created without security descriptor → any local user can connect and send requests. **Recommendation**: add ACL restricting to `LocalSystem` and `Administrators`.
- **Mitigation**: pipe is created with `no sharing` (single connection), limiting the attack to one malicious actor at a time.

### 3.3. Configuration Protection

#### 3.3.1. Atomic Writes

- `config.json` is written via temporary file + atomic rename
- Prevents config corruption on power failure or crash
- **Code**: [`ConfigManager.cpp`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp) — `Save()` method

#### 3.3.2. GUI Isolation

- GUI is the sole writer of `config.json`
- Service only reads config at startup and on reload command
- **Code**: [`ServiceMain.h`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h:261) — `reloadConfig` callback

### 3.4. Network Traffic Protection

#### 3.4.1. Relay Port Isolation

- Relay server listens on `127.0.0.1:34010` — not accessible externally
- Only accepts connections redirected by WinDivert from localhost

#### 3.4.2. Proxy Authentication

- Supports Kerberos/Negotiate (SSPI) — more secure than Basic
- Basic password is only transmitted inside HTTP CONNECT to proxy (localhost/local network)

### 3.5. Log Protection

#### 3.5.1. Log Rotation

- Maximum file size: 10 MB (default)
- Maximum file count: 5
- Old logs are compressed (gzip) to save space
- **Code**: [`LogRotator.cpp`](../src/service/TcpRedirectorService/infrastructure/logging/LogRotator.cpp)

#### 3.5.2. Log Confidentiality

- Passwords and Kerberos tokens are **not logged**
- Connection logs contain: destination IP addresses, ports, process names, traffic volume
- **Recommendation**: enable disk encryption (BitLocker) to protect logs at rest

### 3.6. AuthAgent Launch in User Session

- AuthAgent is launched via `CreateProcessAsUserW()` with user token
- Token is duplicated with minimal required rights: `TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY`
- **Code**: [`KerberosAgentProvider.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp:283)

### 3.7. Code Integrity

- All executables are compiled from source code (C++ service, C# GUI)
- Code signing (Authenticode) is recommended for protection against tampering

## 4. Residual Risks

| Risk | Level | Mitigation |
|------|-------|------------|
| Access to AuthAgent named pipe | Medium | Add ACL (planned) |
| Password in service memory | Low | Only accessible to SYSTEM |
| Service→proxy traffic interception | Low | Localhost / local network |
| DoS via multiple connections | Low | Proxy connection limit |

## 5. Hardening Recommendations

1. **Add ACL to AuthAgent named pipe** — restrict to `LocalSystem` and `Administrators`
2. **Code signing** — Authenticode certificate for executables
3. **Enable BitLocker** — encrypt logs and configuration at rest
4. **Firewall configuration** — restrict port 34011 (IPC) to localhost only
5. **Log auditing** — configure log monitoring for anomalies

## 6. Standards Compliance

The product addresses requirements of the following standards and practices:
- **CWE-256**: password not stored in plaintext (DPAPI)
- **CWE-522**: credentials protected in transit (Kerberos/Negotiate)
- **CWE-312**: tokens not persisted to disk
- **OWASP Top 10**: A07:2021 — Identification and Authentication Failures (addressed)