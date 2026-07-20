/**
 * @file auth_sspi.cpp
 * @brief Реализация SSPI-аутентификации (Negotiate/Kerberos) для HTTP-прокси.
 *
 * Алгоритм:
 *
 * 1. AcquireCredentialsHandle(Negotiate) — получаем учётные данные текущего пользователя
 * 2. InitializeSecurityContext — создаём начальный токен Negotiate
 * 3. Отправляем Proxy-Authorization: Negotiate <base64 token>
 * 4. Если прокси ответил 407 — парсим Proxy-Authenticate: Negotiate <challenge>,
 *    вызываем InitializeSecurityContext с полученным challenge, получаем следующий токен
 * 5. Повторяем, пока SEC_E_OK / SEC_E_COMPLETE_NEEDED
 *
 * Флаги ISC_REQ_CONFIDENTIALITY и ISC_REQ_REPLAY_DETECT добавлены для совместимости
 * с прокси, требующими подписанных токенов.
 *
 * Разработчик: Kalikin Iliya
 */

#include "auth_sspi.h"
#include <cstdio>

namespace tcp_redirector {
namespace infrastructure {

// ====================================================================
// SspiContext::Release
// ====================================================================

void SspiContext::Release() {
    if (context.dwUpper != 0 || context.dwLower != 0) {
        DeleteSecurityContext(&context);
        context = {0, 0};
    }
    if (credentials.dwUpper != 0 || credentials.dwLower != 0) {
        FreeCredentialsHandle(&credentials);
        credentials = {0, 0};
    }
    initialized = false;
}

// ====================================================================
// SspiNegotiate — основной цикл SSPI
// ====================================================================

SspiResult SspiNegotiate(SspiContext& ctx,
                         const std::string& serverToken,
                         std::string& outToken,
                         const std::string& spn) {
    outToken.clear();
    SECURITY_STATUS sc;

    // --- Шаг 0: логируем имя пользователя (только при первом вызове) ---
    if (!ctx.initialized) {
        wchar_t userName[256] = {0};
        DWORD userNameLen = 256;
        if (GetUserNameW(userName, &userNameLen)) {
            printf("[SSPI] Authenticating as: %S (SPN=%s)\n", userName, spn.c_str());
        } else {
            printf("[SSPI] Authenticating as: <GetUserNameW failed, err=%lu> (SPN=%s)\n",
                   GetLastError(), spn.c_str());
        }
    }

    // --- Шаг 1: AcquireCredentialsHandle (только при первом вызове) ---
    if (!ctx.initialized) {
        ctx.targetSpn = spn;
        // Используем пакет Negotiate — Windows сам выберет Kerberos или NTLM
        // const_cast необходимо, т.к. AcquireCredentialsHandleW принимает LPWSTR
        wchar_t pkgName[] = L"Negotiate";
        sc = AcquireCredentialsHandleW(
            NULL,                           // Текущий пользователь
            pkgName,                        // Пакет безопасности
            SECPKG_CRED_OUTBOUND,           // Исходящая аутентификация
            NULL,                           // Не используем LOGON_ID
            NULL,                           // Не передаём auth_data (текущий пользователь)
            NULL, NULL,                     // Нет колбэков
            &ctx.credentials,              // [out] Полученный дескриптор
            NULL);                          // [out] Время жизни (не нужно)

        if (sc != SEC_E_OK) {
            printf("[SSPI] AcquireCredentialsHandle failed: 0x%08X (%s)\n",
                   sc, SspiErrorText(sc).c_str());
            return (sc == SEC_E_NO_CREDENTIALS) ? SspiResult::NoCredentials
                                                : SspiResult::Error;
        }

        // Сохраняем SPN для повторных вызовов
        if (ctx.targetSpn.empty()) {
            ctx.targetSpn = spn;
        }
        ctx.initialized = true;
        printf("[SSPI] AcquireCredentialsHandle OK (Negotiate)\n");
    }

    // --- Шаг 2: Подготовка входного токена (если есть challenge от сервера) ---
    SecBufferDesc inBufDesc = {0};
    SecBuffer     inBuf = {0};
    std::vector<uint8_t> inToken;

    if (!serverToken.empty()) {
        inToken = SspiBase64Decode(serverToken);
        if (inToken.empty()) {
            printf("[SSPI] Failed to decode server token\n");
            return SspiResult::Error;
        }
        inBuf.cbBuffer   = (ULONG)inToken.size();
        inBuf.pvBuffer   = inToken.data();
        inBuf.BufferType = SECBUFFER_TOKEN;
        inBufDesc.ulVersion = SECBUFFER_VERSION;
        inBufDesc.cBuffers  = 1;
        inBufDesc.pBuffers  = &inBuf;
    }

    // --- Шаг 3: InitializeSecurityContext ---
    ULONG outFlags = 0;
    SecBufferDesc outBufDesc = {0};
    SecBuffer     outBuf = {0};
    char tokenBuf[16384];

    outBuf.cbBuffer   = sizeof(tokenBuf);
    outBuf.pvBuffer   = tokenBuf;
    outBuf.BufferType = SECBUFFER_TOKEN;

    outBufDesc.ulVersion = SECBUFFER_VERSION;
    outBufDesc.cBuffers  = 1;
    outBufDesc.pBuffers  = &outBuf;

    // Преобразуем SPN в широкие символы (должны быть изменяемыми для API)
    std::wstring wSpn(ctx.targetSpn.begin(), ctx.targetSpn.end());

    ULONG reqFlags = ISC_REQ_CONFIDENTIALITY |
                     ISC_REQ_REPLAY_DETECT |
                     ISC_REQ_SEQUENCE_DETECT |
                     ISC_REQ_ALLOCATE_MEMORY;

    sc = InitializeSecurityContextW(
        &ctx.credentials,                  // Credentials
        serverToken.empty() ? NULL : &ctx.context,  // Первый вызов → NULL
        wSpn.data(),                       // SPN (writable)
        reqFlags,
        0,
        SECURITY_NATIVE_DREP,
        serverToken.empty() ? NULL : &inBufDesc,   // Входной токен (если есть)
        0,
        &ctx.context,                     // [out] Контекст
        &outBufDesc,                      // [out] Выходной токен
        &outFlags,                        // [out] Флаги
        NULL);                            // [out] Время жизни (не нужно)

    // --- Шаг 4: Обработка результата ---
    if (sc == SEC_E_OK || sc == SEC_I_COMPLETE_NEEDED || sc == SEC_I_COMPLETE_AND_CONTINUE) {
        // Аутентификация завершена
    } else if (sc == SEC_I_CONTINUE_NEEDED) {
        // Требуется продолжение (сервер пришлёт ещё один challenge)
    } else {
        printf("[SSPI] InitializeSecurityContext failed: 0x%08X (%s)\n",
               sc, SspiErrorText(sc).c_str());
        ctx.Release();
        return SspiResult::Error;
    }

    // --- Шаг 5: Кодируем выходной токен в Base64 ---
    if (outBuf.cbBuffer > 0 && outBuf.pvBuffer != NULL) {
        uint8_t* buf = static_cast<uint8_t*>(outBuf.pvBuffer);
        std::vector<uint8_t> outVec(buf, buf + outBuf.cbBuffer);
        outToken = SspiBase64Encode(outVec);

        // Освобождаем память, выделенную SSPI (только если использовали ALLOCATE_MEMORY)
        if (outBuf.BufferType == SECBUFFER_TOKEN && outBuf.pvBuffer != tokenBuf) {
            FreeContextBuffer(outBuf.pvBuffer);
        }
    }

    if (sc == SEC_E_OK) {
        return SspiResult::Complete;
    } else if (sc == SEC_I_CONTINUE_NEEDED) {
        return SspiResult::NeedContinue;
    } else if (sc == SEC_I_COMPLETE_NEEDED || sc == SEC_I_COMPLETE_AND_CONTINUE) {
        // CompleteAuthToken не обязателен для Negotiate/Kerberos
        return SspiResult::Complete;
    }

    return SspiResult::Success;
}

// ====================================================================
// Parse407Challenge — извлечение токена из заголовка 407
// ====================================================================

std::string Parse407Challenge(const std::string& responseBody) {
    // Ищем "Proxy-Authenticate:" (регистронезависимо)
    const char* markers[] = {
        "Proxy-Authenticate: Negotiate ",
        "proxy-authenticate: negotiate ",
        "Proxy-Authenticate: negotiate ",
        "proxy-authenticate: Negotiate "
    };

    for (const char* marker : markers) {
        auto pos = responseBody.find(marker);
        if (pos == std::string::npos) continue;

        pos += strlen(marker);
        auto end = responseBody.find_first_of("\r\n", pos);
        if (end == std::string::npos) {
            end = responseBody.size();
        }
        std::string token = responseBody.substr(pos, end - pos);

        // Удаляем возможный пробел в конце
        while (!token.empty() && (token.back() == ' ' || token.back() == '\r'))
            token.pop_back();

        return token;
    }

    return {};
}

// ====================================================================
// MakeSpn — формирование SPN
// ====================================================================

std::string MakeSpn(const std::string& proxyHost) {
    return "HTTP/" + proxyHost;
}

// ====================================================================
// IsProbablyIpv4Literal — грубая эвристика: строка вида a.b.c.d
// ====================================================================
//
// Kerberos резолвит билет по SPN "HTTP/<FQDN>", а не по IP-литералу.
// Если proxyHost задан как IP (типичный кейс заглушки: 127.0.0.1),
// настоящий Kerberos невозможен — SSPI Negotiate откатится на NTLM.
// Вызывающая сторона использует это для предупреждающего лога.
bool IsProbablyIpv4Literal(const std::string& host) {
    if (host.empty()) return false;
    int dots = 0;
    for (char c : host) {
        if (c == '.') { ++dots; continue; }
        if (c < '0' || c > '9') return false;   // не цифра и не точка → не IPv4-литерал
    }
    return dots == 3;
}

// ====================================================================
// Base64
// ====================================================================

std::string SspiBase64Encode(const std::vector<uint8_t>& data) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "abcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t b = ((uint32_t)data[i]) << 16;
        if (i + 1 < data.size()) b |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < data.size()) b |= (uint32_t)data[i + 2];

        result += chars[(b >> 18) & 0x3F];
        result += chars[(b >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? chars[(b >> 6) & 0x3F] : '=';
        result += (i + 2 < data.size()) ? chars[b & 0x3F] : '=';
    }

    return result;
}

std::vector<uint8_t> SspiBase64Decode(const std::string& data) {
    static const unsigned char D[256] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x3E,0xFF,0xFF,0xFF,0x3F,
        0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,
        0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
        0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,0xFF,0xFF,0xFF,0xFF,0xFF
    };

    std::vector<uint8_t> result;
    int val = 0, valb = -8;
    for (unsigned char c : data) {
        // H2: treat padding '=' as end-of-data — stop decoding immediately
        if (c == '=') break;
        // Skip whitespace and other non-base64 characters
        if (D[c] == 0xFF) continue;
        val = (val << 6) + D[c];
        valb += 6;
        if (valb >= 0) {
            result.push_back((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return result;
}

// ====================================================================
// SspiErrorText
// ====================================================================

std::string SspiErrorText(SECURITY_STATUS sc) {
    switch (sc) {
        case SEC_E_OK:                       return "SEC_E_OK";
        case SEC_E_INSUFFICIENT_MEMORY:      return "SEC_E_INSUFFICIENT_MEMORY";
        case SEC_E_INVALID_HANDLE:           return "SEC_E_INVALID_HANDLE";
        case SEC_E_UNSUPPORTED_FUNCTION:     return "SEC_E_UNSUPPORTED_FUNCTION";
        case SEC_E_TARGET_UNKNOWN:           return "SEC_E_TARGET_UNKNOWN";
        case SEC_E_INTERNAL_ERROR:           return "SEC_E_INTERNAL_ERROR";
        case SEC_E_SECPKG_NOT_FOUND:         return "SEC_E_SECPKG_NOT_FOUND";
        case SEC_E_NOT_OWNER:               return "SEC_E_NOT_OWNER";
        case SEC_E_CANNOT_INSTALL:           return "SEC_E_CANNOT_INSTALL";
        case SEC_E_INVALID_TOKEN:            return "SEC_E_INVALID_TOKEN";
        case SEC_E_CANNOT_PACK:              return "SEC_E_CANNOT_PACK";
        case SEC_E_QOP_NOT_SUPPORTED:        return "SEC_E_QOP_NOT_SUPPORTED";
        case SEC_E_NO_IMPERSONATION:         return "SEC_E_NO_IMPERSONATION";
        case SEC_E_LOGON_DENIED:             return "SEC_E_LOGON_DENIED";
        case SEC_E_UNKNOWN_CREDENTIALS:      return "SEC_E_UNKNOWN_CREDENTIALS";
        case SEC_E_NO_CREDENTIALS:           return "SEC_E_NO_CREDENTIALS (нет Kerberos-билета)";
        case SEC_E_MESSAGE_ALTERED:          return "SEC_E_MESSAGE_ALTERED";
        case SEC_E_OUT_OF_SEQUENCE:          return "SEC_E_OUT_OF_SEQUENCE";
        case SEC_E_NO_AUTHENTICATING_AUTHORITY: return "SEC_E_NO_AUTHENTICATING_AUTHORITY";
        case SEC_I_CONTINUE_NEEDED:          return "SEC_I_CONTINUE_NEEDED";
        case SEC_I_COMPLETE_NEEDED:          return "SEC_I_COMPLETE_NEEDED";
        case SEC_I_COMPLETE_AND_CONTINUE:    return "SEC_I_COMPLETE_AND_CONTINUE";
        case SEC_I_LOCAL_LOGON:              return "SEC_I_LOCAL_LOGON";
        case SEC_I_GENERIC_EXTENSION_RECEIVED: return "SEC_I_GENERIC_EXTENSION_RECEIVED";
        default: {
            char buf[64];
            snprintf(buf, sizeof(buf), "UNKNOWN(0x%08X)", (unsigned int)sc);
            return buf;
        }
    }
}

} // namespace infrastructure
} // namespace tcp_redirector