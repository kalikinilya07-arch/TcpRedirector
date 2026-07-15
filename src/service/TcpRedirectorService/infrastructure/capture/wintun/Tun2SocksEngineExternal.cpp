/**
 * @file Tun2SocksEngineExternal.cpp
 * @brief WP12 — реализация Tun2SocksEngineExternal.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "Tun2SocksEngineExternal.h"

#include "../../process/ChildProcessSupervisor.h"
#include "../../paths/AppPaths.h"
#include "../../utf8_convert.h"

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

// ============================================================================
// Хелперы
// ============================================================================

namespace {

//! Регистронезависимая проверка «содержит подстроку» — для скана уровня строки.
bool ContainsIgnoreCase(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    std::string lc; lc.reserve(hay.size());
    for (char c : hay) lc.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    std::string nl; for (const char* p = needle; *p; ++p) nl.push_back(static_cast<char>(std::tolower((unsigned char)*p)));
    return lc.find(nl) != std::string::npos;
}

//! Резолвит путь к бинарнику: если абсолютный — as-is; если относительный —
//! от директории EXE сервиса.  Возвращает пустую строку, если файл не найден
//! (или доступ к <exeDir> не получен).
std::wstring ResolveExecutable(const std::string& raw_utf8,
                               std::string* outError) {
    if (raw_utf8.empty()) {
        if (outError) *outError = "external_engine.executable is empty";
        return {};
    }
    std::wstring raw = Utf8ToWide(raw_utf8);
    std::filesystem::path p(raw);
    if (!p.is_absolute()) {
        try {
            std::wstring exeDir = paths::GetExecutableDirectoryW();
            p = std::filesystem::path(exeDir) / p;
        } catch (const std::exception& e) {
            if (outError) *outError = std::string("GetExecutableDirectoryW: ") + e.what();
            return {};
        }
    }
    // Нормализация (весёлые «./», «..» и т.п.).
    std::error_code ec;
    auto norm = std::filesystem::weakly_canonical(p, ec);
    if (!ec) p = norm;

    if (!std::filesystem::exists(p, ec)) {
        if (outError) *outError = "executable not found: " + WideToUtf8(p.wstring());
        return {};
    }
    return p.wstring();
}

} // namespace

// ============================================================================
// Логгирование
// ============================================================================

void Tun2SocksEngineExternal::LogInfo(const std::string& m) const {
    if (m_log) m_log->Log(domain::LogLevel::Info,  "tun2socks", m);
}
void Tun2SocksEngineExternal::LogWarn(const std::string& m) const {
    if (m_log) m_log->Log(domain::LogLevel::Warn,  "tun2socks", m);
}
void Tun2SocksEngineExternal::LogError(const std::string& m) const {
    if (m_log) m_log->Log(domain::LogLevel::Error, "tun2socks", m);
}
void Tun2SocksEngineExternal::LogDebug(const std::string& m) const {
    if (m_log) m_log->Log(domain::LogLevel::Debug, "tun2socks", m);
}

// ============================================================================
// Ctor / dtor
// ============================================================================

Tun2SocksEngineExternal::Tun2SocksEngineExternal(
        std::wstring adapter_name,
        infrastructure::ExternalEngineSettings ext,
        domain::ports::ILogSink* log)
    : m_adapterName(std::move(adapter_name)),
      m_ext(std::move(ext)),
      m_log(log) {
}

Tun2SocksEngineExternal::~Tun2SocksEngineExternal() {
    Stop();
}

// ============================================================================
// Child event callbacks
// ============================================================================

void Tun2SocksEngineExternal::OnChildStdout(const std::string& line) {
    // Известные шумные паттерны xjasonlyu/tun2socks — DEBUG.
    // Всё остальное — INFO.  Ошибки на stdout встречаются крайне редко,
    // но если строка явно содержит "error"/"fatal" — WARN (не ERROR,
    // чтобы не спамить в error-уровень логгера согласно guardrail'у).
    if (ContainsIgnoreCase(line, "error") || ContainsIgnoreCase(line, "fatal")) {
        LogWarn(line);
    } else if (ContainsIgnoreCase(line, "debug") || ContainsIgnoreCase(line, "trace")) {
        LogDebug(line);
    } else {
        LogInfo(line);
    }
    // Best-effort парсинг счётчиков не выполняется: у xjasonlyu/tun2socks
    // нет стабильного machine-readable вывода.  Счётчики останутся 0 —
    // это осознанное решение по §6.12 (см. header).
}

void Tun2SocksEngineExternal::OnChildStderr(const std::string& line) {
    // По конвенции Go-программ, все logs идут на stderr.  Клэмпим:
    //   • "debug"/"trace" → DEBUG
    //   • "info"          → INFO
    //   • "warn"          → WARN
    //   • всё остальное (в т.ч. "error"/"fatal") → WARN (не ERROR).
    if (ContainsIgnoreCase(line, "debug") || ContainsIgnoreCase(line, "trace")) {
        LogDebug(line);
    } else if (ContainsIgnoreCase(line, "info")) {
        LogInfo(line);
    } else {
        // Не эскалируем до ERROR — child может быть шумным, а нам нужно
        // сохранить error-уровень для реальных failures в сервисе.
        LogWarn(line);
    }
}

void Tun2SocksEngineExternal::OnChildExit(DWORD exit_code) {
    if (m_ext.restart_on_crash) {
        LogWarn("tun2socks exited (code=" + std::to_string(exit_code)
                + "); supervisor will restart after backoff="
                + std::to_string(m_ext.restart_backoff_ms) + "ms");
    } else {
        LogWarn("tun2socks exited (code=" + std::to_string(exit_code)
                + "); restart_on_crash=false, will not restart");
    }
    m_activeFlows.store(0, std::memory_order_relaxed);
}

// ============================================================================
// Start
// ============================================================================

bool Tun2SocksEngineExternal::Start(std::string* outError) {
    if (m_running.load(std::memory_order_acquire)) {
        return true;
    }

    // 1. Резолвим бинарник.
    std::string err;
    std::wstring exe = ResolveExecutable(m_ext.executable, &err);
    if (exe.empty()) {
        if (outError) *outError = err;
        return false;
    }

    // 2. Собираем argv.  Порядок:
    //      --device   wintun://<adapter_name>
    //      --proxy    socks5://<socks5_listen>
    //      --loglevel info
    //      <extra_args...>
    //
    // ВАЖНО (T8): xjasonlyu/tun2socks использует Go-пакет pflag, где ДЛИННЫЕ
    // опции требуют ДВОЙНОГО дефиса (`--device`, `--proxy`, `--loglevel`), а
    // одиночный дефис — это ТОЛЬКО шорткаты (`-d`, `-p`).  Раньше передавались
    // `-device`/`-proxy`/`-loglevel` (одиночный дефис): pflag парсил их как
    // шорткаты, `-loglevel` ловился как `-l` → «unknown shorthand flag 'l'»,
    // tun2socks падал с кодом 2 и супервизор уходил в рестарт-цикл до лимита
    // («child restart rate limit reached») — external-режим не работал вовсе.
    process::ChildProcessConfig cfg;
    cfg.executable = exe;
    cfg.args.push_back(L"--device");
    cfg.args.push_back(L"wintun://" + m_adapterName);
    cfg.args.push_back(L"--proxy");
    cfg.args.push_back(L"socks5://" + Utf8ToWide(m_ext.socks5_listen));
    cfg.args.push_back(L"--loglevel");
    cfg.args.push_back(L"info");
    for (const auto& a : m_ext.extra_args) {
        if (!a.empty()) cfg.args.push_back(Utf8ToWide(a));
    }

    // 3. Working directory — <exeDir>.  Ребёнок тогда сможет по-относительно
    //    подхватить wintun.dll (если он там же лежит) — на самом деле дочерний
    //    tun2socks ищет wintun.dll в своей process directory и в PATH; наш
    //    exeDir туда попадает автоматически.
    try {
        cfg.working_directory = paths::GetExecutableDirectoryW();
    } catch (...) {
        // Не критично; работаем с наследованным cwd.
        cfg.working_directory.clear();
    }

    cfg.restart_on_crash        = m_ext.restart_on_crash;
    cfg.restart_backoff_ms      = m_ext.restart_backoff_ms;
    // max_restarts_per_minute — default 6 из ChildProcessConfig, не переопределяем.
    // graceful_stop_timeout_ms  — default 3000, устраивает.

    // 4. Callbacks.
    process::ChildProcessCallbacks cb;
    cb.on_stdout = [this](const std::string& l){ OnChildStdout(l); };
    cb.on_stderr = [this](const std::string& l){ OnChildStderr(l); };
    cb.on_exit   = [this](DWORD c){ OnChildExit(c); };

    // 5. Спавн супервизора.
    m_supervisor = std::make_unique<process::ChildProcessSupervisor>(
        std::move(cfg), std::move(cb), m_log);

    std::string sup_err;
    if (!m_supervisor->Start(&sup_err)) {
        if (outError) *outError = "ChildProcessSupervisor::Start failed: " + sup_err;
        m_supervisor.reset();
        return false;
    }

    m_running.store(true, std::memory_order_release);
    LogInfo("external engine started: adapter='"
            + WideToUtf8(m_adapterName)
            + "' proxy=socks5://" + m_ext.socks5_listen
            + " pid=" + std::to_string(m_supervisor->ChildPid()));
    return true;
}

// ============================================================================
// Stop
// ============================================================================

void Tun2SocksEngineExternal::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (m_supervisor) {
        m_supervisor->Stop();
        m_supervisor.reset();
    }
    m_rxBytes.store(0, std::memory_order_relaxed);
    m_txBytes.store(0, std::memory_order_relaxed);
    m_activeFlows.store(0, std::memory_order_relaxed);
    LogInfo("external engine stopped");
}

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
