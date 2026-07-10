#include "LogRotator.h"
#include <ctime>
#include <sstream>
#include <iomanip>

namespace tcp_redirector {
namespace infrastructure {

// ---- Start / Stop ----

bool LogRotator::Start(const Settings& settings,
                        const std::filesystem::path& log_dir,
                        const std::filesystem::path& log_path,
                        CloseLogCallback close_log,
                        OpenLogCallback open_log,
                        LogCallback log_callback) {
    if (m_running.load(std::memory_order_acquire)) {
        return false; // already running
    }

    m_settings = settings;
    m_logDir = log_dir;
    m_logPath = log_path;
    m_closeLog = std::move(close_log);
    m_openLog = std::move(open_log);
    m_log = std::move(log_callback);

    if (!m_settings.enabled) {
        if (m_log) m_log("Scheduled log rotation is disabled in config");
        return true; // not an error — just disabled
    }

    // Default archive dir
    if (m_settings.archive_dir.empty()) {
        m_settings.archive_dir = m_logDir / L"archive";
    }

    m_lastRotate = std::chrono::system_clock::now();
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&LogRotator::SchedulerThread, this);

    if (m_log) {
        std::ostringstream oss;
        oss << "LogRotator started: schedule=" << m_settings.schedule
            << " hour=" << m_settings.hour
            << " minute=" << m_settings.minute
            << " max_age=" << m_settings.max_age_days << "d"
            << " compress=" << (m_settings.compress ? "yes" : "no");
        m_log(oss.str());
    }

    return true;
}

void LogRotator::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);

    if (m_thread.joinable()) {
        m_thread.join();
    }

    if (m_log) m_log("LogRotator stopped");
}

void LogRotator::ForceRotate() {
    if (!m_settings.enabled) return;
    std::lock_guard lock(m_mutex);
    RotateAndArchive();
}

// ---- Scheduler Thread ----

void LogRotator::SchedulerThread() {
    while (m_running.load(std::memory_order_acquire)) {
        // Sleep 60 seconds, but wake up every second to check m_running
        for (int i = 0; i < 60 && m_running.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!m_running.load(std::memory_order_acquire)) break;

        if (NeedRotate()) {
            std::lock_guard lock(m_mutex);
            RotateAndArchive();
        }
    }
}

bool LogRotator::NeedRotate() const {
    if (!m_settings.enabled) return false;

    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - m_lastRotate).count();

    // Protection: minimum 60 seconds between rotations
    if (elapsed < 60) return false;

    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_s(&now_tm, &now_t);

    if (m_settings.schedule == "hourly") {
        // Rotate at the specified minute of each hour
        return (now_tm.tm_min == m_settings.minute);
    } else {
        // "daily": rotate at specified hour:minute
        return (now_tm.tm_hour == m_settings.hour &&
                now_tm.tm_min == m_settings.minute);
    }
}

// ---- Rotation Logic ----

