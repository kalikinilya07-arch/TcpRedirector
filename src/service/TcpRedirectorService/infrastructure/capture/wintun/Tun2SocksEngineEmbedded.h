#pragma once

/**
 * @file Tun2SocksEngineEmbedded.h
 * @brief WP9 — встроенный tun2socks-движок на lwIP 2.2.0.
 *
 * ЖИЗНЕННЫЙ ЦИКЛ:
 *   engine = Tun2SocksEngineEmbedded::Create(session, relayPort, onFlow?);
 *   engine->Start(&err);              // поднимает lwIP + engine-тред
 *   ...                               // движок сам crank'ает трафик
 *   engine->Stop();                   // закрывает все flow'ы, снимает netif
 *
 * ЧТО ДЕЛАЕТ ДВИЖОК:
 *   1. Инициализирует lwIP один раз на процесс (см. §"NO_SYS notes" ниже).
 *   2. Создаёт struct netif, чей linkoutput отправляет исходящие IP-пакеты
 *      обратно в туннель через WintunSession::Send().
 *   3. Запускает dedicated engine-тред, который:
 *        • Ждёт на WaitForMultipleObjects({ session.ReadWaitEvent, stop_event, timer }).
 *        • При событии от wintun'а — читает пакет (WintunSession::ReceiveInto),
 *          заворачивает в pbuf и толкает в netif->input.
 *        • При таймере — вызывает sys_check_timeouts().
 *        • При stop_event — выходит.
 *   4. Держит catch-all TCP-listener: pcb, забинденный на IP_ANY_TYPE:0, чей
 *      accept-хук LWIP_HOOK_IP4_INPUT (см. lwip_hooks_impl.c) заставляет
 *      lwIP «принять» ЛЮБОЙ TCP-пакет из туннеля.
 *   5. На каждый accept:
 *        • Запоминает pcb->local_ip:local_port как original-dst.
 *        • Открывает NON-BLOCKING SOCKET к 127.0.0.1:relayPort.
 *        • Регистрирует tcp_recv/tcp_sent/tcp_err на pcb + запускает per-flow
 *          «socket→pcb» ридер-тред.  «pcb→socket» половина работает целиком
 *          в engine-треде через tcp_recv-callback.
 *        • Опционально вызывает onFlow(FlowMeta{originalDstIp, port, ...}).
 *
 * NO_SYS notes:
 *   • lwIP_init() вызывается ЕДИНОЖДЫ на процесс — повторный Start после Stop
 *     в текущем WP не поддерживается (см. Start() ниже, guard'ится флагом
 *     s_lwip_inited).  Причин две: (а) lwip_init не имеет обратной
 *     lwip_deinit, (б) наш netif удаляется через netif_remove при Stop, но
 *     подсистемы вроде mempool остаются в глобальном состоянии — Start после
 *     Stop может выделять из «мусора».  WP10, если понадобится, обернёт это.
 *   • Весь TCP-API lwIP вызывается ТОЛЬКО из engine-треда.  Все внешние
 *     события (socket-half ридеры) синхронизируются с ним через SRWLOCK
 *     `m_core_lock` — критическая секция вокруг любого вызова
 *     tcp_write/tcp_recved/tcp_output/tcp_close.
 *
 * ЭТОТ WP не подключает движок к сервису.  WP10 обернёт его в WintunCapture.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "ITunEngine.h"
#include "WintunSession.h"
#include "../../process/ProcessResolver.h"

// Форвард-декларации, чтобы не тянуть тяжёлые заголовки RuleEngine/ILogSink
// в публичный .h движка.  LogLevel — enum class с фиксированным
// базовым типом, поэтому его можно форвард-объявить (реальное определение
// приходит через IConnectionMonitor.h/ProxyConfig.h в .cpp).
namespace tcp_redirector {
namespace domain {
enum class LogLevel;
namespace services { class RuleEngine; }
namespace ports { class ILogSink; }
}
}

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

/**
 * @brief Метаданные принятого TCP-flow (для observer-хука).
 *
 * Заполняются в момент tcp_accept ДО того, как байты начали течь.  Строки
 * IP — точечная форма (a.b.c.d).  Порты — host byte order.
 */
