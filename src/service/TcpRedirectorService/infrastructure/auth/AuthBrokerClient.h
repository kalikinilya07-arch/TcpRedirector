#pragma once

/**
 * @file AuthBrokerClient.h
 * @brief Service-side named-pipe CLIENT of the per-user auth helper (Phase 4).
 *
 * Variant 4b, Phase 4. См. plans/kerberos_per_user_auth_helper_plan.md §2 (IPC),
 * §3 (security), §5 (fallback) и src/service/AuthHelper/README.md (§"Phase 4
 * notes").
 *
 * Роль обратна GUI-каналу: helper — СЕРВЕР named-pipe, служба (LocalSystem) —
 * КЛИЕНТ. Клиент:
 *   1. Подключается к \\.\pipe\TcpRedirectorAuth_<sessionId>
 *      (MakeAuthPipeNameW) через CreateFileW с ограниченным ожиданием
 *      (WaitNamedPipe / повторные попытки в пределах helper_timeout_ms) и
 *      переключает канал в PIPE_READMODE_MESSAGE (message-mode, как требует §2.1
 *      и README).
 *   2. Читает первое сообщение — helper'ский `hello` — и отдаёт его вызывающему
 *      (BrokeredAuthProvider) для проверки session_id/nonce (§3.2).
 *   3. Гоняет шаги: SendStep(SspiStepRequest, out) — один write, один read с
 *      таймаутом, парсинг ParseSspiStepResponse. Никогда не бросает.
 *   4. SendRelease(correlation_id) — best-effort освобождение SspiContext на
 *      helper'е (§2.3).
 *
 * Все read/write ограничены helper_timeout_ms через overlapped I/O +
 * WaitForSingleObject; kMaxMessageSize enforced на приёме и отправке. Хэндл pipe
 * закрывается в деструкторе (RAII).
 *
 * Не потокобезопасен: экземпляр живёт в пределах одного соединения relay /
 * одного BrokeredAuthProvider (один поток ConnectionHandler).
 *
 * Что НЕ входит в Phase 4 (сознательно):
 *   - Запуск helper'а / резолв sessionId+nonce — это Phase 5/6. Клиент получает
 *     готовые sessionId и (для проверки на стороне провайдера) nonce извне.
 *   - Проверка серверного PID: реализована best-effort через
 *     GetNamedPipeServerProcessId и выставляется в HelloResult.server_pid;
 *     сверку с ожидаемым PID делает Phase 5, передав его провайдеру/клиенту
 *     (см. TODO ниже и заметки Phase 5).
 */

#include <windows.h>

#include <cstdint>
#include <string>

#include "shared/auth_broker/AuthBrokerProtocol.h"
#include "../../domain/ports/IConnectionMonitor.h"  // domain::ports::ILogSink, LogLevel

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Исход одной IPC-операции клиента (никогда не выражается исключением).
 */
enum class BrokerCallResult {
    Ok = 0,        //!< Операция успешна; полезная нагрузка заполнена.
    NotConnected,  //!< Нет активного соединения с helper'ом.
    Timeout,       //!< Операция не уложилась в helper_timeout_ms.
    IoError,       //!< Ошибка WriteFile/ReadFile/overlapped.
    TooLarge,      //!< Сообщение превысило kMaxMessageSize.
    ParseError,    //!< Ответ не разобрался (malformed / версия / тип).
    ProtocolError  //!< Неожиданный тип сообщения (напр. не hello первым).
};

inline const char* ToString(BrokerCallResult r) {
    switch (r) {
        case BrokerCallResult::Ok:            return "ok";
        case BrokerCallResult::NotConnected:  return "not_connected";
        case BrokerCallResult::Timeout:       return "timeout";
        case BrokerCallResult::IoError:       return "io_error";
        case BrokerCallResult::TooLarge:      return "too_large";
        case BrokerCallResult::ParseError:    return "parse_error";
        case BrokerCallResult::ProtocolError: return "protocol_error";
    }
    return "unknown";
}

/**
 * @brief Результат подключения + чтения первичного `hello` от helper'а.
 *
 * BrokeredAuthProvider сверяет session_id и nonce с ожидаемыми (§3.2). server_pid
 * заполняется best-effort (0, если недоступно); сверку с launched-PID выполнит
 * Phase 5.
 */
struct HelloResult {
    BrokerCallResult result = BrokerCallResult::NotConnected;
    shared::auth_broker::HelloMessage hello;  //!< Разобранный hello (при Ok).
    DWORD server_pid = 0;                      //!< PID серверного процесса pipe (0 = неизв.).
};

/**
 * @brief Клиент named-pipe брокера токенов (service-side).
 */
class AuthBrokerClient {
public:
    /**
     * @param sessionId       Целевая WTS-сессия (имя pipe = MakeAuthPipeNameW).
     * @param helperTimeoutMs Общий таймаут на connect и на каждый read/write (мс).
     *                        <= 0 => используется kDefaultHelperTimeoutMs.
     * @param logSink         Необязательный лог-приёмник (может быть nullptr).
     */
    AuthBrokerClient(std::uint32_t sessionId,
                     int helperTimeoutMs,
                     domain::ports::ILogSink* logSink = nullptr) noexcept
        : m_sessionId(sessionId),
          m_timeoutMs(helperTimeoutMs > 0
                          ? static_cast<DWORD>(helperTimeoutMs)
                          : static_cast<DWORD>(shared::auth_broker::kDefaultHelperTimeoutMs)),
          m_logSink(logSink) {}

    ~AuthBrokerClient() { Close(); }

    AuthBrokerClient(const AuthBrokerClient&) = delete;
    AuthBrokerClient& operator=(const AuthBrokerClient&) = delete;

    /**
     * @brief Подключиться к helper-pipe и прочитать первичный `hello`.
     *
     * Выполняет CreateFileW с ограниченным по времени WaitNamedPipeW-циклом,
     * переключает канал в PIPE_READMODE_MESSAGE, затем читает ровно одно
     * сообщение и парсит его как HelloMessage. Best-effort определяет
     * server_pid (GetNamedPipeServerProcessId).
     *
     * ВНИМАНИЕ: сверку session_id/nonce/PID делает вызывающая сторона
     * (BrokeredAuthProvider), а не клиент. Клиент лишь доставляет hello.
     *
     * @param out  [out] Результат + разобранный hello.
     * @return true, если удалось подключиться и прочитать валидный hello.
     */
    bool Connect(HelloResult& out) {
        Close();

        const std::wstring pipeName = shared::auth_broker::MakeAuthPipeNameW(m_sessionId);
        const ULONGLONG deadline = ::GetTickCount64() + m_timeoutMs;

        HANDLE h = INVALID_HANDLE_VALUE;
        for (;;) {
            h = ::CreateFileW(
                pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,             // без разделения — эксклюзивный клиентский конец.
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,  // overlapped => bounded read/write.
                nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                break;
            }

            const DWORD err = ::GetLastError();
            if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
                Log(domain::LogLevel::Warn,
                    "connect failed (CreateFileW err=" + std::to_string(err) + ")");
                out.result = BrokerCallResult::IoError;
                return false;
            }

            const ULONGLONG now = ::GetTickCount64();
            if (now >= deadline) {
                Log(domain::LogLevel::Warn, "connect timed out waiting for helper pipe");
                out.result = BrokerCallResult::Timeout;
                return false;
            }
            const DWORD remaining = static_cast<DWORD>(deadline - now);
            // WaitNamedPipe ждёт появления/освобождения инстанса; на
            // ERROR_FILE_NOT_FOUND helper ещё не создал pipe — короткий сон.
            if (err == ERROR_PIPE_BUSY) {
                ::WaitNamedPipeW(pipeName.c_str(), remaining < 50 ? remaining : 50);
            } else {
                ::Sleep(remaining < 50 ? remaining : 50);
            }
        }

