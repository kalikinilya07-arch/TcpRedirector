#pragma once

/**
 * @file AuthBrokerProtocol.h
 * @brief Общий (shared) заголовок протокола IPC-брокера токенов SSPI/Negotiate.
 *
 * Variant 4b, Phase 2. См. plans/kerberos_per_user_auth_helper_plan.md §2 (IPC
 * design) и §3 (security model).
 *
 * Этот заголовок — ЕДИНЫЙ ИСТОЧНИК ИСТИНЫ для схемы обмена между:
 *   - LocalSystem-службой TcpRedirectorService.exe (роль КЛИЕНТА named-pipe), и
 *   - per-user helper'ом TcpRedirectorAuthHelper.exe (роль СЕРВЕРА named-pipe).
 *
 * Он содержит ТОЛЬКО данные протокола + (де)сериализацию + вспомогательные
 * функции именования/фрейминга. Здесь НЕТ реализации pipe/сети/SSPI/сессий —
 * это фазы 3-5. Заголовок должен подключаться из ОБОИХ контекстов, поэтому его
 * зависимости минимальны: nlohmann/json + STL + Win32 только для типов имени
 * pipe. Никакой связи с внутренними классами службы.
 *
 * ── Соответствие фаз ─────────────────────────────────────────────────────────
 *   Phase 1 определил domain::ports::AuthStepStatus {Complete, ContinueNeeded,
 *   NoCredentials, Failed} (см. domain/ports/IAuthProvider.h). Протокольный
 *   статус (AuthStatus, ниже) отображается на него функцией ToAuthStepStatus().
 *   BrokeredAuthProvider (Phase 4) выполнит это отображение на реальном приёме.
 *
 * ── Транспорт (Phase 3+, здесь только константы/имена/фрейминг) ───────────────
 *   - Named pipe, по одному на сессию: \\.\pipe\TcpRedirectorAuth_<sessionId>.
 *   - Режим PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE (как в PipeServer.h): один
 *     JSON-документ == одно сообщение; на приёме один ReadFile == одно сообщение.
 *   - Фрейминг: сообщение — это UTF-8 JSON-документ БЕЗ завершающего символа
 *     (message-mode pipe сам держит границы). См. FrameMessage()/наличие
 *     MaxMessageSize для защиты от превышения буфера.
 *   - Один request → один response. Helper дополнительно шлёт `hello` при
 *     подключении клиента (§2.1).
 *
 * Разработчик: Phase 2 (broker protocol library).
 */

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>  // DWORD (session id тип). Только для типов имени pipe.
#endif

