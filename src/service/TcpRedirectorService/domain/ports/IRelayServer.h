#pragma once

/**
 * @file IRelayServer.h
 * @brief Доменный порт для relay-сервера.
 *
 * Определяет интерфейс локального TCP-сервера, который принимает
 * перенаправленные WinDivert соединения и проксирует их через
 * HTTP CONNECT к настроенному прокси-серверу.
 * Реализуется в инфраструктурном слое (TcpRelayServer).
 */

#include <string>
#include <functional>
#include <cstdint>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

/**
 * @brief Тип коллбэка для логирования событий relay-сервера.
 * @param msg Строка сообщения для логирования.
 */
using RelayLogCallback = std::function<void(const std::string&)>;

/**
 * @brief Интерфейс relay-сервера.
 *
 * Relay-сервер слушает локальный порт, принимает перенаправленные
 * TCP-соединения, выполняет HTTP CONNECT к прокси-серверу
 * и организует двунаправленную передачу данных.
 */
class IRelayServer {
public:
    virtual ~IRelayServer() = default;

    /**
     * @brief Установить конфигурацию прокси-сервера.
     * @param config   Параметры прокси (хост, порт, авторизация).
     * @param config_id Идентификатор конфигурации (по умолчанию 1).
     */
    virtual void SetProxyConfig(const ProxyConfig& config, uint32_t config_id = 1) = 0;

    /**
     * @brief Установить коллбэк для логирования.
     * @param cb Функция обратного вызова для сообщений лога.
     */
    virtual void SetLogCallback(RelayLogCallback cb) = 0;

    /**
     * @brief Запустить relay-сервер.
     * @return true, если сервер успешно запущен.
     */
    virtual bool Start() = 0;

    /**
     * @brief Остановить relay-сервер.
     */
    virtual void Stop() = 0;

    /**
     * @brief Проверить, запущен ли сервер.
     * @return true, если сервер работает.
     */
    virtual bool IsRunning() const = 0;

    /**
     * @brief Получить номер порта, на котором слушает сервер.
     * @return Номер порта.
     */
    virtual uint16_t GetPort() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector