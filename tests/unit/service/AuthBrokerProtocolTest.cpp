// =============================================================================
// AuthBrokerProtocolTest.cpp
//
// Unit tests for the shared token-broker protocol library (Phase 2):
//   src/shared/auth_broker/AuthBrokerProtocol.h
//
// Covers (per plan §2, task Phase 2):
//   - Round-trip serialize -> parse of EVERY message type
//     (hello, sspi_step request, sspi_step response, release).
//   - Malformed / missing / wrong-type / bad-version / unknown-op input handling
//     (must NEVER throw across the boundary; must return a ParseError).
//   - Pipe-name construction (MakeAuthPipeName / MakeAuthPipeNameW).
//   - Protocol AuthStatus <-> wire string round-trip.
//   - AuthStatus -> AuthStepStatus (Phase 1) mapping, incl. a compile-time
//     assertion that the local MappedAuthStepStatus mirror is in sync with the
//     real domain::ports::AuthStepStatus.
//   - Framing size limits.
//
// Uses Catch2 v3 (same harness as tcp_redirector_tests).
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>

#include "shared/auth_broker/AuthBrokerProtocol.h"

// Phase 1 real enum — reachable because tests/CMakeLists.txt adds
// ../src/service/TcpRedirectorService/.. to the include path.
#include "domain/ports/IAuthProvider.h"

namespace ab = tcp_redirector::shared::auth_broker;
using tcp_redirector::domain::ports::AuthStepStatus;

// -----------------------------------------------------------------------------
// Compile-time: the local mirror MUST match the real Phase 1 enum (values AND
// order). If someone changes AuthStepStatus, this fails to compile here.
// -----------------------------------------------------------------------------
static_assert(static_cast<int>(ab::MappedAuthStepStatus::Complete) ==
              static_cast<int>(AuthStepStatus::Complete),
              "MappedAuthStepStatus::Complete out of sync with AuthStepStatus");
static_assert(static_cast<int>(ab::MappedAuthStepStatus::ContinueNeeded) ==
              static_cast<int>(AuthStepStatus::ContinueNeeded),
              "MappedAuthStepStatus::ContinueNeeded out of sync with AuthStepStatus");
static_assert(static_cast<int>(ab::MappedAuthStepStatus::NoCredentials) ==
              static_cast<int>(AuthStepStatus::NoCredentials),
              "MappedAuthStepStatus::NoCredentials out of sync with AuthStepStatus");
static_assert(static_cast<int>(ab::MappedAuthStepStatus::Failed) ==
              static_cast<int>(AuthStepStatus::Failed),
              "MappedAuthStepStatus::Failed out of sync with AuthStepStatus");

// Bridge the shared mirror to the real Phase 1 enum (what BrokeredAuthProvider
// will do in Phase 4).
static AuthStepStatus Bridge(ab::MappedAuthStepStatus m) {
    return static_cast<AuthStepStatus>(static_cast<int>(m));
}

// =============================================================================
// Pipe-name construction
// =============================================================================

TEST_CASE("MakeAuthPipeName: per-session name (§2.1)", "[authbroker][pipe]") {
    REQUIRE(ab::MakeAuthPipeName(static_cast<std::uint32_t>(3)) ==
            "\\\\.\\pipe\\TcpRedirectorAuth_3");
    REQUIRE(ab::MakeAuthPipeName(static_cast<std::uint32_t>(0)) ==
            "\\\\.\\pipe\\TcpRedirectorAuth_0");
    REQUIRE(ab::MakeAuthPipeName(static_cast<std::uint32_t>(12345)) ==
            "\\\\.\\pipe\\TcpRedirectorAuth_12345");
}

TEST_CASE("MakeAuthPipeNameW: wide matches narrow", "[authbroker][pipe]") {
    REQUIRE(ab::MakeAuthPipeNameW(static_cast<std::uint32_t>(7)) ==
            L"\\\\.\\pipe\\TcpRedirectorAuth_7");
}

// =============================================================================
// Status <-> wire round-trip
// =============================================================================

TEST_CASE("AuthStatus wire round-trip", "[authbroker][status]") {
    const ab::AuthStatus all[] = {
        ab::AuthStatus::Continue, ab::AuthStatus::Complete,
        ab::AuthStatus::NoCredentials, ab::AuthStatus::Denied,
        ab::AuthStatus::Error
    };
    for (auto s : all) {
        REQUIRE(ab::AuthStatusFromWire(ab::ToWire(s)) == s);
    }
}

