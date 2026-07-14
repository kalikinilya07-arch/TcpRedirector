#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace tcp_redirector {
namespace domain {
namespace ports {

/// Тип провайдера аутентификации.
enum class AuthProviderType {
    KerberosAgent,  // через AuthAgent.exe (пользовательская сессия)
    Basic           // Basic Authentication (логин:пароль)
};

/// Результат создания SSPI-контекста.
/// Контекст живёт ровно один HTTP CONNECT.
struct CreateContextResult {
    bool success;
    uint64_t context_id;        // непрозрачный ID, живёт в AuthAgent
    std::string token;          // начальный SSPI-токен (Base64)
    bool needs_continue;        // true если ожидается 407 + ContinueContext
    std::string error_message;  // при success=false
};

/// Результат продолжения SSPI-контекста (после 407).
struct ContinueContextResult {
    bool success;
    std::string token;          // следующий SSPI-токен (Base64)
    bool needs_continue;        // true если нужен ещё один раунд Negotiate
    std::string error_message;
};

/// Абстракция аутентификации для HTTP CONNECT к прокси.
///
/// Жизненный цикл контекста (ОДНОРАЗОВЫЙ):
///   CreateContext(spn) → [ContinueContext(id, challenge)]* → CloseContext(id)
///
/// Каждый ContextId живёт ровно один HTTP CONNECT.
/// После успеха (200) или ошибки — обязательно CloseContext().
/// Повторное использование ContextId запрещено.
///
/// Скрывает от TcpRelayServer детали получения токенов
/// (SSPI, Kerberos, Basic Auth).
class IAuthenticationProvider {
public:
    virtual ~IAuthenticationProvider() = default;

    /// Создать новый SSPI-контекст для заданного SPN.
    /// Вызывается один раз в начале каждого HTTP CONNECT.
    /// @param spn Service Principal Name (например "HTTP/proxy.corp.local")
    virtual CreateContextResult CreateContext(std::string_view spn) = 0;

    /// Продолжить SSPI-контекст с challenge от прокси (ответ 407).
    /// @param context_id ID, полученный из CreateContext
    /// @param challenge Base64 challenge из заголовка Proxy-Authenticate
    virtual ContinueContextResult ContinueContext(
        uint64_t context_id,
        std::string_view challenge) = 0;

    /// Закрыть SSPI-контекст. Вызывается после успешного CONNECT или ошибки.
    /// После вызова context_id становится невалидным.
    virtual void CloseContext(uint64_t context_id) = 0;

    /// Сбросить все активные контексты (при остановке службы).
    virtual void Reset() = 0;

    /// Тип провайдера.
    virtual AuthProviderType GetType() const = 0;
};

} // namespace ports
} // namespace domain
} // namespace tcp_redirector