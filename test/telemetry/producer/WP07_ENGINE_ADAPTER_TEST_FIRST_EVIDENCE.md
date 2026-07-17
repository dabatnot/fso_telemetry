# WP07 engine adapter test-first evidence

## WP07-A RED checkpoint

The WP07-A checkpoint began with only `test_telemetry_engine_adapter_contract.cpp` and its registration in the standard `unittests` target. No production engine adapter existed, so four always-compiled tests failed explicitly on the absent `telemetry/engine_adapter.h` boundary:

```text
ValueDtoAndCaptureResultContractsExist
FsoOrientationConversionIsCanonicalAndTransactional
EnginePhysicsFlagsMapToTheClosedFstlBitmap
Wp07ABoundariesExcludeSessionWireRuntimeAndOwnership
```

Debug RED reproduction built and linked `unittests`, then produced the expected 0/4 PASS and 4/4 FAIL. The adjacent R2/runtime/heartbeat/reliability baseline remained 93/93 PASS, the allocation executable remained 7/7 PASS, and the R4 loopback oracle remained two explicit skips without opt-in and 2/2 PASS with opt-in.

Independent RED review required the test to use the tracker-frozen API rather than inferred names. The final guarded branch locks known member names, types and defaults for `CaptureVec3f`, `CaptureQuaternionf`, `CaptureOrientationBasis`, `PlayerObservationKey`, `PlayerKinematicsValue`, `PlayerObservationDto`, `PlayerKinematicsSample`, `EnginePhysicsFlagInput` and `CaptureResult`. It requires IEEE-754 32-bit float storage, exact `uint32_t`/`uint64_t` fields, standard-layout/trivially-copyable/nothrow value behavior and the complete closed `uint8_t` values and `Count` sentinels for `CaptureStatus`, `CaptureReason` and `QuaternionConversionStatus`. It deliberately does not freeze aggregate arity, object size, alignment, offsets or padding.

The reviewed quaternion oracle covers identity, positive and negative quarter turns on all axes, all 180-degree dominant-diagonal branches, local-axis round trips that detect transposition, slightly noisy finite bases, normalized output, `q`/`-q` equivalence and canonical sign/tie/negative-zero behavior. Every successful result requires `w > 0`, or `w == +0` with the first nonzero `x/y/z` positive, and every zero component must be positive zero. Every one of the nine input coefficients is independently replaced with NaN, positive infinity and negative infinity. Zero, collinear and overflowing bases are also rejected with the exact closed status and deterministic identity output.

The reviewed physics oracle maps each recognized `PF_*` bit independently, verifies supercap convergence, combinations and all three object-lock inputs, then requires each explicit unmapped bit alone to produce zero and each known-plus-unmapped combination to preserve the known result. Reserved FSTL bits may never escape. Separate traits reject encode, snapshot, replication-state and session-ownership surfaces individually and through their disjunction.

Hardened test source SHA-256 at RED and after GREEN verification:

```text
BD81675F89934A752E04316CB56261337A12F430683D5E976E7C42DE8613E06C
```

## WP07-A independent GREEN verification

The independent test owner inspected the final production slice before execution. `engine_adapter.h` matches the tracker-frozen value/enumeration/function contract. `engine_adapter.cpp` resets quaternion output before validation, rejects non-finite and degenerate bases, interprets FSO right/up/forward world axes as the columns of the logical local-to-world matrix, computes and normalizes in `double`, quantizes to float, checks the closed norm interval and canonicalizes sign and every signed zero. Physics mapping is confined to the reviewed `PF_*` constants, applies the adjudicated lock rules and masks the result with `KnownPhysicsModeFlags`. The collector remains declaration-only because engine collection belongs to WP07-B.

No anomaly was found in the WP07-A production slice. Build integration contains exactly the new production pair and the existing test registration.

Production SHA-256 values inspected for this verification:

```text
BBE495494DFDE0BDD8159C30EB48DE5595A4E3A9A06ED9D6317448F7B84F5BC0  engine_adapter.h
BDF1E739D64A6128A08328593936693C9C437CEDF5E429824E644ECB6FD294E7  engine_adapter.cpp
```

Focused and preserved verification:

