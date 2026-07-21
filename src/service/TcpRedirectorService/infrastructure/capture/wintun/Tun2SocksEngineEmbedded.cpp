/**
 * @file Tun2SocksEngineEmbedded.cpp
 * @brief WP9 — реализация встроенного tun2socks-движка на lwIP 2.2.0.
 *
 * Архитектурные детали см. в Tun2SocksEngineEmbedded.h.  Здесь — конкретика.
 *
 * КОНТРАКТ ТРЕДОВ:
 *   • engine-тред (m_engine_thread) — единственное место, где вызывается
 *     любой lwIP TCP-API за пределами Start()/Stop().  Он же
 *     dequeue'ит pbuf'ы из wintun'а.
 *   • per-flow socket-half тред — читает из loopback-сокета, кладёт байты
 *     в pcb через tcp_write под m_core_lock.
 *   • Все обращения к lwIP из socket-half треда обёрнуты в
 *     std::lock_guard<std::recursive_mutex>(m_core_lock).  engine-тред
 *     держит этот же lock, пока крутит sys_check_timeouts/tcp_input.
 *
 *   Такой упрощённый локинг возможен, потому что engine-тред sleep'ает
 *   на WaitForMultipleObjects, а socket-half треды блокируются на
 *   recv() — оба они большую часть времени не держат lock.  Для v1
 *   этой грубой синхронизации хватает.
 */

// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>        // IP_UNICAST_IF (IPPROTO_IP level option)
#include <windows.h>
#include <iphlpapi.h>        // GetBestRoute2, GetAdaptersAddresses
#include <netioapi.h>        // MIB_IPFORWARD_ROW2
// clang-format on

#include "Tun2SocksEngineEmbedded.h"

#include "../../../domain/services/RuleEngine.h"
#include "../../../domain/ports/IConnectionMonitor.h"  // ILogSink / LogLevel / IConnectionMonitor
#include "../../../domain/ports/IConnectionTable.h"     // Add/Remove — relay dst recovery
#include "../../../domain/entities/ProxyConfig.h"       // ConnectionRecord / ConnectionState
#include "../../utf8_convert.h"
#include "RouteInstaller.h"                             // Option 2a: <dst>/32 bypass

#include <algorithm>
#include <cstring>
#include <sstream>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

extern "C" {
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
}

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

/* ------------------------------------------------------------------------- */
/*  Внутренняя структура per-flow                                              */
/* ------------------------------------------------------------------------- */

/**
 * Полное состояние одного TCP flow'а.  Живёт, пока:
 *   • pcb ещё существует, ИЛИ
 *   • socket-half тред ещё бежит.
 * CloseFlow() безопасно вызывается из любого треда.
 */
struct Tun2SocksEngineEmbedded::Flow {
    struct tcp_pcb*             pcb   = nullptr;    // lwIP-side (TUN)
    SOCKET                      sock  = INVALID_SOCKET; // loopback (relay)
    Tun2SocksEngineEmbedded*    owner = nullptr;

    // FlowMeta для observer'а — сохраняем ещё до старта потоков.
    FlowMeta                    meta;

    // socket-half тред.  Читает `sock`, пишет в `pcb` через tcp_write.
    std::thread                 sock_reader;

    // Флаги — atomics, чтобы избежать локов.
    std::atomic<bool>           closing{false};
    std::atomic<bool>           pcb_dead{false}; // set из engine-thread OnErrCb

    // Сколько байт ещё лежит в TX-очереди pcb (заполнили tcp_write, но
    // соответствующий tcp_sent пока не пришёл).  Используется для back-pressure
    // socket-reader'а: он не читает следующий чанк, пока в pcb есть место.
    std::atomic<uint32_t>       pcb_in_flight{0};

    // Задача 4: id записи в ConnectionTracker (0 — не зарегистрирована).
    uint64_t                    conn_id = 0;

    // Эфемерный порт loopback-сокета к relay (host byte order).  Под этим
    // ключом в ConnectionTable лежит (relay_src_port -> original-dst); relay
    // читает его при accept'е.  0 — запись не создавалась (нет таблицы/ошибка).
    uint16_t                    relay_src_port = 0;

    // УНИКАЛЬНЫЙ ключ в m_flows (m_next_flow_id++ на accept'е).  Именно по нему
    // CloseFlow извлекает владеющий unique_ptr.  НЕ pcb-указатель: pcb
    // переиспользуются lwIP'ом и дали бы коллизию emplace → UAF (см. .h).
    uint64_t                    id = 0;

    // Выставляется socket-half ридером при выходе (EOF/ошибка на стороне
    // relay/прокси).  engine-тред периодически (ReapFinishedReaders) закрывает
    // такие flow'ы — устраняет утечку half-open flow'ов (ридер завершился, а
    // CloseFlow со стороны туннеля так и не пришёл).
    std::atomic<bool>           reader_exited{false};

    // --- Задача 3: DIRECT-passthrough ---
    // true  → это DIRECT-flow (outbound на реальный dst через физ. NIC),
    //         НЕ зарегистрирован в ConnectionTable, socket-half тред —
    //         DirectFlowThread (не FlowSocketReader).
    // false → PROXY-flow (relay), как раньше.
    bool                        is_direct = false;
    // dst IPv4 (network byte order) DIRECT-flow'а — для Option 2a bypass-refcount
    // и снятия /32 на закрытии.  0 = bypass для этого flow'а не ставился.
    uint32_t                    direct_dst_be = 0;
    bool                        direct_bypass_installed = false;
};

/* ------------------------------------------------------------------------- */
/*  Статические переменные                                                      */
/* ------------------------------------------------------------------------- */

std::atomic<bool> Tun2SocksEngineEmbedded::s_lwip_inited{false};

/* ------------------------------------------------------------------------- */
/*  Конструктор / деструктор                                                     */
/* ------------------------------------------------------------------------- */

Tun2SocksEngineEmbedded::Tun2SocksEngineEmbedded(
    std::shared_ptr<WintunSession> session,
    uint16_t relay_port,
    std::function<void(const FlowMeta&)> on_flow)
    : m_session(std::move(session)),
      m_relay_port(relay_port),
      m_on_flow(std::move(on_flow)) {
}

Tun2SocksEngineEmbedded::~Tun2SocksEngineEmbedded() {
    Stop();
}

/* ------------------------------------------------------------------------- */
/*  Утилиты                                                                       */
/* ------------------------------------------------------------------------- */

namespace {

/**
 * @brief Форматирует ip_addr_t как a.b.c.d.  Только IPv4.
 */
std::string Ip4ToString(const ip_addr_t* addr) {
    if (addr == nullptr) return "0.0.0.0";
    const uint32_t v = ip4_addr_get_u32(ip_2_ip4(addr)); // network byte order
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    char buf[16];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

/**
 * @brief Разбирает "a.b.c.d/N" в 32-битный IP (network byte order) + prefix.
 *        Копия того, что уже есть в WintunAdapter — не тянем зависимость.
 */
bool ParseCidrV4Utf8(const std::string& cidr, uint32_t& ipBe, uint8_t& prefix) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) return false;
    const std::string ip = cidr.substr(0, slash);
    const std::string mask = cidr.substr(slash + 1);

    struct in_addr in{};
    if (inet_pton(AF_INET, ip.c_str(), &in) != 1) return false;
    ipBe = in.s_addr;
    int p = std::atoi(mask.c_str());
    if (p < 0 || p > 32) return false;
    prefix = static_cast<uint8_t>(p);
    return true;
}

/**
 * @brief Пытается сделать `.1` из «a.b.c.0/24»-подобной CIDR — эвристика
 *        для выбора IP netif'а.  По плану WP9 разрешено использовать
 *        любой IP из туннельной подсети — «gateway = <net>.1» стандартен.
 */
uint32_t GatewayIpFromCidr(uint32_t ipBe, uint8_t prefix) {
    // ipBe network byte order; берём хост-часть.
    if (prefix == 0 || prefix > 30) return ipBe; // fallback — сам IP
    const uint32_t mask = htonl(prefix == 32 ? 0xFFFFFFFFu
                                              : ~((1u << (32 - prefix)) - 1));
    const uint32_t net = ipBe & mask;
    // .1 в конце
    return net | htonl(1);
}

} // namespace

/* ------------------------------------------------------------------------- */
/*  lwIP callbacks (C-совместимые trampolines)                                    */
/*                                                                              */
/*  Все trampolines собраны в структуре EngineTramp (friend'ится из движка),   */
/*  чтобы не таскать lwIP-типы через public API .h.  Функции static, но        */
/*  сигнатуры полностью совпадают с lwIP-tydef'ами (netif_output_fn и т.д.)    */
/*  — иначе MSVC откажется приводить указатели.                                */
/* ------------------------------------------------------------------------- */

struct EngineTramp {
    /** netif init callback — вызывается lwIP один раз при netif_add. */
    static err_t NetifInit(struct netif* nf) {
        nf->name[0] = 't';
        nf->name[1] = 'r'; // "tr" — TcpRedirector TUN
        nf->mtu = 1500;
        nf->hwaddr_len = 0;
        nf->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
        nf->output = &EngineTramp::NetifOutput;
        nf->linkoutput = &EngineTramp::NetifLinkOutput;
        return ERR_OK;
    }

    /** netif->output (IP layer). У нас нет ARP — прямо в linkoutput. */
    static err_t NetifOutput(struct netif* nf, struct pbuf* p,
                              const ip4_addr_t* /*ip4addr*/) {
        return NetifLinkOutput(nf, p);
    }

    /** netif->linkoutput — исходящий IP-пакет должен уйти в Wintun. */
    static err_t NetifLinkOutput(struct netif* nf, struct pbuf* p) {
        auto* engine = static_cast<Tun2SocksEngineEmbedded*>(nf->state);
        if (engine == nullptr || engine->m_session == nullptr) {
            return ERR_IF;
        }
        uint8_t stackbuf[2048];
        std::unique_ptr<uint8_t[]> heapbuf;
        uint8_t* buf = stackbuf;
        if (p->tot_len > sizeof(stackbuf)) {
            heapbuf.reset(new (std::nothrow) uint8_t[p->tot_len]);
            if (!heapbuf) return ERR_MEM;
            buf = heapbuf.get();
        }
        const u16_t copied = pbuf_copy_partial(p, buf, p->tot_len, 0);
        if (copied != p->tot_len) return ERR_BUF;

        const bool ok = engine->m_session->Send(buf, copied);
        if (ok) {
            engine->m_tx_bytes.fetch_add(copied, std::memory_order_relaxed);
            return ERR_OK;
        }
        return ERR_WOULDBLOCK;
    }

    /** tcp_accept trampoline. arg = engine*. */
    static err_t OnAccept(void* arg, struct tcp_pcb* newpcb, err_t err) {
        auto* engine = static_cast<Tun2SocksEngineEmbedded*>(arg);
        if (err != ERR_OK || newpcb == nullptr) return ERR_VAL;

        FlowMeta meta;
        meta.original_dst_ip   = Ip4ToString(&newpcb->local_ip);
        meta.original_dst_port = newpcb->local_port;
        meta.source_ip         = Ip4ToString(&newpcb->remote_ip);
        meta.source_port       = newpcb->remote_port;

        // === Задача 2: фильтрация по процессу ===
        // Определяем действие ДО открытия сокета.  DecideFlow резолвит
        // процесс-источник по source-порту и спрашивает RuleEngine.
        using FlowDecision = Tun2SocksEngineEmbedded::FlowDecision;
        uint32_t     flowPid = 0;
        std::wstring flowProc;
        const FlowDecision decision = engine->DecideFlow(meta, flowPid, flowProc);
        if (decision == FlowDecision::Block) {
            // BLOCK — отклоняем соединение.
            tcp_abort(newpcb);
            return ERR_ABRT;
        }
        if (decision == FlowDecision::Direct) {
            // === Задача 3: DIRECT-passthrough (Option 1) ===
            const uint32_t dst_be =
                ip4_addr_get_u32(ip_2_ip4(&newpcb->local_ip)); // network byte order

            if (!engine->m_direct.enabled) {
                // direct_passthrough=false → прежнее fail-fast поведение: DIRECT
                // дропается (сервис стабилен, нулевое изменение поведения).
                //   • «проксировать ВЕСЬ трафик» → process_filter_enabled=false;
                //   • селективное проксирование С прямым проходом остального →
                //     direct_passthrough=true (эта задача) или capture_mode=windivert.
                engine->FilterLog(domain::LogLevel::Debug,
                    "[wintun][DIRECT-drop] non-matched flow dropped "
                    "(direct_passthrough=false) -> " + meta.original_dst_ip + ":"
                    + std::to_string(meta.original_dst_port));
                tcp_abort(newpcb);
                return ERR_ABRT;
            }

            // Резолвим физический egress (ifIndex + source-IP) через GetBestRoute2.
            // Это БЫСТРЫЙ табличный lookup ОС (не сетевой I/O) — безопасно на
            // engine-треде.  Сам connect + качалка данных выполняются на
            // DirectFlowThread (НИКОГДА не на engine-треде — инвариант №1).
            uint32_t ifIndex = 0, srcBe = 0;
            const bool egressOk = engine->ResolvePhysicalEgress(dst_be, ifIndex, srcBe);
            if (!egressOk) {
                if (engine->m_direct.fallback_proxy && engine->m_conn_table
                    && engine->m_filter.proxy_configured) {
                    // direct_fallback=proxy → падаем в PROXY-ветку ниже.
                    engine->FilterLog(domain::LogLevel::Debug,
                        "[wintun][DIRECT->proxy-fallback] physical egress unresolved for "
                        + meta.original_dst_ip + ":" + std::to_string(meta.original_dst_port)
                        + " — routing via relay");
                    // fall through to PROXY setup below (decision downgraded).
                } else {
                    engine->FilterLog(domain::LogLevel::Warn,
                        "[wintun][DIRECT-drop] physical egress unresolved for "
                        + meta.original_dst_ip + ":" + std::to_string(meta.original_dst_port)
                        + " (direct_fallback=drop) — flow dropped");
                    tcp_abort(newpcb);
                    return ERR_ABRT;
                }
            } else {
                // Открываем per-flow outbound-сокет к РЕАЛЬНОМУ dst.  НЕ биндим к
                // loopback и НЕ регистрируем в ConnectionTable (PROXY-only шаги).
                SOCKET ds = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (ds == INVALID_SOCKET) {
                    tcp_abort(newpcb);
                    return ERR_ABRT;
                }
                // Пиннуем к физическому NIC (IP_UNICAST_IF + source-bind fallback),
                // чтобы SYN не заворачивался обратно в TUN (инвариант №2).
                if (!engine->PinSocketToPhysical(ds, ifIndex, srcBe)) {
                    ::closesocket(ds);
                    if (engine->m_direct.fallback_proxy && engine->m_conn_table
                        && engine->m_filter.proxy_configured) {
                        engine->FilterLog(domain::LogLevel::Debug,
                            "[wintun][DIRECT->proxy-fallback] IP_UNICAST_IF pin failed for "
                            + meta.original_dst_ip + " — routing via relay");
                        // fall through to PROXY setup below.
                    } else {
                        engine->FilterLog(domain::LogLevel::Warn,
                            "[wintun][DIRECT-drop] IP_UNICAST_IF pin failed for "
                            + meta.original_dst_ip + " (direct_fallback=drop) — flow dropped");
                        tcp_abort(newpcb);
                        return ERR_ABRT;
                    }
                } else {
                    BOOL nodelay = TRUE;
                    ::setsockopt(ds, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

                    auto flow = std::make_unique<Tun2SocksEngineEmbedded::Flow>();
                    flow->pcb           = newpcb;
                    flow->sock          = ds;
                    flow->owner         = engine;
                    flow->meta          = meta;
                    flow->is_direct     = true;
                    flow->direct_dst_be = dst_be;

                    if (engine->m_conn_monitor) {
                        domain::ConnectionRecord rec;
                        rec.id = engine->m_next_conn_id.fetch_add(1, std::memory_order_relaxed);
                        rec.pid = flowPid;
                        rec.process_name = flowProc;
                        rec.destination_ip = meta.original_dst_ip;
                        rec.destination_port = meta.original_dst_port;
                        rec.state = domain::ConnectionState::TunnelEstablished;
                        rec.start_time = std::chrono::steady_clock::now();
                        rec.proxy_enabled = false; // DIRECT — не через прокси.
                        flow->conn_id = rec.id;
                        engine->m_conn_monitor->AddConnection(rec);
                    }

                    tcp_arg (newpcb, flow.get());
                    tcp_recv(newpcb, &EngineTramp::OnRecv);
                    tcp_sent(newpcb, &EngineTramp::OnSent);
                    tcp_err (newpcb, &EngineTramp::OnErr);

                    Tun2SocksEngineEmbedded::Flow* raw = flow.get();
                    const uint64_t flow_id =
                        engine->m_next_flow_id.fetch_add(1, std::memory_order_relaxed);
                    flow->id = flow_id;
                    {
                        std::lock_guard<std::mutex> lk(engine->m_flows_mu);
                        engine->m_flows.emplace(flow_id, std::move(flow));
                    }
                    engine->m_active_flows.fetch_add(1, std::memory_order_relaxed);

                    // DirectFlowThread делает НЕблокирующий connect + двунаправленную
                    // качалку (та же async-схема, что PROXY-flow, но к реальному dst).
                    raw->sock_reader = std::thread(
                        [engine, raw]() { engine->DirectFlowThread(raw); });

                    engine->FilterLog(domain::LogLevel::Debug,
                        "[wintun][flow] DIRECT pid=" + std::to_string(flowPid)
                        + " proc=" + WideToUtf8(flowProc)
                        + " src=" + meta.source_ip + ":" + std::to_string(meta.source_port)
                        + " dst=" + meta.original_dst_ip + ":"
                        + std::to_string(meta.original_dst_port)
                        + " egress_ifindex=" + std::to_string(ifIndex));

                    if (engine->m_on_flow) {
                        try { engine->m_on_flow(meta); }
                        catch (...) {}
                    }
                    return ERR_OK;
                }
            }
            // Если сюда дошли — это proxy-fallback: продолжаем в PROXY-ветку ниже.
        }

        // Сюда доходит PROXY (или DIRECT с proxy-fallback) → форвард на локальный
        // relay 127.0.0.1:relay_port.
        // Guard корректности Option 2a: dst этого flow'а идёт через ПРОКСИ — он
        // НЕ должен иметь DIRECT /32 bypass (иначе трафик проксируемого приложения
        // ушёл бы мимо туннеля).  Регистрируем dst как proxy-used и снимаем bypass.
        engine->NoteProxyDestination(
            ip4_addr_get_u32(ip_2_ip4(&newpcb->local_ip)));

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(engine->m_relay_port);
        inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            tcp_abort(newpcb);
            return ERR_ABRT;
        }

        // === КРИТИЧНО (форвардинг через прокси): регистрация original-dst ===
        // Relay восстанавливает адрес назначения ТОЛЬКО через
        // ConnectionTable::Get(client_port), где client_port — эфемерный порт
        // ЭТОГО loopback-сокета (в отличие от WinDivert, сохраняющего порт
        // приложения).  Поэтому ДО connect() биндим сокет к 127.0.0.1:0, узнаём
        // назначенный порт и кладём (port -> original-dst) в таблицу.  Запись
        // ДО connect() исключает гонку «relay принял соединение раньше, чем мы
        // успели добавить запись».  Без неё relay пишет "No connection record
        // for port X" и закрывает соединение — трафик виден, но не форвардится.
        uint16_t relay_src_port = 0;
        if (engine->m_conn_table != nullptr) {
            const uint32_t orig_dst_be =
                ip4_addr_get_u32(ip_2_ip4(&newpcb->local_ip)); // network byte order
            sockaddr_in la{};
            la.sin_family = AF_INET;
            la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            la.sin_port = 0; // OS назначит эфемерный порт
            if (::bind(s, reinterpret_cast<sockaddr*>(&la), sizeof(la)) == 0) {
                sockaddr_in bound{};
                int blen = sizeof(bound);
                if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
                    relay_src_port = ntohs(bound.sin_port);
                    // proxy_config_id=1 — как в ServiceMain/TcpRelayServer
                    // (SetProxyConfig(cfg, 1)); в single-proxy дизайне на выбор
                    // прокси не влияет, держим консистентным с остальными путями.
                    engine->m_conn_table->Add(relay_src_port,
                                              htonl(INADDR_LOOPBACK),
                                              orig_dst_be,
                                              meta.original_dst_port,
                                              /*proxy_config_id=*/1u);
                }
            }
        }