TEST_CASE("AuthStatus wire literals match plan §2.2", "[authbroker][status]") {
    REQUIRE(std::string(ab::ToWire(ab::AuthStatus::Continue)) == "continue");
    REQUIRE(std::string(ab::ToWire(ab::AuthStatus::Complete)) == "complete");
    REQUIRE(std::string(ab::ToWire(ab::AuthStatus::NoCredentials)) == "no_credentials");
    REQUIRE(std::string(ab::ToWire(ab::AuthStatus::Denied)) == "denied");
    REQUIRE(std::string(ab::ToWire(ab::AuthStatus::Error)) == "error");
}

TEST_CASE("Unknown wire status decodes to Error (no throw)", "[authbroker][status]") {
    REQUIRE(ab::AuthStatusFromWire("bogus") == ab::AuthStatus::Error);
    REQUIRE(ab::AuthStatusFromWire("") == ab::AuthStatus::Error);
}

// =============================================================================
// Protocol status -> Phase 1 AuthStepStatus mapping table
// =============================================================================

TEST_CASE("AuthStatus -> AuthStepStatus mapping (§2.4)", "[authbroker][mapping]") {
    REQUIRE(Bridge(ab::ToAuthStepStatus(ab::AuthStatus::Continue)) ==
            AuthStepStatus::ContinueNeeded);
    REQUIRE(Bridge(ab::ToAuthStepStatus(ab::AuthStatus::Complete)) ==
            AuthStepStatus::Complete);
    REQUIRE(Bridge(ab::ToAuthStepStatus(ab::AuthStatus::NoCredentials)) ==
            AuthStepStatus::NoCredentials);
    // Denied and Error both collapse to Failed (hard fail / internal error).
    REQUIRE(Bridge(ab::ToAuthStepStatus(ab::AuthStatus::Denied)) ==
            AuthStepStatus::Failed);
    REQUIRE(Bridge(ab::ToAuthStepStatus(ab::AuthStatus::Error)) ==
            AuthStepStatus::Failed);
}

// =============================================================================
// Round-trip: hello (helper -> service)
// =============================================================================

TEST_CASE("Round-trip: HelloMessage", "[authbroker][hello]") {
    ab::HelloMessage in;
    in.session_id = 4;
    in.nonce = "9f3ac1e0-nonce";
    in.helper_version = "1.0.0";

    std::string wire = ab::Serialize(in);

    // op must be discoverable via PeekOp.
    std::string op;
    REQUIRE(ab::PeekOp(wire, op) == ab::ParseError::Ok);
    REQUIRE(op == ab::kOpHello);

    ab::HelloMessage out;
    REQUIRE(ab::ParseHelloMessage(wire, out) == ab::ParseError::Ok);
    REQUIRE(out.session_id == in.session_id);
    REQUIRE(out.nonce == in.nonce);
    REQUIRE(out.helper_version == in.helper_version);
}

TEST_CASE("Round-trip: HelloMessage without optional helper_version", "[authbroker][hello]") {
    ab::HelloMessage in;
    in.session_id = 1;
    in.nonce = "abc";
    // helper_version intentionally empty (omitted on the wire).

    ab::HelloMessage out;
    REQUIRE(ab::ParseHelloMessage(ab::Serialize(in), out) == ab::ParseError::Ok);
    REQUIRE(out.session_id == 1);
    REQUIRE(out.nonce == "abc");
    REQUIRE(out.helper_version.empty());
}

// =============================================================================
// Round-trip: sspi_step request (service -> helper)
// =============================================================================

TEST_CASE("Round-trip: SspiStepRequest first step (empty server_token)",
          "[authbroker][sspi]") {
    ab::SspiStepRequest in;
    in.correlation_id = "conn-42";
    in.spn = "HTTP/proxy.corp.example.com";
    in.proxy_host = "proxy.corp.example.com";
    in.server_token = "";  // first step
    in.session_id = 3;

    std::string wire = ab::Serialize(in);

    std::string op;
    REQUIRE(ab::PeekOp(wire, op) == ab::ParseError::Ok);
    REQUIRE(op == ab::kOpSspiStep);

    ab::SspiStepRequest out;
    REQUIRE(ab::ParseSspiStepRequest(wire, out) == ab::ParseError::Ok);
    REQUIRE(out.correlation_id == in.correlation_id);
    REQUIRE(out.spn == in.spn);
    REQUIRE(out.proxy_host == in.proxy_host);
    REQUIRE(out.server_token.empty());
    REQUIRE(out.session_id == in.session_id);
}

TEST_CASE("Round-trip: SspiStepRequest continuation (base64 challenge)",
          "[authbroker][sspi]") {
    ab::SspiStepRequest in;
    in.correlation_id = "conn-99";
    in.spn = "HTTP/proxy.corp.example.com";
    in.proxy_host = "proxy.corp.example.com";
    in.server_token = "YIIF... (base64 challenge) ...==";
    in.session_id = 5;

    ab::SspiStepRequest out;
    REQUIRE(ab::ParseSspiStepRequest(ab::Serialize(in), out) == ab::ParseError::Ok);
    REQUIRE(out.server_token == in.server_token);
    REQUIRE(out.correlation_id == in.correlation_id);
}

// =============================================================================
// Round-trip: sspi_step response (helper -> service)
// =============================================================================

TEST_CASE("Round-trip: SspiStepResponse for each status", "[authbroker][sspi]") {
    const ab::AuthStatus all[] = {
        ab::AuthStatus::Continue, ab::AuthStatus::Complete,
        ab::AuthStatus::NoCredentials, ab::AuthStatus::Denied,
        ab::AuthStatus::Error
    };
    for (auto s : all) {
        ab::SspiStepResponse in;
        in.status = s;
        in.out_token = (s == ab::AuthStatus::Continue || s == ab::AuthStatus::Complete)
                           ? "b64-out-token" : "";
        in.detail = "some log detail";

        ab::SspiStepResponse out;
        REQUIRE(ab::ParseSspiStepResponse(ab::Serialize(in), out) == ab::ParseError::Ok);
        REQUIRE(out.status == s);
        REQUIRE(out.out_token == in.out_token);
        REQUIRE(out.detail == in.detail);
    }
}

TEST_CASE("Round-trip: SspiStepResponse without optional fields", "[authbroker][sspi]") {
    ab::SspiStepResponse in;
    in.status = ab::AuthStatus::Complete;
    // out_token empty, detail empty.

    ab::SspiStepResponse out;
    REQUIRE(ab::ParseSspiStepResponse(ab::Serialize(in), out) == ab::ParseError::Ok);
    REQUIRE(out.status == ab::AuthStatus::Complete);
    REQUIRE(out.out_token.empty());
    REQUIRE(out.detail.empty());
}

// =============================================================================
// Round-trip: release (service -> helper)
// =============================================================================

TEST_CASE("Round-trip: ReleaseMessage", "[authbroker][release]") {
    ab::ReleaseMessage in;
    in.correlation_id = "conn-42";

    std::string wire = ab::Serialize(in);

    std::string op;
    REQUIRE(ab::PeekOp(wire, op) == ab::ParseError::Ok);
    REQUIRE(op == ab::kOpRelease);

    ab::ReleaseMessage out;
    REQUIRE(ab::ParseReleaseMessage(wire, out) == ab::ParseError::Ok);
    REQUIRE(out.correlation_id == in.correlation_id);
}

// =============================================================================
// Malformed input handling — must NEVER throw, always return a ParseError
// =============================================================================

TEST_CASE("Malformed: invalid JSON", "[authbroker][malformed]") {
    ab::SspiStepRequest req;
    REQUIRE(ab::ParseSspiStepRequest("{not json", req) == ab::ParseError::InvalidJson);
    REQUIRE(ab::ParseSspiStepRequest("", req) == ab::ParseError::InvalidJson);

    ab::SspiStepResponse resp;
    REQUIRE(ab::ParseSspiStepResponse("garbage}}}", resp) == ab::ParseError::InvalidJson);
    // On malformed input the response status defaults to Error (safe fallback).
    REQUIRE(resp.status == ab::AuthStatus::Error);

    std::string op;
    REQUIRE(ab::PeekOp("<<<", op) == ab::ParseError::InvalidJson);
}

TEST_CASE("Malformed: JSON that is not an object", "[authbroker][malformed]") {
    ab::SspiStepRequest req;
    REQUIRE(ab::ParseSspiStepRequest("[1,2,3]", req) == ab::ParseError::InvalidJson);
    REQUIRE(ab::ParseSspiStepRequest("42", req) == ab::ParseError::InvalidJson);
    REQUIRE(ab::ParseSspiStepRequest("\"str\"", req) == ab::ParseError::InvalidJson);
}

