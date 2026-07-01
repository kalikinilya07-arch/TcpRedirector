#pragma once

/**
 * @file IConnectionTable.h
 * @brief Доменный порт для таблицы соединений.
 *
 * Определяет интерфейс таблицы, отображающей локальный порт (src_port)
 * на оригинальный адрес назначения и ID конфигурации прокси.
 * Реализуется в инфраструктурном слое (ConnectionTable).
 */

#include <cstdint>

namespace tcp_redirector {
namespace domain {
namespace ports {

/**
 * @brief Структура с полной информацией о соединении.
 *
 * Содержит идентификатор процесса, путь к исполняемому файлу
 * и счётчики переданных байт в обоих направлениях.
 */
struct ConnectionInfo {
    uint32_t pid;                  //!< Идентификатор процесса-владельца соединения
    wchar_t  proc_path[260];       //!< Полный путь к исполняемому файлу процесса (MAX_PATH)
    uint64_t bytes_up;             //!< Байт, переданных от клиента к целевому серверу
    uint64_t bytes_down;           //!< Байт, переданных от целевого сервера к клиенту
};

/**
 * @brief Интерфейс таблицы соединений.
 *
 * Предоставляет методы для добавления, поиска и удаления записей
 * о перенаправляемых TCP-соединениях. Ключом записи является
 * исходный порт (src_port) на локальной машине.
 */
class IConnectionTable {
public:
    virtual ~IConnectionTable() = default;

    /**
     * @brief Добавить или обновить запись о соединении.
     * @param src_port  Исходный порт на локальной машине.
     * @param src_ip    IP-адрес источника.
     * @param orig_dest_ip  Оригинальный IP-адрес назначения.
     * @param orig_dest_port Оригинальный порт назначения.
     * @param proxy_config_id  Идентификатор конфигурации прокси.
     */
    virtual void Add(uint16_t src_port, uint32_t src_ip,
                     uint32_t orig_dest_ip, uint16_t orig_dest_port,
                     uint32_t proxy_config_id) = 0;

    /**
     * @brief Получить оригинальный адрес назначения по порту.
     * @param src_port  Исходный порт.
     * @param[out] out_orig_dest_ip   Оригинальный IP назначения.
     * @param[out] out_orig_dest_port Оригинальный порт назначения.
     * @return true, если запись найдена.
     */
    virtual bool Get(uint16_t src_port, uint32_t* out_orig_dest_ip,
                     uint16_t* out_orig_dest_port) = 0;

    /**
     * @brief Получить ID конфигурации прокси для порта.
     * @param src_port  Исходный порт.
     * @return Идентификатор конфигурации прокси (0, если не найдена).
     */
    virtual uint32_t GetProxyConfigId(uint16_t src_port) = 0;

    /**
     * @brief Проверить, отслеживается ли соединение.
     * @param src_port  Исходный порт.
     * @return true, если порт отслеживается.
     */
    virtual bool IsTracked(uint16_t src_port) = 0;

    /**
     * @brief Удалить запись о соединении.
     * @param src_port  Исходный порт.
     */
    virtual void Remove(uint16_t src_port) = 0;

    /**
     * @brief Очистить все записи.
     */
    virtual void Clear() = 0;

    /**
     * @brief Установить информацию о процессе для соединения.
     * @param src_port  Исходный порт.
     * @param pid       Идентификатор процесса.
     * @param proc_path Полный путь к исполняемому файлу.
     */
    virtual void SetProcessInfo(uint16_t src_port, uint32_t pid, const wchar_t* proc_path) = 0;

    /**
     * @brief Увеличить счётчики переданных байт.
     * @param src_port  Исходный порт.
     * @param up        Байт, отправленных клиентом.
     * @param down      Байт, полученных клиентом.
     */
    virtual void AddBytes(uint16_t src_port, uint64_t up, uint64_t down) = 0;

    /**
     * @brief Получить полную информацию о соединении.
     * @param src_port  Исходный порт.
     * @param[out] info Указатель на структуру ConnectionInfo для заполнения.
     * @return true, если информация успешно получена.
     */
    virtual bool GetInfo(uint16_t src_port, ConnectionInfo* info) = 0;

    /// Number of currently tracked (active) connections.
    virtual uint32_t GetTrackedCount() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector