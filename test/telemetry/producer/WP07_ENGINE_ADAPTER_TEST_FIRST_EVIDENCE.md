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