```text
Build unittests:
  Debug:  PASS.
  Release: PASS.

TelemetryEngineAdapterContract.*:
  Debug:   10/10 PASS.
  Release: 10/10 PASS.

Adjacent R2/runtime/heartbeat/reliability filter:
  Debug:   93/93 PASS.
  Release: 93/93 PASS.

Allocation executable:
  Debug:   7/7 PASS.
  Release: 7/7 PASS.

R4 native loopback with FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1:
  Debug:   IPv4 and IPv6, 2/2 PASS.
  Release: IPv4 and IPv6, 2/2 PASS.

Full unittests with loopback opt-in unset:
  Debug:   864 tests from 112 suites; 862 PASS, 2 expected loopback SKIP;
           2 pre-existing disabled tests; 14,211 ms.
  Release: 864 tests from 112 suites; 862 PASS, 2 expected loopback SKIP;
           2 pre-existing disabled tests; 4,257 ms.

TelemetryEngineAdapterContract stress,
--gtest_shuffle --gtest_random_seed=20260717:
  Debug:   10 tests x 25 iterations = 250/250 PASS.
  Release: 10 tests x 10 iterations = 100/100 PASS.
```

Full-suite generated artifacts under `test/test_data` were removed. `git diff --check` passed after the evidence move with line-ending warnings only. The WP06 evidence file is byte-identical to `HEAD`, and the test hash remained unchanged throughout production inspection, builds and execution.

## Requirement delta

| ID | WP07-A evidence | Honest status after this checkpoint |
|---|---|---|
| `P1-WP-07` | The engine-adapter value, orientation and physics mapping slice is GREEN in Debug and Release. | `PARTIAL` - WP07-A is verified; WP07-B identity/collection and WP07-C cadence/publication remain open. |
| `P1-REQ-016` | Shared observation and sample value/copy boundaries are verified without entity-registry ownership or publication cadence. | `PARTIAL` - the WP07-A DTO boundary is verified; collection and identity remain later work packages. |
| `P1-REQ-022` | Local-to-world quaternion conversion, normalization, canonicalization and closed transactional rejection pass exhaustive targeted tests. | `VERIFIED FOR WP07-A`. |
| `P1-REQ-024` | Every recognized engine physics mapping, alias, lock, combination, unmapped input and reserved-output boundary passes. | `VERIFIED FOR WP07-A`. |
| `P1-REQ-025` | Fixed non-owning observation values and closed capture/conversion status contracts pass without Runtime, allocation or wire behavior. | `PARTIAL` - the WP07-A value/status boundary is verified; collector behavior remains WP07-B. |
| `P1-AC-006` | The source-to-DTO schema boundary and conversion primitives are verified. | `OPEN` - engine collection and later JSON/wire correctness remain WP07-B, WP08 and WP10. |

## WP07-B engine collector RED checkpoint

WP07-B extends the already-registered engine-adapter contract source only. No source group, CMake target, production file, entity registry, cadence, Runtime, replication or allocation contract changed.

The test declares only the tracker-frozen future names needed to keep the current A-only header compilable. A completeness-dependent template produces seven explicit RED failures while `EngineReadView`, `EnginePlayerKinematicsRead` and `FsoEngineReadView` remain incomplete. Once production completes those types, the same template instantiates the full behavioral branch without a feature macro or test-only production seam.

The counted fake reserves the exact eight predicate methods and one snapshot read. Its bounded trace records total calls and overflow separately, so a duplicate or extra call fails without writing beyond the trace array. For each predicate failure independently, the oracle requires calls exactly once in declaration order, zero downstream calls, the exact primary `CaptureStatus`/`CaptureReason`, and a fully default output. The successful path requires all nine calls exactly once in order, one value snapshot, exact positive signature-to-`uint32_t` mapping, exact `now_us`, exact float copies, canonical orientation and closed mapped physics flags. Its source orientation is a non-identity 90-degree rotation about the asymmetric normalized axis `(1,2,3)`; all four quaternion components and the three local-axis-to-world round trips are asserted.

Post-copy validation uses a hostile ladder that keeps all later fields invalid while repairing one field per iteration. It therefore locks the primary order observation key, position, orientation, velocity, rotational velocity and radius, with no partial key, timestamp, pose or flags on any failure. Reusing an output after both a valid sample and a subsequent `NoPlayer`/`InvalidSource` result must restore the complete default DTO and cannot expose stale state.

