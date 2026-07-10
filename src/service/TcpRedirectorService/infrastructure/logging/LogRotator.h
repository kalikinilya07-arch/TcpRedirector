#pragma once

//
// LogRotator — фоновая ротация логов по расписанию.
//
// Запускается как отдельный поток внутри сервиса.
// Просыпается раз в 60 секунд, проверяет need_rotate.
// При срабатывании:
//   1. Закрывает текущий лог-файл (через callback)
//   2. Переименовывает с добавлением даты
//   3. Сжимает старые логи в zip (если compress=true)
//   4. Удаляет архивы старше max_age_days
//   5. Открывает новый лог-файл (через callback)
//
// Конфигурация только в config.json, в UI не выводится.
//

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <windows.h>

namespace tcp_redirector {
namespace infrastructure {

class LogRotator {
public:
    struct Settings {
        bool        enabled = true;
        std::string schedule = "daily";   // "hourly" | "daily"
        int         hour = 3;             // 0-23 (для daily)
        int         minute = 0;           // 0-59
        int         max_age_days = 30;
        std::filesystem::path archive_dir;
        bool        compress = true;
    };

    // Callback: закрыть текущий лог-файл
    using CloseLogCallback = std::function<void()>;
    // Callback: открыть новый лог-файл
    using OpenLogCallback = std::function<bool()>;
    // Callback: логгирование из ротатора
    using LogCallback = std::function<void(const std::string& msg)>;

    LogRotator() = default;
    ~LogRotator() { Stop(); }

    LogRotator(const LogRotator&) = delete;
    LogRotator& operator=(const LogRotator&) = delete;

    bool Start(const Settings& settings,
               const std::filesystem::path& log_dir,
               const std::filesystem::path& log_path,
               CloseLogCallback close_log,
               OpenLogCallback open_log,
               LogCallback log_callback = nullptr);

    void Stop();

    // Принудительная ротация (для тестирования)
    void ForceRotate();

private:
    void SchedulerThread();
    bool NeedRotate() const;
    void RotateAndArchive();
    void DeleteOldArchives();

    Settings m_settings;
    std::filesystem::path m_logDir;
    std::filesystem::path m_logPath;
    CloseLogCallback m_closeLog;
    OpenLogCallback m_openLog;
    LogCallback m_log;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::chrono::system_clock::time_point m_lastRotate;
    mutable std::mutex m_mutex;
};

} // namespace infrastructure
} // namespace tcp_redirector
