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
#include <windows.h>
// clang-format on

#include "Tun2SocksEngineEmbedded.h"

#include "../../../domain/services/RuleEngine.h"
#include "../../../domain/ports/IConnectionMonitor.h"  // ILogSink / LogLevel / IConnectionMonitor
#include "../../../domain/ports/IConnectionTable.h"     // Add/Remove — relay dst recovery
#include "../../../domain/entities/ProxyConfig.h"       // ConnectionRecord / ConnectionState
#include "../../utf8_convert.h"

#include <algorithm>
#include <cstring>
#include <sstream>

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
            // DIRECT (не-совпавший трафик) в embedded НЕ пропускается напрямую.
            //
            // Почему нельзя: OnAccept выполняется НА engine-треде под
            // m_core_lock, а blocking connect() к реальному dst уходит по
            // split-tunnel маршрутам ОБРАТНО в этот же TUN (тот же engine-тред) —
            // SYN некому прочитать, connect висит до таймаута и замораживает
            // весь стек (в неблокирующем варианте — шторм повторных accept'ов).
            // Сквозной direct-проход возможен только в обход туннеля по
            // физическому интерфейсу (нужен async-outbound + interface-bind —
            // вне рамок v1, см. ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md).
            //
            // Поэтому fail-fast: соединение отклоняется (сервис стабилен, без
            // фризов).  Практический вывод:
            //   • «проксировать ВЕСЬ трафик» → wintun.process_filter_enabled=false
            //     (DIRECT тогда не возникает — весь TCP идёт в PROXY);
            //   • селективное проксирование С прямым проходом остального →
            //     capture_mode=windivert.
            engine->FilterLog(domain::LogLevel::Trace,
                "[wintun][DIRECT-drop] non-matched flow dropped (embedded has no "
                "direct pass-through) -> " + meta.original_dst_ip + ":"
                + std::to_string(meta.original_dst_port));
            tcp_abort(newpcb);
            return ERR_ABRT;
        }

        // Сюда доходит только PROXY → форвард на локальный relay 127.0.0.1:relay_port.
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
            rec.proxy_enabled = (decision == FlowDecision::Proxy);
            flow->conn_id = rec.id;
            engine->m_conn_monitor->AddConnection(rec);
        }

        tcp_arg (newpcb, flow.get());
        tcp_recv(newpcb, &EngineTramp::OnRecv);
        tcp_sent(newpcb, &EngineTramp::OnSent);
        tcp_err (newpcb, &EngineTramp::OnErr);

        Tun2SocksEngineEmbedded::Flow* raw = flow.get();
        {
            std::lock_guard<std::mutex> lk(engine->m_flows_mu);
            engine->m_flows.emplace(newpcb, std::move(flow));
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
    // уедет в туннель.  Полное закрытие делает CloseFlow.
    {
        std::lock_guard<std::recursive_mutex> lk(m_core_lock);
        if (!flow->pcb_dead.load(std::memory_order_relaxed) && flow->pcb != nullptr) {
            tcp_shutdown(flow->pcb, 0, 1); // shut_tx=1
        }
    }
    // Не удаляем flow из map'а здесь — это делает Stop() или явный CloseFlow
    // из engine-треда, чтобы избежать гонки с OnRecv/OnErr.
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

    // Закрываем сокет — сокет-ридер вывалится.
    if (flow->sock != INVALID_SOCKET) {
        ::shutdown(flow->sock, SD_BOTH);
        ::closesocket(flow->sock);
        flow->sock = INVALID_SOCKET;
    }

    // Закрываем pcb под lock'ом (если ещё жив).
    if (from_engine_thread) {
        // мы уже в engine-треде — lock не нужен, но всё равно защитимся recursive
        std::lock_guard<std::recursive_mutex> lk(m_core_lock);
        if (!flow->pcb_dead.load(std::memory_order_relaxed) && flow->pcb) {
            tcp_arg (flow->pcb, nullptr);
            tcp_recv(flow->pcb, nullptr);
            tcp_sent(flow->pcb, nullptr);
            tcp_err (flow->pcb, nullptr);
            tcp_close(flow->pcb);
            flow->pcb = nullptr;
        }
    } else {
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
                    FilterLog(domain::LogLevel::Trace, os.str());
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

    // Catch-all listener.  Bind на IP_ANY_TYPE:0 → accept'ится любой SYN.
    // Хук IP4_INPUT (см. lwip_hooks_impl.c) переписывает netif->ip_addr на
    // dest каждого пакета → ip4_input_accept проходит.  Внутри TCP-стека
    // такой pcb (bind = *:0) считается матчащимся любому dst/src.
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
        CloseFlow(f.get(), /*from_engine_thread=*/true);
        if (f->sock_reader.joinable()) f->sock_reader.join();
    }
    m_active_flows.store(0, std::memory_order_relaxed);

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