namespace tcp_redirector {
namespace shared {
namespace auth_broker {

// ============================================================================
// Константы протокола
// ============================================================================

/// Версия wire-протокола. Присутствует в каждом сообщении как поле "v".
/// Несовпадение версии на приёме => AuthStatus::Error (см. ParseResponse).
inline constexpr int kProtocolVersion = 1;

/// Максимальный размер одного сериализованного сообщения (байт). Совпадает с
/// размером буфера pipe в PipeServer.h (65536). И клиент, и сервер ДОЛЖНЫ
/// отклонять сообщения больше этого значения (см. FrameMessage / приёмная
/// сторона в фазах 3-4).
inline constexpr std::size_t kMaxMessageSize = 65536;

/// Ссылочное значение таймаута брокер-вызова по умолчанию (мс). Реальное
/// значение приходит из конфига auth.helper_timeout_ms (см. план §2.4); это —
/// лишь дефолт-референс для клиентской стороны, если конфиг не задан.
inline constexpr std::uint32_t kDefaultHelperTimeoutMs = 4000;

/// Префикс имени per-session named-pipe (без "\\.\pipe\").
inline constexpr const char* kPipeBaseName = "TcpRedirectorAuth_";

/// Полный префикс с локальным pipe-namespace для конструирования имени.
inline constexpr const char* kPipeNamePrefix = "\\\\.\\pipe\\TcpRedirectorAuth_";
inline constexpr const wchar_t* kPipeNamePrefixW = L"\\\\.\\pipe\\TcpRedirectorAuth_";

// ---- Строковые литералы операций (поле "op") -------------------------------
inline constexpr const char* kOpHello    = "hello";
inline constexpr const char* kOpSspiStep = "sspi_step";
inline constexpr const char* kOpRelease  = "release";
inline constexpr const char* kOpPing     = "ping";

// ============================================================================
// Именование pipe (§2.1)
// ============================================================================

/**
 * @brief Построить полное имя per-session named-pipe (ASCII/UTF-8).
 *
 * Формат: \\.\pipe\TcpRedirectorAuth_<sessionId>. Session id в имени исключает
 * коллизии между одновременными RDP-пользователями (план §2.1).
 *
 * @param sessionId  Идентификатор Windows-сессии (WTS session id).
 * @return std::string  Полное имя pipe.
 */
inline std::string MakeAuthPipeName(std::uint32_t sessionId) {
    return std::string(kPipeNamePrefix) + std::to_string(sessionId);
}

/**
 * @brief Wide-версия MakeAuthPipeName (для CreateNamedPipeW / CreateFileW).
 */
inline std::wstring MakeAuthPipeNameW(std::uint32_t sessionId) {
    return std::wstring(kPipeNamePrefixW) + std::to_wstring(sessionId);
}

#if defined(_WIN32)
/**
 * @brief Перегрузка для Win32 DWORD (тип, возвращаемый ProcessIdToSessionId).
 */
inline std::string MakeAuthPipeName(DWORD sessionId) {
    return MakeAuthPipeName(static_cast<std::uint32_t>(sessionId));
}
inline std::wstring MakeAuthPipeNameW(DWORD sessionId) {
    return MakeAuthPipeNameW(static_cast<std::uint32_t>(sessionId));
}
#endif

// ============================================================================
// Протокольный статус ответа (§2.2) и отображение на Phase 1 AuthStepStatus
// ============================================================================

/**
 * @brief Статус ответа helper'а на sspi_step (поле "status").
 *
 * Wire-значения (строки) намеренно совпадают со схемой плана §2.2:
 *   "continue" | "complete" | "no_credentials" | "denied" | "error".
 *
 * Отображение на domain::ports::AuthStepStatus (Phase 1) — см.
 * ToAuthStepStatus() и таблицу в attempt_completion:
 *
 *   ┌────────────────┬──────────────────────────┬──────────────────────────────┐
 *   │ AuthStatus     │ wire "status"            │ AuthStepStatus (Phase 1)      │
 *   ├────────────────┼──────────────────────────┼──────────────────────────────┤
 *   │ Continue       │ "continue"               │ ContinueNeeded                │
 *   │ Complete       │ "complete"               │ Complete                      │
 *   │ NoCredentials  │ "no_credentials"         │ NoCredentials                 │
 *   │ Denied         │ "denied"                 │ Failed  (жёсткий отказ, WARN) │
 *   │ Error          │ "error"                  │ Failed                        │
 *   └────────────────┴──────────────────────────┴──────────────────────────────┘
 *
 * Denied и Error оба маппятся в Failed, но семантически различны: Denied — SPN
 * вне allow-list (никогда не откатываться молча на машинную аутентификацию,
 * план §2.4/§3.3); Error — внутренняя ошибка/таймаут/malformed. Различие
 * сохраняется на уровне AuthStatus для логирования на стороне провайдера.
 */
enum class AuthStatus {
    Continue,       //!< "continue"       — есть токен, ждём следующий challenge.
    Complete,       //!< "complete"       — handshake завершён.
    NoCredentials,  //!< "no_credentials" — нет Kerberos/NTLM-билета у пользователя.
    Denied,         //!< "denied"         — SPN вне allow-list (жёсткий отказ).
    Error           //!< "error"          — внутренняя ошибка / malformed / timeout.
};

/// AuthStatus -> wire-строка.
inline const char* ToWire(AuthStatus s) {
    switch (s) {
        case AuthStatus::Continue:      return "continue";
        case AuthStatus::Complete:      return "complete";
        case AuthStatus::NoCredentials: return "no_credentials";
        case AuthStatus::Denied:        return "denied";
        case AuthStatus::Error:         return "error";
    }
    return "error";
}

/// wire-строка -> AuthStatus. Неизвестное значение => AuthStatus::Error (никогда
/// не бросает).
inline AuthStatus AuthStatusFromWire(const std::string& s) {
    if (s == "continue")       return AuthStatus::Continue;
    if (s == "complete")       return AuthStatus::Complete;
    if (s == "no_credentials") return AuthStatus::NoCredentials;
    if (s == "denied")         return AuthStatus::Denied;
    if (s == "error")          return AuthStatus::Error;
    return AuthStatus::Error;
}

/**
 * @brief Зеркало значений domain::ports::AuthStepStatus (Phase 1).
 *
 * Продублировано ЛОКАЛЬНО намеренно: shared-заголовок не должен зависеть от
 * внутренних заголовков службы (domain/ports/IAuthProvider.h), чтобы helper EXE
 * мог включать его без domain-слоя. Значения и порядок ОБЯЗАНЫ совпадать с
 * enum class AuthStepStatus в IAuthProvider.h. BrokeredAuthProvider (Phase 4)
 * приводит один к другому тривиальным static_cast/switch.
 *
 * !!! При изменении AuthStepStatus в IAuthProvider.h синхронизировать здесь. !!!
 */
enum class MappedAuthStepStatus {
    Complete,        //!< == domain::ports::AuthStepStatus::Complete
    ContinueNeeded,  //!< == domain::ports::AuthStepStatus::ContinueNeeded
    NoCredentials,   //!< == domain::ports::AuthStepStatus::NoCredentials
    Failed           //!< == domain::ports::AuthStepStatus::Failed
};

/**
 * @brief Отобразить протокольный AuthStatus на Phase 1 AuthStepStatus.
 *
 * Документированное отображение (см. таблицу выше). Denied и Error => Failed.
 */
inline MappedAuthStepStatus ToAuthStepStatus(AuthStatus s) {
    switch (s) {
        case AuthStatus::Continue:      return MappedAuthStepStatus::ContinueNeeded;
        case AuthStatus::Complete:      return MappedAuthStepStatus::Complete;
        case AuthStatus::NoCredentials: return MappedAuthStepStatus::NoCredentials;
        case AuthStatus::Denied:        return MappedAuthStepStatus::Failed;
        case AuthStatus::Error:         return MappedAuthStepStatus::Failed;
    }
    return MappedAuthStepStatus::Failed;
}

// ============================================================================
// Результат парсинга: код ошибки декодирования (никогда не бросаем через границу)
// ============================================================================

/**
 * @brief Исход попытки разобрать входящее сообщение.
 *
 * Вся (де)сериализация НЕ бросает исключения через IPC-границу (план §2 п.2):
 * при любом malformed-входе возвращается соответствующий ParseError, а вызывающая
 * сторона трактует это как AuthStatus::Error / fallback (§2.4).
 */
enum class ParseError {
    Ok = 0,           //!< Успех.
    InvalidJson,      //!< Не разобрать как JSON (синтаксис/не объект).
    VersionMismatch,  //!< Поле "v" != kProtocolVersion.
    MissingField,     //!< Обязательное поле отсутствует.
    WrongType,        //!< Поле присутствует, но неверного типа.
    UnknownOp         //!< Поле "op" не распознано.
};

inline const char* ToString(ParseError e) {
    switch (e) {
        case ParseError::Ok:              return "ok";
        case ParseError::InvalidJson:     return "invalid_json";
        case ParseError::VersionMismatch: return "version_mismatch";
        case ParseError::MissingField:    return "missing_field";
        case ParseError::WrongType:       return "wrong_type";
        case ParseError::UnknownOp:       return "unknown_op";
    }
    return "unknown";
}

// ============================================================================
// Типы сообщений (§2.2)
// ============================================================================

/**
 * @brief hello (helper → service), отправляется при подключении клиента (§2.1).
 *
 * Несёт session_id и одноразовый nonce, который менеджер передал helper'у в
 * командной строке при запуске. Служба сверяет nonce, чтобы убедиться, что
 * говорит с тем helper'ом, которого запустила (защита от squatting'а имени
 * pipe, план §3.2).
 */
struct HelloMessage {
    int          v = kProtocolVersion;
    std::uint32_t session_id = 0;  //!< Сессия, которую обслуживает helper.
    std::string  nonce;            //!< Одноразовый секрет, выданный при запуске.
    std::string  helper_version;   //!< Опционально: версия helper.exe (диагностика).
};

/**
 * @brief sspi_step (service → helper) — запрос одного шага Negotiate (§2.2/§2.3).
 *
 * server_token пуст на первом шаге. correlation_id уникален на соединение relay
 * и переиспользуется между шагами одного handshake, чтобы helper переиспользовал
 * SspiContext (§2.3).
 */
struct SspiStepRequest {
    int          v = kProtocolVersion;
    std::string  correlation_id;  //!< "conn-<uint64>", уникален на соединение relay.
    std::string  spn;             //!< SPN цели; ОБЯЗАН быть в allow-list helper'а.
    std::string  proxy_host;      //!< Хост прокси (для деривации/логов).
    std::string  server_token;    //!< base64 challenge; пусто на первом шаге.
    std::uint32_t session_id = 0; //!< Идентичность: целевая сессия.
};

/**
 * @brief Ответ helper'а на sspi_step (helper → service) (§2.2).
 */
struct SspiStepResponse {
    int         v = kProtocolVersion;
    AuthStatus  status = AuthStatus::Error;  //!< continue/complete/no_credentials/denied/error.
    std::string out_token;                   //!< base64 исходящий Negotiate-токен (или пусто).
    std::string detail;                      //!< Опциональный человекочитаемый текст для логов.
};

/**
 * @brief release (service → helper) — освободить SspiContext по correlation_id.
 *
 * Отправляется relay в конце соединения / при ошибке / успехе (§2.3). Helper
 * также GC'ит контексты по TTL, так что потеря release не течёт память вечно.
 */
struct ReleaseMessage {
    int         v = kProtocolVersion;
    std::string correlation_id;  //!< Тот же id, что и у sspi_step.
};

// ============================================================================
// Внутренние helper'ы разбора полей (не бросают)
// ============================================================================
namespace detail {

/// Проверить, что j — объект с "v" == kProtocolVersion. Возвращает ParseError.
inline ParseError CheckEnvelope(const nlohmann::json& j) {
    if (!j.is_object()) return ParseError::InvalidJson;
    auto it = j.find("v");
    if (it == j.end()) return ParseError::MissingField;
    if (!it->is_number_integer() && !it->is_number_unsigned()) return ParseError::WrongType;
    if (it->get<int>() != kProtocolVersion) return ParseError::VersionMismatch;
    return ParseError::Ok;
}

/// Прочитать обязательную строку. false + err при отсутствии/неверном типе.
inline bool GetRequiredString(const nlohmann::json& j, const char* key,
                              std::string& out, ParseError& err) {
    auto it = j.find(key);
    if (it == j.end()) { err = ParseError::MissingField; return false; }
    if (!it->is_string()) { err = ParseError::WrongType; return false; }
    out = it->get<std::string>();
    return true;
}

/// Прочитать опциональную строку (по умолчанию пусто). Неверный тип => WrongType.
inline bool GetOptionalString(const nlohmann::json& j, const char* key,
                              std::string& out, ParseError& err) {
    auto it = j.find(key);
    if (it == j.end()) { out.clear(); return true; }
    if (it->is_null()) { out.clear(); return true; }
    if (!it->is_string()) { err = ParseError::WrongType; return false; }
    out = it->get<std::string>();
    return true;
}

/// Прочитать обязательный uint32. false + err при отсутствии/неверном типе.
inline bool GetRequiredU32(const nlohmann::json& j, const char* key,
                           std::uint32_t& out, ParseError& err) {
    auto it = j.find(key);
    if (it == j.end()) { err = ParseError::MissingField; return false; }
    if (!it->is_number_integer() && !it->is_number_unsigned()) {
        err = ParseError::WrongType; return false;
    }
    out = it->get<std::uint32_t>();
    return true;
}

/// Прочитать опциональный uint32 (по умолчанию 0). Неверный тип => WrongType.
inline bool GetOptionalU32(const nlohmann::json& j, const char* key,
                           std::uint32_t& out, ParseError& err) {
    auto it = j.find(key);
    if (it == j.end()) { out = 0; return true; }
    if (it->is_null()) { out = 0; return true; }
    if (!it->is_number_integer() && !it->is_number_unsigned()) {
        err = ParseError::WrongType; return false;
    }
    out = it->get<std::uint32_t>();
    return true;
}

}  // namespace detail

// ============================================================================
// Сериализация (никогда не бросает)
// ============================================================================

inline std::string Serialize(const HelloMessage& m) {
    nlohmann::json j;
    j["v"] = kProtocolVersion;
    j["op"] = kOpHello;
    j["session_id"] = m.session_id;
    j["nonce"] = m.nonce;
    if (!m.helper_version.empty()) j["helper_version"] = m.helper_version;
    return j.dump();
}

inline std::string Serialize(const SspiStepRequest& m) {
    nlohmann::json j;
    j["v"] = kProtocolVersion;
    j["op"] = kOpSspiStep;
    j["correlation_id"] = m.correlation_id;
    j["spn"] = m.spn;
    j["proxy_host"] = m.proxy_host;
    j["server_token"] = m.server_token;  // пусто на первом шаге — сериализуем явно.
    j["session_id"] = m.session_id;
    return j.dump();
}

inline std::string Serialize(const SspiStepResponse& m) {
    nlohmann::json j;
    j["v"] = kProtocolVersion;
    j["status"] = ToWire(m.status);
    j["out_token"] = m.out_token;
    if (!m.detail.empty()) j["detail"] = m.detail;
    return j.dump();
}

inline std::string Serialize(const ReleaseMessage& m) {
    nlohmann::json j;
    j["v"] = kProtocolVersion;
    j["op"] = kOpRelease;
    j["correlation_id"] = m.correlation_id;
    return j.dump();
}

// ============================================================================
// Разбор входящих запросов (service → helper) — helper-side
// ============================================================================

/**
 * @brief Определить операцию из сырого JSON без полного разбора тела.
 *
 * Helper-цикл использует это, чтобы выбрать, во что парсить (sspi_step / release
 * / ping). Никогда не бросает.
 *
 * @param raw   Сырое сообщение (UTF-8 JSON).
 * @param op    [out] Значение поля "op" (пусто при ошибке).
 * @return ParseError::Ok при валидном envelope + строковом "op".
 */
inline ParseError PeekOp(const std::string& raw, std::string& op) {
    op.clear();
    nlohmann::json j = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return ParseError::InvalidJson;
    ParseError e = detail::CheckEnvelope(j);
    if (e != ParseError::Ok) return e;
    ParseError se = ParseError::Ok;
    if (!detail::GetRequiredString(j, "op", op, se)) return se;
    return ParseError::Ok;
}

inline ParseError ParseSspiStepRequest(const std::string& raw, SspiStepRequest& out) {
    out = SspiStepRequest{};
    nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded()) return ParseError::InvalidJson;
    ParseError e = detail::CheckEnvelope(j);
    if (e != ParseError::Ok) return e;

