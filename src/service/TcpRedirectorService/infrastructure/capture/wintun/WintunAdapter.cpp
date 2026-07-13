/**
 * @file WintunAdapter.cpp
 * @brief WP8 — реализация RAII-жизненного цикла Wintun-адаптера + IPv4-конфига.
 */

// ВНИМАНИЕ: порядок инклудов КРИТИЧЕН.
// <netioapi.h>/<ws2ipdef.h> тянут SOCKADDR_IN и NTSTATUS-цепочку, а они
// требуют <winsock2.h> ДО <windows.h>.  Порядок ниже — единственный
// проверенный рецепт на Windows SDK 10.0.26100.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>       // InetPtonW
#include <ws2ipdef.h>       // struct sockaddr_in6, IN_ADDR
#include <windows.h>
#include <iphlpapi.h>       // CreateUnicastIpAddressEntry, SetUnicastIpAddressEntry
#include <netioapi.h>       // MIB_UNICASTIPADDRESS_ROW, InitializeUnicastIpAddressEntry

#include "WintunAdapter.h"

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

// Обеспечиваем линк iphlpapi.lib даже если vcxproj забудут его добавить.
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

namespace {

/**
 * @brief UTF16 → UTF8 без исключений; для форматирования сообщений.
 */
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0,
                                     w.data(), static_cast<int>(w.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "<invalid-utf16>";
    std::string out(static_cast<size_t>(needed), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0,
                                      w.data(), static_cast<int>(w.size()),
                                      out.data(), needed, nullptr, nullptr);
    if (written <= 0) return "<invalid-utf16>";
    return out;
}

/**
 * @brief Возвращает GLE как строку "GLE=NNN".
 */
std::string GleStr(DWORD gle) {
    std::ostringstream oss;
    oss << "GLE=" << gle;
    return oss.str();
}

} // namespace

// ============================================================================
// WintunAdapter::CreateOrOpen
// ============================================================================

std::unique_ptr<WintunAdapter> WintunAdapter::CreateOrOpen(
        std::shared_ptr<WintunApi> api,
        const CreateParams& p,
        std::string* outError) {

    static std::string s_dummy;
    std::string& err = outError ? *outError : s_dummy;
    err.clear();

    if (!api) {
        err = "WintunAdapter::CreateOrOpen: api is null";
        return nullptr;
    }
    if (p.name.empty()) {
        err = "WintunAdapter::CreateOrOpen: adapter name is empty";
        return nullptr;
    }
    if (p.tunnel_type.empty()) {
        err = "WintunAdapter::CreateOrOpen: tunnel_type is empty";
        return nullptr;
    }

    // Wintun принимает pointer-or-null для requested_guid; std::optional
    // мы разворачиваем в локальную GUID и передаём её адрес.
    const GUID* guidPtr = nullptr;
    GUID guidVal{};
    if (p.requested_guid) {
        guidVal = *p.requested_guid;
        guidPtr = &guidVal;
    }

    // Шаг 1: Create.
    SetLastError(0);
    WINTUN_ADAPTER_HANDLE h = api->CreateAdapter(p.name.c_str(),
                                                  p.tunnel_type.c_str(),
                                                  guidPtr);
    DWORD gleCreate = GetLastError();

    // Шаг 2: если Create упал с ERROR_ALREADY_EXISTS — открываем существующий.
    if (h == nullptr && gleCreate == ERROR_ALREADY_EXISTS) {
        SetLastError(0);
        h = api->OpenAdapter(p.name.c_str());
        if (h == nullptr) {
            DWORD gleOpen = GetLastError();
            std::ostringstream oss;
            oss << "WintunCreateAdapter=ERROR_ALREADY_EXISTS but "
                   "WintunOpenAdapter also failed for '"
                << WideToUtf8(p.name) << "': " << GleStr(gleOpen);
            err = oss.str();
            return nullptr;
        }
    } else if (h == nullptr) {
        std::ostringstream oss;
        oss << "WintunCreateAdapter failed for '" << WideToUtf8(p.name)
            << "' (type='" << WideToUtf8(p.tunnel_type) << "'): "
            << GleStr(gleCreate);
        err = oss.str();
        return nullptr;
    }

    // На этом этапе h валиден.  Собираем объект.  Не используем make_unique,
    // потому что конструктор WintunAdapter приватный.
    std::unique_ptr<WintunAdapter> adapter(new WintunAdapter());
    adapter->m_api    = std::move(api);
    adapter->m_handle = h;
    adapter->m_name   = p.name;

    // Извлекаем LUID — это gratis-операция, лишь копирование поля из
    // структуры адаптера внутри DLL.
    adapter->m_api->GetAdapterLUID(h, &adapter->m_luid);

    return adapter;
}

