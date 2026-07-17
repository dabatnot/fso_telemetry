# WP06-HB test-first evidence

## Baseline and scope

This RED suite was authored against commit
`c94bf732374ebc41d2694e0c968a3eaf12818ba4`, after WP06 handshake and reliability composition
were present. No production file was changed for this capture. The diff is limited to one dedicated
heartbeat contract source, its test-only CMake registration, the isolated post-Ready allocation
contract, and this evidence.

The sub-lot covers producer-side heartbeat negotiation, cadence, request/response handling, bounded
probe and sample state, clock filtering, timeout transitions, fairness, backpressure and cleanup. It
does not add WP08 resynchronization/snapshot behavior, WP09 metric export, native runtime/transport,
new protocol values, or any Phase 0 wire change.

## RED inventory

| Plan IDs | Test | Contract locked by the oracle |
|---|---|---|
| `HB-001`, `HB-003` | `WelcomeFreezesMissionSelectedHeartbeatIntervalForSessionLifetime` | select configured mission H during an active-mission handshake, announce H=500 ms in WELCOME, store it in the slot and never silently change it during that session |
| `HB-002` | `CanonicalRequestQueuesImmediateResponseWithExactFourTimestampPrefix` | accept a canonical session HEARTBEAT Request, refresh `last_valid_network_activity_us`, and queue one non-reliable Response echoing `probe_id/t0`, with `t1=t2=now` and no extra flag |
| `HB-004`, `HB-005` | `ResponseCorrelationRejectsForgedTupleAndFeedsPhase0ClockFilter` | reject the right probe ID paired with a forged `t0` atomically; consume the exact tuple once, reject its duplicate, and prove the Phase 0 result is exactly RTT=90 us and offset=-35 us |
| `HB-006`, `HB-007` | `EightConcurrentProbesSaturateWithoutEvictionAndNinthSampleEvictsOldest` | eight concurrent probes are a hard capacity: the ninth request is `CapacityReached` behavior with no probe eviction or packet-sequence consumption; only a ninth valid clock sample evicts the oldest of eight samples |
| `HB-003`, `HB-010` | `PeriodicCadenceUsesExactBoundariesAndHitchesNeverBurst` | anchor idle due at proof time + 1,000,000 us; emit at `due`, not `due-1`; abandon an unreliable heartbeat on `WouldBlock`; a multi-H hitch strictly before disconnect emits one Request and never causes a same-now burst |
| `HB-008` | `StaleAndDisconnectTimeoutsAreExactAndNeverPromoteStaleToReady` | derive idle thresholds from immutable H=1000 ms and assert exact 3,000,000/10,000,000 us; transition at exact stale/long boundaries; invalidate the filter at stale; a later valid response may rebuild the filter but must not promote `Stale -> ReadyForState` |
| `HB-009` | `HeartbeatRequestBucketIsTwentyThenRefillsAtExactlyTenPerSecond` | session Request bucket accepts exactly 20, rejects the next through `refill-1`, and accepts one at the exact 10/s refill boundary |
| `HB-011` | `SimultaneouslyDueClientsAdvanceOneDatagramPerCallWithoutStarvation` | one bounded datagram per maintenance call and round-robin progress for two simultaneously due clients |
| `HB-012` | `ClosingSessionPurgesProbesSamplesAndOwnedHeartbeatOutput` | after one valid sample, leave a new owned heartbeat/probe pending; terminal close clears output, probes, samples and filter validity |
| `HB-012` | `ClosingUnrelatedSessionPreservesPendingHeartbeatOwner` | with shared controller egress, closing a different slot preserves the heartbeat owner's endpoint, size and bytes exactly |
| `HB-002`–`HB-012` | `HeartbeatCadenceResponseFilterAndTimeoutAllocateNothingAfterReady` | cadence, correlation/filter mutation, `WouldBlock`, stale and disconnect cleanup perform zero dynamic allocations after Ready |

The compile-safe seam contract deliberately requires one time-only maintenance operation:

```cpp
void SessionController::service_session_maintenance(std::uint64_t now_us) noexcept;
```

