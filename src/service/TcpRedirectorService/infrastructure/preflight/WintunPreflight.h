#pragma once

/**
 * @file WintunPreflight.h
 * @brief WP7 — preflight-валидатор для режима capture_mode="wintun".
 *
 * Проверки (engine-aware):
 *
 *   ВСЕГДА (независимо от wintun.engine):
 *     - Каталог .bin\ существует.
 *     - Файл .bin\wintun\<arch>\wintun.dll существует (<arch>=x64 в 64-битной
 *       сборке; x86 в 32-битной).  Hard fail при отсутствии.
 *     - Процесс работает с правами администратора / SYSTEM (hard).
 *     - wintun.adapter_name непустое (hard).
 *     - wintun.tunnel_ipv4_cidr парсится (a.b.c.d/N, prefix ∈ [0..32]) (hard).
 *
 *   ДОПОЛНИТЕЛЬНО, только когда wintun.engine == External:
 *     - external_engine.executable: если относительный — резолвится от <exeDir>;
 *       файл должен существовать.  Hard fail.  Резолвленный абсолютный путь
 *       уходит в warnings как информационное сообщение.
 *     - external_engine.socks5_listen парсится как host:port, port ∈ [1..65535]
 *       (hard).  Запуск tun2socks здесь НЕ производится — это делает WP12.
 *
 * Зависимости WinDivert здесь НЕ проверяются: этот класс инстанциируется
 * ServiceMain'ом только когда capture_mode="wintun".
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <string>
#include <cstdint>

#include "../../domain/ports/ICapturePreflight.h"
#include "../config/Config.h"
#include "../paths/AppPaths.h"

namespace tcp_redirector {
namespace infrastructure {
namespace preflight {

class WintunPreflight final : public domain::ports::ICapturePreflight {
public:
    // Копия конфига — preflight не должен зависеть от времени жизни ConfigManager.
    explicit WintunPreflight(const WintunSettings& cfg)
        : m_cfg(cfg) {}
    ~WintunPreflight() override = default;

    domain::ports::PreflightResult Check() override {
        domain::ports::PreflightResult r;
        r.mode = "wintun";

        // 1. Резолвим exeDir и binDir через AppPaths (WP1).
        std::wstring exeDirW, binDirW;
        try {
            exeDirW = paths::GetExecutableDirectoryW();
            binDirW = paths::GetBinDirectoryW();
        } catch (const std::exception& e) {
            r.failures.push_back(std::string(
                "Cannot resolve executable/bin directory: ") + e.what());
            r.ok = false;
            return r;
        }

        const std::filesystem::path exeDir(exeDirW);
        const std::filesystem::path binDir(binDirW);

        std::error_code ec;
        if (!std::filesystem::exists(binDir, ec) || ec) {
            r.failures.push_back(
                "Bin directory does not exist (expected: " + binDir.string() + ")");
        }

        // 2. wintun.dll в arch-подпапке.  Проект нативно x64; для x86-сборки
        //    (маловероятно, но детектим через sizeof(void*)) — x86.
        const wchar_t* arch = (sizeof(void*) == 8) ? L"x64" : L"x86";
        const auto wintunDll = binDir / L"wintun" / arch / L"wintun.dll";
        if (!std::filesystem::exists(wintunDll, ec) || ec) {
            r.failures.push_back(
                std::string("wintun.dll not found (expected: ")
                + wintunDll.string() + ")");
        }

        // 3. Elevation. Тот же принцип, что в WinDivertPreflight.
        if (!IsProcessElevated()) {
            r.failures.push_back(
                "Service process is not elevated (Wintun requires Administrator or SYSTEM)");
        }

        // 4. adapter_name.
        if (m_cfg.adapter_name.empty()) {
            r.failures.push_back("wintun.adapter_name is empty");
        }

        // 5. tunnel_ipv4_cidr.
        if (!IsValidIPv4Cidr(m_cfg.tunnel_ipv4_cidr)) {
            r.failures.push_back(
                "wintun.tunnel_ipv4_cidr is invalid (expected a.b.c.d/N, N in [0..32]): '"
                + m_cfg.tunnel_ipv4_cidr + "'");
        }

        // 6. External engine — только если выбрано.
        if (m_cfg.engine == WintunEngineKind::External) {
            CheckExternalEngine(exeDir, r);
        }

        r.ok = r.failures.empty();
        return r;
    }

private:
    WintunSettings m_cfg;

    // ---- External-engine ветка ---------------------------------------------

    void CheckExternalEngine(const std::filesystem::path& exeDir,
                             domain::ports::PreflightResult& r) const {
        // 6a. Путь к tun2socks.exe.  Абсолютный — как есть; относительный —
        //     резолвится от exeDir.
        const std::string& raw = m_cfg.external_engine.executable;
        if (raw.empty()) {
            r.failures.push_back(
                "wintun.external_engine.executable is empty (engine=external)");
        } else {
            std::filesystem::path exe(raw);
            if (exe.is_relative()) {
                exe = exeDir / exe;
            }
            std::error_code ec;
            const bool exists = std::filesystem::exists(exe, ec) && !ec;
            // Информируем о резолвленном абсолютном пути (даже при hard-fail).
            r.warnings.push_back(
                "External engine executable resolved to: " + exe.string());
            if (!exists) {
                r.failures.push_back(
                    "wintun.external_engine.executable not found (resolved: "
                    + exe.string() + ")");
            }
        }

        // 6b. socks5_listen: "host:port", port ∈ [1..65535].
        std::string host;
        int port = 0;
        if (!ParseHostPort(m_cfg.external_engine.socks5_listen, host, port)) {
            r.failures.push_back(
                "wintun.external_engine.socks5_listen is not host:port with valid port [1..65535]: '"
                + m_cfg.external_engine.socks5_listen + "'");
        }
    }

    // ---- Утилиты ----------------------------------------------------------

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

    // Мини-валидатор CIDR: 4 октета 0..255, '/', префикс 0..32.
    // Осознанно локальный — не тянем сюда парсер из WP3, чтобы preflight
    // оставался header-only и без побочных зависимостей.
    static bool IsValidIPv4Cidr(const std::string& s) {
        if (s.empty()) return false;
        const auto slash = s.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) return false;

        // Разбор четырёх октетов.
        const std::string ip = s.substr(0, slash);
        int octets[4] = {0, 0, 0, 0};
        int idx = 0;
        int cur = -1;             // -1 значит «пока цифр не было»
        for (size_t i = 0; i <= ip.size(); ++i) {
            const char c = (i < ip.size()) ? ip[i] : '.';
            if (c == '.') {
                if (cur < 0 || cur > 255) return false;
                if (idx >= 4) return false;
                octets[idx++] = cur;
                cur = -1;
            } else if (c >= '0' && c <= '9') {
                if (cur < 0) cur = 0;
                cur = cur * 10 + (c - '0');
                if (cur > 999) return false;   // защита от переполнения
            } else {
                return false;
            }
        }
        if (idx != 4) return false;

        // Разбор префикса.
        const std::string pfx = s.substr(slash + 1);
        if (pfx.empty()) return false;
        int p = 0;
        for (char c : pfx) {
            if (c < '0' || c > '9') return false;
            p = p * 10 + (c - '0');
            if (p > 32) return false;
        }
        return p >= 0 && p <= 32;
    }

    // Разбор "host:port".  Для IPv6 в квадратных скобках достаточно найти
    // последний ':' — этого хватает для нашего preflight, полноценный парсер
    // (WP11/WP12) может быть строже.
    static bool ParseHostPort(const std::string& s, std::string& host, int& port) {
        if (s.empty()) return false;
        const auto colon = s.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return false;

        host = s.substr(0, colon);
        // Снимаем IPv6-скобки, если есть.
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
        if (host.empty()) return false;

        const std::string p = s.substr(colon + 1);
        if (p.empty()) return false;
        int n = 0;
        for (char c : p) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + (c - '0');
            if (n > 65535) return false;
        }
        if (n < 1 || n > 65535) return false;
        port = n;
        return true;
    }
};

} // namespace preflight
} // namespace infrastructure
} // namespace tcp_redirector
