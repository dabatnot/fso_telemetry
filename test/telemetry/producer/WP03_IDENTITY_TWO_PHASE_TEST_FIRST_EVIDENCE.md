# Phase 1 WP03 two-phase session identity test-first evidence

Status: **two-phase identity tranche GREEN after preserved initial and review
REDs; runtime sequencing and injected allocation failure remain sub-tranche B**.

This is sub-tranche A of the final WP03 startup work. Sub-tranche B, the runtime
state machine and first-`EngineUpdate` transaction, is intentionally not
registered until this identity contract is GREEN.

## Ordering defect resolved by this contract

The existing `SessionIdAllocator(RandomSource&)` constructor allocates its
131,072-word hash table immediately. That is incompatible with the normative
startup order: the runtime must draw a nonzero process candidate before budget
calculation, but must not allocate the table priced by that budget until after
the calculation succeeds.

The required two-phase order is therefore:

```text
draw one nonzero candidate without a registry
-> calculate the WP03 known budget subtotal
-> allocate the 131,072 * uint64_t registry storage
-> register the same candidate without another RNG call
-> attempt transport startup
```

The candidate is not described as allocated or published before registration.
The successful known subtotal remains intentionally incomplete; its
`is_complete=false` value is not an error.

## Frozen internal identity API

All names remain producer-internal in `telemetry/identity.h`; the public
`telemetry/telemetry.h` surface is unchanged.

- `draw_session_id_candidate(RandomSource&) noexcept` returns a
  `SessionIdCandidateResult` with closed status
  `SessionIdCandidateStatus::{Ready, EntropyFailure}`. It has no registry
  parameter, consumes exactly one entropy word, and rejects RNG failure or zero
  without retry.
- `SessionIdRegistry` begins with no allocated storage. `allocate_storage()` is
  fail-closed and idempotent: calling it while ready must not clear entries.
- `SessionIdRegistry::StorageSlotCount` and `StorageBytes` are exactly 131,072
  and 1,048,576. They equal the authoritative
  `SessionIdRegistryStorageSlotCount`/`SessionIdRegistryStorageBytes` values
  consumed by the startup-budget subtotal; the table and budget must not carry
  independent literals.
- `SessionIdRegistryStorage` is the named authoritative storage type and is
  exactly `std::array<std::uint64_t, 131'072>`. The priced byte constant equals
  `sizeof(SessionIdRegistryStorage)`, so padding or a future representation
  change cannot silently diverge from accounting.
- `release_storage()` is idempotent, resets the used count, and leaves later
  registration unavailable until a new successful allocation.
- `register_candidate(uint64_t) noexcept` has no RNG dependency and returns the
  closed statuses `Registered`, `StorageUnavailable`, `InvalidCandidate`,
  `Duplicate`, or `Capacity`.
- `SessionIdAllocator(RandomSource&, SessionIdRegistry&)` references the
  separately owned registry. It reports `SessionIdStatus::StorageUnavailable`
  before entropy when storage is absent, preserves the existing 16-draw
  collision limit, and checks 65,536-entry capacity before RNG.
- The 131,072-slot/1,048,576-byte representation must have one authoritative
  definition shared with `startup_budget.h`; duplicating the byte arithmetic in
  two headers would reopen the budget oracle.

## Test-ID inventory

| Test ID | GTest oracle | Contract evidence |
|---|---|---|
| `WP03-ID2-001` | initial candidate succeeds with one draw and no registry; RNG failure and zero each fail after one draw | `P1-REQ-009`, startup-order support for `P1-REQ-014` |
| `WP03-ID2-002` | the registry and budget share the exact 131,072-slot/1 MiB representation; storage starts absent; allocation and release are idempotent; registration exposes every closed status except capacity | `P1-REQ-009`, allocation-order support for `P1-REQ-014` |
| `WP03-ID2-003` | after candidate draw, the real known subtotal succeeds while incomplete and storage remains absent; only then is the exact candidate registered without a second entropy draw | `P1-REQ-009`, `P1-REQ-014` |
| `WP03-ID2-004` | allocator with absent storage fails before RNG and publishes no ID | `P1-REQ-009`, no-partial-publication slice of `P1-AC-011` |
| `WP03-ID2-005` | existing entropy-failure/zero allocator oracle is preserved with explicit storage | `P1-REQ-009` |
| `WP03-ID2-006` | 15 collisions then a unique draw 16 succeeds | `P1-REQ-009` |
| `WP03-ID2-007` | 16 collisions stop at the retry limit and never consume draw 17 | `P1-REQ-009` |
| `WP03-ID2-008` | entropy failure during collision retries remains distinct and non-mutating | `P1-REQ-009` |
| `WP03-ID2-009` | capacity 65,536 is checked before RNG; direct registration reports `Capacity` without eviction while the registry remains live | `P1-REQ-009`, bounded-live-registry slice of `P1-AC-016` |

The source remains
`test/src/telemetry/producer/test_telemetry_identity_contract.cpp`. All producer
profile and native identity oracles are untouched. The file now contains 24
GTests: 23 normal tests plus the existing opt-in profile depth bomb.

## Expected RED build and first production dependency