TEST_CASE("Malformed: version mismatch", "[authbroker][malformed]") {
    std::string wire = R"({"v":2,"op":"sspi_step","correlation_id":"c","spn":"HTTP/h"})";
    ab::SspiStepRequest req;
    REQUIRE(ab::ParseSspiStepRequest(wire, req) == ab::ParseError::VersionMismatch);
}

TEST_CASE("Malformed: missing version field", "[authbroker][malformed]") {
    std::string wire = R"({"op":"release","correlation_id":"c"})";
    ab::ReleaseMessage rel;
    REQUIRE(ab::ParseReleaseMessage(wire, rel) == ab::ParseError::MissingField);
}

TEST_CASE("Malformed: missing required field", "[authbroker][malformed]") {
    // sspi_step without spn.
    std::string wire = R"({"v":1,"op":"sspi_step","correlation_id":"c"})";
    ab::SspiStepRequest req;
    REQUIRE(ab::ParseSspiStepRequest(wire, req) == ab::ParseError::MissingField);

    // hello without nonce.
    std::string hello = R"({"v":1,"op":"hello","session_id":3})";
    ab::HelloMessage h;
    REQUIRE(ab::ParseHelloMessage(hello, h) == ab::ParseError::MissingField);

    // response without status.
    std::string resp = R"({"v":1,"out_token":"x"})";
    ab::SspiStepResponse r;
    REQUIRE(ab::ParseSspiStepResponse(resp, r) == ab::ParseError::MissingField);
    REQUIRE(r.status == ab::AuthStatus::Error);
}

TEST_CASE("Malformed: wrong field type", "[authbroker][malformed]") {
    // correlation_id as number instead of string.
    std::string wire = R"({"v":1,"op":"sspi_step","correlation_id":5,"spn":"HTTP/h"})";
    ab::SspiStepRequest req;
    REQUIRE(ab::ParseSspiStepRequest(wire, req) == ab::ParseError::WrongType);

    // session_id as string instead of number in hello.
    std::string hello = R"({"v":1,"op":"hello","session_id":"three","nonce":"n"})";
    ab::HelloMessage h;
    REQUIRE(ab::ParseHelloMessage(hello, h) == ab::ParseError::WrongType);

    // v as string.
    std::string badv = R"({"v":"1","op":"release","correlation_id":"c"})";
    ab::ReleaseMessage rel;
    REQUIRE(ab::ParseReleaseMessage(badv, rel) == ab::ParseError::WrongType);
}

TEST_CASE("Malformed: unknown / mismatched op", "[authbroker][malformed]") {
    // Parsing a release blob as an sspi_step must report UnknownOp.
    std::string rel = R"({"v":1,"op":"release","correlation_id":"c"})";
    ab::SspiStepRequest req;
    REQUIRE(ab::ParseSspiStepRequest(rel, req) == ab::ParseError::UnknownOp);

    // Completely unknown op via PeekOp still yields the op string with Ok
    // (dispatch layer decides what to do); but typed parse rejects it.
    std::string weird = R"({"v":1,"op":"frobnicate"})";
    std::string op;
    REQUIRE(ab::PeekOp(weird, op) == ab::ParseError::Ok);
    REQUIRE(op == "frobnicate");
    ab::ReleaseMessage r;
    REQUIRE(ab::ParseReleaseMessage(weird, r) == ab::ParseError::UnknownOp);
}

// =============================================================================
// Framing
// =============================================================================

TEST_CASE("Framing: payload within limit passes through unchanged",
          "[authbroker][framing]") {
    std::string payload = ab::Serialize(ab::ReleaseMessage{});
    std::string framed;
    REQUIRE(ab::FrameMessage(payload, framed) == true);
    REQUIRE(framed == payload);  // message-mode pipe: no delimiter added.
    REQUIRE(ab::IsFrameWithinLimit(framed) == true);
}

TEST_CASE("Framing: oversized payload rejected", "[authbroker][framing]") {
    std::string huge(ab::kMaxMessageSize + 1, 'x');
    std::string framed = "sentinel";
    REQUIRE(ab::FrameMessage(huge, framed) == false);
    REQUIRE(framed.empty());
    REQUIRE(ab::IsFrameWithinLimit(huge) == false);
}

TEST_CASE("Constants: protocol version and defaults", "[authbroker][const]") {
    REQUIRE(ab::kProtocolVersion == 1);
    REQUIRE(ab::kMaxMessageSize == 65536u);
    REQUIRE(ab::kDefaultHelperTimeoutMs == 4000u);
}
