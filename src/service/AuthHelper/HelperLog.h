#pragma once

/**
 * @file HelperLog.h
 * @brief Минимальный user-writable логгер для TcpRedirectorAuthHelper.exe.
 *
 * Variant 4b, Phase 3 (см. plans/kerberos_per_user_auth_helper_plan.md §1.4).
 *
 * ПОЧЕМУ ОТДЕЛЬНЫЙ ЛОГГЕР, А НЕ infrastructure::Logger:
 *   - Helper запускается ПОД ОБЫЧНЫМ пользователем (без прав администратора).
 *     Полноценный AsyncLogger службы пишет в ProgramData/каталог службы, куда у
 *     непривилегированного пользователя обычно нет прав на запись.
 *   - Helper — крошечный headless-процесс без WinDivert/wintun/lwIP; тянуть в
 *     него весь Logger.cpp (ротация, ring buffer, listeners, IPC) избыточно.
 *
 * КУДА ПИШЕМ:
 *   %LOCALAPPDATA%\TcpRedirector\auth_helper_<sessionId>.log
 *   (гарантированно user-writable; отдельный файл на сессию во избежание гонок
 *    между helper'ами разных RDP-пользователей). Если %LOCALAPPDATA% недоступен —
 *   fallback на %TEMP%. Формат строки совместим по духу с основным логом:
 *   "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [auth_helper] message".
 *
 * Потокобезопасен (единственный mutex вокруг записи в файл + консоль).
 */

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <filesystem>

namespace tcp_redirector {
namespace auth_helper {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

inline const char* LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

/**
 * @brief Простой синглтон-логгер помощника.
 *
 * Один экземпляр на процесс; инициализируется в main() после определения
 * sessionId, чтобы имя файла содержало сессию.
 */
class HelperLog {
public:
    static HelperLog& Instance() {
        static HelperLog inst;
        return inst;
    }

    /**
     * @brief Открыть лог-файл в user-writable каталоге для данной сессии.
     * @param sessionId  WTS session id (используется в имени файла).
     * @param toConsole  Дублировать ли вывод в stderr (для интерактивной отладки).
     */
    void Init(unsigned long sessionId, bool toConsole) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_toConsole = toConsole;

        std::filesystem::path dir = ResolveUserLogDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);  // best-effort

        wchar_t name[64];
        swprintf(name, 64, L"auth_helper_%lu.log", sessionId);
        m_path = dir / name;

        // Открываем в режиме дозаписи; файл создаётся при отсутствии.
        m_file = _wfopen(m_path.c_str(), L"a, ccs=UTF-8");
        m_minLevel = LogLevel::Debug;
    }

    void SetLevel(LogLevel level) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_minLevel = level;
    }

    std::wstring Path() const { return m_path.wstring(); }

    void Log(LogLevel level, const std::string& msg) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (static_cast<int>(level) < static_cast<int>(m_minLevel)) return;

        std::string line = Timestamp();
        line += " [";
        line += LevelName(level);
        line += "] [auth_helper] ";
        line += msg;
        line += "\n";

        if (m_file) {
            fputs(line.c_str(), m_file);
            fflush(m_file);
        }
        if (m_toConsole) {
            fputs(line.c_str(), stderr);
        }
    }

    void Trace(const std::string& m) { Log(LogLevel::Trace, m); }
    void Debug(const std::string& m) { Log(LogLevel::Debug, m); }
    void Info(const std::string& m)  { Log(LogLevel::Info, m); }
    void Warn(const std::string& m)  { Log(LogLevel::Warn, m); }
    void Error(const std::string& m) { Log(LogLevel::Error, m); }

    ~HelperLog() {
        if (m_file) {
            fclose(m_file);
            m_file = nullptr;
        }
    }

private:
    HelperLog() = default;
    HelperLog(const HelperLog&) = delete;
    HelperLog& operator=(const HelperLog&) = delete;

    static std::filesystem::path ResolveUserLogDir() {
        // Предпочитаем %LOCALAPPDATA%\TcpRedirector (user-writable, per-user).
        wchar_t* buf = nullptr;
        size_t len = 0;
        std::filesystem::path base;
        if (_wdupenv_s(&buf, &len, L"LOCALAPPDATA") == 0 && buf && buf[0]) {
            base = buf;
        }
        if (buf) { free(buf); buf = nullptr; }

        if (base.empty()) {
            // Fallback: %TEMP%.
            if (_wdupenv_s(&buf, &len, L"TEMP") == 0 && buf && buf[0]) {
                base = buf;
            }
            if (buf) { free(buf); buf = nullptr; }
        }
        if (base.empty()) {
            base = L".";  // последний резерв — текущий каталог.
        }
        return base / L"TcpRedirector";
    }

    static std::string Timestamp() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[32];
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds);
        return buf;
    }

    std::mutex m_mutex;
    std::FILE* m_file = nullptr;
    std::filesystem::path m_path;
    LogLevel m_minLevel = LogLevel::Debug;
    bool m_toConsole = false;
};

// Удобные свободные функции.
inline void LogTrace(const std::string& m) { HelperLog::Instance().Trace(m); }
inline void LogDebug(const std::string& m) { HelperLog::Instance().Debug(m); }
inline void LogInfo(const std::string& m)  { HelperLog::Instance().Info(m); }
inline void LogWarn(const std::string& m)  { HelperLog::Instance().Warn(m); }
inline void LogError(const std::string& m) { HelperLog::Instance().Error(m); }

}  // namespace auth_helper
}  // namespace tcp_redirector
