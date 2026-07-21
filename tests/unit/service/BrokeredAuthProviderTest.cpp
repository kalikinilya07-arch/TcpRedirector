// =============================================================================
// BrokeredAuthProviderTest.cpp
//
// Unit / loopback-integration tests for the service-side broker (Phase 4):
//   src/service/TcpRedirectorService/infrastructure/auth/AuthBrokerClient.h
//   src/service/TcpRedirectorService/infrastructure/auth/BrokeredAuthProvider.h
//
// Strategy: a FAKE helper pipe-SERVER runs in a background thread on a real
// message-mode named pipe (\\.\pipe\TcpRedirectorAuth_<sessionId>), mimicking the
// Phase-3 helper: it first sends a `hello`, then answers `sspi_step` requests
// with scripted SspiStepResponses, and records any `release` it receives. The
// tests then drive the REAL AuthBrokerClient + BrokeredAuthProvider against it.
//
// Covers (per Phase 4 task):
//   - hello / nonce validation (match => works; mismatch => Failed).
//   - full continue -> complete handshake, correlation_id reused across steps.
//   - denied SPN => Failed (no silent fallback).
//   - timeout => Failed (fake server delays past helper_timeout_ms).
//   - release sent on provider destruction.
//   - status-mapping + request/response serialization glue (via real exchange).
//
// Windows-only (named pipes). Uses Catch2 v3 (same harness as the other tests).
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "shared/auth_broker/AuthBrokerProtocol.h"
#include "infrastructure/auth/AuthBrokerClient.h"
#include "infrastructure/auth/BrokeredAuthProvider.h"

namespace ab = tcp_redirector::shared::auth_broker;
namespace infra = tcp_redirector::infrastructure;
using tcp_redirector::domain::ports::AuthStepStatus;

namespace {

// A single scripted response the fake helper returns for the Nth sspi_step.
struct ScriptedStep {
    ab::AuthStatus status = ab::AuthStatus::Complete;
    std::string    out_token;
    std::string    detail;
    DWORD          delay_ms = 0;  // artificial delay before responding (timeout tests).
};

// -----------------------------------------------------------------------------
// FakeHelperServer — a minimal message-mode pipe SERVER for one client.
// -----------------------------------------------------------------------------
class FakeHelperServer {
public:
    FakeHelperServer(std::uint32_t sessionId, std::string nonce)
        : m_sessionId(sessionId), m_nonce(std::move(nonce)) {}

    ~FakeHelperServer() { Stop(); }

    void SetHelloNonce(std::string nonce) { m_helloNonce = std::move(nonce); m_helloNonceSet = true; }
    void SetHelloSessionId(std::uint32_t s) { m_helloSessionId = s; m_helloSessionIdSet = true; }
    void PushStep(ScriptedStep s) { m_script.push_back(std::move(s)); }

    // Whether a `release` message was observed by the server thread.
    bool ReleaseSeen() const { return m_releaseSeen.load(); }
    std::string LastReleaseCorrId() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_lastReleaseCorr;
    }
    std::vector<std::string> StepCorrIds() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_stepCorrIds;
    }

    bool Start() {
        const std::wstring pipeName = ab::MakeAuthPipeNameW(m_sessionId);
        m_pipe = ::CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 65536, 65536, 0, nullptr);
        if (m_pipe == INVALID_HANDLE_VALUE) return false;
        m_running = true;
        m_thread = std::thread([this]() { ServerLoop(); });
        return true;
    }

    void Stop() {
        m_running = false;
        if (m_pipe != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(m_pipe, nullptr);
            ::DisconnectNamedPipe(m_pipe);
        }
        if (m_thread.joinable()) m_thread.join();
        if (m_pipe != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
    }

private:
    void WriteMsg(const std::string& s) {
        DWORD written = 0;
        ::WriteFile(m_pipe, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
        ::FlushFileBuffers(m_pipe);
    }

    bool ReadMsg(std::string& out) {
        char buf[65536];
        DWORD read = 0;
        BOOL ok = ::ReadFile(m_pipe, buf, sizeof(buf), &read, nullptr);
        if (!ok || read == 0) return false;
        out.assign(buf, read);
        return true;
    }

    void ServerLoop() {
        BOOL connected = ::ConnectNamedPipe(m_pipe, nullptr);
        if (!connected && ::GetLastError() == ERROR_PIPE_CONNECTED) connected = TRUE;
        if (!connected) return;

        // 1) Send hello first.
        ab::HelloMessage hello;
        hello.session_id = m_helloSessionIdSet ? m_helloSessionId : m_sessionId;
        hello.nonce = m_helloNonceSet ? m_helloNonce : m_nonce;
        hello.helper_version = "fake-1.0";
        WriteMsg(ab::Serialize(hello));

        // 2) Answer requests.
        std::size_t stepIdx = 0;
        while (m_running.load()) {
            std::string raw;
            if (!ReadMsg(raw)) break;

            std::string op;
            if (ab::PeekOp(raw, op) != ab::ParseError::Ok) continue;

            if (op == ab::kOpSspiStep) {
                ab::SspiStepRequest req;
                ab::ParseSspiStepRequest(raw, req);
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_stepCorrIds.push_back(req.correlation_id);
                }

                ScriptedStep scripted;
                if (stepIdx < m_script.size()) scripted = m_script[stepIdx];
                ++stepIdx;

                if (scripted.delay_ms > 0) {
                    ::Sleep(scripted.delay_ms);
                }

                ab::SspiStepResponse resp;
                resp.status = scripted.status;
                resp.out_token = scripted.out_token;
                resp.detail = scripted.detail;
                WriteMsg(ab::Serialize(resp));
            } else if (op == ab::kOpRelease) {
                ab::ReleaseMessage rel;
                ab::ParseReleaseMessage(raw, rel);
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_lastReleaseCorr = rel.correlation_id;
                }
                m_releaseSeen.store(true);
            }
        }
    }

    std::uint32_t m_sessionId;
    std::string   m_nonce;

    bool          m_helloNonceSet = false;
    std::string   m_helloNonce;
    bool          m_helloSessionIdSet = false;
    std::uint32_t m_helloSessionId = 0;

    std::vector<ScriptedStep> m_script;

    HANDLE        m_pipe = INVALID_HANDLE_VALUE;
    std::thread   m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_releaseSeen{false};

    mutable std::mutex       m_mtx;
    std::string              m_lastReleaseCorr;
    std::vector<std::string> m_stepCorrIds;
};

