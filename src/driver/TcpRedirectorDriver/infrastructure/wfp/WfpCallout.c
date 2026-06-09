#include <ntddk.h>
#include <fwpmk.h>
#include "WfpCallout.h"
#include <initguid.h>
#include "../../domain/services/RedirectService.h"

// External global service instance
extern REDIRECT_SERVICE g_RedirectService;

// GUIDs for our callouts
DEFINE_GUID(TCP_REDIRECT_CALLOUT_GUID,
    0xe8f7a8b1, 0x5c3d, 0x4a2e, 0x9b, 0x1f, 0x8d, 0x4e, 0x2c, 0x7a, 0x3b, 0x6f);

DEFINE_GUID(TCP_FLOW_ESTABLISHED_CALLOUT_GUID,
    0xd9f6b9c2, 0x6d4e, 0x5b3f, 0xac, 0x2a, 0x9e, 0x5f, 0x3d, 0x8b, 0x4c, 0x7a);

// FWPM_LAYER_ALE_AUTH_CONNECT_V4: {0xb0e9f5e2-0x2c6e-0x4e5d-0xa8-0x8c-0x3e-0x5b-0x6d-0x77-0x95-0x29}
DEFINE_GUID(FWPM_LAYER_ALE_AUTH_CONNECT_V4,
    0xb0e9f5e2, 0x2c6e, 0x4e5d, 0xa8, 0x8c, 0x3e, 0x5b, 0x6d, 0x77, 0x95, 0x29);

DEFINE_GUID(TCP_REDIRECT_PROVIDER_GUID,
    0xf1a2b3c4, 0x5d6e, 0x7f8a, 0x9b, 0x0c, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b, 0x6c);

DEFINE_GUID(TCP_REDIRECT_SUBLAYER_GUID,
    0xa1b2c3d4, 0xe5f6, 0xa7b8, 0xc9, 0xd0, 0xe1, 0xf2, 0xa3, 0xb4, 0xc5, 0xd6);

// Global redirect handle for TCP connection redirection
HANDLE g_redirectHandle = NULL;
UINT32 g_redirectCalloutId = 0;
UINT32 g_flowCalloutId = 0;

//
// Callout classification function for ALE_AUTH_CONNECT
//
VOID NTAPI
TcpRedirectClassify(
    _In_ const FWPS_INCOMING_VALUES* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES* inMetaValues,
    _Inout_opt_ void* layerData,
    _In_opt_ const void* classifyContext,
    _In_ const FWPS_FILTER* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT* classifyOut)
{
    PROCESS_IDENTITY processId;
    RULE_ACTION_TYPE action;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(filter);
    UNREFERENCED_PARAMETER(flowContext);
    UNREFERENCED_PARAMETER(layerData);

    // Default action: permit (do not block if we can't process)
    classifyOut->actionType = FWP_ACTION_PERMIT;

    // Get the remote address and port
    UINT32 remoteAddr = inFixedValues->
        incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS].value.uint32;
    UINT16 remotePort = inFixedValues->
        incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_PORT].value.uint16;

    // Identify the process
    status = RedirectService_IdentifyProcess(inMetaValues, &processId);
    if (!NT_SUCCESS(status)) {
        return; // Permit
    }

    // Check rules
    action = RedirectService_MatchRule(&g_RedirectService, &processId);
    if (action != RuleActionRedirect) {
        return; // Permit or Block based on rule
    }

    // Perform redirect
    if (g_redirectHandle != NULL && classifyContext != NULL) {
        FWPS_CONNECT_REQUEST* connectRequest = NULL;
        HANDLE classifyHandle = NULL;

        status = FwpsAcquireClassifyHandle0(
            (void*)classifyContext,
            0,
            &classifyHandle);

        if (NT_SUCCESS(status)) {
            status = FwpsAcquireWritableLayerDataPointer0(
                classifyHandle,
                filter->filterId,
                POOL_FLAG_NON_PAGED,
                (PVOID*)&connectRequest,
                classifyOut);

            if (NT_SUCCESS(status) && connectRequest != NULL) {
                // Create and queue redirect info
                PREDIRECT_INFO redirectInfo = (PREDIRECT_INFO)ExAllocatePool2(
                    POOL_FLAG_NON_PAGED,
                    sizeof(REDIRECT_INFO),
                    'nDrT');

                if (redirectInfo != NULL) {
                    RtlZeroMemory(redirectInfo, sizeof(REDIRECT_INFO));
                    redirectInfo->ProcessId = processId.ProcessId;
                    redirectInfo->OriginalAddressV4 = remoteAddr;
                    redirectInfo->OriginalPort = remotePort;
                    redirectInfo->IsIPv6 = FALSE;
                    RtlCopyMemory(redirectInfo->ProcessPath, processId.ProcessPath,
                        sizeof(processId.ProcessPath));

                    // Set up the redirect
                    connectRequest->redirectHandle = g_redirectHandle;

                    status = RedirectService_Enqueue(&g_RedirectService, redirectInfo);
                    if (NT_SUCCESS(status)) {
                        // Flag as redirected
                        classifyOut->actionType = FWP_ACTION_PERMIT;
                    }
                    else {
                        ExFreePoolWithTag(redirectInfo, 'nDrT');
                    }
                }

                FwpsApplyModifiedLayerData0(
                    classifyHandle,
                    (PVOID)connectRequest,
                    0);
            }

            FwpsReleaseClassifyHandle0(classifyHandle);
        }
    }
}

