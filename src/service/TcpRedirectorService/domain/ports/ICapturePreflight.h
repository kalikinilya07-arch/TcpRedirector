#pragma once

/**
 * @file ICapturePreflight.h
 * @brief WP7 — интерфейс проверки зависимостей активного capture-движка ДО его запуска.
 *
 * Ключевое требование (см. plans/WINTUN_INTEGRATION_PLAN.md §8 «Mode-conditional
 * dependency validation» и §11 «WP7», пункт #6 требований пользователя):
 *
 *   При выборе одного из режимов захвата (WinDivert | Wintun) стартовые проверки
 *   и валидация файлов/зависимостей НЕАКТИВНОГО механизма должны быть ПРОПУЩЕНЫ.
 *
 * Схема:
 *   ServiceMain::Initialize()
 *     └── switch (capture_mode)
 *           ├─ WinDivert → WinDivertPreflight::Check()   (проверяет ТОЛЬКО файлы WinDivert)
 *           └─ Wintun    → WintunPreflight::Check()      (проверяет ТОЛЬКО wintun.dll и,
 *                                                         при engine="external",
 *                                                         tun2socks.exe)
 *
 * Порт живёт в domain/ports, без зависимостей от инфраструктуры (STL only), чтобы
 * тесты могли подменить реализацию.
 */

#include <string>
#include <vector>

namespace tcp_redirector {
namespace domain {
namespace ports {

/**
 * @brief Результат preflight-проверки одного capture-движка.
 *
 * Разделение failures/warnings:
 *   - failures — фатально, сервис должен отказаться стартовать.
 *   - warnings — некритично (например, версия DLL не определяется через
 *                GetFileVersionInfo), сервис стартует, но пишет в лог.
 *
 * Поле mode содержит человекочитаемое имя активного движка ("windivert" | "wintun")
 * и используется в лог-сообщениях и (в будущем) в IPC GetStatus-ответе.
 */
struct PreflightResult {
    bool ok = false;                         //!< Итог: true, если failures пусто.
    std::string mode;                        //!< "windivert" | "wintun".
    std::vector<std::string> failures;       //!< Фатальные проблемы.
    std::vector<std::string> warnings;       //!< Некритичные предупреждения.
};

/**
 * @brief Интерфейс preflight-валидатора capture-движка.
 *
 * Реализации:
 *   - infrastructure::preflight::WinDivertPreflight
 *   - infrastructure::preflight::WintunPreflight (engine-aware)
 */
class ICapturePreflight {
public:
    virtual ~ICapturePreflight() = default;

    /**
     * @brief Выполнить набор проверок, специфичных для конкретного движка.
     * @return Результат: ok=false, если хотя бы одна проверка попала в failures.
     */
    virtual PreflightResult Check() = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector
