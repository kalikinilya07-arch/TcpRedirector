#pragma once

//
// Config — полная структура конфигурации приложения.
// Содержит все секции: app, proxy, auth, log, stats.
// exeName извлекается автоматически из exePath (не хранится в JSON).
//

#include <string>
#include <cstdint>

namespace tcp_redirector {
namespace infrastructure {

struct AppSettings {
    std::wstring exePath;                // полный путь к EXE для перехвата
};

struct ProxySettings {
    std::string host = "127.0.0.1";      // адрес HTTP-прокси
    uint16_t    port = 3128;             // порт HTTP-прокси
    bool        enabled = true;          // включено ли проксирование
};

struct AuthSettings {
    bool        enabled = false;         // требуется ли авторизация
    std::string username;                // логин для Basic Auth
    std::string encryptedPassword;       // пароль, зашифрованный через DPAPI (Base64)
    bool        kerberos = false;        // если true — Negotiate/Kerberos вместо Basic
};

struct LogSettings {
    int  level = 2;                      // 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
    bool fileEnabled = true;             // писать ли лог в файл
    int  maxSizeMB = 10;                 // макс. размер файла до ротации
};

struct StatsSettings {
    int updateIntervalMs = 2000;         // интервал GUI-таймера статистики
};

struct Config {
    AppSettings   app;
    ProxySettings proxy;
    AuthSettings  auth;
    LogSettings   log;
    StatsSettings stats;

    // Вычислить exeName из exePath (последний компонент после \)
    std::wstring GetExeName() const {
        auto pos = app.exePath.find_last_of(L'\\');
        if (pos != std::wstring::npos)
            return app.exePath.substr(pos + 1);
        return app.exePath;
    }
};

} // namespace infrastructure
} // namespace tcp_redirector
