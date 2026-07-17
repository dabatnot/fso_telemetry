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

## Historical R0/R1 checkpoint status

At this historical checkpoint, R0 and the authorized R1 controller slice were GREEN in Debug and Release, while R2 had not started. The final R2 verification below supersedes only that native-composition status; production still remains fail-closed before socket allocation because the real global startup budget is incomplete.

## R2 injectable native composition RED checkpoint

This checkpoint adds only the native-runtime test-first harness and its test-source registration. It does not add production code, production CMake integration, a real loopback harness, R3 work, or an R2 allocation target.

The adjudicated R2 contract is locked as follows:

- the native composition is an internal injectable owner with `noexcept` start, tick, purge and shutdown seams;
- `NativeSessionRuntime` has no startup-budget argument and no completeness bypass;
- only a fake `RuntimeStartupServices` may inject a complete budget, and construction/configuration of the native stack belongs inside the successful `start_transport()` gate;
- the real WP06 budget remains incomplete with deferred mask `0x00f0` and therefore constructs no socket stack;
- an engine update reads the monotonic clock exactly once, then applies lifecycle before timeouts, reliable work, periodic work and nonblocking I/O;
- order is asserted behaviorally, without a production observer, for the four inversions lifecycle/timeout, timeout/REL, REL/periodic and periodic/I/O;
- receive `Closed` or `Error` is globally fatal; send `Closed` or `Error` completes the selected controller output once and then performs global teardown;
- the shared per-tick attempt budget is bounded to `1..256`, alternates its initial I/O direction across ticks, and a direction-local `WouldBlock` does not stop the other direction;
- shutdown and wrong-thread updates do not clock, service or drain the native stack.

Nine named tests now reserve those behavioral gates:

```text
NativeCompositionHeaderAndNoexceptContractExist
BudgetGateConstructsNativeStackOnlyInsideSuccessfulStartTransport
EngineUpdateReadsOneClockThenAppliesLifecycleBeforeTick
FourBehavioralInversionsLockLifecycleTimeoutReliablePeriodicAndIoOrder
SharedBudgetAlternatesAcrossTicksAndWouldBlockStopsOnlyOneDirection
AllowlistAndVersionNegotiationComposeWithoutDurableRejectedState
PermanentReceiveClosedOrErrorPurgesAllAndNeverReopens
PermanentSendClosedOrErrorCompletesOnceThenPurgesGlobally
ShutdownAndWrongThreadNeverClockServiceOrDrain
```

The source uses `__has_include("telemetry/native_session_runtime.h")`. This keeps the test target compilable before production exists while making every R2 gate explicitly RED. Once that header exists, the native-backed branch becomes the active harness and any concrete API drift must be adapted to the contract rather than hidden with a production bypass.

### Reviewer-requested R2 oracle strengthening

The future native-backed branch was rewritten after the first RED review. The eight findings are now represented explicitly, while the header-absent branch remains the active expected RED:

1. `RuntimeCompositionServices` returns the real WP06 subtotal (`0x00f0`) to a real `Runtime` and proves `BudgetFailure`, zero native construction and zero socket open. Its synthetic complete variant constructs `NativeSessionRuntime` only inside `start_transport()`.
2. The same real `Runtime` receives one pending mission-lifecycle marker; the fake counts exactly one monotonic read and one service call, then compares the service context with the post-lifecycle mission generation.
3. Four separate behavioral sub-scenarios observe lifecycle before service, timeout before a due REL retry, REL before a simultaneous heartbeat, and periodic heartbeat before an `N=1` socket opportunity. No production order observer is requested.
4. Native start rejects budgets `0` and `257` without opening a socket; `N=1` produces `R,S,R,S` across four ready ticks; RX and TX `WouldBlock` are exercised symmetrically with remaining budget and progress in the other direction.
5. A nonallowlisted source produces zero bytes and no usage/random mutation; 1.1 creates an accepted slot; 1.0 emits an exact minor-1.0 `WELCOME(UnsupportedVersion, session_id=0)`. Its bounded rejection replay cache is asserted separately from active slots and reliable-window ownership.
6. Receive and send `Closed` and `Error` start from multiple slots plus cache, preproof, REL, heartbeat schedules and queued output. Every case requires one socket close, complete purge, no reopen/retry on future ticks, and exactly one fatal send attempt for the selected output.
7. A real `Runtime` callback from a worker performs zero clock/service/RX/TX calls. Main-thread shutdown asserts the existing `stop collection -> close stores -> invalidate mission -> stop transport -> summary -> release allocations -> release registry` order, idempotence and no pending datagram drain.
8. Native references live entirely below the header guard; `<algorithm>` is explicit; the SFINAE trait reports a partial member API as a focused test failure and requires `noexcept` start/tick/purge/shutdown. A constructor accepting a budget subtotal is explicitly forbidden.

Because the production header is still absent at this checkpoint, this strengthened future branch has not been compiled or claimed GREEN. Its concrete names are an implementation handoff contract; once production lands, the test owner must compile it and adapt only API spelling/shape that preserves these behaviors.

### Second-review precision pass

The next review found that several of the strengthened scenarios were still satisfiable by weaker implementations. The test branch now tightens them as follows:

- the real Runtime service fake appends `C`, lifecycle invalidation appends `L`, and native service appends `T` to one shared trace; the oracle requires exactly `C,L,T` and the exact mission-purge service sequence `1,2,3,4,5`;
- lifecycle replacement decodes the old and new WELCOME packets, requires the old/new peer endpoints to differ, forbids reuse of the old session ID and requires exactly one surviving new slot;
- the REL/periodic scenario advances to `1,020,100 us`, where both the retained SESSION_BEGIN retry and the first idle heartbeat are due but disconnect is not; it requires SESSION_BEGIN first and HEARTBEAT on the following eligible send;
- `establish_ready` snapshots the pre-existing active-session count, waits until the scripted final SESSION_BEGIN ACK is actually consumed, then requires exactly `sessions_before + 1`; it no longer assumes that every fixture starts with zero sessions;
- the periodic/I/O inversion uses the exact boundary `1,030,010 us`: WELCOME proof is applied at `30,010 us`, and the SESSION_BEGIN ACK at `30,020 us` changes readiness without re-anchoring the `1,000,000 us` idle-heartbeat deadline;
- the `WouldBlock` harness preloads a real WELCOME without a send attempt by spending all four setup units on three rejected RX datagrams and the final valid HELLO; the following ticks require TX-`WouldBlock` then RX progress, followed by RX-`WouldBlock` then TX progress;
- every budget assertion now snapshots calls immediately before the tick and checks that tick's RX+TX delta is at most four; no cumulative call-count proxy remains;
- after the wrong-thread callback's zero-work assertion, the next main-thread tick must fault with `MainThreadViolation`, execute exact global teardown `1,4,2,6,8,9`, free the native composition and still perform zero clock/service/I/O work.

The remaining completion observability point was adjudicated with one narrowly scoped internal seam: `telemetry::detail::NativeOutputCompletionPort` is a mandatory non-null reference with only `complete(SessionController&, IoStatus) noexcept`. The production `NativeOutputCompletionForwarder` is required to be nothrow-default-constructible and to forward exactly to `SessionController::complete_output`; no non-portable object-representation or `is_empty` assertion is made for this polymorphic type. `RuntimeCompositionServices` constructs the native runtime with that production port. The counting fake owns a real `NativeOutputCompletionForwarder`, records usage immediately before the call, delegates to `forwarder.complete(controller, status)` exactly once, then records usage immediately afterwards. A fatal send now requires one additional port call with exact `Closed`/`Error`, queued output before, only the selected owner closed afterwards while unrelated slots still exist, and only then complete global purge. Late ticks must preserve the completion count and perform no send or reopen. A direct purge without completion can no longer satisfy this oracle. No general phase observer, optional/null hook, configuration, dynamic allocation or additional callback surface is introduced by the test contract.

Debug RED capture:

```text
cmake --build build --config Debug --target unittests --parallel 4
Result: PASS; test_telemetry_native_runtime_integration_contract.cpp compiled and unittests linked.

build/bin/Debug/unittests.exe --gtest_filter="TelemetryNativeRuntimeIntegrationContract.*"
Result: 9 tests; 0 PASS, 9 expected FAIL.
Common blocking oracle: telemetry/native_session_runtime.h does not exist.
```

The RED is therefore an absent production composition, not a compiler failure or an unrelated regression.

### Post-production R2 GREEN capture

After the native production composition landed, the guarded native branch compiled with the two adjudicated oracle corrections above. Debug verification against the current production implementation is:

```text
cmake --build build --config Debug --target unittests --parallel 4
Result: PASS; native_session_runtime.cpp, runtime.cpp, runtime_adapter.cpp and the R2 test linked.

build/bin/Debug/unittests.exe --gtest_filter="TelemetryNativeRuntimeIntegrationContract.*"
Result: 9 tests; 9 PASS, 0 FAIL (132 ms total).
```

The same build preserved the focused baselines:

```text
R0/R1 NAT9: 9/9 PASS.
Allocation contract: 4/4 PASS.
TelemetryWp06HeartbeatContract: 24/24 PASS.
TelemetryWp06ReliabilityContract: 13/13 PASS.
git diff --check: PASS (line-ending warnings only).
```

At that intermediate checkpoint, this closed the injectable native-composition R2 test gate in Debug only; the final verification below adds Release and independent test-owner evidence. Real IPv4/IPv6 loopback proof and later R3 work remain outside this slice.

### Final independent R2 GREEN verification

The test owner independently inspected the final production slice after implementation. The inspection confirmed:

- `NativeRuntimeStartupServices::calculate_known_budget()` composes WP04 into the real WP06 subtotal;
- the real subtotal remains incomplete with deferred mask `0x00f0`, and `Runtime` rejects it before registry allocation, candidate registration, `start_transport()`, native construction or socket bind;
- the synthetic complete budget exists only in the fake `RuntimeStartupServices`, and native construction occurs inside its successful `start_transport()`;
- each engine update reads the clock once, applies lifecycle, builds the post-lifecycle context and then calls native tick service;
- native service order is housekeeping, timeout, REL, periodic, then the bounded alternating I/O scheduler;
- a real send is followed by exactly one focused output-completion call before fatal global purge; receive/send `Closed` and `Error` close globally and never reopen;
- wrong-thread and shutdown paths cannot clock, service or drain the native transport.

No production or test source was edited during this final verification. Six test hashes were captured before execution and recaptured afterwards; every value was identical:

```text
B51C3C193444D190BFB496301EE7A5E235EA386D454236A9D9BC13D08B94F2AF  test_telemetry_native_runtime_integration_contract.cpp
8C2C2D3C1E9396B813C9DE06126DC2F0D978DB3D3E0D1020731BFB8E66CBF659  test_telemetry_runtime_startup_contract.cpp
82B0ADA00FDBD9EBFD1C11741B4509C14EF93942119E1079105864E9E093804D  test_telemetry_runtime_lifecycle_contract.cpp
3243E1DCCFECC22F458E990B8DC1AF843B6702AFFFCF952FF2434E975904B49B  test_telemetry_session_controller_heartbeat_contract.cpp
84DE2DB14C23D10C64B56E2103E95D5C9283C2388FDFBAC8771566BDE4662EF0  test_telemetry_session_controller_contract.cpp
09CD4E4F85DB92E9F2F91C8FCBE882203A1E41ECE942560F63DD83A97306F5D6  test_telemetry_session_controller_allocations.cpp
```

Final Debug and Release build command:

```text
cmake --build build --config <Debug|Release> --target unittests telemetry_session_controller_allocation_contract_tests --parallel 4
Result: PASS in Debug and Release.
```

Final targeted matrix:

| Suite | Debug | Release |
|---|---:|---:|
| `TelemetryNativeRuntimeIntegrationContract.*` | 9/9 PASS | 9/9 PASS |
| R0/R1 NAT9 filter | 9/9 PASS | 9/9 PASS |
| `TelemetryRuntime*.*` | 47/47 PASS | 47/47 PASS |
| `TelemetryWp06HeartbeatContract.*` | 24/24 PASS | 24/24 PASS |
| `TelemetryWp06ReliabilityContract.*` | 13/13 PASS | 13/13 PASS |
| allocation executable | 4/4 PASS | 4/4 PASS |

