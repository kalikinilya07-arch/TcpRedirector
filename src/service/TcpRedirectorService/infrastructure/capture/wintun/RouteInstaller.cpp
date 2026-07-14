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
#include <vector>

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

// Заполнить MIB_IPFORWARD_ROW2 для одного маршрута произвольной длины префикса.
// prefix_addr_host — адрес сети в HOST byte order; prefix_len — длина префикса.
void FillRowGeneric(MIB_IPFORWARD_ROW2& row,
                    NET_LUID luid,
                    uint32_t prefix_addr_host,
                    uint8_t prefix_len,
                    const IN_ADDR& next_hop,
                    uint32_t metric) {
    InitializeIpForwardEntry(&row);
    row.InterfaceLuid = luid;
    row.DestinationPrefix.Prefix.si_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    // sin_addr хранится в network byte order.
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = htonl(prefix_addr_host);
    row.DestinationPrefix.PrefixLength = prefix_len;
    row.NextHop.si_family = AF_INET;
    row.NextHop.Ipv4.sin_family = AF_INET;
    row.NextHop.Ipv4.sin_addr = next_hop;
    row.Metric   = metric;
    row.Protocol = MIB_IPPROTO_NETMGMT;  // не persistent через reboot
    row.Origin   = NlroManual;
    // Оставляем defaults: ValidLifetime/PreferredLifetime = INFINITE, Immortal=FALSE.
    row.SitePrefixLength = 0;
}

// Обёртка совместимости: /1-маршрут по первому байту (0 или 128).
void FillRow(MIB_IPFORWARD_ROW2& row,
             NET_LUID luid,
             uint8_t prefix_first_byte,
             const IN_ADDR& next_hop,
             uint32_t metric) {
    FillRowGeneric(row, luid,
                   static_cast<uint32_t>(prefix_first_byte) << 24,
                   /*prefix_len=*/1u, next_hop, metric);
}

// Пересекается ли /prefix_len подсеть с базовым адресом base_host с диапазоном
// [range_start, range_start + range_size) (все в HOST byte order).
bool SubnetIntersectsRange(uint32_t base_host, uint8_t prefix_len,
                           uint32_t range_start, uint64_t range_size) {
    const uint64_t subnet_size = (prefix_len == 0)
        ? (uint64_t{1} << 32)
        : (uint64_t{1} << (32 - prefix_len));
    const uint64_t sub_lo = base_host;
    const uint64_t sub_hi = sub_lo + subnet_size;          // exclusive
    const uint64_t rng_lo = range_start;
    const uint64_t rng_hi = rng_lo + range_size;           // exclusive
    return sub_lo < rng_hi && rng_lo < sub_hi;
}

// Полностью ли /prefix_len подсеть содержится в диапазоне.
bool SubnetWithinRange(uint32_t base_host, uint8_t prefix_len,
                       uint32_t range_start, uint64_t range_size) {
    const uint64_t subnet_size = (prefix_len == 0)
        ? (uint64_t{1} << 32)
        : (uint64_t{1} << (32 - prefix_len));
    const uint64_t sub_lo = base_host;
    const uint64_t sub_hi = sub_lo + subnet_size;          // exclusive
    const uint64_t rng_lo = range_start;
    const uint64_t rng_hi = rng_lo + range_size;           // exclusive
    return sub_lo >= rng_lo && sub_hi <= rng_hi;
}

// Carve-out диапазоны, которые НЕЛЬЗЯ заворачивать в TUN (HOST byte order).
// {start, size}.  proxy /32 обрабатывается отдельно (InstallHostBypass).
struct CarveRange { uint32_t start; uint64_t size; };
const CarveRange kCarveOuts[] = {
    { 0x00000000u, uint64_t{1} << 24 },   // 0.0.0.0/8    — "this network"
    { 0x7F000000u, uint64_t{1} << 24 },   // 127.0.0.0/8  — loopback (relay + loopback-proxy)
    { 0xA9FE0000u, uint64_t{1} << 16 },   // 169.254.0.0/16 — link-local
};

bool IntersectsAnyCarveOut(uint32_t base_host, uint8_t prefix_len) {
    for (const auto& c : kCarveOuts) {
        if (SubnetIntersectsRange(base_host, prefix_len, c.start, c.size)) return true;
    }
    return false;
}

