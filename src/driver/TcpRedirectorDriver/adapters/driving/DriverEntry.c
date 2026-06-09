#include <ntddk.h>
#include <fwpmk.h>
#include "../../domain/services/RedirectService.h"
#include "../../infrastructure/wfp/WfpCallout.h"
#include "../../infrastructure/ioctl/IoctlHandler.h"

//
// Global service instance
//
REDIRECT_SERVICE g_RedirectService;

//
// Device globals
//
PDEVICE_OBJECT g_DeviceObject = NULL;
UNICODE_STRING g_DeviceName;
UNICODE_STRING g_SymbolicLinkName;
HANDLE g_EngineHandle = NULL;

//
// Function prototypes
//
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
DRIVER_DISPATCH DriverCreateClose;
DRIVER_DISPATCH DriverDeviceControl;

//
// DriverEntry - called when driver is loaded
//
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    FWPM_SESSION session;

    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrint("TcpRedirector: DriverEntry called\n");

    // Initialize the redirect service
    status = RedirectService_Initialize(&g_RedirectService);
    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: RedirectService_Initialize failed: 0x%X\n", status);
        return status;
    }

    // Set up driver dispatch routines
    DriverObject->DriverUnload = DriverUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DriverCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverDeviceControl;

    // Create device
    status = IoctlHandler_CreateDevice(
        &g_DeviceObject,
        &g_DeviceName,
        &g_SymbolicLinkName);

    if (!NT_SUCCESS(status)) {
        RedirectService_Cleanup(&g_RedirectService);
        return status;
    }

    // Open WFP engine
    RtlZeroMemory(&session, sizeof(session));
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;

    status = FwpmEngineOpen(
        NULL,
        RPC_C_AUTHN_WINNT,
        NULL,
        &session,
        &g_EngineHandle);

    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: FwpmEngineOpen failed: 0x%X\n", status);
        IoctlHandler_DeleteDevice(g_DeviceObject, &g_DeviceName, &g_SymbolicLinkName);
        g_DeviceObject = NULL;
        RedirectService_Cleanup(&g_RedirectService);
        return status;
    }

    // Register WFP callout
    status = WfpCallout_Register(g_EngineHandle, DriverObject);
    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: WfpCallout_Register failed: 0x%X\n", status);
        FwpmEngineClose(g_EngineHandle);
        g_EngineHandle = NULL;
        IoctlHandler_DeleteDevice(g_DeviceObject, &g_DeviceName, &g_SymbolicLinkName);
        g_DeviceObject = NULL;
        RedirectService_Cleanup(&g_RedirectService);
        return status;
    }

    // Initialize IOCTL handler
    IoctlHandler_Initialize();

    DbgPrint("TcpRedirector: Driver loaded successfully\n");
    return STATUS_SUCCESS;
}

//
// DriverUnload - called when driver is unloaded
//
VOID
DriverUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    DbgPrint("TcpRedirector: DriverUnload called\n");

    // Unregister WFP callout
    if (g_EngineHandle != NULL) {
        WfpCallout_Unregister(g_EngineHandle);
        FwpmEngineClose(g_EngineHandle);
        g_EngineHandle = NULL;
    }

    // Delete device
    if (g_DeviceObject != NULL) {
        IoctlHandler_DeleteDevice(g_DeviceObject, &g_DeviceName, &g_SymbolicLinkName);
        g_DeviceObject = NULL;
    }

    // Cleanup service
    RedirectService_Cleanup(&g_RedirectService);

    DbgPrint("TcpRedirector: Driver unloaded successfully\n");
}

//
// IRP_MJ_CREATE / IRP_MJ_CLOSE handler
//
NTSTATUS
DriverCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

//
// IRP_MJ_DEVICE_CONTROL handler
//
NTSTATUS
DriverDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    return IoctlHandler_DeviceControl(DeviceObject, Irp);
}