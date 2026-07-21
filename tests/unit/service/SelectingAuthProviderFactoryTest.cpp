// =============================================================================
// SelectingAuthProviderFactoryTest.cpp
//
// Unit tests for the Phase 6/7 selecting auth-provider factory:
//   src/service/TcpRedirectorService/infrastructure/auth/SelectingAuthProviderFactory.h
//   src/service/TcpRedirectorService/infrastructure/auth/DropAuthProvider.h
//
// These tests exercise the DECISION TABLE (§5) in isolation using fakes for the
// two injected ports (IUserSessionResolver, IBrokeredParamsSource). No Win32,
// no named pipes, no real sessions.
//
// Decision table under test:
//   feature off                       -> LocalSspiProvider
//   resolved + helper                 -> BrokeredAuthProvider (DropOnFailure=true)
//   resolve fails,  policy=drop       -> DropAuthProvider     (Failed)
//   no helper,      policy=drop       -> DropAuthProvider     (Failed)
//   no helper,      policy=system     -> LocalSspiProvider    (DropOnFailure=false)
//   no helper,      policy=error      -> DropAuthProvider     (Failed)
//   DropAuthProvider::NextToken       -> always Failed
//
// Windows-only (BrokeredAuthProvider pulls in AuthBrokerClient). Catch2 v3.
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "infrastructure/auth/SelectingAuthProviderFactory.h"
#include "infrastructure/auth/DropAuthProvider.h"
#include "infrastructure/auth/BrokeredAuthProvider.h"
#include "infrastructure/auth/LocalSspiProvider.h"

namespace auth = tcp_redirector::infrastructure::auth;
namespace infra = tcp_redirector::infrastructure;
using tcp_redirector::domain::ports::AuthStepStatus;
using tcp_redirector::domain::ports::ConnectionIdentity;
using tcp_redirector::domain::ports::IAuthProvider;
using tcp_redirector::domain::AuthFallbackPolicy;

namespace {

// ---- Fakes -----------------------------------------------------------------

// Session resolver that either always succeeds (returning a fixed sessionId) or
// always fails, depending on construction.
class FakeSessionResolver : public auth::IUserSessionResolver {
public:
    FakeSessionResolver(bool ok, std::uint32_t sid) : m_ok(ok), m_sid(sid) {}

    bool ResolveSession(const ConnectionIdentity& /*id*/,
                        std::uint32_t& sessionIdOut) override {
        ++calls;
        if (!m_ok) return false;
        sessionIdOut = m_sid;
        return true;
    }

    int calls = 0;

private:
    bool m_ok;
    std::uint32_t m_sid;
};

// Params source that returns a helper for exactly one session id (or never).
class FakeParamsSource : public auth::IBrokeredParamsSource {
public:
    FakeParamsSource(bool hasHelper, std::uint32_t forSession)
        : m_has(hasHelper), m_session(forSession) {}

    bool BuildBrokeredParams(std::uint32_t sessionId,
                             infra::BrokeredAuthParams& out) override {
        ++calls;
        if (!m_has || sessionId != m_session) return false;
        out.session_id = sessionId;
        out.expected_nonce = "deadbeef";
        out.spn = "HTTP/proxy.corp.example.com";
        out.proxy_host = "proxy.corp.example.com";
        out.helper_timeout_ms = 4000;
        return true;
    }

    int calls = 0;

private:
    bool m_has;
    std::uint32_t m_session;
};

// Helper: classify the concrete provider type via dynamic_cast.
enum class Kind { Local, Brokered, Drop, Unknown };

Kind ClassifyProvider(IAuthProvider* p) {
    if (dynamic_cast<infra::LocalSspiProvider*>(p)) return Kind::Local;
    if (dynamic_cast<infra::BrokeredAuthProvider*>(p)) return Kind::Brokered;
    if (dynamic_cast<infra::DropAuthProvider*>(p)) return Kind::Drop;
    return Kind::Unknown;
}

auth::SelectingAuthProviderConfig MakeCfg(bool enabled, AuthFallbackPolicy pol) {
    auth::SelectingAuthProviderConfig c;
    c.perUserEnabled = enabled;
    c.fallbackPolicy = pol;
    c.spn = "HTTP/proxy.corp.example.com";
    c.proxyHost = "proxy.corp.example.com";
    c.helperTimeoutMs = 4000;
    return c;
}

}  // namespace

// =============================================================================
// Feature flag OFF => always LocalSspiProvider (legacy machine account).
// =============================================================================
TEST_CASE("feature off yields LocalSspiProvider", "[selectauth]") {
    FakeSessionResolver resolver(/*ok*/ true, /*sid*/ 3);
    FakeParamsSource params(/*hasHelper*/ true, /*forSession*/ 3);

    auth::SelectingAuthProviderFactory factory(
        MakeCfg(/*enabled*/ false, AuthFallbackPolicy::Drop),
        &resolver, &params, nullptr);

    auto p = factory.Create(ConnectionIdentity{});
    REQUIRE(p != nullptr);
    REQUIRE(ClassifyProvider(p.get()) == Kind::Local);
    REQUIRE(p->DropOnFailure() == false);
    // Resolver/params must NOT even be consulted when the feature is off.
    REQUIRE(resolver.calls == 0);
    REQUIRE(params.calls == 0);
}

