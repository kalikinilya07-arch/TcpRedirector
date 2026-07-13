#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

/**
 * @file arch/cc.h
 * @brief WP9 — минимальный CPU/OS-abstraction для lwIP 2.2.0 под MSVC 2022 (x64).
 *
 * lwIP требует, чтобы каждый порт предоставил `arch/cc.h` с базовыми
 * типами и макросами.  Файл ЛЕЖИТ В ПРОЕКТЕ (не в external/lwip), потому
 * что относится к нашей конфигурации, а vendor-исходники lwIP не редактируем.
 *
 * Всё, что требуется от `cc.h`:
 *   • Целочисленные типы u8_t/s8_t/u16_t/s16_t/u32_t/s32_t + `mem_ptr_t`.
 *   • Форматные строки для отладочной печати.
 *   • Байтпорядковые макросы, PACK_STRUCT_* и LWIP_PLATFORM_ASSERT / DIAG.
 *   • LWIP_RAND() — источник псевдослучайных чисел для ISN.
 *
 * MSVC поддерживает <stdint.h> ещё с VS2010 — используем его.  `__attribute__`
 * и pragma-pack макросы адаптированы под MSVC.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h> /* rand() */

/* --- integer types -------------------------------------------------------- */
/* lwIP исторически использует эти typedef'ы.  Просто маппим на stdint.       */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uint64_t  u64_t;
typedef int64_t   s64_t;

typedef uintptr_t mem_ptr_t;

/* --- printf format specifiers -------------------------------------------- */
#define X8_F   "02x"
#define U16_F  "hu"
#define S16_F  "hd"
#define X16_F  "hx"
#define U32_F  "u"
#define S32_F  "d"
#define X32_F  "x"
#define SZT_F  "zu"

/* --- byte order ----------------------------------------------------------- */
/* MSVC x86/x64 всегда little-endian; и так — но объявим явно.               */
#ifndef BYTE_ORDER
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#define BYTE_ORDER    LITTLE_ENDIAN
#endif

/* --- struct packing ------------------------------------------------------- */
/* MSVC не понимает __attribute__((packed)).  Заворачиваем каждую packed-структуру
 * в pragma pack(1).  lwIP разработчиками спроектирован именно так — они дают
 * четыре крючка PACK_STRUCT_BEGIN/END/STRUCT/FIELD, каждый порт заполняет.
 */
#define PACK_STRUCT_BEGIN         __pragma(pack(push, 1))
#define PACK_STRUCT_STRUCT        /* nothing — упаковка задаётся BEGIN/END */
#define PACK_STRUCT_END           __pragma(pack(pop))
#define PACK_STRUCT_FIELD(x)      x
/* НЕ определяем PACK_STRUCT_USE_INCLUDES: lwIP по умолчанию использует
 * BEGIN/END/STRUCT/FIELD-макросы и не требует внешних bpstruct.h/epstruct.h. */

/* --- diagnostics ---------------------------------------------------------- */
#include <stdio.h>

#define LWIP_PLATFORM_DIAG(x)     do { printf x; } while (0)

#define LWIP_PLATFORM_ASSERT(x)                                                    \
    do {                                                                           \
        fprintf(stderr, "lwip assert \"%s\" at %s:%d\n", (x), __FILE__, __LINE__); \
        fflush(stderr);                                                            \
        __debugbreak();                                                            \
    } while (0)

/* --- randomness ----------------------------------------------------------- */
/* Используется для генерации ISN и т.п.  rand() достаточно для v1 — на этапе
 * hardening (WP13) можно перейти на BCryptGenRandom.                             */
#define LWIP_RAND()               ((u32_t)rand())

/* --- MSVC warning silencers ---------------------------------------------- */
#ifdef _MSC_VER
/* lwIP местами использует C89-style смешивание, MSVC C4200/C4820/C4127 плюются;
 * оставляем warning-level, но глушим самые шумные категории только внутри lwIP. */
#pragma warning(disable: 4127)  /* conditional expression is constant           */
#pragma warning(disable: 4820)  /* padding                                      */
#pragma warning(disable: 4204)  /* non-constant aggregate initializer           */
#pragma warning(disable: 4706)  /* assignment within conditional expression    */
#pragma warning(disable: 4267)  /* size_t → smaller int conversion            */
#pragma warning(disable: 4244)  /* possible loss of data conversion           */
#pragma warning(disable: 4245)  /* signed/unsigned mismatch                    */
#pragma warning(disable: 4310)  /* cast truncates constant value               */
#pragma warning(disable: 4324)  /* structure padded due to alignment specifier */
#pragma warning(disable: 4090)  /* different const/volatile qualifiers         */
#endif

#endif /* LWIP_ARCH_CC_H */