There is no `mission_active` parameter. The handshake already receives mission state and must choose
the configured mission or idle interval there, advertise that interval in WELCOME, then retain it as
the immutable session H. The fixed per-slot observation seam used by the tests is `slot.heartbeat`
with `probes`, `clock_filter`, `negotiated_interval_ms`, `stale_timeout_us`,
`disconnect_timeout_us`, `next_periodic_due_us`, `last_valid_network_activity_us`,
`last_valid_clock_response_us` and `clock_stale`.

The cadence and timeout oracles do not derive their expected values from production fields. With
the deterministic proof at `200 us`, idle H=1000 ms makes the first due exactly `1,000,200 us`, the
next due exactly `2,000,200 us`, stale exactly `3,000,000 us` and disconnect exactly
`10,000,000 us`. Mission H=500 ms makes first/next due `500,200/1,000,200 us`; its `3*H` and `10*H`
values remain below the normative minima, so its stale/disconnect thresholds are also exactly
`3,000,000/10,000,000 us`.

Two boundary corrections are explicit:

- eight in-flight probes are a saturated set. A ninth probe is not allowed to evict any of them;
  FIFO eviction applies only when adding a ninth valid sample to the eight-sample `ClockFilter`;
- a valid heartbeat response after `Stale` may rebuild clock evidence and refresh network activity,
  but it cannot restore semantic readiness. WP08 resynchronization or a new session owns recovery.

## Measured RED

Both test targets build successfully in Debug. The new tests are compile-safe so they preserve a
runnable test executable while reporting the absent production API and behavior precisely.

```powershell
cmake --build build --target unittests telemetry_session_controller_allocation_contract_tests `
  --config Debug --parallel 4
# exit 0

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06HeartbeatContract.*' `
  '--gtest_brief=1' '--gtest_color=no'
# exit 1: 0/10 passed, intentional RED

& .\build\bin\Debug\telemetry_session_controller_allocation_contract_tests.exe `
  '--gtest_filter=TelemetryWp06AllocationContract.Heartbeat*' `
  '--gtest_brief=1' '--gtest_color=no'
# exit 1: 0/1 passed, intentional RED
```

The measured failures are narrowly attributable to WP06-HB gaps:

1. canonical session HEARTBEAT input is rejected with the existing drop reason value `0x03`, so no
   immediate Response is queued;
2. the `service_session_maintenance(now_us)` seam and fixed per-slot heartbeat state are absent,
   blocking exact cadence/threshold, tuple correlation, clock result, capacity, timeout, fairness,
   owner-cleanup and unrelated-owner preservation execution;
3. an active-mission handshake still echoes the HELLO request (1000 ms) instead of announcing the
   configured mission interval (500 ms) in WELCOME;
4. the isolated allocation oracle fails at the same missing service/state seam, before claiming any
   allocation result.

The unchanged predecessor baseline remains green and separates these new failures from regression:

```powershell
& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06HandshakeContract.*:TelemetryWp06ReliabilityContract.*:TelemetryWp06BudgetContract.*' `
  '--gtest_brief=1' '--gtest_color=no'
# exit 0: 37/37 passed

& .\build\bin\Debug\telemetry_session_controller_allocation_contract_tests.exe `
  '--gtest_filter=-TelemetryWp06AllocationContract.Heartbeat*' `
  '--gtest_brief=1' '--gtest_color=no'
# exit 0: 2/2 passed
```

## Gate status

This evidence is intentionally RED. `P1-REQ-018`, `P1-REQ-019`, `D1-011`, `P1-AC-010` and the
heartbeat portion of `P1-WP-06` remain open until independently authored production code makes the
unchanged heartbeat and allocation oracles pass, predecessor tests remain green, Phase 0 freeze
evidence remains unchanged, and independent review accepts the focused production/test diff.

## Provisional GREEN verification before P1-REQ-013 adjudication

Independent production made the original ten heartbeat tests and isolated allocation oracle green
without weakening those tests. The following commands were executed before the subsequent atomic
ingress review added three new tests:

```powershell
cmake --build build --target unittests telemetry_session_controller_allocation_contract_tests `
  --config Debug --parallel 4

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06HeartbeatContract.*' '--gtest_brief=1' '--gtest_color=no'
# 10/10 passed

& .\build\bin\Debug\telemetry_session_controller_allocation_contract_tests.exe `
  '--gtest_filter=TelemetryWp06AllocationContract.Heartbeat*' '--gtest_brief=1' '--gtest_color=no'
# 1/1 passed

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06*:*TelemetryProtocolClock.*:*TelemetryProtocolRateLimiter.*:*TelemetryProtocolReliabilityHarness.*:*TelemetryProtocolReliabilityMessages.*:*TelemetryProtocolReliableReceive*.*:*TelemetryProtocolReliableWindow.*:*TelemetryProtocolPreallocatedReliableParity.*:*TelemetryProtocolSession.*:*TelemetryProtocolSessionContext.*' `
  '--gtest_brief=1' '--gtest_color=no'
# 189/189 passed across 17 suites

& .\build\bin\Debug\telemetry_session_controller_allocation_contract_tests.exe `
  '--gtest_brief=1' '--gtest_color=no'
# 3/3 passed

& .\build\bin\Debug\unittests.exe '--gtest_brief=1' '--gtest_color=no'
# 824/824 passed across 109 suites; 2 disabled
```

Deterministic repetition also passed: mission/idle cadence plus two-client fairness ran 200 times
each (600/600 executions), and the heartbeat allocation path ran 1,000/1,000 executions. Release
builds of both targets succeeded; the original heartbeat suite passed 10/10 and the full isolated
allocation executable passed 3/3 in Release.

These results prove the original RED gaps became green. They are provisional rather than a closed
gate because the following independent inspection found that rejected heartbeat input could still
refresh the long-timeout activity timestamp.

## Post-GREEN P1-REQ-013 atomic-ingress RED

Tracker adjudication confirmed that only a valid admitted Request or an exactly correlated Response
may refresh heartbeat network activity. Rate-limited Requests and unknown, mismatched, duplicate or
discarded/late Responses must be mutation-free across activity timestamps, probes, filter and output.
A valid admitted Request still refreshes activity when its Response encounters existing-output
backpressure and the older pending heartbeat is subsequently abandoned on `WouldBlock`.

Three dedicated tests were added after the provisional matrix, without production changes:

| Test | New atomic contract |
|---|---|
| `RateLimitedRequestCannotRefreshTimeoutOrMutateHeartbeatState` | after consuming the exact Request burst, the next rate-limited Request preserves network/clock activity, probes, samples and empty output |
| `OnlyExactlyCorrelatedResponseMayRefreshNetworkAndClockActivity` | unknown ID and matching ID with wrong `t0` preserve an invalid empty filter; only the exact live tuple refreshes network/clock activity and creates exact RTT=290 us, offset=-135 us evidence; duplicate and discarded/late Response preserve those timestamps and filter values completely |
| `AdmittedRequestRefreshesActivityBeforeBusyResponseAndWouldBlock` | a valid Request refreshes network activity but leaves `last_valid_clock_response_us` unchanged before its Response reports `OutputBusy`; the pre-existing heartbeat remains byte-identical and its later `WouldBlock` cleanup changes neither activity timestamp |

Measured Debug RED:

```powershell
cmake --build build --target unittests --config Debug --parallel 4

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06HeartbeatContract.RateLimitedRequestCannotRefreshTimeoutOrMutateHeartbeatState:TelemetryWp06HeartbeatContract.OnlyExactlyCorrelatedResponseMayRefreshNetworkAndClockActivity:TelemetryWp06HeartbeatContract.AdmittedRequestRefreshesActivityBeforeBusyResponseAndWouldBlock' `
  '--gtest_brief=1' '--gtest_color=no'
