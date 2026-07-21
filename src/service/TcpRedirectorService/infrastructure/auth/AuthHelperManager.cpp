/**
 * @file AuthHelperManager.cpp
 * @brief Variant 4b, Phase 5 — реализация AuthHelperManager.
 *
 * См. заголовок для контракта и plans/kerberos_per_user_auth_helper_plan.md
 * §1.2/§1.3, §3, §6.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>       // ConvertSidToStringSidW
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include "AuthHelperManager.h"
#include "shared/auth_broker/AuthBrokerProtocol.h"  // MakeAuthPipeName

#ifndef STATUS_SUCCESS
#  define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace tcp_redirector {
namespace infrastructure {
namespace auth {

// ============================================================================
// Локальные утилиты
// ============================================================================
namespace {

std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int need = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(),
                                     static_cast<int>(ws.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                     static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), need);
    return out;
}

int64_t NowMonoMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int ClampInt(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi ? hi : v);
}

// RAII для WTS-выделенной памяти (WTSFreeMemory).
struct WtsMem {
    void* p = nullptr;
    ~WtsMem() { if (p) ::WTSFreeMemory(p); }
};

// RAII для HANDLE.
struct ScopedHandle {
    HANDLE h = nullptr;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE hh) : h(hh) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    void reset(HANDLE hh = nullptr) {
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        h = hh;
    }
    HANDLE release() { HANDLE t = h; h = nullptr; return t; }
    bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};

// RAII для environment-блока (DestroyEnvironmentBlock).
struct ScopedEnvBlock {
    LPVOID p = nullptr;
    ~ScopedEnvBlock() { if (p) ::DestroyEnvironmentBlock(p); }
};

}  // namespace

// ============================================================================
// detail:: чистые хелперы (тестируемые)
// ============================================================================
namespace detail {

std::string GenerateNonceHex(std::size_t byteLen) {
    if (byteLen == 0) byteLen = 32;
    std::vector<unsigned char> buf(byteLen, 0);
    NTSTATUS st = ::BCryptGenRandom(nullptr, buf.data(),
                                    static_cast<ULONG>(buf.size()),
                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != STATUS_SUCCESS) {
        return std::string();  // fail-closed: пустой nonce => запуск пропускается.
    }
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(byteLen * 2);
    for (unsigned char c : buf) {
        out.push_back(kHex[(c >> 4) & 0xF]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}

std::string DeriveSpn(const std::string& authSpn, const std::string& proxyHost) {
    if (!authSpn.empty()) return authSpn;
    if (!proxyHost.empty()) return "HTTP/" + proxyHost;
    return std::string();
}

std::vector<std::wstring> BuildHelperArgs(std::uint32_t sessionId,
                                          const std::string& nonceHex,
                                          const std::string& spn,
                                          const std::string& proxyHost,
                                          const std::string& version) {
    std::vector<std::wstring> args;
    args.push_back(L"--session");
    args.push_back(std::to_wstring(sessionId));
    args.push_back(L"--nonce");
    args.push_back(Utf8ToWide(nonceHex));
    if (!spn.empty()) {
        args.push_back(L"--spn");
        args.push_back(Utf8ToWide(spn));
    } else if (!proxyHost.empty()) {
        args.push_back(L"--proxy-host");
        args.push_back(Utf8ToWide(proxyHost));
    }
    if (!version.empty()) {
        args.push_back(L"--version");
        args.push_back(Utf8ToWide(version));
    }
    return args;
}

// Квотинг одного аргумента по правилам CommandLineToArgvW (как в
// ChildProcessSupervisor — единый идиом проекта).
static std::wstring QuoteArgW(const std::wstring& arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    for (auto it = arg.begin(); ; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const std::wstring& exe,
                              const std::vector<std::wstring>& args) {
    std::wstring line = QuoteArgW(exe);
    for (const auto& a : args) {
        line.push_back(L' ');
        line.append(QuoteArgW(a));
    }
    return line;
}

}  // namespace detail

// ============================================================================
// Логирование
// ============================================================================
void AuthHelperManager::LogInfo(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Info, "authhelpermgr", msg);
}
void AuthHelperManager::LogWarn(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Warn, "authhelpermgr", msg);
}
void AuthHelperManager::LogError(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Error, "authhelpermgr", msg);
}
void AuthHelperManager::LogDebug(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Debug, "authhelpermgr", msg);
}

// ============================================================================
// Ctor / dtor / MakeConfig
// ============================================================================
AuthHelperManager::AuthHelperManager(AuthHelperManagerConfig cfg,
                                     domain::ports::ILogSink* log)
    : m_cfg(std::move(cfg)), m_log(log) {
    m_cfg.restartBackoffMs     = ClampInt(m_cfg.restartBackoffMs, 100, 60000);
    m_cfg.maxRestartsPerMinute = ClampInt(m_cfg.maxRestartsPerMinute, 1, 600);
    m_cfg.monitorPollMs        = ClampInt(m_cfg.monitorPollMs, 100, 60000);
    if (m_cfg.helperTimeoutMs <= 0) m_cfg.helperTimeoutMs = 5000;
}

AuthHelperManager::~AuthHelperManager() {
    Stop();
}

AuthHelperManagerConfig AuthHelperManager::MakeConfig(
    const domain::ProxyConfig& pc,
    const std::wstring& helperExePath,
    const std::string& helperVersion) {
    AuthHelperManagerConfig cfg;
    cfg.helperExePath  = helperExePath;
    cfg.proxyHost      = WideToUtf8(pc.host);
    const std::string authSpn = WideToUtf8(pc.auth_spn);
    cfg.spn            = detail::DeriveSpn(authSpn, cfg.proxyHost);
    cfg.helperTimeoutMs = pc.helper_timeout_ms > 0 ? pc.helper_timeout_ms : 5000;
    cfg.helperVersion  = helperVersion;
    return cfg;
}

// ============================================================================
// Job Object
// ============================================================================
bool AuthHelperManager::EnsureJob() {
    if (m_job) return true;
    m_job = ::CreateJobObjectW(nullptr, nullptr);
    if (!m_job) {
        LogError("CreateJobObjectW failed, GLE=" + std::to_string(::GetLastError()));
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jinfo{};
    jinfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(m_job, JobObjectExtendedLimitInformation,
                                   &jinfo, sizeof(jinfo))) {
        LogError("SetInformationJobObject failed, GLE=" +
                 std::to_string(::GetLastError()));
        ::CloseHandle(m_job);
        m_job = nullptr;
        return false;
    }
    return true;
}

// ============================================================================
// Session enumeration
// ============================================================================
bool AuthHelperManager::IsSessionEligible(DWORD sessionId) {
    // §6 session-0 isolation: никогда не запускаем helper в сессии служб.
    return sessionId != 0;
}

std::vector<DWORD> AuthHelperManager::EnumerateEligibleSessions() const {
    std::vector<DWORD> result;
    PWTS_SESSION_INFOW pSessions = nullptr;
    DWORD count = 0;
    if (!::WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1,
                                 &pSessions, &count)) {
        LogWarn("WTSEnumerateSessionsW failed, GLE=" +
                std::to_string(::GetLastError()));
        return result;
    }
    WtsMem guard{pSessions};

    for (DWORD i = 0; i < count; ++i) {
        const WTS_SESSION_INFOW& s = pSessions[i];
        const DWORD sid = s.SessionId;
        if (!IsSessionEligible(sid)) continue;
        // Только активные / подключённые интерактивные сессии.
        if (s.State != WTSActive && s.State != WTSConnected) continue;

        // Есть ли залогиненный пользователь? Пробуем получить его токен.
        HANDLE hTok = nullptr;
        if (!::WTSQueryUserToken(sid, &hTok) || !hTok) {
            // Нет пользовательского токена (например login-screen) — пропускаем.
            continue;
        }
        ::CloseHandle(hTok);
        result.push_back(sid);
    }
    return result;
}

// ============================================================================
// Resolve token user (SID + name)
// ============================================================================
bool AuthHelperManager::ResolveTokenUser(HANDLE token, std::string& sidOut,
                                         std::string& nameOut) {
    sidOut.clear();
    nameOut.clear();

    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) return false;
    std::vector<unsigned char> buf(needed);
    if (!::GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
        return false;
    }
    const TOKEN_USER* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
    if (!tu->User.Sid) return false;

    LPWSTR sidStr = nullptr;
    if (::ConvertSidToStringSidW(tu->User.Sid, &sidStr) && sidStr) {
        sidOut = WideToUtf8(sidStr);
        ::LocalFree(sidStr);
    }

    // Имя (DOMAIN\user) — best-effort, только для логов.
    WCHAR name[256];  DWORD nameLen = 256;
    WCHAR domain[256]; DWORD domLen = 256;
    SID_NAME_USE use;
    if (::LookupAccountSidW(nullptr, tu->User.Sid, name, &nameLen,
                            domain, &domLen, &use)) {
        std::wstring full = std::wstring(domain) + L"\\" + name;
        nameOut = WideToUtf8(full);
    }
    return !sidOut.empty();
}

// ============================================================================
// Launch (WTS + CreateProcessAsUser)
// ============================================================================
bool AuthHelperManager::LaunchHelperLocked(DWORD sessionId, Entry& outEntry) {
    // 1. Токен пользователя сессии.
    HANDLE rawUserTok = nullptr;
    if (!::WTSQueryUserToken(sessionId, &rawUserTok) || !rawUserTok) {
        LogWarn("WTSQueryUserToken(session=" + std::to_string(sessionId) +
                ") failed, GLE=" + std::to_string(::GetLastError()) +
                " (skip session)");
        return false;
    }
    ScopedHandle userTok(rawUserTok);

    // 2. Резолв SID/имени (для трекинга/логов) — не фатально при неудаче.
    std::string userSid, userName;
    ResolveTokenUser(userTok.h, userSid, userName);

    // 3. Дублируем в primary-token.
    HANDLE rawPrimary = nullptr;
    if (!::DuplicateTokenEx(userTok.h, TOKEN_ALL_ACCESS, nullptr,
                            SecurityImpersonation, TokenPrimary, &rawPrimary) ||
        !rawPrimary) {
        LogWarn("DuplicateTokenEx(session=" + std::to_string(sessionId) +
                ") failed, GLE=" + std::to_string(::GetLastError()));
        return false;
    }
    ScopedHandle primary(rawPrimary);

    // 4. Environment-блок пользователя (USERPROFILE, Kerberos ticket cache и т.п.).
    ScopedEnvBlock env;
    if (!::CreateEnvironmentBlock(&env.p, userTok.h, FALSE)) {
        // Не фатально — helper всё равно стартует; логируем.
        LogWarn("CreateEnvironmentBlock(session=" + std::to_string(sessionId) +
                ") failed, GLE=" + std::to_string(::GetLastError()) +
                " (continuing without env block)");
        env.p = nullptr;
    }

    // 5. Nonce.
    const std::string nonce = detail::GenerateNonceHex(32);
    if (nonce.empty()) {
        LogError("GenerateNonceHex failed (session=" + std::to_string(sessionId) +
                 ") — skipping launch (fail-closed)");
        return false;
    }

    // 6. Аргументы + командная строка.
    std::vector<std::wstring> args = detail::BuildHelperArgs(
        sessionId, nonce, m_cfg.spn, m_cfg.proxyHost, m_cfg.helperVersion);
    std::wstring cmdline = detail::BuildCommandLine(m_cfg.helperExePath, args);
    std::vector<wchar_t> cmdMutable(cmdline.begin(), cmdline.end());
    cmdMutable.push_back(L'\0');

    // 7. STARTUPINFO: интерактивный desktop пользователя.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    std::wstring desktop = L"winsta0\\default";
    si.lpDesktop = desktop.data();

    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                        CREATE_NEW_PROCESS_GROUP;

    BOOL ok = ::CreateProcessAsUserW(
        primary.h,
        m_cfg.helperExePath.c_str(),
        cmdMutable.data(),
        nullptr, nullptr,
        FALSE,
        flags,
        env.p,
        nullptr,
        &si,
        &pi);
    if (!ok) {
        LogWarn("CreateProcessAsUserW(session=" + std::to_string(sessionId) +
                ") failed, GLE=" + std::to_string(::GetLastError()));
        return false;
    }

    ScopedHandle proc(pi.hProcess);
    ScopedHandle thr(pi.hThread);

    // 8. Прикрепляем к kill-on-close job.
    if (m_job) {
        if (!::AssignProcessToJobObject(m_job, proc.h)) {
            // Не фатально по функциональности, но потеряем гарантию kill-on-close.
            LogWarn("AssignProcessToJobObject(session=" +
                    std::to_string(sessionId) + ", pid=" +
                    std::to_string(pi.dwProcessId) + ") failed, GLE=" +
                    std::to_string(::GetLastError()));
        }
    }

    // 9. Заполняем Entry (владеет process/thread-handle).
    outEntry = Entry{};
    outEntry.hProcess = proc.release();
    outEntry.hThread  = thr.release();
    outEntry.hStdinWrite = nullptr;  // stdin-watchdog не используется (kill через job/Terminate).

    HelperRecord& r = outEntry.rec;
    r.sessionId   = sessionId;
    r.userSid     = userSid;
    r.userName    = userName;
    r.pid         = pi.dwProcessId;
    r.nonce       = nonce;
    r.pipeName    = shared::auth_broker::MakeAuthPipeName(
                        static_cast<std::uint32_t>(sessionId));
    r.launchedSpn = m_cfg.spn;
    r.proxyHost   = m_cfg.proxyHost;
    r.startTime   = std::chrono::steady_clock::now();
    r.hProcess    = outEntry.hProcess;  // не владеющая копия.

    LogInfo("launched helper session=" + std::to_string(sessionId) +
            " pid=" + std::to_string(pi.dwProcessId) +
            " user=" + (userName.empty() ? userSid : userName) +
            " spn=" + (m_cfg.spn.empty() ? "<none>" : m_cfg.spn) +
            " pipe=" + r.pipeName);
    return true;
}

// ============================================================================
// EnsureHelper (launch if missing) — под m_mutex
// ============================================================================
bool AuthHelperManager::EnsureHelperLocked(DWORD sessionId, bool isRelaunch) {
    if (!IsSessionEligible(sessionId)) {
        LogDebug("session " + std::to_string(sessionId) +
                 " not eligible (session 0) — skip");
        return false;
    }

    auto it = m_entries.find(sessionId);
    if (it != m_entries.end() && !isRelaunch) {
        // Уже есть живой (или ещё не отмеченный мёртвым) helper — ничего не делаем.
        if (it->second.hProcess &&
            ::WaitForSingleObject(it->second.hProcess, 0) == WAIT_TIMEOUT) {
            return true;  // жив.
        }
        // Отметка есть, но процесс мёртв — пойдём как relaunch ниже.
    }

    // Anti-storm: сохраняем rate-limit окно из старой записи (если relaunch).
    std::deque<int64_t> ticks;
    bool givingUp = false;
    if (it != m_entries.end()) {
        ticks = it->second.restartTicksMs;
        givingUp = it->second.givingUp;
    }

    if (isRelaunch) {
        if (givingUp) {
            return false;  // уже сдались по этой сессии.
        }
        const int64_t now = NowMonoMs();
        while (!ticks.empty() && (now - ticks.front()) > 60000) {
            ticks.pop_front();
        }
        if (static_cast<int>(ticks.size()) >= m_cfg.maxRestartsPerMinute) {
            LogError("relaunch rate limit reached for session=" +
                     std::to_string(sessionId) + "; giving up until next logon");
            givingUp = true;
            // Обновим/создадим запись с givingUp, без процесса.
            Entry ge;
            ge.restartTicksMs = ticks;
            ge.givingUp = true;
            ge.rec.sessionId = sessionId;
            m_entries[sessionId] = std::move(ge);
            return false;
        }
        ticks.push_back(now);
    }

    // Убираем старую (мёртвую) запись перед запуском.
    if (it != m_entries.end()) {
        if (it->second.hThread)  ::CloseHandle(it->second.hThread);
        if (it->second.hProcess) ::CloseHandle(it->second.hProcess);
        if (it->second.hStdinWrite) ::CloseHandle(it->second.hStdinWrite);
        m_entries.erase(it);
    }

    Entry entry;
    if (!LaunchHelperLocked(sessionId, entry)) {
        // Запуск не удался — сохраняем окно rate-limit для будущих relaunch.
        Entry fail;
        fail.restartTicksMs = ticks;
        fail.givingUp = givingUp;
        fail.rec.sessionId = sessionId;
        m_entries[sessionId] = std::move(fail);
        return false;
    }
    entry.restartTicksMs = ticks;
    entry.givingUp = false;
    m_entries[sessionId] = std::move(entry);
    return true;
}

// ============================================================================
// Kill helper
// ============================================================================
void AuthHelperManager::KillHelperLocked(DWORD sessionId) {
    auto it = m_entries.find(sessionId);
    if (it == m_entries.end()) return;
    Entry& e = it->second;
    if (e.hStdinWrite) {
        // Graceful (если бы использовали stdin-watchdog): закрытие = сигнал выхода.
        ::CloseHandle(e.hStdinWrite);
        e.hStdinWrite = nullptr;
    }
    if (e.hProcess) {
        // Форс-килл: helper должен исчезнуть на logoff немедленно.
        ::TerminateProcess(e.hProcess, 0);
        ::CloseHandle(e.hProcess);
        e.hProcess = nullptr;
    }
    if (e.hThread) {
        ::CloseHandle(e.hThread);
        e.hThread = nullptr;
    }
    LogInfo("killed helper session=" + std::to_string(sessionId) +
            " pid=" + std::to_string(e.rec.pid));
    m_entries.erase(it);
}

void AuthHelperManager::KillAllLocked() {
    for (auto& kv : m_entries) {
        Entry& e = kv.second;
        if (e.hStdinWrite) { ::CloseHandle(e.hStdinWrite); e.hStdinWrite = nullptr; }
        if (e.hProcess)    { ::CloseHandle(e.hProcess);    e.hProcess = nullptr; }
        if (e.hThread)     { ::CloseHandle(e.hThread);     e.hThread = nullptr; }
    }
    m_entries.clear();
}

// ============================================================================
// Monitor loop (crash recovery)
// ============================================================================
void AuthHelperManager::MonitorLoop() {
    for (;;) {
        DWORD wait = ::WaitForSingleObject(m_stopEvent,
                                           static_cast<DWORD>(m_cfg.monitorPollMs));
        if (wait == WAIT_OBJECT_0) {
            break;  // Stop() запрошен.
        }

        // Собираем сессии, чьи helper'ы умерли, но которые ещё активны.
        std::vector<DWORD> dead;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto& kv : m_entries) {
                Entry& e = kv.second;
                if (e.givingUp) continue;
                if (!e.hProcess) { dead.push_back(kv.first); continue; }
                if (::WaitForSingleObject(e.hProcess, 0) == WAIT_OBJECT_0) {
                    DWORD code = 0;
                    ::GetExitCodeProcess(e.hProcess, &code);
                    LogWarn("helper exited unexpectedly session=" +
                            std::to_string(kv.first) + " pid=" +
                            std::to_string(e.rec.pid) + " exit=" +
                            std::to_string(code));
                    dead.push_back(kv.first);
                }
            }
        }

        if (dead.empty()) continue;

        // Какие из мёртвых сессий ещё активны? (только их relaunch'им).
        std::vector<DWORD> eligible = EnumerateEligibleSessions();

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_started.load(std::memory_order_acquire)) break;
            for (DWORD sid : dead) {
                const bool stillActive =
                    std::find(eligible.begin(), eligible.end(), sid) != eligible.end();
                if (!stillActive) {
                    // Сессия ушла (logoff произошёл вне OnSessionChange) — убираем.
                    KillHelperLocked(sid);
                    continue;
                }
                LogInfo("relaunching crashed helper session=" +
                        std::to_string(sid) + " after backoff " +
                        std::to_string(m_cfg.restartBackoffMs) + "ms");
                // Backoff перед relaunch (короткий сон под конец итерации; чтобы не
                // держать mutex во сне — выйдем и поспим ниже). Для простоты и
                // безопасности спим здесь коротко без mutex: снимем ниже.
                EnsureHelperLocked(sid, /*isRelaunch=*/true);
            }
        }

        // Backoff между итерациями relaunch (вне mutex).
        ::WaitForSingleObject(m_stopEvent,
                              static_cast<DWORD>(m_cfg.restartBackoffMs));
        if (::WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;
    }
}

