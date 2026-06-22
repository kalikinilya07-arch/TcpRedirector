#pragma once

/**
 * @file ProxyConfig.h
 * @brief Доменные сущности конфигурации, правил и статистики.
 *
 * Содержит все структуры данных, используемые в доменном слое:
 * конфигурация прокси, правила фильтрации, записи соединений,
 * события перенаправления, уровни логирования и статистика.
 */

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <functional>
#include <memory>

namespace tcp_redirector {
namespace domain {

/**
 * @brief Конфигурация HTTP-прокси-сервера.
 *
 * Содержит адрес, порт, флаг необходимости авторизации
 * и зашифрованный пароль (хранится в зашифрованном виде,
 * никогда не выводится в лог или дамп конфигурации).
 */
struct ProxyConfig {
    std::wstring host;                      //!< Адрес прокси-сервера
    uint16_t port = 3128;                   //!< Порт прокси-сервера
    bool auth_required = false;             //!< Требуется ли авторизация
    std::wstring login;                     //!< Логин для Basic-авторизации
    std::vector<uint8_t> encrypted_password; //!< Пароль, зашифрованный через DPAPI
    bool has_password = false;              //!< Флаг наличия пароля
};

/**
 * @brief Тип правила фильтрации трафика.
 *
 * Определяет, по какому признаку применяется правило:
 * - ProcessName — по имени процесса
 * - ProcessPath — по полному пути к процессу
 * - Global — глобальное правило (для всех процессов)
 */
enum class RuleType {
    ProcessName,  //!< По имени процесса
    ProcessPath,  //!< По полному пути к процессу
    Global        //!< Глобально для всех процессов
};

/**
 * @brief Действие, выполняемое при срабатывании правила.
 *
 * - Proxy — перенаправить трафик через прокси
 * - Direct — пропустить трафик без изменений
 * - Block — заблокировать трафик
 */
enum class RuleAction {
    Proxy,   //!< Перенаправить на прокси
    Direct,  //!< Пропустить напрямую
    Block    //!< Заблокировать
};

/**
 * @brief Правило фильтрации трафика.
 *
 * Определяет, какой трафик перенаправлять на прокси,
 * пропускать напрямую или блокировать.
 */
struct Rule {
    std::string id;                    //!< Уникальный идентификатор правила
    RuleType type = RuleType::ProcessName;  //!< Тип правила
    RuleAction action = RuleAction::Proxy;  //!< Действие при срабатывании
    std::wstring pattern;              //!< Шаблон для сопоставления (поддерживает *)
    std::wstring description;          //!< Описание правила
    int priority = 0;                  //!< Приоритет (меньше = выше приоритет)
    bool enabled = true;               //!< Флаг активности правила
    std::chrono::system_clock::time_point created;   //!< Дата создания
    std::chrono::system_clock::time_point modified;  //!< Дата последнего изменения
};

/**
 * @brief Состояние TCP-соединения в жизненном цикле перенаправления.
 */
enum class ConnectionState {
    Redirecting,          //!< Ожидание перенаправления на relay
    ConnectingToProxy,    //!< Установка соединения с прокси
    TunnelEstablished,    //!< Туннель через прокси установлен
    Closing,              //!< Соединение закрывается
    Closed,               //!< Соединение закрыто
    Error                 //!< Ошибка соединения
};

/**
 * @brief Запись активного или завершённого соединения.
 *
 * Содержит полную информацию: PID процесса, путь, адрес назначения,
 * длительность, объём переданных данных и текущее состояние.
 */
struct ConnectionRecord {
    uint64_t id = 0;                        //!< Уникальный ID соединения
    uint32_t pid = 0;                       //!< PID процесса-владельца
    std::wstring process_name;              //!< Имя процесса
    std::wstring process_path;              //!< Полный путь к процессу
    std::wstring destination_host;          //!< Хост назначения
    std::string destination_ip;             //!< IP-адрес назначения
    uint16_t destination_port = 0;          //!< Порт назначения
    std::chrono::steady_clock::time_point start_time; //!< Время начала
    std::chrono::milliseconds duration{0};  //!< Длительность сессии
    uint64_t rx_bytes = 0;                  //!< Получено байт
    uint64_t tx_bytes = 0;                  //!< Отправлено байт
    bool proxy_enabled = true;              //!< Флаг использования прокси
    ConnectionState state = ConnectionState::Redirecting; //!< Текущее состояние
};

/**
 * @brief Событие перенаправления TCP-соединения.
 *
 * Генерируется компонентом захвата при обнаружении нового
 * TCP-соединения, подлежащего перенаправлению через прокси.
 */
struct RedirectEvent {
    uint64_t redirect_id = 0;           //!< Идентификатор события
    uint32_t pid = 0;                   //!< PID процесса-инициатора
    std::wstring process_path;          //!< Путь к процессу
    uint32_t original_address_v4 = 0;   //!< Оригинальный IPv4 назначения
    uint16_t original_port = 0;         //!< Оригинальный порт назначения
    uint16_t redirect_local_port = 0;   //!< Локальный порт relay (для перенаправления)
    bool is_ipv6 = false;               //!< Флаг IPv6-соединения
};

/**
 * @brief Уровень важности сообщения лога.
 */
enum class LogLevel {
    Trace = 0,   //!< Трассировка (максимальная детализация)
    Debug = 1,   //!< Отладочные сообщения
    Info = 2,    //!< Информационные сообщения
    Warn = 3,    //!< Предупреждения
    Error = 4,   //!< Критические ошибки
    Off = 5      //!< Логирование отключено
};

/**
 * @brief Запись лога.
 *
 * Содержит временную метку, уровень, имя логгера, сообщение,
 * а также опциональную информацию о файле и строке исходного кода.
 */
struct LogEntry {
    std::chrono::system_clock::time_point timestamp; //!< Временная метка
    LogLevel level = LogLevel::Info;    //!< Уровень важности
    std::string logger;                 //!< Имя компонента-логгера
    std::string message;                //!< Текст сообщения
    std::string file;                   //!< Имя файла (__FILE__, опционально)
    int line = 0;                       //!< Номер строки (__LINE__, опционально)
    std::string function;               //!< Имя функции (__FUNCTION__, опционально)
};

/**
 * @brief Агрегированная статистика работы сервиса.
 *
 * Содержит счётчики соединений, объём трафика и ошибки работы с прокси.
 */
struct ServiceStats {
    uint64_t total_connections = 0;     //!< Всего соединений
    uint64_t active_connections = 0;    //!< Активных соединений
    uint64_t total_rx_bytes = 0;        //!< Всего получено байт
    uint64_t total_tx_bytes = 0;        //!< Всего отправлено байт
    uint64_t proxy_errors = 0;          //!< Ошибок прокси-соединений
    double avg_latency_ms = 0.0;        //!< Средняя задержка в мс
};

/**
 * @brief Статистика драйвера захвата (WinDivert).
 *
 * Содержит счётчики перенаправлений, размер очереди и количество правил.
 */
struct DriverStats {
    uint32_t pending_redirects = 0;     //!< Ожидающих обработки перенаправлений
    int64_t total_redirects = 0;        //!< Всего выполнено перенаправлений
    int64_t failed_redirects = 0;       //!< Ошибок перенаправления
    uint32_t rules_count = 0;           //!< Количество загруженных правил
    uint32_t queue_max_size = 4096;     //!< Максимальный размер очереди событий
};

} // namespace domain
} // namespace tcp_redirector