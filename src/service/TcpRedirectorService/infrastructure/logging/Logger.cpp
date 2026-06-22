#include "Logger.h"
#include <iostream>
#include <cstring>

namespace tcp_redirector {
namespace infrastructure {

// ---- Конструктор / Деструктор ----

Logger::Logger() = default;

Logger::~Logger() {
    Shutdown();
}

// ---- Инициализация ----

bool Logger::Initialize(const std::filesystem::path& log_dir,
                         domain::LogLevel level,
                         size_t max_file_size_mb,
                         size_t max_files) {
    try {
        m_logDir = log_dir;
        m_currentLevel.store(level, std::memory_order_relaxed);
        m_maxFileSize = max_file_size_mb * 1024ULL * 1024ULL;
        m_maxFiles = max_files;

        // Пытаемся создать директорию и открыть лог-файл.
        // Если не получается — не фатально, работаем без файлового лога.
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
        if (!ec && OpenLogFile()) {
            // всё хорошо
        } else {
            fprintf(stderr, "[WARN] Logger: cannot open log file (run as Admin), errno=%d\n", errno);
        }

        m_running = true;
        m_writerThread = std::thread(&Logger::WriterThread, this);
        return true;
    } catch (...) {
        return false;
    }
}

void Logger::Shutdown() {
    m_running = false;
    {
        std::lock_guard lock(m_asyncQueue.mutex);
        m_asyncQueue.cv.notify_all();
    }
    if (m_writerThread.joinable()) {
        m_writerThread.join();
    }
    {
        std::lock_guard lock(m_fileMutex);
        if (m_file) {
            std::fclose(m_file);
            m_file = nullptr;
        }
    }
}

// ---- ILogSink: Log ----

void Logger::Log(domain::LogLevel level, const std::string& logger,
                  const std::string& message) {
    if (level < m_currentLevel.load(std::memory_order_relaxed)) return;

    domain::LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    entry.logger = logger;
    entry.message = message;

    {
        std::lock_guard lock(m_asyncQueue.mutex);
        m_asyncQueue.queue.push(std::move(entry));
    }
    m_asyncQueue.cv.notify_one();
}

void Logger::Log(domain::LogLevel level, const std::string& logger,
                  const std::string& message,
                  const std::string& file, int line,
                  const std::string& function) {
    if (level < m_currentLevel.load(std::memory_order_relaxed)) return;

    domain::LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    entry.logger = logger;
    entry.message = message;
    entry.file = file;
    entry.line = line;
    entry.function = function;

    {
        std::lock_guard lock(m_asyncQueue.mutex);
        m_asyncQueue.queue.push(std::move(entry));
    }
    m_asyncQueue.cv.notify_one();
}

// ---- ILogSink: Level ----

void Logger::SetLevel(domain::LogLevel level) {
    m_currentLevel.store(level, std::memory_order_relaxed);
}

domain::LogLevel Logger::GetLevel() const {
    return m_currentLevel.load(std::memory_order_relaxed);
}

// ---- ILogSink: Callback / Recent Entries ----

void Logger::SetOnLogEntry(LogCallback callback) {
    std::lock_guard lock(m_callbackMutex);
    m_logCallback = std::move(callback);
}

std::vector<domain::LogEntry> Logger::GetRecentEntries(size_t max_count) const {
    std::lock_guard lock(m_ringMutex);
    size_t idx = m_ringIndex.load(std::memory_order_relaxed);
    size_t total = (idx < RING_BUFFER_SIZE) ? idx : RING_BUFFER_SIZE;
    if (total == 0) return {};

    std::vector<domain::LogEntry> result;
    size_t start = (total > max_count) ? total - max_count : 0;
    for (size_t i = start; i < total; i++) {
        result.push_back(m_ringBuffer[i]);
    }
    return result;
}

// ---- Listener mechanism ----

uint64_t Logger::RegisterListener(ListenerCallback callback) {
    std::lock_guard lock(m_listenersMutex);
    uint64_t id = m_nextListenerId++;
    m_listeners[id] = std::move(callback);
    return id;
}

void Logger::UnregisterListener(uint64_t listener_id) {
    std::lock_guard lock(m_listenersMutex);
    m_listeners.erase(listener_id);
}

// ---- Writer Thread ----

void Logger::WriterThread() {
    while (m_running) {
        std::vector<domain::LogEntry> batch;
        {
            std::unique_lock lock(m_asyncQueue.mutex);
            m_asyncQueue.cv.wait(lock, [this] {
                return !m_asyncQueue.queue.empty() || !m_running;
            });
            if (!m_running && m_asyncQueue.queue.empty()) break;
            while (!m_asyncQueue.queue.empty()) {
                batch.push_back(std::move(m_asyncQueue.queue.front()));
                m_asyncQueue.queue.pop();
            }
        }

        for (const auto& entry : batch) {
            // 1. Цветной вывод в консоль
            WriteColorConsole(entry);

            // 2. Запись в файл
            std::string formatted = FormatLogMessage(entry);
            {
                std::lock_guard lock(m_fileMutex);
                if (m_file) {
                    std::fputs(formatted.c_str(), m_file);
                    std::fputs("\n", m_file);
                    std::fflush(m_file);

                    long pos = std::ftell(m_file);
                    if (pos > 0 && static_cast<size_t>(pos) >= m_maxFileSize) {
                        RotateLogFile();
                    }
                }
            }

            // 3. ILogSink callback (push to GUI pipe)
            {
                std::lock_guard lock(m_callbackMutex);
                if (m_logCallback) {
                    m_logCallback(entry);
                }
            }

            // 4. Listener'ы
            NotifyListeners(entry);

            // 5. Ring buffer
            AddToRingBuffer(entry);
        }
    }

    // Flush остатки перед shutdown
    std::lock_guard lock(m_fileMutex);
    if (m_file) std::fflush(m_file);
}

// ---- File operations ----

bool Logger::OpenLogFile() {
    m_logPath = m_logDir / L"tcp_redirector.log";
    m_file = _wfopen(m_logPath.c_str(), L"a");
    return (m_file != nullptr);
}

void Logger::RotateLogFile() {
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }

    auto lastPath = m_logDir / (L"tcp_redirector." +
        std::to_wstring(m_maxFiles - 1) + L".log");
    std::error_code ec;
    std::filesystem::remove(lastPath, ec);

    for (size_t i = m_maxFiles - 1; i > 0; --i) {
        auto oldPath = m_logDir / (L"tcp_redirector." +
            std::to_wstring(i - 1) + L".log");
        auto newPath = m_logDir / (L"tcp_redirector." +
            std::to_wstring(i) + L".log");
        if (std::filesystem::exists(oldPath, ec)) {
            std::filesystem::rename(oldPath, newPath, ec);
        }
    }

    auto backupPath = m_logDir / L"tcp_redirector.1.log";
    if (std::filesystem::exists(m_logPath, ec)) {
        std::filesystem::rename(m_logPath, backupPath, ec);
    }

    OpenLogFile();
}

// ---- Formatting ----

std::string Logger::FormatLogMessage(const domain::LogEntry& entry) const {
    auto now = entry.timestamp;
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;

    const char* levelStr = "?????";
    switch (entry.level) {
        case domain::LogLevel::Trace: levelStr = "TRACE"; break;
        case domain::LogLevel::Debug: levelStr = "DEBUG"; break;
        case domain::LogLevel::Info:  levelStr = "INFO "; break;
        case domain::LogLevel::Warn:  levelStr = "WARN "; break;
        case domain::LogLevel::Error: levelStr = "ERROR"; break;
        default: break;
    }

    char timestamp[32] = {0};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&in_time_t));

    char result[4096];
    if (!entry.file.empty()) {
        std::snprintf(result, sizeof(result),
            "[%s.%03d] [%s] [%-12s] %s  (%s:%d)",
            timestamp, (int)ms, levelStr, entry.logger.c_str(),
            entry.message.c_str(), entry.file.c_str(), entry.line);
    } else {
        std::snprintf(result, sizeof(result),
            "[%s.%03d] [%s] [%-12s] %s",
            timestamp, (int)ms, levelStr, entry.logger.c_str(),
            entry.message.c_str());
    }
    return std::string(result);
}

void Logger::WriteColorConsole(const domain::LogEntry& entry) const {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) return;

    WORD color = 0x07;
    switch (entry.level) {
        case domain::LogLevel::Error: color = 0x0C; break;
        case domain::LogLevel::Warn:  color = 0x0E; break;
        case domain::LogLevel::Info:  color = 0x0F; break;
        case domain::LogLevel::Debug: color = 0x08; break;
        case domain::LogLevel::Trace: color = 0x07; break;
        default: break;
    }

    SetConsoleTextAttribute(hConsole, color);
    std::string formatted = FormatLogMessage(entry);
    std::fprintf(stdout, "%s\n", formatted.c_str());
    SetConsoleTextAttribute(hConsole, 0x07);
    std::fflush(stdout);
}

// ---- Ring buffer ----

void Logger::AddToRingBuffer(const domain::LogEntry& entry) {
    size_t idx = m_ringIndex.fetch_add(1, std::memory_order_relaxed) % RING_BUFFER_SIZE;
    std::lock_guard lock(m_ringMutex);
    m_ringBuffer[idx] = entry;
}

// ---- Notify listeners ----

void Logger::NotifyListeners(const domain::LogEntry& entry) {
    std::lock_guard lock(m_listenersMutex);
    for (auto& [id, cb] : m_listeners) {
        if (cb) cb(entry);
    }
}

} // namespace infrastructure
} // namespace tcp_redirector