# exit 1: 0/3 passed, intentional RED

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06HeartbeatContract.*' '--gtest_brief=1' '--gtest_color=no'
# exit 1: 10/13 passed; exactly the three new tests failed
```

Observed mismatches match the inspected defect precisely:

- the rate-limited Request changed activity from `1019` to `2000`;
- unknown-ID and wrong-`t0` Responses changed activity from `201` to `1000300` then `1000400`;
- duplicate and discarded/late Responses advanced activity past the exact correlated value
  `1000500`, to `1000501` and `2000300`;
- the valid Request encountering existing output failed to refresh activity from `201` to `1201`.

The strengthened filter/clock invariants introduce no additional observed mismatch: the exact
Response produces a valid one-sample filter with minimum RTT `290 us` and smoothed offset `-135 us`;
unknown/mismatched Responses leave the initially invalid filter empty; duplicate and discarded/late
Responses preserve the exact filter, sample count, probes and `last_valid_clock_response_us`.
Likewise, the Request/backpressure path correctly leaves `last_valid_clock_response_us` unchanged
before and after `WouldBlock`. The remaining RED is therefore isolated to network-activity commit
ordering.

The prior allocation oracle is not affected and no allocation-specific RED was added. WP06-HB is
again open pending an independently authored production correction and rerun of the three-test RED,
the complete 13-test heartbeat suite, allocation, WP06/Phase 0 regression and stress evidence.

## P1-REQ-013 GREEN and final matrix before hostile-time expansion

The independently authored activity-order correction made all three atomic-ingress RED tests green
without removing or relaxing any prior assertion. A focused diff inspection confirmed that the test
changes since their RED capture only strengthened clock/filter invariants and later added new hostile
scenarios; no original expectation changed.

Fresh Debug results:

| Gate | Result |
|---|---:|
| P1-REQ-013 targeted activity tests | 3/3 |
| complete heartbeat suite before hostile-time expansion | 13/13 |
| isolated allocation executable | 3/3 |
| WP06 + composed Phase 0 clock/rate/reliability/session families | 192/192 |
| complete unit-test executable | 827/827, 2 disabled |
| targeted P1 activity + cadence + mission cadence + fairness, 100 repetitions | 600/600 |
| heartbeat allocation path, 500 repetitions | 500/500 |

Release builds of `unittests` and
`telemetry_session_controller_allocation_contract_tests` succeeded after the production correction.
Release then passed the targeted activity tests 3/3, heartbeat 13/13, allocation 3/3 and the same
WP06/Phase 0 families 192/192.

The RED-to-GREEN activity transition is exact:

- rate-limited Request: activity remains `1019` instead of advancing to `2000`;
- unknown-ID/wrong-`t0`, duplicate and discarded/late Responses preserve network activity;
- the exact correlated Response alone refreshes network and clock activity and preserves exact
  RTT/offset evidence;
- a valid admitted Request refreshes network activity to `1201` under `OutputBusy`, while
  `last_valid_clock_response_us` stays unchanged through `WouldBlock`.

## Hostile correlated-timestamp TDD expansion

Tracker adjudication then froze the remaining integration matrix for hostile timestamps. Four new
controller tests were added without production changes:

| Test | Normative outcome |
|---|---|
| `PayloadInvalidResponseIsRejectedBeforeProbeCorrelation` | wire-invalid `t2<t1` is rejected before correlation; probe, network/clock activity and empty filter remain unchanged |
| `NegativeRoundTripConsumesProbeAndPreservesPriorClockEvidence` | exact tuple with negative RTT consumes the probe and refreshes network only; prior valid filter and clock timestamp remain exact |
| `OffsetOverflowConsumesProbeAndPurgesPriorClockFilter` | exact tuple with out-of-range offset consumes and refreshes network, does not refresh clock, and invalidates/purges prior filter evidence |
| `InitiatorTimeReversalConsumesProbeInvalidatesClockAndMarksStale` | exact tuple with `t3<t0` consumes without unsigned underflow, refreshes network only, invalidates filter and marks `Stale`; its duplicate cannot refresh again, restore readiness or clear `clock_stale` |

Measured current Debug state:

```powershell
cmake --build build --target unittests --config Debug --parallel 4

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06HeartbeatContract.PayloadInvalidResponseIsRejectedBeforeProbeCorrelation:TelemetryWp06HeartbeatContract.NegativeRoundTripConsumesProbeAndPreservesPriorClockEvidence:TelemetryWp06HeartbeatContract.OffsetOverflowConsumesProbeAndPurgesPriorClockFilter:TelemetryWp06HeartbeatContract.InitiatorTimeReversalConsumesProbeInvalidatesClockAndMarksStale' `
  '--gtest_brief=1' '--gtest_color=no'