The numeric oracle accepts both inclusive boundaries for every position, velocity and rotational-velocity component, plus both radius boundaries. It rejects both immediately adjacent out-of-range values for every component and radius. Every component and all nine orientation coefficients are independently exercised with NaN, positive infinity and negative infinity. Finite positive and negative subnormals are asserted exactly in all position, velocity and rotational-velocity components, a positive subnormal radius is preserved, a negative subnormal radius is rejected, and every successful source negative zero is canonicalized to positive zero. Every zero in a reset DTO, including nested vectors, quaternion and radius, must also be positive zero. The raw snapshot's known member names, exact types and defaults are locked without arity or ABI assertions. The real FSO view contract is final, nothrow constructible and factory-created by value; the RED test only proves future compile/link integration and never reads live engine globals, while no-data-member statelessness remains a source-review obligation because the polymorphic type has no portable emptiness trait.

Debug RED reproduction:

```text
cmake --build build --config Debug --target unittests --parallel 4
Result: PASS; the extended existing contract source compiled and unittests linked.

build/bin/Debug/unittests.exe \
  --gtest_filter="TelemetryEngineCollectorContract.*" --gtest_color=no
Result: expected RED; 7 tests ran, 0 PASS, 7 FAIL.
Cause: EnginePlayerKinematicsRead, EngineReadView and FsoEngineReadView are not yet complete in A production.

Preserved Debug baselines:
  WP07-A engine adapter: 10/10 PASS.
  R2/runtime/heartbeat/reliability: 93/93 PASS.
  Allocation executable: 7/7 PASS.
  R4 native IPv4/IPv6 loopback with opt-in: 2/2 PASS.

Extended A+B contract source SHA-256:
  3D9A6E44F81B62C147522A8EAD50F7270A5CF8C174D5DFF8FDB97B007F9745BA
```

This is a Debug-only RED checkpoint pending independent review. It makes no WP07-B production, Release or full-suite claim.

| ID | WP07-B RED evidence | Honest status after this checkpoint |
|---|---|---|
| `P1-WP-07` | The collector API, precondition order, atomic snapshot and validation boundaries are executable RED contracts. | `PARTIAL / RED` - WP07-A remains verified; WP07-B production and all WP07-C work remain open. |
| `P1-REQ-016` | Source observation to non-owning DTO, exact sample time and no-stale transactionality are reserved. | `RED / RESERVED` - collector implementation is absent. |
| `P1-REQ-024` | Safe ordered engine reads, exact float mapping, finite/bounded validation and canonical orientation are reserved. | `RED / RESERVED` - collector and real FSO view implementation are absent. |
| `P1-REQ-025` | Closed capture reasons and default-output behavior are reserved without Runtime, registry or wire ownership. | `RED / RESERVED` - collector implementation is absent. |
| `P1-AC-006` | Engine source-to-DTO behavior is reserved by seven failing collector tests. | `OPEN` - no WP07-B production result exists, and later JSON/wire work remains WP08/WP10. |

## WP07-B independent GREEN verification

The independent test owner inspected the completed types, real FSO view and collector before execution. `FsoEngineReadView` has no data members and the factory returns it by value. Each predicate guards every global or indexed access it performs: type precedes ship instance, bounds precede `Objects`/`Ships` dereferences, and object/player/ship coherence converges on the same bounded object entry. The snapshot method defensively repeats the complete mission, pointer, type, instance, object-index and ship-index validation before copying only value fields; it retains no engine pointer, reference or index.

`collect_player_kinematics` first resets its output, invokes the eight predicates in the frozen order, reads exactly one raw snapshot only after all predicates pass, and validates signature, position, orientation, velocity, rotational velocity and radius in the frozen primary order. It builds a local candidate, canonicalizes all numeric zeroes, maps only the closed physics bitmap and commits once. Every early return therefore leaves the full default DTO. No anomaly was found in the WP07-B slice.

The hardened A+B test source remained unchanged throughout production inspection and verification:

```text
3D9A6E44F81B62C147522A8EAD50F7270A5CF8C174D5DFF8FDB97B007F9745BA  test_telemetry_engine_adapter_contract.cpp
```

Production SHA-256 values inspected:

```text
C2FFB4A61414B8CA316E83859CC1A7E8B606879681A103B75A71047F42380A46  engine_adapter.h
4E8E1E7EAF110044B4FAA1DF037D3E40FBC6A90C8879A7F68D18418A43D7E1CF  engine_adapter.cpp
```

Independent verification results:

```text
Build unittests:
  Debug:   PASS.
  Release: PASS.

TelemetryEngineAdapterContract.* + TelemetryEngineCollectorContract.*:
  Debug:   17/17 PASS.
  Release: 17/17 PASS.

Adjacent R2/runtime/heartbeat/reliability filter:
  Debug:   93/93 PASS.
  Release: 93/93 PASS.

Allocation executable:
  Debug:   7/7 PASS.
  Release: 7/7 PASS.

R4 native loopback with FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1:
  Debug:   IPv4 and IPv6, 2/2 PASS.
  Release: IPv4 and IPv6, 2/2 PASS.

Full unittests with loopback opt-in unset:
  Debug:   871 tests from 113 suites; 869 PASS, 2 expected loopback SKIP;
           2 pre-existing disabled tests; 13,582 ms.
  Release: 871 tests from 113 suites; 869 PASS, 2 expected loopback SKIP;
           2 pre-existing disabled tests; 4,257 ms.

A+B engine-adapter stress,
--gtest_shuffle --gtest_random_seed=20260717:
  Debug:   17 tests x 25 iterations = 425/425 PASS.
  Release: 17 tests x 10 iterations = 170/170 PASS.
```

Full-suite generated artifacts under `test/test_data` were removed. Final `git diff --check` passed with line-ending warnings only.

### WP07-B requirement delta

| ID | WP07-B GREEN evidence | Honest status after this checkpoint |
|---|---|---|
| `P1-WP-07` | Engine observation, ordered safety checks, atomic collection and invalid-source transactionality are GREEN in Debug and Release. | `PARTIAL` - WP07-A/B are verified; entity registry/cadence/publication in WP07-C remain open. |
| `P1-REQ-016` | Exact engine snapshot to non-owning DTO, sample time and no-stale behavior pass. | `VERIFIED FOR WP07-B` - identity allocation and publication remain WP07-C. |
| `P1-REQ-024` | Real view safety, ordered validation, bounds, non-finite handling, subnormal preservation, local-to-world quaternion and positive-zero canonicalization pass. | `VERIFIED FOR WP07-B`. |
| `P1-REQ-025` | Closed capture reasons and full default output on every non-valid path pass without Runtime, registry or wire ownership. | `VERIFIED FOR WP07-B`. |
| `P1-AC-006` | Engine source-to-DTO behavior and conversion primitives are GREEN. | `PARTIAL` - identity, publication and later JSON/wire correctness remain WP07-C, WP08 and WP10. |

## WP07-C session entity registry RED checkpoint

WP07-C adds one always-compiled entity-registry contract source to the existing `unittests` target. It changes no production file and does not begin cadence, Runtime integration, session-slot ownership, budget repricing or allocation instrumentation; the latter remains a final WP07-D gate.

Seven explicit tests fail while `telemetry/entity_id_registry.h` is absent. The guarded future branch locks the tracker-frozen `uint8_t` status values and defaults for resolve, invalidation and materialization. It also locks the exact `EntityIdResolveResult` member types, exact return types and `noexcept` status of `resolve`, `invalidate`, `reset_session` and `materialize_player_sample`, plus the exact return types and `const noexcept` contract of `has_active_mapping`, `active_object_signature`, `active_entity_id` and `last_allocated_entity_id`. The registry remains final and nothrow constructible. The contract deliberately makes no size, alignment, aggregate-arity or ABI assertion.

The pure registry oracle requires ID 1 for the first valid key, stability for the same active key, strict increment on replacement, and a new ID when an old key reappears after replacement or explicit invalidation. Invalid key and repeated invalidation clear only the active mapping and preserve the last counter. A reset exercised directly while a mapping is active clears both that mapping and counter, allowing the next wire session to restart at 1. A separate registry initialized at `UINT64_MAX` proves `CounterExhausted` is sticky until `reset_session()`, after which the next key also receives ID 1.

Deterministic injected-counter tests prove that `UINT64_MAX` itself is assignable from `max-1`, the same active key at max remains stable, and every subsequent new-key attempt returns `CounterExhausted`, clears active state and never wraps or reuses an ID. A constructor starting at max also reserves the materialization overflow path without iteration.

Materialization first resets a reused output. A valid observation must resolve exactly one identity transition, distinguish `MaterializedNew` from `MaterializedExisting`, copy every kinematics value exactly, publish the nonzero entity ID, and remain copy-independent. The complete Cartesian product of capture statuses (including `Count`) and reasons (including `Count`) is classified: only the closed NoPlayer and InvalidSource pairs receive their corresponding result; every mismatch is `InvalidCapture`. All non-valid paths invalidate active state exactly once in observable effect, preserve the monotonic counter, consume no ID and leave a full default sample: quaternion `w` is exactly 1 and every zero float is positive zero by `signbit`. Valid capture with signature zero is `InvalidCapture`; exhausted allocation is `EntityIdCounterExhausted` with no partial sample.