Full suites:

```text
Debug:   852/852 PASS from 110 suites; 2 pre-existing disabled tests; 12,693 ms.
Release: 852/852 PASS from 110 suites; 2 pre-existing disabled tests; 4,396 ms.
```

Proportionate deterministic stress used `--gtest_shuffle --gtest_random_seed=20260717`:

```text
Critical R2/runtime/heartbeat/reliability filter:
  Debug   93 tests x 10 iterations = 930/930 PASS.
  Release 93 tests x  5 iterations = 465/465 PASS.
Allocation executable:
  Debug    4 tests x 10 iterations = 40/40 PASS.
  Release  4 tests x  5 iterations = 20/20 PASS.
```

`git diff --check` passed with line-ending warnings only. Full-suite generated `test/test_data` artifacts were removed, and final status contains only the expected shared production slice plus the authorized R2 test registration, test source and native evidence. No anomaly, flaky iteration, hash drift, test relaxation, unexpected bind, build failure or baseline regression was observed.

Preserved Debug baselines:

```text
R0/R1 NAT9: 9/9 PASS.
Allocation contract: 4/4 PASS.
TelemetryWp06HeartbeatContract: 24/24 PASS.
TelemetryWp06ReliabilityContract: 13/13 PASS.
```

The historical RED above is resolved: R2 injectable composition is `GREEN IN DEBUG AND RELEASE`, with independent review and reproduction completed. At this R2-only checkpoint, `P1-AC-005` was `OPEN` and `G1-D` was `PARTIAL` because the incomplete real global budget prevented production bind and real IPv4/IPv6 loopback proof had not yet been added. The later R4 section supersedes that loopback-evidence status without changing the production-budget limitation.

## R3 native allocation test-first checkpoint: immediate GREEN gap

This checkpoint extends only the existing allocation-contract source and executable. It adds no production code, no new test target, no R4 source and no real loopback work. The existing target already links the `code` target containing `runtime.cpp` and `native_session_runtime.cpp`, so no CMake source addition was required.

The allocation fixture deliberately differs from the vector-backed R2 behavioral fixture:

- receive scripts, send-status scripts and captured datagrams use fixed-capacity `std::array` storage;
- all protocol packets are encoded outside an armed allocation window;
- the real `Runtime` owns startup ordering through a fake `RuntimeStartupServices`;
- only the synthetic complete-budget fake constructs `NativeSessionRuntime`, and only inside `start_transport()`;
- the allocation probe is armed only after the outer runtime has reached `RuntimeState::Ready`;
- the adjacent real-budget test preserves the true deferred mask `0x00f0` and proves zero native construction, socket open or bind boundary crossing.

Three named R3 tests cover the authorized slice:

```text
RealDeferredBudgetMaskConstructsNoNativeStackOrSocket
ReadyRuntimeLifecycleAndBackpressureAllocateNothing
PermanentReceiveAndSendFailuresAllocateNothingAndStayClosed
```

The main lifecycle oracle measures zero post-Ready allocations across an idle tick, HELLO/WELCOME, WELCOME ACK/SESSION_BEGIN, deterministic REL retry, SESSION_BEGIN ACK, periodic heartbeat, RX and TX `WouldBlock`, correlated heartbeat response, disconnect timeout, explicit mission purge, shutdown and inert late callbacks. Fatal receive and send loops cover both `Closed` and `Error`; each measured path remains allocation-free, releases owned usage, closes its single socket exactly once and performs no I/O on a late tick.

The first Debug execution did not produce a RED failure. This is recorded as an immediate GREEN gap rather than retroactively manufacturing a failure: the production composition delivered by R2 already satisfies these R3 allocation oracles.

```text
cmake --build build --config Debug --target telemetry_session_controller_allocation_contract_tests --parallel 4
Result: PASS; the existing allocation executable compiled and linked.

build/bin/Debug/telemetry_session_controller_allocation_contract_tests.exe \
  --gtest_filter="TelemetryNativeAllocationContract.*"
Result: 3/3 PASS; immediate GREEN gap.
```

Debug baseline reproduction after the final oracle strengthening:

```text
Allocation executable: 7/7 PASS (4 historical WP06 + 3 native R3).
R2/runtime/heartbeat/reliability filter: 93/93 PASS.
```

The independent review approved the Debug R3 oracle without requesting production, test, CMake or R4 changes. Final verification then reproduced the unchanged oracle in both configurations.

```text
Release build:
  telemetry_session_controller_allocation_contract_tests: PASS.
  unittests: PASS.

Allocation executable:
  Debug:   7/7 PASS (4 historical WP06 + 3 native R3).
  Release: 7/7 PASS (4 historical WP06 + 3 native R3).

Adjacent R2/runtime/heartbeat/reliability filter:
  Debug:   93/93 PASS.
  Release: 93/93 PASS.

Full unittests:
  Debug:   852/852 PASS from 110 suites; 2 pre-existing disabled tests; 13,505 ms.
  Release: 852/852 PASS from 110 suites; 2 pre-existing disabled tests; 4,084 ms.

R3 allocation stress, --gtest_shuffle --gtest_random_seed=20260717:
  Debug:   3 tests x 25 iterations = 75/75 PASS.
  Release: 3 tests x 10 iterations = 30/30 PASS.
```

Six test-source hashes were identical before and after execution:

```text
C7B679B0D1D5C7FE60046C50183B7A530EF69178B67D72E25274B94C31504F51  test_telemetry_session_controller_allocations.cpp
B51C3C193444D190BFB496301EE7A5E235EA386D454236A9D9BC13D08B94F2AF  test_telemetry_native_runtime_integration_contract.cpp
8C2C2D3C1E9396B813C9DE06126DC2F0D978DB3D3E0D1020731BFB8E66CBF659  test_telemetry_runtime_startup_contract.cpp
82B0ADA00FDBD9EBFD1C11741B4509C14EF93942119E1079105864E9E093804D  test_telemetry_runtime_lifecycle_contract.cpp
3243E1DCCFECC22F458E990B8DC1AF843B6702AFFFCF952FF2434E975904B49B  test_telemetry_session_controller_heartbeat_contract.cpp
84DE2DB14C23D10C64B56E2103E95D5C9283C2388FDFBAC8771566BDE4662EF0  test_telemetry_session_controller_contract.cpp
```

Full-suite generated artifacts under `test/test_data` were removed. Final status contains only the authorized allocation test and this evidence update. This closes R3 as `GREEN IN DEBUG AND RELEASE`; it does not change the real incomplete global budget, authorize production binding or claim the R4 real-loopback gate.

## R4 opt-in native loopback test-first checkpoint: immediate GREEN gap

R4 adds one always-compiled source to the standard `unittests` target and exactly two tests:

```text
IPv4NegotiationHeartbeatAndShutdown
IPv6NegotiationHeartbeatAndShutdown
```

Both tests inspect the exact environment variable `FSO_TELEMETRY_RUN_NATIVE_LOOPBACK`. Any value other than `1`, including absence, produces an explicit skip before socket setup. With opt-in active, the IPv4 path is required. The IPv6 path may skip only for the closed preflight set `WSAEAFNOSUPPORT`/`WSAEPROTONOSUPPORT` at `socket`, `WSAEADDRNOTAVAIL` at `bind`, or the corresponding POSIX `EAFNOSUPPORT`/`EPROTONOSUPPORT` and `EADDRNOTAVAIL`. WinSock bootstrap failure, `getsockname` failure and every other socket or bind error are ordinary failures that report both stage and OS code. The source includes `<cerrno>` explicitly for the POSIX classification.

The test-only harness preserves the production gate and port contract:

- a real `Runtime` owns startup and shutdown ordering;
- the fake synthetic-complete `RuntimeStartupServices` constructs `NativeSessionRuntime` only inside `start_transport()`;
- an adjacent real-budget scenario requires deferred mask `0x00f0`, `BudgetFailure`, zero native construction and zero backend open/bind attempt;
- the server delegates through the real `NativeUdpSocketBackend` and receives a valid configured port in `1024..65535` obtained by OS `bind(0)`/`getsockname()`/close;
- a bind race retries the complete probe-and-start transaction at most 16 times, without sleeping;
- the client uses a separate real `NativeUdpSocketBackend` and binds native port zero;
- WinSock, raw probe sockets, native client sockets and server lifetime are RAII-owned;
- one shared pump is bounded by 4096 iterations and two wall-clock seconds and uses only `yield()`.

For each available family, the real datagram exchange proves HELLO 1.1, accepted WELCOME 1.1 with nonzero session ID, WELCOME ACK, SESSION_BEGIN, SESSION_BEGIN ACK, one active session, periodic HEARTBEAT request, correlated HEARTBEAT response, activity refresh beyond the previous exact disconnect boundary, and idempotent shutdown with zero server sockets and one server close.

The first opt-in Debug execution passed immediately. As with R3, this is recorded honestly as a GREEN gap rather than manufacturing a RED failure: the existing R2 native composition already supports the real loopback contract.

```text
cmake --build build --config Debug --target unittests --parallel 4
Result: PASS; the new source compiled and unittests linked.

FSO_TELEMETRY_RUN_NATIVE_LOOPBACK unset:
  TelemetryNativeRuntimeLoopbackContract.*: 0 PASS, 2 explicit SKIP.

FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1:
  IPv4: PASS (6 ms).
  IPv6: PASS (4 ms).
  Total: 2/2 PASS (11 ms).

Preserved Debug baselines:
  R2/runtime/heartbeat/reliability: 93/93 PASS.
  Allocation executable: 7/7 PASS.

R4 source SHA-256:
  E1DDA853D9C54D7C4FED61690F4070CCC7E05919DDDE4F4857443AAC1C37C11A
```

Independent review approved the Debug R4 oracle after narrowing IPv6 skips to the closed stage/error set above. Final verification reproduced the unchanged oracle in both configurations:

```text
Release build:
  unittests: PASS; the loopback source compiled and linked.

FSO_TELEMETRY_RUN_NATIVE_LOOPBACK unset:
  Debug:   0 PASS, 2 explicit SKIP.
  Release: 0 PASS, 2 explicit SKIP.

FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1:
  Debug:   IPv4 and IPv6, 2/2 PASS (11 ms).
  Release: IPv4 and IPv6, 2/2 PASS (9 ms).

Bounded opt-in collision/flaky repetition,
--gtest_shuffle --gtest_random_seed=20260717:
  Debug:   2 tests x 10 iterations = 20/20 PASS.
  Release: 2 tests x  5 iterations = 10/10 PASS.

Adjacent R2/runtime/heartbeat/reliability filter:
  Debug:   93/93 PASS.
  Release: 93/93 PASS.

Allocation executable:
  Debug:   7/7 PASS.
  Release: 7/7 PASS.

Full unittests with loopback opt-in unset:
  Debug:   854 tests from 111 suites; 852 PASS, 2 expected loopback SKIP;
           2 pre-existing disabled tests; 13,079 ms.
  Release: 854 tests from 111 suites; 852 PASS, 2 expected loopback SKIP;
           2 pre-existing disabled tests; 3,999 ms.
```

Seven test-source hashes were identical before and after execution:

```text
E1DDA853D9C54D7C4FED61690F4070CCC7E05919DDDE4F4857443AAC1C37C11A  test_telemetry_native_runtime_loopback_contract.cpp
B51C3C193444D190BFB496301EE7A5E235EA386D454236A9D9BC13D08B94F2AF  test_telemetry_native_runtime_integration_contract.cpp
C7B679B0D1D5C7FE60046C50183B7A530EF69178B67D72E25274B94C31504F51  test_telemetry_session_controller_allocations.cpp
8C2C2D3C1E9396B813C9DE06126DC2F0D978DB3D3E0D1020731BFB8E66CBF659  test_telemetry_runtime_startup_contract.cpp
82B0ADA00FDBD9EBFD1C11741B4509C14EF93942119E1079105864E9E093804D  test_telemetry_runtime_lifecycle_contract.cpp
3243E1DCCFECC22F458E990B8DC1AF843B6702AFFFCF952FF2434E975904B49B  test_telemetry_session_controller_heartbeat_contract.cpp
84DE2DB14C23D10C64B56E2103E95D5C9283C2388FDFBAC8771566BDE4662EF0  test_telemetry_session_controller_contract.cpp
```

Full-suite generated artifacts under `test/test_data` were removed. Final status contains only the authorized source registration, loopback source and evidence update. R4 is `GREEN IN DEBUG AND RELEASE`. Combined with the already-green R2 allowlist and exact 1.0 rejection oracles, R4 verifies the complete `P1-AC-005` behavior only for the native composition reached through the synthetic-complete test gate. It does not close global `P1-AC-005` or `G1-D`: the real global startup budget remains incomplete with mask `0x00f0`, so the production path remains correctly fail-closed before native construction or bind. No production source, startup budget calculation, port validation or runtime bypass was changed.

## R2 requirement delta

| ID | R2 evidence | Honest status after this run |
|---|---|---|
| `P1-REQ-017` | Runtime mission/shutdown and fatal-transport convergence pass with injected sockets and exact teardown order in Debug and Release. | `VERIFIED FOR R2` — independently reviewed and reproduced. |
| `P1-REQ-018` | Native timeout-before-REL and REL-before-periodic composition pass in Debug and Release. | `VERIFIED FOR R2` — independently reviewed and reproduced. |
| `P1-REQ-019` | Exact periodic deadline and periodic-before-I/O composition pass in Debug and Release. | `VERIFIED FOR R2` — independently reviewed and reproduced. |
| `P1-AC-010` | Controller heartbeat 24/24 and native ordered scenarios pass in Debug and Release, including deterministic stress. | `VERIFIED FOR R2` — independent review and reproduction completed. |
| `P1-AC-005` | Fake-backend allowlist and exact 1.0 rejection remain green; opt-in real IPv4/IPv6 sockets negotiate 1.1 through Runtime, session and heartbeat in Debug and Release. | `PARTIAL` — the native/synthetic-gate network slice verifies every behavioral clause, but the real `0x00f0` budget still blocks the production bind path. |
| `G1-D` | Mission/menu/fatal/shutdown convergence remains green; real IPv4/IPv6 session, heartbeat and socket-zero idempotent shutdown pass in Debug and Release. | `PARTIAL` — lifecycle behavior is verified through the injected/synthetic-gate composition, but the incomplete real budget still prevents production native convergence. |

## Historical R0/R1 requirement mapping

The table below records the earlier checkpoint and is retained as historical evidence; statements that native composition was unproven are superseded by the R2 delta above.

| ID | Evidence in this checkpoint | Honest status |
|---|---|---|
| `P1-REQ-009` | Initial process reservation remains unpublished; a controller draw colliding with it is rejected, purge retains all used IDs, and the next session receives a fresh ID. | `PARTIAL` — the no-reuse slice is directly covered; complete identity/profile and entropy-failure closure remains distributed across WP03 evidence. |
| `P1-REQ-014` | The real incomplete subtotal faults before allocation/socket work; only a synthetic complete budget reaches the ordered allocation boundaries. The allocation executable requires zero post-`Ready` allocation for the new phases and purge. | `PARTIAL` — baseline/candidate/delta and remaining deferred WP07–WP09 storage are intentionally not priced here. |
| `P1-REQ-017` | `purge_all(MissionDiscontinuity)` releases session-scoped slots, REL state, cache, preproof, heartbeat state, output, session/target limiter ownership and cursors while preserving process-scoped IDs and pre-session abuse history. | `PARTIAL` — the R1 controller purge slice is green; engine mission/menu/shutdown and socket convergence remain outside R0/R1. |
| `P1-REQ-018` | Exact timeout boundary is ordered before REL and periodic work; due REL is ordered before heartbeat and remains transactional until completion. Existing session/REL baselines remain green. | `PARTIAL` — the R1 controller ordering slice is green; native tick composition remains unproven. |
| `P1-REQ-019` | Periodic heartbeat after REL completion, hitch coalescing, probe/filter purge and cursor restart are green; the existing heartbeat boundary/filter/overflow suite remains green. | `PARTIAL` — isolated/controller behavior is green, native tick composition remains open. |
