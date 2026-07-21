# Wintun Embedded — DIRECT Passthrough Design Options

**Type:** Design / analysis only (NO implementation).
**Scope:** How to let traffic from applications NOT in the redirect list (DIRECT flows) leave via the standard system route in `capture_mode=wintun`, `wintun.engine=embedded`, instead of being `tcp_abort`'d.
**Date:** 2026-07-21

---

## 1. Problem statement & current behaviour

In Wintun embedded mode, **ALL** IPv4-TCP is pulled into the TUN via split-tunnel routes
(`RouteInstaller::InstallSplitTunnel`, an on-link ladder covering `0.0.0.0/0` minus carve-outs
`127/8`, `0/8`, `169.254/16`, plus a `<proxy>/32` bypass via the physical gateway).

The in-process lwIP engine ([`Tun2SocksEngineEmbedded`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp)) decides per flow in
[`EngineTramp::OnAccept()`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:239) using
[`DecideFlow()`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:689):

| Decision | Current action | Code |
|---|---|---|
| **PROXY** | bind loopback socket → register `(ephemeral_port → original-dst)` in `ConnectionTable` → `connect()` to relay `127.0.0.1:relay_port` → per-flow reader thread | [`OnAccept`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:290) |
| **DIRECT** | `tcp_abort(newpcb)` — **connection rejected** (the behaviour we want to change) | [`OnAccept:261`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:261) |
| **BLOCK** | `tcp_abort(newpcb)` | [`OnAccept:256`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:256) |

### Why DIRECT is dropped today (the core constraint)
A blocking `connect()` to the real destination made from `OnAccept` runs **on the engine thread
under `m_core_lock`**. Because the split-tunnel routes cover `0.0.0.0/0`, the SYN of that outbound
socket is routed **back into the same TUN** — and the only thread that can read/pump the TUN is the
engine thread, which is stuck inside `connect()`. Result: deadlock/freeze of the whole stack
(documented in `АРХИТЕКТУРА §3.2`, `ИЗВЕСТНЫЕ_ПРОБЛЕМЫ §0.5`, and inline at
[`OnAccept:261`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:261)).

**Therefore any real solution must satisfy two invariants:**
1. **No lwIP call may block the engine thread** (all outbound work must be async / off-thread — mirroring the existing PROXY path, which uses per-flow `sock_reader` threads + a reaper).
2. **DIRECT outbound packets must NOT re-enter the TUN** — they must be forced onto the physical NIC (escape the `0.0.0.0/0` ladder).

### Threading & primitives already available (reused by several options below)
- Per-flow reader-thread pattern + reaper — [`FlowSocketReader`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:474), [`ReaperThreadMain`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:631). This is exactly the async machinery a DIRECT socket needs.
- Physical route discovery — [`RouteInstaller::InstallHostBypass`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/RouteInstaller.cpp:287) already calls `GetBestRoute2()` to find the physical interface + next-hop + **best source address** for an arbitrary dst. This is the primitive needed to bind/scope a socket to the physical NIC.
- Raw packet injection back into TUN — [`WintunSession::Send`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/WintunSession.h:126).
- PID→process resolution — [`ProcessResolver`](../src/service/TcpRedirectorService/infrastructure/process/ProcessResolver.h) (per-SYN, with one 20 ms retry; per-app not per-destination).
- IPv6 is neutralised (RST on SYN) by [`HandleIpv6Packet`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:920); all options are IPv4-only and inherit that limitation.

---

## 2. Options

### Option 1 — Async outbound socket, forced onto the physical interface (in-engine "direct relay")

**Principle of operation.**
Treat DIRECT exactly like PROXY, except the per-flow socket connects to the **real
`original-dst`** instead of the relay, and is **pinned to the physical NIC** so its packets
escape the TUN ladder. Concretely, in [`OnAccept`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:261) when `decision == Direct`:
1. Create a `SOCK_STREAM` socket. **Do not** `bind` to loopback and **do not** register in `ConnectionTable` (that is a PROXY-only step).
2. Force it off the TUN using one (or a belt-and-suspenders combination) of:
   - `IP_UNICAST_IF` set to the physical interface index (best: overrides the routing table's egress interface for this socket), and/or
   - `bind()` to the physical NIC's source IPv4 (from `GetBestRoute2()` — the same call `InstallHostBypass` already uses), and/or
   - a per-dst `<dst>/32` bypass route via the physical gateway (reuse [`InstallHostBypass`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/RouteInstaller.cpp:287) generalised to any dst) so LPM sends it out the physical NIC.
3. Make `connect()` **non-blocking / connect off-thread**: kick the per-flow `sock_reader` thread (like PROXY) and let it do the connect + bidirectional pump. The engine thread returns `ERR_OK` immediately from `OnAccept`; data is pumped through `tcp_write`/`OnRecv` exactly as for PROXY.
4. Reuse `CloseFlow`/`RetireFlow`/reaper unchanged (the Flow struct already models a generic pcb↔socket pair; only "socket target" and "no ConnectionTable entry" differ).

This is the direct implementation of the deferred design noted at [`OnAccept:261`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:261) ("async-outbound + interface-bind").

**Pros.**
- Fully realises "selective proxy + direct the rest" **inside embedded mode** — the one thing embedded currently can't do.
- Keeps all the embedded-only advantages: per-user Kerberos on the PROXY path is untouched; no dependency on WinDivert driver.
- Reuses the proven per-flow reader/reaper machinery → threading model is already battle-tested (post-B14/B15 stability fixes).
- Per-app selection stays where it already is (`DecideFlow`), no need to know destinations in advance.
- Self-contained: no new external process, no route churn if `IP_UNICAST_IF`/source-bind is used (no per-dst routes needed).

**Cons / risks.**
- **Escaping the TUN is the crux.** `IP_UNICAST_IF` is the cleanest (per-socket egress override, no route-table mutation) but the interface index must be discovered reliably and re-discovered on network changes (roaming, VPN up/down). Source-IP `bind()` alone is *not* always sufficient because LPM can still pick the TUN as egress; `IP_UNICAST_IF` is more robust. `SO_DONTROUTE` is **not** appropriate (it restricts to on-link only and breaks routed/gateway traffic). A per-dst `/32` route works but re-introduces route churn and a teardown/leak-cleanup burden (cf. F1 stale-bypass bug in the ladder plan).
- This is effectively re-implementing a TCP proxy datapath (terminate-in-lwIP, re-originate on physical NIC) → it is **NAT-like**: the DIRECT connection's real source becomes the service host, not the app. Fine for most TCP, but breaks anything doing source-IP validation or inbound reverse channels (rare for outbound client TCP).
- **PID-resolution timing (§3.2):** DIRECT is the default when `pid==0` ([`DecideFlow:711`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:711)). Today a `pid==0` mis-DIRECT is simply dropped; with passthrough it would silently proxy-around, which is arguably *better* (no drop) but means a momentarily-unresolved matched app could escape the proxy for its first connection.
- Loop-guard needed: our own outbound DIRECT sockets must never be re-captured. Self-PID is already DIRECT ([`DecideFlow:719`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:719)); with `IP_UNICAST_IF`/source-bind escaping the TUN they won't re-enter anyway, but this must be verified.
- Extra socket + thread per DIRECT flow → on a busy host where *most* traffic is DIRECT, this is a large amount of user-space copying/threading (embedded becomes a full software router for everything). Scales worse than native passthrough.
- IPv6 still blocked (inherits `block_ipv6`); DIRECT IPv6 apps get RST → IPv4 fallback only.

**Implementation complexity: Medium–High.**
Main touch points: [`OnAccept`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:239) (new DIRECT branch), [`FlowSocketReader`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:474) (async connect + connect-failure handling for a remote dst vs local relay), a new helper to resolve physical ifIndex/source IP (wrap `GetBestRoute2`, likely factored out of `RouteInstaller`), a `Flow::is_direct` flag, and `EmbeddedProcessFilter`/config plumbing in [`WintunCapture.cpp`](../src/service/TcpRedirectorService/infrastructure/capture/WintunCapture.cpp:448). The threading contract is reused, which contains the complexity.

**Compatibility.**
- **Kerberos per-user:** ✅ unaffected — PROXY path (which carries per-user auth via relay) is unchanged; DIRECT never touches auth.
- **IPv6 blocking:** ⚠️ unchanged (IPv4-only passthrough; IPv6 still RST).
- **Existing PROXY path:** ✅ fully preserved; DIRECT is an additive branch.

---

### Option 2 — Route-ladder / narrower TUN routing (exclude DIRECT destinations from the TUN)

**Principle of operation.**
Instead of routing all of `0.0.0.0/0` into the TUN and rejecting DIRECT at the lwIP layer, keep
*only the destinations that must be proxied* inside the TUN. Two sub-variants:
- **(2a) Per-destination catch-out:** install higher-priority `<dst>/32` routes via the physical
  gateway for DIRECT destinations (generalise [`InstallHostBypass`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/RouteInstaller.cpp:287)) so those packets never enter the TUN. Learned dynamically: on the first SYN for a DIRECT dst, add its `/32` bypass; the app retransmits and the retry goes physical.
- **(2b) Positive routing:** don't install the catch-all ladder at all; only route the *proxy target destinations* into the TUN. But targets are chosen **per-app (PID), not per-destination** — the app can talk to any dst — so there is no known finite dst set to route.

**Pros.**
- (2a) leaves the actual DIRECT datapath to the OS/native stack → **best possible DIRECT performance and correctness** (no NAT, real source IP, IPv6 could even work natively for those dsts if we also carved IPv6).
- Reuses existing `/32`-bypass + `GetBestRoute2` machinery and its teardown logic.
- No per-flow user-space proxying of DIRECT traffic.

**Cons / risks.**
- **Fundamental per-app vs per-destination mismatch.** Selection is by process (`DecideFlow` resolves PID→path), but routing is by destination IP. A given `/32` can be used by *both* a PROXY app and a DIRECT app simultaneously → you cannot cleanly bypass a dst for one app but tunnel it for another using routes alone. This is the killer for the common "same host, different apps" case.
- **First-packet drop / latency:** the decision (and thus the `/32` install) happens only *after* the SYN already entered the TUN and reached lwIP. The first SYN is still captured; you must `tcp_abort` it and rely on app retransmit after the route exists — visible connect latency, and some apps give up.
- **Route churn & leaks:** dynamic `/32`s accumulate; need TTL/eviction and crash-safe cleanup (the ladder plan already flags stale-bypass leakage, F1, as a real bug). Thousands of DIRECT dsts = thousands of routes.
- **Coexistence with other full-tunnel VPNs** is explicitly *not solvable* by finite ladders (see `wintun_route_ladder_design_2026-07-14.md` — LPM ties resolve by metric then non-deterministic tiebreak).
- (2b) is infeasible: no finite destination set for per-app selection.

**Implementation complexity: Medium (2a) / N/A (2b).**
Touch points: generalise [`RouteInstaller::InstallHostBypass`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/RouteInstaller.cpp:287)/`Uninstall`/`CleanupStaleBypass`, add a dynamic dst-bypass manager with eviction, and an "abort + learn route" branch in [`OnAccept`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.cpp:261). But the per-app/per-dst mismatch means it can't be *correct*, only approximate.

**Compatibility.**
- **Kerberos:** ✅ unaffected.
- **IPv6:** could theoretically improve (native IPv6 for bypassed dsts) but only if IPv6 carve-outs added; today ⚠️ still blocked.
- **PROXY path:** ⚠️ **at risk** — if a proxied app and a direct app share a dst, the `/32` bypass would wrongly send the proxied app's traffic direct. Correctness hazard.

---

### Option 3 — WinDivert-based selective capture (recommend/prefer WinDivert, or hybrid selection)

**Principle of operation.**
Selection happens at the **WinDivert layer**, where non-matched traffic is simply *never diverted*
and flows natively (exactly how [`WinDivertCapture`](../src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp) already works — it only rewrites matched
outbound SYNs' dst → `127.0.0.1:relay_port`, everything else passes untouched). Two framings:
- **(3a) Operational recommendation:** for the "selective proxy + direct the rest" use-case, use
  `capture_mode=windivert`. This is already the documented guidance (`ИЗВЕСТНЫЕ_ПРОБЛЕМЫ §0.5`,
  [`WintunCapture.cpp:477`](../src/service/TcpRedirectorService/infrastructure/capture/WintunCapture.cpp:477)).
- **(3b) Hybrid:** keep the Wintun TUN + relay + embedded auth, but move *capture/selection* to a WFP/WinDivert front-end that only steers matched apps into the TUN, letting the rest flow native. Essentially WinDivert decides, TUN/relay executes.

**Pros.**
- (3a) needs **zero code** — it already works and is documented. DIRECT is native (correct source IP, native IPv6, best performance).
- WinDivert selection is at the kernel WFP layer → robust against coexisting full-tunnel VPNs (the ladder can't guarantee this).
- Per-app selection is native to WinDivert's model.

**Cons / risks.**
- **Loses the reason embedded exists.** The whole point of embedded is **per-user Kerberos** (external `tun2socks` can't do per-user auth; WinDivert-relay can, but the embedded lwIP + relay chain is the per-user-auth-friendly path). If you tell users "for direct passthrough, switch to WinDivert," you're telling per-user-Kerberos users they can't have selective direct passthrough. That is precisely the gap this task is meant to close.
- (3b) is a **large architecture change**: adding a WFP/WinDivert selection front-end in front of the TUN duplicates capture layers, doubles the driver surface (WinDivert64.sys), and re-introduces the WinDivert dependency that Wintun mode was meant to avoid. Complexity and failure modes multiply.
- WinDivert requires the kernel driver/service ([`EnsureDriverRunning`](../src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:111)); some deployment environments specifically avoid it.

**Implementation complexity: Low (3a, docs only) / Very High (3b).**

**Compatibility.**
- **Kerberos:** (3a) ✅ WinDivert-relay path supports per-user auth; but this abandons embedded — depends on whether the user needs *embedded specifically*. (3b) preserves embedded auth but at huge cost.
- **IPv6:** ✅ native for DIRECT (not forced-blocked in WinDivert mode).
- **PROXY path:** (3a) different mode entirely; (3b) unchanged embedded PROXY path but new selection layer.

---

### Option 4 — lwIP-level direct forwarding with source-substitution / NAT out the physical NIC

**Principle of operation.**
Rather than terminating the DIRECT TCP connection in lwIP and re-originating via a Winsock socket
(Option 1), perform **packet-level NAT** for DIRECT flows: keep the connection as raw IP packets,
rewrite the source to the physical NIC's address, and emit them on the physical interface via a
**second raw egress path** (e.g. a raw socket / `WinDivertSend`-style injection, or a second
WinsockSOCK that we feed reconstructed segments). Return packets are NAT'd back and injected into
the TUN via [`WintunSession::Send`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/WintunSession.h:126).

**Pros.**
- Conceptually avoids per-flow user-space TCP termination for DIRECT (could be lighter than Option 1 if done as pure packet NAT).
- Could preserve more TCP semantics than a re-originated socket in edge cases.

**Cons / risks.**
- **Windows has no clean user-mode raw-TCP send path.** Raw sockets on Windows cannot send TCP (blocked since XP SP2). Emitting arbitrary TCP on the physical NIC from user space requires **WinDivert (kernel driver)** or a WFP callout — i.e. it collapses into Option 3's dependency, defeating the purpose of Wintun mode.
- If instead you feed a normal socket (like Option 1) then it *is* Option 1, but with a hand-rolled NAT/segment-reassembly layer bolted on → strictly more complexity, more bug surface, for no benefit.
- Full stateful NAT table, checksum recompute, seq/ack translation, MSS/PMTU handling — essentially writing a second TCP/NAT stack. High risk of the same re-entry/threading bugs (B14/B15) that took multiple iterations to stabilise.
- Return-path injection into the TUN must be correct or lwIP/app desyncs.

**Implementation complexity: Very High** (and gated on a kernel component it was trying to avoid).

**Compatibility.**
- **Kerberos:** ✅ unaffected (PROXY untouched).
- **IPv6:** ⚠️ still blocked.
- **PROXY path:** ✅ preserved, but the added NAT stack raises regression risk for the shared engine thread.

---

### Option 5 — Additional approaches considered

- **`IP_UNICAST_IF` (per-socket egress interface).** *Not a standalone option* — it is the recommended escape mechanism **inside Option 1**. It overrides the egress interface for a single socket without mutating the route table, cleanly sending DIRECT sockets out the physical NIC while the TUN ladder stays in place. Preferred over source-bind and over `/32` routes.
- **`SO_DONTROUTE`.** ❌ Rejected: restricts sends to on-link destinations only; breaks any routed/gateway-bound DIRECT traffic. Not a general passthrough mechanism.
- **WFP per-app split-tunnel (kernel callout / `FWPM_LAYER_ALE_CONNECT_REDIRECT`).** Could do true per-app selection in the kernel and leave DIRECT native. But this is a **new kernel component** with driver-signing, complexity, and support burden far beyond this task; it also overlaps with what WinDivert already provides (Option 3). Noted, not recommended.
- **`process_filter_enabled=false` (proxy everything).** The existing "no DIRECT arises" escape hatch — valid when the user wants *all* traffic proxied, but it does **not** satisfy the requirement (it proxies rather than directs the non-listed apps).

---

## 3. Comparison summary

| Option | DIRECT correctness | Escapes TUN cleanly | Per-app selection | Preserves embedded/Kerberos | Complexity | Verdict |
|---|---|---|---|---|---|---|
| **1. Async outbound + `IP_UNICAST_IF`** | Good (NAT-like) | Yes (per-socket) | Yes (native, `DecideFlow`) | Yes | Medium–High | **Recommended** |
| **2a. Dynamic `/32` bypass** | Excellent (native) | Yes (per-dst) | ❌ per-dst not per-app | Yes | Medium | Correctness hazard |
| **3a. Use WinDivert mode** | Excellent (native) | N/A | Yes | ❌ abandons embedded | Low (docs) | Fallback / status quo |
| **3b. WinDivert+TUN hybrid** | Excellent | Yes | Yes | Yes but heavy | Very High | Overkill |
| **4. lwIP packet NAT** | Good | Needs kernel send | Yes | Yes | Very High | Not viable in user mode |

---

## 4. Recommendation (ranked)

1. **Option 1 — Async outbound socket forced onto the physical NIC via `IP_UNICAST_IF`.**
   This is the only approach that delivers *selective proxy + direct-the-rest* **inside embedded
   mode** (thus preserving per-user Kerberos and avoiding WinDivert), while respecting both hard
   invariants (never block the engine thread; never re-enter the TUN). It reuses the already-hardened
   per-flow reader/reaper machinery and the existing `GetBestRoute2` physical-path discovery. Best
   overall trade-off. Prefer `IP_UNICAST_IF` as the escape mechanism (fall back to source-IP bind if
   the interface index is momentarily unknown; avoid per-dst routes and `SO_DONTROUTE`).

2. **Option 3a — Recommend `capture_mode=windivert`** as the *documented fallback* for users who do
   not specifically need embedded/per-user-Kerberos. Zero code; already works. Keep as the honest
   "if you don't need embedded, this is simpler" guidance, but it does **not** close the embedded gap.

3. **Option 2a — Dynamic `/32` bypass** only as a *narrow optimisation on top of Option 1* (e.g. to
   offload high-volume DIRECT dsts to the native stack), never as the primary mechanism, because of
   the per-app/per-dst correctness hazard and route-leak burden.

4. **Options 3b and 4 — not recommended.** 3b is disproportionate; 4 is not viable in user mode on
   Windows (no raw TCP send without a kernel driver, which defeats Wintun mode's purpose).

### Suggested config additions (for Option 1)
- `wintun.direct_passthrough` (bool, default `false` for backward-compat / current fail-fast
  behaviour). When `true`, DIRECT flows are passed through via the physical NIC instead of
  `tcp_abort`. Wire through [`EmbeddedProcessFilter`](../src/service/TcpRedirectorService/infrastructure/capture/wintun/Tun2SocksEngineEmbedded.h:111) and
  [`WintunCapture.cpp`](../src/service/TcpRedirectorService/infrastructure/capture/WintunCapture.cpp:448).
- (Optional) a `direct` policy value for rules, so an app can be *explicitly* directed (vs. falling
  through to the DIRECT default), distinct from `proxy`/`block`, surfaced in `RuleEngine`/`apps[]`.
- (Optional) `wintun.direct_egress_interface` override (auto-detect via `GetBestRoute2` by default;
  manual override for multi-NIC / VPN-coexistence edge cases).
- (Optional) `wintun.direct_fallback` = `drop` | `proxy` to choose behaviour when a DIRECT flow's
  physical egress cannot be established (e.g. no physical route) — preserving today's safe drop as a
  fallback.

### Notes for a future implementation task
- Keep the DIRECT connect **off the engine thread** (in `sock_reader`, non-blocking connect) — this
  is the single most important correctness rule (invariant #1).
- Verify with `IP_UNICAST_IF` set that DIRECT sockets do **not** re-enter the TUN (invariant #2)
  before enabling by default.
- Re-detect the physical interface on network-change notifications (`NotifyIpInterfaceChange`) so
  DIRECT egress survives roaming / VPN up-down.
- DIRECT remains IPv4-only while `block_ipv6=true`; document that DIRECT IPv6 apps still fall back to
  IPv4 via the existing RST mechanism.
