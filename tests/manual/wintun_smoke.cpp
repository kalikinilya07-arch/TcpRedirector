/**
 * @file wintun_smoke.cpp
 * @brief WP8 — manual smoke-test для WintunApi/WintunAdapter/WintunSession.
 *
 * НЕ ЯВЛЯЕТСЯ ЧАСТЬЮ build.bat.  Собирается вручную через
 * tests/manual/build_wintun_smoke.bat, требует наличия рабочего
 * wintun.dll в .bin\wintun\x64\wintun.dll и запуска от Администратора.
 *
 * Что делает:
 *   1) Load() — грузит wintun.dll.
 *   2) Печатает Running Driver Version.
 *   3) CreateOrOpen() адаптера "TcpRedirectorSmoke".
 *   4) ConfigureIpv4("10.6.7.1/24").
 *   5) Start() сессии на 4 MiB, ждёт до 500 мс на readEvent; если что-то
 *      пришло — печатает первый байт (версию IP).
 *   6) Закрывает всё в LIFO-порядке.
 *
 * Не пампит трафик через lwIP (это WP9/WP10).  Ошибки — в stderr,
 * exit-code 0 при полном успехе, иначе 1..N.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "../../src/service/TcpRedirectorService/infrastructure/capture/wintun/WintunApi.h"
#include "../../src/service/TcpRedirectorService/infrastructure/capture/wintun/WintunAdapter.h"
#include "../../src/service/TcpRedirectorService/infrastructure/capture/wintun/WintunSession.h"

using namespace tcp_redirector::infrastructure::capture::wintun;

static void PrintW(const char* prefix, const std::wstring& w) {
    std::fprintf(stderr, "%s", prefix);
    // Печатаем через WriteConsoleW, если stderr — консоль; иначе — как ASCII fallback.
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (!WriteConsoleW(h, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr)) {
        for (wchar_t c : w) std::fputc(c < 128 ? (char)c : '?', stderr);
    }
    std::fputc('\n', stderr);
}

int main() {
    std::string err;

    std::fprintf(stderr, "[smoke] Loading wintun.dll...\n");
    auto api = WintunApi::Load(&err);
    if (!api) {
        std::fprintf(stderr, "[smoke] FAIL: %s\n", err.c_str());
        return 1;
    }
    PrintW("[smoke] Loaded from: ", api->DllPath());
    std::fprintf(stderr, "[smoke] Running driver version: 0x%08X\n",
                 api->RunningDriverVersion());

    WintunAdapter::CreateParams p;
    p.name         = L"TcpRedirectorSmoke";
    p.tunnel_type  = L"TcpRedirector";
    // Пустой GUID → wintun выберет сам.  Для стабильности во WP10 сюда
    // подставится значение из config.wintun.adapter_guid.
    p.requested_guid = std::nullopt;

    std::fprintf(stderr, "[smoke] CreateOrOpen adapter...\n");
    auto adapter = WintunAdapter::CreateOrOpen(api, p, &err);
    if (!adapter) {
        std::fprintf(stderr, "[smoke] FAIL: %s\n", err.c_str());
        return 2;
    }
    std::fprintf(stderr, "[smoke] Adapter LUID (hi/lo): 0x%016llX\n",
                 static_cast<unsigned long long>(adapter->Luid().Value));
    std::fprintf(stderr, "[smoke] Interface index: %lu\n",
                 static_cast<unsigned long>(adapter->InterfaceIndex()));

    std::fprintf(stderr, "[smoke] ConfigureIpv4 10.6.7.1/24...\n");
    if (!adapter->ConfigureIpv4(L"10.6.7.1/24", &err)) {
        std::fprintf(stderr, "[smoke] FAIL: %s\n", err.c_str());
        return 3;
    }

    std::fprintf(stderr, "[smoke] Starting session (4 MiB)...\n");
    auto session = WintunSession::Start(api, adapter->Handle(),
                                         WintunSession::kDefaultCapacity, &err);
    if (!session) {
        std::fprintf(stderr, "[smoke] FAIL: %s\n", err.c_str());
        return 4;
    }

    // Ждём 500 мс любого пакета.  Не гарантия что что-то придёт —
    // это лишь sanity check жизнеспособности сессии.
    std::fprintf(stderr, "[smoke] Waiting up to 500ms for a packet...\n");
    HANDLE stop = CreateEventW(nullptr, TRUE /*manualReset*/, FALSE, nullptr);
    // Взводим stop через 500мс через таймер: простой Sleep-thread.
    HANDLE timerThread = CreateThread(
        nullptr, 0,
        [](LPVOID lp) -> DWORD {
            Sleep(500);
            SetEvent(reinterpret_cast<HANDLE>(lp));
            return 0;
        },
        stop, 0, nullptr);

    std::vector<uint8_t> buf;
    int n = session->ReceiveInto(buf, stop, &err);
    if (n < 0) {
        std::fprintf(stderr, "[smoke] Receive FAIL: %s\n", err.c_str());
    } else if (n == 0) {
        std::fprintf(stderr, "[smoke] No packet within 500ms (expected if no traffic routed).\n");
    } else {
        std::fprintf(stderr, "[smoke] Got %d bytes; first byte=0x%02X (IP version nibble=%u)\n",
                     n, buf[0], (buf[0] >> 4) & 0x0F);
    }

    WaitForSingleObject(timerThread, INFINITE);
    CloseHandle(timerThread);
    CloseHandle(stop);

    std::fprintf(stderr, "[smoke] Cleanup...\n");
    // LIFO: session, adapter, api.
    session.reset();
    adapter.reset();
    api.reset();
    std::fprintf(stderr, "[smoke] OK\n");
    return 0;
}