bool WithinAnyCarveOut(uint32_t base_host, uint8_t prefix_len) {
    for (const auto& c : kCarveOuts) {
        if (SubnetWithinRange(base_host, prefix_len, c.start, c.size)) return true;
    }
    return false;
}

// Рекурсивно разложить подсеть base_host/prefix_len в набор маршрутов,
// пропуская carve-out диапазоны.  Листья кладутся с длиной не короче
// leaf_prefix; если лист пересекает carve-out, он дробится глубже (до /32),
// пока чистая часть не отделится от исключаемой.
void ExpandLadder(uint32_t base_host, uint8_t prefix_len, uint8_t leaf_prefix,
                  std::vector<std::pair<uint32_t, uint8_t>>& out) {
    // Полностью внутри carve-out — не ставим вовсе.
    if (WithinAnyCarveOut(base_host, prefix_len)) return;

    const bool touches = IntersectsAnyCarveOut(base_host, prefix_len);
    if (!touches) {
        // Чистая подсеть.  Достигли нужной глубины листа — фиксируем.
        if (prefix_len >= leaf_prefix) {
            out.emplace_back(base_host, prefix_len);
            return;
        }
        // Ещё не глубоко — дробим до leaf_prefix (равномерная лестница).
    } else {
        // Пересекает carve-out частично — обязаны дробить, чтобы отделить.
        if (prefix_len >= 32) return; // /32 внутри carve-out — пропускаем
    }
    const uint8_t child_len = static_cast<uint8_t>(prefix_len + 1);
    const uint32_t half = 1u << (31 - prefix_len); // размер половины в адресах
    ExpandLadder(base_host,            child_len, leaf_prefix, out);
    ExpandLadder(base_host + half,     child_len, leaf_prefix, out);
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
                                       int ladder_prefix,
                                       std::string* outError) {
    // Валидируем переданный next-hop только как sanity-check формата (обычно
    // это host-часть tunnel_ipv4_cidr).  Само значение НЕ используется — маршрут
    // ставится on-link (см. ниже), — поэтому результат парсинга отбрасываем.
    IN_ADDR ignored{};
    if (!ParseIpv4W(next_hop_ipv4, ignored)) {
        if (outError) {
            *outError = "RouteInstaller: invalid next-hop IPv4 (expected a.b.c.d)";
        }
        return false;
    }

    if (ladder_prefix < 1) ladder_prefix = 1;
    if (ladder_prefix > 8) ladder_prefix = 8;

    // ON-LINK next-hop (0.0.0.0): Wintun — L3-адаптер без ARP/соседей; маршрут
    // с next-hop = адрес самого интерфейса неоднозначен (стек может «доставить
    // себе», пакеты не попадают в TUN).  On-link — идиоматичный для WireGuard.
    IN_ADDR nh{}; nh.S_un.S_addr = 0;

    // Разложить 0.0.0.0/0 в лестницу /ladder_prefix за вычетом carve-out
    // диапазонов (127/8, 0/8, 169.254/16).
    std::vector<std::pair<uint32_t, uint8_t>> leaves;
    ExpandLadder(/*base=*/0u, /*prefix_len=*/0u,
                 static_cast<uint8_t>(ladder_prefix), leaves);

    // Ставим по очереди; при провале откатываем всё уже поставленное.
    std::vector<std::pair<uint32_t, uint8_t>> installed;
    installed.reserve(leaves.size());
    for (const auto& lf : leaves) {
        MIB_IPFORWARD_ROW2 row{};
        FillRowGeneric(row, luid, lf.first, lf.second, nh, metric);
        DWORD rc = CreateOneRoute(row);
        if (rc != NO_ERROR) {
            // Откат.
            for (const auto& done : installed) {
                MIB_IPFORWARD_ROW2 r{};
                FillRowGeneric(r, luid, done.first, done.second, nh, 0u);
                (void)DeleteOneRoute(r);
            }
            if (outError) {
                *outError = FormatWinErr("CreateIpForwardEntry2(ladder leaf)", rc);
            }
            return false;
        }
        installed.push_back(lf);
    }

    if (installed.empty()) {
        // Не должно случаться (лестница /1..8 всегда даёт >=1 лист вне carve-out).
        if (outError) *outError = "RouteInstaller: ladder produced no routes";
        return false;
    }

    return true;
}