Debug RED reproduction:

```text
cmake --build build --config Debug --target unittests --parallel 4
Result: PASS; the new test-only source compiled and unittests linked.

build/bin/Debug/unittests.exe \
  --gtest_filter="TelemetryEntityIdRegistryContract.*" --gtest_color=no
Result: expected RED; 7 tests ran, 0 PASS, 7 FAIL.
Cause: telemetry/entity_id_registry.h is absent.

Preserved Debug baselines:
  WP07-A/B engine adapter and collector: 17/17 PASS.
  R2/runtime/heartbeat/reliability: 93/93 PASS.
  Allocation executable: 7/7 PASS.
  R4 native IPv4/IPv6 loopback with opt-in: 2/2 PASS.

WP07-C contract source SHA-256:
  E1EA27E7457C2D4DB2D2C52ADD7AA4B2FE3F9FD9986CFDC8EBD2DD497D65EF14
```

This is a Debug-only RED checkpoint pending independent review. Zero-allocation instrumentation, per-slot ownership, budget proof and cadence remain explicitly deferred to WP07-D.

| ID | WP07-C RED evidence | Honest status after this checkpoint |
|---|---|---|
| `P1-WP-07` | Session-scoped identity stability, invalidation, overflow and sample materialization are executable RED contracts. | `PARTIAL / RED` - WP07-A/B remain verified; WP07-C production and WP07-D integration remain open. |
| `P1-REQ-016` | Exact observation-to-sample materialization and invalid/no-player removal are reserved. | `RED / RESERVED` - registry production is absent. |
| `P1-REQ-025` | Nonzero monotonic IDs, no reuse, max/exhaustion and session reset semantics are reserved. | `RED / RESERVED` - registry production is absent. |
| `P1-AC-006` | DTO-to-identity-bearing sample behavior is reserved without publication or wire claims. | `OPEN` - no WP07-C production result exists and later publication/JSON/wire work remains. |

## WP07-C independent GREEN verification

The independent test owner inspected the registry and materialization implementation before execution. `resolve` rejects signature zero transactionally, preserves an existing active mapping, invalidates before every replacement, assigns the complete nonzero `uint64_t` range without wrapping, and leaves exhaustion sticky until `reset_session`. Invalidation clears every active-map observer without changing the monotonic counter; session reset additionally clears that counter.

`materialize_player_sample` resets its output before classification, accepts only the frozen closed capture-status/reason pairs, invalidates on every non-valid path, and resolves identity only for `Valid`/`None` with a nonzero signature. Exhaustion and unexpected resolve results return without a partial sample. Successful materialization copies the observation into a local candidate and commits it once with the resolved ID. No registry, materialization or build-integration anomaly was found.

The hardened WP07-C test source remained unchanged throughout production inspection and verification:

```text
E1EA27E7457C2D4DB2D2C52ADD7AA4B2FE3F9FD9986CFDC8EBD2DD497D65EF14  test_telemetry_entity_id_registry_contract.cpp
```

Production SHA-256 values inspected:

```text
FC8ACED383938572057DED9AFB2E023C723AF8BDFF6F18C1C097E98037EAA3FB  entity_id_registry.h
4406DA882BBC3FC4901C70A9F129D67DC86E709593D3CCEDF4A0C533BA64D53A  entity_id_registry.cpp
```

Independent verification results:

```text
Build unittests and allocation target:
  Debug:   PASS.
  Release: PASS.

TelemetryEntityIdRegistryContract.*:
  Debug:   7/7 PASS.
  Release: 7/7 PASS.

TelemetryEngineAdapterContract.* + TelemetryEngineCollectorContract.*:
  Debug:   17/17 PASS.
  Release: 17/17 PASS.

Adjacent native Runtime integration/runtime/heartbeat/reliability filter:
  Debug:   93/93 PASS.
  Release: 93/93 PASS.

Allocation executable:
  Debug:   7/7 PASS.
  Release: 7/7 PASS.

R4 native loopback with FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1:
  Debug:   IPv4 and IPv6, 2/2 PASS.
  Release: IPv4 and IPv6, 2/2 PASS.

Full unittests with loopback opt-in unset:
  Debug:   878 tests from 114 suites; 876 PASS, 2 expected loopback SKIP;
           2 pre-existing disabled tests; 12,928 ms.
  Release: 878 tests from 114 suites; 876 PASS, 2 expected loopback SKIP;
           2 pre-existing disabled tests; 4,250 ms.

A+B+C engine-adapter/collector/registry stress,
--gtest_shuffle --gtest_random_seed=20260717:
  Debug:   24 tests x 25 iterations = 600/600 PASS.
  Release: 24 tests x 10 iterations = 240/240 PASS.
```

Full-suite generated artifacts under `test/test_data` were removed. Final `git diff --check` passed with line-ending warnings only.

### WP07-C requirement delta

| ID | WP07-C GREEN evidence | Honest status after this checkpoint |
|---|---|---|
| `P1-WP-07` | Session-scoped identity stability, invalidation, overflow and transactional sample materialization are GREEN in Debug and Release. | `PARTIAL` - WP07-A/B/C are verified; cadence, slot ownership, budget and allocation instrumentation remain WP07-D. |
| `P1-REQ-016` | Observation-to-sample materialization, exact value copy and invalid/no-player removal pass. | `VERIFIED FOR WP07-C` - publication remains WP07-D and later JSON/wire work remains WP08/WP10. |
| `P1-REQ-025` | Nonzero monotonic IDs, no reuse, max/exhaustion and wire-session reset semantics pass. | `VERIFIED FOR WP07-C`. |
| `P1-AC-006` | DTO-to-identity-bearing sample behavior is GREEN. | `PARTIAL` - cadence/publication and later JSON/wire correctness remain open. |

## WP07-D1 capture cadence RED checkpoint

After the independent reviewer approved the oracle and the Linux C++ CI barrier completed successfully, WP07-D1 adds one always-compiled cadence contract source to `unittests`. This API and its closed enum are a local internal implementation contract only: they add no FSTL wire value, field, layout or compatibility claim.

The guarded future branch locks the exact final/noexcept `Capture30Hz` surface, including `configure(std::uint32_t)`, and the tracker-frozen cadence semantics. Every valid rate from 1 through 60 is checked against an independent quotient-plus-remainder ceiling oracle, with explicit 1/30/60 periods of 1,000,000/33,334/16,667 microseconds and rejection of 0/61. Invalid configuration followed by a valid lower timestamp must be immediately due, proving complete history reset. The first active poll is immediately due; exact boundaries emit once; hitches skip missed periods and advance from `now`; inactive, `stop` and `reset` retain distinct scopes; every poll participates in monotonic-time validation; and both initial and later deadline overflow fail closed without a due capture. It deliberately makes no object-size, alignment or wire assertion.

Debug RED reproduction:

```text
cmake --build build --config Debug --target unittests --parallel 4
Result: PASS; CMake regenerated, the test-only source compiled and unittests linked.

build/bin/Debug/unittests.exe \
  --gtest_filter="TelemetryCaptureSchedulerContract.*" --gtest_color=no
Result: expected RED; 7 tests ran, 0 PASS, 7 FAIL.
Cause: telemetry/capture_scheduler.h is absent.

Preserved Debug baselines:
  WP07-C entity registry: 7/7 PASS.
  WP07-A/B engine adapter and collector: 17/17 PASS.
  Native Runtime integration/runtime/heartbeat/reliability: 93/93 PASS.
  Allocation executable: 7/7 PASS.
  R4 native IPv4/IPv6 loopback with opt-in: 2/2 PASS.

WP07-D1 contract source SHA-256:
  50E64D211FE92A1CC702C8DF220D1CEDD90772B4C469476F8DFC9CF21856DED4
```

This is a Debug-only RED checkpoint. Cadence production, per-slot ownership, native/runtime capture integration, budget repricing and allocation instrumentation remain open; WP08 wire publication remains explicitly excluded.

## WP07-D1 independent GREEN verification

The independent test owner inspected the completed scheduler before execution. Configuration resets all prior state, accepts exactly rates 1 through 60 and computes the ceiling period using integer quotient and remainder. Polling validates monotonic time before activity, emits the first active capture immediately, advances every due deadline from the current time without catch-up, and fails closed on deadline overflow. Inactive polling, `stop` and `reset` implement their distinct tracker-frozen state scopes. No scheduler anomaly was found.

The strengthened D1 test source remained unchanged throughout production inspection and verification:

```text
50E64D211FE92A1CC702C8DF220D1CEDD90772B4C469476F8DFC9CF21856DED4  test_telemetry_capture_scheduler_contract.cpp
```

Production SHA-256 values inspected:

