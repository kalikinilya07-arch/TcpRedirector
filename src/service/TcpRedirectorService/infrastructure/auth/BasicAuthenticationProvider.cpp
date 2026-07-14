#include "BasicAuthenticationProvider.h"

namespace tcp_redirector {
namespace infrastructure {

domain::ports::CreateContextResult BasicAuthenticationProvider::CreateContext(
    std::string_view /*spn*/) {

    domain::ports::CreateContextResult result{};
    result.success = true;
    result.context_id = 0;  // Basic не использует контексты
    result.token = Base64Encode(m_username + ":" + m_password);
    result.needs_continue = false;  // Basic не многошаговый
    return result;
}

domain::ports::ContinueContextResult BasicAuthenticationProvider::ContinueContext(
    uint64_t /*context_id*/,
    std::string_view /*challenge*/) {

    // Basic Auth не поддерживает многошаговую аутентификацию
    domain::ports::ContinueContextResult result{};
    result.success = false;
    result.error_message = "Basic auth does not support multi-step negotiation";
    return result;
}

void BasicAuthenticationProvider::CloseContext(uint64_t /*context_id*/) {
    // Basic не использует контексты — no-op
}

void BasicAuthenticationProvider::Reset() {
    // Нечего сбрасывать
}

std::string BasicAuthenticationProvider::Base64Encode(const std::string& input) {
    static const char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);

    size_t i = 0;
    unsigned char a3[3];
    for (auto c : input) {
        a3[i++] = static_cast<unsigned char>(c);
        if (i == 3) {
            result += kBase64[(a3[0] & 0xfc) >> 2];
            result += kBase64[((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4)];
            result += kBase64[((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6)];
            result += kBase64[a3[2] & 0x3f];
            i = 0;
        }
    }

    if (i > 0) {
        for (size_t j = i; j < 3; ++j) a3[j] = '\0';
        result += kBase64[(a3[0] & 0xfc) >> 2];
        result += kBase64[((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4)];
        if (i == 2) {
            result += kBase64[((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6)];
        } else {
            result += '=';
        }
        result += '=';
    }

    return result;
}

} // namespace infrastructure
} // namespace tcp_redirector