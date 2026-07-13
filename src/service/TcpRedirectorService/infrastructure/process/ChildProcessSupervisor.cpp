/**
 * @file ChildProcessSupervisor.cpp
 * @brief WP12 — реализация ChildProcessSupervisor.
 *
 * См. header для подробного контракта.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "ChildProcessSupervisor.h"

namespace tcp_redirector {
namespace infrastructure {
namespace process {

// ============================================================================
// Utility helpers (internal linkage)
// ============================================================================

namespace {

//! Квотинг одного аргумента по правилам CommandLineToArgvW (Daniel Colascione).
std::wstring QuoteArgW(const std::wstring& arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    for (auto it = arg.begin(); ; ++it) {
        size_t backslashes = 0;
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

std::string WideToUtf8Local(const std::wstring& ws) {
    if (ws.empty()) return {};
    int need = ::WideCharToMultiByte(CP_UTF8, 0,
                                     ws.data(), static_cast<int>(ws.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0,
                          ws.data(), static_cast<int>(ws.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

int64_t NowMonoMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int ClampInt(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi ? hi : v);
}

} // namespace

// ============================================================================
// Logging wrappers (nullptr-safe)
// ============================================================================

void ChildProcessSupervisor::LogInfo(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Info,  "child", msg);
}
void ChildProcessSupervisor::LogWarn(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Warn,  "child", msg);
}
void ChildProcessSupervisor::LogError(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Error, "child", msg);
}
void ChildProcessSupervisor::LogDebug(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Debug, "child", msg);
}

// ============================================================================
// Ctor / dtor
// ============================================================================

ChildProcessSupervisor::ChildProcessSupervisor(ChildProcessConfig cfg,
                                               ChildProcessCallbacks cb,
                                               domain::ports::ILogSink* log)
    : m_cfg(std::move(cfg)),
      m_cb(std::move(cb)),
      m_log(log) {
    m_cfg.restart_backoff_ms       = ClampInt(m_cfg.restart_backoff_ms,       100, 60000);
    m_cfg.graceful_stop_timeout_ms = ClampInt(m_cfg.graceful_stop_timeout_ms, 100, 60000);
    m_cfg.max_restarts_per_minute  = ClampInt(m_cfg.max_restarts_per_minute,    1,   600);
}

ChildProcessSupervisor::~ChildProcessSupervisor() {
    Stop();
}

// ============================================================================
// Reader loop (static)
// ============================================================================

void ChildProcessSupervisor::ReaderLoop(HANDLE pipe_read,
                                        std::function<void(const std::string&)> cb) {
    constexpr DWORD kBuf = 4096;
    char buf[kBuf];
    std::string acc;
    acc.reserve(kBuf);

    for (;;) {
        DWORD read = 0;
        BOOL ok = ::ReadFile(pipe_read, buf, kBuf, &read, nullptr);
        if (!ok || read == 0) {
            if (!acc.empty() && cb) {
                if (acc.back() == '\r') acc.pop_back();
                if (!acc.empty()) cb(acc);
            }
            break;
        }
        acc.append(buf, read);

        // Резать по '\n'.
        size_t start = 0;
        for (size_t i = 0; i < acc.size(); ++i) {
            if (acc[i] == '\n') {
                size_t end = i;
                if (end > start && acc[end - 1] == '\r') --end;
                if (cb) cb(acc.substr(start, end - start));
                start = i + 1;
            }
        }
        if (start > 0) acc.erase(0, start);

        // Аварийная защита от бесконечно длинных строк.
        if (acc.size() > 64 * 1024) {
            if (cb) cb(acc);
            acc.clear();
        }
    }
}

// ============================================================================
// Start
// ============================================================================

bool ChildProcessSupervisor::Start(std::string* outError) {
    if (m_started.load(std::memory_order_acquire)) {
        return true;
    }

    // 1. stop-event (manual-reset).
    m_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop_event) {
        if (outError) *outError = "CreateEventW(stop) failed, GLE="
                                  + std::to_string(::GetLastError());
        return false;
    }

    // 2. Job Object с KILL_ON_JOB_CLOSE.
    m_job = ::CreateJobObjectW(nullptr, nullptr);
    if (!m_job) {
        if (outError) *outError = "CreateJobObjectW failed, GLE="
                                  + std::to_string(::GetLastError());
        ::CloseHandle(m_stop_event); m_stop_event = nullptr;
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jinfo{};
    jinfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(m_job, JobObjectExtendedLimitInformation,
                                   &jinfo, sizeof(jinfo))) {
        if (outError) *outError = "SetInformationJobObject failed, GLE="
                                  + std::to_string(::GetLastError());
        ::CloseHandle(m_job);        m_job        = nullptr;
        ::CloseHandle(m_stop_event); m_stop_event = nullptr;
        return false;
    }

    // 3. Первый спавн.
    std::string err;
    if (!SpawnChild(err)) {
        if (outError) *outError = err;
        // Job закрываем — вместе с ним умрёт всё что успело.
        if (m_job) { ::CloseHandle(m_job); m_job = nullptr; }
        ::CloseHandle(m_stop_event); m_stop_event = nullptr;
        return false;
    }

    // 4. Супервизор.
    m_stop_requested.store(false, std::memory_order_release);
    m_supervisor_thread = std::thread([this]{ SupervisorLoop(); });

    m_started.store(true, std::memory_order_release);
    LogInfo("child supervisor started: '"
            + WideToUtf8Local(m_cfg.executable)
            + "' pid=" + std::to_string(ChildPid()));
    return true;
}

// ============================================================================
// Stop
// ============================================================================

void ChildProcessSupervisor::Stop() {
    if (!m_started.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    m_stop_requested.store(true, std::memory_order_release);
    if (m_stop_event) ::SetEvent(m_stop_event);

    // Пробуем graceful для ТЕКУЩЕЙ генерации ребёнка.  Супервизор в это
    // время сидит либо на WaitForSingleObject(hProc), либо в backoff-сне.
    DWORD  pid   = m_child_pid.load(std::memory_order_acquire);
    HANDLE hProc = m_child_process;  // snapshot; ридеры-треды пайпы уже держат
    if (pid != 0 && hProc && ::WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT) {
        if (!::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)) {
            LogDebug("GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid="
                     + std::to_string(pid) + ") GLE="
                     + std::to_string(::GetLastError())
                     + " — will fall back to job close");
        }
        ::WaitForSingleObject(hProc,
                              static_cast<DWORD>(m_cfg.graceful_stop_timeout_ms));
    }

    // Force-kill гарантированно.
    KillJobAndCloseHandles();

    if (m_supervisor_thread.joinable()) {
        m_supervisor_thread.join();
    }

    if (m_stop_event) {
        ::CloseHandle(m_stop_event);
        m_stop_event = nullptr;
    }
    m_stop_requested.store(false, std::memory_order_release);
    m_child_pid.store(0, std::memory_order_release);
    m_child_alive.store(false, std::memory_order_release);
    m_restart_ticks_ms.clear();
    LogInfo("child supervisor stopped");
}

// ============================================================================
// KillJobAndCloseHandles — закрывает job (ОС форс-килит ребёнка) и всё, что
// относится к per-child генерации.  НЕ трогает stop_event.
// ============================================================================

void ChildProcessSupervisor::KillJobAndCloseHandles() {
    if (m_job) {
        ::CloseHandle(m_job);
        m_job = nullptr;
    }
    // Пайпы закроются, как только ребёнок умрёт → ридеры выйдут.
    JoinReaders();

    if (m_child_thread)  { ::CloseHandle(m_child_thread);  m_child_thread  = nullptr; }
    if (m_child_process) { ::CloseHandle(m_child_process); m_child_process = nullptr; }
    if (m_stdout_read)   { ::CloseHandle(m_stdout_read);   m_stdout_read   = nullptr; }
    if (m_stderr_read)   { ::CloseHandle(m_stderr_read);   m_stderr_read   = nullptr; }
}

void ChildProcessSupervisor::JoinReaders() {
    if (m_stdout_reader.joinable()) m_stdout_reader.join();
    if (m_stderr_reader.joinable()) m_stderr_reader.join();
}

// ============================================================================
// SpawnChild — один спавн; при провале не оставляет per-child хэндлов.
// job (m_job) должен уже быть создан ДО вызова.
// ============================================================================

bool ChildProcessSupervisor::SpawnChild(std::string& err) {
    HANDLE stdoutR = nullptr, stdoutW = nullptr;
    HANDLE stderrR = nullptr, stderrW = nullptr;
    HANDLE nullIn  = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    auto close_h = [](HANDLE h){
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    };

    if (!::CreatePipe(&stdoutR, &stdoutW, &sa, 0)) {
        err = "CreatePipe(stdout) failed, GLE=" + std::to_string(::GetLastError());
        return false;
    }
    if (!::SetHandleInformation(stdoutR, HANDLE_FLAG_INHERIT, 0)) {
        err = "SetHandleInformation(stdout_read) failed, GLE="
              + std::to_string(::GetLastError());
        close_h(stdoutR); close_h(stdoutW);
        return false;
    }

    if (!::CreatePipe(&stderrR, &stderrW, &sa, 0)) {
        err = "CreatePipe(stderr) failed, GLE=" + std::to_string(::GetLastError());
        close_h(stdoutR); close_h(stdoutW);
        return false;
    }
    if (!::SetHandleInformation(stderrR, HANDLE_FLAG_INHERIT, 0)) {
        err = "SetHandleInformation(stderr_read) failed, GLE="
              + std::to_string(::GetLastError());
        close_h(stdoutR); close_h(stdoutW);
        close_h(stderrR); close_h(stderrW);
        return false;
    }

    nullIn = ::CreateFileW(L"NUL",
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa,
                           OPEN_EXISTING,
                           0,
                           nullptr);
    if (nullIn == INVALID_HANDLE_VALUE) {
        err = "CreateFileW(NUL) failed, GLE=" + std::to_string(::GetLastError());
        close_h(stdoutR); close_h(stdoutW);
        close_h(stderrR); close_h(stderrW);
        return false;
    }

    STARTUPINFOW si{};
    si.cb        = sizeof(si);
    si.dwFlags   = STARTF_USESTDHANDLES;
    si.hStdInput  = nullIn;
    si.hStdOutput = stdoutW;
    si.hStdError  = stderrW;
    PROCESS_INFORMATION pi{};

    std::wstring cmdline = BuildCommandLine(m_cfg.executable, m_cfg.args);
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    const wchar_t* cwd = m_cfg.working_directory.empty()
                        ? nullptr
                        : m_cfg.working_directory.c_str();

    DWORD flags = CREATE_NO_WINDOW
                | CREATE_SUSPENDED
                | CREATE_NEW_PROCESS_GROUP;

    BOOL cp_ok = ::CreateProcessW(
        m_cfg.executable.c_str(),
        cmd_buf.data(),
        nullptr, nullptr,
        /*bInheritHandles*/ TRUE,
        flags,
        /*env*/ nullptr,
        cwd,
        &si,
        &pi);

    DWORD cp_gle = ::GetLastError();

    // Write-концы пайпов и NUL нам больше не нужны — их держит ребёнок.
    close_h(stdoutW); stdoutW = nullptr;
    close_h(stderrW); stderrW = nullptr;
    close_h(nullIn);  nullIn = INVALID_HANDLE_VALUE;

    if (!cp_ok) {
        err = "CreateProcessW('" + WideToUtf8Local(m_cfg.executable)
            + "') failed, GLE=" + std::to_string(cp_gle);
        close_h(stdoutR);
        close_h(stderrR);
        return false;
    }

    // Assign to job ДО ResumeThread.  Если ассайн упадёт, всё равно
    // TerminateProcess — иначе ребёнок ускачет из-под нас.
    if (!::AssignProcessToJobObject(m_job, pi.hProcess)) {
        err = "AssignProcessToJobObject failed, GLE="
              + std::to_string(::GetLastError());
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        close_h(stdoutR);
        close_h(stderrR);
        return false;
    }

    if (::ResumeThread(pi.hThread) == (DWORD)-1) {
        err = "ResumeThread failed, GLE=" + std::to_string(::GetLastError());
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        close_h(stdoutR);
        close_h(stderrR);
        return false;
    }

    // --- Успех: сохраняем per-child хэндлы + запускаем ридеров.

    m_child_process = pi.hProcess;
    m_child_thread  = pi.hThread;
    m_stdout_read   = stdoutR;
    m_stderr_read   = stderrR;
    m_child_pid.store(pi.dwProcessId, std::memory_order_release);
    m_child_alive.store(true, std::memory_order_release);

    // Ридер-треды.  Копируем HANDLE и callback в лямбду.
    HANDLE hStdout = m_stdout_read;
    HANDLE hStderr = m_stderr_read;
    auto cbOut = m_cb.on_stdout;
    auto cbErr = m_cb.on_stderr;
    m_stdout_reader = std::thread([hStdout, cbOut]{ ReaderLoop(hStdout, cbOut); });
    m_stderr_reader = std::thread([hStderr, cbErr]{ ReaderLoop(hStderr, cbErr); });

    LogDebug("child spawned: pid=" + std::to_string(pi.dwProcessId));
    return true;
}

