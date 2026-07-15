#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/**
 * @file lwipopts.h
 * @brief WP9 — конфигурация lwIP для embedded tun2socks-движка (NO_SYS=1).
 *
 * Ключевые решения:
 *   - NO_SYS=1: без семафоров/мьютексов lwIP; весь стек крутится в одном
 *     dedicated «engine»-треде (см. Tun2SocksEngineEmbedded).  Loopback-сокеты
 *     живут в собственных thread'ах, обмен с lwIP идёт через lock'нутый
 *     LWIP_TCPIP_CORE_LOCKING аналог — а именно, вручную взятую critical
 *     section, экспортируемую движком.
 *   - LWIP_IPV4=1, LWIP_IPV6=0 для v1 (см. §6 плана: IPv6 опционален).
 *   - Только TCP raw API (tcp_bind/listen/accept/…): LWIP_TCP=1,
 *     LWIP_SOCKET=0, LWIP_NETCONN=0.
 *   - MEM_LIBC_MALLOC=1 + MEMP_MEM_MALLOC=1: пусть CRT рулит памятью,
 *     без внутренних lwIP pool'ов для memp — так проще на MSVC, лишний
 *     .bss не займём.  Для pbuf оставляем PBUF_POOL_SIZE=128 (пул нужен
 *     для входящих rx-пакетов, чтобы не звать malloc из «горячего» пути).
 *   - LWIP_TCP_KEEPALIVE=1: relay-сокет живёт долго, keepalive поможет
 *     отвалить зависшие сессии.
 *   - LWIP_STATS=0: экономим ~1 KiB и синхронизации.
 *   - LWIP_TIMEVAL_PRIVATE=0: используем стандартный <sys/time.h>… нет, на
 *     Windows он отсутствует; оставляем =1, чтобы lwIP определил свою.
 *   - LWIP_PROVIDE_ERRNO=1: у MSVC errno есть, но lwIP импортирует свой набор
 *     E-констант через lwip/errno.h; проще, чтобы lwIP их сам определил.
 *
 * Всё, что не переопределено, берётся из include/lwip/opt.h (дефолты lwIP).
 */

/* --- глобальные переключатели -------------------------------------------- */
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_TIMERS                     1
#define LWIP_TIMERS_CUSTOM              0

/* --- дизабл всего, что не нужно ----------------------------------------- */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_DNS                        0
#define LWIP_ARP                        0
#define LWIP_ETHERNET                   0
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_RAW                        1  /* нужен для tcp raw API (не сокеты) */
#define LWIP_ICMP                       0
#define LWIP_UDP                        0
#define LWIP_TCP                        1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_STATS                      0
#define LWIP_NETIF_API                  0
#define LWIP_NETIF_HOSTNAME             0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_REMOVE_CALLBACK      0

/* --- память -------------------------------------------------------------- */
#define MEM_LIBC_MALLOC                 1
#define MEMP_MEM_MALLOC                 1
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (256 * 1024)

/* --- pbuf pool ----------------------------------------------------------- */
#define PBUF_POOL_SIZE                  128
#define PBUF_POOL_BUFSIZE               1600   /* MTU 1500 + запас на заголовки */

/* --- TCP tuning ---------------------------------------------------------- */
#define TCP_MSS                         1460
/* NOTE (WP9): план запрашивал TCP_WND = 64*TCP_MSS ≈ 93 KiB, но без window
 * scaling lwIP хранит TCP_WND в u16_t (< 64 KiB), что init.c проверяет
 * через #error.  Ограничиваем окно 44*MSS ≈ 64 KiB — это максимум без
 * scaling'а; TCP_SND_BUF оставляем 64*MSS (lwIP держит его в tcpwnd_size_t
 * шире u16_t при большом лимите).  Для локального loopback-relay такой
 * буфер более чем достаточен.  Window scaling — задача hardening-этапа. */
#define TCP_WND                         (44 * TCP_MSS)
#define TCP_SND_BUF                     (44 * TCP_MSS)
#define TCP_SND_QUEUELEN                (4 * (TCP_SND_BUF) / (TCP_MSS))
#define MEMP_NUM_TCP_PCB                256    /* макс. одновременных TCP-потоков */
#define MEMP_NUM_TCP_PCB_LISTEN         8
#define MEMP_NUM_TCP_SEG                (2 * TCP_SND_QUEUELEN)
#define LWIP_TCP_KEEPALIVE              1

/* --- checksum offload (нет — считаем на CPU) ---------------------------- */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_TCP              1

/* --- errno / time helpers ----------------------------------------------- */
#define LWIP_PROVIDE_ERRNO              1
#define LWIP_TIMEVAL_PRIVATE            1

/* --- debug (отключено в релизе) ----------------------------------------- */
#ifndef LWIP_DEBUG
#define LWIP_DEBUG                      0
#endif

/* --- hooks --------------------------------------------------------------- */
/* Активируем LWIP_HOOK_IP4_INPUT через LWIP_HOOK_FILENAME (см. §"Hooks" в
 * include/lwip/opt.h).  Хук нужен, чтобы принимать любой TCP-пакет,
 * пришедший из туннеля, — без него ip4_input отбрасывает пакеты, чей
 * destination-IP не совпадает с IP нашего netif'а.  См. lwip_hooks.h. */
#define LWIP_HOOK_FILENAME              "lwip_hooks.h"

/* --- catch-all listener: wildcard destination port (B14) ----------------- */
/* Проект держит ОДИН catch-all TCP-listener (tcp_bind(IP_ANY_TYPE, 0) +
 * tcp_listen), который должен принимать SYN на ЛЮБОЙ destination-порт
 * (tun2socks-паттерн).  Стоковый lwIP матчит listener только при
 * lpcb->local_port == dst-порт (см. tcp_in.c), а порт-0 в tcp_bind получает
 * конкретный эфемерный порт через tcp_new_port() — поэтому НИ ОДИН SYN не
 * матчился и OnAccept никогда не вызывался («трафик входит в туннель, но не
 * выходит на прокси», active_flows=0).  См. ИЗВЕСТНЫЕ_ПРОБЛЕМЫ.md §B14.
 *
 * Этот флаг включает МИНИМАЛЬНЫЙ project-owned патч в vendored tcp_in.c
 * (помечен «TCPREDIR B14»): listen-pcb с local_port==0 трактуется как
 * wildcard по порту, а порт нового pcb берётся из реального dst SYN'а
 * (tcphdr->dest) — чтобы движок восстановил original-dst в OnAccept.
 * Все правки vendored-кода обёрнуты в этот макрос и легко откатываются. */
#define TCP_REDIRECTOR_WILDCARD_LISTEN  1

/* --- alignment / packing ------------------------------------------------- */
/* определено в arch/cc.h — здесь ничего не переопределяем */

#endif /* LWIP_LWIPOPTS_H */
