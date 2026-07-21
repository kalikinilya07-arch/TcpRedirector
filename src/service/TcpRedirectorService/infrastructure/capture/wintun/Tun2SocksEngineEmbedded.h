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
#include <ifdef.h>   // NET_LUID (для SetTunLuid / резолва физ. egress'а)

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
namespace ports { class ILogSink; class IConnectionMonitor; class IConnectionTable; }
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
 * @brief (Задача 3) Конфигурация DIRECT-passthrough внутри embedded-движка.
 *
 * Option 1 (primary): при enabled=true DIRECT-flow'ы (трафик приложений НЕ из
 * списка перенаправления) не дропаются (tcp_abort), а проходят наружу через
 * per-flow outbound-сокет, пиннутый к физическому NIC через IP_UNICAST_IF
 * (чтобы SYN не заворачивался обратно в TUN).  Асинхронный connect + качалка
 * данных выполняются на per-flow sock_reader-треде (НИКОГДА не на engine-треде).
 *
 * Option 2a (optimization): при route_optimization=true для установленных
 * DIRECT-назначений ставится динамический <dst>/32 bypass-маршрут через
 * физический шлюз, чтобы последующий трафик к тому же IP уходил мимо TUN на
 * уровне ОС.  /32 НИКОГДА не ставится для dst, используемого PROXY-flow'ом.
 *
 * fallback_proxy — что делать, когда физический egress установить не удалось:
 *   false (drop)  → tcp_abort (безопасное поведение по умолчанию);
 *   true  (proxy) → отправить flow через relay/прокси.
 *
 * IPv4-only: IPv6 по-прежнему нейтрализуется (SetBlockIpv6).
 */
struct EmbeddedDirectConfig {
    bool         enabled = false;            //!< direct_passthrough.
    bool         fallback_proxy = false;     //!< direct_fallback == "proxy".
    bool         route_optimization = true;  //!< direct_route_optimization (Option 2a).
    std::string  egress_interface;           //!< direct_egress_interface (имя/ifIndex/IP; "" = авто).
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

    /**
     * @brief Задать монитор соединений для GUI-трассировки (Задача 4).
     *
     * Если задан, движок регистрирует каждый установленный flow в
     * ConnectionTracker (AddConnection на accept, RemoveConnection на close),
     * чтобы блок трассировки в GUI показывал соединения и в Wintun embedded
     * режиме (а не только в WinDivert).  Должно вызываться ДО Start().
     */
    void SetConnectionMonitor(domain::ports::IConnectionMonitor* mon) {
        m_conn_monitor = mon;
    }

    /**
     * @brief Задать таблицу соединений для восстановления original-dst в relay.
     *
     * КРИТИЧНО для форвардинга через прокси.  Relay восстанавливает оригинальный
     * адрес назначения ТОЛЬКО через ConnectionTable::Get(client_port).  В отличие
     * от WinDivert (который сохраняет source-порт приложения), embedded-движок
     * открывает НОВЫЙ loopback-сокет к relay со своим эфемерным портом, поэтому
     * обязан сам зарегистрировать (loopback_port -> original-dst) ДО connect().
     * Без этого relay пишет "No connection record for port X" и закрывает
     * соединение — трафик виден в туннеле, но НЕ форвардится к прокси.
     * Не owned.  Должно вызываться ДО Start().
     */
    void SetConnectionTable(domain::ports::IConnectionTable* table) {
        m_conn_table = table;
    }

    /**
     * @brief Включить нейтрализацию IPv6: движок отвечает TCP RST на IPv6-SYN,
     *        приходящий из туннеля (не-TCP IPv6 дропается), чтобы приложение
     *        мгновенно откатилось на IPv4 (туннель/relay/CONNECT — только IPv4).
     *        Работает вместе с IPv6 catch-all маршрутами (WintunCapture).
     *        Должно вызываться ДО Start().
     */
    void SetBlockIpv6(bool enabled) { m_block_ipv6 = enabled; }

