#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <windows.h>
#include "../../domain/ports/IConnectionMonitor.h"

namespace tcp_redirector {
namespace infrastructure {

class Logger : public domain::ports::ILogSink {
public:
    Logger() = default;

    bool Initialize(const std::filesystem::path& log_dir,
                    domain::LogLevel level = domain::LogLevel::Info,
                    size_t max_file_size_mb = 50,
                    size_t max_files = 10) {
        try {
            std::filesystem::create_directories(log_dir);
            m_logDir = log_dir;
            m_currentLevel = level;
            m_maxFileSize = max_file_size_mb * 1024 * 1024;
            m_maxFiles = max_files;

            OpenLogFile();
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void Shutdown() {
        std::unique_lock lock(m_mutex);
        if (m_file.is_open()) {
            m_file.close();
        }
    }

    void Log(domain::LogLevel level, const std::string& logger_name,
             const std::string& message) override {
        if (static_cast<int>(level) < static_cast<int>(m_currentLevel)) return;

        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::ostringstream ss;
        ss << "[" << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << ms.count() << "]"
           << "[" << LevelToString(level) << "]"
           << "[" << logger_name << "] "
           << message;

        std::string formatted = ss.str();

        // Output to debugger (works in both debug and release)
        OutputDebugStringA((formatted + "\n").c_str());

        // Write to file
        {
            std::unique_lock lock(m_mutex);
            if (m_file.is_open()) {
                m_file << formatted << std::endl;
                m_file.flush();
                CheckRotation();
            }
        }

        // Store for GUI
        domain::LogEntry entry;
        entry.timestamp = now;
        entry.level = level;
        entry.logger = logger_name;
        entry.message = message;

        {
            std::unique_lock lock(m_entriesMutex);
            m_recentEntries.push_back(entry);
            if (m_recentEntries.size() > 1000) {
                m_recentEntries.erase(m_recentEntries.begin());
            }
        }

        // Push to GUI callback
        if (m_logCallback) {
            m_logCallback(entry);
        }
    }

    void SetLevel(domain::LogLevel level) override {
        m_currentLevel = level;
    }

    domain::LogLevel GetLevel() const override { return m_currentLevel; }

    void SetOnLogEntry(LogCallback callback) override {
        m_logCallback = callback;
    }

    std::vector<domain::LogEntry> GetRecentEntries(size_t max_count) const override {
        std::unique_lock lock(m_entriesMutex);
        if (m_recentEntries.size() <= max_count) {
            return m_recentEntries;
        }
        return std::vector<domain::LogEntry>(
            m_recentEntries.end() - static_cast<long long>(max_count),
            m_recentEntries.end());
    }

    // Convenience methods
    void Info(const std::string& logger, const std::string& msg) {
        Log(domain::LogLevel::Info, logger, msg);
    }

    void Debug(const std::string& logger, const std::string& msg) {
        Log(domain::LogLevel::Debug, logger, msg);
    }

    void Trace(const std::string& logger, const std::string& msg) {
        Log(domain::LogLevel::Trace, logger, msg);
    }

    void Warn(const std::string& logger, const std::string& msg) {
        Log(domain::LogLevel::Warn, logger, msg);
    }

    void Error(const std::string& logger, const std::string& msg) {
        Log(domain::LogLevel::Error, logger, msg);
    }

private:
    static const char* LevelToString(domain::LogLevel level) {
        switch (level) {
            case domain::LogLevel::Trace: return "TRACE";
            case domain::LogLevel::Debug: return "DEBUG";
            case domain::LogLevel::Info:  return "INFO ";
            case domain::LogLevel::Warn:  return "WARN ";
            case domain::LogLevel::Error: return "ERROR";
            default: return "?????";
        }
    }

    void OpenLogFile() {
        m_logPath = m_logDir / "tcp_redirector.log";
        m_file.open(m_logPath, std::ios::app);
    }

    void CheckRotation() {
        if (!m_file.is_open()) return;

        auto size = std::filesystem::file_size(m_logPath);
        if (size < m_maxFileSize) return;

        m_file.close();

        // Rotate: remove oldest, shift files
        auto base = m_logDir / "tcp_redirector";
        auto last = m_logDir / ("tcp_redirector." +
            std::to_string(m_maxFiles - 1) + ".log");
        std::filesystem::remove(last);

        for (size_t i = m_maxFiles - 1; i > 0; --i) {
            auto old_name = m_logDir / ("tcp_redirector." +
                std::to_string(i - 1) + ".log");
            auto new_name = m_logDir / ("tcp_redirector." +
                std::to_string(i) + ".log");
            if (std::filesystem::exists(old_name)) {
                std::filesystem::rename(old_name, new_name);
            }
        }

        std::filesystem::rename(m_logPath,
            m_logDir / "tcp_redirector.1.log");
        OpenLogFile();
    }

    std::ofstream m_file;
    std::filesystem::path m_logDir;
    std::filesystem::path m_logPath;
    domain::LogLevel m_currentLevel = domain::LogLevel::Info;
    size_t m_maxFileSize = 50 * 1024 * 1024;
    size_t m_maxFiles = 10;
    mutable std::mutex m_mutex;
    mutable std::mutex m_entriesMutex;
    std::vector<domain::LogEntry> m_recentEntries;
    LogCallback m_logCallback;
};

} // namespace infrastructure
} // namespace tcp_redirector