        if (::connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) {
            const int wsaErr = ::WSAGetLastError();
            if (relay_src_port != 0 && engine->m_conn_table != nullptr) {
                engine->m_conn_table->Remove(relay_src_port);
            }
            ::closesocket(s);
            tcp_abort(newpcb);
            // Ранее этот сбой был молчаливым («ошибок в логах нет», хотя flow
            // не форвардился).  Теперь явно логируем на WARN.
            engine->FilterLog(domain::LogLevel::Warn,
                "[wintun][flow] connect to relay 127.0.0.1:"
                + std::to_string(engine->m_relay_port) + " FAILED (WSA="
                + std::to_string(wsaErr) + ") for dst "
                + meta.original_dst_ip + ":" + std::to_string(meta.original_dst_port)
                + " — flow dropped");
            return ERR_ABRT;
        }
        BOOL nodelay = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        auto flow = std::make_unique<Tun2SocksEngineEmbedded::Flow>();
        flow->pcb   = newpcb;
        flow->sock  = s;
        flow->owner = engine;
        flow->meta  = meta;
        flow->relay_src_port = relay_src_port;

        // Задача 4: регистрируем соединение в трекере для GUI-трассировки.
        if (engine->m_conn_monitor) {
            domain::ConnectionRecord rec;
            rec.id = engine->m_next_conn_id.fetch_add(1, std::memory_order_relaxed);
            rec.pid = flowPid;
            rec.process_name = flowProc;
            rec.destination_ip = meta.original_dst_ip;
            rec.destination_port = meta.original_dst_port;
            rec.state = domain::ConnectionState::TunnelEstablished;
            rec.start_time = std::chrono::steady_clock::now();
            // Сюда доходит PROXY или DIRECT-с-proxy-fallback — в обоих случаях
            // flow фактически идёт через прокси.
            rec.proxy_enabled = true;
            flow->conn_id = rec.id;
            engine->m_conn_monitor->AddConnection(rec);
        }

        tcp_arg (newpcb, flow.get());
        tcp_recv(newpcb, &EngineTramp::OnRecv);
        tcp_sent(newpcb, &EngineTramp::OnSent);
        tcp_err (newpcb, &EngineTramp::OnErr);

        Tun2SocksEngineEmbedded::Flow* raw = flow.get();
        // УНИКАЛЬНЫЙ id — ключ в m_flows (НЕ pcb-указатель, чтобы исключить
        // коллизию при переиспользовании pcb lwIP'ом → UAF).
        const uint64_t flow_id =
            engine->m_next_flow_id.fetch_add(1, std::memory_order_relaxed);
        flow->id = flow_id;
        {
            std::lock_guard<std::mutex> lk(engine->m_flows_mu);
            engine->m_flows.emplace(flow_id, std::move(flow));
        }
        engine->m_active_flows.fetch_add(1, std::memory_order_relaxed);

        raw->sock_reader = std::thread([engine, raw]() { engine->FlowSocketReader(raw); });

        // T5: явная DEBUG-строка на каждый установленный PROXY-flow — «кто/куда
        // подключается» (процесс, dst, эфемерный relay-порт для восстановления
        // original-dst).  Помогает диагностировать «трафик виден, но не идёт».
        engine->FilterLog(domain::LogLevel::Debug,
            "[wintun][flow] PROXY pid=" + std::to_string(flowPid)
            + " proc=" + WideToUtf8(flowProc)
            + " src=" + meta.source_ip + ":" + std::to_string(meta.source_port)
            + " dst=" + meta.original_dst_ip + ":" + std::to_string(meta.original_dst_port)
            + " relay_port=" + std::to_string(engine->m_relay_port)
            + " relay_src_port=" + std::to_string(relay_src_port)
            + " connect=OK");

        if (engine->m_on_flow) {
            try { engine->m_on_flow(meta); }
            catch (...) { /* observer не должен ронять движок */ }
        }
        return ERR_OK;
    }

    /** tcp_recv — данные из туннеля.  arg = Flow*. */
    static err_t OnRecv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err) {
        auto* flow = static_cast<Tun2SocksEngineEmbedded::Flow*>(arg);
        if (err != ERR_OK) {
            if (p) pbuf_free(p);
            if (flow) flow->owner->CloseFlow(flow, /*from_engine_thread=*/true);
            return ERR_OK;
        }
        if (p == nullptr) {
            if (flow) ::shutdown(flow->sock, SD_SEND);
            return ERR_OK;
        }
        uint8_t buf[2048];
        u16_t offset = 0;
        while (offset < p->tot_len) {
            const u16_t chunk = std::min<u16_t>(
                static_cast<u16_t>(sizeof(buf)),
                static_cast<u16_t>(p->tot_len - offset));
            const u16_t got = pbuf_copy_partial(p, buf, chunk, offset);
            if (got == 0) break;
            int sent = ::send(flow->sock, reinterpret_cast<const char*>(buf), got, 0);
            if (sent <= 0) {
                pbuf_free(p);
                flow->owner->CloseFlow(flow, /*from_engine_thread=*/true);
                return ERR_ABRT;
            }
            offset += static_cast<u16_t>(sent);
        }
        tcp_recved(tpcb, p->tot_len);
        flow->owner->m_rx_bytes.fetch_add(p->tot_len, std::memory_order_relaxed);
        pbuf_free(p);
        return ERR_OK;
    }

    /** tcp_sent — подтверждение доставки. */
    static err_t OnSent(void* arg, struct tcp_pcb* /*tpcb*/, u16_t len) {
        auto* flow = static_cast<Tun2SocksEngineEmbedded::Flow*>(arg);
        if (flow) flow->pcb_in_flight.fetch_sub(len, std::memory_order_relaxed);
        return ERR_OK;
    }

    /** tcp_err — pcb больше не валиден. */
    static void OnErr(void* arg, err_t /*err*/) {
        auto* flow = static_cast<Tun2SocksEngineEmbedded::Flow*>(arg);
        if (!flow) return;
        flow->pcb_dead.store(true, std::memory_order_relaxed);
        flow->pcb = nullptr;
        if (flow->sock != INVALID_SOCKET) {
            ::shutdown(flow->sock, SD_BOTH);
        }
    }
};

/* ------------------------------------------------------------------------- */
/*  Socket-half reader                                                            */
/* ------------------------------------------------------------------------- */