    /**
     * @brief (Задача 3) Задать конфигурацию DIRECT-passthrough (Option 1 + 2a).
     *
     * Должно вызываться ДО Start().  При direct.enabled=false движок работает
     * по-старому — DIRECT-flow'ы дропаются (tcp_abort), нулевое изменение
     * поведения.  См. EmbeddedDirectConfig.
     */
    void SetDirectConfig(const EmbeddedDirectConfig& direct) {
        m_direct = direct;
    }

    /**
     * @brief (Корневой фикс DIRECT) Задать LUID Wintun-адаптера, чтобы движок
     *        при резолве физического egress'а МОГ ИСКЛЮЧИТЬ туннель.
     *
     * Без этого GetBestRoute2 к произвольному dst возвращает САМ Wintun
     * (его split-tunnel /1-лестница выигрывает LPM у физического /0), и
     * DIRECT-сокет, запиннутый по такому ifIndex, отправляет SYN обратно в
     * туннель -> нет egress'а -> таймаут («трафик не-целевых приложений
     * не проходит»).  Должно вызываться ДО Start().
     *
     * @param luid LUID Wintun-адаптера (WintunAdapter::Luid()).
     */
    void SetTunLuid(NET_LUID luid) { m_tun_luid = luid; }

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

    // per-flow socket-half reader thread entry (PROXY-flow: relay-сокет).
    void FlowSocketReader(Flow* flow);

    // --- Задача 3: DIRECT-passthrough (Option 1 + Option 2a) ---

    // per-flow тред DIRECT-flow'а: НЕблокирующий connect к оригинальному dst
    // через физически-пиннутый сокет, затем двунаправленная качалка данных
    // (та же схема, что FlowSocketReader, но connect + physical egress).
    // ВСЯ блокирующая работа — здесь, НИКОГДА на engine-треде.
    void DirectFlowThread(Flow* flow);

    // Резолвит физический интерфейс egress для dst_be (network byte order):
    // ifIndex + source-IPv4 (network byte order) через GetBestRoute2, с учётом
    // ручного override m_direct.egress_interface.  Возвращает false, если
    // физический путь определить нельзя (тогда применяется direct_fallback).
    // out_if_index/out_src_be — как раньше; дополнительно out_luid/out_nexthop_be
    // возвращают физический интерфейс/шлюз для корректного Option 2a /32-bypass.
    bool ResolvePhysicalEgress(uint32_t dst_be,
                               uint32_t& out_if_index,
                               uint32_t& out_src_be,
                               NET_LUID& out_luid,
                               uint32_t& out_nexthop_be);

    // Пиннит сокет s к физическому интерфейсу: IP_UNICAST_IF (primary, индекс в
    // NETWORK byte order) + source-IP bind (fallback).  false — не удалось.
    bool PinSocketToPhysical(SOCKET s, uint32_t if_index, uint32_t src_be);

    // Option 2a: пытается поставить <dst_be>/32 bypass через ЯВНО заданный
    // физический интерфейс/шлюз (if_index/luid/next_hop_be из
    // ResolvePhysicalEgress), если dst не используется PROXY-flow'ом.  Refcount
    // по dst.  Вызывается из DIRECT-треда.  Передача физ. egress'а явно —
    // критично: иначе InstallHostBypass в рантайме резолвит Wintun и bypass
    // указывает обратно в туннель (бесполезен/вреден).
    // iter-2: возвращает true, если /32-bypass гарантированно стоит для dst
    // (установлен сейчас ИЛИ уже был установлен и refcount увеличен).  Ставится
    // ДО connect() как ПРЕДУСЛОВИЕ DIRECT-egress'а, а не как пост-оптимизация.
    bool MaybeInstallDirectBypass(uint32_t dst_be, uint32_t if_index,
                                  NET_LUID luid, uint32_t next_hop_be);
    // Декремент refcount; при обнулении снимает /32-маршрут.
    void ReleaseDirectBypass(uint32_t dst_be);
    // Регистрирует dst как «используемый PROXY»; если для него был поставлен
    // DIRECT /32 bypass — немедленно снимает его (guard корректности).
    void NoteProxyDestination(uint32_t dst_be);
    // Снимает ВСЕ оставшиеся DIRECT /32 bypass-маршруты (Stop/shutdown).
    void RemoveAllDirectBypasses();

