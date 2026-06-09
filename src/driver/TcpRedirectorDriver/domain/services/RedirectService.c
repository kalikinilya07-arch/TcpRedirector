#include <ntddk.h>
#include <fwpmk.h>
#include "../../infrastructure/wfp/fwpsk_fwd.h"
#include "RedirectService.h"

// Process functions from ntifs.h (not available in ntddk.h)
__declspec(dllimport) NTSTATUS NTAPI
PsLookupProcessByProcessId(
    _In_  HANDLE    ProcessId,
    _Out_ PEPROCESS* Process
);

__declspec(dllimport) PUNICODE_STRING NTAPI
PsGetProcessImageFileName(
    _In_ PEPROCESS Process
);

__declspec(dllimport) NTSTATUS NTAPI
SeLocateProcessImageName(
    _In_  PEPROCESS        Process,
    _Out_ PUNICODE_STRING* ImageName
);

NTSTATUS
RedirectService_Initialize(_Out_ PREDIRECT_SERVICE Service)
{
    NTSTATUS status;

    if (Service == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Service, sizeof(REDIRECT_SERVICE));

    InitializeListHead(&Service->RedirectQueue);
    InitializeListHead(&Service->RulesList);

    KeInitializeSpinLock(&Service->QueueLock);
    KeInitializeSpinLock(&Service->RulesLock);
    KeInitializeEvent(&Service->RedirectEvent, NotificationEvent, FALSE);

    Service->QueueMaxCount = MAX_REDIRECT_QUEUE_SIZE;
    Service->QueueCount = 0;
    Service->NextRedirectId = 1;
    Service->RulesCount = 0;
    Service->TotalRedirects = 0;
    Service->FailedRedirects = 0;
    Service->TotalBytesRedirected = 0;
    Service->RedirectHandle = NULL;

    return STATUS_SUCCESS;
}

VOID
RedirectService_Cleanup(_Inout_ PREDIRECT_SERVICE Service)
{
    KLOCK_QUEUE_HANDLE lockHandle;
    PLIST_ENTRY entry;
    PREDIRECT_INFO info;

    if (Service == NULL) return;

    // Clear redirect queue
    KeAcquireInStackQueuedSpinLock(&Service->QueueLock, &lockHandle);
    while (!IsListEmpty(&Service->RedirectQueue)) {
        entry = RemoveHeadList(&Service->RedirectQueue);
        info = CONTAINING_RECORD(entry, REDIRECT_INFO, ListEntry);
        ExFreePoolWithTag(info, 'rDrT');
    }
    Service->QueueCount = 0;
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    // Clear rules
    KeAcquireInStackQueuedSpinLock(&Service->RulesLock, &lockHandle);
    while (!IsListEmpty(&Service->RulesList)) {
        entry = RemoveHeadList(&Service->RulesList);
        PRULE_ENTRY rule = CONTAINING_RECORD(entry, RULE_ENTRY, ListEntry);
        ExFreePoolWithTag(rule, 'lRrT');
    }
    Service->RulesCount = 0;
    KeReleaseInStackQueuedSpinLock(&lockHandle);
}

NTSTATUS
RedirectService_Enqueue(_Inout_ PREDIRECT_SERVICE Service, _In_ PREDIRECT_INFO Info)
{
    KLOCK_QUEUE_HANDLE lockHandle;
    NTSTATUS status = STATUS_SUCCESS;

    if (Service == NULL || Info == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireInStackQueuedSpinLock(&Service->QueueLock, &lockHandle);

    if (Service->QueueCount >= Service->QueueMaxCount) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        InterlockedIncrement64(&Service->FailedRedirects);
    }
    else {
        LARGE_INTEGER tickCount;
        Info->RedirectId = (UINT64)InterlockedIncrement(&Service->NextRedirectId);
        Info->Status = RedirectPending;
        KeQueryTickCount(&tickCount);
        Info->Timestamp = (UINT64)tickCount.QuadPart;
        InsertTailList(&Service->RedirectQueue, &Info->ListEntry);
        Service->QueueCount++;
        InterlockedIncrement64(&Service->TotalRedirects);
    }

    KeReleaseInStackQueuedSpinLock(&lockHandle);

    if (NT_SUCCESS(status)) {
        KeSetEvent(&Service->RedirectEvent, IO_NO_INCREMENT, FALSE);
    }

    return status;
}

