#pragma once

/**
 * @file ITunEngine.h
 * @brief WP9 — общий интерфейс движка «TUN → относительный accept».
 *
 * НАЗНАЧЕНИЕ:
 *   WP10 будет держать `WintunCapture` (реализация ICapture), внутри которого
 *   ЖИВЁТ один движок из семейства ITunEngine: либо
 *   `Tun2SocksEngineEmbedded` (lwIP, WP9), либо `Tun2SocksEngineExternal`
 *   (внешний tun2socks.exe, WP12).  Оба движка обещают одинаковый контракт:
 *     • Start/Stop — жизненный цикл.
 *     • Счётчики трафика/потоков для телеметрии.
 *
 *   Интерфейс намеренно вводится СЕЙЧАС (в WP9), а не в WP10, чтобы
 *   embedded-движок сразу писался «под ITunEngine» и WP10 сводился к прокидке
 *   инстанса за фасад.
 *
 * ПРИМЕЧАНИЕ ПРО КОЛБЭКИ:
 *   Ни один метод не принимает per-flow-callback — маршрутизация flow'ов
 *   (accept → connect(loopback:relay) → splice) полностью инкапсулирована
 *   внутри конкретной реализации.  Причина: у embedded и external движков
 *   принципиально разные точки, где рождается сокет, ведущий в relay; фасад
 *   WintunCapture не должен об этом знать.  Наблюдаемость даётся тремя
 *   счётчиками ниже + опциональным observer-хуком в конструкторе конкретного
 *   движка (см. FlowMeta в Tun2SocksEngineEmbedded.h).
 */

#include <cstdint>
#include <string>

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

/**
 * @brief Универсальный порт tun2socks-движка.
 *
 * Владение: WintunCapture (WP10) держит unique_ptr<ITunEngine>.  Start()
 * поднимает всё, что нужно движку (треды, listener'ы, дочерние процессы).
 * Stop() гарантирует, что после возврата никаких активных потоков/сокетов
 * движок не удерживает.
 */
class ITunEngine {
public:
    virtual ~ITunEngine() = default;

    /**
     * @brief Запустить движок.  Идемпотентен: повторный Start после успешного
     *        Start возвращает true.  После Stop повторный Start НЕ обязан
     *        работать (конкретные движки могут это документировать отдельно —
     *        embedded lwIP-движок, например, поддерживает единственный
     *        Start/Stop-цикл на процесс).
     *
     * @param outError [out, optional] диагностика при провале.
     * @return true при успехе.
     */
    virtual bool Start(std::string* outError) = 0;

    /**
     * @brief Остановить движок.  Идемпотентен.  После возврата — никаких
     *        живых потоков/сокетов движок не держит.
     */
    virtual void Stop() = 0;

    /**
     * @brief Сколько байт прочитано ИЗ туннеля с последнего Start (RX).
     */
    virtual uint64_t RxBytes() const = 0;

    /**
     * @brief Сколько байт записано В туннель с последнего Start (TX).
     */
    virtual uint64_t TxBytes() const = 0;

    /**
     * @brief Сколько сейчас живых TCP flow'ов.
     */
    virtual uint32_t ActiveFlows() const = 0;
};

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