    // Reaper-тред: join'ит завершившиеся per-flow ридер-треды и уничтожает
    // Flow-объекты ВНЕ engine-треда/ридера (иначе дедлок/UAF). См. .cpp.
    void ReaperThreadMain();

    // Переносит владение Flow из m_flows в очередь на утилизацию и будит
    // reaper.  Вызывается ТОЛЬКО из engine-треда (внутри CloseFlow).
    void RetireFlow(Flow* flow);

    // Периодически из EngineThreadMain: закрывает flow'ы, чей socket-half
    // ридер уже завершился (EOF/ошибка relay), но которые ещё не закрыты со
    // стороны туннеля — устраняет утечку half-open flow'ов.
    void ReapFinishedReaders();

    // lwIP callback trampolines живут в анонимном namespace .cpp — их
    // сигнатуры зависят от lwIP-типов, а тянуть lwIP-headers в .h мы не хотим
    // (весь наружный API этого класса — pure C++/WinAPI).  friend-декларация
    // нужна, чтобы trampolines могли достучаться до приватного состояния.
    friend struct EngineTramp;

    // Утилиты
    // reason — короткая метка ИСТОЧНИКА закрытия (для диагностики: reaper vs
    // lwIP-callback vs stop-flag).  Логируется на Debug в начале CloseFlow.
    void CloseFlow(Flow* flow, bool from_engine_thread,
                   const char* reason = "unspecified");

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
     * @param out_pid       [out] PID процесса-источника (0, если не определён).
     * @param out_proc_name [out] короткое имя процесса ("" если неизвестно).
     * @return Proxy / Direct / Block.  Если фильтрация выключена — всегда Proxy.
     */
    FlowDecision DecideFlow(const FlowMeta& meta,
                            uint32_t& out_pid,
                            std::wstring& out_proc_name);

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

    // Задача 4: монитор соединений для GUI-трассировки (не owned).
    domain::ports::IConnectionMonitor* m_conn_monitor = nullptr;
    std::atomic<uint64_t>          m_next_conn_id{1};

    // Таблица соединений (не owned): embedded-движок регистрирует в ней
    // (loopback-src-port -> original-dst) для каждого PROXY-flow, чтобы relay
    // мог восстановить адрес назначения при accept'е loopback-сокета движка
    // (см. SetConnectionTable).  nullptr → форвардинг к прокси невозможен.
    domain::ports::IConnectionTable* m_conn_table = nullptr;

    // Нейтрализация IPv6: RST на IPv6-SYN, дроп прочего IPv6 (см. SetBlockIpv6).
    bool                           m_block_ipv6 = false;

    // Задача 3: конфигурация DIRECT-passthrough (Option 1 + 2a).  См.
    // EmbeddedDirectConfig.  m_direct.enabled=false → DIRECT дропается (как было).
    EmbeddedDirectConfig           m_direct;

    // Корневой фикс DIRECT: LUID Wintun-адаптера — исключается при резолве
    // физического egress'а, чтобы GetBestRoute2 не вернул сам туннель (см.
    // SetTunLuid / ResolvePhysicalEgress).  {0} = не задан (тогда резолвер
    // работает как раньше — небезопасно, но не крашит).
    NET_LUID                       m_tun_luid{};

    // Option 2a: учёт динамических DIRECT /32 bypass-маршрутов и guard'а против
    // проксируемых dst.  Ключи — dst IPv4 в NETWORK byte order.
    //   • m_bypass_refcount: dst -> число активных DIRECT-flow'ов, для которых
    //     установлен /32 bypass (refcount → снятие при обнулении);
    //   • m_proxy_dsts: dst, замеченные на PROXY-flow'ах — для них /32 bypass
    //     НИКОГДА не ставится (а если был — снимается).  Корректностный guard.
    std::mutex                     m_bypass_mu;
    std::unordered_map<uint32_t, uint32_t> m_bypass_refcount;
    std::unordered_set<uint32_t>   m_proxy_dsts;