```text
6C2198075EABD15DBCD1C96450C5F36171C192B47F36990A6D7FB8F906B90A7F  capture_scheduler.h
14CDACF3B440A3B3336D2C7CFEAB99D3AA4FE2D8CEEEFB647172F7802982D9C0  capture_scheduler.cpp
```

The first incremental Debug replay still contained the RED object compiled while `capture_scheduler.h` was absent: MSBuild cannot discover a header that failed `__has_include` as a future dependency. A broad `--clean-first` attempt reported the repository's duplicate-project `MSB5004` condition and did not clean that object. The test owner therefore removed only the stale Debug and Release scheduler-test object files under `build/test/src/unittests.dir`, then rebuilt both configurations; both recompilations visibly compiled `test_telemetry_capture_scheduler_contract.cpp` against the production header.

Independent verification results:

```text
Build unittests and the existing allocation target after targeted object rebuild:
  Debug:   PASS.
  Release: PASS.

TelemetryCaptureSchedulerContract.*:
  Debug:   7/7 PASS.
  Release: 7/7 PASS.

WP07-A/B/C adapter, collector and entity-registry contracts:
  Debug:   24/24 PASS.
  Release: 24/24 PASS.

Adjacent native Runtime integration/runtime/heartbeat/reliability filter:
  Debug:   93/93 PASS.
  Release: 93/93 PASS.

Existing allocation executable:
  Debug:   7/7 PASS.
  Release: 7/7 PASS.
  Qualification: adjacent non-regression only; this executable does not yet
  exercise Capture30Hz and is not a zero-allocation proof for the scheduler.

R4 native loopback with FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1:
  Debug:   IPv4 and IPv6, 2/2 PASS.
  Release: IPv4 and IPv6, 2/2 PASS.

Capture scheduler stress,
--gtest_shuffle --gtest_random_seed=20260717:
  Debug:   7 tests x 100 iterations = 700/700 PASS.
  Release: 7 tests x 50 iterations = 350/350 PASS.
```

## WP07-D2 per-slot observation ownership RED checkpoint

WP07-D2 now has an always-compiled, test-only local contract. It reserves no FSTL wire field or numeric value and does not enter cadence, native runtime, startup-budget repricing, WP08 publication, or later-phase functionality.

The guarded future branch locks the exact `SessionPlayerMaterializationResult` name and seven-field aggregate, exact `noexcept` member-pointer signatures for apply/clear, the exact `player_entity_ids`/`latest_player_sample`/`latest_player_sample_status`/`has_latest_player_sample` slot names and types, and the exact static `bool noexcept` friend seed pointer. Its ten tests use complete sample comparisons and unique balanced buckets. They cover Empty/AwaitWelcome ineligibility and rejected seeding; one- and two-Ready-slot new/existing/replacement identity; a timeout-driven Stale slot with all observable session, heartbeat, network and reliable state preserved; an independent explicit switch oracle over every closed enum pair plus `Valid/None` key zero, including invalidation and ID-2 reappearance; idempotent all-state clear; close/reuse isolation; purge; and owning-slot-only exhaustion close with aggregate reset.

The existing allocation executable also contains A11. Its future branch configures two slots and measures every post-configure D2 path: Empty, AwaitWelcome, two Ready owners, new/existing/replacement, the exhaustive status/reason matrix and key zero, repeated clear, real Stale, seeded exhaustion close, explicit close of the survivor, and purge. Probe arming surrounds only the operation under measurement and is disarmed before every assertion.

Final RED reproduction after the reviewer hardening pass:

```text
cmake --build build --config Debug --target unittests telemetry_session_controller_allocation_contract_tests --parallel 4
cmake --build build --config Release --target unittests telemetry_session_controller_allocation_contract_tests --parallel 4
Result: PASS in Debug and Release; both test sources compiled and both executables linked.

unittests --gtest_filter=TelemetrySessionPlayerObservationContract.*
Debug:   expected RED, 10 ran, 0 PASS, 10 FAIL.
Release: expected RED, 10 ran, 0 PASS, 10 FAIL.
Sole cause: SessionController per-slot player observation API/ownership is absent.

telemetry_session_controller_allocation_contract_tests --gtest_filter=TelemetryWp07D2AllocationContract.*
Debug:   expected RED, 1 ran, 0 PASS, 1 FAIL.
Release: expected RED, 1 ran, 0 PASS, 1 FAIL.
Sole cause: D2 apply/clear/friend-seed seams are absent.

Preserved baselines, rerun after the final rebuild:
  Debug:   D1 7/7; A+B+C 24/24; session 37/37; adjacent 93/93;
           historical allocation 7/7; opt-in native loopback 2/2 PASS.
  Release: D1 7/7; A+B+C 24/24; session 37/37; adjacent 93/93;
           historical allocation 7/7; opt-in native loopback 2/2 PASS.
```