//
// Callout notification function
//
NTSTATUS NTAPI
TcpRedirectNotify(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE notifyType,
    _In_ const GUID* filterKey,
    _Inout_ FWPS_FILTER* filter)
{
    UNREFERENCED_PARAMETER(filterKey);
    UNREFERENCED_PARAMETER(filter);

    switch (notifyType) {
    case FWPS_CALLOUT_NOTIFY_ADD_FILTER:
        DbgPrint("TcpRedirector: Filter added\n");
        break;
    case FWPS_CALLOUT_NOTIFY_DELETE_FILTER:
        DbgPrint("TcpRedirector: Filter deleted\n");
        break;
    }

    return STATUS_SUCCESS;
}

//
// Flow delete callback
//
NTSTATUS NTAPI
TcpRedirectFlowDelete(
    _In_ UINT16 layerId,
    _In_ UINT32 calloutId,
    _In_ UINT64 flowContext)
{
    UNREFERENCED_PARAMETER(layerId);
    UNREFERENCED_PARAMETER(calloutId);
    UNREFERENCED_PARAMETER(flowContext);
    return STATUS_SUCCESS;
}

//
// Register the WFP callout and add filters
//
NTSTATUS
WfpCallout_Register(
    _In_ HANDLE engineHandle,
    _In_ HANDLE deviceObject)
{
    NTSTATUS status;
    FWPS_CALLOUT sCallout;
    FWPM_CALLOUT mCallout;
    FWPM_FILTER filter;
    FWPM_FILTER_CONDITION filterConditions[1];

    // Create redirect handle first
    status = FwpsRedirectHandleCreate0(
        AF_INET,
        &g_redirectHandle);

    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: Failed to create redirect handle: 0x%X\n", status);
        return status;
    }

    // Register kernel callout
    RtlZeroMemory(&sCallout, sizeof(sCallout));
    sCallout.calloutKey = TCP_REDIRECT_CALLOUT_GUID;
    sCallout.flags = 0;
    sCallout.classifyFn = TcpRedirectClassify;
    sCallout.notifyFn = TcpRedirectNotify;
    sCallout.flowDeleteFn = TcpRedirectFlowDelete;

    status = FwpsCalloutRegister((void*)deviceObject, &sCallout, &g_redirectCalloutId);
    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: FwpsCalloutRegister failed: 0x%X\n", status);
        FwpsRedirectHandleDestroy0(g_redirectHandle);
        g_redirectHandle = NULL;
        return status;
    }

    // Add callout to the WFP engine
    RtlZeroMemory(&mCallout, sizeof(mCallout));
    mCallout.calloutKey = TCP_REDIRECT_CALLOUT_GUID;
    mCallout.displayData.name = L"TcpRedirector ALE Connect Callout";
    mCallout.displayData.description = L"Redirects TCP connections through HTTP proxy";
    mCallout.flags = 0;
    mCallout.providerKey = &TCP_REDIRECT_PROVIDER_GUID;
    mCallout.applicableLayer = FWPM_LAYER_ALE_AUTH_CONNECT_V4;

    status = FwpmCalloutAdd(engineHandle, &mCallout, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: FwpmCalloutAdd failed: 0x%X\n", status);
    }

    // Add provider
    FWPM_PROVIDER provider;
    RtlZeroMemory(&provider, sizeof(provider));
    provider.providerKey = TCP_REDIRECT_PROVIDER_GUID;
    provider.displayData.name = L"TcpRedirector Provider";
    provider.displayData.description = L"TcpRedirector WFP Provider";
    provider.flags = 0;
    FwpmProviderAdd(engineHandle, &provider, NULL);

    // Add sublayer
    FWPM_SUBLAYER sublayer;
    RtlZeroMemory(&sublayer, sizeof(sublayer));
    sublayer.subLayerKey = TCP_REDIRECT_SUBLAYER_GUID;
    sublayer.displayData.name = L"TcpRedirector Sublayer";
    sublayer.displayData.description = L"TcpRedirector WFP Sublayer";
    sublayer.providerKey = &TCP_REDIRECT_PROVIDER_GUID;
    sublayer.weight = 0x100;
    FwpmSubLayerAdd(engineHandle, &sublayer, NULL);

    // Add the filter on ALE_AUTH_CONNECT_V4
    RtlZeroMemory(&filter, sizeof(filter));
    filter.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
    filter.displayData.name = L"TcpRedirector ALE Connect Filter";
    filter.displayData.description = L"Redirect TCP connections through HTTP proxy";
    filter.action.type = FWP_ACTION_CALLOUT_TERMINATING;
    filter.action.calloutKey = TCP_REDIRECT_CALLOUT_GUID;
    filter.subLayerKey = TCP_REDIRECT_SUBLAYER_GUID;
    filter.weight.type = FWP_EMPTY;
    filter.numFilterConditions = 0;
    filter.filterCondition = NULL;

    status = FwpmFilterAdd(engineHandle, &filter, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        DbgPrint("TcpRedirector: FwpmFilterAdd failed: 0x%X\n", status);
    }

    DbgPrint("TcpRedirector: WFP callout registered successfully (ID: %u)\n",
        g_redirectCalloutId);

    return STATUS_SUCCESS;
}

//
// Unregister the WFP callout
//
VOID
WfpCallout_Unregister(_In_ HANDLE engineHandle)
{
    if (g_redirectCalloutId != 0) {
        FwpsCalloutUnregisterById(g_redirectCalloutId);
        g_redirectCalloutId = 0;
    }

    // Remove WFP objects
    if (engineHandle != NULL) {
        FwpmFilterDeleteByKey(engineHandle, &TCP_REDIRECT_CALLOUT_GUID);
        FwpmCalloutDeleteByKey(engineHandle, &TCP_REDIRECT_CALLOUT_GUID);
    }

    if (g_redirectHandle != NULL) {
        FwpsRedirectHandleDestroy0(g_redirectHandle);
        g_redirectHandle = NULL;
    }

    DbgPrint("TcpRedirector: WFP callout unregistered\n");
}