        // Переключить в message-mode (README/§2.1: PIPE_READMODE_MESSAGE).
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!::SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) {
            Log(domain::LogLevel::Warn,
                "SetNamedPipeHandleState(MESSAGE) failed err=" +
                    std::to_string(::GetLastError()));
            ::CloseHandle(h);
            out.result = BrokerCallResult::IoError;
            return false;
        }

        m_pipe = h;
        m_deadlineForConnect = deadline;

        // Best-effort: PID серверного процесса (для Phase 5 сверки).
        DWORD serverPid = 0;
        if (::GetNamedPipeServerProcessId(h, &serverPid)) {
            out.server_pid = serverPid;
            m_serverPid = serverPid;
        }
        // TODO(Phase 5): передать ожидаемый launched-PID и сверить с m_serverPid
        //                здесь (жёсткий отказ при несовпадении).

        // Прочитать первичный hello (в пределах оставшегося таймаута).
        std::string raw;
        const DWORD helloTimeout = RemainingMs(deadline);
        const BrokerCallResult rr = ReadOneMessage(raw, helloTimeout);
        if (rr != BrokerCallResult::Ok) {
            Log(domain::LogLevel::Warn,
                std::string("failed to read hello: ") + ToString(rr));
            out.result = rr;
            Close();
            return false;
        }

        shared::auth_broker::HelloMessage hello;
        const shared::auth_broker::ParseError pe =
            shared::auth_broker::ParseHelloMessage(raw, hello);
        if (pe != shared::auth_broker::ParseError::Ok) {
            Log(domain::LogLevel::Warn,
                std::string("hello parse error: ") + shared::auth_broker::ToString(pe));
            out.result = BrokerCallResult::ParseError;
            Close();
            return false;
        }

        out.hello = hello;
        out.result = BrokerCallResult::Ok;
        Log(domain::LogLevel::Debug,
            "connected to helper session=" + std::to_string(m_sessionId) +
                " server_pid=" + std::to_string(m_serverPid));
        return true;
    }

    bool IsConnected() const noexcept { return m_pipe != INVALID_HANDLE_VALUE; }

    DWORD ServerPid() const noexcept { return m_serverPid; }

    /**
     * @brief Отправить sspi_step и получить ответ.
     *
     * Один write + один read, ограниченные helper_timeout_ms. Никогда не
     * бросает: на любую ошибку возвращает соответствующий BrokerCallResult, а
     * `out` при !Ok имеет out.status == AuthStatus::Error.
     *
     * @param req  Запрос (correlation_id/spn/proxy_host/server_token/session_id).
     * @param out  [out] Разобранный ответ helper'а.
     * @return BrokerCallResult::Ok при успешном обмене и разборе.
     */
    BrokerCallResult SendStep(const shared::auth_broker::SspiStepRequest& req,
                              shared::auth_broker::SspiStepResponse& out) {
        out = shared::auth_broker::SspiStepResponse{};
        out.status = shared::auth_broker::AuthStatus::Error;

        if (!IsConnected()) return BrokerCallResult::NotConnected;

        std::string frame;
        if (!shared::auth_broker::FrameMessage(shared::auth_broker::Serialize(req), frame)) {
            return BrokerCallResult::TooLarge;
        }

        // Каждый шаг получает свежий бюджет времени helper_timeout_ms.
        const ULONGLONG deadline = ::GetTickCount64() + m_timeoutMs;

        BrokerCallResult wr = WriteOneMessage(frame, RemainingMs(deadline));
        if (wr != BrokerCallResult::Ok) {
            Log(domain::LogLevel::Warn,
                std::string("SendStep write failed: ") + ToString(wr));
            return wr;
        }

        std::string raw;
        BrokerCallResult rr = ReadOneMessage(raw, RemainingMs(deadline));
        if (rr != BrokerCallResult::Ok) {
            Log(domain::LogLevel::Warn,
                std::string("SendStep read failed: ") + ToString(rr));
            return rr;
        }

        const shared::auth_broker::ParseError pe =
            shared::auth_broker::ParseSspiStepResponse(raw, out);
        if (pe != shared::auth_broker::ParseError::Ok) {
            Log(domain::LogLevel::Warn,
                std::string("SendStep parse error: ") + shared::auth_broker::ToString(pe));
            out.status = shared::auth_broker::AuthStatus::Error;
            return BrokerCallResult::ParseError;
        }
        return BrokerCallResult::Ok;
    }

    /**
     * @brief Отправить release (best-effort; ответа не ждём).
     *
     * Helper также GC'ит контексты по TTL (§2.3), так что потеря release не течёт
     * память. Возвращает результат ТОЛЬКО записи (read не выполняется).
     */
    BrokerCallResult SendRelease(const std::string& correlationId) {
        if (!IsConnected()) return BrokerCallResult::NotConnected;

        shared::auth_broker::ReleaseMessage msg;
        msg.correlation_id = correlationId;

        std::string frame;
        if (!shared::auth_broker::FrameMessage(shared::auth_broker::Serialize(msg), frame)) {
            return BrokerCallResult::TooLarge;
        }

        const ULONGLONG deadline = ::GetTickCount64() + m_timeoutMs;
        const BrokerCallResult wr = WriteOneMessage(frame, RemainingMs(deadline));
        if (wr != BrokerCallResult::Ok) {
            Log(domain::LogLevel::Debug,
                std::string("SendRelease write failed (best-effort): ") + ToString(wr));
        }
        return wr;
    }

    /**
     * @brief Закрыть pipe (RAII; безопасно многократно).
     */
    void Close() noexcept {
        if (m_pipe != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(m_pipe, nullptr);
            ::CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
    }

private:
    // -- Остаток времени до дедлайна (мс), не меньше нуля. --------------------
    static DWORD RemainingMs(ULONGLONG deadline) noexcept {
        const ULONGLONG now = ::GetTickCount64();
        return now >= deadline ? 0u : static_cast<DWORD>(deadline - now);
    }

    // -- Один overlapped write целиком, с таймаутом. -------------------------
    BrokerCallResult WriteOneMessage(const std::string& frame, DWORD timeoutMs) {
        if (frame.size() > shared::auth_broker::kMaxMessageSize) {
            return BrokerCallResult::TooLarge;
        }
        OVERLAPPED ov = {};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return BrokerCallResult::IoError;

        BrokerCallResult result = BrokerCallResult::Ok;
        DWORD written = 0;
        BOOL ok = ::WriteFile(m_pipe, frame.data(),
                              static_cast<DWORD>(frame.size()), &written, &ov);
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_IO_PENDING) {
                const DWORD w = ::WaitForSingleObject(ov.hEvent, timeoutMs);
                if (w == WAIT_OBJECT_0) {
                    if (!::GetOverlappedResult(m_pipe, &ov, &written, FALSE)) {
                        result = BrokerCallResult::IoError;
                    }
                } else {
                    ::CancelIoEx(m_pipe, &ov);
                    ::GetOverlappedResult(m_pipe, &ov, &written, TRUE);  // drain
                    result = (w == WAIT_TIMEOUT) ? BrokerCallResult::Timeout
                                                 : BrokerCallResult::IoError;
                }
            } else {
                result = BrokerCallResult::IoError;
            }
        }
        if (result == BrokerCallResult::Ok && written != frame.size()) {
            result = BrokerCallResult::IoError;
        }
        ::CloseHandle(ov.hEvent);
        return result;
    }

    // -- Одно overlapped-сообщение (message-mode) целиком, с таймаутом. ------
    BrokerCallResult ReadOneMessage(std::string& out, DWORD timeoutMs) {
        out.clear();
        // Буфер размером с лимит протокола: в message-mode один ReadFile == одно
        // сообщение; при переполнении вернётся ERROR_MORE_DATA => TooLarge.
        std::string buf;
        buf.resize(shared::auth_broker::kMaxMessageSize);

        OVERLAPPED ov = {};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return BrokerCallResult::IoError;

        BrokerCallResult result = BrokerCallResult::Ok;
        DWORD read = 0;
        BOOL ok = ::ReadFile(m_pipe, &buf[0],
                             static_cast<DWORD>(buf.size()), &read, &ov);
        DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();

        if (!ok && err == ERROR_IO_PENDING) {
            const DWORD w = ::WaitForSingleObject(ov.hEvent, timeoutMs);
            if (w == WAIT_OBJECT_0) {
                if (!::GetOverlappedResult(m_pipe, &ov, &read, FALSE)) {
                    err = ::GetLastError();
                    result = (err == ERROR_MORE_DATA) ? BrokerCallResult::TooLarge
                                                      : BrokerCallResult::IoError;
                } else {
                    err = ERROR_SUCCESS;
                }
            } else {
                ::CancelIoEx(m_pipe, &ov);
                ::GetOverlappedResult(m_pipe, &ov, &read, TRUE);  // drain
                result = (w == WAIT_TIMEOUT) ? BrokerCallResult::Timeout
                                             : BrokerCallResult::IoError;
            }
        } else if (!ok) {
            result = (err == ERROR_MORE_DATA) ? BrokerCallResult::TooLarge
                                              : BrokerCallResult::IoError;
        }

        if (result == BrokerCallResult::Ok) {
            if (read == 0) {
                result = BrokerCallResult::IoError;  // пустое сообщение / закрытие.
            } else {
                buf.resize(read);
                if (!shared::auth_broker::IsFrameWithinLimit(buf)) {
                    result = BrokerCallResult::TooLarge;
                } else {
                    out = std::move(buf);
                }
            }
        }
        ::CloseHandle(ov.hEvent);
        return result;
    }

    void Log(domain::LogLevel level, const std::string& msg) const {
        if (m_logSink) {
            m_logSink->Log(level, "authbroker", msg);
        }
    }

    std::uint32_t          m_sessionId;
    DWORD                  m_timeoutMs;
    domain::ports::ILogSink* m_logSink = nullptr;
    HANDLE                 m_pipe = INVALID_HANDLE_VALUE;
    DWORD                  m_serverPid = 0;
    ULONGLONG              m_deadlineForConnect = 0;
};

} // namespace infrastructure
} // namespace tcp_redirector
