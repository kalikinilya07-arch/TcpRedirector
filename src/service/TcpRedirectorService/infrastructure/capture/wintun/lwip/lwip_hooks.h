#ifndef LWIP_HOOKS_H
#define LWIP_HOOKS_H

/**
 * @file lwip_hooks.h
 * @brief WP9 — приёмник LWIP_HOOK_FILENAME.
 *
 * lwIP-исходники, содержащие точки расширения (ip4.c, tcp_in.c и т.д.),
 * инклудят LWIP_HOOK_FILENAME (см. include/lwip/opt.h §"Hooks").
 * Мы направляем сюда все хуки, а собственно логика лежит в
 * lwip_hooks_impl.c (project-owned, external/lwip не редактируем).
 *
 * КРИТИЧЕСКИЙ ХУК: LWIP_HOOK_IP4_INPUT.
 *   lwIP по умолчанию отбрасывает пакет, если dest-IP не совпадает с
 *   IP netif'а (`ip4_input_accept`).  Для tun2socks нужно ПРИНЯТЬ ЛЮБОЙ
 *   TCP-пакет, пришедший из туннеля — цель этой сессии как раз в том,
 *   чтобы играть роль «любого» получателя в подсети туннеля.
 *
 *   Решение (проверенный паттерн, используемый badvpn/tun2socks и
 *   hev-socks5-tunnel): в ip4_input перезаписать netif->ip_addr на
 *   dest-адрес входящего пакета *перед* тем, как lwIP проверит
 *   принадлежность.  После этого `ip4_input_accept(netif)` возвращает
 *   true, пакет попадает во внутренности TCP-стека, и tcp_accept видит
 *   в новом pcb настоящий original-dst (`pcb->local_ip`).
 *
 *   Так как весь lwIP-стек крутится в ОДНОМ движковом треде (NO_SYS=1),
 *   гонки за netif->ip_addr нет: пакет обработан до следующего вызова
 *   netif_input().  Возвращаем 0 → пакет обрабатывается lwIP как обычно.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct pbuf;
struct netif;

/**
 * @brief Заглушка-обёртка над реализацией из lwip_hooks_impl.c.
 *
 * Возвращает 0 — lwIP продолжает обычную обработку пакета.
 * != 0 — hook считает, что пакет полностью потреблён (мы этого не делаем).
 */
int tcp_redirector_ip4_input_hook(struct pbuf *pbuf, struct netif *input_netif);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Собственно точка, куда lwIP включит наш хук.  Макрос должен быть определён
 * ДО того, как lwIP-исходник его вызовет — что и делает LWIP_HOOK_FILENAME. */
#define LWIP_HOOK_IP4_INPUT(pbuf, input_netif) \
    tcp_redirector_ip4_input_hook((pbuf), (input_netif))

#endif /* LWIP_HOOKS_H */