// Unique-ish session id per test to avoid pipe-name collisions between cases.
std::uint32_t NextSessionId() {
    static std::atomic<std::uint32_t> s{40000};
    return s.fetch_add(1);
}

infra::BrokeredAuthParams MakeParams(std::uint32_t sessionId,
                                     const std::string& nonce,
                                     const std::string& corr) {
    infra::BrokeredAuthParams p;
    p.session_id = sessionId;
    p.expected_nonce = nonce;
    p.spn = "HTTP/proxy.corp.example.com";
    p.proxy_host = "proxy.corp.example.com";
    p.helper_timeout_ms = 2000;
    p.correlation_id_override = corr;
    return p;
}

} // namespace

// =============================================================================
// hello / nonce validation
// =============================================================================

TEST_CASE("BrokeredAuthProvider: hello nonce match => handshake works",
          "[brokered][hello]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "secret-nonce");
    server.PushStep({ab::AuthStatus::Complete, "OUT_TOKEN_1", "", 0});
    REQUIRE(server.Start());

    infra::BrokeredAuthProvider provider(MakeParams(sid, "secret-nonce", "conn-1"));
    std::string outToken;
    const AuthStepStatus st = provider.NextToken("HTTP/proxy.corp.example.com", "", outToken);

    REQUIRE(st == AuthStepStatus::Complete);
    REQUIRE(outToken == "OUT_TOKEN_1");
}

TEST_CASE("BrokeredAuthProvider: nonce mismatch => Failed (no fallback)",
          "[brokered][hello]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "the-real-nonce");
    server.SetHelloNonce("attacker-nonce");  // squatted pipe scenario.
    server.PushStep({ab::AuthStatus::Complete, "SHOULD_NOT_BE_USED", "", 0});
    REQUIRE(server.Start());

    infra::BrokeredAuthProvider provider(MakeParams(sid, "the-real-nonce", "conn-1"));
    std::string outToken;
    const AuthStepStatus st = provider.NextToken("HTTP/proxy.corp.example.com", "", outToken);

    REQUIRE(st == AuthStepStatus::Failed);
    REQUIRE(outToken.empty());
}

TEST_CASE("BrokeredAuthProvider: hello session_id mismatch => Failed",
          "[brokered][hello]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "secret-nonce");
    server.SetHelloSessionId(sid + 999);  // wrong session in hello.
    server.PushStep({ab::AuthStatus::Complete, "X", "", 0});
    REQUIRE(server.Start());

    infra::BrokeredAuthProvider provider(MakeParams(sid, "secret-nonce", "conn-1"));
    std::string outToken;
    const AuthStepStatus st = provider.NextToken("HTTP/proxy.corp.example.com", "", outToken);

    REQUIRE(st == AuthStepStatus::Failed);
}

// =============================================================================
// full continue -> complete handshake (correlation reuse)
// =============================================================================

TEST_CASE("BrokeredAuthProvider: continue -> complete, correlation reused",
          "[brokered][handshake]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "n");
    server.PushStep({ab::AuthStatus::Continue, "TOKEN_STEP1", "", 0});
    server.PushStep({ab::AuthStatus::Complete, "TOKEN_STEP2", "", 0});
    REQUIRE(server.Start());

    infra::BrokeredAuthProvider provider(MakeParams(sid, "n", "conn-42"));

    std::string out1;
    REQUIRE(provider.NextToken("HTTP/proxy.corp.example.com", "", out1) ==
            AuthStepStatus::ContinueNeeded);
    REQUIRE(out1 == "TOKEN_STEP1");

    std::string out2;
    REQUIRE(provider.NextToken("HTTP/proxy.corp.example.com", "CHALLENGE_B64", out2) ==
            AuthStepStatus::Complete);
    REQUIRE(out2 == "TOKEN_STEP2");

    // Both steps used the SAME correlation id (§2.3).
    const auto corrs = server.StepCorrIds();
    REQUIRE(corrs.size() == 2);
    REQUIRE(corrs[0] == "conn-42");
    REQUIRE(corrs[1] == "conn-42");
}