// ============================================================================
// SupervisorLoop — вертится, пока Stop не запросит выход.
// ============================================================================

void ChildProcessSupervisor::SupervisorLoop() {
    for (;;) {
        // Ждём либо смерти ребёнка, либо stop_event.
        HANDLE waits[2] = { m_child_process, m_stop_event };
        DWORD rc = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);

        // Собираем exit-code (даже если stop_event — фиксируем текущий статус).
        DWORD exit_code = 0;
        if (m_child_process) {
            ::GetExitCodeProcess(m_child_process, &exit_code);
        }
        // STILL_ACTIVE = 259 — значит процесс ещё жив; в контексте
        // stop_event это нормально.
        bool child_still_alive = (exit_code == STILL_ACTIVE);

        // Дождаться, пока ридеры выйдут (пайпы закроются, когда ребёнок умрёт).
        // Если ребёнок ещё жив (stop_event триггернуло раньше), джойн будет ждать
        // после того, как Stop() убьёт job.  Но мы не в Stop() контексте —
        // просто вернём управление, Stop() дождётся нас через join().
        if (!child_still_alive) {
            JoinReaders();

            // Закрыть per-child хэндлы (кроме job, он остаётся жить между
            // спавнами) — но только если ребёнок реально умер.
            if (m_child_thread)  { ::CloseHandle(m_child_thread);  m_child_thread  = nullptr; }
            if (m_child_process) { ::CloseHandle(m_child_process); m_child_process = nullptr; }
            if (m_stdout_read)   { ::CloseHandle(m_stdout_read);   m_stdout_read   = nullptr; }
            if (m_stderr_read)   { ::CloseHandle(m_stderr_read);   m_stderr_read   = nullptr; }
            m_child_alive.store(false, std::memory_order_release);

            if (m_cb.on_exit) {
                try { m_cb.on_exit(exit_code); } catch (...) {}
            }
        }

        if (m_stop_requested.load(std::memory_order_acquire)) {
            // Stop() позаботится об остальном (закрытии job'а, если ещё не закрыт,
            // и о нашем join'е).  Просто выходим.
            return;
        }

        if (!m_cfg.restart_on_crash) {
            LogWarn("child exited (code=" + std::to_string(exit_code)
                    + "); restart_on_crash=false, supervisor idle until Stop()");
            // Ждём stop_event.
            ::WaitForSingleObject(m_stop_event, INFINITE);
            return;
        }

        // Rate-limit rolling window (60 s).
        const int64_t now = NowMonoMs();
        const int64_t window_start = now - 60 * 1000;
        while (!m_restart_ticks_ms.empty()
               && m_restart_ticks_ms.front() < window_start) {
            m_restart_ticks_ms.pop_front();
        }
        if (static_cast<int>(m_restart_ticks_ms.size()) >= m_cfg.max_restarts_per_minute) {
            LogError("child restart rate limit reached ("
                     + std::to_string(m_cfg.max_restarts_per_minute)
                     + "/60s); giving up");
            // До следующего внешнего Start() — сидим на stop_event.
            ::WaitForSingleObject(m_stop_event, INFINITE);
            return;
        }
        m_restart_ticks_ms.push_back(now);

        // Backoff (прерывается stop_event'ом).
        DWORD wait_rc = ::WaitForSingleObject(
            m_stop_event,
            static_cast<DWORD>(m_cfg.restart_backoff_ms));
        if (wait_rc == WAIT_OBJECT_0) {
            return; // Stop() пришёл во время backoff
        }

        // Респавн.
        std::string err;
        if (!SpawnChild(err)) {
            LogError("child respawn failed: " + err
                     + "; will retry after backoff");
            // На следующей итерации таймер отсчитается ещё раз.
            // Чтобы не крутиться в тайт-цикле — доп. пауза.
            ::WaitForSingleObject(m_stop_event,
                                  static_cast<DWORD>(m_cfg.restart_backoff_ms));
            if (m_stop_requested.load(std::memory_order_acquire)) return;
            continue;
        }
        LogInfo("child respawned: pid=" + std::to_string(ChildPid()));
    }
}

} // namespace process
} // namespace infrastructure
} // namespace tcp_redirector