struct FlowMeta {
    std::string original_dst_ip;   //!< dotted-IPv4, например "10.6.7.42".
    uint16_t    original_dst_port; //!< host byte order.
    std::string source_ip;         //!< dotted-IPv4 источника (TUN-side ephemeral).
    uint16_t    source_port;       //!< host byte order.
};

/**
 * @brief Конфигурация фильтрации по процессу внутри embedded-движка.
 *
 * Задача 2: раньше embedded-движок гнал ВЕСЬ IPv4-TCP через relay (прокси)
 * без разбора процесса-источника (Option 2b).  Теперь, при enabled=true,
 * движок в момент accept'а резолвит процесс-источник по source-порту и
 * применяет те же правила RuleEngine (apps[] v2 + legacy), что и WinDivert:
 *   • PROXY  → форвард на 127.0.0.1:relay_port (как раньше);
 *   • DIRECT → прямой outbound-сокет на реальный original-dst (минуя прокси);
 *   • BLOCK  → tcp_abort (соединение отклоняется).
 *
 * Все поля опциональны, кроме rule_engine (без него фильтрация no-op).
 */
struct EmbeddedProcessFilter {
    bool                          enabled = false;        //!< Включить фильтрацию.
    domain::services::RuleEngine* rule_engine = nullptr;  //!< Правила (не owned).
    std::wstring                  target_process_path;    //!< Legacy fallback-путь.
    bool                          proxy_configured = true;//!< Есть ли валидный прокси.
    domain::ports::ILogSink*      log = nullptr;          //!< Опциональный лог (тег "wintun").
};

/**
 * @brief Встроенный tun2socks-движок (реализация ITunEngine).
 *
 * Все методы, кроме конструктора/Start/Stop, потоко-безопасны.
 */
class Tun2SocksEngineEmbedded : public ITunEngine {
public:
    /**
     * @brief Конструктор.
     *
     * @param session   Уже открытая WintunSession (owned выше по стеку).
     *                  Мы держим shared_ptr — движок не переживает сессию,
     *                  но и наоборот тоже: Stop() не закрывает сессию.
     * @param relay_port Порт локального TcpRelayServer (127.0.0.1:relay_port).
     *                   Именно туда движок форвардит каждый принятый flow.
     * @param on_flow   [опционально] observer, вызываемый при каждом
     *                  успешно установленном flow'е.  Выполняется в engine-треде;
     *                  callback обязан быть быстрым, иначе он тормозит весь стек.
     */
    Tun2SocksEngineEmbedded(std::shared_ptr<WintunSession> session,
                            uint16_t relay_port,
                            std::function<void(const FlowMeta&)> on_flow = {});

    /**
     * @brief Задать конфигурацию фильтрации по процессу (Задача 2).
     *
     * Должно вызываться ДО Start().  Если filter.enabled=false или
     * rule_engine=nullptr — движок работает по-старому (всё через relay).
     */
    void SetProcessFilter(const EmbeddedProcessFilter& filter) {
        m_filter = filter;
    }

    ~Tun2SocksEngineEmbedded() override;

    Tun2SocksEngineEmbedded(const Tun2SocksEngineEmbedded&) = delete;
    Tun2SocksEngineEmbedded& operator=(const Tun2SocksEngineEmbedded&) = delete;
    Tun2SocksEngineEmbedded(Tun2SocksEngineEmbedded&&) = delete;
    Tun2SocksEngineEmbedded& operator=(Tun2SocksEngineEmbedded&&) = delete;

