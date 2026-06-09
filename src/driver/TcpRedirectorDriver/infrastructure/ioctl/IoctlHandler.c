#include <ntddk.h>
#include <fwpmk.h>
#include "../../infrastructure/wfp/fwpsk_fwd.h"
#include "IoctlHandler.h"
#include "../../domain/services/RedirectService.h"

extern REDIRECT_SERVICE g_RedirectService;

VOID
IoctlHandler_Initialize(VOID)
{
    // No additional initialization needed
}

NTSTATUS
IoctlHandler_DeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIO_STACK_LOCATION irpStack;
    ULONG ioctlCode;
    ULONG inputLength, outputLength;
    PVOID inputBuffer, outputBuffer;
    ULONG_PTR bytesReturned = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    irpStack = IoGetCurrentIrpStackLocation(Irp);
    ioctlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;
    inputBuffer = Irp->AssociatedIrp.SystemBuffer;
    inputLength = irpStack->Parameters.DeviceIoControl.InputBufferLength;
    outputBuffer = Irp->AssociatedIrp.SystemBuffer;
    outputLength = irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    switch (ioctlCode) {
    case IOCTL_REDIRECTOR_GET_PENDING:
    {
        // Return pending redirects in batch
        ULONG maxCount = outputLength / sizeof(REDIRECT_DATA_PACKET);
        if (maxCount > 64) maxCount = 64; // Batch limit

        PREDIRECT_INFO redirects[64];
        ULONG actualCount = maxCount;
        NTSTATUS dequeueStatus = RedirectService_PeekBatch(
            &g_RedirectService, redirects, &actualCount);

        if (NT_SUCCESS(dequeueStatus)) {
            PREDIRECT_DATA_PACKET packet = (PREDIRECT_DATA_PACKET)outputBuffer;
            for (ULONG i = 0; i < actualCount; i++) {
                packet[i].RedirectId = redirects[i]->RedirectId;
                packet[i].ProcessId = redirects[i]->ProcessId;
                RtlCopyMemory(packet[i].ProcessPath, redirects[i]->ProcessPath,
                    sizeof(redirects[i]->ProcessPath));
                packet[i].OriginalAddressV4 = redirects[i]->OriginalAddressV4;
                packet[i].OriginalPort = redirects[i]->OriginalPort;
                packet[i].RedirectLocalPort = redirects[i]->RedirectLocalPort;
                packet[i].IsIPv6 = redirects[i]->IsIPv6;
            }
            bytesReturned = actualCount * sizeof(REDIRECT_DATA_PACKET);

            // Free the redirect info structures
            for (ULONG i = 0; i < actualCount; i++) {
                ExFreePoolWithTag(redirects[i], 'nDrT');
            }
            status = STATUS_SUCCESS;
        }
        else {
            status = STATUS_NOT_FOUND;
        }
        break;
    }

    case IOCTL_REDIRECTOR_ACK_REDIRECT:
    {
        if (inputLength >= sizeof(UINT64)) {
            UINT64 redirectId = *(PUINT64)inputBuffer;
            status = RedirectService_Acknowledge(&g_RedirectService, redirectId);
        }
        else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }

    case IOCTL_REDIRECTOR_GET_STATS:
    {
        if (outputLength >= sizeof(DRIVER_STATS_PACKET)) {
            PDRIVER_STATS_PACKET stats = (PDRIVER_STATS_PACKET)outputBuffer;
            ULONG pending;
            LONGLONG total, failed;
            RedirectService_GetStats(&g_RedirectService, &pending, &total, &failed);
            stats->PendingRedirects = pending;
            stats->TotalRedirects = total;
            stats->FailedRedirects = failed;
            stats->RulesCount = (ULONG)g_RedirectService.RulesCount;
            stats->QueueMaxSize = MAX_REDIRECT_QUEUE_SIZE;
            bytesReturned = sizeof(DRIVER_STATS_PACKET);
            status = STATUS_SUCCESS;
        }
        else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }

    case IOCTL_REDIRECTOR_SET_RULES:
    {
        ULONG ruleCount = inputLength / sizeof(RULE_ENTRY);
        if (ruleCount > 0) {
            PRULE_ENTRY rules = (PRULE_ENTRY)inputBuffer;

            // Clear existing rules first
            RedirectService_ClearRules(&g_RedirectService);

            // Add new rules
            for (ULONG i = 0; i < ruleCount; i++) {
                status = RedirectService_AddRule(&g_RedirectService, &rules[i]);
                if (!NT_SUCCESS(status)) {
                    break;
                }
            }
        }
        break;
    }

    case IOCTL_REDIRECTOR_CLEAR_RULES:
    {
        status = RedirectService_ClearRules(&g_RedirectService);
        break;
    }

    case IOCTL_REDIRECTOR_QUERY_PROCESS:
    {
        if (inputLength >= sizeof(HANDLE) && outputLength >= sizeof(PROCESS_IDENTITY)) {
            HANDLE pid = *(PHANDLE)inputBuffer;
            PPROCESS_IDENTITY procInfo = (PPROCESS_IDENTITY)outputBuffer;
            FWPS_INCOMING_METADATA_VALUES meta;
            RtlZeroMemory(&meta, sizeof(meta));
            meta.processId = (UINT64)pid;
            status = RedirectService_IdentifyProcess(&meta, procInfo);
            if (NT_SUCCESS(status)) {
                bytesReturned = sizeof(PROCESS_IDENTITY);
            }
        }
        else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

NTSTATUS
IoctlHandler_CreateDevice(
    _Out_ PDEVICE_OBJECT* DeviceObject,
    _Out_ PUNICODE_STRING DeviceName,
    _Out_ PUNICODE_STRING SymbolicLinkName)
{
    NTSTATUS status;

    RtlInitUnicodeString(DeviceName, L"\\Device\\TcpRedirectorDriver");
    RtlInitUnicodeString(SymbolicLinkName, L"\\DosDevices\\TcpRedirectorDriver");

    status = IoCreateDevice(
        NULL,
        0,
        DeviceName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        DeviceObject);

    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: IoCreateDevice failed: 0x%X\n", status);
        return status;
    }

    status = IoCreateSymbolicLink(SymbolicLinkName, DeviceName);
    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: IoCreateSymbolicLink failed: 0x%X\n", status);
        IoDeleteDevice(*DeviceObject);
        *DeviceObject = NULL;
        return status;
    }

    // Set device to handle IOCTL
    (*DeviceObject)->Flags |= DO_DIRECT_IO;

    DbgPrint("TcpRedirector: Device created successfully\n");
    return STATUS_SUCCESS;
}

VOID
IoctlHandler_DeleteDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PUNICODE_STRING DeviceName,
    _In_ PUNICODE_STRING SymbolicLinkName)
{
    if (DeviceObject != NULL) {
        IoDeleteSymbolicLink(SymbolicLinkName);
        IoDeleteDevice(DeviceObject);
        DbgPrint("TcpRedirector: Device deleted\n");
    }
}