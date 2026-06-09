#pragma once

#include <ntddk.h>

// IOCTL codes
#define IOCTL_REDIRECTOR_BASE 0x8000

#define IOCTL_REDIRECTOR_GET_PENDING \
    CTL_CODE(IOCTL_REDIRECTOR_BASE, 0x800, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

#define IOCTL_REDIRECTOR_ACK_REDIRECT \
    CTL_CODE(IOCTL_REDIRECTOR_BASE, 0x801, METHOD_IN_DIRECT, FILE_ANY_ACCESS)

#define IOCTL_REDIRECTOR_GET_STATS \
    CTL_CODE(IOCTL_REDIRECTOR_BASE, 0x802, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

#define IOCTL_REDIRECTOR_SET_RULES \
    CTL_CODE(IOCTL_REDIRECTOR_BASE, 0x803, METHOD_IN_DIRECT, FILE_ANY_ACCESS)

#define IOCTL_REDIRECTOR_CLEAR_RULES \
    CTL_CODE(IOCTL_REDIRECTOR_BASE, 0x804, METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_REDIRECTOR_QUERY_PROCESS \
    CTL_CODE(IOCTL_REDIRECTOR_BASE, 0x805, METHOD_IN_DIRECT, FILE_ANY_ACCESS)

// Structure for IOCTL data exchange
typedef struct _REDIRECT_DATA_PACKET {
    UINT64 RedirectId;
    HANDLE ProcessId;
    WCHAR ProcessPath[260];
    UINT32 OriginalAddressV4;
    UINT16 OriginalAddressV6[8];
    UINT16 OriginalPort;
    UINT16 RedirectLocalPort;
    BOOLEAN IsIPv6;
    ULONG Padding;
} REDIRECT_DATA_PACKET, *PREDIRECT_DATA_PACKET;

typedef struct _DRIVER_STATS_PACKET {
    ULONG PendingRedirects;
    LONGLONG TotalRedirects;
    LONGLONG FailedRedirects;
    ULONG RulesCount;
    ULONG QueueMaxSize;
} DRIVER_STATS_PACKET, *PDRIVER_STATS_PACKET;

// Initialize IOCTL handling
VOID
IoctlHandler_Initialize(VOID);

// IRP_MJ_DEVICE_CONTROL handler
NTSTATUS
IoctlHandler_DeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
);

// Helper to create the device and symbolic link
NTSTATUS
IoctlHandler_CreateDevice(
    _Out_ PDEVICE_OBJECT* DeviceObject,
    _Out_ PUNICODE_STRING DeviceName,
    _Out_ PUNICODE_STRING SymbolicLinkName
);

VOID
IoctlHandler_DeleteDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PUNICODE_STRING DeviceName,
    _In_ PUNICODE_STRING SymbolicLinkName
);