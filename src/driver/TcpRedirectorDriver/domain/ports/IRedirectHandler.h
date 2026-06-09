#pragma once

#include "../../infrastructure/wfp/fwpsk_fwd.h"
#include "../entities/RedirectInfo.h"

typedef struct _CLASSIFY_CONTEXT {
    HANDLE ClassifyHandle;
    FWPS_CLASSIFY_OUT* ClassifyOut;
    const FWPS_INCOMING_VALUES* FixedValues;
    const FWPS_INCOMING_METADATA_VALUES* MetaValues;
} CLASSIFY_CONTEXT, *PCLASSIFY_CONTEXT;

typedef NTSTATUS (*RedirectHandlerClassify)(
    _In_ PCLASSIFY_CONTEXT Context,
    _Out_ PREDIRECT_INFO RedirectInfo
);

typedef NTSTATUS (*RedirectHandlerNotify)(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE NotifyType,
    _In_ const GUID* FilterKey,
    _Inout_ FWPS_FILTER* Filter
);

typedef NTSTATUS (*RedirectHandlerFlowDelete)(
    _In_ UINT16 LayerId,
    _In_ UINT32 CalloutId,
    _In_ UINT64 FlowContext
);

typedef struct _REDIRECT_HANDLER_VTABLE {
    RedirectHandlerClassify Classify;
    RedirectHandlerNotify Notify;
    RedirectHandlerFlowDelete FlowDelete;
} REDIRECT_HANDLER_VTABLE, *PREDIRECT_HANDLER_VTABLE;