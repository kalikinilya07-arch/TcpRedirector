/**
 * @file lwip_hooks_impl.c
 * @brief WP9 — реализация LWIP_HOOK_IP4_INPUT для tun2socks-паттерна.
 *
 * ЧТО ДЕЛАЕТ ХУК:
 *   Для каждого входящего IP-пакета переписывает `netif->ip_addr` на
 *   dest-адрес пакета.  После этого стандартная проверка `ip4_input_accept`
 *   пропустит пакет (dst == netif->ip_addr).  Возвращает 0 — lwIP продолжает
 *   обработку пакета как обычно.
 *
 *   На accept'е (`tcp_accept` callback) `pcb->local_ip` будет равен настоящему
 *   destination-адресу пакета — именно ему движок должен «подключиться»
 *   (форвардить в relay).
 *
 * ПОЧЕМУ БЕЗ ЛОКОВ:
 *   NO_SYS=1 → весь lwIP-стек, включая ip_input и netif->input(), крутится
 *   в ЕДИНСТВЕННОМ engine-треде.  Перезапись netif->ip_addr между хуком и
 *   следующим циклом входного пакета невозможна — гонок нет.
 */

#include <string.h>

#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/prot/ip4.h"

/* Прототип объявлен в lwip_hooks.h */
int tcp_redirector_ip4_input_hook(struct pbuf *pbuf, struct netif *input_netif)
{
    if (pbuf == NULL || input_netif == NULL) {
        return 0;
    }

    /* pbuf->payload на момент вызова хука указывает на начало IP-заголовка.
     * IP версия уже проверена в ip4_input (см. код выше вызова хука). */
    if (pbuf->len < sizeof(struct ip_hdr)) {
        return 0; /* лишний драйв — оставим lwIP отбросить как «короткий пакет». */
    }

    const struct ip_hdr *iphdr = (const struct ip_hdr *)pbuf->payload;

    /* Забираем dest-адрес пакета и перезаписываем IP netif'а НАПРЯМУЮ.
     *
     * ВАЖНО (исправление бага «рвутся параллельные соединения»):
     * НЕЛЬЗЯ использовать netif_set_ipaddr() — в lwIP 2.2.0 смена адреса
     * вызывает netif_do_ip_addr_changed() → tcp_netif_ip_addr_changed(),
     * которая делает tcp_abort() ВСЕМ активным pcb, чей local_ip совпадает
     * со старым адресом netif'а.  В tun2socks-паттерне local_ip каждого
     * pcb == его original-dst, поэтому при переключении netif->ip_addr с
     * dst1 на dst2 стек убивал все flow'ы к dst1.  При параллельном трафике
     * к нескольким адресам (обычный браузер) соединения непрерывно рвали
     * друг друга — выживал трафик лишь к одному dst за раз.
     *
     * NO_SYS=1 + единственный engine-тред → запись поля напрямую безопасна.
     * ip4_addr_t хранит адрес в network byte order, как и iphdr->dest. */
    ip4_addr_copy(*ip_2_ip4(&input_netif->ip_addr), iphdr->dest);

    /* 0 — lwIP продолжит стандартную обработку. */
    return 0;
}