    std::string op;
    if (!detail::GetRequiredString(j, "op", op, e)) return e;
    if (op != kOpSspiStep) return ParseError::UnknownOp;

    if (!detail::GetRequiredString(j, "correlation_id", out.correlation_id, e)) return e;
    if (!detail::GetRequiredString(j, "spn", out.spn, e)) return e;
    if (!detail::GetOptionalString(j, "proxy_host", out.proxy_host, e)) return e;
    if (!detail::GetOptionalString(j, "server_token", out.server_token, e)) return e;
    if (!detail::GetOptionalU32(j, "session_id", out.session_id, e)) return e;
    return ParseError::Ok;
}

inline ParseError ParseReleaseMessage(const std::string& raw, ReleaseMessage& out) {
    out = ReleaseMessage{};
    nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded()) return ParseError::InvalidJson;
    ParseError e = detail::CheckEnvelope(j);
    if (e != ParseError::Ok) return e;

    std::string op;
    if (!detail::GetRequiredString(j, "op", op, e)) return e;
    if (op != kOpRelease) return ParseError::UnknownOp;

    if (!detail::GetRequiredString(j, "correlation_id", out.correlation_id, e)) return e;
    return ParseError::Ok;
}

// ============================================================================
// Разбор входящих ответов/hello (helper → service) — service-side
// ============================================================================

inline ParseError ParseHelloMessage(const std::string& raw, HelloMessage& out) {
    out = HelloMessage{};
    nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded()) return ParseError::InvalidJson;
    ParseError e = detail::CheckEnvelope(j);
    if (e != ParseError::Ok) return e;

