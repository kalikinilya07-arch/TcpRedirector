#pragma once

//
// AsyncLogger — асинхронная система логирования.
// - Асинхронная очередь (queue + mutex + cv + WriterThread)
// - Ring buffer на 2000 записей для IPC get_logs
// - Batch pop — WriterThread забирает все сообщения разом (swap)
// - Ротация файлов при превышении maxSizeMB
// - Цветной вывод в консоль
// - Listener-механизм для подписки GUI
// - Потокобезопасен
//

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <array>
#include <functional>
#include <unordered_map>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <memory>
#include <windows.h>
#include "../../domain/ports/IConnectionMonitor.h"
#include "LogRotator.h"

namespace tcp_redirector {
namespace infrastructure {

class Logger : public domain::ports::ILogSink {
public:
    Logger();
    ~Logger() override;

    // Инициализация
    bool Initialize(const std::filesystem::path& log_dir,
                    domain::LogLevel level = domain::LogLevel::Info,
                    size_t max_file_size_mb = 10,
                    size_t max_files = 5);

    // --- Scheduled rotation ---
    // Call after Initialize() to enable time-based rotation.
    // Settings come from config.json (log_rotation section), not UI.
    void EnableScheduledRotation(const LogRotator::Settings& settings);
    void Shutdown();

    // --- ILogSink interface ---
    void Log(domain::LogLevel level, const std::string& logger,
             const std::string& message) override;

    void Log(domain::LogLevel level, const std::string& logger,
             const std::string& message,
             const std::string& file, int line,
             const std::string& function);

    void SetLevel(domain::LogLevel level) override;
    domain::LogLevel GetLevel() const override;

    void SetOnLogEntry(LogCallback callback) override;
    std::vector<domain::LogEntry> GetRecentEntries(size_t max_count = 100) const override;

    // --- Listener mechanism (push model) ---
    using ListenerCallback = std::function<void(const domain::LogEntry&)>;
    uint64_t RegisterListener(ListenerCallback callback);
    void UnregisterListener(uint64_t listener_id);

    // Convenience methods (совместимость)
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
    void WriterThread();
    bool OpenLogFile();
    void RotateLogFile();
    std::string FormatLogMessage(const domain::LogEntry& entry) const;
    void WriteColorConsole(const domain::LogEntry& entry) const;
    void AddToRingBuffer(const domain::LogEntry& entry);
    void NotifyListeners(const domain::LogEntry& entry);

    // Async queue
    struct AsyncQueue {
        std::queue<domain::LogEntry> queue;
        std::mutex mutex;
        std::condition_variable cv;
    };
    AsyncQueue m_asyncQueue;
    std::thread m_writerThread;
    std::atomic<bool> m_running{false};

    // Level
    std::atomic<domain::LogLevel> m_currentLevel{domain::LogLevel::Info};

    // File
    std::FILE* m_file = nullptr;
    std::filesystem::path m_logDir;
    std::filesystem::path m_logPath;
    size_t m_maxFileSize = 10 * 1024 * 1024;
    size_t m_maxFiles = 5;
    std::mutex m_fileMutex;

    // Ring buffer for IPC get_logs
    static constexpr size_t RING_BUFFER_SIZE = 2000;
    std::array<domain::LogEntry, RING_BUFFER_SIZE> m_ringBuffer;
    std::atomic<size_t> m_ringIndex{0};
    mutable std::mutex m_ringMutex;

    // LogCallback (ILogSink совместимость)
    LogCallback m_logCallback;
    mutable std::mutex m_callbackMutex;

    // Listeners (push model)
    std::unordered_map<uint64_t, ListenerCallback> m_listeners;
    uint64_t m_nextListenerId = 1;
    mutable std::mutex m_listenersMutex;

    // Scheduled log rotation (separate thread with scheduler)
    std::unique_ptr<LogRotator> m_rotator;
};

} // namespace infrastructure
} // namespace tcp_redirector