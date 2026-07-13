#pragma once

/**
 * @file AppPaths.h
 * @brief Резолверы стандартных путей приложения (рядом с EXE сервиса).
 *
 * WP1 (см. plans/WINTUN_INTEGRATION_PLAN.md §2, §11):
 *   Все пути ресурсов сервиса (config.json, logs\, .bin\) резолвятся
 *   на основе каталога исполняемого файла, полученного через
 *   GetModuleFileNameW(NULL, ...). Это единственный надёжный способ
 *   узнать директорию EXE при запуске под SCM, где cwd = System32.
 *
 * Header-only, без зависимостей от CRT-specific `_pgmptr` (ненадёжно
 * под SCM) и без GetCurrentDirectory.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <stdexcept>
#include <system_error>

namespace tcp_redirector {
namespace infrastructure {
namespace paths {

/**
 * @brief Вернуть директорию исполняемого файла сервиса с завершающим '\'.
 *
 * Использует GetModuleFileNameW(NULL, ...). При ERROR_INSUFFICIENT_BUFFER
 * увеличивает буфер вплоть до 32767 (лимит long path в Windows).
 *
 * @return Путь к каталогу EXE, включая завершающий обратный слэш.
 * @throws std::runtime_error при неустранимой ошибке WinAPI (с GetLastError в сообщении).
 */
inline std::wstring GetExecutableDirectoryW() {
    // Стартуем с MAX_PATH; при переполнении удваиваем до 32767.
    DWORD bufSize = MAX_PATH;
    std::wstring buf;
    for (;;) {
        buf.assign(bufSize, L'\0');
        SetLastError(0);
        DWORD written = GetModuleFileNameW(nullptr, buf.data(), bufSize);
        DWORD err = GetLastError();

        if (written == 0) {
            throw std::runtime_error(
                "GetModuleFileNameW failed, GLE=" + std::to_string(err));
        }
        if (written < bufSize) {
            // Успех — записано без обрезания.
            buf.resize(written);
            break;
        }
        // ERROR_INSUFFICIENT_BUFFER (при переполнении written == bufSize
        // и последний символ не '\0').
        if (bufSize >= 32767) {
            throw std::runtime_error(
                "GetModuleFileNameW: path exceeds 32767 chars, GLE="
                + std::to_string(err));
        }
        bufSize = (bufSize < 16384) ? (bufSize * 2) : 32767;
    }

    // Отрезаем имя файла: ищем последний '\' или '/'.
    size_t slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        // Совсем без разделителя — маловероятно, но защитимся.
        throw std::runtime_error(
            "GetExecutableDirectoryW: no path separator in module filename");
    }
    // Оставляем сам '\'.
    buf.resize(slash + 1);
    return buf;
}

/**
 * @brief Путь к config.json рядом с EXE сервиса.
 * @return <exeDir>\config.json
 */
inline std::wstring GetConfigPathW() {
    return GetExecutableDirectoryW() + L"config.json";
}

/**
 * @brief Директория логов рядом с EXE сервиса (с завершающим '\').
 * @return <exeDir>\logs\
 */
inline std::wstring GetLogDirectoryW() {
    return GetExecutableDirectoryW() + L"logs\\";
}

/**
 * @brief Директория .bin\ рядом с EXE (для будущих WP: wintun.dll, tun2socks и т.п.).
 * @return <exeDir>\.bin\
 */
inline std::wstring GetBinDirectoryW() {
    return GetExecutableDirectoryW() + L".bin\\";
}

} // namespace paths
} // namespace infrastructure
} // namespace tcp_redirector