// =============================================================================
// Resolved + helper present => BrokeredAuthProvider (drop-semantics).
// =============================================================================
TEST_CASE("resolved with helper yields BrokeredAuthProvider", "[selectauth]") {
    FakeSessionResolver resolver(/*ok*/ true, /*sid*/ 7);
    FakeParamsSource params(/*hasHelper*/ true, /*forSession*/ 7);

    auth::SelectingAuthProviderFactory factory(
        MakeCfg(/*enabled*/ true, AuthFallbackPolicy::Drop),
        &resolver, &params, nullptr);

    ConnectionIdentity id;
    id.client_port = 51000;
    auto p = factory.Create(id);
    REQUIRE(p != nullptr);
    REQUIRE(ClassifyProvider(p.get()) == Kind::Brokered);
    REQUIRE(p->DropOnFailure() == true);
    REQUIRE(resolver.calls == 1);
    REQUIRE(params.calls == 1);
}

// =============================================================================
// Resolve fails, policy=drop => DropAuthProvider.
// =============================================================================
TEST_CASE("resolve failure with drop policy yields DropAuthProvider",
          "[selectauth]") {
    FakeSessionResolver resolver(/*ok*/ false, /*sid*/ 0);
    FakeParamsSource params(/*hasHelper*/ true, /*forSession*/ 1);

    auth::SelectingAuthProviderFactory factory(
        MakeCfg(/*enabled*/ true, AuthFallbackPolicy::Drop),
        &resolver, &params, nullptr);

    auto p = factory.Create(ConnectionIdentity{});
    REQUIRE(ClassifyProvider(p.get()) == Kind::Drop);
    REQUIRE(p->DropOnFailure() == true);
    // params must not be consulted if resolution already failed.
    REQUIRE(params.calls == 0);
}

// =============================================================================
// Resolved but NO helper for that session, policy=drop => DropAuthProvider.
// =============================================================================
TEST_CASE("no helper with drop policy yields DropAuthProvider", "[selectauth]") {
    FakeSessionResolver resolver(/*ok*/ true, /*sid*/ 9);
    // Helper exists only for session 2, but resolver returns 9 => miss.
    FakeParamsSource params(/*hasHelper*/ true, /*forSession*/ 2);

    auth::SelectingAuthProviderFactory factory(
        MakeCfg(/*enabled*/ true, AuthFallbackPolicy::Drop),
        &resolver, &params, nullptr);

    auto p = factory.Create(ConnectionIdentity{});
    REQUIRE(ClassifyProvider(p.get()) == Kind::Drop);
    REQUIRE(params.calls == 1);  // it WAS consulted, and missed.
}

// =============================================================================
// No helper, policy=system => LocalSspiProvider (legacy fallback allowed).
// =============================================================================
TEST_CASE("no helper with system policy yields LocalSspiProvider",
          "[selectauth]") {
    FakeSessionResolver resolver(/*ok*/ true, /*sid*/ 9);
    FakeParamsSource params(/*hasHelper*/ false, /*forSession*/ 0);

    auth::SelectingAuthProviderFactory factory(
        MakeCfg(/*enabled*/ true, AuthFallbackPolicy::System),
        &resolver, &params, nullptr);

    auto p = factory.Create(ConnectionIdentity{});
    REQUIRE(ClassifyProvider(p.get()) == Kind::Local);
    REQUIRE(p->DropOnFailure() == false);
}

// =============================================================================
// No helper, policy=error => DropAuthProvider (drop + error log).
// =============================================================================
TEST_CASE("no helper with error policy yields DropAuthProvider", "[selectauth]") {
    FakeSessionResolver resolver(/*ok*/ false, /*sid*/ 0);
    FakeParamsSource params(/*hasHelper*/ false, /*forSession*/ 0);

    auth::SelectingAuthProviderFactory factory(
        MakeCfg(/*enabled*/ true, AuthFallbackPolicy::Error),
        &resolver, &params, nullptr);

    auto p = factory.Create(ConnectionIdentity{});
    REQUIRE(ClassifyProvider(p.get()) == Kind::Drop);
    REQUIRE(p->DropOnFailure() == true);
}

// =============================================================================
// Null resolver/params (feature on, no dependencies) => fallback (drop default).
// =============================================================================
TEST_CASE("null dependencies fall back per policy", "[selectauth]") {
    auth::SelectingAuthProviderFactory factory(
        MakeCfg(/*enabled*/ true, AuthFallbackPolicy::Drop),
        /*resolver*/ nullptr, /*params*/ nullptr, nullptr);

    auto p = factory.Create(ConnectionIdentity{});
    REQUIRE(ClassifyProvider(p.get()) == Kind::Drop);
}

// =============================================================================
// DropAuthProvider always returns Failed and clears the out token.
// =============================================================================
TEST_CASE("DropAuthProvider always returns Failed", "[selectauth][drop]") {
    infra::DropAuthProvider drop;
    std::string out = "garbage";
    REQUIRE(drop.NextToken("HTTP/x", "", out) == AuthStepStatus::Failed);
    REQUIRE(out.empty());
    // Idempotent across multiple steps.
    out = "again";
    REQUIRE(drop.NextToken("HTTP/x", "challenge", out) == AuthStepStatus::Failed);
    REQUIRE(out.empty());
    REQUIRE(drop.DropOnFailure() == true);
}
