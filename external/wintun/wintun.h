/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2021 WireGuard LLC. All Rights Reserved.
 *
 * This is the public API for wintun.dll.  Only the subset used by
 * TcpRedirector is declared here.  Types and constants mirror the
 * official header published at:
 *   https://git.zx2c4.com/wintun/tree/api/wintun.h
 *
 * We deliberately DO NOT #include the vendor header from a network
 * download; the signatures below match wintun 0.14.x exactly and are
 * documented on https://www.wintun.net/ .
 *
 * Consumers must resolve every function via GetProcAddress() from
 * wintun.dll — this header only supplies typedefs.  See
 * infrastructure/capture/wintun/WintunApi.h for the loader.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <ifdef.h>      /* NET_LUID, NET_IFINDEX */
#include <guiddef.h>    /* GUID */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque handle types.  We only ever hold pointers; the underlying
 * struct definitions live inside wintun.dll and are intentionally
 * incomplete here.
 */
typedef struct _WINTUN_ADAPTER *WINTUN_ADAPTER_HANDLE;
typedef struct _WINTUN_SESSION *WINTUN_SESSION_HANDLE;

/*
 * Ring buffer capacity range (from wintun docs).  Must be a power of two.
 * Minimum 128 KiB, maximum 64 MiB.  We use 4 MiB by default (see
 * WintunSession::Start below).
 */
#define WINTUN_MIN_RING_CAPACITY  0x20000     /* 128 KiB */
#define WINTUN_MAX_RING_CAPACITY  0x4000000   /* 64 MiB  */

/* Maximum size of a single IP packet exchanged over Wintun. */
#define WINTUN_MAX_IP_PACKET_SIZE 0xFFFF

/* ---------------------------------------------------------------------------
 * Adapter management
 * ------------------------------------------------------------------------- */

/**
 * Creates a new Wintun adapter.
 *
 * @param Name          Unicode identifier of the adapter (max 128 chars).
 * @param TunnelType    Vendor / product string; Wintun surfaces this in
 *                      the adapter's display metadata.
 * @param RequestedGUID Optional pointer to a GUID to assign to the
 *                      adapter (recommended for stability across
 *                      restarts).  Pass NULL to let the OS assign one.
 * @return WINTUN_ADAPTER_HANDLE on success, NULL on failure (call
 *         GetLastError()).  Common errors: ERROR_ALREADY_EXISTS,
 *         ERROR_ACCESS_DENIED (non-admin).
 */
typedef WINTUN_ADAPTER_HANDLE (WINAPI *WINTUN_CREATE_ADAPTER_FUNC)(
    LPCWSTR Name, LPCWSTR TunnelType, const GUID *RequestedGUID);

/**
 * Opens an existing Wintun adapter by name.  Fails with
 * ERROR_FILE_NOT_FOUND if no adapter matches.
 */
typedef WINTUN_ADAPTER_HANDLE (WINAPI *WINTUN_OPEN_ADAPTER_FUNC)(LPCWSTR Name);

/**
 * Closes an adapter handle.  If the adapter was created (not just
 * opened), this also removes the adapter from the system.
 */
typedef void (WINAPI *WINTUN_CLOSE_ADAPTER_FUNC)(WINTUN_ADAPTER_HANDLE Adapter);

/**
 * Uninstalls the Wintun driver from the system.  Not used at runtime;
 * exposed for completeness.
 */
typedef BOOL (WINAPI *WINTUN_DELETE_DRIVER_FUNC)(void);

/**
 * Retrieves the NET_LUID of the given adapter.  Never fails on a valid
 * handle.
 */
typedef void (WINAPI *WINTUN_GET_ADAPTER_LUID_FUNC)(
    WINTUN_ADAPTER_HANDLE Adapter, NET_LUID *Luid);

/**
 * Returns the version of the loaded Wintun driver, packed as
 * (major << 16) | minor.  Zero on failure (GetLastError()).
 */
typedef DWORD (WINAPI *WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC)(void);

/* ---------------------------------------------------------------------------
 * Data session
 * ------------------------------------------------------------------------- */

/**
 * Starts a Wintun data session on an adapter.
 *
 * @param Adapter  Handle returned by CreateAdapter / OpenAdapter.
 * @param Capacity Ring-buffer capacity in bytes.  Must be a power of
 *                 two in [WINTUN_MIN_RING_CAPACITY, WINTUN_MAX_RING_CAPACITY].
 * @return Session handle on success, NULL on failure (GetLastError()).
 */
typedef WINTUN_SESSION_HANDLE (WINAPI *WINTUN_START_SESSION_FUNC)(
    WINTUN_ADAPTER_HANDLE Adapter, DWORD Capacity);

/**
 * Ends a data session, releasing all ring-buffer memory.  Any outstanding
 * packets returned by ReceivePacket() become invalid immediately.
 */
typedef void (WINAPI *WINTUN_END_SESSION_FUNC)(WINTUN_SESSION_HANDLE Session);

/**
 * Returns a manual-reset event handle that becomes signalled whenever
 * a packet is available to receive.  The handle is owned by Wintun; do
 * NOT CloseHandle() it.
 */
typedef HANDLE (WINAPI *WINTUN_GET_READ_WAIT_EVENT_FUNC)(WINTUN_SESSION_HANDLE Session);

/**
 * Attempts to dequeue a packet from the receive ring.
 *
 * @param Session     Session handle.
 * @param PacketSize  On success, receives the packet size in bytes.
 * @return Pointer to Wintun-owned packet memory (valid until
 *         ReleaseReceivePacket is called), or NULL if the ring is
 *         empty or an error occurred.  On NULL, GetLastError() returns
 *         ERROR_NO_MORE_ITEMS when empty (caller should wait on the
 *         read-wait event), or ERROR_HANDLE_EOF when the session died.
 */
typedef BYTE* (WINAPI *WINTUN_RECEIVE_PACKET_FUNC)(
    WINTUN_SESSION_HANDLE Session, DWORD *PacketSize);

/**
 * Releases a packet previously returned by ReceivePacket back to Wintun.
 * The pointer becomes invalid immediately.
 */
typedef void (WINAPI *WINTUN_RELEASE_RECEIVE_PACKET_FUNC)(
    WINTUN_SESSION_HANDLE Session, const BYTE *Packet);

/**
 * Reserves space in the send ring for a packet.  Caller memcpy's into
 * the returned buffer, then calls SendPacket to submit.
 *
 * @return NULL if the ring is full (ERROR_BUFFER_OVERFLOW) or the
 *         session is dead (ERROR_HANDLE_EOF).
 */
typedef BYTE* (WINAPI *WINTUN_ALLOCATE_SEND_PACKET_FUNC)(
    WINTUN_SESSION_HANDLE Session, DWORD PacketSize);

/**
 * Submits a packet previously reserved with AllocateSendPacket.
 */
typedef void (WINAPI *WINTUN_SEND_PACKET_FUNC)(
    WINTUN_SESSION_HANDLE Session, const BYTE *Packet);

#ifdef __cplusplus
} /* extern "C" */
#endif
