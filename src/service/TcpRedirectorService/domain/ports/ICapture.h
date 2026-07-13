#pragma once

/**
 * @file ICapture.h
 * @brief Доменный порт для захвата и перенаправления сетевого трафика.
 *
 * Определяет интерфейс компонента, отвечающего за перехват TCP-пакетов
 * через WinDivert, их анализ и перенаправление на локальный relay-сервер
 * путём модификации полей заголовков (DST modification).
 * Реализуется в инфраструктурном слое (WinDivertCapture).
 */

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "../entities/ProxyConfig.h"
#include "IConnectionTable.h"

namespace tcp_redirector {
namespace domain {

// Forward declaration to avoid pulling RuleEngine.h (which pulls Config.h) here.
namespace services { class RuleEngine; }

namespace ports {

/**
 * @brief Интерфейс захвата и перенаправления трафика.
 *
 * Предоставляет методы управления жизненным циклом захвата,
 * настройки целевого процесса, конфигурации relay и прокси,
 * а также получения событий перенаправления и статистики.
 */
class ICapture {
public:
    virtual ~ICapture() = default;

    // --- Жизненный цикл ---

    /**
     * @brief Открыть устройство захвата и запустить обработку.
     * @return true, если устройство успешно открыто.
     */
    virtual bool Open() = 0;

    /**
     * @brief Закрыть устройство захвата и остановить обработку.
     */
    virtual void Close() = 0;

    /**
     * @brief Проверить, открыто ли устройство захвата.
     * @return true, если устройство активно.
     */
    virtual bool IsOpen() const = 0;

    // --- Конфигурация захвата ---

    /**
     * @brief Установить путь к исполняемому файлу целевого процесса.
     * @param exePath Полный путь к EXE-файлу.
     */
    virtual void SetTargetProcess(const std::wstring& exePath) = 0;

    /**
     * @brief Установить порт локального relay-сервера.
     * @param port Номер порта.
     */
    virtual void SetRelayPort(uint16_t port) = 0;

    /**
     * @brief Установить адрес внешнего прокси-сервера.
     * @param host Хост прокси.
     * @param port Порт прокси.
     */
    virtual void SetProxyConfig(const std::string& host, uint16_t port) = 0;

    /**
     * @brief Установить таблицу соединений для хранения перенаправлений.
     * @param table Указатель на реализацию IConnectionTable.
     */
    virtual void SetConnectionTable(IConnectionTable* table) = 0;

    // --- Управление редиректами ---

    /**
     * @brief Получить накопившиеся события перенаправления.
     * @param timeout_ms Максимальное время ожидания в мс (по умолч. 1000).
     * @return Вектор событий перенаправления.
     */
    virtual std::vector<RedirectEvent> GetPendingRedirects(
        uint32_t timeout_ms = 1000) = 0;

    /**
     * @brief Подтвердить обработку события перенаправления.
     * @param redirect_id Идентификатор события.
     * @return true, если подтверждение успешно.
     */
    virtual bool AckRedirect(uint64_t redirect_id) = 0;

    // --- Статистика ---

    /**
     * @brief Получить статистику драйвера захвата.
     * @return Структура DriverStats с текущими счётчиками.
     */
    virtual DriverStats GetStats() = 0;

    // --- Системные ---

    /**
     * @brief Получить дескриптор события для ожидания.
     * @return Указатель на объект события (HANDLE).
     */
    virtual void* GetEventHandle() const = 0;

    // --- WP6: полиморфные accessor-ы (заменяют static_cast<WinDivertCapture*>) ---

    /**
     * @brief Установить движок правил (RuleEngine) для матчинга процессов/портов.
     *
     * По умолчанию — no-op (не все реализации нуждаются в RuleEngine
     * или могут прикреплять его иным путём). Конкретные реализации
     * могут переопределить и вернуть true при успешном присоединении.
     * @param engine Сырой указатель на RuleEngine (может быть nullptr).
     * @return true, если движок принят к использованию.
     */
    virtual bool SetRuleEngine(domain::services::RuleEngine* /*engine*/) { return false; }

    /**
     * @brief Суммарные принятые (RX) байты на уровне capture-engine.
     *
     * Дефолтная реализация возвращает 0 — позволяет консюмерам
     * (IPC/StatsCollector) работать через базовый интерфейс без
     * static_cast к конкретной реализации.
     */
    virtual uint64_t GetTotalRxBytes() const { return 0; }

    /**
     * @brief Суммарные отправленные (TX) байты на уровне capture-engine.
     */
    virtual uint64_t GetTotalTxBytes() const { return 0; }

    /**
     * @brief Текущее число активных соединений (по данным connection table).
     */
    virtual uint32_t GetActiveConnections() const { return 0; }
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector