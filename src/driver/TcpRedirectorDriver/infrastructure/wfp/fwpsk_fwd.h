#pragma once

//
// Forward declarations for fwps/fwpm kernel types and functions.
// Used by the local fwpsk.h stub to avoid the broken WDK fwpsk.h.
//

#include <ntddk.h>
#include <fwptypes.h>   // FWP_BYTE_BLOB, FWP_DIRECTION, etc.
#include <ws2def.h>      // ADDRESS_FAMILY, SOCKADDR, SCOPE_ID, WSACMSGHDR, etc.

// ========== FWPS_FIELD_* constants ==========

// ALE_AUTH_CONNECT_V4 layer fields
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_ALE_APP_ID              0
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_ALE_USER_ID             1
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_ADDRESS        2
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_ADDRESS_TYPE   3
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_PORT           4
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_PROTOCOL             5
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS       6
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_PORT          7
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_ALE_REMOTE_USER_ID      8
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_ALE_REMOTE_MACHINE_ID   9
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_DESTINATION_ADDRESS_TYPE 10
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_INTERFACE      11
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_FLAGS                   12
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_INTERFACE_TYPE          13
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_TUNNEL_TYPE             14

#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_ICMP_TYPE \
    FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_PORT
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_ICMP_CODE \
    FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_PORT

// ========== FWP_ACTION_* constants ==========
#ifndef FWP_ACTION_PERMIT
#define FWP_ACTION_PERMIT               (0x00000000)
#define FWP_ACTION_BLOCK                (0x00000001)
#define FWP_ACTION_CALLOUT_TERMINATING  (0x00000002)
#define FWP_ACTION_CALLOUT_INSPECTION   (0x00000003)
#define FWP_ACTION_CALLOUT_UNKNOWN      (0x00000004)
#endif

// ========== FWP weight types ==========
#ifndef FWP_EMPTY
#define FWP_EMPTY                       (0)
#define FWP_UINT8                       (1)
#define FWP_UINT16                      (2)
#define FWP_UINT32                      (3)
#define FWP_UINT64                      (4)
#endif

// ========== FWPS callout flags ==========
#ifndef FWPS_CALLOUT_FLAG_CONDITIONAL_ON_FLOW
#define FWPS_CALLOUT_FLAG_CONDITIONAL_ON_FLOW   (0x00000001)
#define FWPS_CALLOUT_FLAG_RECLASSIFY_IF         (0x00000002)
#endif

// ========== Sub-structures for FWPS_INCOMING_METADATA_VALUES0 ==========

// FWPS_DISCARD_MODULE0 (from shared/fwpstypes.h)
typedef enum _FWPS_DISCARD_MODULE0 {
    FWPS_DISCARD_MODULE_GENERAL   = 0,
    FWPS_DISCARD_MODULE_TRANSPORT = 1,
    FWPS_DISCARD_MODULE_NETWORK   = 2,
    FWPS_DISCARD_MODULE_MAX       = 3
} FWPS_DISCARD_MODULE0;

// FWPS_DISCARD_METADATA0 (from shared/fwpstypes.h)
typedef struct _FWPS_DISCARD_METADATA0 {
    FWPS_DISCARD_MODULE0 discardModule;
    UINT32               discardReason;
    UINT64               filterId;
} FWPS_DISCARD_METADATA0;

// FWPS_INBOUND_FRAGMENT_METADATA0 (from shared/fwpstypes.h)
typedef struct _FWPS_INBOUND_FRAGMENT_METADATA0 {
    UINT32 fragmentIdentification;
    UINT16 fragmentOffset;
    ULONG  fragmentLength;
} FWPS_INBOUND_FRAGMENT_METADATA0;

// IP_ADDRESS_PREFIX (simplified)
typedef struct _IP_ADDRESS_PREFIX {
    UINT8   prefixLength;
    UINT32  address;
} IP_ADDRESS_PREFIX;

// NDIS typedefs (needed for metadata structure)
typedef UINT32 NDIS_SWITCH_PORT_ID;
typedef UINT16 NDIS_SWITCH_NIC_INDEX;

// NOTE: SCOPE_ID, WSACMSGHDR, ADDRESS_FAMILY, SOCKADDR come from <ws2def.h>
// NOTE: FWP_BYTE_BLOB, FWP_DIRECTION come from <fwptypes.h>

// ========== FWPS_INCOMING_METADATA_VALUES0 (correct layout from WDK fwpsk.h) ==========
typedef struct _FWPS_INCOMING_METADATA_VALUES0 {
    UINT32                          currentMetadataValues;
    UINT32                          flags;
    UINT64                          reserved;
    FWPS_DISCARD_METADATA0          discardMetadata;
    UINT64                          flowHandle;
    UINT32                          ipHeaderSize;
    UINT32                          transportHeaderSize;
    FWP_BYTE_BLOB*                  processPath;
    UINT64                          token;
    UINT64                          processId;           // ← PID here
    UINT32                          sourceInterfaceIndex;
    UINT32                          destinationInterfaceIndex;
    ULONG                           compartmentId;
    FWPS_INBOUND_FRAGMENT_METADATA0 fragmentMetadata;
    ULONG                           pathMtu;
    HANDLE                          completionHandle;
    UINT64                          transportEndpointHandle;
    SCOPE_ID                        remoteScopeId;
    WSACMSGHDR*                     controlData;
    ULONG                           controlDataLength;
    FWP_DIRECTION                   packetDirection;
#if 1
    PVOID                           headerIncludeHeader;
    ULONG                           headerIncludeHeaderLength;
#if 1
    IP_ADDRESS_PREFIX               destinationPrefix;
    UINT16                          frameLength;
    UINT64                          parentEndpointHandle;
    UINT32                          icmpIdAndSequence;
    DWORD                           localRedirectTargetPID;
    SOCKADDR*                       originalDestination;
#if 1
    HANDLE                          redirectRecords;
    UINT32                          currentL2MetadataValues;
    UINT32                          l2Flags;
    UINT32                          ethernetMacHeaderSize;
    UINT32                          wiFiOperationMode;
    NDIS_SWITCH_PORT_ID             vSwitchSourcePortId;
    NDIS_SWITCH_NIC_INDEX           vSwitchSourceNicIndex;
    NDIS_SWITCH_PORT_ID             vSwitchDestinationPortId;
    UINT32                          padding0;
    USHORT                          padding1;
    UINT32                          padding2;
    HANDLE                          vSwitchPacketContext;
#endif
#endif
#endif
#if 1
    PVOID                           subProcessTag;
    UINT64                          reserved1;
#endif
} FWPS_INCOMING_METADATA_VALUES0;

// Backward compatibility typedef
typedef FWPS_INCOMING_METADATA_VALUES0 FWPS_INCOMING_METADATA_VALUES;

// ========== FWPS_CONNECT_REQUEST ==========
typedef struct _FWPS_CONNECT_REQUEST {
    UINT64          localAddress;
    UINT16          localPort;
    UINT16          remotePort;
    UINT64          localAddressV6[4];
    UINT64          remoteAddressV6[4];
    UINT32          ipVersion;
    ADDRESS_FAMILY  remoteAddressFamily;
    HANDLE          redirectHandle;
    UINT32          flags;
    ULONG           headerIncludeHeaderLength;
    UCHAR           headerIncludeHeaders[1];
    UCHAR           padding[3];
} FWPS_CONNECT_REQUEST;

// ========== FWPS_CLASSIFY_OUT ==========
typedef struct _FWPS_CLASSIFY_OUT {
    UINT64  actionType;
    UINT32  outFlags;
    UINT32  reserved;
    UINT32  rights;
} FWPS_CLASSIFY_OUT;

// ========== FWPS_INCOMING_VALUES (simplified) ==========
typedef struct _FWPS_INCOMING_VALUES {
    UINT16 layerId;
    UINT32 valueCount;
    struct {
        UINT16 id;
        UINT32 type;
        union {
            UINT8   uint8;
            UINT16  uint16;
            UINT32  uint32;
            UINT64  uint64;
            void*   ptr;
        } value;
    } incomingValue[1];
} FWPS_INCOMING_VALUES;

// ========== FWPS_FILTER ==========
typedef struct _FWPS_FILTER {
    UINT32 filterId;
} FWPS_FILTER;

// ========== FWPS_CALLOUT_NOTIFY_TYPE ==========
typedef enum _FWPS_CALLOUT_NOTIFY_TYPE {
    FWPS_CALLOUT_NOTIFY_ADD_FILTER    = 0,
    FWPS_CALLOUT_NOTIFY_DELETE_FILTER = 1,
    FWPS_CALLOUT_NOTIFY_MAX           = 2
} FWPS_CALLOUT_NOTIFY_TYPE;

// ========== FWPS_CALLOUT ==========
typedef struct _FWPS_CALLOUT FWPS_CALLOUT;

typedef VOID (NTAPI *FWPS_CALLOUT_CLASSIFY_FN)(
    _In_      const FWPS_INCOMING_VALUES*           inFixedValues,
    _In_      const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _Inout_opt_ void*                               layerData,
    _In_opt_  const void*                           classifyContext,
    _In_      const FWPS_FILTER*                    filter,
    _In_      UINT64                                flowContext,
    _Inout_   FWPS_CLASSIFY_OUT*                    classifyOut
);

typedef NTSTATUS (NTAPI *FWPS_CALLOUT_NOTIFY_FN)(
    _In_    FWPS_CALLOUT_NOTIFY_TYPE notifyType,
    _In_    const GUID*              filterKey,
    _Inout_ FWPS_FILTER*             filter
);

typedef NTSTATUS (NTAPI *FWPS_CALLOUT_FLOW_DELETE_FN)(
    _In_ UINT16 layerId,
    _In_ UINT32 calloutId,
    _In_ UINT64 flowContext
);

struct _FWPS_CALLOUT {
    GUID                        calloutKey;
    UINT32                      flags;
    FWPS_CALLOUT_CLASSIFY_FN    classifyFn;
    FWPS_CALLOUT_NOTIFY_FN      notifyFn;
    FWPS_CALLOUT_FLOW_DELETE_FN flowDeleteFn;
};

// ========== Function imports from fwpsk.lib ==========

__declspec(dllimport) NTSTATUS NTAPI
FwpsAcquireClassifyHandle0(
    _In_  const void* classifyContext,
    _In_  UINT32      flags,
    _Out_ HANDLE*     classifyHandle
);

__declspec(dllimport) NTSTATUS NTAPI
FwpsAcquireWritableLayerDataPointer0(
    _In_    HANDLE              classifyHandle,
    _In_    UINT64              filterId,
    _In_    UINT32              flags,
    _Out_   PVOID*              writableLayerData,
    _Inout_ FWPS_CLASSIFY_OUT*  classifyOut
);

__declspec(dllimport) VOID NTAPI
FwpsApplyModifiedLayerData0(
    _In_ HANDLE classifyHandle,
    _In_ PVOID  modifiedLayerData,
    _In_ UINT32 flags
);

__declspec(dllimport) VOID NTAPI
FwpsReleaseClassifyHandle0(
    _In_ HANDLE classifyHandle
);

__declspec(dllimport) NTSTATUS NTAPI
FwpsRedirectHandleCreate0(
    _In_  ADDRESS_FAMILY af,
    _Out_ HANDLE*        redirectHandle
);

__declspec(dllimport) NTSTATUS NTAPI
FwpsCalloutRegister(
    _In_  void*              deviceObject,
    _In_  const FWPS_CALLOUT* callout,
    _Out_ UINT32*             calloutId
);

__declspec(dllimport) NTSTATUS NTAPI
FwpsRedirectHandleDestroy0(
    _In_ HANDLE redirectHandle
);

__declspec(dllimport) NTSTATUS NTAPI
FwpsCalloutUnregisterById(
    _In_ UINT32 calloutId
);
