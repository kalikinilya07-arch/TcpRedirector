#pragma once

/**
 * @file IConfigStore.h
 * @brief Доменные порты для хранения конфигурации и управления секретами.
 *
 * Определяет интерфейсы:
 * - IConfigStore — чтение/запись конфигурации приложения
 * - ISecretsManager — шифрование/дешифрование паролей (DPAPI)
 */

#include <optional>
#include "../entities/ProxyConfig.h"

namespace tcp_redirector {
namespace domain {
namespace ports {

/**
 * @brief Интерфейс хранилища конфигурации.
 *
 * Реализуется ConfigManager (infrastructure). Предоставляет
 * методы загрузки, сохранения и доступа к секциям конфигурации:
 * настройки прокси, правила фильтрации, параметры логирования.
 */
class IConfigStore {
public:
    virtual ~IConfigStore() = default;

    /**
     * @brief Загрузить конфигурацию из постоянного хранилища.
     * @return true, если загрузка выполнена успешно.
     */
    virtual bool Load() = 0;

    /**
     * @brief Сохранить текущую конфигурацию в постоянное хранилище.
     * @return true, если сохранение выполнено успешно.
     */
    virtual bool Save() = 0;

    /**
     * @brief Получить конфигурацию прокси.
     * @return Структура ProxyConfig.
     */
    virtual ProxyConfig GetProxyConfig() const = 0;

    /**
     * @brief Установить конфигурацию прокси.
     * @param config Новые параметры прокси.
     * @return true, если конфигурация сохранена.
     */
    virtual bool SetProxyConfig(const ProxyConfig& config) = 0;

    /**
     * @brief Получить список правил фильтрации.
     * @return Вектор правил.
     */
    virtual std::vector<Rule> GetRules() const = 0;

    /**
     * @brief Установить список правил фильтрации.
     * @param rules Вектор правил.
     * @return true, если правила сохранены.
     */
    virtual bool SetRules(const std::vector<Rule>& rules) = 0;

    /**
     * @brief Получить уровень логирования.
     * @return Текущий уровень логирования.
     */
    virtual LogLevel GetLogLevel() const = 0;

    /**
     * @brief Установить уровень логирования.
     * @param level Новый уровень логирования.
     * @return true, если уровень сохранён.
     */
    virtual bool SetLogLevel(LogLevel level) = 0;

    /**
     * @brief Получить путь к директории логов.
     * @return Путь в файловой системе.
     */
    virtual std::filesystem::path GetLogDirectory() const = 0;

    /**
     * @brief Получить максимальный размер лог-файла в МБ.
     * @return Размер в мегабайтах.
     */
    virtual uint32_t GetMaxLogFileSizeMB() const = 0;

    /**
     * @brief Получить максимальное количество лог-файлов.
     * @return Количество файлов.
     */
    virtual uint32_t GetMaxLogFiles() const = 0;
};

/**
 * @brief Интерфейс менеджера секретов.
 *
 * Отвечает за шифрование и дешифрование конфиденциальных данных
 * (например, пароля прокси) с использованием DPAPI (CryptProtectData).
 */
class ISecretsManager {
public:
    virtual ~ISecretsManager() = default;

    /**
     * @brief Зашифровать строку пароля через DPAPI.
     * @param plaintext Пароль в открытом виде.
     * @return Зашифрованные данные в виде вектора байт.
     */
    virtual std::vector<uint8_t> Encrypt(const std::wstring& plaintext) = 0;

    /**
     * @brief Расшифровать данные через DPAPI.
     * @param ciphertext Зашифрованные байты.
     * @return Пароль в открытом виде.
     */
    virtual std::wstring Decrypt(const std::vector<uint8_t>& ciphertext) = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector