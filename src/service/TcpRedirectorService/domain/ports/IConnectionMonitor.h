#pragma once

/**
 * @file IConnectionMonitor.h
 * @brief Доменные порты мониторинга соединений, логирования и IPC с GUI.
 *
 * Содержит три доменных интерфейса:
 * - IConnectionMonitor — наблюдение за активными соединениями
 * - ILogSink         — приём логов
 * - IGUIIpc          — двусторонняя связь с GUI через именованные каналы
 */

#include <functional>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

/**
 * @brief Интерфейс мониторинга соединений.
 *
 * Предоставляет методы для добавления, обновления, удаления записей
 * соединений, а также поиска, фильтрации и подписки на изменения.
 */
class IConnectionMonitor {
public:
    virtual ~IConnectionMonitor() = default;

    /**
     * @brief Добавить новую запись о соединении.
     * @param record Структура с данными соединения.
     */
    virtual void AddConnection(const ConnectionRecord& record) = 0;

    /**
     * @brief Обновить существующую запись соединения.
     * @param id      Идентификатор соединения.
     * @param updates Поля, которые необходимо обновить.
     */
    virtual void UpdateConnection(uint64_t id, const ConnectionRecord& updates) = 0;

    /**
     * @brief Удалить запись соединения.
     * @param id Идентификатор соединения.
     */
    virtual void RemoveConnection(uint64_t id) = 0;

    /**
     * @brief Получить список активных соединений.
     * @return Вектор записей активных соединений.
     */
    virtual std::vector<ConnectionRecord> GetActiveConnections() const = 0;

    /**
     * @brief Поиск соединений по текстовому запросу.
     * @param query Строка поиска.
     * @return Вектор найденных записей.
     */
    virtual std::vector<ConnectionRecord> Search(const std::wstring& query) const = 0;

    /**
     * @brief Фильтрация соединений по PID процесса.
     * @param pid Идентификатор процесса.
     * @return Вектор записей, принадлежащих процессу.
     */
    virtual std::vector<ConnectionRecord> FilterByProcess(uint32_t pid) const = 0;

    /**
     * @brief Получить агрегированную статистику по всем соединениям.
     * @return Структура ServiceStats.
     */
    virtual ServiceStats GetAggregatedStats() const = 0;

    /// Тип коллбэка уведомления об изменениях в списке соединений.
    using ConnectionsCallback = std::function<void(const std::vector<ConnectionRecord>&)>;

    /**
     * @brief Установить коллбэк для уведомления об изменении соединений.
     * @param callback Функция обратного вызова.
     */
    virtual void SetOnConnectionsChanged(ConnectionsCallback callback) = 0;
};

/**
 * @brief Интерфейс приёмника логов (Log Sink).
 *
 * Реализуется асинхронным логгером (infrastructure::Logger).
 * Поддерживает установку уровня логирования, подписку на события
 * и получение последних записей через кольцевой буфер.
 */
class ILogSink {
public:
    virtual ~ILogSink() = default;

    /// Тип коллбэка для получения каждой записи лога.
    using LogCallback = std::function<void(const LogEntry&)>;

    /**
     * @brief Записать сообщение в лог.
     * @param level   Уровень важности.
     * @param logger  Имя логгера (компонента).
     * @param message Текст сообщения.
     */
    virtual void Log(LogLevel level, const std::string& logger,
                     const std::string& message) = 0;

    /**
     * @brief Установить уровень логирования.
     * @param level Новый уровень.
     */
    virtual void SetLevel(LogLevel level) = 0;

    /**
     * @brief Получить текущий уровень логирования.
     * @return Текущий уровень.
     */
    virtual LogLevel GetLevel() const = 0;

    /**
     * @brief Установить коллбэк для получения записей лога.
     * @param callback Функция обратного вызова.
     */
    virtual void SetOnLogEntry(LogCallback callback) = 0;

    /**
     * @brief Получить последние записи из кольцевого буфера.
     * @param max_count Максимальное количество записей (по умолч. 100).
     * @return Вектор записей лога.
     */
    virtual std::vector<LogEntry> GetRecentEntries(size_t max_count = 100) const = 0;
};

/**
 * @brief Интерфейс IPC-связи с GUI-клиентом.
 *
 * Обеспечивает двустороннюю связь с графическим интерфейсом
 * через именованные каналы Windows. Позволяет отправлять данные
 * соединений, логов, статистики, конфигурации и правил,
 * а также принимать запросы от GUI.
 */
class IGUIIpc {
public:
    virtual ~IGUIIpc() = default;

    /// Тип коллбэка для обработки входящих запросов от GUI.
    using RequestCallback = std::function<void(const std::string& method,
                                                const std::string& params,
                                                std::string& response)>;

    /**
     * @brief Запустить IPC-сервер.
     * @return true, если сервер запущен.
     */
    virtual bool Start() = 0;

    /**
     * @brief Остановить IPC-сервер.
     */
    virtual void Stop() = 0;

    /**
     * @brief Проверить, подключён ли GUI-клиент.
     * @return true, если клиент подключён.
     */
    virtual bool IsConnected() const = 0;

    /**
     * @brief Отправить список соединений GUI.
     * @param connections Вектор записей соединений.
     */
    virtual void SendConnections(const std::vector<ConnectionRecord>& connections) = 0;

    /**
     * @brief Отправить запись лога GUI.
     * @param entry Запись лога.
     */
    virtual void SendLog(const LogEntry& entry) = 0;

    /**
     * @brief Отправить статистику сервиса GUI.
     * @param stats Структура статистики.
     */
    virtual void SendStats(const ServiceStats& stats) = 0;

    /**
     * @brief Отправить конфигурацию прокси GUI.
     * @param config Параметры прокси.
     */
    virtual void SendConfig(const ProxyConfig& config) = 0;

    /**
     * @brief Отправить список правил GUI.
     * @param rules Вектор правил.
     */
    virtual void SendRules(const std::vector<Rule>& rules) = 0;

    /**
     * @brief Установить коллбэк для обработки запросов от GUI.
     * @param callback Функция обратного вызова.
     */
    virtual void SetOnRequest(RequestCallback callback) = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector