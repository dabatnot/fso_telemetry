# Phase 1 WP03 deferred-startup runtime test-first evidence

Status: **pure runtime sub-tranche GREEN after preserved and independently
reviewed RED; the engine integration seam and WP04 transport remain open**.

This is sub-tranche B of the final WP03 startup work. It connects the already
GREEN strict configuration, producer identity, two-phase session identity and
known-budget subtotal contracts into one first-`EngineUpdate` transaction. It
does not implement or claim a UDP transport, a `Ready` runtime, or any later
Phase 1 lifecycle behavior.

## Frozen internal runtime boundary

The test source is
`test/src/telemetry/producer/test_telemetry_runtime_startup_contract.cpp` and is
registered only in the ordinary `unittests` source group. The test requires a
new producer-internal `telemetry/runtime.h`; the public
`telemetry/telemetry.h` surface must remain exactly
`void telemetry::initialize() noexcept`.

The internal boundary required by the oracle contains:

- startup states `Cold`, `Starting`, `Disabled` and `Faulted`;
- closed terminal reasons `None`, `ConfigAbsent`, `ConfigInvalid`,
  `ConfigDisabled`, `MainThreadNotCaptured`, `MainThreadViolation`, `ProducerIdentityFailure`,
  `SessionCandidateFailure`, `BudgetFailure`, `AllocationFailure`,
  `SessionRegistrationFailure` and `TransportUnavailable`;
- closed configuration observations `Absent`, `Invalid`, `Disabled` and
  `Enabled`, with the validated `max_clients` copied into the runtime result;
- a `RuntimeStartupServices` adapter boundary for main-thread capture/check,
  configuration, producer identity, one session-candidate draw, known-budget
  calculation, registry allocation/registration/release, provisional transport
  startup and a closed diagnostic reason;
- a `Runtime` whose `capture_main_thread()` and `on_engine_update()` are
  `noexcept`, and whose internal state, terminal reason, socket count,
  published session ID and diagnostic count are observable without mutation.

The pure services boundary exposes no socket primitive and is intentionally
link-time injectable. The production
engine adapter will own real configuration/identity/thread facilities; isolated
WP02 executables will link a disabled adapter while compiling the same pure
runtime class. No registration-only branch in `telemetry.cpp` and no parallel
boolean/stub runtime are permitted.

## Exact startup and rollback contract

`capture_main_thread()` is idempotent and performs no startup work. The first
`on_engine_update()` must perform, in order:

```text
main-thread check
-> configuration
-> producer identity
-> draw exactly one nonzero session candidate
-> calculate the WP03 known budget subtotal
-> allocate the exact session registry storage
-> register the same candidate without another RNG call
-> call the provisional transport adapter once
```

The known subtotal is accepted when `error=None` even though
`is_complete=false`; this is not evidence that the deferred WP04/WP06/WP08/WP09
storage categories are priced.

Because WP04 does not yet exist, the provisional transport adapter returns
`TransportUnavailable`. It is called exactly once. The runtime is observed as
still `Starting`, with neither a published session ID nor a socket, at that
boundary; it then rolls back the registry, emits one closed diagnostic, and
becomes terminal `Faulted`.
Repeated updates perform no work and cannot retry startup.

All earlier failures stop at the exact owning step. An update before the
idempotent capture faults as `MainThreadNotCaptured` without even calling the
thread-check service. A captured but wrong thread stops before configuration.
The thread check observes `Cold`; config and every subsequently reached startup
service observe `Starting`. Absent/invalid/disabled configuration becomes terminal
`Disabled` before identity. Producer, candidate and budget failure stop before
allocation. Allocation failure invokes idempotent release to clean possible
partial storage. Every non-`Registered` registry status invokes release and
stops before transport. Fault paths emit exactly one diagnostic; absent or
explicitly disabled configuration permits zero or one, while invalid
configuration requires one. All terminal paths retain zero sockets and no
published session ID.

## Test-ID inventory

| Test ID | GTest oracle | Contract evidence |
|---|---|---|
| `WP03-RT-001` | main-thread capture is idempotent; runtime stays `Cold` and performs no startup before first update | deferred-startup slice of `P1-REQ-015` |
| `WP03-RT-002` | update without capture faults before the thread-check service/config, emits the exact closed reason once and never retries | main-thread/fail-closed slice of `P1-REQ-016` |
| `WP03-RT-003` | captured wrong-thread faults before configuration; thread check observes `Cold`; exact diagnostic is unique | main-thread/fail-closed slice of `P1-REQ-016` |
| `WP03-RT-004` | absent, invalid and disabled configuration become terminal `Disabled` before identity/allocation/transport, with diagnostic cardinality bounded at one | `P1-REQ-006`–`009`, partial `P1-AC-004` |
| `WP03-RT-005` | producer identity error or zero ID stops before candidate draw | `P1-REQ-009`, partial `P1-AC-004`/`011` |
| `WP03-RT-006` | entropy failure or zero candidate consumes exactly one candidate call and stops before budget | `P1-REQ-009`, partial `P1-AC-011` |
| `WP03-RT-007` | every closed budget error stops before allocation and transport, using validated `max_clients` | `P1-REQ-014`, partial `P1-AC-004`/`011` |
| `WP03-RT-008` | allocation failure releases possible partial registry storage, starts no transport and never retries | `P1-REQ-014`, partial `P1-AC-004`/`011` |
| `WP03-RT-009` | `StorageUnavailable`, `InvalidCandidate`, `Duplicate` and `Capacity` registration failures release storage; the same candidate is observed and transport remains untouched | `P1-REQ-009`, `P1-REQ-014`, partial `P1-AC-011` |
| `WP03-RT-010` | incomplete successful subtotal is accepted; exact candidate is registered after one draw; transport boundary is called once while state remains `Starting` and ID/socket counts remain zero, then registry rollback and terminal fault occur | startup-order slice of `P1-REQ-009`/`014`, partial `P1-AC-004`/`011` |

## Expected RED build and raw failure

The new test-only oracle and its source-group registration were built from the
repository root with serialized MSBuild:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /nr:false
```

Exit code: `1`.

```text
  code.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\code.lib
  gtest.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest.lib
  test_telemetry_runtime_startup_contract.cpp
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_runtime_startup_contract.cpp(1,10): fatal error C1083: Impossible d'ouvrir le fichier include : 'telemetry/runtime.h' : No such file or directory [D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\test\src\unittests.vcxproj]
```

This is the intended first production dependency. The production `code`
library and GTest library built before the test translation unit reached the
missing header. No production file was changed for this RED.

Independent test review then required and approved three strengthenings before
production handoff: a distinct no-capture terminal reason with no thread-check
service call, observation of `Cold` at the check and `Starting` through every
reached startup service, and exact one-element diagnostic vectors for every
fault. The final ten-test oracle above was rebuilt after those changes and
reproduced the same single C1083 missing-header RED with exit code `1`.

## Pure-runtime GREEN verification

Production delivered only `code/telemetry/runtime.{h,cpp}` and their production
source-group registration. The serialized Release unit-test target rebuilt and
linked with exit code `0`:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /nr:false
```

The exact runtime selection then passed independently:

```text
[==========] Running 10 tests from 1 test suite.
[----------] 10 tests from TelemetryRuntimeStartupContract (0 ms total)
[==========] 10 tests from 1 test suite ran. (1 ms total)
[  PASSED  ] 10 tests.
```

A combined selection containing all 394 immutable protocol tests, all normal
configuration/profile/identity/native/OS entropy/known-budget tests, the public
initialize smoke and the ten runtime tests passed `463/463` in 564 ms. The two
opt-in truncated-container bombs passed separately with
`--gtest_also_run_disabled_tests` (`2/2`).

The complete current Release binary passed:

```text
[==========] Running 690 tests from 97 test suites.
[==========] 690 tests from 97 test suites ran. (3533 ms total)
[  PASSED  ] 690 tests.
YOU HAVE 2 DISABLED TESTS
```

Its disposable settings, preset and pilot fixtures were removed only after
their resolved paths were checked under `test/test_data`; that subtree was
clean afterward.

GCC 11.4 compiled both the production runtime and final ten-test oracle with
`-std=c++17 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`; both commands
returned exit code `0`.

### Preserved WP02 isolation and fast path

The existing instrumented initialization, allocation-failure and disabled
benchmark targets rebuilt together with exit code `0`. The registration oracle
passed `1/1`; calibration still observed exactly five callback-registration
allocations, and each injected failure index `0..4` reported
`contract_check=PASS`.

Fresh baseline and disabled benchmarks each measured 100,000 callbacks with
zero tracked C++ allocations and passing timing checks. Disabled mean was
`0.000074152 ms` and p99 was `0.000100000 ms`. These targets still exercise the
pre-runtime WP02 callback implementation at this checkpoint; compiling them
against the same pure runtime and a link-time disabled adapter is intentionally
the next integration RED, not a result inferred from these regressions.

## Requirement and gate state after pure GREEN

| Contract or gate | Evidence state |
|---|---|
| `P1-REQ-006`–`009` | GREEN pure runtime proof connects the already-GREEN config/identity contracts through every terminal startup path |
| `P1-REQ-014` | GREEN for known-subtotal/allocation/registration ordering and rollback; complete Phase 1 budget remains explicitly open |
| `P1-REQ-015` | partial only: pure deferred-startup semantics are specified; the real event callback and shared runtime instance still need an integration RED/GREEN |
| `P1-REQ-016` | GREEN at the pure runtime boundary for capture-required, wrong-thread and state-order behavior; production thread adapter remains open |
| `P1-AC-004` | partial GREEN: pure runtime proves zero socket/session publication and bounded exact diagnostics; real config adapter/event path remains open |
| `P1-AC-011` | partial GREEN rollback/no-partial-publication proof; fuzz and broader allocation instrumentation remain open |
| `G1-C` | open: integration adapter, WP04 transport and complete quotas are absent |

## Explicit handoff

- implement only the pure runtime header/source and production source-group
  registration needed by this oracle;
- preserve the exact order, terminal no-retry behavior and rollback ownership;
- keep `TransportUnavailable` as the provisional WP03 terminal outcome and do
  not add sockets or a false `Ready` state;
- after the pure runtime is GREEN, add a separate integration RED proving that
  the registered `EngineUpdate` callback reaches this same runtime instance via
  the link-time service factory, while the three isolated WP02 targets compile
  the same runtime with a disabled adapter;
- verify at that integration boundary that the WP03 disabled adapter contains
  no socket-open primitive; the pure fake alone does not claim a socket syscall
  measurement;
- retain full transport, complete budgets, engine lifecycle, metrics, fuzz,
  benchmarks and soak as open gates owned by their later work packages.
