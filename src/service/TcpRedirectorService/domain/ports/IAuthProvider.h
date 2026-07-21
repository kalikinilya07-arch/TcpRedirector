#pragma once

/**
 * @file IAuthProvider.h
 * @brief Доменный порт для провайдера многошаговой Negotiate-аутентификации.
 *
 * Абстрагирует получение токенов Proxy-Authorization: Negotiate от конкретного
 * механизма их производства. На сегодня (Variant 4b, Phase 1) существует ровно
 * одна реализация — infrastructure::LocalSspiProvider, оборачивающая
 * infrastructure::SspiNegotiate под машинным аккаунтом (LocalSystem). Поведение
 * relay при этом байт-в-байт совпадает с прежним прямым вызовом SspiNegotiate.
 *
 * Позже (Phase 4) появится infrastructure::BrokeredAuthProvider, который будет
 * реализовывать ЭТОТ ЖЕ интерфейс, но получать токены от per-user helper-процесса
 * через named-pipe брокер. relay при этом менять не придётся — он работает только
 * через IAuthProvider. См. plans/kerberos_per_user_auth_helper_plan.md §4.1.
 *
 * Модель состояния:
 *   - IAuthProvider — это ОДИН многошаговый handshake на ОДНО соединение relay.
 *     Он держит внутри всё необходимое per-connection-состояние (для локальной
 *     реализации это infrastructure::SspiContext; для брокера — correlation_id).
 *   - Экземпляр создаётся фабрикой IAuthProviderFactory::Create() в начале
 *     ConnectionHandler и уничтожается при выходе из него (RAII), что 1:1
 *     повторяет прежнее время жизни SspiContext на стеке.
 *   - Идентичность соединения (ConnectionIdentity) передаётся в фабрику при
 *     создании. Локальный провайдер её игнорирует (машинный аккаунт), брокерный
 *     будет использовать для выбора helper'а нужной сессии (Phase 4/6).
 */

#include <string>
#include <memory>
#include <cstdint>

namespace tcp_redirector {
namespace domain {
namespace ports {

/**
 * @brief Статус одного шага Negotiate-handshake.
 *
 * Отражает исход IAuthProvider::NextToken. Значения намеренно повторяют
 * семантику infrastructure::SspiResult, чтобы relay-логика отката осталась
 * прежней:
 *   - Complete       ↔ SspiResult::Complete / Success (контекст готов);
 *   - ContinueNeeded ↔ SspiResult::NeedContinue (сервер пришлёт ещё challenge);
 *   - NoCredentials  ↔ SspiResult::NoCredentials (нет Kerberos/NTLM-билета);
 *   - Failed         ↔ SspiResult::Error (внутренняя ошибка).
 */
enum class AuthStepStatus {
    Complete,        //!< Токен получен, аутентификация завершена.
    ContinueNeeded,  //!< Токен получен, ожидается следующий challenge (407).
    NoCredentials,   //!< Учётные данные недоступны (нет билета). См. §5 fallback.
    Failed           //!< Внутренняя ошибка провайдера.
};

/**
 * @brief Идентичность соединения relay для выбора источника токена.
 *
 * На Phase 1 поля заполняются тем, что уже доступно в ConnectionHandler, и
 * ЛОКАЛЬНЫМ провайдером не используются. Структура введена заранее, чтобы
 * брокерный провайдер (Phase 4/6) мог по этим данным резолвить PID→session→SID
 * и выбрать helper нужной пользовательской сессии, БЕЗ изменения сигнатуры
 * интерфейса в будущем.
 *
 * См. plans/kerberos_per_user_auth_helper_plan.md §1.3.
 */
struct ConnectionIdentity {
    uint16_t client_port = 0;      //!< Эфемерный локальный порт клиента (ключ ConnectionTable).
    uint32_t orig_dest_ip = 0;     //!< Оригинальный IPv4 назначения (network byte order).
    uint16_t orig_dest_port = 0;   //!< Оригинальный порт назначения (host byte order).
    uint32_t proxy_config_id = 0;  //!< Идентификатор конфигурации прокси.
};

/**
 * @brief Провайдер многошаговой Negotiate-аутентификации (порт).
 *
 * Один экземпляр обслуживает один handshake одного соединения relay и хранит
 * всё промежуточное состояние внутри. Не потокобезопасен: экземпляр создаётся
 * и используется в пределах одного ConnectionHandler-потока.
 */
class IAuthProvider {
public:
    virtual ~IAuthProvider() = default;

    /**
     * @brief Выполнить один шаг Negotiate-handshake.
     *
     * Первый вызов (serverToken пуст) производит начальный токен для заголовка
     * Proxy-Authorization: Negotiate <outToken>. Последующие вызовы получают
     * challenge, извлечённый из 407-ответа (Proxy-Authenticate: Negotiate ...),
     * и возвращают следующий токен.
     *
     * Точное соответствие прежним вызовам infrastructure::SspiNegotiate:
     *   - первый шаг заменяет TcpRelayServer.h:482;
     *   - шаг после 407 заменяет TcpRelayServer.h:616.
     *
     * @param spn          SPN целевого прокси (обычно "HTTP/<host>").
     * @param serverToken  Challenge от сервера (base64); пустая строка на первом шаге.
     * @param outToken     [out] Исходящий токен (base64) для Proxy-Authorization.
     * @return AuthStepStatus  Исход шага.
     */
    virtual AuthStepStatus NextToken(const std::string& spn,
                                     const std::string& serverToken,
                                     std::string& outToken) = 0;

    /**
     * @brief Признак «drop-семантики» для этого провайдера (Variant 4b, Phase 7).
     *
     * Возвращает true для провайдеров per-user-пути (BrokeredAuthProvider,
     * DropAuthProvider), у которых AuthStepStatus::Failed ДОЛЖЕН приводить к
     * жёсткому закрытию соединения (drop), НИКОГДА не откатываясь на машинную
     * (LocalSystem) SSPI-аутентификацию. См. §5 fallback_policy=drop/error.
     *
     * Для legacy-провайдера infrastructure::LocalSspiProvider возвращает false —
     * его обработка Failed/NoCredentials в relay остаётся прежней (обратная
     * совместимость: SSPI отключается на соединении и CONNECT идёт без Negotiate).
     *
     * relay использует этот флаг, чтобы при per-user-пути трактовать Failed как
     * терминальный drop, а при legacy-пути — как прежде. Значение по умолчанию
     * false, чтобы существующие реализации не меняли поведение.
     */
    virtual bool DropOnFailure() const { return false; }
};

/**
 * @brief Фабрика провайдеров аутентификации (порт).
 *
 * relay получает фабрику один раз (через сеттер) и вызывает Create() на каждое
 * соединение, требующее Negotiate-аутентификации. На Phase 1 фабрика всегда
 * возвращает infrastructure::LocalSspiProvider (машинный аккаунт). На Phase 4/6
 * фабрика будет выбирать LocalSspiProvider или BrokeredAuthProvider по конфигу
 * (per_user_auth_enabled) и доступности helper'а для сессии.
 */
class IAuthProviderFactory {
public:
    virtual ~IAuthProviderFactory() = default;

    /**
     * @brief Создать провайдер для одного соединения.
     * @param identity  Идентичность соединения (для брокерного выбора; локальным
     *                  провайдером игнорируется).
     * @return Уникальный владеющий указатель на провайдер (живёт время handshake).
     */
    virtual std::unique_ptr<IAuthProvider> Create(const ConnectionIdentity& identity) = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector
