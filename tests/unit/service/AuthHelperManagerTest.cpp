// =============================================================================
// AuthHelperManagerTest.cpp
//
// Unit tests for the service-side per-user auth helper manager (Phase 5):
//   src/service/TcpRedirectorService/infrastructure/auth/AuthHelperManager.h/.cpp
//
// Session enumeration and CreateProcessAsUser launch are environment-dependent
// (need real interactive sessions + LocalSystem privileges) and are NOT
// unit-testable here. We instead cover the pure/deterministic logic per the
// Phase 5 task:
//   - nonce generation: non-empty, correct length, uniqueness across calls.
//   - SPN derivation from config (auth_spn else HTTP/<host>).
//   - helper argv construction (flags/order, --spn vs --proxy-host precedence).
//   - command-line quoting/assembly.
//   - MakeConfig() derivation from domain::ProxyConfig.
//   - record-table lookup on an un-started manager (TryGet/BuildBrokeredParams
//     return false; no helpers => HelperCount()==0).
//
// Windows-only. Uses Catch2 v3 (same harness as the other tests).
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

#include "infrastructure/auth/AuthHelperManager.h"

namespace ahm = tcp_redirector::infrastructure::auth;
namespace det = tcp_redirector::infrastructure::auth::detail;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
namespace {

// Find the value that follows a given flag in an argv vector (or L"" if absent).
std::wstring ValueAfter(const std::vector<std::wstring>& args,
                        const std::wstring& flag) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1];
    }
    return std::wstring();
}

bool HasFlag(const std::vector<std::wstring>& args, const std::wstring& flag) {
    for (const auto& a : args) if (a == flag) return true;
    return false;
}

}  // namespace

// =============================================================================
// Nonce generation
// =============================================================================
TEST_CASE("GenerateNonceHex produces correct-length hex", "[authhelpermgr][nonce]") {
    const std::string n = det::GenerateNonceHex(32);
    REQUIRE(n.size() == 64);  // 32 bytes -> 64 hex chars.
    for (char c : n) {
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        REQUIRE(isHex);
    }
}

TEST_CASE("GenerateNonceHex honors byte length", "[authhelpermgr][nonce]") {
    REQUIRE(det::GenerateNonceHex(16).size() == 32);
    REQUIRE(det::GenerateNonceHex(8).size() == 16);
    // 0 => default 32 bytes.
    REQUIRE(det::GenerateNonceHex(0).size() == 64);
}

TEST_CASE("GenerateNonceHex is unique across many calls", "[authhelpermgr][nonce]") {
    std::set<std::string> seen;
    constexpr int kN = 500;
    for (int i = 0; i < kN; ++i) {
        std::string n = det::GenerateNonceHex(32);
        REQUIRE(!n.empty());
        // No collisions expected for a 256-bit random value.
        REQUIRE(seen.insert(n).second);
    }
    REQUIRE(seen.size() == static_cast<std::size_t>(kN));
}

// =============================================================================
// SPN derivation
// =============================================================================
TEST_CASE("DeriveSpn prefers explicit auth_spn", "[authhelpermgr][spn]") {
    REQUIRE(det::DeriveSpn("HTTP/explicit.corp", "proxy.corp") == "HTTP/explicit.corp");
}

TEST_CASE("DeriveSpn falls back to HTTP/<host>", "[authhelpermgr][spn]") {
    REQUIRE(det::DeriveSpn("", "proxy.corp.example.com") ==
            "HTTP/proxy.corp.example.com");
}

TEST_CASE("DeriveSpn returns empty when both empty (fail-closed)", "[authhelpermgr][spn]") {
    REQUIRE(det::DeriveSpn("", "").empty());
}

// =============================================================================
// Helper argv construction
// =============================================================================
TEST_CASE("BuildHelperArgs emits session/nonce/spn/version", "[authhelpermgr][args]") {
    auto args = det::BuildHelperArgs(7, "deadbeef", "HTTP/proxy.corp", "proxy.corp", "1");

    REQUIRE(ValueAfter(args, L"--session") == L"7");
    REQUIRE(ValueAfter(args, L"--nonce") == L"deadbeef");
    REQUIRE(ValueAfter(args, L"--spn") == L"HTTP/proxy.corp");
    REQUIRE(ValueAfter(args, L"--version") == L"1");
    // When an SPN is present, --proxy-host must NOT be emitted.
    REQUIRE_FALSE(HasFlag(args, L"--proxy-host"));
}

