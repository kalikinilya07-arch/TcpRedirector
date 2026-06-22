#pragma once

/**
 * @file Config.h
 * @brief Полная структура конфигурации приложения.
 *
 * Содержит все секции конфигурации: app (целевой процесс),
 * proxy (адрес прокси), auth (авторизация), log (логирование),
 * stats (статистика). exeName извлекается автоматически из exePath.
 */

#include <string>
#include <cstdint>

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Настройки приложения: целевой процесс для перехвата.
 */
struct AppSettings {
    std::wstring exePath;   //!< Полный путь к EXE-файлу целевого процесса
};

/**
 * @brief Настройки HTTP-прокси-сервера.
 */
struct ProxySettings {
    std::string host = "127.0.0.1";  //!< Адрес HTTP-прокси
    uint16_t    port = 3128;          //!< Порт HTTP-прокси
    bool        enabled = true;       //!< Флаг включения проксирования
};

/**
 * @brief Настройки авторизации на прокси-сервере.
 */
struct AuthSettings {
    bool        enabled = false;       //!< Требуется ли авторизация
    std::string username;              //!< Логин для Basic Auth
    std::string encryptedPassword;     //!< Пароль, зашифрованный через DPAPI (Base64)
    bool        kerberos = false;      //!< Использовать Negotiate/Kerberos вместо Basic
};

/**
 * @brief Настройки системы логирования.
 */
struct LogSettings {
    int  level = 2;               //!< Уровень логирования: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
    bool fileEnabled = true;      //!< Писать ли лог в файл
    int  maxSizeMB = 10;          //!< Максимальный размер файла до ротации (МБ)
};

/**
 * @brief Настройки сбора статистики.
 */
struct StatsSettings {
    int updateIntervalMs = 2000;  //!< Интервал обновления статистики для GUI (мс)
};

/**
 * @brief Полная конфигурация приложения.
 *
 * Объединяет все секции конфигурации в единую структуру.
 * Предоставляет метод GetExeName() для извлечения имени файла из пути.
 */
struct Config {
    AppSettings   app;     //!< Настройки целевого процесса
    ProxySettings proxy;   //!< Настройки прокси-сервера
    AuthSettings  auth;    //!< Настройки авторизации
    LogSettings   log;     //!< Настройки логирования
    StatsSettings stats;   //!< Настройки статистики

    /**
     * @brief Извлечь имя исполняемого файла из полного пути.
     * @return Имя файла (последний компонент после \\)
     */
    std::wstring GetExeName() const {
        auto pos = app.exePath.find_last_of(L'\\');
        if (pos != std::wstring::npos)
            return app.exePath.substr(pos + 1);
        return app.exePath;
    }
};

} // namespace infrastructure
} // namespace tcp_redirector
