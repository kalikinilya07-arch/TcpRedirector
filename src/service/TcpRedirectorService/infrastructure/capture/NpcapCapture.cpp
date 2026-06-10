#include "NpcapCapture.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <psapi.h>
#include <string>
#include <tlhelp32.h>

// TCP_TABLE_OWNER_PID_CONNECT may not be defined with WIN32_LEAN_AND_MEAN
#ifndef TCP_TABLE_OWNER_PID_CONNECT
#define TCP_TABLE_OWNER_PID_CONNECT 5
#endif

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

namespace tcp_redirector {
namespace infrastructure {

NpcapCapture::NpcapCapture() {
    m_hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

NpcapCapture::~NpcapCapture() {
    Close();
    if (m_hEvent) {
        CloseHandle(m_hEvent);
        m_hEvent = nullptr;
    }
}

bool NpcapCapture::LoadPcapApi() {
    return m_api.Load();
}

bool NpcapCapture::Open() {
    if (m_running) return true;

    if (!LoadPcapApi()) {
        return false;
    }

    char errbuf[PCAP_ERRBUF_SIZE] = {0};

    // Enumerate network devices
    pcap_if_t* allDevs = nullptr;
    if (m_api.findalldevs(&allDevs, errbuf) != 0) {
        m_api.Unload();
        return false;
    }

    // List ALL available devices for diagnostics
    printf("[Npcap] Available devices:\n");
    int devIdx = 0;
    for (pcap_if_t* d = allDevs; d != nullptr; d = d->next) {
        printf("  [%d] %s", devIdx++, d->name);
        if (d->description) printf(" (%s)", d->description);
        if (d->flags & PCAP_IF_LOOPBACK) printf(" [LOOPBACK]");
        if (d->flags & PCAP_IF_UP) printf(" [UP]");
        else printf(" [DOWN]");
        printf("\n");
    }

    // Select best device: skip WAN miniports, Bluetooth, Wi-Fi Direct Virtual
    // Prefer real Ethernet/WiFi adapters
    const char* device = nullptr;
    const char* fallback = nullptr;

    auto is_virtual = [](const char* desc) -> bool {
        if (!desc) return false;
        std::string d(desc);
        return d.find("WAN Miniport") != std::string::npos ||
               d.find("Bluetooth") != std::string::npos ||
               d.find("Wi-Fi Direct Virtual") != std::string::npos;
    };

    for (pcap_if_t* d = allDevs; d != nullptr; d = d->next) {
        if ((d->flags & PCAP_IF_LOOPBACK) || !(d->flags & PCAP_IF_UP))
            continue;
        if (is_virtual(d->description))
            continue;
        // Prefer Ethernet over WiFi
        if (d->description && strstr(d->description, "Ethernet")) {
            device = d->name;
            break; // Best choice
        }
        if (!device && d->description &&
            (strstr(d->description, "Wi-Fi") || strstr(d->description, "Wireless"))) {
            device = d->name;
        }
        if (!fallback) fallback = d->name;
    }

    if (!device) device = fallback; // Any non-virtual adapter
    if (!device) device = "any";    // Last resort

    // Open capture device
    printf("[Npcap] Opening device: %s\n", device);
    m_handle = m_api.open_live(device, 65536, 1, 1000, errbuf);
    m_api.freealldevs(allDevs);

    if (!m_handle) {
        printf("[Npcap] ERROR opening device: %s\n", errbuf);
        m_api.Unload();
        return false;
    }
    printf("[Npcap] Device opened successfully\n");

    // Set filter: outgoing TCP SYN (no ACK) = new connection attempts
    // Exclude loopback
    struct bpf_program fp;
    // Capture ALL TCP SYN (including loopback — client may use 127.0.0.1 proxy)
    const char* filter = "tcp[tcpflags] & (tcp-syn) != 0 and tcp[tcpflags] & (tcp-ack) == 0";
    if (m_api.compile(m_handle, &fp, filter, 1, PCAP_NETMASK_UNKNOWN) == 0) {
        m_api.setfilter(m_handle, &fp);
        m_api.freecode(&fp);
    }

    // Start capture thread
    m_running = true;
    m_initialized = true;
    m_captureThread = std::thread(&NpcapCapture::CaptureLoop, this);

    return true;
}

void NpcapCapture::Close() {
    m_running = false;

    if (m_handle) {
        m_api.breakloop(m_handle);
    }

    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }

    if (m_handle) {
        m_api.close(m_handle);
        m_handle = nullptr;
    }

    m_api.Unload();
    m_initialized = false;
}

bool NpcapCapture::IsOpen() const {
    return m_initialized && m_running;
}

void NpcapCapture::CaptureLoop() {
    printf("[Npcap] Capture loop started (SYN filter)\n");
    while (m_running) {
        int ret = m_api.dispatch(m_handle, 10, PacketHandler,
                                 reinterpret_cast<u_char*>(this));
        if (ret < 0) break;
    }
    printf("[Npcap] Capture loop ended. Total packets: %llu\n",
           static_cast<unsigned long long>(m_stats.packets_captured));
}

void NpcapCapture::PacketHandler(u_char* user, const struct pcap_pkthdr* header,
                                  const u_char* packet) {
    auto* self = reinterpret_cast<NpcapCapture*>(user);
    self->ProcessSynPacket(packet, header->len);
}

void NpcapCapture::ProcessSynPacket(const u_char* packet, uint32_t length) {
    if (length < 40) return; // Ethernet(14) + IP(20) + TCP min

    m_stats.packets_captured++;

    const u_char* ip_hdr = packet + 14;

    // Validate IP version
    uint8_t ip_ver = (ip_hdr[0] >> 4) & 0x0F;
    if (ip_ver != 4) return; // IPv4 only for MVP

    // Extract IP header fields (big-endian)
    uint8_t ip_hdr_len = (ip_hdr[0] & 0x0F) * 4;
    uint32_t src_addr = (ip_hdr[12] << 24) | (ip_hdr[13] << 16) |
                        (ip_hdr[14] << 8) | ip_hdr[15];
    uint32_t dst_addr = (ip_hdr[16] << 24) | (ip_hdr[17] << 16) |
                        (ip_hdr[18] << 8) | ip_hdr[19];

    // Extract TCP header fields
    const u_char* tcp_hdr = ip_hdr + ip_hdr_len;
    uint16_t src_port = (tcp_hdr[0] << 8) | tcp_hdr[1];
    uint16_t dst_port = (tcp_hdr[2] << 8) | tcp_hdr[3];

    // Find PID by source port
    uint32_t pid = FindPidBySourcePort(src_port, dst_addr, dst_port);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        m_stats.pid_lookups_failed++;
        return;
    }

