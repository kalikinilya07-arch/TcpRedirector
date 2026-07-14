// =============================================================================
// ConnectionTableTest.cpp
//
// Unit tests for infrastructure::ConnectionTable (QA audit 2026-07-14, finding
// B2). Locks in the single-connection correctness of the src_port-keyed
// reverse-lookup table and DOCUMENTS the inherent collision behaviour when two
// simultaneously-tracked connections reuse the same ephemeral src_port to
// different destinations.
//
// Why there is no "compound key on read" here: every read/remove call site
// (TcpRelayServer::Get(client_port), WinDivertCapture return-path Get(dstPort),
// Remove(...)) only knows the src_port at lookup time — the original
// destination is exactly what it is trying to recover. So the table cannot be
// keyed on (src_port, orig_dest_ip) at read time. The compound key that Add()
// uses only prevents the older colliding entry from being *overwritten*, so it
// survives and becomes resolvable again once the newer one is removed. That
// best-effort behaviour is what this test pins down.
//
// Build (matches build.bat style):
//   cl /EHsc /std:c++20 /Fe:ConnectionTableTest.exe ConnectionTableTest.cpp \
//      /I <repo>\src\service\TcpRedirectorService
// =============================================================================
#include <cassert>
#include <iostream>
#include <cstring>
#include "../../../src/service/TcpRedirectorService/infrastructure/relay/ConnectionTable.h"

using tcp_redirector::infrastructure::ConnectionTable;
using tcp_redirector::domain::ports::ConnectionInfo;

// Helper: readable IPv4 literal -> raw uint32_t (endianness irrelevant for the
// table, which only compares equality).
static uint32_t IP(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d);
}

static void TestAddAndGet() {
    std::cout << "Test: AddAndGet..." << std::endl;
    ConnectionTable t;
    t.Add(51000, IP(192,168,0,10), IP(93,184,216,34), 443, 1);

    uint32_t ip = 0; uint16_t port = 0;
    assert(t.Get(51000, &ip, &port) == true);
    assert(ip == IP(93,184,216,34));
    assert(port == 443);
    assert(t.GetTrackedCount() == 1);
    std::cout << "  PASS" << std::endl;
}

static void TestGetMissing() {
    std::cout << "Test: GetMissing..." << std::endl;
    ConnectionTable t;
    uint32_t ip = 0xDEAD; uint16_t port = 0xBEEF;
    assert(t.Get(12345, &ip, &port) == false);
    assert(t.IsTracked(12345) == false);
    assert(t.GetProxyConfigId(12345) == 0);
    std::cout << "  PASS" << std::endl;
}

static void TestUpdateInPlaceSameKey() {
    std::cout << "Test: UpdateInPlaceSameKey..." << std::endl;
    ConnectionTable t;
    // Same (src_port, orig_dest_ip) => update, not a second entry.
    t.Add(51000, IP(10,0,0,1), IP(1,1,1,1), 80, 1);
    t.Add(51000, IP(10,0,0,1), IP(1,1,1,1), 8080, 2);
    assert(t.GetTrackedCount() == 1);

    uint16_t port = 0;
    assert(t.Get(51000, nullptr, &port) == true);
    assert(port == 8080);                 // updated port
    assert(t.GetProxyConfigId(51000) == 2); // updated proxy id
    std::cout << "  PASS" << std::endl;
}

static void TestProxyConfigIdAndIsTracked() {
    std::cout << "Test: ProxyConfigIdAndIsTracked..." << std::endl;
    ConnectionTable t;
    t.Add(4000, IP(10,0,0,2), IP(8,8,8,8), 53, 7);
    assert(t.IsTracked(4000) == true);
    assert(t.GetProxyConfigId(4000) == 7);
    std::cout << "  PASS" << std::endl;
}

static void TestRemove() {
    std::cout << "Test: Remove..." << std::endl;
    ConnectionTable t;
    t.Add(5555, IP(10,0,0,3), IP(20,20,20,20), 443, 1);
    assert(t.GetTrackedCount() == 1);
    t.Remove(5555);
    assert(t.GetTrackedCount() == 0);
    uint32_t ip = 0; uint16_t port = 0;
    assert(t.Get(5555, &ip, &port) == false);
    // Removing a non-existent port must be a no-op (no underflow of count).
    t.Remove(9999);
    assert(t.GetTrackedCount() == 0);
    std::cout << "  PASS" << std::endl;
}

static void TestBytesAndInfo() {
    std::cout << "Test: BytesAndInfo..." << std::endl;
    ConnectionTable t;
    t.Add(6001, IP(10,0,0,4), IP(1,2,3,4), 443, 1);
    t.SetProcessInfo(6001, 4242, L"C:\\Apps\\MyApp\\MyApp.exe");
    t.AddBytes(6001, 100, 0);
    t.AddBytes(6001, 0, 250);
    t.AddBytes(6001, 5, 5);

    ConnectionInfo info;
    std::memset(&info, 0, sizeof(info));
    assert(t.GetInfo(6001, &info) == true);
    assert(info.pid == 4242);
    assert(info.bytes_up == 105);
    assert(info.bytes_down == 255);
    assert(std::wcscmp(info.proc_path, L"C:\\Apps\\MyApp\\MyApp.exe") == 0);
    std::cout << "  PASS" << std::endl;
}

// Documents the KNOWN collision behaviour (finding B2): two live connections
// reuse the same ephemeral src_port to different destinations.
static void TestSrcPortCollisionBestEffort() {
    std::cout << "Test: SrcPortCollisionBestEffort..." << std::endl;
    ConnectionTable t;
    const uint32_t dstA = IP(11,11,11,11);
    const uint32_t dstB = IP(22,22,22,22);

    t.Add(50000, IP(10,0,0,5), dstA, 443, 1);
    t.Add(50000, IP(10,0,0,5), dstB, 8443, 2); // different dst, same src_port

    // Both entries retained (compound key on Add prevents overwrite).
    assert(t.GetTrackedCount() == 2);

    // Get() is keyed on src_port only, so it resolves to the most-recently
    // added mapping (dstB). This is the documented best-effort behaviour.
    uint32_t ip = 0; uint16_t port = 0;
    assert(t.Get(50000, &ip, &port) == true);
    assert(ip == dstB);
    assert(port == 8443);

    // Removing once drops one colliding entry; the survivor stays resolvable.
    t.Remove(50000);
    assert(t.GetTrackedCount() == 1);
    assert(t.Get(50000, &ip, &port) == true);
    assert(ip == dstA);       // the other mapping survives and is now returned
    assert(port == 443);
    std::cout << "  PASS" << std::endl;
}

static void TestClear() {
    std::cout << "Test: Clear..." << std::endl;
    ConnectionTable t;
    t.Add(7001, IP(10,0,0,6), IP(1,1,1,1), 443, 1);
    t.Add(7002, IP(10,0,0,6), IP(1,1,1,2), 443, 1);
    assert(t.GetTrackedCount() == 2);
    t.Clear();
    assert(t.GetTrackedCount() == 0);
    assert(t.IsTracked(7001) == false);
    assert(t.IsTracked(7002) == false);
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== ConnectionTable Unit Tests ===" << std::endl << std::endl;
    TestAddAndGet();
    TestGetMissing();
    TestUpdateInPlaceSameKey();
    TestProxyConfigIdAndIsTracked();
    TestRemove();
    TestBytesAndInfo();
    TestSrcPortCollisionBestEffort();
    TestClear();
    std::cout << std::endl << "All tests passed!" << std::endl;
    return 0;
}
