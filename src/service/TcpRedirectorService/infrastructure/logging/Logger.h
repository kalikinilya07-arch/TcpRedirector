#pragma once

//
// AsyncLogger — асинхронная система логирования.
// - Асинхронная очередь (queue + mutex + cv + WriterThread)
// - Ring buffer на 2000 записей для IPC get_logs
// - Batch pop — WriterThread забирает все сообщения разом (swap)
// - Ротация файлов при превышении maxSizeMB (дефолт 50 МБ)
// - Retention архивов: удаление архивов старше 24 ч и/или при
//   суммарном объёме архивов > 500 МБ (выполняется в WriterThread)
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
#include <cstdint>
#include <windows.h>
#include "../../domain/ports/IConnectionMonitor.h"

namespace tcp_redirector {
namespace infrastructure {

class Logger : public domain::ports::ILogSink {
public:
    Logger();
    ~Logger() override;

    // Инициализация
    bool Initialize(const std::filesystem::path& log_dir,
                    domain::LogLevel level = domain::LogLevel::Info,
                    size_t max_file_size_mb = 50,
                    size_t max_files = 5);
    void Shutdown();

    // Переустановить порог ротации основного лога (МБ) уже после Initialize.
    // Потокобезопасно; применяется к следующей проверке размера файла.
    void SetMaxFileSizeMB(size_t max_file_size_mb);

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
    // Retention-очистка архивов в m_logDir. Вызывается из WriterThread
    // (не в горячем пути). Удаляет архивы старше RETENTION_MAX_AGE и, если
    // суммарный объём архивов превышает RETENTION_MAX_TOTAL_BYTES, — самые
    // старые архивы, пока объём не станет допустимым. Активный лог не трогается.
    void CleanupArchives();
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

    // Счётчик записей после последней retention-очистки (для периодического
    // запуска CleanupArchives в WriterThread без блокировки горячего пути).
    uint64_t m_entriesSinceCleanup = 0;
    static constexpr uint64_t CLEANUP_EVERY_N_ENTRIES = 5000;

    // Level
    std::atomic<domain::LogLevel> m_currentLevel{domain::LogLevel::Info};

    // File
    std::FILE* m_file = nullptr;
    std::filesystem::path m_logDir;
    std::filesystem::path m_logPath;
    std::atomic<size_t> m_maxFileSize{50ULL * 1024ULL * 1024ULL};
    size_t m_maxFiles = 5;
    std::mutex m_fileMutex;

    // --- Retention (Задача 3) ---
    // Базовое имя активного лога (без пути) — исключается из retention.
    static constexpr const wchar_t* LOG_BASENAME = L"tcp_redirector.log";
    // Префикс/суффикс архивных файлов: tcp_redirector.<stamp>.log
    static constexpr const wchar_t* ARCHIVE_PREFIX = L"tcp_redirector.";
    static constexpr const wchar_t* ARCHIVE_SUFFIX = L".log";
    // Максимальный возраст архива — 24 часа (в 100-нс интервалах FILETIME).
    static constexpr uint64_t RETENTION_MAX_AGE_100NS =
        24ULL * 60ULL * 60ULL * 10'000'000ULL;
    // Максимальный суммарный объём архивов — 500 МБ.
    static constexpr uint64_t RETENTION_MAX_TOTAL_BYTES =
        500ULL * 1024ULL * 1024ULL;

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
};

} // namespace infrastructure
} // namespace tcp_redirector