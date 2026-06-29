#pragma once

/**
 * @file utf8_convert.h
 * @brief Корректная конвертация UTF-8 ↔ UTF-16 через Win32 API.
 *
 * Замена для ломающего не-ASCII кода:
 *   std::wstring(s.begin(), s.end())       // ← Ломает не-ASCII (M6)
 *   std::string(ws.begin(), ws.end())      // ← Ломает не-ASCII (M6)
 *
 * Использование:
 *   std::wstring ws = Utf8ToWide("Привет мир");
 *   std::string  s  = WideToUtf8(L"Привет мир");
 */

#include <string>
#include <vector>
#include <windows.h>

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Преобразовать UTF-8 строку в UTF-16 (std::wstring).
 * @param s  Входная UTF-8 строка.
 * @return  UTF-16 строка. Пустая строка при ошибке.
 */
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &result[0], len);
    return result;
}

/**
 * @brief Преобразовать UTF-16 строку в UTF-8 (std::string).
 * @param ws  Входная UTF-16 строка.
 * @return   UTF-8 строка. Пустая строка при ошибке.
 */
inline std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), &result[0], len, nullptr, nullptr);
    return result;
}

/**
 * @brief Безопасно скопировать wstring в std::string (UTF-8),
 *        заменяя неконвертируемые символы на '?'.
 */
inline std::string WideToUtf8Lossy(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, WC_NO_BEST_FIT_CHARS,
                                  ws.data(), (int)ws.size(),
                                  nullptr, 0, "?", nullptr);
    if (len <= 0) return {};
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, WC_NO_BEST_FIT_CHARS,
                        ws.data(), (int)ws.size(),
                        &result[0], len, "?", nullptr);
    return result;
}

} // namespace infrastructure
} // namespace tcp_redirector
