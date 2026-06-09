#pragma once

#include <ntddk.h>
#include <fwpmk.h>

// Use forward declarations instead of broken fwpsk.h
// fwpsk.h from WDK 10.0.28000.0 is incompatible with MSVC 14.4x+
#include "fwpsk_fwd.h"

// Callout classification function
VOID NTAPI
TcpRedirectClassify(
    _In_ const FWPS_INCOMING_VALUES* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES* inMetaValues,
    _Inout_opt_ void* layerData,
    _In_opt_ const void* classifyContext,
    _In_ const FWPS_FILTER* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT* classifyOut
);

// Callout notification function
NTSTATUS NTAPI
TcpRedirectNotify(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE notifyType,
    _In_ const GUID* filterKey,
    _Inout_ FWPS_FILTER* filter
);

// Flow delete callback
NTSTATUS NTAPI
TcpRedirectFlowDelete(
    _In_ UINT16 layerId,
    _In_ UINT32 calloutId,
    _In_ UINT64 flowContext
);

// Register/unregister callouts
NTSTATUS
WfpCallout_Register(
    _In_ HANDLE engineHandle,
    _In_ HANDLE deviceObject
);

VOID
WfpCallout_Unregister(
    _In_ HANDLE engineHandle
);

// External GUIDs
extern const GUID TCP_REDIRECT_CALLOUT_GUID;
extern const GUID TCP_FLOW_ESTABLISHED_CALLOUT_GUID;
extern const GUID TCP_REDIRECT_PROVIDER_GUID;
extern const GUID TCP_REDIRECT_SUBLAYER_GUID;

// Global redirect handle
extern HANDLE g_redirectHandle;
extern UINT32 g_redirectCalloutId;