    // Разбор IPv6-пакета из туннеля: при block_ipv6 отвечает TCP RST на SYN,
    // иначе просто дропает.  Возвращает true, если пакет обработан (не должен
    // идти в lwIP).  Реализация в .cpp.
    bool HandleIpv6Packet(const uint8_t* pkt, int len);

    HANDLE                         m_stop_event = nullptr;
    std::thread                    m_engine_thread;
    std::atomic<bool>              m_running{false};

    // Глобальный «core lock» — единая критическая секция вокруг любого вызова
    // lwIP API из «внешних» тредов (socket-half ридеры).  engine-тред держит
    // его перманентно, пока крутится — беря/отдавая только на wait'ах.
    // Реализация: std::recursive_mutex (сокет-ридер держит его пока пушит
    // байты и обновляет счётчики).
    mutable std::recursive_mutex   m_core_lock;

    // Список активных flow'ов.
    //
    // КЛЮЧ — УНИКАЛЬНЫЙ монотонный flow-id (m_next_flow_id), А НЕ указатель
    // на pcb.  Это критично: lwIP ПЕРЕИСПОЛЬЗУЕТ адреса tcp_pcb из пула
    // (MEMP_TCP_PCB).  Если ключом был бы pcb-указатель, то новый OnAccept с тем же
    // (переиспользованным) адресом вызвал бы emplace с уже существующим ключом:
    // emplace НЕ перезаписывает → новый unique_ptr<Flow> тут же уничтожается, а
    // raw-указатель (уже в tcp_arg и в sock_reader) остаётся висячим → use-after-free
    // в CloseFlow/OnRecv (именно этот краш 0xC0000005 в CloseFlow был найден в
    // дампе).  Уникальный id полностью устраняет эту коллизию.
    // owning storage — unique_ptr в значении.
    std::mutex                     m_flows_mu;
    std::unordered_map<uint64_t, std::unique_ptr<Flow>> m_flows;
    std::atomic<uint64_t>          m_next_flow_id{1};

    // --- Reaper: безопасное уничтожение закрытых flow'ов ---
    // CloseFlow (из engine-треда) переносит владение Flow сюда; reaper-тред
    // join'ит socket-ридер и уничтожает Flow.  Так engine-тред не блокируется
    // на join'е ридера (который может ждать m_core_lock — иначе дедлок).
    std::mutex                     m_retire_mu;
    std::condition_variable        m_retire_cv;
    std::vector<std::unique_ptr<Flow>> m_retire;
    std::thread                    m_reaper_thread;
    std::atomic<bool>              m_reaper_run{false};

    // Указатели на lwIP-объекты (void*, чтобы не тянуть lwIP-headers в .h)
    void*                          m_netif = nullptr;        // struct netif*
    void*                          m_listen_pcb = nullptr;   // struct tcp_pcb*

    // Счётчики для ITunEngine
    std::atomic<uint64_t>          m_rx_bytes{0};
    std::atomic<uint64_t>          m_tx_bytes{0};
    std::atomic<uint32_t>          m_active_flows{0};

    // Диагностические счётчики (T5): пакеты IPv4/IPv6 из туннеля, RST на IPv6,
    // дропнутый IPv6.  Периодически логируются в EngineThreadMain (TRACE).
    std::atomic<uint64_t>          m_pkts_v4{0};
    std::atomic<uint64_t>          m_pkts_v6{0};
    std::atomic<uint64_t>          m_ipv6_rst{0};
    std::atomic<uint64_t>          m_ipv6_dropped{0};

    // Одноразовая инициализация lwIP (per-process).  См. Start() и §"NO_SYS notes"
    // выше — Start после Stop в v1 не поддерживается.
    static std::atomic<bool>       s_lwip_inited;
};

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