TEST_CASE("BuildHelperArgs uses --proxy-host when SPN empty", "[authhelpermgr][args]") {
    auto args = det::BuildHelperArgs(3, "abcd", "", "proxy.corp.example.com", "2");

    REQUIRE(ValueAfter(args, L"--session") == L"3");
    REQUIRE(ValueAfter(args, L"--proxy-host") == L"proxy.corp.example.com");
    REQUIRE_FALSE(HasFlag(args, L"--spn"));
    REQUIRE(ValueAfter(args, L"--version") == L"2");
}

TEST_CASE("BuildHelperArgs omits allow-list flags when both empty", "[authhelpermgr][args]") {
    auto args = det::BuildHelperArgs(1, "aa", "", "", "1");
    REQUIRE_FALSE(HasFlag(args, L"--spn"));
    REQUIRE_FALSE(HasFlag(args, L"--proxy-host"));
    // session/nonce/version still present.
    REQUIRE(ValueAfter(args, L"--session") == L"1");
    REQUIRE(ValueAfter(args, L"--nonce") == L"aa");
}

// =============================================================================
// Command-line assembly / quoting
// =============================================================================
TEST_CASE("BuildCommandLine quotes exe and args with spaces", "[authhelpermgr][cmdline]") {
    std::vector<std::wstring> args = {L"--session", L"5", L"--spn", L"HTTP/a b"};
    std::wstring cmd = det::BuildCommandLine(L"C:\\Program Files\\Helper.exe", args);

    // Exe path with a space must be quoted.
    REQUIRE(cmd.find(L"\"C:\\Program Files\\Helper.exe\"") != std::wstring::npos);
    // Arg with a space must be quoted.
    REQUIRE(cmd.find(L"\"HTTP/a b\"") != std::wstring::npos);
    // A simple arg without spaces stays bare.
    REQUIRE(cmd.find(L" --session 5") != std::wstring::npos);
}

// =============================================================================
// MakeConfig derivation from domain::ProxyConfig
// =============================================================================
TEST_CASE("MakeConfig derives SPN from explicit auth_spn", "[authhelpermgr][config]") {
    tcp_redirector::domain::ProxyConfig pc;
    pc.host = L"proxy.corp.example.com";
    pc.auth_spn = L"HTTP/custom.corp";
    pc.helper_timeout_ms = 4321;

    auto cfg = ahm::AuthHelperManager::MakeConfig(pc, L"C:\\svc\\Helper.exe", "9");
    REQUIRE(cfg.spn == "HTTP/custom.corp");
    REQUIRE(cfg.proxyHost == "proxy.corp.example.com");
    REQUIRE(cfg.helperTimeoutMs == 4321);
    REQUIRE(cfg.helperExePath == L"C:\\svc\\Helper.exe");
    REQUIRE(cfg.helperVersion == "9");
}

TEST_CASE("MakeConfig derives SPN from host when auth_spn empty", "[authhelpermgr][config]") {
    tcp_redirector::domain::ProxyConfig pc;
    pc.host = L"proxy.corp.example.com";
    pc.auth_spn = L"";
    pc.helper_timeout_ms = 0;  // => clamped to default 5000.

    auto cfg = ahm::AuthHelperManager::MakeConfig(pc, L"Helper.exe");
    REQUIRE(cfg.spn == "HTTP/proxy.corp.example.com");
    REQUIRE(cfg.helperTimeoutMs == 5000);
}

// =============================================================================
// Record-table lookup on an un-started manager
// =============================================================================
TEST_CASE("TryGet/BuildBrokeredParams on empty manager return false",
          "[authhelpermgr][table]") {
    ahm::AuthHelperManagerConfig cfg;
    cfg.helperExePath = L"C:\\svc\\Helper.exe";
    cfg.spn = "HTTP/proxy.corp";
    cfg.proxyHost = "proxy.corp";
    cfg.helperTimeoutMs = 4000;

    ahm::AuthHelperManager mgr(cfg, /*log=*/nullptr);

    REQUIRE(mgr.HelperCount() == 0);

    ahm::HelperRecord rec;
    REQUIRE_FALSE(mgr.TryGet(1, rec));
    REQUIRE_FALSE(mgr.TryGet(42, rec));

    tcp_redirector::infrastructure::BrokeredAuthParams params;
    REQUIRE_FALSE(mgr.BuildBrokeredParams(1, params));
}
