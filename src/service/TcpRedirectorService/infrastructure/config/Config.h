#pragma once

/**
 * @file Config.h
 * @brief Полная структура конфигурации приложения (schema v2).
 *
 * Содержит все секции конфигурации:
 *  - app       — legacy: целевой процесс (сохраняется для обратной совместимости);
 *  - proxy     — параметры HTTP-прокси;
 *  - auth      — авторизация на прокси;
 *  - log       — логирование;
 *  - stats     — статистика;
 *  - capture_mode — WP3: движок захвата ("windivert" | "wintun");
 *  - wintun    — WP3: настройки Wintun-адаптера и tun2socks-движка;
 *  - apps      — WP3: пер-приложение правила с многопортовыми списками
 *                и флагом route_all_traffic.
 *
 * NB: Поле config_version = 2 отличает v2 от исторического v1
 *     (в v1 маркер отсутствовал).
 */

#include <string>
#include <vector>
#include <cstdint>

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Настройки приложения: целевой процесс для перехвата (legacy v1).
 *
 * В v2 замещается apps[]; поле остаётся ради обратной совместимости
 * и «мирроринга» в legacy rules[] при сохранении.
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

    //! (Задача №2) Шифровать ли пароль прокси через DPAPI.
    //! true (по умолчанию) — прежнее поведение: пароль хранится в
    //! encryptedPassword (DPAPI+Base64).
    //! false — пароль хранится/используется «как есть» в открытом поле
    //! password (в обход DPAPI). Диагностический/совместимостный режим:
    //! снимает класс проблем «DPAPI-blob зашифрован не тем аккаунтом/скоупом
    //! → не расшифровался → Basic-auth молча падает». ВНИМАНИЕ: пароль лежит
    //! в config.json открытым текстом — только для доверенного окружения.
    bool        encryptPassword = true;
    //! Пароль в открытом виде. Используется ТОЛЬКО при encryptPassword=false.
    std::string password;
};

/**
 * @brief Настройки системы логирования.
 */
struct LogSettings {
    int  level = 2;               //!< Уровень: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=TRACE
    bool fileEnabled = true;      //!< Писать ли лог в файл
    int  maxSizeMB = 50;          //!< Максимальный размер файла до ротации (МБ)
};

/**
 * @brief Настройки локального IPC-канала управления (GUI ↔ служба).
 */
struct IpcSettings {
    //! (Задача №1) Требовать ли токен-аутентификацию на IPC-канале.
    //! true (по умолчанию) — прежнее поведение: служба генерирует .ipc_token
    //! (DACL Administrators/SYSTEM) и отклоняет запросы без корректного токена.
    //! false — аутентификация НЕ навязывается: любой локальный клиент может
    //! управлять службой через loopback без токена/привилегий.
    //! ВНИМАНИЕ: повышает локальную поверхность атаки — только для
    //! диагностики/доверенного окружения.
    bool auth_enabled = true;
};

/**
 * @brief Настройки сбора статистики.
 */
struct StatsSettings {
    int updateIntervalMs = 2000;  //!< Интервал обновления статистики для GUI (мс)
};

// ============================================================================
// WP3: Schema v2 additions
// ============================================================================

/**
 * @brief Активный движок захвата трафика.
 *
 * Строковые формы в JSON — строго нижним регистром: "windivert" | "wintun".
 */
enum class CaptureMode {
    WinDivert,  //!< WinDivert-based capture (по умолчанию, legacy path).
    Wintun      //!< Wintun-based capture (tun2socks-подобный движок).
};

/**
 * @brief Тип tun2socks-движка при capture_mode = wintun.
 *
 * Строковые формы: "embedded" (встроенный движок на lwIP)
 * либо "external" (внешний tun2socks.exe, управляемый супервизором).
 */
enum class WintunEngineKind {
    Embedded,  //!< Встроенный tun2socks на lwIP (default).
    External   //!< Внешний бинарник tun2socks.exe.
};

// --- Helpers: enum ⇄ string ---------------------------------------------------
// Определены как inline-функции, чтобы Config.h оставался header-only и
// не тянул дополнительный .cpp.  Строки — ровно то, что читается/пишется в JSON.

