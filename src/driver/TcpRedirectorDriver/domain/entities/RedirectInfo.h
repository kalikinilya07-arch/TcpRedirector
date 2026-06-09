#pragma once

#include <ntddk.h>

#define MAX_PROCESS_PATH_LENGTH 260
#define MAX_REDIRECT_QUEUE_SIZE 4096
#define REDIRECT_TIMEOUT_SECONDS 30

typedef enum _REDIRECT_STATUS {
    RedirectPending = 0,
    RedirectAcknowledged = 1,
    RedirectCompleted = 2,
    RedirectFailed = 3,
    RedirectTimedOut = 4
} REDIRECT_STATUS;

typedef struct _REDIRECT_INFO {
    LIST_ENTRY ListEntry;
    UINT64 RedirectId;
    HANDLE ProcessId;
    WCHAR ProcessPath[MAX_PROCESS_PATH_LENGTH];
    UINT32 OriginalAddressV4;
    UINT16 OriginalAddressV6[8];
    UINT16 OriginalPort;
    UINT16 RedirectLocalPort;
    BOOLEAN IsIPv6;
    REDIRECT_STATUS Status;
    UINT64 Timestamp;
    UINT64 FlowHandle;
    KSPIN_LOCK Lock;
} REDIRECT_INFO, *PREDIRECT_INFO;

typedef struct _PROCESS_IDENTITY {
    HANDLE ProcessId;
    WCHAR ProcessName[64];
    WCHAR ProcessPath[MAX_PROCESS_PATH_LENGTH];
} PROCESS_IDENTITY, *PPROCESS_IDENTITY;

typedef enum _RULE_ACTION_TYPE {
    RuleActionPermit = 0,
    RuleActionRedirect = 1,
    RuleActionBlock = 2
} RULE_ACTION_TYPE;

typedef struct _RULE_ENTRY {
    LIST_ENTRY ListEntry;
    UINT32 RuleId;
    RULE_ACTION_TYPE Action;
    BOOLEAN IsWildcard;
    BOOLEAN IsPathRule;
    WCHAR Pattern[MAX_PROCESS_PATH_LENGTH];
    UINT32 Priority;
} RULE_ENTRY, *PRULE_ENTRY;