# exit 1: 2/4 passed

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06HeartbeatContract.*' '--gtest_brief=1' '--gtest_color=no'
# exit 1: 15/17 passed
```

The payload-invalid pre-correlation and negative-RTT outcomes are already GREEN. Two new RED gaps
remain precise:

- offset overflow consumes the probe and preserves clock activity as required, but leaves the prior
  filter valid with one sample instead of purging it;
- initiator time reversal consumes and refreshes network without underflow, and its duplicate does
  not refresh again, but the prior filter remains valid with one sample and session progress remains
  `ReadyForState` instead of `Stale`; the duplicate-specific persistence checks consequently also
  observe `ReadyForState` and `clock_stale=false` instead of preserving `Stale/true`.

Therefore the 13-test P1-REQ-013 activity correction is verified GREEN in both configurations, but
WP06-HB is open again on these two hostile-time invalidation/state-transition gaps. Release and broad
matrices above predate only the four additive hostile-time tests; no GREEN is claimed for those new
RED cases.

## Final hostile-time GREEN and independent verification

Independent production changes closed the two remaining hostile-time gaps. All 17 heartbeat tests
were kept unchanged throughout that correction and this final verification; no assertion, test
registration or CMake entry was relaxed to obtain GREEN.

The final RED-to-GREEN hostile matrix is:

| Hostile case | Final independently observed outcome |
|---|---|
| payload-invalid `t2<t1` | remained GREEN: rejected before probe correlation with no state mutation |
| negative RTT | remained GREEN: exact probe consumed, network refreshed, prior clock evidence preserved |
| offset overflow | became GREEN: exact probe consumed, network refreshed, prior clock filter invalidated and purged |
| initiator time reversal `t3<t0` | became GREEN: exact probe consumed without underflow, clock invalidated, `clock_stale=true`, progress set to `Stale`; duplicate preserves that state |

Both configurations were rebuilt from the final shared tree:

```powershell
cmake --build build --target unittests telemetry_session_controller_allocation_contract_tests `
  --config Debug --parallel 4
# exit 0

cmake --build build --target unittests telemetry_session_controller_allocation_contract_tests `
  --config Release --parallel 4
# exit 0
```

The exact final verification matrix is:

| Gate | Debug | Release |
|---|---:|---:|
| four hostile correlated-timestamp tests | 4/4 | 4/4 |
| complete heartbeat contract suite | 17/17 | 17/17 |
| isolated allocation executable | 3/3 | 3/3 |
| WP06 + composed Phase 0 clock/rate/reliability/session families | 196/196 across 17 suites | 196/196 across 17 suites |
| complete unit-test executable | 831/831 across 109 suites, 2 disabled | 831/831 across 109 suites, 2 disabled |
| hostile + P1 activity + cadence/mission/fairness stress | 1,000/1,000 (10 tests x 100) | 500/500 (10 tests x 50) |
| isolated allocation stress | 1,500/1,500 (3 tests x 500) | 750/750 (3 tests x 250) |

The focused P1 activity tests are included in both the 17-test suite and the repeated sensitive
matrix. They preserve the earlier adjudicated ordering: a rate-limited Request cannot refresh
activity; only an exactly correlated Response may refresh response-side activity; and an admitted
Request refreshes activity before an `OutputBusy` response path, with later `WouldBlock` cleanup
remaining mutation-free.

Representative final commands were:

```powershell
& .\build\bin\<Config>\unittests.exe `
  '--gtest_filter=TelemetryWp06HeartbeatContract.*' `
  '--gtest_brief=1' '--gtest_color=no'

& .\build\bin\<Config>\unittests.exe `
  '--gtest_filter=TelemetryWp06*:*TelemetryProtocolClock.*:*TelemetryProtocolRateLimiter.*:*TelemetryProtocolReliabilityHarness.*:*TelemetryProtocolReliabilityMessages.*:*TelemetryProtocolReliableReceive*.*:*TelemetryProtocolReliableWindow.*:*TelemetryProtocolPreallocatedReliableParity.*:*TelemetryProtocolSession.*:*TelemetryProtocolSessionContext.*' `
  '--gtest_brief=1' '--gtest_color=no'

& .\build\bin\<Config>\unittests.exe '--gtest_brief=1' '--gtest_color=no'

& .\build\bin\<Config>\telemetry_session_controller_allocation_contract_tests.exe `
  '--gtest_brief=1' '--gtest_color=no'
```

No runtime failure, flaky repetition or Phase 0 regression was observed in this final pass. The
expected diagnostics printed by unrelated negative parser/string tests in the complete executable
did not produce test failures. WP06-HB test evidence is final GREEN; overall gate closure remains
subject to the independent reviewer and contract tracker responsibilities required by the phase
implementation workflow.
