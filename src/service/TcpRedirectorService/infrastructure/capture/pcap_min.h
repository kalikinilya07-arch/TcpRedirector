#pragma once

//
// Minimal pcap header — defines types and a runtime-loadable API table.
// No Npcap SDK or .lib files required for build.
// At runtime, PcapApi::Load() resolves all symbols from wpcap.dll.
//

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCAP_ERRBUF_SIZE    256
#define PCAP_IF_LOOPBACK    0x00000001
#define PCAP_IF_UP          0x00000002
#define PCAP_NETMASK_UNKNOWN 0xffffffff

struct bpf_program {
    uint32_t bf_len;
    void*    bf_insns;
};

struct pcap_pkthdr {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t caplen;
    uint32_t len;
};

struct pcap_if {
    struct pcap_if* next;
    char*           name;
    char*           description;
    void*           addresses;
    uint32_t        flags;
};

typedef struct pcap_if pcap_if_t;
typedef void* pcap_t;
typedef unsigned char u_char;

typedef void (*pcap_handler)(u_char* user, const struct pcap_pkthdr* h, const u_char* bytes);

#ifdef __cplusplus
}
#endif

//
// Runtime-loaded pcap API — no static linking, no SDK needed.
//
struct PcapApi {
    HMODULE dll = nullptr;

    // Function pointer typedefs
    typedef int     (*PFN_findalldevs)(pcap_if_t**, char*);
    typedef void    (*PFN_freealldevs)(pcap_if_t*);
    typedef pcap_t* (*PFN_open_live)(const char*, int, int, int, char*);
    typedef int     (*PFN_compile)(pcap_t*, struct bpf_program*, const char*, int, uint32_t);
    typedef int     (*PFN_setfilter)(pcap_t*, struct bpf_program*);
    typedef void    (*PFN_freecode)(struct bpf_program*);
    typedef int     (*PFN_dispatch)(pcap_t*, int, pcap_handler, u_char*);
    typedef void    (*PFN_breakloop)(pcap_t*);
    typedef void    (*PFN_close)(pcap_t*);

    // Function pointers
    PFN_findalldevs  findalldevs  = nullptr;
    PFN_freealldevs  freealldevs  = nullptr;
    PFN_open_live    open_live    = nullptr;
    PFN_compile      compile      = nullptr;
    PFN_setfilter    setfilter    = nullptr;
    PFN_freecode     freecode     = nullptr;
    PFN_dispatch     dispatch     = nullptr;
    PFN_breakloop    breakloop    = nullptr;
    PFN_close        close        = nullptr;

    bool Load() {
        if (dll) return true;

        // Try Npcap path first, then system-wide
        dll = LoadLibraryW(L"wpcap.dll");
        if (!dll) {
            dll = LoadLibraryW(L"C:\\Windows\\System32\\Npcap\\wpcap.dll");
        }
        if (!dll) return false;

        #define LOAD(fn, name) \
            fn = reinterpret_cast<decltype(fn)>(GetProcAddress(dll, name)); \
            if (!fn) { Unload(); return false; }

        LOAD(findalldevs,  "pcap_findalldevs");
        LOAD(freealldevs,  "pcap_freealldevs");
        LOAD(open_live,    "pcap_open_live");
        LOAD(compile,      "pcap_compile");
        LOAD(setfilter,    "pcap_setfilter");
        LOAD(freecode,     "pcap_freecode");
        LOAD(dispatch,     "pcap_dispatch");
        LOAD(breakloop,    "pcap_breakloop");
        LOAD(close,        "pcap_close");

        #undef LOAD
        return true;
    }

    void Unload() {
        findalldevs  = nullptr;
        freealldevs  = nullptr;
        open_live    = nullptr;
        compile      = nullptr;
        setfilter    = nullptr;
        freecode     = nullptr;
        dispatch     = nullptr;
        breakloop    = nullptr;
        close        = nullptr;

        if (dll) {
            FreeLibrary(dll);
            dll = nullptr;
        }
    }

    bool IsLoaded() const { return dll != nullptr && findalldevs != nullptr; }
};