TEST_CASE("BrokeredAuthProvider: no_credentials maps to NoCredentials",
          "[brokered][mapping]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "n");
    server.PushStep({ab::AuthStatus::NoCredentials, "", "no ticket", 0});
    REQUIRE(server.Start());

    infra::BrokeredAuthProvider provider(MakeParams(sid, "n", "conn-1"));
    std::string out;
    REQUIRE(provider.NextToken("HTTP/proxy.corp.example.com", "", out) ==
            AuthStepStatus::NoCredentials);
}

// =============================================================================
// denied SPN => Failed
// =============================================================================

TEST_CASE("BrokeredAuthProvider: denied SPN => Failed (never downgrades)",
          "[brokered][denied]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "n");
    server.PushStep({ab::AuthStatus::Denied, "", "spn not on allow-list", 0});
    REQUIRE(server.Start());

    infra::BrokeredAuthProvider provider(MakeParams(sid, "n", "conn-1"));
    std::string out;
    REQUIRE(provider.NextToken("LDAP/dc.corp.example.com", "", out) ==
            AuthStepStatus::Failed);
    REQUIRE(out.empty());
}

TEST_CASE("BrokeredAuthProvider: error status => Failed", "[brokered][error]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "n");
    server.PushStep({ab::AuthStatus::Error, "", "internal", 0});
    REQUIRE(server.Start());

    infra::BrokeredAuthProvider provider(MakeParams(sid, "n", "conn-1"));
    std::string out;
    REQUIRE(provider.NextToken("HTTP/proxy.corp.example.com", "", out) ==
            AuthStepStatus::Failed);
}

// =============================================================================
// timeout => Failed
// =============================================================================

TEST_CASE("BrokeredAuthProvider: helper timeout => Failed", "[brokered][timeout]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "n");
    // Server delays well past helper_timeout_ms before responding.
    server.PushStep({ab::AuthStatus::Complete, "LATE_TOKEN", "", 1500});
    REQUIRE(server.Start());

    infra::BrokeredAuthParams p = MakeParams(sid, "n", "conn-1");
    p.helper_timeout_ms = 300;  // shorter than the 1500ms server delay.
    infra::BrokeredAuthProvider provider(p);

    std::string out;
    const AuthStepStatus st =
        provider.NextToken("HTTP/proxy.corp.example.com", "", out);
    REQUIRE(st == AuthStepStatus::Failed);
}

TEST_CASE("BrokeredAuthProvider: no helper present => Failed",
          "[brokered][nohelper]") {
    const std::uint32_t sid = NextSessionId();  // no server started for it.
    infra::BrokeredAuthParams p = MakeParams(sid, "n", "conn-1");
    p.helper_timeout_ms = 300;
    infra::BrokeredAuthProvider provider(p);

    std::string out;
    REQUIRE(provider.NextToken("HTTP/proxy.corp.example.com", "", out) ==
            AuthStepStatus::Failed);
}

// =============================================================================
// release on destruction
// =============================================================================

TEST_CASE("BrokeredAuthProvider: sends release on destruction",
          "[brokered][release]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "n");
    server.PushStep({ab::AuthStatus::Complete, "TOK", "", 0});
    REQUIRE(server.Start());

    {
        infra::BrokeredAuthProvider provider(MakeParams(sid, "n", "conn-release"));
        std::string out;
        REQUIRE(provider.NextToken("HTTP/proxy.corp.example.com", "", out) ==
                AuthStepStatus::Complete);
        // provider destroyed here -> best-effort release.
    }

    // Give the server thread a moment to observe the release message.
    for (int i = 0; i < 50 && !server.ReleaseSeen(); ++i) {
        ::Sleep(10);
    }
    REQUIRE(server.ReleaseSeen());
    REQUIRE(server.LastReleaseCorrId() == "conn-release");
}

// =============================================================================
// AuthBrokerClient direct: default correlation id format
// =============================================================================

TEST_CASE("BrokeredAuthProvider: auto correlation id has conn- prefix",
          "[brokered][corr]") {
    const std::uint32_t sid = NextSessionId();
    FakeHelperServer server(sid, "n");
    server.PushStep({ab::AuthStatus::Complete, "TOK", "", 0});
    REQUIRE(server.Start());

    infra::BrokeredAuthParams p = MakeParams(sid, "n", "");  // no override.
    infra::BrokeredAuthProvider provider(p);
    std::string out;
    REQUIRE(provider.NextToken("HTTP/proxy.corp.example.com", "", out) ==
            AuthStepStatus::Complete);

    const auto corrs = server.StepCorrIds();
    REQUIRE(corrs.size() == 1);
    REQUIRE(corrs[0].rfind("conn-", 0) == 0);  // starts with "conn-".
}