    std::string op;
    if (!detail::GetRequiredString(j, "op", op, e)) return e;
    if (op != kOpHello) return ParseError::UnknownOp;

    if (!detail::GetRequiredU32(j, "session_id", out.session_id, e)) return e;
    if (!detail::GetRequiredString(j, "nonce", out.nonce, e)) return e;
    if (!detail::GetOptionalString(j, "helper_version", out.helper_version, e)) return e;
    return ParseError::Ok;
}

/**
 * @brief Разобрать ответ на sspi_step.
 *
 * ВАЖНО (§2 п.2): при любом malformed-входе поле out.status выставляется в
 * AuthStatus::Error, а возвращаемый ParseError описывает причину. Вызывающая
 * сторона (BrokeredAuthProvider) трактует всё, кроме Ok, как Error/fallback,
 * не полагаясь на исключения.
 */
inline ParseError ParseSspiStepResponse(const std::string& raw, SspiStepResponse& out) {
    out = SspiStepResponse{};
    out.status = AuthStatus::Error;  // безопасный дефолт на случай раннего выхода.

    nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded()) return ParseError::InvalidJson;
    ParseError e = detail::CheckEnvelope(j);
    if (e != ParseError::Ok) return e;

    std::string statusStr;
    if (!detail::GetRequiredString(j, "status", statusStr, e)) return e;
    out.status = AuthStatusFromWire(statusStr);

    if (!detail::GetOptionalString(j, "out_token", out.out_token, e)) return e;
    if (!detail::GetOptionalString(j, "detail", out.detail, e)) return e;
    return ParseError::Ok;
}

