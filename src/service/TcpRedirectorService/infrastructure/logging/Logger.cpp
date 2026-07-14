#include "Logger.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>

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
        m_maxFileSize.store(max_file_size_mb * 1024ULL * 1024ULL,
                            std::memory_order_relaxed);
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

void Logger::SetMaxFileSizeMB(size_t max_file_size_mb) {
    m_maxFileSize.store(max_file_size_mb * 1024ULL * 1024ULL,
                        std::memory_order_relaxed);
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
    result.reserve(max_count < total ? max_count : total);

    // Ring buffer wrap-around: data may not be contiguous.
    // The physical layout is:
    //   [ idx % N  ...  N-1 ]  (older if idx >= N)
    //   [    0     ...  idx % N - 1 ] (newer)
    // We want the *last* `max_count` entries in chronological order.

    if (idx < RING_BUFFER_SIZE) {
        // Linear case: 0 .. idx-1
        size_t start = (total > max_count) ? total - max_count : 0;
        for (size_t i = start; i < total; i++) {
            result.push_back(m_ringBuffer[i]);
        }
    } else {
        // Circular case: physical layout is [wrap..N-1] then [0..wrap-1]
        size_t wrap = idx % RING_BUFFER_SIZE;
        size_t available = total;

        if (available <= max_count) {
            // Return everything
            for (size_t i = wrap; i < RING_BUFFER_SIZE; i++) {
                result.push_back(m_ringBuffer[i]);
            }
            for (size_t i = 0; i < wrap; i++) {
                result.push_back(m_ringBuffer[i]);
            }
        } else {
            // Return only the last `max_count` entries
            size_t skip = available - max_count;
            size_t i = (wrap + skip) % RING_BUFFER_SIZE;
            for (size_t count = 0; count < max_count; count++) {
                result.push_back(m_ringBuffer[i]);
                i = (i + 1) % RING_BUFFER_SIZE;
            }
        }
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

        bool rotatedThisBatch = false;
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
                    if (pos > 0 && static_cast<size_t>(pos) >=
                            m_maxFileSize.load(std::memory_order_relaxed)) {
                        RotateLogFile();
                        rotatedThisBatch = true;
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

        // Retention-очистка архивов (Задача 3) — вне горячего пути:
        // выполняется после ротации в текущем батче и/или периодически
        // (раз в CLEANUP_EVERY_N_ENTRIES записей).
        m_entriesSinceCleanup += batch.size();
        if (rotatedThisBatch ||
                m_entriesSinceCleanup >= CLEANUP_EVERY_N_ENTRIES) {
            CleanupArchives();
            m_entriesSinceCleanup = 0;
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
    // Задача 3: активный лог ротируется в архив с временной меткой
    // tcp_redirector.YYYYMMDD_HHMMSS.log. Метка позволяет определять
    // возраст архива по имени, а retention-очистка (CleanupArchives)
    // удаляет старые/избыточные архивы. Схема с numbered-backup
    // (tcp_redirector.1.log …) больше не используется.
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }

    std::error_code ec;

    // Формируем имя архива по локальному времени. При коллизии имени
    // (несколько ротаций в одну секунду) добавляем миллисекунды.
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32] = {0};
    swprintf(stamp, 32, L"%04u%02u%02u_%02u%02u%02u",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    std::wstring archiveName = std::wstring(ARCHIVE_PREFIX) + stamp + ARCHIVE_SUFFIX;
    std::filesystem::path archivePath = m_logDir / archiveName;
    if (std::filesystem::exists(archivePath, ec)) {
        wchar_t stampMs[40] = {0};
        swprintf(stampMs, 40, L"%04u%02u%02u_%02u%02u%02u_%03u",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        archiveName = std::wstring(ARCHIVE_PREFIX) + stampMs + ARCHIVE_SUFFIX;
        archivePath = m_logDir / archiveName;
    }

    if (std::filesystem::exists(m_logPath, ec)) {
        std::filesystem::rename(m_logPath, archivePath, ec);
    }

    OpenLogFile();

    // Немедленная очистка архивов после ротации (WriterThread также
    // вызовет CleanupArchives по флагу rotatedThisBatch, но выполнить
    // здесь безопасно и идемпотентно).
    CleanupArchives();
}

// ---- Retention: очистка архивов ----

void Logger::CleanupArchives() {
    // Собираем список архивных файлов (все, кроме активного лога) вида
    // tcp_redirector.*.log в m_logDir. Для каждого запоминаем путь,
    // размер и время последней модификации (FILETIME, 64-бит).
    struct ArchiveInfo {
        std::wstring path;
        uint64_t     lastWrite100ns; // FILETIME как uint64 (100-нс интервалы)
        uint64_t     size;
    };

    std::vector<ArchiveInfo> archives;

    std::wstring pattern = m_logDir.wstring();
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern += L'\\';
    }
    pattern += std::wstring(ARCHIVE_PREFIX) + L"*" + ARCHIVE_SUFFIX;

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return; // нет архивов — ничего чистить
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        // Исключаем активный лог tcp_redirector.log из retention.
        if (_wcsicmp(fd.cFileName, LOG_BASENAME) == 0) continue;

        ULARGE_INTEGER li;
        li.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
        li.HighPart = fd.ftLastWriteTime.dwHighDateTime;

        ULARGE_INTEGER sz;
        sz.LowPart  = fd.nFileSizeLow;
        sz.HighPart = fd.nFileSizeHigh;

        ArchiveInfo info;
        info.path = m_logDir.wstring();
        if (!info.path.empty() && info.path.back() != L'\\' &&
                info.path.back() != L'/') {
            info.path += L'\\';
        }
        info.path += fd.cFileName;
        info.lastWrite100ns = li.QuadPart;
        info.size = sz.QuadPart;
        archives.push_back(std::move(info));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (archives.empty()) return;

    // Текущее системное время в том же формате (UTC, 100-нс интервалы).
    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER nowLi;
    nowLi.LowPart  = ftNow.dwLowDateTime;
    nowLi.HighPart = ftNow.dwHighDateTime;
    uint64_t now100ns = nowLi.QuadPart;

    std::error_code ec;

    // 1) Удаляем архивы старше 24 часов (по времени последней модификации).
    for (auto it = archives.begin(); it != archives.end();) {
        uint64_t age = (now100ns > it->lastWrite100ns)
            ? (now100ns - it->lastWrite100ns) : 0;
        if (age > RETENTION_MAX_AGE_100NS) {
            std::filesystem::remove(std::filesystem::path(it->path), ec);
            it = archives.erase(it);
        } else {
            ++it;
        }
    }

    // 2) Если суммарный объём оставшихся архивов > 500 МБ — удаляем
    //    самые старые, пока объём не станет ≤ лимита.
    uint64_t total = 0;
    for (const auto& a : archives) total += a.size;

    if (total > RETENTION_MAX_TOTAL_BYTES) {
        // Сортируем по возрастанию времени модификации (старейшие первыми).
        std::sort(archives.begin(), archives.end(),
                  [](const ArchiveInfo& a, const ArchiveInfo& b) {
                      return a.lastWrite100ns < b.lastWrite100ns;
                  });
        for (auto& a : archives) {
            if (total <= RETENTION_MAX_TOTAL_BYTES) break;
            std::filesystem::remove(std::filesystem::path(a.path), ec);
            total = (total > a.size) ? (total - a.size) : 0;
        }
    }
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