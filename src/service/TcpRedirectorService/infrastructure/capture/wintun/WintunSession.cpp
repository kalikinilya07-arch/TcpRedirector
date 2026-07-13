/**
 * @file WintunSession.cpp
 * @brief WP8 — реализация RAII-сессии данных Wintun.
 */

#include "WintunSession.h"

#include <cstring>
#include <sstream>
#include <string>

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

namespace {

/**
 * @brief power-of-two в диапазоне [WINTUN_MIN_RING_CAPACITY..WINTUN_MAX_RING_CAPACITY]?
 */
bool IsValidCapacity(uint32_t cap) {
    if (cap < WINTUN_MIN_RING_CAPACITY) return false;
    if (cap > WINTUN_MAX_RING_CAPACITY) return false;
    // POW2-проверка через bit trick.
    return (cap & (cap - 1u)) == 0u;
}

} // namespace

// ============================================================================
// WintunSession::Start
// ============================================================================

std::unique_ptr<WintunSession> WintunSession::Start(
        std::shared_ptr<WintunApi> api,
        WINTUN_ADAPTER_HANDLE adapter,
        uint32_t capacity,
        std::string* outError) {

    static std::string s_dummy;
    std::string& err = outError ? *outError : s_dummy;
    err.clear();

    if (!api) {
        err = "WintunSession::Start: api is null";
        return nullptr;
    }
    if (!adapter) {
        err = "WintunSession::Start: adapter handle is null";
        return nullptr;
    }
    if (!IsValidCapacity(capacity)) {
        std::ostringstream oss;
        oss << "WintunSession::Start: invalid capacity=" << capacity
            << " (must be power-of-two in [0x"
            << std::hex << WINTUN_MIN_RING_CAPACITY << ", 0x"
            << WINTUN_MAX_RING_CAPACITY << "])";
        err = oss.str();
        return nullptr;
    }

    SetLastError(0);
    WINTUN_SESSION_HANDLE s = api->StartSession(adapter, capacity);
    if (s == nullptr) {
        DWORD gle = GetLastError();
        std::ostringstream oss;
        oss << "WintunStartSession failed: GLE=" << gle
            << " (capacity=" << capacity << ")";
        err = oss.str();
        return nullptr;
    }

    // GetReadWaitEvent — «gratis» вызов: возвращает уже созданный wintun-event.
    HANDLE readEv = api->GetReadWaitEvent(s);
    if (readEv == nullptr) {
        DWORD gle = GetLastError();
        std::ostringstream oss;
        oss << "WintunGetReadWaitEvent returned NULL: GLE=" << gle;
        err = oss.str();
        // Обязательно закрываем сессию, иначе wintun будет думать, что мы её держим.
        api->EndSession(s);
        return nullptr;
    }

    std::unique_ptr<WintunSession> sess(new WintunSession());
    sess->m_api       = std::move(api);
    sess->m_session   = s;
    sess->m_readEvent = readEv;
    return sess;
}

// ============================================================================
// WintunSession::~WintunSession
// ============================================================================

WintunSession::~WintunSession() {
    if (m_session && m_api && m_api->EndSession) {
        m_api->EndSession(m_session);
        m_session = nullptr;
    }
    // m_readEvent мы НЕ закрываем: он owned by wintun.
    m_readEvent = nullptr;
}

// ============================================================================
// WintunSession::ReceiveInto
// ============================================================================

int WintunSession::ReceiveInto(std::vector<uint8_t>& out_buffer,
                                HANDLE stop_event,
                                std::string* errorMsg) {
    static std::string s_dummy;
    std::string& err = errorMsg ? *errorMsg : s_dummy;
    err.clear();

    if (!m_session || !m_api || !m_api->ReceivePacket) {
        err = "WintunSession::ReceiveInto: session is not started";
        return -1;
    }

    // Валидность stop_event: NULL и INVALID_HANDLE_VALUE трактуем одинаково — «не ждать stop».
    const bool haveStop = (stop_event != nullptr && stop_event != INVALID_HANDLE_VALUE);

    for (;;) {
        DWORD size = 0;
        SetLastError(0);
        BYTE* pkt = m_api->ReceivePacket(m_session, &size);
        if (pkt != nullptr) {
            // Копируем в вызывающий буфер и освобождаем wintun-память.
            out_buffer.assign(pkt, pkt + size);
            m_api->ReleaseReceivePacket(m_session, pkt);
            return static_cast<int>(size);
        }

        DWORD gle = GetLastError();
        if (gle == ERROR_NO_MORE_ITEMS) {
            // Пустой ring — ждём readEvent (+ опциональный stop).
            HANDLE waits[2] = { m_readEvent, stop_event };
            DWORD nCount    = haveStop ? 2u : 1u;
            DWORD rc = WaitForMultipleObjects(nCount, waits, FALSE, INFINITE);

            if (rc == WAIT_OBJECT_0) {
                // Данные готовы — переходим к следующей итерации цикла.
                continue;
            }
            if (haveStop && rc == WAIT_OBJECT_0 + 1) {
                // Отмена — контрактный возврат 0.
                return 0;
            }

            DWORD wgle = GetLastError();
            std::ostringstream oss;
            oss << "WaitForMultipleObjects failed in WintunSession::ReceiveInto: rc="
                << rc << " GLE=" << wgle;
            err = oss.str();
            return -1;
        }

        if (gle == ERROR_HANDLE_EOF) {
            err = "WintunSession::ReceiveInto: session ended (ERROR_HANDLE_EOF)";
            return -1;
        }

        // Любая другая ошибка — фатально.
        std::ostringstream oss;
        oss << "WintunReceivePacket failed: GLE=" << gle;
        err = oss.str();
        return -1;
    }
}

// ============================================================================
// WintunSession::Send
// ============================================================================

bool WintunSession::Send(const uint8_t* data, uint32_t size) {
    if (!m_session || !m_api || !m_api->AllocateSendPacket || !m_api->SendPacket) {
        // GLE выставим сами, чтобы вызывающий видел вменяемый код.
        SetLastError(ERROR_INVALID_STATE);
        return false;
    }
    if (data == nullptr || size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (size > WINTUN_MAX_IP_PACKET_SIZE) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    SetLastError(0);
    BYTE* dst = m_api->AllocateSendPacket(m_session, size);
    if (dst == nullptr) {
        // GLE уже выставлен wintun'ом (ERROR_BUFFER_OVERFLOW / ERROR_HANDLE_EOF).
        return false;
    }
    std::memcpy(dst, data, size);
    m_api->SendPacket(m_session, dst);
    return true;
}

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