// ============================================================================
// Фрейминг (§2.1)
// ============================================================================

/**
 * @brief Подготовить сериализованное сообщение к отправке по message-mode pipe.
 *
 * Поскольку pipe работает в PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE (как в
 * PipeServer.h), границы сообщений держит сам pipe: один WriteFile == одно
 * сообщение, завершающий разделитель НЕ добавляется (в точности как GUI-канал в
 * PipeServer.h, где WriteFile пишет msg.data()/msg.size() без '\n').
 *
 * Единственная работа фрейминга здесь — проверка размера: сообщение больше
 * kMaxMessageSize отклоняется (возвращается false), чтобы приёмный буфер
 * (65536, как в PipeServer.h) никогда не переполнялся.
 *
 * @param payload  Сериализованное JSON-сообщение (из Serialize()).
 * @param out      [out] Кадр для WriteFile (идентичен payload при успехе).
 * @return true, если payload помещается в лимит; иначе false (out очищается).
 */
inline bool FrameMessage(const std::string& payload, std::string& out) {
    if (payload.size() > kMaxMessageSize) {
        out.clear();
        return false;
    }
    out = payload;
    return true;
}

/**
 * @brief Проверить, что принятый кадр не превышает лимит протокола.
 *
 * Симметрично FrameMessage для приёмной стороны: приём кадра больше
 * kMaxMessageSize трактуется как ошибка протокола до попытки JSON-разбора.
 *
 * @param frame  Принятые байты одного сообщения.
 * @return true, если размер в пределах лимита.
 */
inline bool IsFrameWithinLimit(const std::string& frame) {
    return frame.size() <= kMaxMessageSize;
}

}  // namespace auth_broker
}  // namespace shared
}  // namespace tcp_redirector
