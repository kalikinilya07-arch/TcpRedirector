#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

/**
 * @file arch/sys_arch.h
 * @brief WP9 — минимальный sys_arch для NO_SYS=1 порта lwIP.
 *
 * Даже при NO_SYS=1 lwIP всё равно включает `arch/sys_arch.h` через
 * `include/lwip/sys.h`.  Файл существует; определений семафоров/мьютексов
 * не нужно — при NO_SYS lwIP их не использует.  Единственный обязательный
 * контракт для NO_SYS: `u32_t sys_now(void)` — миллисекундный монотонный
 * счётчик; реализация лежит в sys_now_msvc.cpp.
 *
 * ПРИМЕЧАНИЕ:  протоколы (arp, dhcp и т.п.) обращаются к макросам SYS_ARCH_*
 * только когда SYS_LIGHTWEIGHT_PROT=1; мы держим этот флаг в lwipopts.h = 0
 * (NO_SYS=1 и однопоточный доступ к lwIP-стеку через движковый thread).
 */

#include "arch/cc.h"

/* Ничего платформо-специфичного не декларируем — при NO_SYS=1 lwIP не
 * требует sys_sem_t / sys_mbox_t / sys_thread_t.  sys_now() — extern C
 * прототип для реализации в sys_now_msvc.cpp; lwIP объявляет её сам через
 * lwip/sys.h, поэтому здесь ре-декларация не требуется.
 */

#endif /* LWIP_ARCH_SYS_ARCH_H */