    // Get process info
    auto processName = GetProcessNameByPid(pid);
    auto processPath = GetProcessPathByPid(pid);
    if (processName.empty()) {
        m_stats.pid_lookups_failed++;
        return;
    }

    m_stats.syn_filtered++;

    // Create redirect event
    domain::RedirectEvent event;
    event.redirect_id = m_nextRedirectId++;
    event.pid = pid;
    event.process_path = processPath;
    event.original_address_v4 = dst_addr;
    event.original_port = dst_port;
    event.redirect_local_port = 0; // Will be assigned by proxy engine
    event.is_ipv6 = false;

    m_stats.redirects_emitted++;
    EnqueueEvent(event);

    // Signal event for polling
    SetEvent(m_hEvent);
}

uint32_t NpcapCapture::FindPidBySourcePort(uint16_t src_port,
                                            uint32_t dst_addr, uint16_t dst_port) {
    // Use GetExtendedTcpTable to find the PID owning a local port
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                        static_cast<TCP_TABLE_CLASS>(TCP_TABLE_OWNER_PID_CONNECT), 0);

    std::vector<uint8_t> buffer(size);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());

    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                             static_cast<TCP_TABLE_CLASS>(TCP_TABLE_OWNER_PID_CONNECT), 0) != NO_ERROR) {
        return 0;
    }

    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        const auto& row = table->table[i];
        // Match by local port and remote address/port
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) == src_port &&
            row.dwRemoteAddr == dst_addr &&
            ntohs(static_cast<u_short>(row.dwRemotePort)) == dst_port) {
            return row.dwOwningPid;
        }
    }

    return 0;
}

void NpcapCapture::EnqueueEvent(const domain::RedirectEvent& event) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_eventQueue.size() < MAX_QUEUE_SIZE) {
        m_eventQueue.push(event);
    }
}

std::vector<domain::RedirectEvent> NpcapCapture::DrainQueue() {
    std::vector<domain::RedirectEvent> result;
    std::lock_guard<std::mutex> lock(m_queueMutex);
    while (!m_eventQueue.empty()) {
        result.push_back(m_eventQueue.front());
        m_eventQueue.pop();
    }
    return result;
}

std::vector<domain::RedirectEvent> NpcapCapture::GetPendingRedirects(
    uint32_t timeout_ms) {
    if (!m_running) return {};

    // Wait for events or timeout
    if (timeout_ms > 0) {
        WaitForSingleObject(m_hEvent, timeout_ms);
    }
    ResetEvent(m_hEvent);

    return DrainQueue();
}

bool NpcapCapture::AckRedirect(uint64_t redirect_id) {
    // In Npcap model, ack is no-op — events are fire-and-forget
    (void)redirect_id;
    return true;
}

bool NpcapCapture::UpdateRules(const std::vector<domain::Rule>& rules) {
    std::lock_guard<std::mutex> lock(m_rulesMutex);
    m_rules = rules;
    return true;
}

domain::DriverStats NpcapCapture::GetStats() {
    domain::DriverStats stats;
    stats.pending_redirects = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        stats.pending_redirects = static_cast<uint32_t>(m_eventQueue.size());
    }
    stats.total_redirects = m_stats.redirects_emitted;
    stats.failed_redirects = m_stats.pid_lookups_failed;
    stats.rules_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_rulesMutex);
        stats.rules_count = static_cast<uint32_t>(m_rules.size());
    }
    return stats;
}

std::optional<domain::ports::ProcessInfo> NpcapCapture::QueryProcess(uint32_t pid) {
    domain::ports::ProcessInfo info;
    info.pid = pid;
    info.name = GetProcessNameByPid(pid);
    info.path = GetProcessPathByPid(pid);
    if (info.name.empty()) return std::nullopt;
    return info;
}

void* NpcapCapture::GetEventHandle() const {
    return m_hEvent;
}

NpcapCapture::CaptureStats NpcapCapture::GetCaptureStats() const {
    return m_stats;
}

// Static helpers

bool NpcapCapture::IsNpcapInstalled() {
    PcapApi api;
    return api.Load();
}

std::string NpcapCapture::IpToString(uint32_t addr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
             (addr >> 8) & 0xFF, addr & 0xFF);
    return buf;
}

std::wstring NpcapCapture::GetProcessNameByPid(uint32_t pid) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return L"";

    PROCESSENTRY32W pe = { sizeof(pe) };
    std::wstring name;
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                name = pe.szExeFile;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    return name;
}

std::wstring NpcapCapture::GetProcessPathByPid(uint32_t pid) {
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return L"";

    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &size);
    CloseHandle(hProcess);

    return ok ? std::wstring(path) : L"";
}

} // namespace infrastructure
} // namespace tcp_redirector