void LogRotator::RotateAndArchive() {
    try {
        // 1. Close current log file
        if (m_closeLog) {
            m_closeLog();
        }

        // 2. Generate timestamped name
        auto now = std::chrono::system_clock::now();
        std::time_t now_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm;
        localtime_s(&now_tm, &now_t);

        std::ostringstream ts;
        ts << std::setfill('0')
           << std::setw(4) << (now_tm.tm_year + 1900) << "-"
           << std::setw(2) << (now_tm.tm_mon + 1) << "-"
           << std::setw(2) << now_tm.tm_mday << "_"
           << std::setw(2) << now_tm.tm_hour
           << std::setw(2) << now_tm.tm_min;
        std::string timestamp = ts.str();

        auto rotated_path = m_logDir / (L"tcp_redirector_" +
            std::wstring(timestamp.begin(), timestamp.end()) + L".log");

        // 3. Rename current log to timestamped name
        std::error_code ec;
        if (std::filesystem::exists(m_logPath, ec)) {
            std::filesystem::rename(m_logPath, rotated_path, ec);
            if (ec) {
                if (m_log) m_log("Rotate: rename failed: " + ec.message());
            } else {
                if (m_log) m_log("Rotate: renamed to " + std::string(timestamp.begin(), timestamp.end()) + ".log");
            }
        }

        // 4. Compress to zip (if enabled)
        if (m_settings.compress && std::filesystem::exists(rotated_path, ec)) {
            // Ensure archive directory exists
            std::filesystem::create_directories(m_settings.archive_dir, ec);

            auto zip_path = m_settings.archive_dir / (L"logs_" +
                std::wstring(timestamp.begin(), timestamp.end()) + L".zip");

            // Use PowerShell Compress-Archive via CreateProcess
            std::wstring ps_cmd =
                L"powershell.exe -NoProfile -NonInteractive -Command \""
                L"Compress-Archive -Path '"
                + rotated_path.wstring()
                + L"' -DestinationPath '"
                + zip_path.wstring()
                + L"' -Force\"";

            STARTUPINFOW si = { sizeof(si) };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;

            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, ps_cmd.data(), nullptr, nullptr,
                              FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                              &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 30000); // 30s timeout
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);

                // Verify zip was created
                if (std::filesystem::exists(zip_path, ec)) {
                    // Delete the original .log file (now archived)
                    std::filesystem::remove(rotated_path, ec);
                    if (m_log) m_log("Rotate: archived to zip");
                } else {
                    if (m_log) m_log("Rotate: compress failed, keeping .log file");
                }
            } else {
                if (m_log) m_log("Rotate: failed to launch PowerShell for compression");
            }
        } else if (!m_settings.compress) {
            // Move to archive dir without compression
            std::filesystem::create_directories(m_settings.archive_dir, ec);
            auto dest = m_settings.archive_dir / rotated_path.filename();
            std::filesystem::rename(rotated_path, dest, ec);
            if (m_log) m_log("Rotate: moved to archive (no compression)");
        }

        // 5. Delete old archives
        DeleteOldArchives();

        // 6. Re-open log file
        if (m_openLog) {
            m_openLog();
            if (m_log) m_log("Rotate: new log file opened");
        }

        // 7. Record rotation time
        m_lastRotate = now;

    } catch (const std::exception& e) {
        if (m_log) {
            std::string err = "Rotate: exception: ";
            err += e.what();
            m_log(err);
        }
        // Always try to re-open log file even on error
        if (m_openLog) m_openLog();
    }
}

void LogRotator::DeleteOldArchives() {
    std::error_code ec;

    // Scan both archive_dir and log_dir for old files
    std::function<void(const std::filesystem::path&)> scan_dir =
        [&](const std::filesystem::path& dir) {
            if (!std::filesystem::exists(dir, ec)) return;

            auto now = std::chrono::system_clock::now();
            auto cutoff = now - std::chrono::hours(24 * m_settings.max_age_days);
            auto cutoff_tt = std::chrono::system_clock::to_time_t(cutoff);

            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;

                auto ext = entry.path().extension().wstring();
                // Only clean up .zip and rotated .log files (not current log)
                if (ext != L".zip" && ext != L".log") continue;
                if (entry.path().filename() == L"tcp_redirector.log") continue;

                auto ftime = entry.last_write_time(ec);
                if (ec) continue;

                // On MSVC, file_time_type is system_clock::time_point
                // For portability, convert via duration since epoch
                auto ftime_sys = std::chrono::time_point<
                    std::chrono::system_clock,
                    std::chrono::system_clock::duration>(
                    ftime.time_since_epoch());
                if (ftime_sys < cutoff) {
                    std::filesystem::remove(entry.path(), ec);
                    if (!ec && m_log) {
                        std::string name = entry.path().filename().string();
                        m_log("Rotate: deleted old archive: " + name);
                    }
                }
            }
        };

    scan_dir(m_settings.archive_dir);
    scan_dir(m_logDir);
}

} // namespace infrastructure
} // namespace tcp_redirector
