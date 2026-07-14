#pragma once

/**
 * @file WintunSession.h
 * @brief WP8 — RAII-обёртка над data-сессией Wintun (ring-buffer I/O).
 *
 * НАЗНАЧЕНИЕ:
 *   Открыть/закрыть сессию (StartSession/EndSession), предоставить
 *   блокирующее чтение с поддержкой stop-event и неблокирующую отправку
 *   пакета.  Класс не знает про lwIP; он работает уровнем «сырой IP-пакет
 *   вход/выход».
 *
 * СЕМАНТИКА RX:
 *   ReceiveInto() ждёт на event'е Wintun.  Если параллельно взводится
 *   stop_event — метод корректно выходит с кодом «отменено».  Пакеты
 *   выбираются из ring-buffer'а Wintun (owned memory), копируются во
 *   владелецкий std::vector, и сразу же освобождаются через
 *   ReleaseReceivePacket.  Это упрощает жизнь вызывающему за счёт одного
 *   memcpy — приемлемая цена для WP8 (WP10 при необходимости добавит
 *   zero-copy overload).
 *
 * СЕМАНТИКА TX:
 *   Send() резервирует буфер в TX-ring через AllocateSendPacket, копирует
 *   данные, отправляет через SendPacket.  Возвращает false при
 *   ERROR_HANDLE_EOF (сессия мертва) и ERROR_BUFFER_OVERFLOW (ring полон);
 *   различить эти случаи можно через GetLastError после false.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "WintunApi.h"

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

class WintunSession {
public:
    /**
     * @brief Разумный дефолт ring-capacity.  Wintun docs рекомендуют 4 MiB.
     *
     * Должен быть power-of-two в [WINTUN_MIN_RING_CAPACITY, WINTUN_MAX_RING_CAPACITY].
     */
    static constexpr uint32_t kDefaultCapacity = 0x400000; // 4 MiB

    /**
     * @brief Открыть data-сессию для уже созданного адаптера.
     *
     * @param api       Загруженный WintunApi (не nullptr).
     * @param adapter   Валидный WINTUN_ADAPTER_HANDLE.  ВРЕМЯ ЖИЗНИ:
     *                  вызывающий обязан держать адаптер живым, пока жива
     *                  сессия.  Мы не берём shared-owning на адаптер, чтобы
     *                  не тащить heavy зависимость; но держим shared_ptr
     *                  на API — DLL точно не выгрузится раньше сессии.
     * @param capacity  Ring-buffer capacity (power-of-two, диапазон см. выше).
     * @param outError  [out, обязателен] диагностика.
     */
    static std::unique_ptr<WintunSession> Start(
        std::shared_ptr<WintunApi> api,
        WINTUN_ADAPTER_HANDLE adapter,
        uint32_t capacity,
        std::string* outError);

    ~WintunSession();

    WintunSession(const WintunSession&) = delete;
    WintunSession& operator=(const WintunSession&) = delete;
    WintunSession(WintunSession&&) = delete;
    WintunSession& operator=(WintunSession&&) = delete;

    /**
     * @brief Блокирующее чтение одного пакета.
     *
     * Внутри — цикл:
     *   • ReceivePacket → если !nullptr, скопировать в out_buffer, освободить,
     *     вернуть размер.
     *   • Иначе GLE=ERROR_NO_MORE_ITEMS: подождать на
     *     WaitForMultipleObjects({m_readEvent, stop_event}) INFINITE.
     *     - stop_event сработал → return 0 (отмена).
     *     - readEvent → продолжить цикл.
     *   • Иначе (другая ошибка WinAPI) → return -1, заполнить errorMsg.
     *
     * @param out_buffer Целевой буфер.  Метод присвоит ему точно
     *                   packet_size байт; предыдущее содержимое перезаписано.
     * @param stop_event Опциональное событие отмены.  Если INVALID_HANDLE_VALUE
     *                   / NULL — метод ждёт только readEvent.
     * @param errorMsg   [out, обязателен] заполняется только при ret<0.
     * @return >0 — количество прочитанных байт (равно out_buffer.size()).
     *          0 — stop_event сработал, буфер не изменён.
     *         <0 — фатальная ошибка сессии; errorMsg содержит детали.
     */
    int ReceiveInto(std::vector<uint8_t>& out_buffer,
                    HANDLE stop_event,
                    std::string* errorMsg);

    /**
     * @brief НЕблокирующее чтение одного пакета (без ожидания на event'е).
     *
     * В отличие от ReceiveInto(), при пустом ring-буфере метод немедленно
     * возвращает 0 (а НЕ ждёт readEvent).  Нужен для engine-цикла, который
     * должен дренировать всё доступное, а затем сам решать, когда ждать
     * (иначе блокирующее ожидание внутри дренажа «съедает» вызовы
     * sys_check_timeouts() и таймеры lwIP не срабатывают).
     *
     * @param out_buffer Целевой буфер (перезаписывается при ret>0).
     * @param errorMsg   [out, опционально] заполняется только при ret<0.
     * @return >0 — прочитано байт; 0 — ring пуст (ERROR_NO_MORE_ITEMS);
     *         <0 — фатальная ошибка сессии.
     */
    int TryReceiveInto(std::vector<uint8_t>& out_buffer,
                       std::string* errorMsg);

    /**
     * @brief Неблокирующая отправка одного IP-пакета.
     *
     * @return true при успехе; false — GLE даст ERROR_BUFFER_OVERFLOW
     *         (ring полон, повторить позже) или ERROR_HANDLE_EOF (сессия мертва).
     */
    bool Send(const uint8_t* data, uint32_t size);

    /**
     * @brief Событие «есть пакет на прочтение».  Владеет им wintun; НЕ CloseHandle.
     */
    HANDLE ReadWaitEvent() const { return m_readEvent; }

    /**
     * @brief Handle низлежащей сессии (для отладки / расширений).  Не для CloseHandle.
     */
    WINTUN_SESSION_HANDLE Handle() const { return m_session; }

private:
    WintunSession() = default;

    std::shared_ptr<WintunApi> m_api;
    WINTUN_SESSION_HANDLE       m_session   = nullptr;
    HANDLE                      m_readEvent = nullptr; // не владеем; wintun-owned.
};

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
