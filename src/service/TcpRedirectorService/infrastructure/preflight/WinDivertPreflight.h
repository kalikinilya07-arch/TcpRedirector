#pragma once

/**
 * @file WinDivertPreflight.h
 * @brief WP7 — preflight-валидатор для режима capture_mode="windivert".
 *
 * Проверяет ТОЛЬКО зависимости WinDivert:
 *   - <exeDir>\WinDivert.dll      — hard fail при отсутствии.
 *   - <exeDir>\WinDivert64.sys    — hard fail при отсутствии.
 *     (В 32-битной сборке — WinDivert32.sys; выбор по sizeof(void*).)
 *   - Процесс имеет права администратора (LocalSystem-контекст SCM даёт это
 *     естественно) — hard fail при отсутствии.
 *   - Версия WinDivert.dll через GetFileVersionInfo — только warning
 *     при отсутствии/невозможности определить (не блокирует старт).
 *
 * Зависимости Wintun здесь НЕ проверяются: этот класс инстанциируется
 * ServiceMain'ом только когда capture_mode="windivert".
 *
 * Header-only: класс небольшой, не тянет ничего кроме WinAPI + std::filesystem.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

// Линкер тянет version.lib через #pragma comment ниже, чтобы vcxproj не пришлось
// править в этом WP.  При отсутствии VerQueryValueW-инфы мы деградируем в warning.
#pragma comment(lib, "version.lib")

#include "../../domain/ports/ICapturePreflight.h"
#include "../paths/AppPaths.h"

namespace tcp_redirector {
namespace infrastructure {
namespace preflight {

class WinDivertPreflight final : public domain::ports::ICapturePreflight {
public:
    WinDivertPreflight() = default;
    ~WinDivertPreflight() override = default;

    domain::ports::PreflightResult Check() override {
        domain::ports::PreflightResult r;
        r.mode = "windivert";

        // 1. exeDir must resolve.  AppPaths::GetExecutableDirectoryW бросает
        //    std::runtime_error при неудаче GetModuleFileNameW — ловим здесь,
        //    чтобы preflight никогда не крешил сервис.
        std::wstring exeDirW;
        try {
            exeDirW = paths::GetExecutableDirectoryW();
        } catch (const std::exception& e) {
            r.failures.push_back(std::string(
                "Cannot resolve executable directory (WinAPI failure): ") + e.what());
            r.ok = false;
            return r;
        }

        const std::filesystem::path exeDir(exeDirW);

        // 2. WinDivert.dll рядом с EXE.
        const auto dllPath = exeDir / L"WinDivert.dll";
        std::error_code ec;
        if (!std::filesystem::exists(dllPath, ec) || ec) {
            r.failures.push_back(
                "WinDivert.dll not found next to service EXE (expected: "
                + dllPath.string() + ")");
        }

        // 3. WinDivert*.sys рядом с EXE.  Выбираем разрядность драйвера
        //    по разрядности процесса сервиса.
        const wchar_t* sysName = (sizeof(void*) == 8) ? L"WinDivert64.sys"
                                                     : L"WinDivert32.sys";
        const auto sysPath = exeDir / sysName;
        if (!std::filesystem::exists(sysPath, ec) || ec) {
            r.failures.push_back(
                std::string("WinDivert driver file not found next to service EXE (expected: ")
                + sysPath.string() + ")");
        }

        // 4. Elevation. Под SCM/LocalSystem это всегда true; при ручном запуске
        //    из-под обычного пользователя — hard fail.
        if (!IsProcessElevated()) {
            r.failures.push_back(
                "Service process is not elevated (WinDivert requires Administrator or SYSTEM)");
        }

        // 5. Версия WinDivert.dll — best-effort, только warning.
        if (std::filesystem::exists(dllPath, ec)) {
            std::string versionStr;
            if (!TryReadFileVersion(dllPath.wstring(), versionStr)) {
                r.warnings.push_back(
                    "Cannot read WinDivert.dll version (GetFileVersionInfo failed); continuing");
            } else if (!versionStr.empty()) {
                r.warnings.push_back("WinDivert.dll version: " + versionStr);
            }
        }

        r.ok = r.failures.empty();
        return r;
    }

private:
    // Возвращает true, если текущий процесс запущен с повышенными привилегиями
    // (Administrator / LocalSystem).  Использует TokenElevation — стандартный
    // способ Vista+.
    static bool IsProcessElevated() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }
        TOKEN_ELEVATION elevation{};
        DWORD retLen = 0;
        BOOL ok = GetTokenInformation(token, TokenElevation,
                                      &elevation, sizeof(elevation), &retLen);
        CloseHandle(token);
        if (!ok) return false;
        return elevation.TokenIsElevated != 0;
    }

    // Best-effort чтение FileVersion через VerQueryValueW.
    // Возвращает false при любой ошибке WinAPI.
    static bool TryReadFileVersion(const std::wstring& path, std::string& out) {
        DWORD dummy = 0;
        DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
        if (size == 0) return false;

        std::vector<BYTE> buf(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return false;

        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT ffiLen = 0;
        if (!VerQueryValueW(buf.data(), L"\\",
                            reinterpret_cast<LPVOID*>(&ffi), &ffiLen) ||
            !ffi || ffiLen < sizeof(VS_FIXEDFILEINFO)) {
            return false;
        }

        char tmp[64];
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%u.%u.%u.%u",
                    HIWORD(ffi->dwFileVersionMS),
                    LOWORD(ffi->dwFileVersionMS),
                    HIWORD(ffi->dwFileVersionLS),
                    LOWORD(ffi->dwFileVersionLS));
        out = tmp;
        return true;
    }
};

} // namespace preflight
} // namespace infrastructure
} // namespace tcp_redirector
