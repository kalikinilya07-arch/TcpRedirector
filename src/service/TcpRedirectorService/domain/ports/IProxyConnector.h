#pragma once

/**
 * @file IProxyConnector.h
 * @brief Доменные порты для сессий прокси-соединений.
 *
 * Определяет интерфейсы IProxySession (одна прокси-сессия)
 * и IProxyConnector (фабрика сессий с управлением жизненным циклом).
 * Реализуются в инфраструктурном слое (ProxySession / ProxyEngine).
 */

#include <functional>
#include <memory>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

/**
 * @brief Статистика одной прокси-сессии.
 *
 * Содержит счётчики переданных байт, длительность сессии,
 * флаг подключения и сообщение об ошибке (при наличии).
 */
struct ProxySessionStats {
    uint64_t rx_bytes = 0;             //!< Байт, полученных от прокси
    uint64_t tx_bytes = 0;             //!< Байт, отправленных прокси
    std::chrono::milliseconds duration{0}; //!< Длительность сессии
    bool connected = false;            //!< Флаг установленного соединения
    std::string error_message;          //!< Сообщение об ошибке (если есть)
};

/**
 * @brief Интерфейс одной прокси-сессии.
 *
 * Представляет одно прокси-соединение: CONNECT к целевому серверу,
 * двунаправленный бридж данных, сбор статистики.
 */
class IProxySession {
public:
    virtual ~IProxySession() = default;

    /**
     * @brief Запустить сессию для заданного события перенаправления.
     * @param redirect Событие перенаправления с информацией о соединении.
     * @return true, если сессия успешно запущена.
     */
    virtual bool Start(const RedirectEvent& redirect) = 0;

    /**
     * @brief Закрыть сессию и освободить ресурсы.
     */
    virtual void Close() = 0;

    /**
     * @brief Получить статистику сессии.
     * @return Структура ProxySessionStats.
     */
    virtual ProxySessionStats GetStats() const = 0;

    /**
     * @brief Проверить, активна ли сессия.
     * @return true, если туннель установлен.
     */
    virtual bool IsActive() const = 0;

    /**
     * @brief Получить уникальный идентификатор сессии.
     * @return ID сессии.
     */
    virtual uint64_t GetSessionId() const = 0;
};

/**
 * @brief Интерфейс фабрики и менеджера прокси-сессий.
 *
 * Отвечает за инициализацию, создание сессий,
 * агрегированную статистику и graceful shutdown.
 */
class IProxyConnector {
public:
    virtual ~IProxyConnector() = default;

    /**
     * @brief Инициализировать коннектор с конфигурацией прокси.
     * @param config Параметры прокси-сервера.
     * @return true, если инициализация прошла успешно.
     */
    virtual bool Initialize(const ProxyConfig& config) = 0;

    /**
     * @brief Остановить все сессии и освободить ресурсы.
     */
    virtual void Shutdown() = 0;

    /**
     * @brief Создать новую прокси-сессию для события перенаправления.
     * @param redirect Событие перенаправления.
     * @param callback Коллбэк, вызываемый при завершении (success/error).
     * @return Умный указатель на созданную сессию.
     */
    virtual std::shared_ptr<IProxySession> CreateSession(
        const RedirectEvent& redirect,
        std::function<void(bool success, const std::string& error)> callback) = 0;

    /**
     * @brief Получить агрегированную статистику по всем сессиям.
     * @return Структура ServiceStats.
     */
    virtual ServiceStats GetAggregatedStats() const = 0;

    /**
     * @brief Проверить, инициализирован ли коннектор.
     * @return true, если инициализация выполнена.
     */
    virtual bool IsInitialized() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector