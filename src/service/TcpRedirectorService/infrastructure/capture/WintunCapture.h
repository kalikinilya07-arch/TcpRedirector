#pragma once

/**
 * @file WintunCapture.h
 * @brief WP10 — реальный ICapture-фасад для Wintun-based режима захвата.
 *
 * WP10 ЗАМЕНИЛ WP6-ЗАГЛУШКУ:
 *   Класс теперь ДЕЙСТВИТЕЛЬНО поднимает Wintun-стек:
 *     1) Загружает wintun.dll (WintunApi).
 *     2) Создаёт/открывает адаптер (WintunAdapter) с опциональным
 *        стабильным GUID из WintunSettings.
 *     3) Назначает IPv4-адрес адаптеру (WintunAdapter::ConfigureIpv4).
 *     4) Ставит split-tunnel маршруты 0.0.0.0/1 + 128.0.0.0/1 через
 *        RouteInstaller (Option 2b, §6.7-§6.8 плана).
 *     5) Открывает data-сессию (WintunSession).
 *     6) Поднимает встроенный tun2socks-движок
 *        (Tun2SocksEngineEmbedded, ITunEngine), который принимает
 *        TCP-соединения из TUN'а и форвардит их на 127.0.0.1:relay_port.
 *
 *   Только engine="embedded" поддерживается в WP10. External-движок и
 *   Socks5Adapter появятся в WP12/WP12a.
 *
 * TEARDOWN (Close):
 *   LIFO-порядок: engine.Stop → session end → маршруты Uninstall →
 *   адаптер Close → API Unload.  Каждый шаг идемпотентен и не бросает —
 *   мы хотим, чтобы Close всегда доводил уборку до конца.
 *
 * RULE ENGINE (Задача 2):
 *   Wintun-путь по Option 2b маршрутизирует ВЕСЬ IPv4-TCP через туннель.
 *   ТЕПЕРЬ embedded-движок фильтрует трафик ПО ПРОЦЕССУ внутри себя: в момент
 *   accept'а он резолвит процесс-источник по source-порту и применяет те же
 *   apps[]-правила RuleEngine, что и WinDivert (PROXY / DIRECT / BLOCK).
 *   Управляется полем wintun.process_filter_enabled (по умолчанию true).
 *   При DIRECT движок открывает прямой outbound на оригинальный dst (минуя
 *   прокси); при BLOCK — отклоняет соединение (tcp_abort).
 *   ОГРАНИЧЕНИЕ: external-движок (tun2socks.exe) НЕ поддерживает фильтрацию
 *   по процессу на уровне соединения (PID на границе SOCKS5 — это сам
 *   tun2socks.exe).  Для external весь трафик проксируется как раньше.
 *
 * STATS:
 *   RX/TX-байты и число активных flow'ов берутся у ITunEngine (если он
 *   поднят); иначе — нули.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../domain/ports/ICapture.h"
#include "../../domain/ports/IConnectionMonitor.h"
#include "../../domain/ports/IConnectionTable.h"
#include "../../domain/services/RuleEngine.h"
#include "../config/Config.h"

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {
class WintunApi;
class WintunAdapter;
class WintunSession;
class ITunEngine;
} // namespace wintun
} // namespace capture

/**
 * @brief ICapture-реализация на базе Wintun + tun2socks (embedded).
 *
 * ВНЕШНИЙ КОНТРАКТ — тот же, что у [`WinDivertCapture`](WinDivertCapture.h:41):
 *   Open/Close/IsOpen + ICapture-геттеры/сеттеры.  Внутри — совсем другой
 *   механизм: не WinDivert-фильтр, а полноценный TUN-интерфейс с
 *   пользовательским TCP-стеком (lwIP) внутри.
 */
class WintunCapture : public domain::ports::ICapture {
public:
    /**
     * @brief Конструктор.
     *
     * @param settings    Настройки секции wintun из config.json (копия).
     * @param relay_port  Порт локального TcpRelayServer.  Именно в этот
     *                    порт embedded-движок будет форвардить каждое
     *                    принятое из туннеля TCP-соединение.
     * @param log         Опциональный ILogSink.  Может быть nullptr — тогда
     *                    капчур молча делает свою работу без логов.
     */
    WintunCapture(WintunSettings settings,
                  uint16_t relay_port,
                  domain::ports::ILogSink* log = nullptr);

    ~WintunCapture() override;

    WintunCapture(const WintunCapture&) = delete;
    WintunCapture& operator=(const WintunCapture&) = delete;
    WintunCapture(WintunCapture&&) = delete;
    WintunCapture& operator=(WintunCapture&&) = delete;

    // --- ICapture: жизненный цикл ---

    bool Open() override;
    void Close() override;
    bool IsOpen() const override { return m_open.load(std::memory_order_acquire); }

    // --- ICapture: конфигурация (в Wintun-режиме частично no-op) ---

    void SetTargetProcess(const std::wstring& exePath) override {
        // В Wintun-режиме матчинг делает RuleEngine (apps[]).  Поле сохраняем
        // только для симметрии с WinDivertCapture и потенциального логирования.
        m_targetProcessPath = exePath;
    }