void Tun2SocksEngineEmbedded::FlowSocketReader(Flow* flow) {
    // Ридер тупо блокирующе recv'ит на сокете, пишет в pcb через tcp_write.
    // Back-pressure — по tcp_sndbuf().
    char buf[4096];
    while (!flow->closing.load(std::memory_order_relaxed) &&
           !flow->pcb_dead.load(std::memory_order_relaxed)) {
        int n = ::recv(flow->sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            // EOF или ошибка — половина мертва.
            break;
        }
        // Пушим в pcb.  Может понадобиться несколько итераций, если снд-буф
        // не хочет всё сразу.
        int off = 0;
        while (off < n) {
            if (flow->pcb_dead.load(std::memory_order_relaxed)) goto done;
            std::lock_guard<std::recursive_mutex> lk(m_core_lock);
            if (flow->pcb_dead.load(std::memory_order_relaxed) || flow->pcb == nullptr) goto done;
            const u16_t space = tcp_sndbuf(flow->pcb);
            if (space == 0) {
                // pcb полон — sleep и повторим.  10 мс — компромисс между CPU
                // и латентностью.
                Sleep(10);
                continue;
            }
            const u16_t chunk = static_cast<u16_t>(std::min<int>(n - off, space));
            const err_t e = tcp_write(flow->pcb, buf + off, chunk, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) {
                tcp_output(flow->pcb);
                flow->pcb_in_flight.fetch_add(chunk, std::memory_order_relaxed);
                off += chunk;
            } else if (e == ERR_MEM) {
                // Нет памяти — sleep и повторим.
                Sleep(10);
            } else {
                goto done;
            }
        }
    }
done:
    // Половинное закрытие — говорим lwIP «нам больше писать нечего», FIN
    // уедет в туннель.  Полное закрытие делает CloseFlow (из engine-треда).
    {
        std::lock_guard<std::recursive_mutex> lk(m_core_lock);
        if (!flow->pcb_dead.load(std::memory_order_relaxed) && flow->pcb != nullptr) {
            tcp_shutdown(flow->pcb, 0, 1); // shut_tx=1
        }
    }
    // Ридер завершился (relay/прокси закрыли соединение).  НЕ удаляем flow из
    // map'а и НЕ вызываем CloseFlow здесь (нельзя join'ить самого себя и нельзя
    // трогать lwIP вне engine-треда безопасно для tcp_close).  Вместо этого ставим
    // флаг reader_exited — engine-тред (ReapFinishedReaders) полностью закроет
    // flow и отдаст его reaper'у на уничтожение.  Это устраняет утечку half-open
    // flow'ов (pcb/сокеты/треды) — главную причину исчерпания пулов и краша.
    flow->reader_exited.store(true, std::memory_order_release);
}

/* ------------------------------------------------------------------------- */
/*  Задача 3: DIRECT-passthrough (Option 1 + Option 2a)                           */
/* ------------------------------------------------------------------------- */

bool Tun2SocksEngineEmbedded::ResolvePhysicalEgress(uint32_t dst_be,
                                                    uint32_t& out_if_index,
                                                    uint32_t& out_src_be) {
    out_if_index = 0;
    out_src_be = 0;

    // GetBestRoute2 — быстрый lookup таблицы маршрутов ОС (не сетевой I/O).
    // ВАЖНО: он вернёт лучший маршрут к dst С УЧЁТОМ наших split-tunnel /1-
    // маршрутов, то есть может указать на TUN.  Нам нужен ФИЗИЧЕСКИЙ путь.
    // Поэтому если ручной override не задан — мы также умеем предпочесть
    // не-TUN интерфейс: наш netif не является системным адаптером с LUID в
    // таблице маршрутов Windows (lwIP-netif живёт в user space), поэтому
    // GetBestRoute2 в embedded-режиме уже возвращает физический адаптер —
    // split-tunnel /1 стоят на Wintun-адаптере (реальный NDIS), но source-IP
    // для него — TUN-подсеть; чтобы гарантированно уйти на физику, мы задаём
    // IP_UNICAST_IF по физическому ifIndex.  Здесь резолвим физический путь.

    // Ручной override интерфейса (имя/ifIndex/IP).
    if (!m_direct.egress_interface.empty()) {
        const std::string& s = m_direct.egress_interface;
        // 1) числовой ifIndex?
        bool numeric = !s.empty();
        for (char c : s) { if (c < '0' || c > '9') { numeric = false; break; } }
        if (numeric) {
            out_if_index = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
        } else {
            // 2) IPv4-адрес интерфейса?
            IN_ADDR ia{};
            if (inet_pton(AF_INET, s.c_str(), &ia) == 1) {
                out_src_be = ia.S_un.S_addr;
                // Найдём ifIndex по source-IP через GetBestRoute2 (source=ia).
            }
            // 3) имя интерфейса — конвертируем через if_nametoindex.
            else {
                NET_LUID luid{};
                std::wstring w(s.begin(), s.end());
                if (ConvertInterfaceAliasToLuid(w.c_str(), &luid) == NO_ERROR) {
                    NET_IFINDEX idx = 0;
                    if (ConvertInterfaceLuidToIndex(&luid, &idx) == NO_ERROR) {
                        out_if_index = idx;
                    }
                }
            }
        }
    }

    // Всегда прогоняем GetBestRoute2 для получения ifIndex/source, если чего-то
    // не хватает.  Это даёт корректный source-IP физического интерфейса.
    if (out_if_index == 0 || out_src_be == 0) {
        SOCKADDR_INET dst{};
        dst.si_family = AF_INET;
        dst.Ipv4.sin_family = AF_INET;
        dst.Ipv4.sin_addr.S_un.S_addr = dst_be;

        MIB_IPFORWARD_ROW2 best{};
        SOCKADDR_INET bestSrc{};
        DWORD rc = GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &best, &bestSrc);
        if (rc != NO_ERROR) {
            return false;
        }
        if (out_if_index == 0) out_if_index = best.InterfaceIndex;
        if (out_src_be == 0 && bestSrc.si_family == AF_INET) {
            out_src_be = bestSrc.Ipv4.sin_addr.S_un.S_addr;
        }
    }

    return out_if_index != 0;
}

bool Tun2SocksEngineEmbedded::PinSocketToPhysical(SOCKET s,
                                                  uint32_t if_index,
                                                  uint32_t src_be) {
    bool pinned = false;

    // IP_UNICAST_IF — переопределяет egress-интерфейс сокета, минуя таблицу
    // маршрутов (наши /1 в TUN больше не «притягивают» этот сокет).  КРИТИЧНО:
    // значение — ifIndex в NETWORK byte order (в отличие от IPV6_UNICAST_IF!).
    if (if_index != 0) {
        DWORD ifIndexNbo = htonl(if_index);
        if (::setsockopt(s, IPPROTO_IP, IP_UNICAST_IF,
                         reinterpret_cast<const char*>(&ifIndexNbo),
                         sizeof(ifIndexNbo)) == 0) {
            pinned = true;
        }
    }

    // Source-IP bind как «belt-and-suspenders» / fallback: если IP_UNICAST_IF
    // не сработал, bind к физическому source-IP помогает LPM выбрать физику.
    if (src_be != 0) {
        sockaddr_in la{};
        la.sin_family = AF_INET;
        la.sin_addr.s_addr = src_be;
        la.sin_port = 0;
        if (::bind(s, reinterpret_cast<sockaddr*>(&la), sizeof(la)) == 0) {
            pinned = true;
        }
    }

    return pinned;
}

void Tun2SocksEngineEmbedded::NoteProxyDestination(uint32_t dst_be) {
    if (!m_direct.enabled || !m_direct.route_optimization) return;
    bool hadBypass = false;
    {
        std::lock_guard<std::mutex> lk(m_bypass_mu);
        m_proxy_dsts.insert(dst_be);
        hadBypass = (m_bypass_refcount.find(dst_be) != m_bypass_refcount.end());
    }
    // Если для этого dst ранее стоял DIRECT /32 bypass — снимаем немедленно:
    // проксируемое приложение НЕ должно уходить мимо туннеля (guard).
    if (hadBypass) {
        std::string err;
        (void)RouteInstaller::UninstallHostBypass(dst_be, &err);
        std::lock_guard<std::mutex> lk(m_bypass_mu);
        m_bypass_refcount.erase(dst_be);
        FilterLog(domain::LogLevel::Debug,
            "[wintun][bypass] removed DIRECT /32 bypass — dst now used by PROXY");
    }
}

void Tun2SocksEngineEmbedded::MaybeInstallDirectBypass(uint32_t dst_be) {
    if (!m_direct.enabled || !m_direct.route_optimization) return;
    // Loopback не нуждается в bypass (InstallHostBypass сам это отсекает).
    if (static_cast<uint8_t>(dst_be & 0xFF) == 127) return;

    bool doInstall = false;
    {
        std::lock_guard<std::mutex> lk(m_bypass_mu);
        // Guard: dst используется PROXY → НЕ ставим (иначе сломаем проксирование).
        if (m_proxy_dsts.find(dst_be) != m_proxy_dsts.end()) return;
        auto it = m_bypass_refcount.find(dst_be);
        if (it == m_bypass_refcount.end()) {
            m_bypass_refcount.emplace(dst_be, 1u);
            doInstall = true;
        } else {
            ++(it->second); // уже стоит — только refcount++
        }
    }
    if (doInstall) {
        std::string err;
        if (RouteInstaller::InstallHostBypass(dst_be, &err)) {
            FilterLog(domain::LogLevel::Debug,
                "[wintun][bypass] installed DIRECT /32 bypass for established dst");
        } else {
            // Не удалось — откатываем refcount, работаем без оптимизации (Option 1
            // software-роутинг остаётся корректным).
            std::lock_guard<std::mutex> lk(m_bypass_mu);
            m_bypass_refcount.erase(dst_be);
            FilterLog(domain::LogLevel::Debug,
                "[wintun][bypass] InstallHostBypass failed (non-fatal): " + err);
        }
    }
}

void Tun2SocksEngineEmbedded::ReleaseDirectBypass(uint32_t dst_be) {
    bool doRemove = false;
    {
        std::lock_guard<std::mutex> lk(m_bypass_mu);
        auto it = m_bypass_refcount.find(dst_be);
        if (it == m_bypass_refcount.end()) return;
        if (--(it->second) == 0) {
            m_bypass_refcount.erase(it);
            doRemove = true;
        }
    }
    if (doRemove) {
        std::string err;
        (void)RouteInstaller::UninstallHostBypass(dst_be, &err);
    }
}

