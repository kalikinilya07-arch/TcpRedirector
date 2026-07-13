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

    /* Забираем dest-адрес пакета и перезаписываем IP netif'а.
     * ip4_addr_t в lwIP хранит адрес в network byte order (u32_t), как и
     * поле iphdr->dest — так что просто копируем сырое значение через
     * ip_addr_copy_from_ip4 (безопасное для alignment API lwIP). */
    ip4_addr_t new_ip;
    ip4_addr_copy(new_ip, iphdr->dest);
    netif_set_ipaddr(input_netif, &new_ip);

    /* 0 — lwIP продолжит стандартную обработку. */
    return 0;
}
