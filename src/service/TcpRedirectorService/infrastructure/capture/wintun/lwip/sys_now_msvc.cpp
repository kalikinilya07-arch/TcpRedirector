/**
 * @file sys_now_msvc.cpp
 * @brief WP9 — реализация `u32_t sys_now(void)` для NO_SYS-порта lwIP на Windows.
 *
 * lwIP объявляет прототип `u32_t sys_now(void)` в include/lwip/sys.h и требует
 * его от каждого порта.  Для NO_SYS этот таймер используется исключительно
 * механизмом `sys_check_timeouts()` — миллисекундный монотонный счётчик.
 *
 * `GetTickCount64()` был бы правильнее с точки зрения долгого uptime, но lwIP
 * ожидает u32_t (~49.7 суток wrap-around) и умеет с этим жить: сравнения
 * времени внутри lwIP делаются через макрос `sys_now() - t0` в u32-арифметике,
 * которая корректно оборачивается.
 */

extern "C" {
#include "lwip/arch.h"
#include "lwip/sys.h"
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" u32_t sys_now(void) {
    return static_cast<u32_t>(GetTickCount());
}