The reviewed test-only refactor was compiled from the repository root with
serialized MSBuild:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /nr:false /v:minimal
```

Exit code: `1`.

```text
  code.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\code.lib
  gtest.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest.lib
  test_telemetry_identity_contract.cpp
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_identity_contract.cpp(523,33): error C2039: 'draw_session_id_candidate' : n'est pas membre de 'telemetry::detail'
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_identity_contract.cpp(524,2): error C2039: 'SessionIdCandidateStatus' : n'est pas membre de 'telemetry::detail'
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_identity_contract.cpp(544,10): error C2039: 'SessionIdRegistry' : n'est pas membre de 'telemetry::detail'
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_identity_contract.cpp(541,2): error C2039: 'StorageSlotCount' : n'est pas membre de 'telemetry::detail'
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_identity_contract.cpp(547,2): error C2039: 'SessionIdRegistrationStatus' : n'est pas membre de 'telemetry::detail'
```

MSVC then emitted dependent parse errors and stopped after its 100-error cap.
The first errors above are the intended missing production contract; the
already-built production library itself remained GREEN and no production file
was changed for this RED.

## Intermediate GREEN and preserved review RED

The first production refactor introduced candidate drawing, explicit registry
ownership, idempotent allocation/release, closed registration statuses and an
allocator referencing the registry. The Release unit-test target linked with
exit code `0`, and the complete normal identity selection passed:

```text
[==========] Running 23 tests from 3 test suites.
[----------] 6 tests from TelemetryProducerProfileContract (2 ms total)
[----------] 8 tests from TelemetryProducerIdentityContract (0 ms total)
[----------] 9 tests from TelemetrySessionIdentityContract (5 ms total)
[==========] 23 tests from 3 test suites ran. (8 ms total)
[  PASSED  ] 23 tests.
```

Static review then found that the byte constant was still expressed as
`slot_count * sizeof(uint64_t)` while the concrete array type was private. That
would permit a later representation change to diverge silently from the budget.
The test-only oracle was strengthened to require the named exact storage alias
and `SessionIdRegistryStorageBytes == sizeof(SessionIdRegistryStorage)`.

The serialized Release rebuild returned exit code `1` at this single new
production dependency:

```text
  test_telemetry_identity_contract.cpp
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_identity_contract.cpp(540,39): error C2039: 'SessionIdRegistryStorage' : n'est pas membre de 'telemetry::detail'
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_identity_contract.cpp(545,2): error C2039: 'SessionIdRegistryStorage' : n'est pas membre de 'telemetry::detail'
```

No production file was edited for this hardening RED. The successful 23-test
run is preserved as intermediate evidence, not misreported as final closure.

## Final GREEN verification

Production introduced the exact public-internal alias
`SessionIdRegistryStorage`, derived
`SessionIdRegistryStorageBytes = sizeof(SessionIdRegistryStorage)`, and made the
registry own that alias. The serialized Release unit-test target then rebuilt
and linked with exit code `0`.

The complete normal identity selection passed independently:

```text
[==========] 23 tests from 3 test suites ran. (6 ms total)
[  PASSED  ] 23 tests.
```

The isolated 900-container profile bomb passed `1/1` with
`--gtest_also_run_disabled_tests`. A combined regression selection then passed
`430/430`: five native profile/OS entropy tests, ten WP03 known-budget tests,
twenty normal configuration tests, 394 immutable protocol tests and the public
initialization smoke. The isolated configuration bomb also passed `1/1`.

The GCC 11.4 warning-as-error syntax pass found one pre-existing test-only
range-loop copy of a `std::string`. The loop variable was changed to
`const auto&`; the Release target rebuilt with exit code `0` and the same normal
identity selection remained `23/23 PASS`. No production code changed for that
portability cleanup.

The complete current Release binary passed:

```text
[==========] 680 tests from 96 test suites ran. (3482 ms total)
[  PASSED  ] 680 tests.
YOU HAVE 2 DISABLED TESTS
```

The full suite's disposable settings/preset fixtures were removed only after
each resolved path was verified under `test/test_data`; that subtree was clean
afterward.

### WP02 isolation and fast path

The instrumented initialization, allocation-failure and disabled-benchmark
targets rebuilt together with exit code `0`. The registration/idempotence test
passed `1/1`. Calibration still observed exactly five callback-registration
allocations, and every injected index `0..4` reported `contract_check=PASS`.

Fresh baseline and disabled benchmarks each measured 100,000 callbacks with
zero tracked C++ allocations and passing timing checks. The disabled result was
mean `0.000074573 ms`, p99 `0.000100000 ms`; samples stayed under the ignored
`build` tree.

### Independent reviewer reproduction

An independent reviewer rebuilt the serialized Release target successfully and
reproduced normal identity `23/23`, the isolated profile bomb `1/1`, and native
profile plus OS entropy plus BUDGET `15/15`. GCC 11.4 strict compilation passed
for both production and the final oracle. Static inspection confirmed one exact
131,072-`uint64_t` storage alias (`1,048,576` bytes), candidate draw before
storage, registration without RNG, idempotent rollback, and the exact
15/16/17-draw collision boundaries. The reviewer approved sub-tranche A with no
open finding; runtime order, injected allocation failure and process-lifetime
ownership remain explicitly assigned to B.

## Requirement and gate state

| Contract or gate | Evidence state after final GREEN |
|---|---|
| `P1-REQ-009` | GREEN for two-phase candidate/storage/registration plus preserved entropy, collision and capacity behavior while a registry remains live |
| `P1-REQ-014` | partial GREEN: the exact registry storage is priced by `sizeof`, and the API/oracle demonstrate candidate → subtotal → allocation → registration; the production runtime transaction remains open |
| `P1-AC-004` | open: first-tick terminal state, zero sockets and unique diagnostic belong to sub-tranche B |
| `P1-AC-011` | partial: the new API forbids partial ID publication; actual allocation-failure and runtime exception evidence remain open |
| `P1-AC-016` | partial: capacity and no eviction are proven while the registry is live; `release_storage()` intentionally permits pre-publication rollback, so process-lifetime ownership/non-reuse remains a runtime-B obligation |
| `G1-C` | open: runtime transaction and unavailable-transport rollback are not yet implemented |

## Explicit handoff and remaining gates

- retain the now-GREEN 23 normal identity tests, isolated bomb, native profile,
  OS entropy, BUDGET and full regressions;
- independently inspect the centralized alias, absence of eager allocation and
  portable strict compilation;
- register sub-tranche B for exact runtime sequencing,
  injected allocation failure, table rollback on `TransportUnavailable`, zero
  sockets, one diagnostic and terminal no-retry behavior.
