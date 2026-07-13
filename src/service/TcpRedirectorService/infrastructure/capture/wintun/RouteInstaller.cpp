/**
 * @file RouteInstaller.cpp
 * @brief WP10 — реализация split-tunnel маршрутов через IP Helper.
 */

// ВНИМАНИЕ: порядок включений критичен и совпадает с WintunAdapter.cpp.
// <netioapi.h> / <ws2ipdef.h> тянут NTSTATUS-цепочку, а она требует
// <winsock2.h> строго ДО <windows.h>.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>        // InetPtonW
#include <ws2ipdef.h>        // SOCKADDR_IN, IN_ADDR (полные определения)
#include <windows.h>
#include <iphlpapi.h>        // CreateIpForwardEntry2 (публичный интерфейс)
#include <netioapi.h>        // MIB_IPFORWARD_ROW2, InitializeIpForwardEntry

#include <cstring>
#include <string>
#include <utility>

#include "RouteInstaller.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

namespace {

// Разбор "a.b.c.d" в IN_ADDR через InetPtonW.  false — синтаксическая
// ошибка.  InetPtonW — из ws2tcpip.h, линкуется через ws2_32.lib.
bool ParseIpv4W(const std::wstring& s, IN_ADDR& out) {
    if (s.empty()) return false;
    const int rc = InetPtonW(AF_INET, s.c_str(), &out);
    return rc == 1;
}

// Форматирование ошибки WinAPI.
std::string FormatWinErr(const char* what, DWORD gle) {
    return std::string(what) + " failed, GLE=" + std::to_string(gle);
}

// Заполнить MIB_IPFORWARD_ROW2 для одного /1-маршрута.  prefix_first_byte —
// либо 0 (для 0.0.0.0/1), либо 128 (для 128.0.0.0/1).
void FillRow(MIB_IPFORWARD_ROW2& row,
             NET_LUID luid,
             uint8_t prefix_first_byte,
             const IN_ADDR& next_hop,
             uint32_t metric) {
    InitializeIpForwardEntry(&row);
    row.InterfaceLuid = luid;
    row.DestinationPrefix.Prefix.si_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b1 = prefix_first_byte;
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b2 = 0;
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b3 = 0;
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b4 = 0;
    row.DestinationPrefix.PrefixLength = 1;
    row.NextHop.si_family = AF_INET;
    row.NextHop.Ipv4.sin_family = AF_INET;
    row.NextHop.Ipv4.sin_addr = next_hop;
    row.Metric   = metric;
    row.Protocol = MIB_IPPROTO_NETMGMT;  // не persistent через reboot
    row.Origin   = NlroManual;
    // Оставляем defaults: ValidLifetime/PreferredLifetime = INFINITE, Immortal=FALSE.
    row.SitePrefixLength = 0;
}

// Одиночный CreateIpForwardEntry2.  ERROR_OBJECT_ALREADY_EXISTS — success.
// Возвращает 0 при полном успехе или WinAPI error code иначе.
DWORD CreateOneRoute(MIB_IPFORWARD_ROW2& row) {
    DWORD rc = CreateIpForwardEntry2(&row);
    if (rc == NO_ERROR || rc == ERROR_OBJECT_ALREADY_EXISTS) {
        return NO_ERROR;
    }
    return rc;
}

// Одиночный DeleteIpForwardEntry2.  ERROR_NOT_FOUND — success.
DWORD DeleteOneRoute(MIB_IPFORWARD_ROW2& row) {
    DWORD rc = DeleteIpForwardEntry2(&row);
    if (rc == NO_ERROR || rc == ERROR_NOT_FOUND) {
        return NO_ERROR;
    }
    return rc;
}

} // namespace

// ---------------------------------------------------------------------------
// Публичный API
// ---------------------------------------------------------------------------

bool RouteInstaller::InstallSplitTunnel(NET_LUID luid,
                                       const std::wstring& next_hop_ipv4,
                                       uint32_t metric,
                                       std::string* outError) {
    IN_ADDR nh{};
    if (!ParseIpv4W(next_hop_ipv4, nh)) {
        if (outError) {
            *outError = "RouteInstaller: invalid next-hop IPv4 (expected a.b.c.d)";
        }
        return false;
    }

    // Первый маршрут: 0.0.0.0/1
    MIB_IPFORWARD_ROW2 row_lo{};
    FillRow(row_lo, luid, 0u, nh, metric);
    DWORD rc = CreateOneRoute(row_lo);
    if (rc != NO_ERROR) {
        if (outError) {
            *outError = FormatWinErr("CreateIpForwardEntry2(0.0.0.0/1)", rc);
        }
        return false;
    }

    // Второй маршрут: 128.0.0.0/1
    MIB_IPFORWARD_ROW2 row_hi{};
    FillRow(row_hi, luid, 128u, nh, metric);
    rc = CreateOneRoute(row_hi);
    if (rc != NO_ERROR) {
        // Откатываем первый маршрут, чтобы не оставить orphan-полу-установку.
        (void)DeleteOneRoute(row_lo);
        if (outError) {
            *outError = FormatWinErr("CreateIpForwardEntry2(128.0.0.0/1)", rc);
        }
        return false;
    }

    return true;
}

bool RouteInstaller::UninstallSplitTunnel(NET_LUID luid,
                                         std::string* outError) {
    // При удалении next-hop/metric не важны — совпадение по LUID + prefix.
    // Тем не менее заполняем строку целиком: некоторые версии стека сравнивают
    // NextHop, если он не 0.  Мы делаем NextHop = 0.0.0.0, чтобы ядро искало
    // маршрут по (LUID, prefix) — стандартный кейс.
    IN_ADDR zero{}; zero.S_un.S_addr = 0;

    MIB_IPFORWARD_ROW2 row_lo{};
    FillRow(row_lo, luid, 0u, zero, 0u);
    DWORD rc_lo = DeleteOneRoute(row_lo);

    MIB_IPFORWARD_ROW2 row_hi{};
    FillRow(row_hi, luid, 128u, zero, 0u);
    DWORD rc_hi = DeleteOneRoute(row_hi);

    if (rc_lo == NO_ERROR && rc_hi == NO_ERROR) {
        return true;
    }

    if (outError) {
        std::string msg = "RouteInstaller::Uninstall partial failure:";
        if (rc_lo != NO_ERROR) {
            msg += " " + FormatWinErr("DeleteIpForwardEntry2(0.0.0.0/1)", rc_lo);
        }
        if (rc_hi != NO_ERROR) {
            msg += " " + FormatWinErr("DeleteIpForwardEntry2(128.0.0.0/1)", rc_hi);
        }
        *outError = std::move(msg);
    }
    return false;
}

bool RouteInstaller::IsInstalled(NET_LUID luid) {
    IN_ADDR zero{}; zero.S_un.S_addr = 0;

    // GetIpForwardEntry2 требует, чтобы у переданной строки были заполнены
    // InterfaceLuid, DestinationPrefix, NextHop.  Если маршрут существует —
    // NO_ERROR; иначе ERROR_NOT_FOUND.
    MIB_IPFORWARD_ROW2 row_lo{};
    FillRow(row_lo, luid, 0u, zero, 0u);
    const DWORD rc_lo = GetIpForwardEntry2(&row_lo);

    MIB_IPFORWARD_ROW2 row_hi{};
    FillRow(row_hi, luid, 128u, zero, 0u);
    const DWORD rc_hi = GetIpForwardEntry2(&row_hi);

    return (rc_lo == NO_ERROR) || (rc_hi == NO_ERROR);
}

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
