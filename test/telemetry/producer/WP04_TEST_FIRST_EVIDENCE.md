# Phase 1 WP04 UDP transport and scheduler test-first evidence

Status: **the isolated WP04 transport, datagram-scheduler, startup-budget and
runtime fail-closed slices are GREEN after preserved test-first RED cycles**.
The complete Phase 1 producer is not claimed READY: the startup-budget report
still carries deferred categories, so the production runtime deliberately fails
before allocation, registration or transport startup. `G1-C` therefore remains
OPEN until the later work packages complete the budget and runtime transaction.

## Contract slice and phase boundary

This tranche implements the bounded main-thread transport foundation required by
`P1-WP-04`:

- producer-owned, nonblocking UDP sockets with IPv4, IPv6 and safe dual-stack
  operation;
- canonical `EndpointKey` conversion, including IPv4-mapped IPv6 receive
  addresses;
- verified bind address and effective port, fail-closed socket-option handling,
  and reverse-order rollback on every open failure;
- one preallocated 1,200-byte receive buffer and one preallocated 1,200-byte
  transmit buffer, with one copy into transport-owned storage before native send;
- a pure six-priority datagram selector over externally owned candidates, with
  round-robin fairness inside a priority;
- a common per-tick budget in `[1, 256]`, dynamic ingress/egress readiness, and
  direction latching after `WouldBlock`;
- explicit receive disposition separating a delivered datagram from a rejected
  datagram without converting policy rejection into a terminal socket error;
- exactly 2,400 newly priced application-buffer bytes in the startup-budget
  subtotal, while the fixed native socket-record registry remains bounded
  metadata rather than application payload storage.

The tranche intentionally does **not** add session state, ingress allowlist and
anti-amplification policy, rate quotas, per-client queues, reliable retention,
replication/baselines/deltas, video, serialization workers, or background
threads. Those belong to later Phase 1 work packages. No Phase 0 wire schema,
message, vector or protocol asset is changed.

The internal native backend remains non-copyable and non-movable so an OS handle
cannot be duplicated through ordinary C++ value transfer. WinSock lifecycle is
still owned by the engine; production telemetry code contains no `WSAStartup` or
`WSACleanup`. Tests use only a balanced test-local socket-API scope.

## Test sources and build integration

The WP04 contract is exercised by:

- `test/src/telemetry/producer/test_telemetry_transport_contract.cpp`;
- `test/src/telemetry/producer/test_telemetry_datagram_scheduler_contract.cpp`;
- `test/src/telemetry/producer/test_telemetry_wp04_startup_budget_contract.cpp`;
- the strengthened incomplete-budget gate in
  `test/src/telemetry/producer/test_telemetry_runtime_startup_contract.cpp`.

They are registered in `test/src/source_groups.cmake`. The production sources
are registered in `code/source_groups.cmake`.

The exact final targeted filter is:

```text
TelemetryTransportContract.*
TelemetryDatagramTickSchedulerContract.*
TelemetryDatagramPrioritySelectorContract.*
TelemetryRuntimeStartupContract.*
TelemetryWp03KnownBudgetContract.*
TelemetryWp04StartupBudgetContract.*
```

It contains 56 tests across six suites.

## Preserved RED inventory

Each row records an observed failure before the corresponding production change.
The initial queue-owning scheduler proposal was rejected before production work
because queue ownership belongs to WP06/WP08; the accepted RED uses a pure
selector over external candidates.

| RED | Observed failure | Contract correction proved by the later GREEN |
|---|---|---|
| `WP04-RED-001` | the strengthened runtime test `IncompleteOrDeferredBudgetFailsBeforeAllocationRegistrationAndTransport` failed `0/1`: the old runtime allocated, registered, called transport and ended as `TransportUnavailable` | any incomplete/deferred budget fails as `BudgetFailure` before allocation, registration and socket work |
| `WP04-RED-002` | the first Debug compile failed with `C1083` for missing `telemetry/transport.h` and `telemetry/datagram_scheduler.h` | the new internal transport and scheduler contracts exist and are build-integrated |
| `WP04-RED-003` | `SendCopiesExactlyOneDatagramIntoPreallocatedTransportStorage` failed `0/1`; the backend observed the caller's pointer | transmit uses the dedicated preallocated transport buffer and copies exactly one datagram before backend send |
| `WP04-RED-004` | the serialized budget compile failed with missing `transport_buffer_bytes`, WP04 constants and budget apply/calculate functions | the checked WP04 subtotal prices exactly two 1,200-byte application buffers |
| `WP04-RED-005` | fake-backend `SingleDualStackSocketRoutesCanonicalIpv4AndIpv6Endpoints` failed `0/1`; canonical IPv4 could not use an IPv6 wildcard socket with `ipv6_only=false` | dual-stack routing maps canonical IPv4 endpoints to mapped IPv6 only at the native boundary |
| `WP04-RED-006` | native `NativeDualStackWildcardExchangesIpv4AndIpv6WithCanonicalEndpoints` failed `0/1` at open with `SocketOptionFailed`; an independent Python probe showed that IPv6-only false and bind `::` were supported | Windows socket-option readback accepts the actual returned byte length after zero-initializing the value buffer |
| `WP04-RED-007` | `ExplicitDualStackWildcardAcceptsANarrowedIpv4OnlyAllowlist` failed `0/1` as `InvalidConfiguration` | a narrowed IPv4-only allowlist is valid for an explicitly dual-stack wildcard bind |
| `WP04-RED-008` | `EffectivePortMismatchOrZeroFailsAddressVerificationAndRollsBack` failed `0/1`; requested port 42042 with effective port 42043 was accepted and left one socket open | bind verification checks both canonical address and the nonzero effective port, then rolls back on mismatch |
| `WP04-RED-009` | `SpecificNonLoopbackBindAcceptsANonemptyCatchAllAllowlist` failed `0/1` | a specific non-loopback bind requires a nonempty allowlist; the `/0` prohibition applies to wildcard exposure |
| `WP04-RED-010` | `UnverifiableCurrentSocketRollsBackCurrentThenPriorInReverseOrder` failed `0/1`; close order was `{41,42}` instead of `{42,41}` | the current returned handle is retained as an uncommitted acquisition and closed before prior committed handles |
| `WP04-RED-011` | `CompleteRelatchesReadinessBeforeAttemptingTheSameDirectionAgain` failed `0/1`, performing 64 ingress operations instead of 1 | readiness is recomputed after every completed operation |
| `WP04-RED-012` | `CompleteIngressCanMakeEgressReadyForTheRemainingSharedBudget` failed `0/1`, performing 64 ingress and 0 egress operations instead of 1 and 1 | ingress and egress share one budget and each completion can make the other direction ready |
| `WP04-RED-013` | four compile-time assertions failed (`C2338`) because the native backend was copy/move constructible or assignable | native socket ownership explicitly deletes copy and move construction/assignment |
| `WP04-RED-014` | the required Windows source scan for `SIO_UDP_CONNRESET`/`WSAIoctl` had no hit | Windows UDP open disables connection-reset delivery and fails closed if `WSAIoctl` fails; the needed SDK declaration comes from `mswsock.h` |
| `WP04-RED-015` | the receive-disposition compile failed with missing enum/type/field errors | `None=0`, `Datagram=1`, and `Rejected=2` are explicit while `IoStatus` remains the closed four-value I/O status |

An earlier native Windows IPv6-option test also exposed non-normalized option
readback. After correction, the native loopback and dual-stack tests passed all
repeat runs below without a flake.

## Final build and test evidence

All commands were executed from the repository root. Debug and Release were
built independently:

```powershell
cmake --build build --config Debug --target unittests -- /m:4
cmake --build build --config Release --target unittests -- /m:4
```

Both commands exited `0`.

The exact targeted command was run once in each configuration:

```powershell
& .\build\bin\Debug\unittests.exe --gtest_color=no --gtest_filter=TelemetryTransportContract.*:TelemetryDatagramTickSchedulerContract.*:TelemetryDatagramPrioritySelectorContract.*:TelemetryRuntimeStartupContract.*:TelemetryWp03KnownBudgetContract.*:TelemetryWp04StartupBudgetContract.*
& .\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryTransportContract.*:TelemetryDatagramTickSchedulerContract.*:TelemetryDatagramPrioritySelectorContract.*:TelemetryRuntimeStartupContract.*:TelemetryWp03KnownBudgetContract.*:TelemetryWp04StartupBudgetContract.*
```

| Configuration | Suites | Tests | Result | Elapsed |
|---|---:|---:|---|---:|
| Debug | 6 | 56 | 56 passed, 0 failed | 17 ms |
| Release | 6 | 56 | 56 passed, 0 failed | 9 ms |

Native IPv4/IPv6 and dual-stack loopback were repeated independently in both
configurations:

```powershell
& .\build\bin\Debug\unittests.exe --gtest_color=no --gtest_brief=1 --gtest_repeat=20 --gtest_break_on_failure --gtest_filter=TelemetryTransportContract.NativeBackendSendsAndReceivesDirectUdpOnIpv4AndIpv6Loopback:TelemetryTransportContract.NativeDualStackWildcardExchangesIpv4AndIpv6WithCanonicalEndpoints
& .\build\bin\Release\unittests.exe --gtest_color=no --gtest_brief=1 --gtest_repeat=20 --gtest_break_on_failure --gtest_filter=TelemetryTransportContract.NativeBackendSendsAndReceivesDirectUdpOnIpv4AndIpv6Loopback:TelemetryTransportContract.NativeDualStackWildcardExchangesIpv4AndIpv6WithCanonicalEndpoints
```

Each configuration completed 20 iterations times 2 tests: `40/40` executions
passed with no failure or flake. The first test performs direct IPv4 and IPv6
exchanges; the second performs IPv4-through-dual-stack canonicalization and an
IPv6 response through the same wildcard socket.

The complete telemetry unit selection was then run in both configurations:

```powershell
& .\build\bin\Debug\unittests.exe --gtest_color=no --gtest_filter=Telemetry*
& .\build\bin\Release\unittests.exe --gtest_color=no --gtest_brief=1 --gtest_filter=Telemetry*
```

| Configuration | Suites | Tests | Result | Elapsed | Disabled |
|---|---:|---:|---|---:|---:|
| Debug | 50 | 499 | 499 passed, 0 failed | 6,410 ms | 2 |
| Release | 50 | 499 | 499 passed, 0 failed | 502 ms | 2 |

The two disabled tests are pre-existing opt-in stress/depth tests. No WP04 test
was disabled and no failure was hidden.

Additional focused reruns passed:

- invalid receive disposition: `1/1`;
- native dual-stack exchange: `1/1`;
- fake dual-stack/allowlist/effective-port/rollback selection: `6/6`;
- dynamic scheduler readiness and `WouldBlock` selection: `3/3`.

## Phase 0 compatibility and specification validation

All protocol and specification gates were rerun after production and test changes:

| Check | Result |
|---|---|
| `verify_fstl_1_0_freeze.py --check --repo .` | exit `0`; 438 files; tree SHA-256 `9baac6a20db33bcf350066ed533c5581b7117410899d7bc4a6dc24406e47856d` |
| `verify_fstl_1_1_amendment.py --check --repo .` | exit `0`; 15 snapshots, 6 messages, 3 negotiations, 2 internal oracles; v1.0 hash unchanged |
| `verify_schema_vectors.py --check` | exit `0`; 20 messages, 28 records, 2,479 bytes; layout SHA-256 `d5e7ae20571bc0e08f1d123f7529fd6430466872ad0dd5cb22ea37b24128e0aa`; encoded SHA-256 `ee45ad75728442145aa2689211f237868f095a39a2735b89ae5868c3dbe95438` |
| `verify_telemetry_assets.py --repo . --require-complete` | exit `0`; 182 fixtures: 54 valid, 128 invalid; 20/20 messages and 28/28 records; 126 invalid categories and 4 seeds |
| `inspect_phase_contract.ps1` on Phase 0 | exit `0`; 8 documents, 5,300 lines, 27 requirements, 24 acceptance criteria, 12 work packages, 6 gates |
| `inspect_phase_contract.ps1` on Phase 1 | exit `0`; 8 documents, 1,912 lines, 38 requirements, 20 acceptance criteria, 11 work packages, 7 gates |
| `validate_phase_specs.ps1 -PhaseDirectory ...` on Phase 0 and Phase 1 | both exit `0`; structure, UTF-8, headings, fences, links, anchors, placeholders and traceability passed |

The first validation attempt omitted mandatory `-PhaseDirectory` and exited `1`
at argument binding. Both corrected phase-directory invocations passed; the
initial invocation was not a product or specification failure.

## Static boundary evidence

Final source scans establish the following:

- no `WSAStartup`, `WSACleanup`, or telemetry-owned socket-API initialization in
  `code/telemetry`;
- Windows open contains `WSAIoctl(SIO_UDP_CONNRESET, FALSE)` semantics and closes
  the socket with `SocketOptionFailed` on failure;
- no `psnet_send`, `multi_io_send`, PSNET/multiplayer transport, video target, or
  related reuse in the new transport/scheduler sources;
- no `std::vector`, `std::deque`, `std::queue`, list/map/unordered container,
  mutex, worker thread, SPSC queue, `new`, `malloc` family, `make_*`, `push_back`,
  `emplace`, or `reserve` in the new transport/scheduler sources;
- the native registry is fixed-size metadata, and the test contract includes
  `static_assert(sizeof(NativeUdpSocketBackend) <= 512)`;
- `test/test_data` is unchanged.

`git diff --check`, a dedicated trailing-whitespace scan of this evidence file,
and strict UTF-8 decoding all pass after the evidence content was finalized.

## Requirement, acceptance and gate disposition

| ID | Evidence state after WP04 |
|---|---|
| `P1-REQ-010` | WP04 transport slice GREEN for dedicated sockets, canonical IPv4/IPv6, native loopback, dual-stack policy and rollback. Complete runtime bind is intentionally blocked by the incomplete startup budget. |
| `P1-REQ-011` | WP04 scheduler slice GREEN for six priorities, owner round-robin, common budget, dynamic readiness and `WouldBlock`; later session/client queue integration remains open. |
| `P1-REQ-012` | GREEN for the 1,200-byte application datagram ceiling, two fixed buffers and preserved Phase 0 wire assets. Reliable/reassembly ownership remains in its scheduled work packages. |
| `P1-REQ-013` | OPEN for WP06 ingress validation, allowlist enforcement, anti-amplification and rate limiting. WP04 proves only bind-exposure configuration rules and non-terminal rejected-datagram plumbing. |
| `P1-REQ-014` | PARTIAL: transport buffers add exactly 2,400 checked bytes; the report remains incomplete with deferred mask `0x00fb`, so no complete Phase 1 startup budget is claimed. |
| `P1-AC-005` | PARTIAL/OPEN: dual-stack exposure and narrowed allowlist configuration are tested, but negotiation and live ingress allowlist behavior are not implemented here. |
| `P1-AC-012` | PARTIAL: bounded transport and scheduler behavior is proved; reliable-window and delta resource semantics remain WP06/WP08 work. |
| `G1-C` | OPEN: the isolated transport transaction is GREEN, but the complete startup budget, full preallocation-before-bind runtime transaction, live ingress allowlist, anti-amplification, rate limits and quotas are not yet available. |

No specification checkbox or tracker entry is closed by this evidence alone.
The implementation reviewer classified two native-backend observations as P3
and nonblocking: internal staging is visible only to an artificial reentrant
backend, and `close_socket` could be hardened in the future to no-op for an
untracked handle instead of closing it as it does today. Neither is a P0/P1/P2
contract failure, and neither weakens the independently observed reverse-order
rollback, handle invalidation, or native loopback results.

## Verdict

The bounded WP04 component is independently GREEN in Debug and Release, including
native IPv4, native IPv6, dual-stack canonicalization, rollback, buffer ownership,
scheduler dynamics, fail-closed runtime gating, protocol freeze, and static
phase-boundary scans. Phase 1 as a whole remains intentionally incomplete, and
`G1-C` remains OPEN rather than being silently redefined or prematurely closed.