// ============================================================================
// WintunAdapter::~WintunAdapter
// ============================================================================

WintunAdapter::~WintunAdapter() {
    if (m_handle && m_api && m_api->CloseAdapter) {
        // По контракту wintun: CloseAdapter для «созданного» адаптера
        // удаляет его из системы; для «открытого» — просто закрывает.
        // Нам подходит и то и другое — WP10 обеспечивает симметрию через
        // сохранённый флаг «мы владельцы».  Здесь оставляем поведение
        // по-умолчанию (delete-on-close), т.к. в WP8 идентификация
        // владельца ещё не понадобилась.
        m_api->CloseAdapter(m_handle);
        m_handle = nullptr;
    }
}

// ============================================================================
// WintunAdapter::InterfaceIndex
// ============================================================================

NET_IFINDEX WintunAdapter::InterfaceIndex() const {
    NET_IFINDEX idx = 0;
    // ConvertInterfaceLuidToIndex не работает с несуществующими LUID,
    // но у нас LUID заведомо жив (адаптер открыт).  Ошибку молча
    // маскируем: возвращаем 0, вызывающий может решить, что делать.
    if (ConvertInterfaceLuidToIndex(&m_luid, &idx) != NO_ERROR) {
        return 0;
    }
    return idx;
}

// ============================================================================
// WintunAdapter::ParseCidrV4
// ============================================================================

bool WintunAdapter::ParseCidrV4(const std::wstring& cidr,
                                 uint32_t& ipBigEndian,
                                 uint8_t& prefixOut) {
    // Ищем '/'.
    const size_t slash = cidr.find(L'/');
    if (slash == std::wstring::npos || slash == 0 || slash + 1 >= cidr.size()) {
        return false;
    }

    // Разбор IPv4 через InetPtonW (без CRT-atoi).  Копируем в null-terminated
    // временную строку.
    std::wstring ipPart = cidr.substr(0, slash);
    IN_ADDR addr{};
    if (InetPtonW(AF_INET, ipPart.c_str(), &addr) != 1) {
        return false;
    }
    ipBigEndian = addr.S_un.S_addr;  // big-endian (сеть).

    // Разбор префикса.
    int pfx = 0;
    for (size_t i = slash + 1; i < cidr.size(); ++i) {
        wchar_t c = cidr[i];
        if (c < L'0' || c > L'9') return false;
        pfx = pfx * 10 + (c - L'0');
        if (pfx > 32) return false;
    }
    if (pfx < 0 || pfx > 32) return false;
    prefixOut = static_cast<uint8_t>(pfx);
    return true;
}

// ============================================================================
// WintunAdapter::ConfigureIpv4
// ============================================================================

bool WintunAdapter::ConfigureIpv4(const std::wstring& cidr,
                                   std::string* outError) {
    static std::string s_dummy;
    std::string& err = outError ? *outError : s_dummy;
    err.clear();

    if (!m_handle) {
        err = "WintunAdapter::ConfigureIpv4: adapter is not open";
        return false;
    }

    uint32_t ipBE = 0;
    uint8_t  prefix = 0;
    if (!ParseCidrV4(cidr, ipBE, prefix)) {
        err = "WintunAdapter::ConfigureIpv4: invalid CIDR (expected a.b.c.d/N): '"
              + WideToUtf8(cidr) + "'";
        return false;
    }

    MIB_UNICASTIPADDRESS_ROW row{};
    InitializeUnicastIpAddressEntry(&row);
    row.Address.si_family        = AF_INET;
    row.Address.Ipv4.sin_family  = AF_INET;
    row.Address.Ipv4.sin_addr.S_un.S_addr = ipBE;
    row.InterfaceLuid            = m_luid;
    row.OnLinkPrefixLength       = prefix;
    // «Preferred» — минуем DAD; для TUN-адаптера с private IP это стандартно.
    row.DadState                 = IpDadStatePreferred;

    // Сначала Create; если уже есть — Set.
    DWORD rc = CreateUnicastIpAddressEntry(&row);
    if (rc == ERROR_OBJECT_ALREADY_EXISTS) {
        rc = SetUnicastIpAddressEntry(&row);
    }
    if (rc != NO_ERROR) {
        std::ostringstream oss;
        oss << "CreateUnicastIpAddressEntry/Set failed for CIDR '"
            << WideToUtf8(cidr) << "' on adapter '"
            << WideToUtf8(m_name) << "': rc=" << rc;
        err = oss.str();
        return false;
    }
    return true;
}

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