inline const char* CaptureModeToString(CaptureMode m) {
    switch (m) {
        case CaptureMode::Wintun:    return "wintun";
        case CaptureMode::WinDivert: // fallthrough
        default:                     return "windivert";
    }
}

/**
 * @brief Разбор строки в CaptureMode.
 * @param s Строковое значение из JSON (регистр-независимо).
 * @param out Результат (устанавливается только при валидном значении).
 * @return true, если строка распознана; false — иначе (out не тронут).
 */
inline bool CaptureModeFromString(const std::string& s, CaptureMode& out) {
    // Нормализация по нижнему регистру.
    std::string lc; lc.reserve(s.size());
    for (char c : s) lc.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c));
    if (lc == "windivert") { out = CaptureMode::WinDivert; return true; }
    if (lc == "wintun")    { out = CaptureMode::Wintun;    return true; }
    return false;
}

inline const char* WintunEngineKindToString(WintunEngineKind e) {
    switch (e) {
        case WintunEngineKind::External: return "external";
        case WintunEngineKind::Embedded: // fallthrough
        default:                         return "embedded";
    }
}

/**
 * @brief Разбор строки в WintunEngineKind.
 * @param s Строковое значение из JSON (регистр-независимо).
 * @param out Результат (устанавливается только при валидном значении).
 * @return true, если строка распознана; false — иначе (out не тронут).
 */
inline bool WintunEngineKindFromString(const std::string& s, WintunEngineKind& out) {
    std::string lc; lc.reserve(s.size());
    for (char c : s) lc.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c));
    if (lc == "embedded") { out = WintunEngineKind::Embedded; return true; }
    if (lc == "external") { out = WintunEngineKind::External; return true; }
    return false;
}

/**
 * @brief Диапазон портов [from; to] (включительный).
 *
 * Валидируется на этапе загрузки конфига: 1 ≤ from ≤ to ≤ 65535.
 */
struct PortRange {
    uint16_t from{0};
    uint16_t to{0};
};

/**
 * @brief Настройки внешнего tun2socks-движка (wintun.engine = "external").
 */
struct ExternalEngineSettings {
    //!< Путь к исполняемому файлу; относительный — от директории EXE.
    std::string executable = ".bin/tun2socks/tun2socks.exe";
    //!< Дополнительные аргументы CLI (передаются как есть).
    std::vector<std::string> extra_args;
    //!< Локальный SOCKS5-адрес, куда tun2socks будет форвардить
    //!< и куда биндится SOCKS5-адаптер relay.  Формат "host:port".
    std::string socks5_listen = "127.0.0.1:1080";
    //!< Автоматически перезапускать процесс при падении.
    bool        restart_on_crash = true;
    //!< Задержка перед авторестартом (мс).
    int         restart_backoff_ms = 2000;
};

/**
 * @brief Настройки Wintun-адаптера и tun2socks-движка.
 *
 * Активируются, только если capture_mode = "wintun".  При "windivert"
 * структура заполнена дефолтами, но никем не используется.
 */
struct WintunSettings {
    //!< Имя Wintun-адаптера (Display Name).
    std::string adapter_name = "TcpRedirector";
    //!< GUID адаптера (пусто = сгенерировать на первом старте и persist-нуть).
    std::string adapter_guid;
    //!< IPv4-CIDR на TUN-стороне; шлюз = .1.
    std::string tunnel_ipv4_cidr = "10.6.7.1/24";
    //!< IPv6-CIDR; пусто = IPv6 в туннеле выключен.
    std::string tunnel_ipv6_cidr;
    //!< MTU интерфейса (clamped в [576, 65535]).
    int mtu = 1500;
    //!< Тип tun2socks-движка.
    WintunEngineKind engine = WintunEngineKind::Embedded;
    //!< Настройки внешнего движка (используются только при engine=External).
    ExternalEngineSettings external_engine{};

    //!< Задача 2: включить фильтрацию по процессу внутри Wintun-движка.
    //!< При true (по умолчанию) embedded-движок применяет те же apps[]-правила,
    //!< что и WinDivert: резолвит процесс-источник по source-порту и решает
    //!< PROXY / DIRECT / BLOCK.  При false — сохраняется прежнее поведение
    //!< Option 2b (весь IPv4-TCP гонится через прокси без фильтрации).
    //!< В external-движке поле не влияет (см. ограничение external-режима).
    bool process_filter_enabled = true;

    //!< Глубина «лестницы» split-tunnel маршрутов (длина префикса листьев).
    //!< Вместо одиночной пары 0.0.0.0/1 + 128.0.0.0/1 движок раскладывает
    //!< 0.0.0.0/0 набором из 2^D маршрутов /D (за вычетом carve-out диапазонов
    //!< 127/8, 0/8, 169.254/16 и /32 к прокси).  Чем больше D, тем более
    //!< специфичны маршруты и тем увереннее они выигрывают longest-prefix-match
    //!< у сосуществующих full-tunnel VPN, раскладывающих свою лестницу.
    //!< Допустимо 1..8.  D=1 = историческое поведение (пара /1).  D=5 бьёт
    //!< VPN, использующие префиксы вплоть до /4.  Цена — 2^D маршрутов.
    //!< ВАЖНО: две full-tunnel-капчи на одной машине взаимоисключающи по
    //!< маршрутизации — гарантия победы над VPN РАВНОЙ/большей глубины
    //!< отсутствует (это физика, а не дефект).  Полная независимость — только WFP.
    int route_ladder_prefix = 5;

    //!< Блокировать IPv6 на время работы Wintun-захвата (embedded, IPv4-only).
    //!< Туннель/relay/CONNECT работают только по IPv4, а на dual-stack хостах
    //!< ОС (RFC 6724) предпочитает глобальный IPv6 и трафик уходит мимо TUN.
    //!< При true (по умолчанию) сервис заворачивает весь IPv6 в TUN (маршруты
    //!< ::/1 + 8000::/1) и embedded-движок отвечает TCP RST на IPv6-SYN, чтобы
    //!< приложение мгновенно откатилось на IPv4 (который идёт в туннель и
    //!< проксируется).  Не-TCP IPv6 дропается.  Всё снимается на Close.  При
    //!< false IPv6 не трогается (возможна утечка мимо прокси).
    bool block_ipv6 = true;
};

/**
 * @brief Пер-приложение правило (schema v2, apps[]).
 *
 * Матчинг (реализация — WP4 в RuleEngine): процесс подходит по exe_path/pattern,
 * если совпадает pattern.  Затем проверяется порт назначения:
 *   port_match := route_all_traffic
 *              || any p in ports where p == dst_port
 *              || any r in port_ranges where r.from <= dst_port <= r.to
 * Правило срабатывает при (process_match && port_match).
 */
struct AppRule {
    std::string exe_path;   //!< Опциональный полный путь к EXE (для UX/логов).
    std::string pattern;    //!< Обязательный шаблон имени/пути процесса.
    std::string proxy_id;   //!< Идентификатор прокси (сейчас "default").
    bool        route_all_traffic = false; //!< true = игнорировать ports/port_ranges.
    std::vector<uint16_t>  ports;          //!< Дискретные порты.
    std::vector<PortRange> port_ranges;    //!< Диапазоны портов [from; to].
};

/**
 * @brief Полная конфигурация приложения (schema v2).
 *
 * Объединяет все секции конфигурации в единую структуру.
 * Предоставляет метод GetExeName() для извлечения имени файла из legacy-пути.
 */
struct Config {
    // Metadata / версии
    int config_version = 2;                     //!< Маркер схемы; 2 для v2.

    // Legacy v1 sections (сохраняются)
    AppSettings   app;     //!< Настройки целевого процесса (legacy)
    ProxySettings proxy;   //!< Настройки прокси-сервера
    AuthSettings  auth;    //!< Настройки авторизации
    LogSettings   log;     //!< Настройки логирования
    StatsSettings stats;   //!< Настройки статистики

    // WP3 v2 additions
    CaptureMode    capture_mode = CaptureMode::Wintun; //!< Активный движок захвата (дефолт: Wintun embedded).
    WintunSettings wintun;                                 //!< Настройки Wintun-адаптера.
    std::vector<AppRule> apps;                             //!< Пер-приложение правила v2.

    IpcSettings    ipc;    //!< (Задача №1) Настройки IPC-канала управления.

    /**
     * @brief Извлечь имя исполняемого файла из полного пути (legacy).
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