void Tun2SocksEngineEmbedded::RemoveAllDirectBypasses() {
    std::vector<uint32_t> dsts;
    {
        std::lock_guard<std::mutex> lk(m_bypass_mu);
        dsts.reserve(m_bypass_refcount.size());
        for (const auto& kv : m_bypass_refcount) dsts.push_back(kv.first);
        m_bypass_refcount.clear();
        m_proxy_dsts.clear();
    }
    for (uint32_t d : dsts) {
        std::string err;
        (void)RouteInstaller::UninstallHostBypass(d, &err);
    }
    if (!dsts.empty()) {
        FilterLog(domain::LogLevel::Debug,
            "[wintun][bypass] removed " + std::to_string(dsts.size())
            + " DIRECT /32 bypass route(s) on shutdown");
    }
}

void Tun2SocksEngineEmbedded::DirectFlowThread(Flow* flow) {
    // Выполняется НА per-flow треде (НЕ на engine-треде): здесь можно блокирующе
    // connect'иться и recv'иться — engine-тред не затрагивается (инвариант №1).
    // Сокет уже создан и запиннут к физическому NIC в OnAccept (инвариант №2).

    // 1) connect к реальному dst.  Делаем НЕблокирующим с таймаутом через select,
    //    чтобы поток не завис навечно на неответчивом хосте.
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = flow->direct_dst_be;
    target.sin_port = htons(flow->meta.original_dst_port);

    u_long nb = 1;
    ::ioctlsocket(flow->sock, FIONBIO, &nb);
    int cr = ::connect(flow->sock, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    bool connected = (cr == 0);
    if (!connected) {
        const int e = ::WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            fd_set wfds, efds;
            FD_ZERO(&wfds); FD_ZERO(&efds);
            FD_SET(flow->sock, &wfds); FD_SET(flow->sock, &efds);
            timeval tv{}; tv.tv_sec = 10; tv.tv_usec = 0; // 10 c на connect
            int sr = ::select(0, nullptr, &wfds, &efds, &tv);
            if (sr > 0 && FD_ISSET(flow->sock, &wfds)) {
                int soErr = 0; int len = sizeof(soErr);
                if (::getsockopt(flow->sock, SOL_SOCKET, SO_ERROR,
                                 reinterpret_cast<char*>(&soErr), &len) == 0
                    && soErr == 0) {
                    connected = true;
                }
            }
        }
    }
    // Возвращаем сокет в блокирующий режим для recv-качалки.
    nb = 0;
    ::ioctlsocket(flow->sock, FIONBIO, &nb);

    if (!connected || flow->closing.load(std::memory_order_relaxed)) {
        // connect провалился — просим engine-тред закрыть flow (FIN/RST уедет в TUN,
        // приложение мгновенно получит отказ).  reader_exited → ReapFinishedReaders.
        flow->reader_exited.store(true, std::memory_order_release);
        FilterLog(domain::LogLevel::Debug,
            "[wintun][flow] DIRECT connect FAILED -> " + flow->meta.original_dst_ip
            + ":" + std::to_string(flow->meta.original_dst_port));
        return;
    }

    // 2) Option 2a: соединение установлено — ставим <dst>/32 bypass (если dst не
    //    проксируется).  Отмечаем на flow, чтобы CloseFlow декрементировал refcount.
    MaybeInstallDirectBypass(flow->direct_dst_be);
    {
        std::lock_guard<std::mutex> lk(m_bypass_mu);
        if (m_bypass_refcount.find(flow->direct_dst_be) != m_bypass_refcount.end()) {
            flow->direct_bypass_installed = true;
        }
    }

    // 3) Двунаправленная качалка (socket→pcb).  Идентична PROXY-half —
    //    переиспользуем ту же схему back-pressure по tcp_sndbuf.
    char buf[4096];
    while (!flow->closing.load(std::memory_order_relaxed) &&
           !flow->pcb_dead.load(std::memory_order_relaxed)) {
        int n = ::recv(flow->sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        int off = 0;
        while (off < n) {
            if (flow->pcb_dead.load(std::memory_order_relaxed)) goto ddone;
            std::lock_guard<std::recursive_mutex> lk(m_core_lock);
            if (flow->pcb_dead.load(std::memory_order_relaxed) || flow->pcb == nullptr) goto ddone;
            const u16_t space = tcp_sndbuf(flow->pcb);
            if (space == 0) { Sleep(10); continue; }
            const u16_t chunk = static_cast<u16_t>(std::min<int>(n - off, space));
            const err_t e = tcp_write(flow->pcb, buf + off, chunk, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) {
                tcp_output(flow->pcb);
                flow->pcb_in_flight.fetch_add(chunk, std::memory_order_relaxed);
                off += chunk;
            } else if (e == ERR_MEM) {
                Sleep(10);
            } else {
                goto ddone;
            }
        }
    }
ddone:
    {
        std::lock_guard<std::recursive_mutex> lk(m_core_lock);
        if (!flow->pcb_dead.load(std::memory_order_relaxed) && flow->pcb != nullptr) {
            tcp_shutdown(flow->pcb, 0, 1); // shut_tx=1 — FIN в туннель
        }
    }
    flow->reader_exited.store(true, std::memory_order_release);
}

/* ------------------------------------------------------------------------- */
/*  CloseFlow                                                                     */
/* ------------------------------------------------------------------------- */

void Tun2SocksEngineEmbedded::CloseFlow(Flow* flow, bool from_engine_thread) {
    if (!flow) return;
    if (flow->closing.exchange(true, std::memory_order_relaxed)) return;

    // Задача 4: снимаем запись из трекера (GUI-трассировка).
    if (m_conn_monitor && flow->conn_id != 0) {
        m_conn_monitor->RemoveConnection(flow->conn_id);
        flow->conn_id = 0;
    }

    // Снимаем запись original-dst из ConnectionTable (создавалась для PROXY в
    // OnAccept).  К этому моменту relay давно прочитал её при accept'е, поэтому
    // удаление безопасно и предотвращает неограниченный рост таблицы.
    if (m_conn_table && flow->relay_src_port != 0) {
        m_conn_table->Remove(flow->relay_src_port);
        flow->relay_src_port = 0;
    }

    // Задача 3 (Option 2a): для DIRECT-flow'а декрементируем refcount /32-bypass
    // маршрута; при обнулении маршрут снимается (устраняет утечку stale-route).
    if (flow->is_direct && flow->direct_bypass_installed && flow->direct_dst_be != 0) {
        ReleaseDirectBypass(flow->direct_dst_be);
        flow->direct_bypass_installed = false;
    }

    // Закрываем сокет — сокет-ридер вывалится.
    if (flow->sock != INVALID_SOCKET) {
        ::shutdown(flow->sock, SD_BOTH);
        ::closesocket(flow->sock);
        flow->sock = INVALID_SOCKET;
    }

    // Закрываем pcb под lock'ом (если ещё жив).  ВАЖНО: сначала снимаем ВСЕ
    // lwIP-callback'и и tcp_arg=nullptr, только потом tcp_close — так после
    // закрытия ни один входящий сегмент/ошибка не вызовет callback с указателем
    // на этот Flow (он вот-вот будет уничтожен reaper'ом).  Это ключ к
    // устранению use-after-free при переиспользовании pcb из пула lwIP.
    {
        // recursive_mutex: безопасно и из engine-треда (уже под core-lock через
        // nf->input), и из Stop() (отдельный тред, но engine уже остановлен).
        std::lock_guard<std::recursive_mutex> lk(m_core_lock);
        if (!flow->pcb_dead.load(std::memory_order_relaxed) && flow->pcb) {
            tcp_arg (flow->pcb, nullptr);
            tcp_recv(flow->pcb, nullptr);
            tcp_sent(flow->pcb, nullptr);
            tcp_err (flow->pcb, nullptr);
            tcp_close(flow->pcb);
            flow->pcb = nullptr;
        }
    }

    // Утилизация Flow.
    //  • from_engine_thread=true (штатный путь: OnRecv/OnErr/ReapFinishedReaders):
    //    извлекаем владеющий unique_ptr из m_flows по flow->id, декрементируем
    //    active_flows и передаём Flow reaper'у, который join'ит socket-ридер и
    //    уничтожит объект ВНЕ engine-треда (нельзя join'ить ридер из engine-
    //    треда под core-lock: ридер может ждать этот же lock → дедлок).
    //  • from_engine_thread=false (путь Stop()): владение Flow уже вынесено в
    //    Stop() (см. to_drop), поэтому здесь НЕ трогаем map/active_flows/reaper —
    //    Stop() сам join'ит ридер и уничтожит объект.
    if (from_engine_thread) {
        RetireFlow(flow);
    }
}

/* ------------------------------------------------------------------------- */
/*  RetireFlow / Reaper — безопасное уничтожение закрытых flow'ов                 */
/* ------------------------------------------------------------------------- */

void Tun2SocksEngineEmbedded::RetireFlow(Flow* flow) {
    // Извлекаем владеющий unique_ptr из m_flows по уникальному id.
    std::unique_ptr<Flow> owned;
    {
        std::lock_guard<std::mutex> lk(m_flows_mu);
        auto it = m_flows.find(flow->id);
        if (it == m_flows.end()) {
            // Уже извлечён (двойной retire не должен случаться — closing
            // защищает — но на всякий случай не падаем).
            return;
        }
        owned = std::move(it->second);
        m_flows.erase(it);
    }
    m_active_flows.fetch_sub(1, std::memory_order_relaxed);

    // Передаём reaper'у: он join'ит socket-ридер (тот уже разбужен закрытием
    // сокета выше) и уничтожит Flow.  Если reaper не запущен (теоретически —
    // при гонке со Stop), уничтожаем прямо здесь после detach ридера, чтобы не
    // блокировать engine-тред.
    {
        std::lock_guard<std::mutex> lk(m_retire_mu);
        if (m_reaper_run.load(std::memory_order_relaxed)) {
            m_retire.push_back(std::move(owned));
            m_retire_cv.notify_one();
            return;
        }
    }
    // Fallback без reaper'а: detach ридер, объект уничтожится по выходу owned.
    if (owned && owned->sock_reader.joinable()) {
        owned->sock_reader.detach();
    }
}

void Tun2SocksEngineEmbedded::ReaperThreadMain() {
    for (;;) {
        std::vector<std::unique_ptr<Flow>> batch;
        {
            std::unique_lock<std::mutex> lk(m_retire_mu);
            m_retire_cv.wait(lk, [this] {
                return !m_retire.empty()
                    || !m_reaper_run.load(std::memory_order_relaxed);
            });
            batch.swap(m_retire);
            if (batch.empty() && !m_reaper_run.load(std::memory_order_relaxed)) {
                return; // остановка и очередь пуста — выходим
            }
        }
        for (auto& f : batch) {
            if (f && f->sock_reader.joinable()) {
                f->sock_reader.join(); // ридер уже вышел (сокет закрыт) — быстро
            }
            // ~Flow() здесь: сокет уже закрыт в CloseFlow, pcb обнулён,
            // callback'и сняты — уничтожение безопасно.
        }
    }
}

void Tun2SocksEngineEmbedded::ReapFinishedReaders() {
    // Вызывается ИЗ engine-треда.  Закрывает flow'ы, чей socket-ридер уже вышел
    // (relay/прокси закрыли соединение), но со стороны туннеля FIN так и не
    // пришёл (иначе OnRecv уже вызвал бы CloseFlow).  Без этого такие flow'ы
    // жили бы вечно (утечка pcb/сокетов/тредов).
    std::vector<Flow*> to_close;
    {
        std::lock_guard<std::mutex> lk(m_flows_mu);
        for (auto& kv : m_flows) {
            Flow* f = kv.second.get();
            if (f->reader_exited.load(std::memory_order_acquire)
                && !f->closing.load(std::memory_order_relaxed)) {
                to_close.push_back(f);
            }
        }
    }
    // CloseFlow извлекает владение из map'а — вызываем ВНЕ блокировки m_flows_mu
    // (RetireFlow сам берёт m_flows_mu).  from_engine_thread=true: мы в engine-
    // треде.
    for (Flow* f : to_close) {
        CloseFlow(f, /*from_engine_thread=*/true);
    }
}

/* ------------------------------------------------------------------------- */
/*  Задача 2: фильтрация по процессу                                             */
/* ------------------------------------------------------------------------- */

void Tun2SocksEngineEmbedded::FilterLog(domain::LogLevel level,
                                        const std::string& msg) const {
    if (!m_filter.log) return;
    m_filter.log->Log(level, "wintun", msg);
}

Tun2SocksEngineEmbedded::FlowDecision
Tun2SocksEngineEmbedded::DecideFlow(const FlowMeta& meta,
                                    uint32_t& out_pid,
                                    std::wstring& out_proc_name) {
    out_pid = 0;
    out_proc_name.clear();

    // Фильтрация выключена или нет движка правил → поведение Option 2b:
    // весь трафик идёт через прокси (relay).
    if (!m_filter.enabled || m_filter.rule_engine == nullptr) {
        return FlowDecision::Proxy;
    }

    // 1. PID по source-порту (source_port — host byte order).
    //    §6.9 плана: при промахе один ретрай через 20 мс — TCP-таблица ОС
    //    может отставать от только что установленного соединения.
    uint32_t pid = m_resolver.ResolvePidBySourcePort(meta.source_port);
    if (pid == 0) {
        Sleep(20);
        pid = m_resolver.ResolvePidBySourcePort(meta.source_port);
    }
    out_pid = pid;
    if (pid == 0) {
        // PID так и не найден.  В отличие от WinDivert (где решение можно
        // пересмотреть на следующем пакете), здесь решение принимается один
        // раз на SYN.  Дефолт — DIRECT: не гоним неизвестный трафик в прокси.
        return FlowDecision::Direct;
    }

    // 2. Собственный процесс — DIRECT (защита от петли).
    if (pid == GetCurrentProcessId()) {
        return FlowDecision::Direct;
    }

    // 3. Путь процесса.
    const std::wstring procPath = process::ProcessResolver::GetProcessPath(pid);
    if (procPath.empty()) {
        return FlowDecision::Direct;
    }
    const std::wstring procName = process::ProcessResolver::ShortName(procPath);
    out_proc_name = procName;

    // 4. Port-aware матчинг (v2 apps[]).
    const std::string procNameUtf8 = WideToUtf8(procName);
    const std::string procPathUtf8 = WideToUtf8(procPath);
    auto appMatch = m_filter.rule_engine->MatchForFlow(
        procNameUtf8, procPathUtf8, meta.original_dst_port);
    if (appMatch.matched) {
        if (!m_filter.proxy_configured) return FlowDecision::Direct;
        FilterLog(domain::LogLevel::Trace, "[wintun][PROXY] " + procNameUtf8
                  + " -> " + meta.original_dst_ip + ":"
                  + std::to_string(meta.original_dst_port));
        return FlowDecision::Proxy;
    }

    // 5. Legacy-матчинг по имени/пути процесса.
    auto action = m_filter.rule_engine->Match(procName, procPath);
    if (action == domain::RuleAction::Block) {
        FilterLog(domain::LogLevel::Debug, "[wintun][BLOCK] " + procNameUtf8);
        return FlowDecision::Block;
    }
    if (action == domain::RuleAction::Proxy) {
        if (!m_filter.proxy_configured) return FlowDecision::Direct;
        return FlowDecision::Proxy;
    }

    // 6. Fallback: target-process по пути + дерево процессов (helper/child).
    if (!m_filter.target_process_path.empty()) {
        // Прямое совпадение пути.
        if (_wcsicmp(procPath.c_str(), m_filter.target_process_path.c_str()) == 0) {
            m_target_pid.store(pid, std::memory_order_relaxed);
            return m_filter.proxy_configured ? FlowDecision::Proxy
                                             : FlowDecision::Direct;
        }
        // Резолвим target PID, если ещё не знаем.
        uint32_t tpid = m_target_pid.load(std::memory_order_relaxed);
        if (tpid == 0) {
            tpid = process::ProcessResolver::FindPidByImageName(
                m_filter.target_process_path);
            if (tpid != 0) m_target_pid.store(tpid, std::memory_order_relaxed);
        }
        // Дерево процессов вверх — ловим helper/child.
        if (tpid != 0 && process::ProcessResolver::IsDescendantOf(pid, tpid)) {
            return m_filter.proxy_configured ? FlowDecision::Proxy
                                             : FlowDecision::Direct;
        }
    }

    // 7. Ничего не сматчилось — DIRECT (прямой outbound, минуя прокси).
    return FlowDecision::Direct;
}

/* ------------------------------------------------------------------------- */
/*  Engine thread                                                                 */
/* ------------------------------------------------------------------------- */

void Tun2SocksEngineEmbedded::EngineThreadMain() {
    // Таймер lwIP: sys_check_timeouts надо звать регулярно, шаг 250 мс — с запасом.
    HANDLE timer = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due{};
    due.QuadPart = -2500000; // 250 мс в единицах 100 нс, negative = relative
    ::SetWaitableTimer(timer, &due, 250, nullptr, nullptr, FALSE);

    HANDLE waits[3] = { m_session->ReadWaitEvent(), m_stop_event, timer };
    std::vector<uint8_t> rxbuf;
    rxbuf.reserve(2048);

    // T5: троттлинг периодической TRACE-сводки счётчиков пакетов.
    ULONGLONG lastStatsTick = ::GetTickCount64();
    uint64_t  lastV4 = 0, lastV6 = 0;

    while (m_running.load(std::memory_order_relaxed)) {
        // Дрейним всё, что есть в wintun'е, ДО следующего wait'а — иначе
        // event сбрасывается, а пакеты остаются.
        //
        // ВАЖНО (исправление «зависаний/таймаутов» под нагрузкой): здесь
        // используется НЕблокирующий TryReceiveInto().  Раньше вызывался
        // ReceiveInto(), который при пустом ring'е блокировался на event'е
        // (INFINITE) и возвращал <=0 только на stop — из-за этого внутренний
        // цикл фактически никогда не выходил в штатном трафике, и
        // sys_check_timeouts()/внешний WaitForMultipleObjects были мёртвым
        // кодом.  lwIP-таймеры (ретрансмиссии, delayed-ACK) не срабатывали →
        // соединения зависали.  Теперь дренаж выходит по пустому ring'у (0),
        // после чего гарантированно вызывается sys_check_timeouts() и
        // выполняется ожидание с таймаутом.
        for (;;) {
            std::string ignored_err;
            int n = m_session->TryReceiveInto(rxbuf, &ignored_err);
            if (n <= 0) break; // 0 — ring пуст; <0 — ошибка сессии

            // Версия IP — старший ниббл первого байта.  IPv6 (=6) lwIP-стек не
            // обрабатывает (IPv4-only): при block_ipv6 отвечаем RST на TCP-SYN
            // (мгновенный откат приложения на IPv4), прочий IPv6 дропаем.  Без
            // block_ipv6 — пропускаем в lwIP (там он молча отбросится).
            if (n >= 1 && (rxbuf[0] >> 4) == 6) {
                m_pkts_v6.fetch_add(1, std::memory_order_relaxed);
                if (m_block_ipv6) {
                    HandleIpv6Packet(rxbuf.data(), n);
                    continue; // не отдаём в lwIP
                }
                // block_ipv6=false — падаем ниже, lwIP отбросит IPv6 сам.
            } else {
                m_pkts_v4.fetch_add(1, std::memory_order_relaxed);
            }

            std::lock_guard<std::recursive_mutex> lk(m_core_lock);
            struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(n), PBUF_POOL);
            if (!p) continue; // pool пуст — дропаем пакет (TCP пересчитается)
            pbuf_take(p, rxbuf.data(), static_cast<u16_t>(n));
            auto* nf = static_cast<struct netif*>(m_netif);
            if (nf->input(p, nf) != ERR_OK) {
                pbuf_free(p);
            }
        }
        // Дёрнем таймеры (мы могли долго крутиться в дрейне).
        {
            std::lock_guard<std::recursive_mutex> lk(m_core_lock);
            sys_check_timeouts();
        }

        // Закрываем flow'ы, чей socket-ридер уже вышел (relay/прокси закрыли
        // соединение), но FIN со стороны туннеля ещё не пришёл.  Выполняется СТРОГО
        // в engine-треде (все tcp_* — только отсюда).  Устраняет утечку half-open
        // flow'ов (pcb/сокеты/треды) — корневую причину исчерпания пулов lwIP
        // и аварийного завершения службы под нагрузкой.
        ReapFinishedReaders();

        // T5: периодическая (раз в ~5 c) TRACE-сводка о трафике из туннеля —
        // видно, доходят ли пакеты и в каком соотношении IPv4/IPv6.  Логируем
        // только при изменении, чтобы не спамить в простое.
        if (m_filter.log) {
            const ULONGLONG now = ::GetTickCount64();
            if (now - lastStatsTick >= 5000) {
                const uint64_t v4 = m_pkts_v4.load(std::memory_order_relaxed);
                const uint64_t v6 = m_pkts_v6.load(std::memory_order_relaxed);
                if (v4 != lastV4 || v6 != lastV6) {
                    std::ostringstream os;
                    os << "[wintun][rx-stats] tun packets: ipv4=" << v4
                       << " ipv6=" << v6
                       << " (ipv6_rst=" << m_ipv6_rst.load(std::memory_order_relaxed)
                       << " ipv6_dropped=" << m_ipv6_dropped.load(std::memory_order_relaxed)
                       << ") active_flows=" << m_active_flows.load(std::memory_order_relaxed);
                    // Понижено Trace→Debug (задача №3): сводка «доходят ли
                    // пакеты до TUN и в каком соотношении IPv4/IPv6» — ключ к
                    // диагностике «трафик не выходит», должна быть видна на DEBUG.
                    FilterLog(domain::LogLevel::Debug, os.str());
                    lastV4 = v4;
                    lastV6 = v6;
                }
                lastStatsTick = now;
            }
        }
        // Ждём событие: пакет из туннеля, stop или периодический timer (250 мс)
        // для регулярного sys_check_timeouts().
        DWORD r = ::WaitForMultipleObjects(3, waits, FALSE, 500);
        if (r == WAIT_OBJECT_0 + 1) break; // stop
        // остальные — timer или сессия; идём на новый круг.
    }

    ::CancelWaitableTimer(timer);
    ::CloseHandle(timer);
}

/* ------------------------------------------------------------------------- */
/*  IPv6-нейтрализация: RST на IPv6 TCP-SYN, дроп прочего IPv6                    */
/* ------------------------------------------------------------------------- */

namespace {

// 16-битная контрольная сумма поверх произвольного буфера (для TCP over IPv6
// с псевдозаголовком).  data/len — суммируемая область; возвращает свёрнутую
// сумму в host byte order (не инвертированную — инверсию делает вызывающий).
uint32_t Checksum16Accumulate(const uint8_t* data, size_t len, uint32_t seed) {
    uint32_t sum = seed;
    size_t i = 0;
    for (; i + 1 < len; i += 2) {
        sum += (static_cast<uint32_t>(data[i]) << 8) | data[i + 1];
    }
    if (i < len) {
        sum += static_cast<uint32_t>(data[i]) << 8; // нечётный последний байт
    }
    return sum;
}

uint16_t Fold16(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum & 0xFFFF);
}

} // namespace

bool Tun2SocksEngineEmbedded::HandleIpv6Packet(const uint8_t* pkt, int len) {
    // Разбор фиксированного IPv6-заголовка (40 байт).  Дробную поддержку
    // extension-headers не делаем — у TCP-SYN их на практике нет.
    if (len < 40) { m_ipv6_dropped.fetch_add(1, std::memory_order_relaxed); return true; }
    const uint8_t nextHeader = pkt[6];
    const uint8_t* srcAddr = pkt + 8;   // 16 байт
    const uint8_t* dstAddr = pkt + 24;  // 16 байт

    // Только TCP (6) обрабатываем RST'ом; прочее (UDP/ICMPv6/ext) — дроп.
    if (nextHeader != 6 /*IPPROTO_TCP*/) {
        m_ipv6_dropped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const int tcpOff = 40;
    if (len < tcpOff + 20) { m_ipv6_dropped.fetch_add(1, std::memory_order_relaxed); return true; }

    const uint8_t* tcp = pkt + tcpOff;
    const uint16_t srcPort = (static_cast<uint16_t>(tcp[0]) << 8) | tcp[1];
    const uint16_t dstPort = (static_cast<uint16_t>(tcp[2]) << 8) | tcp[3];
    const uint32_t seq = (static_cast<uint32_t>(tcp[4]) << 24) |
                         (static_cast<uint32_t>(tcp[5]) << 16) |
                         (static_cast<uint32_t>(tcp[6]) << 8)  |
                          static_cast<uint32_t>(tcp[7]);
    const uint8_t flags = tcp[13];
    const bool isSyn = (flags & 0x02) != 0;
    const bool isRst = (flags & 0x04) != 0;
    const bool isAck = (flags & 0x10) != 0;

    // RST шлём только на «чистый» SYN (не SYN-ACK, не RST).  Прочие TCP-сегменты
    // IPv6 (ретрансмиты и т.п.) — дроп, приложение и так упадёт по таймауту/RST.
    if (isRst || !isSyn || isAck) {
        m_ipv6_dropped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Формируем IPv6 TCP RST|ACK: swap src/dst адресов и портов, seq=0,
    // ack=incoming_seq+1.  40 (IPv6) + 20 (TCP без опций) = 60 байт.
    uint8_t out[60];
    std::memset(out, 0, sizeof(out));
    // --- IPv6 header ---
    out[0] = 0x60;                 // version=6, traffic class 0
    out[4] = 0x00; out[5] = 20;    // payload length = 20 (TCP header)
    out[6] = 6;                    // next header = TCP
    out[7] = 64;                   // hop limit
    std::memcpy(out + 8,  dstAddr, 16); // src = исходный dst
    std::memcpy(out + 24, srcAddr, 16); // dst = исходный src
    // --- TCP header ---
    uint8_t* rtcp = out + 40;
    rtcp[0] = static_cast<uint8_t>(dstPort >> 8); rtcp[1] = static_cast<uint8_t>(dstPort & 0xFF);
    rtcp[2] = static_cast<uint8_t>(srcPort >> 8); rtcp[3] = static_cast<uint8_t>(srcPort & 0xFF);
    // seq = 0 (уже занулено).  ack = seq+1.
    const uint32_t ackNum = seq + 1;
    rtcp[8]  = static_cast<uint8_t>((ackNum >> 24) & 0xFF);
    rtcp[9]  = static_cast<uint8_t>((ackNum >> 16) & 0xFF);
    rtcp[10] = static_cast<uint8_t>((ackNum >> 8) & 0xFF);
    rtcp[11] = static_cast<uint8_t>(ackNum & 0xFF);
    rtcp[12] = 0x50;               // data offset = 5 (20 байт), reserved 0
    rtcp[13] = 0x14;               // flags = RST|ACK
    // window=0, urgent=0.  Контрольная сумма — ниже.

    // TCP checksum с IPv6-псевдозаголовком (RFC 2460): src(16)+dst(16)+
    // upper-layer-length(4)+zeros(3)+next-header(1) + сам TCP-сегмент.
    uint8_t pseudo[40];
    std::memcpy(pseudo,      out + 8,  16); // src (RST src)
    std::memcpy(pseudo + 16, out + 24, 16); // dst (RST dst)
    pseudo[32] = 0; pseudo[33] = 0; pseudo[34] = 0; pseudo[35] = 20; // upper-layer len
    pseudo[36] = 0; pseudo[37] = 0; pseudo[38] = 0; pseudo[39] = 6;  // next header
    uint32_t sum = Checksum16Accumulate(pseudo, sizeof(pseudo), 0);
    sum = Checksum16Accumulate(rtcp, 20, sum);
    const uint16_t csum = Fold16(sum);
    rtcp[16] = static_cast<uint8_t>(csum >> 8);
    rtcp[17] = static_cast<uint8_t>(csum & 0xFF);

    // Отправляем RST обратно в туннель.
    if (m_session && m_session->Send(out, sizeof(out))) {
        m_ipv6_rst.fetch_add(1, std::memory_order_relaxed);
        if (m_filter.log) {
            std::ostringstream os;
            os << "[wintun][ipv6-rst] src_port=" << srcPort
               << " dst_port=" << dstPort
               << " -> RST|ACK sent (forcing IPv4 fallback)";
            FilterLog(domain::LogLevel::Trace, os.str());
        }
    } else {
        m_ipv6_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/*  Start / Stop                                                                  */
/* ------------------------------------------------------------------------- */

bool Tun2SocksEngineEmbedded::Start(std::string* outError) {
    if (m_running.exchange(true)) return true; // уже запущены
    if (!m_session) {
        if (outError) *outError = "Tun2SocksEngineEmbedded: session is null";
        m_running.store(false);
        return false;
    }
    // lwip_init — один раз на процесс.  См. §"NO_SYS notes" в .h.
    bool expected = false;
    if (s_lwip_inited.compare_exchange_strong(expected, true)) {
        lwip_init();
    }

    m_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop_event) {
        if (outError) *outError = "Tun2SocksEngineEmbedded: CreateEvent failed";
        m_running.store(false);
        return false;
    }

    // Adder netif.  IP берём как «gateway»-heuristic из CIDR туннеля; но, так
    // как WP9 не имеет доступа к WintunSettings напрямую (тот флоу — WP10),
    // используем нейтральный дефолт 0.0.0.0/0 + маску 0.  Хук
    // LWIP_HOOK_IP4_INPUT всё равно перепишет netif->ip_addr на dest каждого
    // входящего пакета — так что стартовое значение не влияет на приём.
    // Для исходящего IP-роутинга (netif->output → linkoutput) IP тоже не
    // важен: мы никогда не порождаем trafик «изнутри» — только эхо-отвечаем
    // на принятые пакеты, TCP-стек сам заполнит src/dst.
    auto* nf = new struct netif{};
    nf->state = this;
    ip4_addr_t any{}, mask{}, gw{};
    ip4_addr_set_zero(&any);
    ip4_addr_set_zero(&mask);
    ip4_addr_set_zero(&gw);
    struct netif* added = netif_add(nf, &any, &mask, &gw, this,
                                     &EngineTramp::NetifInit,
                                     netif_input);
    if (!added) {
        delete nf;
        ::CloseHandle(m_stop_event);
        m_stop_event = nullptr;
        m_running.store(false);
        if (outError) *outError = "Tun2SocksEngineEmbedded: netif_add failed";
        return false;
    }
    netif_set_default(nf);
    netif_set_up(nf);
    netif_set_link_up(nf);
    m_netif = nf;

    // Catch-all listener.  Bind на IP_ANY_TYPE, порт 0 — WILDCARD по порту.
    //
    // B14 (критично): в стоковом lwIP tcp_bind(pcb, IP_ANY_TYPE, 0) НЕ даёт
    // wildcard по порту — при port==0 вызывается tcp_new_port() и listener
    // получает КОНКРЕТНЫЙ эфемерный порт, а tcp_input матчит SYN к listener'у
    // только при lpcb->local_port == dst-порт.  Из-за этого ни один SYN
    // приложения (dst 9080/9300/443/…) не матчился, OnAccept не вызывался, и
    // трафик «входил в туннель, но не выходил на прокси» (active_flows=0).
    //
    // Хук IP4_INPUT (lwip_hooks_impl.c) решает только проверку IP-адреса
    // (переписывает netif->ip_addr на dst), но НЕ проверку порта.  Поэтому
    // ниже мы принудительно возвращаем local_port=0 ПОСЛЕ tcp_bind, а
    // project-owned патч в tcp_in.c (TCP_REDIRECTOR_WILDCARD_LISTEN, см.
    // lwipopts.h + ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md §B14) трактует listener с
    // local_port==0 как «любой порт» и подставляет реальный dst-порт в новый
    // pcb, чтобы OnAccept восстановил original-dst.
    struct tcp_pcb* pcb = tcp_new();
    if (!pcb) {
        netif_remove(nf);
        delete nf;
        m_netif = nullptr;
        ::CloseHandle(m_stop_event);
        m_stop_event = nullptr;
        m_running.store(false);
        if (outError) *outError = "Tun2SocksEngineEmbedded: tcp_new failed";
        return false;
    }
    err_t err = tcp_bind(pcb, IP_ANY_TYPE, 0);
    if (err != ERR_OK) {
        tcp_close(pcb);
        netif_remove(nf);
        delete nf;
        m_netif = nullptr;
        ::CloseHandle(m_stop_event);
        m_stop_event = nullptr;
        m_running.store(false);
        if (outError) {
            std::ostringstream os;
            os << "Tun2SocksEngineEmbedded: tcp_bind failed err=" << (int)err;
            *outError = os.str();
        }
        return false;
    }
#if defined(TCP_REDIRECTOR_WILDCARD_LISTEN) && TCP_REDIRECTOR_WILDCARD_LISTEN
    // B14: tcp_bind(…, 0) назначил конкретный эфемерный порт через
    // tcp_new_port(); принудительно сбрасываем его в 0, чтобы listener стал
    // wildcard'ом по порту.  tcp_listen ниже скопирует local_port=0 в
    // listen-pcb, а патч в tcp_in.c примет SYN на любой dst-порт.  pcb ещё не
    // в списках lwIP — сброс порта безопасен (NO_SYS, единственный тред).
    pcb->local_port = 0;
#endif
    struct tcp_pcb* listen_pcb = tcp_listen(pcb);
    if (!listen_pcb) {
        tcp_close(pcb);
        netif_remove(nf);
        delete nf;
        m_netif = nullptr;
        ::CloseHandle(m_stop_event);
        m_stop_event = nullptr;
        m_running.store(false);
        if (outError) *outError = "Tun2SocksEngineEmbedded: tcp_listen failed";
        return false;
    }
    tcp_arg(listen_pcb, this);
    tcp_accept(listen_pcb, &EngineTramp::OnAccept);
    m_listen_pcb = listen_pcb;

    // Reaper-тред (уничтожение закрытых flow'ов вне engine-треда) — до engine.
    m_reaper_run.store(true, std::memory_order_relaxed);
    m_reaper_thread = std::thread([this]() { ReaperThreadMain(); });

    // engine-тред
    m_engine_thread = std::thread([this]() { EngineThreadMain(); });

    // Подавим ссылку — параметр gw/mask используются только внутри netif_add.
    (void)gw; (void)mask; (void)any;

    return true;
}

void Tun2SocksEngineEmbedded::Stop() {
    if (!m_running.exchange(false)) return; // уже остановлены

    if (m_stop_event) ::SetEvent(m_stop_event);
    if (m_engine_thread.joinable()) m_engine_thread.join();

    // Закрываем listener.
    if (m_listen_pcb) {
        std::lock_guard<std::recursive_mutex> lk(m_core_lock);
        tcp_close(static_cast<struct tcp_pcb*>(m_listen_pcb));
        m_listen_pcb = nullptr;
    }

    // Закрываем все живые flow'ы.  Сокет-half треды при этом вывалятся из
    // recv() и мы их join'ним.
    std::vector<std::unique_ptr<Flow>> to_drop;
    {
        std::lock_guard<std::mutex> lk(m_flows_mu);
        for (auto& kv : m_flows) to_drop.push_back(std::move(kv.second));
        m_flows.clear();
    }
    for (auto& f : to_drop) {
        // from_engine_thread=false: владение Flow уже у нас (to_drop), CloseFlow
        // не должен дёргать map/reaper — мы сами join'им ридер и уничтожим объект.
        CloseFlow(f.get(), /*from_engine_thread=*/false);
        if (f->sock_reader.joinable()) f->sock_reader.join();
    }
    to_drop.clear();
    m_active_flows.store(0, std::memory_order_relaxed);

    // Задача 3 (Option 2a): гарантированно снимаем ВСЕ оставшиеся DIRECT /32
    // bypass-маршруты (на случай flow'ов, закрытых через Stop-путь без
    // ReleaseDirectBypass) — устраняет утечку stale-route после остановки.
    RemoveAllDirectBypasses();

    // Останавливаем reaper и добираем всё, что могло остаться в очереди утилизации.
    {
        std::lock_guard<std::mutex> lk(m_retire_mu);
        m_reaper_run.store(false, std::memory_order_relaxed);
        m_retire_cv.notify_all();
    }
    if (m_reaper_thread.joinable()) m_reaper_thread.join();
    // Гарантированно уничтожаем остаток очереди (если reaper вышел раньше).
    {
        std::vector<std::unique_ptr<Flow>> leftovers;
        {
            std::lock_guard<std::mutex> lk(m_retire_mu);
            leftovers.swap(m_retire);
        }
        for (auto& f : leftovers) {
            if (f && f->sock_reader.joinable()) f->sock_reader.join();
        }
    }

    // Убираем netif.
    if (m_netif) {
        std::lock_guard<std::recursive_mutex> lk(m_core_lock);
        auto* nf = static_cast<struct netif*>(m_netif);
        netif_remove(nf);
        delete nf;
        m_netif = nullptr;
    }

    if (m_stop_event) {
        ::CloseHandle(m_stop_event);
        m_stop_event = nullptr;
    }
    // lwIP_init нет обратного вызова — глобальные пулы остаются.
    // Повторный Start в v1 не поддерживается (§"NO_SYS notes" в .h).
}

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