NTSTATUS
RedirectService_Dequeue(_Inout_ PREDIRECT_SERVICE Service, _Out_ PREDIRECT_INFO* Info)
{
    KLOCK_QUEUE_HANDLE lockHandle;
    PLIST_ENTRY entry;
    NTSTATUS status = STATUS_NOT_FOUND;

    if (Service == NULL || Info == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireInStackQueuedSpinLock(&Service->QueueLock, &lockHandle);

    if (!IsListEmpty(&Service->RedirectQueue)) {
        entry = RemoveHeadList(&Service->RedirectQueue);
        *Info = CONTAINING_RECORD(entry, REDIRECT_INFO, ListEntry);
        Service->QueueCount--;
        status = STATUS_SUCCESS;
    }

    KeReleaseInStackQueuedSpinLock(&lockHandle);

    return status;
}

NTSTATUS
RedirectService_PeekBatch(_Inout_ PREDIRECT_SERVICE Service,
    _Out_writes_to_(*Count, *Count) PREDIRECT_INFO* Batch,
    _Inout_ ULONG* Count)
{
    KLOCK_QUEUE_HANDLE lockHandle;
    ULONG batchSize;
    ULONG i = 0;
    PLIST_ENTRY entry;

    if (Service == NULL || Batch == NULL || Count == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    batchSize = *Count;

    KeAcquireInStackQueuedSpinLock(&Service->QueueLock, &lockHandle);

    while (i < batchSize && !IsListEmpty(&Service->RedirectQueue)) {
        entry = RemoveHeadList(&Service->RedirectQueue);
        Batch[i] = CONTAINING_RECORD(entry, REDIRECT_INFO, ListEntry);
        Service->QueueCount--;
        i++;
    }

    KeReleaseInStackQueuedSpinLock(&lockHandle);

    *Count = i;
    return (i > 0) ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

NTSTATUS
RedirectService_Acknowledge(_Inout_ PREDIRECT_SERVICE Service, _In_ UINT64 RedirectId)
{
    // In this version, acknowledgement is implicit via dequeue
    // Future: support for out-of-order acknowledgement
    UNREFERENCED_PARAMETER(Service);
    UNREFERENCED_PARAMETER(RedirectId);
    return STATUS_SUCCESS;
}

NTSTATUS
RedirectService_AddRule(_Inout_ PREDIRECT_SERVICE Service, _In_ PRULE_ENTRY Rule)
{
    KLOCK_QUEUE_HANDLE lockHandle;
    NTSTATUS status = STATUS_SUCCESS;

    if (Service == NULL || Rule == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    PRULE_ENTRY newRule = (PRULE_ENTRY)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(RULE_ENTRY), 'lRrT');
    if (newRule == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(newRule, Rule, sizeof(RULE_ENTRY));
    RtlZeroMemory(&newRule->ListEntry, sizeof(LIST_ENTRY));

    KeAcquireInStackQueuedSpinLock(&Service->RulesLock, &lockHandle);
    InsertTailList(&Service->RulesList, &newRule->ListEntry);
    Service->RulesCount++;
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    return status;
}

NTSTATUS
RedirectService_ClearRules(_Inout_ PREDIRECT_SERVICE Service)
{
    KLOCK_QUEUE_HANDLE lockHandle;
    PLIST_ENTRY entry;

    if (Service == NULL) return STATUS_INVALID_PARAMETER;

    KeAcquireInStackQueuedSpinLock(&Service->RulesLock, &lockHandle);
    while (!IsListEmpty(&Service->RulesList)) {
        entry = RemoveHeadList(&Service->RulesList);
        PRULE_ENTRY rule = CONTAINING_RECORD(entry, RULE_ENTRY, ListEntry);
        ExFreePoolWithTag(rule, 'lRrT');
    }
    Service->RulesCount = 0;
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    return STATUS_SUCCESS;
}

RULE_ACTION_TYPE
RedirectService_MatchRule(_In_ PREDIRECT_SERVICE Service,
    _In_ PPROCESS_IDENTITY ProcessId)
{
    KLOCK_QUEUE_HANDLE lockHandle;
    PLIST_ENTRY entry;
    RULE_ACTION_TYPE action = RuleActionPermit;

    if (Service == NULL || ProcessId == NULL) {
        return RuleActionPermit;
    }

    KeAcquireInStackQueuedSpinLock(&Service->RulesLock, &lockHandle);

    entry = Service->RulesList.Flink;
    while (entry != &Service->RulesList) {
        PRULE_ENTRY rule = CONTAINING_RECORD(entry, RULE_ENTRY, ListEntry);
        entry = entry->Flink;

        // Simple match: if pattern appears in process name or path
        if (rule->IsWildcard) {
            // Wildcard "*" matches everything
            action = rule->Action;
            break;
        }

        if (rule->IsPathRule) {
            // Match against full path
            if (wcsstr(ProcessId->ProcessPath, rule->Pattern) != NULL) {
                action = rule->Action;
                break;
            }
        }
        else {
            // Match against process name
            if (wcsstr(ProcessId->ProcessName, rule->Pattern) != NULL) {
                action = rule->Action;
                break;
            }
        }
    }

    KeReleaseInStackQueuedSpinLock(&lockHandle);

    return action;
}

NTSTATUS
RedirectService_IdentifyProcess(
    _In_ const FWPS_INCOMING_METADATA_VALUES* MetaValues,
    _Out_ PPROCESS_IDENTITY ProcessId)
{
    NTSTATUS status;
    HANDLE pid;
    PEPROCESS process;
    PUNICODE_STRING imageName;

    if (MetaValues == NULL || ProcessId == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(ProcessId, sizeof(PROCESS_IDENTITY));

    // Get PID from metadata
    ProcessId->ProcessId = (HANDLE)MetaValues->processId;

    // Get the process object
    status = PsLookupProcessByProcessId(ProcessId->ProcessId, &process);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Get process image name (short name)
    imageName = PsGetProcessImageFileName(process);
    if (imageName != NULL) {
        RtlCopyMemory(ProcessId->ProcessName, imageName->Buffer,
            min(imageName->Length, sizeof(ProcessId->ProcessName) - 2));
    }

    // Get full process path (may fail in some contexts)
    status = SeLocateProcessImageName(process, &imageName);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(ProcessId->ProcessPath, imageName->Buffer,
            min(imageName->Length, sizeof(ProcessId->ProcessPath) - 2));
    }

    ObDereferenceObject(process);
    return STATUS_SUCCESS;
}

VOID
RedirectService_GetStats(_In_ PREDIRECT_SERVICE Service,
    _Out_ ULONG* PendingCount,
    _Out_ LONGLONG* TotalRedirects,
    _Out_ LONGLONG* FailedRedirects)
{
    if (Service == NULL) return;

    if (PendingCount) *PendingCount = (ULONG)Service->QueueCount;
    if (TotalRedirects) *TotalRedirects = Service->TotalRedirects;
    if (FailedRedirects) *FailedRedirects = Service->FailedRedirects;
}