Final SHA-256 values:

```text
DDC273CE462A4A4D41E1FA078198CF565F7829A03A0DBB30CC469BB76695F8EC  test_telemetry_player_observation_slot_contract.cpp
84F6FBA5AA5458D40DB6565E5F1BD526818464FC371A8D70379E03263977F2EB  test_telemetry_session_controller_allocations.cpp
16C27C6F94F53F42F18CFF4C5106A1B966F1ED7F4CAC5A7D71F1F0D4D4D95038  test/src/source_groups.cmake
```

This checkpoint is intentionally RED: D1 remains GREEN, D2 production is absent, and D3-D6 plus WP08 remain open. No production source or production build integration was changed.

## WP07-D2 independent GREEN verification

After D2 production landed, the test owner separated handshake message sequences from controller time. `open_ready` and `open_awaiting` now require an explicit `base_time_us`; every multi-slot scenario uses strictly increasing deterministic times, and U7 resumes at 4,000,000 us after creating a real Stale slot. The counter-exhaustion seed seam is now defined exclusively in the shared test header `telemetry_session_controller_player_test_access.h`: its only method refuses invalid, Empty, and AwaitWelcome slots, accepts only Ready/Stale, replaces the test slot registry, and clears only the latest player sample fields. No callable seed symbol exists in production. These changes preserve every API, ownership, lifecycle, outcome-bucket, exhaustion, Empty-slot immutability, and allocation oracle.

Final verification results:

```text
Build unittests and allocation target:
  Debug:   PASS.
  Release: PASS.

TelemetrySessionPlayerObservationContract.*:
  Debug:   10/10 PASS.
  Release: 10/10 PASS.

TelemetryWp07D2AllocationContract.*:
  Debug:   1/1 PASS.
  Release: 1/1 PASS.

Capture scheduler baseline:
  Debug:   7/7 PASS.
  Release: 7/7 PASS.

WP07-A/B/C adapter, collector and entity-registry baseline:
  Debug:   24/24 PASS.
  Release: 24/24 PASS.

Session budget/ingress/handshake/security baseline:
  Debug:   37/37 PASS.
  Release: 37/37 PASS.

Adjacent native Runtime integration/runtime/heartbeat/reliability baseline:
  Debug:   93/93 PASS.
  Release: 93/93 PASS.

Complete allocation executable, including A11:
  Debug:   8/8 PASS.
  Release: 8/8 PASS.

Native loopback with FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1:
  Debug:   IPv4 and IPv6, 2/2 PASS.
  Release: IPv4 and IPv6, 2/2 PASS.
```

Final SHA-256 values inspected:

```text
62B573B816029B3C5A8CAD49CDB03A53FE5C12A1F1A5D134344A28BE9D673F74  telemetry_session_controller_player_test_access.h
9BA6907F5E99AFC279E677445044CE51F612413DD0A2089C000909BEE71E5FBC  test_telemetry_player_observation_slot_contract.cpp
0FC27BD1AA9FC516C17BC01E02D4D0F2F2D385885BAC7C511DC1C92D23D676C2  test_telemetry_session_controller_allocations.cpp
369B60530E8DAC64BD50695801CF82D458684B033B010C70051ECF0E1D9C379A  test/src/source_groups.cmake
FEE8F498BB6D80E34145A24D83EC703DAC2DF508CBE2F6A20E78883FE3DC0BF2  session_controller.h
732F7AAFFB834E46A12E1532DD7A62CE659EAF6B2B1E470901FA4F2502B8DC1E  session_controller.cpp
```

D2 per-slot player observation ownership, lifecycle reset, balanced outcomes, counter exhaustion isolation, and post-configuration zero-allocation coverage are GREEN. D3-D6 and WP08 remain open. No production defect was observed by this verification.

Final `git diff --check` passed with line-ending warnings only. D1 cadence production is complete and independently GREEN. D2–D6 remain open: per-slot observation ownership, native/runtime capture integration and lifecycle, startup-budget repricing, and scheduler-aware zero-allocation instrumentation. WP08 wire publication remains explicitly excluded.
