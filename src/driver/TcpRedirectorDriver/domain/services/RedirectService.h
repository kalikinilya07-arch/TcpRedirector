#pragma once

#include <ntddk.h>
#include "../entities/RedirectInfo.h"
#include "../ports/IRedirectHandler.h"

typedef struct _REDIRECT_SERVICE {
    // Queue management
    LIST_ENTRY RedirectQueue;
    KSPIN_LOCK QueueLock;
    volatile LONG QueueCount;
    volatile LONG QueueMaxCount;

    // Redirect ID generator
    volatile LONG NextRedirectId;

    // Event for user-mode notification
    KEVENT RedirectEvent;

    // Rules cache
    LIST_ENTRY RulesList;
    KSPIN_LOCK RulesLock;
    volatile LONG RulesCount;

    // Redirect handle (created once)
    HANDLE RedirectHandle;

    // Statistics
    volatile LONGLONG TotalRedirects;
    volatile LONGLONG FailedRedirects;
    volatile LONGLONG TotalBytesRedirected;

} REDIRECT_SERVICE, *PREDIRECT_SERVICE;

// Service initialization and cleanup
NTSTATUS RedirectService_Initialize(_Out_ PREDIRECT_SERVICE Service);
VOID RedirectService_Cleanup(_Inout_ PREDIRECT_SERVICE Service);

// Queue operations
NTSTATUS RedirectService_Enqueue(_Inout_ PREDIRECT_SERVICE Service,
    _In_ PREDIRECT_INFO Info);
NTSTATUS RedirectService_Dequeue(_Inout_ PREDIRECT_SERVICE Service,
    _Out_ PREDIRECT_INFO* Info);
NTSTATUS RedirectService_PeekBatch(_Inout_ PREDIRECT_SERVICE Service,
    _Out_writes_to_(*Count, *Count) PREDIRECT_INFO* Batch,
    _Inout_ ULONG* Count);
NTSTATUS RedirectService_Acknowledge(_Inout_ PREDIRECT_SERVICE Service,
    _In_ UINT64 RedirectId);

// Rules management
NTSTATUS RedirectService_AddRule(_Inout_ PREDIRECT_SERVICE Service,
    _In_ PRULE_ENTRY Rule);
NTSTATUS RedirectService_ClearRules(_Inout_ PREDIRECT_SERVICE Service);
RULE_ACTION_TYPE RedirectService_MatchRule(_In_ PREDIRECT_SERVICE Service,
    _In_ PPROCESS_IDENTITY ProcessId);

// Process identification
NTSTATUS RedirectService_IdentifyProcess(
    _In_ const FWPS_INCOMING_METADATA_VALUES* MetaValues,
    _Out_ PPROCESS_IDENTITY ProcessId);

// Statistics
VOID RedirectService_GetStats(_In_ PREDIRECT_SERVICE Service,
    _Out_ ULONG* PendingCount,
    _Out_ LONGLONG* TotalRedirects,
    _Out_ LONGLONG* FailedRedirects);