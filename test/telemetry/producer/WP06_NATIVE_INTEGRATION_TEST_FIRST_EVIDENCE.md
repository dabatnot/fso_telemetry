# WP06 native integration — R0/R1 test-first evidence

Date: 2026-07-17

Scope authorized for this RED checkpoint:

- R0 startup budget gate and reservation ordering;
- R1 separated timeout/periodic controller phases, priority, purge-all and allocation proof;
- tests and evidence only;
- no production edit, native pump, socket composition, R2 or loopback work.

## Contract locked by R0

`TelemetryRuntimeStartupContract.RealKnownBudgetRemainsFailClosedBeforeEveryAllocationAndSocketBoundary`
uses the real WP04 known-budget calculation. The subtotal remains incomplete with deferred categories, and startup reaches `BudgetFailure` before registry allocation, candidate registration or transport start.

`TelemetryRuntimeStartupContract.InjectedCompleteBudgetReservesCandidateBeforeTransportAndPublishesOnlyReady`
uses a synthetic complete budget available only through the test seam. It locks the order:

1. calculate budget;
2. allocate registry;
3. reserve the initial process candidate;
4. start transport;
5. enter `Ready`.

Every observed allocation, reservation and transport service boundary remains in `RuntimeState::Starting`; `Ready` becomes visible only after successful return from `start_transport()`.

The reserved candidate remains unpublished and is not an active session. The R1 purge oracle additionally reserves `0x1111`, scripts the controller allocator to draw `0x1111` first, and requires allocation of `0x2222` instead. This proves that `SessionController` cannot reuse the startup safety reservation.

R0 is GREEN against current production. It does not authorize native binding: the real budget is still incomplete.

## Contract locked by R1

The new tests require three `noexcept` seams:

- `service_timeouts(now_us)`;
- existing `service_reliability(now_us)`;
- `service_periodic(now_us)`;
- plus `purge_all(SessionCloseReason)`.

The oracles require:

- timeout closes an expired session before due reliable or heartbeat selection;
- a due reliable control datagram wins over a simultaneously due periodic heartbeat;
- periodic heartbeat becomes eligible only after the reliable output completes;
- a hitch creates one heartbeat and advances the next deadline from `now`, without catch-up;
- mission purge clears every slot, WELCOME replay cache, preproof ledger, pending output, probes, clock filter, reliable items, session/target rate-limit ownership and fairness cursors;
- mission purge preserves the process-scoped limiter watermark and HELLO/session-creation buckets keyed by source address;
- purge preserves the process-wide used-ID registry;
- a regressed post-purge datagram is rejected without mutation, and a saturated HELLO source remains saturated after mission purge;
- fresh sources can negotiate new sessions with new IDs, while old session/target limiter capacity is reclaimed;
- ordered maintenance and purge allocate nothing after controller `Ready`.

## Historical RED capture

Build:

```text
cmake --build build --target unittests telemetry_session_controller_allocation_contract_tests --parallel 4
Result: PASS; both executables linked successfully.
```

Targeted R0/R1 run against the first production implementation of the ordered seams:

```text
build/bin/Debug/unittests.exe --gtest_filter="TelemetryRuntimeStartupContract.RealKnownBudgetRemainsFailClosedBeforeEveryAllocationAndSocketBoundary:TelemetryRuntimeStartupContract.InjectedCompleteBudgetReservesCandidateBeforeTransportAndPublishesOnlyReady:TelemetryWp06HeartbeatContract.OrderedPhases*:TelemetryWp06HeartbeatContract.SeparatePeriodicPhase*:TelemetryWp06HeartbeatContract.PurgeAll*:TelemetryWp06HeartbeatContract.MissionPurge*"
Result: 9 tests; 7 PASS, 2 R1 FAIL.
```

The two intentional failures are:

```text
TelemetryWp06HeartbeatContract.MissionPurgePreservesSaturatedHelloSourceQuotaWhileClearingHandshakeState
TelemetryWp06HeartbeatContract.MissionPurgePreservesGlobalLimiterTimeAndRejectsRegressionWithoutMutation
```

Both failures isolated the former `purge_all()` implementation calling `ProtocolRateLimiter::reset(0)`: the saturated source was incorrectly admitted, and the regressed timestamp was incorrectly admitted and mutated cache/preproof/output state. Ordered phases, session-scoped purge, cursor reset and session/target admission capacity were already GREEN.

Allocation checkpoint:

```text
build/bin/Debug/telemetry_session_controller_allocation_contract_tests.exe --gtest_filter="TelemetryWp06AllocationContract.OrderedTimeoutReliablePeriodicAndPurgeAllocateNothingAfterReady"
Result: 1 test; PASS with zero post-`Ready` allocations.
```

