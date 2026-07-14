#include "SspiEngine.h"
#include <sstream>
#include <cstdio>

namespace tcp_redirector {
namespace auth_agent {

SspiEngine::SspiEngine() = default;
SspiEngine::~SspiEngine() = default;

SspiStepResult SspiEngine::CreateContext(
    std::string_view spn,
    CredHandle& outCredentials,
    CtxtHandle& outContext) {

    SspiStepResult result{};
    result.success = false;

    // 1. Acquire credentials (в контексте пользовательской сессии)
    TimeStamp expiry;
    wchar_t pkgName[] = L"Negotiate";
    SECURITY_STATUS sc = AcquireCredentialsHandleW(
        nullptr,                    // pszPrincipal = текущий пользователь
        pkgName,                    // Пакет безопасности
        SECPKG_CRED_OUTBOUND,       // Исходящая аутентификация
        nullptr,                    // pLogonID
        nullptr,                    // pAuthData
        nullptr,                    // GetKeyFn
        nullptr,                    // GetKeyArg
        &outCredentials,
        &expiry);

    if (sc != SEC_E_OK) {
        result.error_message = "AcquireCredentialsHandleW failed: " +
                               SspiErrorText(sc);
        return result;
    }

    // 2. Initialize security context (первый вызов — без входного токена)
    std::string spnStr = MakeSpn(spn);
    std::wstring wSpn(spnStr.begin(), spnStr.end());

    SecBufferDesc outBufDesc;
    SecBuffer outBuf;
    outBuf.BufferType = SECBUFFER_TOKEN;
    outBuf.cbBuffer = 0;
    outBuf.pvBuffer = nullptr;
    outBufDesc.ulVersion = SECBUFFER_VERSION;
    outBufDesc.cBuffers = 1;
    outBufDesc.pBuffers = &outBuf;

    ULONG ctxAttr = 0;
    TimeStamp ctxExpiry;

    sc = InitializeSecurityContextW(
        &outCredentials,
        nullptr,                    // phContext = nullptr (новый контекст)
        const_cast<LPWSTR>(wSpn.c_str()),
        ISC_REQ_CONFIDENTIALITY | ISC_REQ_CONNECTION,
        0,                          // Reserved1
        SECURITY_NATIVE_DREP,       // TargetDataRep
        nullptr,                    // pInput (нет входного токена)
        0,                          // Reserved2
        &outContext,
        &outBufDesc,
        &ctxAttr,
        &ctxExpiry);

    if (sc == SEC_I_COMPLETE_NEEDED || sc == SEC_I_COMPLETE_AND_CONTINUE) {
        CompleteAuthToken(&outContext, &outBufDesc);
    }

    if (sc == SEC_E_OK || sc == SEC_I_CONTINUE_NEEDED ||
        sc == SEC_I_COMPLETE_NEEDED || sc == SEC_I_COMPLETE_AND_CONTINUE) {

        if (outBuf.cbBuffer > 0 && outBuf.pvBuffer != nullptr) {
            std::vector<uint8_t> token(
                static_cast<uint8_t*>(outBuf.pvBuffer),
                static_cast<uint8_t*>(outBuf.pvBuffer) + outBuf.cbBuffer);
            result.token = Base64Encode(token);
            FreeContextBuffer(outBuf.pvBuffer);
        }

        result.success = true;
        result.needs_continue = (sc == SEC_I_CONTINUE_NEEDED ||
                                 sc == SEC_I_COMPLETE_AND_CONTINUE);
    } else {
        result.error_message = "InitializeSecurityContextW failed: " +
                               SspiErrorText(sc);
        FreeCredentialsHandle(&outCredentials);
    }

    return result;
}

SspiStepResult SspiEngine::ContinueContext(
    CtxtHandle& context,
    std::string_view challenge) {

    SspiStepResult result{};
    result.success = false;

    // Декодируем challenge из Base64
    std::vector<uint8_t> challengeBytes = Base64Decode(std::string(challenge));

    SecBuffer inBuf;
    inBuf.BufferType = SECBUFFER_TOKEN;
    inBuf.cbBuffer = static_cast<ULONG>(challengeBytes.size());
    inBuf.pvBuffer = challengeBytes.data();

    SecBufferDesc inBufDesc;
    inBufDesc.ulVersion = SECBUFFER_VERSION;
    inBufDesc.cBuffers = 1;
    inBufDesc.pBuffers = &inBuf;

    SecBuffer outBuf;
    outBuf.BufferType = SECBUFFER_TOKEN;
    outBuf.cbBuffer = 0;
    outBuf.pvBuffer = nullptr;

    SecBufferDesc outBufDesc;
    outBufDesc.ulVersion = SECBUFFER_VERSION;
    outBufDesc.cBuffers = 1;
    outBufDesc.pBuffers = &outBuf;

    ULONG ctxAttr = 0;
    TimeStamp ctxExpiry;

    SECURITY_STATUS sc = InitializeSecurityContextW(
        nullptr,                    // phCredential (уже есть в контексте)
        &context,
        nullptr,                    // pszTargetName
        ISC_REQ_CONFIDENTIALITY | ISC_REQ_CONNECTION,
        0,
        SECURITY_NATIVE_DREP,
        &inBufDesc,
        0,
        &context,
        &outBufDesc,
        &ctxAttr,
        &ctxExpiry);

    if (sc == SEC_I_COMPLETE_NEEDED || sc == SEC_I_COMPLETE_AND_CONTINUE) {
        CompleteAuthToken(&context, &outBufDesc);
    }

    if (sc == SEC_E_OK || sc == SEC_I_CONTINUE_NEEDED ||
        sc == SEC_I_COMPLETE_NEEDED || sc == SEC_I_COMPLETE_AND_CONTINUE) {

        if (outBuf.cbBuffer > 0 && outBuf.pvBuffer != nullptr) {
            std::vector<uint8_t> token(
                static_cast<uint8_t*>(outBuf.pvBuffer),
                static_cast<uint8_t*>(outBuf.pvBuffer) + outBuf.cbBuffer);
            result.token = Base64Encode(token);
            FreeContextBuffer(outBuf.pvBuffer);
        }

        result.success = true;
        result.needs_continue = (sc == SEC_I_CONTINUE_NEEDED ||
                                 sc == SEC_I_COMPLETE_AND_CONTINUE);
    } else {
        result.error_message = "InitializeSecurityContextW (continue) failed: " +
                               SspiErrorText(sc);
    }

    return result;
}

void SspiEngine::FreeCredentials(CredHandle& creds) {
    FreeCredentialsHandle(&creds);
}

void SspiEngine::DeleteContext(CtxtHandle& ctx) {
    DeleteSecurityContext(&ctx);
}

// ============================================================================
// Helpers
// ============================================================================

std::string SspiEngine::Base64Encode(const std::vector<uint8_t>& data) {
    static const char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    unsigned char a3[3];
    for (auto c : data) {
        a3[i++] = c;
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

std::vector<uint8_t> SspiEngine::Base64Decode(const std::string& data) {
    static const unsigned char kDecode[128] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
        52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
        64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64
    };

    std::vector<uint8_t> result;
    result.reserve((data.size() / 4) * 3);

    int val = 0, valb = -8;
    for (unsigned char c : data) {
        if (c >= 128 || kDecode[c] == 64) continue;
        val = (val << 6) + kDecode[c];
        valb += 6;
        if (valb >= 0) {
            result.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return result;
}

std::string SspiEngine::MakeSpn(std::string_view host) {
    return "HTTP/" + std::string(host);
}

std::string SspiEngine::SspiErrorText(SECURITY_STATUS sc) {
    switch (sc) {
    case SEC_E_NO_CREDENTIALS:
        return "SEC_E_NO_CREDENTIALS — Kerberos/NTLM credentials not available";
    case SEC_E_WRONG_PRINCIPAL:
        return "SEC_E_WRONG_PRINCIPAL — target principal mismatch";
    case SEC_E_TARGET_UNKNOWN:
        return "SEC_E_TARGET_UNKNOWN — SPN not recognized";
    case SEC_E_INVALID_TOKEN:
        return "SEC_E_INVALID_TOKEN — malformed token";
    case SEC_E_INVALID_HANDLE:
        return "SEC_E_INVALID_HANDLE — invalid context handle";
    case SEC_E_INTERNAL_ERROR:
        return "SEC_E_INTERNAL_ERROR — LSA internal error";
    case SEC_E_LOGON_DENIED:
        return "SEC_E_LOGON_DENIED — logon denied";
    default: {
        char buf[64];
        snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(sc));
        return std::string(buf);
    }
    }
}

} // namespace auth_agent
} // namespace tcp_redirector