    void SetRelayPort(uint16_t port) override { m_relayPort = port; }

    void SetProxyConfig(const std::string& host, uint16_t port) override {
        m_proxyHost = host;
        m_proxyPort = port;
    }

    void SetConnectionTable(domain::ports::IConnectionTable* table) override {
        m_connTable = table;
    }

    // --- ICapture: события перенаправления (в Wintun-режиме не используются) ---

    std::vector<domain::RedirectEvent> GetPendingRedirects(
        uint32_t /*timeout_ms*/ = 1000) override { return {}; }

    bool AckRedirect(uint64_t /*redirect_id*/) override { return true; }

    // --- ICapture: статистика/системные ---

    domain::DriverStats GetStats() override { return {}; }
    void* GetEventHandle() const override { return nullptr; }

    // --- ICapture: полиморфные accessor-ы (WP6) ---

    bool SetRuleEngine(domain::services::RuleEngine* engine) override {
        m_ruleEngine = engine;
        return true;
    }

    uint64_t GetTotalRxBytes()  const override;
    uint64_t GetTotalTxBytes()  const override;
    uint32_t GetActiveConnections() const override;

    // --- Инфраструктурные setters (не в ICapture, симметрия с WinDivertCapture) ---

    void SetLogSink(domain::ports::ILogSink* sink) { m_log = sink; }
    void SetConnectionMonitor(domain::ports::IConnectionMonitor* mon) {
        m_connectionMonitor = mon;
    }

    /// Read-only доступ к сохранённым Wintun-настройкам.
    const WintunSettings& GetSettings() const { return m_settings; }

private:
    // Аккуратный TearDown — вызывается из Close() и из ветвей ошибки в Open().
    // Идемпотентен, порядок LIFO (см. §6.6 плана).
    void TearDown();

    // Разбирает "a.b.c.d/N" из WintunSettings.tunnel_ipv4_cidr и возвращает
    // host-часть как wide-строку "a.b.c.d" — используется как next-hop
    // для split-tunnel маршрутов (сам TUN-интерфейс является шлюзом).
    // false при ошибке парсинга (пишет outError).
    static bool ExtractGatewayFromCidr(const std::string& cidr,
                                       std::wstring& gwOut,
                                       std::string* outError);

    // Опционально парсит adapter_guid из WintunSettings в GUID.  Если поле
    // пустое или некорректное — возвращает std::nullopt (адаптер получит
    // случайный GUID от wintun'а).
    static bool TryParseGuid(const std::string& s, GUID& out);

    // Логирование — тонкая обёртка вокруг ILogSink с nullptr-guard'ом.
    void LogInfo (const std::string& msg) const;
    void LogWarn (const std::string& msg) const;
    void LogError(const std::string& msg) const;
    void LogDebug(const std::string& msg) const;

    // --- Настройки ---

    WintunSettings                             m_settings;
    uint16_t                                   m_relayPort;
    domain::ports::ILogSink*                   m_log = nullptr;
    domain::services::RuleEngine*              m_ruleEngine = nullptr;
    domain::ports::IConnectionTable*           m_connTable = nullptr;
    domain::ports::IConnectionMonitor*         m_connectionMonitor = nullptr;
    std::wstring                               m_targetProcessPath;
    std::string                                m_proxyHost;
    uint16_t                                   m_proxyPort = 0;

    // --- Runtime state ---

    // shared_ptr — сессия/движок держат живой API, пока живы они.
    std::shared_ptr<capture::wintun::WintunApi>     m_api;
    std::unique_ptr<capture::wintun::WintunAdapter> m_adapter;
    std::shared_ptr<capture::wintun::WintunSession> m_session;
    std::unique_ptr<capture::wintun::ITunEngine>    m_engine;
    bool                                       m_routesInstalled = false;
    // IPv6 catch-all маршруты (::/1 + 8000::/1) — при wintun.block_ipv6=true.
    bool                                       m_ipv6RoutesInstalled = false;

    // C3-фикс: host-bypass /32 к вышестоящему прокси (мимо туннеля), чтобы
    // исходящее соединение relay→proxy не заворачивалось обратно в TUN (петля)
    // и удалённый прокси оставался достижимым.  Ставим bypass для ВСЕХ
    // A-записей прокси (round-robin/failover), а не только для первой — иначе
    // relay, резолвящий имя независимо, мог бы выбрать не-покрытый IP.
    std::vector<uint32_t>                      m_bypassIps;

    // DNS/UDP-митигация: физический /32 bypass для системных DNS-серверов.
    // lwIP собран с LWIP_UDP=0 — UDP-DNS, попавший в TUN по лестнице, молча
    // дропается, и имя-резолвинг DIRECT-приложений ломается («большинство
    // трафика не проходит»).  Ставим bypass ДО лестницы, чтобы DNS (UDP и TCP)
    // шёл напрямую по физике.  Снимаются на Close (см. TearDown).
    std::vector<uint32_t>                      m_dnsBypassIps;

    std::atomic<bool>                          m_open{false};
};

} // namespace infrastructure
} // namespace tcp_redirector