## Green baselines preserved

Adjacent runtime, handshake, reliability and pre-existing heartbeat contracts, excluding the seven R1 tests introduced by this checkpoint:

```text
Result: 64 tests from 4 suites; 64 PASS.
```

Historical allocation baseline, excluding the then-new allocation oracle:

```text
Result: 3 tests; 3 PASS.
```

## Final independent GREEN verification

The final implementation was inspected before execution. The test oracles above were unchanged. `purge_all(reason)` now calls `close_slot()` for every owned slot, which invokes `ProtocolRateLimiter::purge_session(session_id, endpoint)` and therefore releases session and target entries. It then clears controller-owned session state. It does not call `ProtocolRateLimiter::reset()`, so the global monotonic watermark and pre-session source buckets survive mission purge. Shutdown remains object-lifetime teardown and is not modeled as mission purge.

Debug verification:

```text
Build: PASS.
R0/R1 NAT9: 9/9 PASS.
Allocation contract: 4/4 PASS.
TelemetryWp06HeartbeatContract: 24/24 PASS.
TelemetryWp06ReliabilityContract: 13/13 PASS.
Full unittests: 843/843 PASS; 2 pre-existing disabled tests.
Critical randomized stress: 39 tests x 25 iterations = 975/975 PASS.
Allocation randomized stress: 4 tests x 25 iterations = 100/100 PASS.
```

Release verification:

```text
Build: PASS.
R0/R1 NAT9: 9/9 PASS.
Allocation contract: 4/4 PASS.
TelemetryWp06HeartbeatContract: 24/24 PASS.
TelemetryWp06ReliabilityContract: 13/13 PASS.
Full unittests: 843/843 PASS; 2 pre-existing disabled tests.
Critical randomized stress: 39 tests x 10 iterations = 390/390 PASS.
Allocation randomized stress: 4 tests x 10 iterations = 40/40 PASS.
```

No test failure, flaky iteration, allocation regression, build error or purge-semantics anomaly was observed. Expected diagnostics emitted by unrelated parser and safe-string tests appeared during the full suites, but their tests passed.

## Checkpoint status

R0 and the authorized R1 controller slice are GREEN in Debug and Release. Production remains fail-closed before socket allocation because the real global startup budget is incomplete. WP06, native end-to-end `P1-AC-005`, and `G1-D` remain open. R2/native pump and loopback tests have not started.

## Requirement and acceptance mapping

| ID | Evidence in this checkpoint | Honest status |
|---|---|---|
| `P1-REQ-009` | Initial process reservation remains unpublished; a controller draw colliding with it is rejected, purge retains all used IDs, and the next session receives a fresh ID. | `PARTIAL` — the no-reuse slice is directly covered; complete identity/profile and entropy-failure closure remains distributed across WP03 evidence. |
| `P1-REQ-014` | The real incomplete subtotal faults before allocation/socket work; only a synthetic complete budget reaches the ordered allocation boundaries. The allocation executable requires zero post-`Ready` allocation for the new phases and purge. | `PARTIAL` — baseline/candidate/delta and remaining deferred WP07–WP09 storage are intentionally not priced here. |
| `P1-REQ-017` | `purge_all(MissionDiscontinuity)` releases session-scoped slots, REL state, cache, preproof, heartbeat state, output, session/target limiter ownership and cursors while preserving process-scoped IDs and pre-session abuse history. | `PARTIAL` — the R1 controller purge slice is green; engine mission/menu/shutdown and socket convergence remain outside R0/R1. |
| `P1-REQ-018` | Exact timeout boundary is ordered before REL and periodic work; due REL is ordered before heartbeat and remains transactional until completion. Existing session/REL baselines remain green. | `PARTIAL` — the R1 controller ordering slice is green; native tick composition remains unproven. |
| `P1-REQ-019` | Periodic heartbeat after REL completion, hitch coalescing, probe/filter purge and cursor restart are green; the existing heartbeat boundary/filter/overflow suite remains green. | `PARTIAL` — isolated/controller behavior is green, native tick composition remains open. |
| `P1-AC-010` | All 24 heartbeat tests, including ordered timeout/REL/periodic and process-lifetime purge semantics, pass in Debug and Release. | `PARTIAL/OPEN` — the isolated/controller slice is green; do not close acceptance until later native composition evidence is reviewed. |
| `P1-AC-005` | R0 proves only fail-closed startup and reservation semantics; existing isolated handshake tests remain green. | `OPEN` — no R2 pump or real IPv4/IPv6 loopback negotiation was started. |
| `G1-D` | R1 defines controller convergence and purge ownership. | `OPEN` — mission/menu/shutdown plus native sockets are not proven end to end. |