// ============================================================================
// Start / Stop
// ============================================================================
bool AuthHelperManager::Start() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_started.load(std::memory_order_acquire)) {
        return true;
    }
    if (m_cfg.helperExePath.empty()) {
        LogError("helperExePath is empty — cannot start AuthHelperManager");
        return false;
    }

    if (!EnsureJob()) {
        return false;
    }

    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) {
        LogError("CreateEventW(stop) failed, GLE=" +
                 std::to_string(::GetLastError()));
        ::CloseHandle(m_job);
        m_job = nullptr;
        return false;
    }

    // Первичная энумерация + запуск.
    std::vector<DWORD> sessions = EnumerateEligibleSessions();
    LogInfo("initial enumeration: " + std::to_string(sessions.size()) +
            " eligible session(s)");
    for (DWORD sid : sessions) {
        EnsureHelperLocked(sid, /*isRelaunch=*/false);
    }

    m_started.store(true, std::memory_order_release);
    m_monitor = std::thread([this] { MonitorLoop(); });

    LogInfo("AuthHelperManager started with " +
            std::to_string(m_entries.size()) + " helper(s)");
    return true;
}

void AuthHelperManager::Stop() {
    // Сначала гасим монитор (вне m_mutex, чтобы избежать deadlock на join).
    std::thread monitorToJoin;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_started.exchange(false)) {
            // Не запущен: всё равно подчистим job/event на всякий случай.
            if (m_stopEvent) { ::CloseHandle(m_stopEvent); m_stopEvent = nullptr; }
            if (m_job) { ::CloseHandle(m_job); m_job = nullptr; }
            KillAllLocked();
            return;
        }
        if (m_stopEvent) ::SetEvent(m_stopEvent);
        monitorToJoin = std::move(m_monitor);
    }

    if (monitorToJoin.joinable()) {
        monitorToJoin.join();
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    // Закрываем job -> KILL_ON_JOB_CLOSE форс-килит всех детей.
    if (m_job) {
        ::CloseHandle(m_job);
        m_job = nullptr;
    }
    // Закрываем наши дескрипторы записей (процессы уже мертвы через job).
    KillAllLocked();
    if (m_stopEvent) {
        ::CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    LogInfo("AuthHelperManager stopped");
}

// ============================================================================
// OnSessionChange
// ============================================================================
void AuthHelperManager::OnSessionChange(DWORD event, DWORD sessionId) {
    // §6, утверждённые решения: keep-alive на disconnect/lock; kill только на
    // logoff; ensure на logon/connect/unlock.
    switch (event) {
        case WTS_SESSION_LOGON:
        case WTS_CONSOLE_CONNECT:
        case WTS_REMOTE_CONNECT:
        case WTS_SESSION_UNLOCK: {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_started.load(std::memory_order_acquire)) return;
            LogInfo("session-change ensure event=" + std::to_string(event) +
                    " session=" + std::to_string(sessionId));
            EnsureHelperLocked(sessionId, /*isRelaunch=*/false);
            break;
        }
        case WTS_SESSION_LOGOFF: {
            std::lock_guard<std::mutex> lk(m_mutex);
            LogInfo("session-change LOGOFF session=" + std::to_string(sessionId) +
                    " — killing helper");
            KillHelperLocked(sessionId);
            break;
        }
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT:
        case WTS_SESSION_LOCK:
        default:
            // Keep-alive: сессия ещё залогинена (или незначащее событие).
            LogDebug("session-change keep-alive event=" + std::to_string(event) +
                     " session=" + std::to_string(sessionId));
            break;
    }
}

// ============================================================================
// Lookup / params
// ============================================================================
bool AuthHelperManager::TryGet(std::uint32_t sessionId, HelperRecord& out) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_entries.find(static_cast<DWORD>(sessionId));
    if (it == m_entries.end()) return false;
    if (!it->second.hProcess) return false;  // мёртвая заглушка (rate-limit/fail).
    out = it->second.rec;
    return true;
}

bool AuthHelperManager::BuildBrokeredParams(
    std::uint32_t sessionId,
    infrastructure::BrokeredAuthParams& out) const {
    HelperRecord r;
    if (!TryGet(sessionId, r)) return false;
    out = infrastructure::BrokeredAuthParams{};
    out.session_id          = r.sessionId;
    out.expected_nonce      = r.nonce;
    out.spn                 = r.launchedSpn;
    out.proxy_host          = r.proxyHost;
    out.helper_timeout_ms   = m_cfg.helperTimeoutMs;
    out.expected_server_pid = r.pid;
    // correlation_id_override оставляем пустым (провайдер сгенерирует сам).
    return true;
}

std::size_t AuthHelperManager::HelperCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::size_t n = 0;
    for (const auto& kv : m_entries) {
        if (kv.second.hProcess) ++n;
    }
    return n;
}

}  // namespace auth
}  // namespace infrastructure
}  // namespace tcp_redirector