bool RouteInstaller::UninstallSplitTunnel(NET_LUID luid,
                                         std::string* outError) {
    // Перечисляем всю IPv4-таблицу и удаляем наши on-link (NextHop=0.0.0.0)
    // NETMGMT-маршруты на данном LUID с PrefixLength в [1..8] — это вся лестница
    // любой глубины (и историческая пара /1).  Так teardown не зависит от того,
    // какой ladder_prefix использовался при установке.
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    DWORD rc = GetIpForwardTable2(AF_INET, &table);
    if (rc != NO_ERROR || table == nullptr) {
        if (outError) *outError = FormatWinErr("GetIpForwardTable2(uninstall)", rc);
        return false;
    }

    std::string errAgg;
    bool anyErr = false;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& r = table->Table[i];
        if (r.InterfaceLuid.Value != luid.Value) continue;
        if (r.DestinationPrefix.Prefix.si_family != AF_INET) continue;
        const uint8_t plen = r.DestinationPrefix.PrefixLength;
        if (plen < 1 || plen > 8) continue;
        // Только on-link (NextHop 0.0.0.0) NETMGMT — наши маршруты.
        if (r.NextHop.Ipv4.sin_addr.S_un.S_addr != 0) continue;
        if (r.Protocol != MIB_IPPROTO_NETMGMT) continue;

        MIB_IPFORWARD_ROW2 del = r; // копия для удаления
        DWORD drc = DeleteIpForwardEntry2(&del);
        if (drc != NO_ERROR && drc != ERROR_NOT_FOUND) {
            anyErr = true;
            errAgg += " " + FormatWinErr("DeleteIpForwardEntry2(ladder leaf)", drc);
        }
    }
    FreeMibTable(table);

    if (anyErr) {
        if (outError) *outError = "RouteInstaller::Uninstall partial failure:" + errAgg;
        return false;
    }
    return true;
}

bool RouteInstaller::InstallHostBypass(uint32_t dst_ipv4_be, std::string* outError) {
    // Loopback (127.0.0.0/8) не нуждается в bypass — ОС держит для него
    // отдельный on-link маршрут на loopback-интерфейсе, который заведомо
    // более специфичен, чем наши /1.
    const uint8_t firstOctet = static_cast<uint8_t>(dst_ipv4_be & 0xFF);
    if (firstOctet == 127) {
        return true;
    }

    SOCKADDR_INET dst{};
    dst.si_family = AF_INET;
    dst.Ipv4.sin_family = AF_INET;
    dst.Ipv4.sin_addr.S_un.S_addr = dst_ipv4_be;

    // Ищем текущий лучший маршрут к прокси (ДО установки наших /1-маршрутов).
    MIB_IPFORWARD_ROW2 best{};
    SOCKADDR_INET bestSrc{};
    DWORD rc = GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &best, &bestSrc);
    if (rc != NO_ERROR) {
        if (outError) *outError = FormatWinErr("GetBestRoute2(proxy)", rc);
        return false;
    }

    // Строим /32-маршрут к прокси через найденный интерфейс/next-hop.
    MIB_IPFORWARD_ROW2 row{};
    InitializeIpForwardEntry(&row);
    row.InterfaceLuid = best.InterfaceLuid;
    row.InterfaceIndex = best.InterfaceIndex;
    row.DestinationPrefix.Prefix.si_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = dst_ipv4_be;
    row.DestinationPrefix.PrefixLength = 32;
    row.NextHop = best.NextHop;
    row.Metric = 1;
    row.Protocol = MIB_IPPROTO_NETMGMT;
    row.Origin = NlroManual;
    row.SitePrefixLength = 0;

    rc = CreateIpForwardEntry2(&row);
    if (rc != NO_ERROR && rc != ERROR_OBJECT_ALREADY_EXISTS) {
        if (outError) *outError = FormatWinErr("CreateIpForwardEntry2(proxy/32 bypass)", rc);
        return false;
    }
    return true;
}

bool RouteInstaller::UninstallHostBypass(uint32_t dst_ipv4_be, std::string* outError) {
    const uint8_t firstOctet = static_cast<uint8_t>(dst_ipv4_be & 0xFF);
    if (firstOctet == 127) {
        return true; // ничего не ставили
    }

    SOCKADDR_INET dst{};
    dst.si_family = AF_INET;
    dst.Ipv4.sin_family = AF_INET;
    dst.Ipv4.sin_addr.S_un.S_addr = dst_ipv4_be;

    // Повторно резолвим интерфейс/next-hop для точного совпадения строки.
    MIB_IPFORWARD_ROW2 best{};
    SOCKADDR_INET bestSrc{};
    DWORD rc = GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &best, &bestSrc);
    if (rc != NO_ERROR) {
        // Не смогли найти — вероятно уже снят; не считаем фатальным.
        return true;
    }

    MIB_IPFORWARD_ROW2 row{};
    InitializeIpForwardEntry(&row);
    row.InterfaceLuid = best.InterfaceLuid;
    row.InterfaceIndex = best.InterfaceIndex;
    row.DestinationPrefix.Prefix.si_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = dst_ipv4_be;
    row.DestinationPrefix.PrefixLength = 32;
    row.NextHop = best.NextHop;

    DWORD drc = DeleteIpForwardEntry2(&row);
    if (drc != NO_ERROR && drc != ERROR_NOT_FOUND) {
        if (outError) *outError = FormatWinErr("DeleteIpForwardEntry2(proxy/32 bypass)", drc);
        return false;
    }
    return true;
}

bool RouteInstaller::CleanupStaleBypass(const std::vector<uint32_t>& keep_ipv4_be,
                                        std::string* outError) {
    // Перечисляем IPv4-таблицу; удаляем наши /32 NETMGMT-маршруты (proxy-bypass),
    // адрес которых НЕ входит в keep-множество (актуальные proxy-IP).  Так
    // снимаются «протёкшие» bypass'ы от прошлых запусков с другим proxy.host.
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    DWORD rc = GetIpForwardTable2(AF_INET, &table);
    if (rc != NO_ERROR || table == nullptr) {
        if (outError) *outError = FormatWinErr("GetIpForwardTable2(cleanup-bypass)", rc);
        return false;
    }

    std::string errAgg;
    bool anyErr = false;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& r = table->Table[i];
        if (r.DestinationPrefix.Prefix.si_family != AF_INET) continue;
        if (r.DestinationPrefix.PrefixLength != 32) continue;
        if (r.Protocol != MIB_IPPROTO_NETMGMT) continue;
        // Только маршруты с ненулевым next-hop — наши bypass ведут через физ.
        // gateway; on-link /32 (0.0.0.0 next-hop) не характерны для bypass.
        const uint32_t dst_be = r.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr;
        // 127.x мы никогда не ставим — пропускаем на всякий случай.
        if (static_cast<uint8_t>(dst_be & 0xFF) == 127) continue;
        bool keep = false;
        for (uint32_t k : keep_ipv4_be) { if (k == dst_be) { keep = true; break; } }
        if (keep) continue;

        MIB_IPFORWARD_ROW2 del = r;
        DWORD drc = DeleteIpForwardEntry2(&del);
        if (drc != NO_ERROR && drc != ERROR_NOT_FOUND) {
            anyErr = true;
            errAgg += " " + FormatWinErr("DeleteIpForwardEntry2(stale bypass)", drc);
        }
    }
    FreeMibTable(table);

    if (anyErr) {
        if (outError) *outError = "RouteInstaller::CleanupStaleBypass partial:" + errAgg;
        return false;
    }
    return true;
}

bool RouteInstaller::SetInterfaceMetric(NET_LUID luid, uint32_t metric,
                                        std::string* outError) {
    MIB_IPINTERFACE_ROW row{};
    InitializeIpInterfaceEntry(&row);
    row.Family = AF_INET;
    row.InterfaceLuid = luid;
    DWORD rc = GetIpInterfaceEntry(&row);
    if (rc != NO_ERROR) {
        if (outError) *outError = FormatWinErr("GetIpInterfaceEntry", rc);
        return false;
    }
    row.UseAutomaticMetric = FALSE;
    row.Metric = metric;
    // SitePrefixLength для IPv4 должен быть валиден; после GetIpInterfaceEntry он
    // уже корректен, но на всякий случай обнуляем поле, которое Set не любит.
    row.SitePrefixLength = 0;
    rc = SetIpInterfaceEntry(&row);
    if (rc != NO_ERROR) {
        if (outError) *outError = FormatWinErr("SetIpInterfaceEntry", rc);
        return false;
    }
    return true;
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