    // --- ITunEngine ---
    bool Start(std::string* outError) override;
    void Stop() override;
    uint64_t RxBytes() const override { return m_rx_bytes.load(std::memory_order_relaxed); }
    uint64_t TxBytes() const override { return m_tx_bytes.load(std::memory_order_relaxed); }
    uint32_t ActiveFlows() const override { return m_active_flows.load(std::memory_order_relaxed); }

private:
    // Полное объявление скрыто в .cpp — здесь только opaque struct fwd
    // (per-flow state держит tcp_pcb*, socket, буферы, ридер-тред).
    struct Flow;

    // engine-thread entry
    void EngineThreadMain();

    // per-flow socket-half reader thread entry
    void FlowSocketReader(Flow* flow);

    // lwIP callback trampolines живут в анонимном namespace .cpp — их
    // сигнатуры зависят от lwIP-типов, а тянуть lwIP-headers в .h мы не хотим
    // (весь наружный API этого класса — pure C++/WinAPI).  friend-декларация
    // нужна, чтобы trampolines могли достучаться до приватного состояния.
    friend struct EngineTramp;

    // Утилиты
    void CloseFlow(Flow* flow, bool from_engine_thread);

    // --- Задача 2: фильтрация по процессу ---

    /// Решение для одного flow'а (аналог WinDivertCapture::CheckProcessRule).
    enum class FlowDecision { Proxy, Direct, Block };

    /**
     * @brief Определить действие для принятого flow'а по процессу-источнику.
     *
     * Резолвит PID по source-порту, получает путь процесса, спрашивает
     * RuleEngine (MatchForFlow → legacy Match → target-path/дерево процессов).
     * Вызывается ВНЕ core-lock (резолв PID может быть тяжёлым).
     *
     * @param meta FlowMeta принятого соединения (source/dst).
     * @return Proxy / Direct / Block.  Если фильтрация выключена — всегда Proxy.
     */
    FlowDecision DecideFlow(const FlowMeta& meta);

    /// Тонкая обёртка логирования (nullptr-safe, тег "wintun").
    void FilterLog(domain::LogLevel level, const std::string& msg) const;

    // --- члены ---
    std::shared_ptr<WintunSession> m_session;
    uint16_t                       m_relay_port;
    std::function<void(const FlowMeta&)> m_on_flow;

    // Задача 2: конфигурация фильтрации + резолвер процесса.
    EmbeddedProcessFilter          m_filter;
    process::ProcessResolver       m_resolver;
    std::atomic<uint32_t>          m_target_pid{0};

    HANDLE                         m_stop_event = nullptr;
    std::thread                    m_engine_thread;
    std::atomic<bool>              m_running{false};

    // Глобальный «core lock» — единая критическая секция вокруг любого вызова
    // lwIP API из «внешних» тредов (socket-half ридеры).  engine-тред держит
    // его перманентно, пока крутится — беря/отдавая только на wait'ах.
    // Реализация: std::recursive_mutex (сокет-ридер держит его пока пушит
    // байты и обновляет счётчики).
    mutable std::recursive_mutex   m_core_lock;

    // Список активных flow'ов.  key — void* на pcb (только для быстрой
    // диагностики; owning storage — unique_ptr в значении).
    std::mutex                     m_flows_mu;
    std::unordered_map<void*, std::unique_ptr<Flow>> m_flows;

    // Указатели на lwIP-объекты (void*, чтобы не тянуть lwIP-headers в .h)
    void*                          m_netif = nullptr;        // struct netif*
    void*                          m_listen_pcb = nullptr;   // struct tcp_pcb*

    // Счётчики для ITunEngine
    std::atomic<uint64_t>          m_rx_bytes{0};
    std::atomic<uint64_t>          m_tx_bytes{0};
    std::atomic<uint32_t>          m_active_flows{0};

    // Одноразовая инициализация lwIP (per-process).  См. Start() и §"NO_SYS notes"
    // выше — Start после Stop в v1 не поддерживается.
    static std::atomic<bool>       s_lwip_inited;
};

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
