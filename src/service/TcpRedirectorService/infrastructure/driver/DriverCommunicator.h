#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <vector>
#include <optional>
#include <thread>
#include <atomic>
#include <cstring>
#include "../../domain/ports/IDriverCommunicator.h"

namespace tcp_redirector {
namespace infrastructure {

// IOCTL codes matching the kernel driver
#define IOCTL_REDIRECTOR_GET_PENDING \
    CTL_CODE(0x8000, 0x800, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_REDIRECTOR_ACK_REDIRECT \
    CTL_CODE(0x8000, 0x801, METHOD_IN_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_REDIRECTOR_GET_STATS \
    CTL_CODE(0x8000, 0x802, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

struct RedirectDataPacket {
    UINT64 RedirectId;
    HANDLE ProcessId;
    wchar_t ProcessPath[260];
    UINT32 OriginalAddressV4;
    UINT16 OriginalAddressV6[8];
    UINT16 OriginalPort;
    UINT16 RedirectLocalPort;
    BOOLEAN IsIPv6;
    ULONG Padding;
};

struct DriverStatsPacket {
    ULONG PendingRedirects;
    LONGLONG TotalRedirects;
    LONGLONG FailedRedirects;
    ULONG RulesCount;
    ULONG QueueMaxSize;
};

class DriverCommunicator : public domain::ports::IDriverCommunicator {
public:
    DriverCommunicator() : m_hDevice(INVALID_HANDLE_VALUE), m_hEvent(NULL) {}

    ~DriverCommunicator() override { Close(); }

    bool Open() override {
        m_hDevice = CreateFileW(L"\\\\.\\TcpRedirectorDriver",
            GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (m_hDevice == INVALID_HANDLE_VALUE) return false;
        m_hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        return m_hEvent != NULL;
    }

    void Close() override {
        if (m_hEvent) { CloseHandle(m_hEvent); m_hEvent = NULL; }
        if (m_hDevice != INVALID_HANDLE_VALUE) { CloseHandle(m_hDevice); m_hDevice = INVALID_HANDLE_VALUE; }
    }

    bool IsOpen() const override { return m_hDevice != INVALID_HANDLE_VALUE; }
    void* GetEventHandle() const override { return m_hEvent; }

    std::vector<domain::RedirectEvent> GetPendingRedirects(uint32_t timeout_ms) override {
        std::vector<domain::RedirectEvent> result;
        if (m_hDevice == INVALID_HANDLE_VALUE) return result;

        RedirectDataPacket packets[64];
        DWORD bytesReturned = 0;
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = m_hEvent;
        ResetEvent(m_hEvent);

        if (!DeviceIoControl(m_hDevice, IOCTL_REDIRECTOR_GET_PENDING,
                NULL, 0, packets, sizeof(packets), &bytesReturned, &overlapped)) {
            if (GetLastError() == ERROR_IO_PENDING) {
                WaitForSingleObject(m_hEvent, timeout_ms);
                GetOverlappedResult(m_hDevice, &overlapped, &bytesReturned, FALSE);
            }
        }

        size_t count = bytesReturned / sizeof(RedirectDataPacket);
        for (size_t i = 0; i < count; i++) {
            domain::RedirectEvent ev;
            ev.redirect_id = packets[i].RedirectId;
            ev.pid = (uint32_t)(uintptr_t)packets[i].ProcessId;
            ev.process_path = packets[i].ProcessPath;
            ev.original_address_v4 = packets[i].OriginalAddressV4;
            ev.original_port = packets[i].OriginalPort;
            ev.redirect_local_port = packets[i].RedirectLocalPort;
            ev.is_ipv6 = packets[i].IsIPv6 != FALSE;
            result.push_back(ev);
        }
        return result;
    }

    bool AckRedirect(uint64_t redirect_id) override {
        if (m_hDevice == INVALID_HANDLE_VALUE) return false;
        DWORD bytesReturned = 0;
        return DeviceIoControl(m_hDevice, IOCTL_REDIRECTOR_ACK_REDIRECT,
            &redirect_id, sizeof(redirect_id), NULL, 0, &bytesReturned, NULL);
    }

    bool UpdateRules(const std::vector<domain::Rule>& rules) override { return true; }

    domain::DriverStats GetStats() override {
        domain::DriverStats stats;
        if (m_hDevice == INVALID_HANDLE_VALUE) return stats;
        DriverStatsPacket packet = {0};
        DWORD bytesReturned = 0;
        if (DeviceIoControl(m_hDevice, IOCTL_REDIRECTOR_GET_STATS,
                NULL, 0, &packet, sizeof(packet), &bytesReturned, NULL)) {
            stats.pending_redirects = packet.PendingRedirects;
            stats.total_redirects = packet.TotalRedirects;
            stats.failed_redirects = packet.FailedRedirects;
            stats.rules_count = packet.RulesCount;
        }
        return stats;
    }

    std::optional<domain::ports::ProcessInfo> QueryProcess(uint32_t pid) override {
        // MVP: process info extracted from redirect events
        return std::nullopt;
    }

private:
    HANDLE m_hDevice;
    HANDLE m_hEvent;
};

} // namespace infrastructure
} // namespace tcp_redirector