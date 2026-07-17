# WP06-REL test-first evidence

## Baseline and scope

The RED suite is based on commit `617f65db3` (`Implement telemetry Phase 1 WP06 handshake`). Remote
GitHub Actions run `29543111611` completed successfully before these tests were written. This
work package owns only the remaining producer-side WP06 reliability composition. It does not add
heartbeat probes, offset filtering, heartbeat cadence, `Stale` timers or long-session timeout
behavior. The only timeout interaction in scope is applying the terminal policy emitted by the
Phase 0 reliable window at its immutable retention deadline.

No production file was changed for this RED capture. Tests were added to the existing WP06
controller and isolated allocation executables so they reuse the real handshake fixture, real wire
codecs, the fixed preallocated window and the global allocation interceptor.

## RED inventory

The initial bounded sub-lot covers the planned REL contracts as follows:

| Plan IDs | Test | Contract |
|---|---|---|
| `REL-001`, `REL-006` | `SessionBeginAckLifecycleIsDuplicateSafeAndMutationAtomic` | on one controller accept VALIDATED, accept its exact duplicate without release, suppress the due retry, ignore a forged tuple atomically, release on exact APPLIED and ignore that ACK once late |
| `REL-003`, `REL-006`, `REL-007` | `NackDecisionsComposeWithoutRequiringAnImmediateRetransmission` | compose selective/full/wait/incoherent NACK decisions; ResourceLimit must be accepted without immediate output, preserve the original RTO at `due-1`/`due`, retransmit the exact logical identity and retain the immutable deadline |
| `REL-006`, `REL-007` | `SessionBeginTerminalNacksExposeStaleReasonAndUnsupportedIsAtomic` | accept StaleBaseline/SemanticValidationFailed/DeadlineExpired as terminal, release the tuple and expose `Stale` plus exact `MarkSessionStale`; reject known-tuple UnsupportedMessage without mutation |
| `REL-009` | `QueuedOutputIsPurgedOnlyWhenItsOwningSlotCloses` | with two slots, preserve client A's queued retry byte-identically when B closes, then purge it when owner A closes |
| `REL-004`, `REL-009` | `WelcomeWouldBlockAtExactDeadlinePurgesOutputAndClosesOwner` | an original WELCOME retained transactionally by WouldBlock must be purged at its exact deadline before CloseSession reclaims the owner and window |
| `REL-004`, `REL-006`, `REL-009` | `SessionBeginRetryWouldBlockAtDeadlinePurgesWithoutSequenceConsumption` | a SESSION_BEGIN retry retained by WouldBlock must be preempted at the exact deadline, purge output, release the window and expose `Stale`/`MarkSessionStale` without committing its packet sequence |
| `REL-004`, `REL-008`, `REL-009` | `CrossOwnerBlockedRetrySurvivesWelcomeOwnerDeadlineByteIdentically` | a blocked retry owned by B must not prevent A's WELCOME CloseSession deadline and must survive A cleanup byte-identically with B state and sequence unchanged |
| `REL-004`, `REL-006`, `REL-008`, `REL-009` | `CrossOwnerBlockedRetrySurvivesSessionBeginStaleDeadlineByteIdentically` | a blocked retry owned by B must not prevent A's SESSION_BEGIN MarkSessionStale deadline; both packet sequences remain uncommitted and no replacement retry is queued |
| `REL-002`–`REL-005` | `ServiceRtoDeadlineAndWouldBlockAreBoundedAndTransactional` | require noexcept service/peek/complete seams; exact RTO boundary, immutable deadline, retransmission wire identity and transactional `WouldBlock` |
| `REL-008` | `DueReliableClientsMakeProgressOneDatagramPerServiceCall` | bounded one-datagram service and progress for two simultaneously due clients |
| `REL-010`, `REL-011` | `ReliabilityServiceAndWouldBlockAllocateNothingAfterReady` | exercise RTO service plus `WouldBlock`/`Complete` with the post-Ready allocation interceptor armed |

The compile-safe seam contract requires these exact controller operations:

```cpp
controller.service_reliability(now_us);       // noexcept, at most one action
controller.peek_output(output);               // noexcept, non-destructive
controller.complete_output(IoStatus::...);     // noexcept, WouldBlock retains; Complete consumes
```

The behavioral oracle does not prescribe a new result enum. Observable state, pending bytes,
retained-window usage, endpoint, decoded datagram and session ownership provide the evidence.
Terminal SESSION_BEGIN policy is observable without introducing any wire value: the slot must
publish `ProducerSessionProgress::Stale`, `has_reliability_terminal_policy == true` and
`reliability_terminal_policy == ReliableTerminalPolicy::MarkSessionStale`. UnsupportedMessage
must leave that terminal marker absent. These are reliability outcomes only; the tests do not
start a heartbeat timer or implement WP06-HB.

## Measured RED

Both test targets build successfully in Debug. After independent review strengthened ACK lifecycle,
terminal-state and output-ownership coverage, the focused controller filter runs six tests and
fails all six against `617f65db3`:

1. SESSION_BEGIN VALIDATED and its exact duplicate are rejected as `WelcomeProofMismatch`; the
   missing service seam also prevents proof that validation suppresses the due retry before exact
   APPLIED releases the tuple. The same sequence locks forged and late ACK mutation atomicity.
2. `ResourceLimit` remains rejected as `PayloadInvalid` with a dropped disposition; the missing
   service seam also prevents the strengthened proof that `due-1` has no output, `due` emits the
   exact retained identity, and the original retention deadline remains immutable. MissingFragments,
   BadMessageCrc and BadFragmentLayout already queue their required retry, while known-tuple
   UnsupportedMessage is correctly rejected incoherently.
3. `StaleBaseline`, `SemanticValidationFailed` and `DeadlineExpired` are rejected as
   `PayloadInvalid`, leave the SESSION_BEGIN retained and expose neither `Stale` progress nor its
   exact `MarkSessionStale` reason. The adjacent UnsupportedMessage branch remains mutation-free.
4. the missing transactional peek seam blocks the two-slot proof that closing B preserves A's
   queued retry byte-identically before closing A purges it.
5. the noexcept `service_reliability`, `peek_output` and `complete_output` seams are absent, so no
   autonomous RTO/deadline or transactional `WouldBlock` proof can execute;
6. the same missing service seam blocks the bounded two-client progress proof.

The isolated allocation executable builds and its new REL test fails RED because the same three
seams are absent. Its original HELLO/ACK/NACK allocation contract remains green and still observes
zero allocations after Ready.

Commands and measured totals:

```powershell
cmake --build build --target unittests telemetry_session_controller_allocation_contract_tests --config Debug --parallel 4
& build/bin/Debug/unittests.exe --gtest_filter='TelemetryWp06ReliabilityContract.*' --gtest_brief=1
# 0/6 passed: six intentional RED tests

& build/bin/Debug/telemetry_session_controller_allocation_contract_tests.exe `
  --gtest_filter='TelemetryWp06AllocationContract.ReliabilityServiceAndWouldBlockAllocateNothingAfterReady'
# 0/1 passed: intentional missing-seam RED
```

The unchanged local baseline remains green: 70/70 tests across WP06 handshake/budget/ingress/
security, fixed-window parity and the general Phase 0 reliable window, plus 1/1 original isolated
allocation contract. This separates the new REL gaps from regressions in the approved WP06-HS and
Phase 0 behavior.

## Dependency and boundary locks

- Frozen ACK/NACK/datagram wire formats and the general Phase 0 reliable-window oracle are reused,
  never refreshed.
- The controller must preserve its fixed preallocated per-client window and zero post-Ready
  allocation claim.
- `IoStatus` comes from the already implemented WP04 transport seam; `WouldBlock` consumes no
  retained action and cannot spin.
- A terminal reliable policy may mark or close the owning session, but heartbeat sampling, clock
  filtering, heartbeat-driven `Stale` and long-timeout cleanup remain WP06-HB.
- Transactions, baselines and snapshots remain WP08; metrics/export remain WP09.

This evidence is intentionally RED. It must not be marked GREEN until production is implemented
and the focused tests, allocation target, WP06-HS, Phase 0 reliability/parity, `Telemetry*`, freeze
and Debug/Release matrices pass without weakening these oracles.

## Final independent GREEN validation

Production was implemented only after the final ResourceLimit RED capture. The test owner retained
exclusive ownership of both controller test files and this evidence file; the six REL tests and two
allocation tests were not weakened or edited after that final RED. The production diff is limited
to `code/telemetry/session_controller.h` and `.cpp`; the test diff remains the test-first oracle
described above.

Fresh Debug and Release builds of `unittests` and
`telemetry_session_controller_allocation_contract_tests` succeeded. The independent local matrix is:

| Gate | Debug | Release |
|---|---:|---:|
| `TelemetryWp06ReliabilityContract.*` | 6/6 | 6/6 |
| all `TelemetryWp06*` + fixed-window parity + general Phase 0 reliable window | 76/76 | 76/76 |
| `TelemetryWp06BudgetContract.*` | 5/5 | 5/5 |
| `Telemetry*` | 583/583, 2 disabled | 583/583, 2 disabled |
| complete unit-test executable | 810/810, 2 disabled | 810/810, 2 disabled |
| isolated allocation executable | 2/2 | 2/2 |
| six REL tests repeated 50 times | 300/300 | 300/300 |
| two isolated allocation tests repeated 1,000 times | 2,000/2,000 | 2,000/2,000 |

The strengthened RED behaviors are now GREEN without alternate expectations:

1. one controller accepts SESSION_BEGIN VALIDATED and its exact duplicate, exposes no retry at the
   original RTO, rejects a forged tuple without observable mutation, releases on exact APPLIED and
   treats the same ACK as late without reopening state;
2. ResourceLimit is accepted without immediate output, leaves `due-1` empty, retransmits the exact
   retained type/message/fragment/CRC/payload identity at `due`, changes only packet sequence and
   retransmission flag, and does not extend the immutable retention deadline;
3. StaleBaseline, SemanticValidationFailed and DeadlineExpired release SESSION_BEGIN and expose
   `ProducerSessionProgress::Stale`, `has_reliability_terminal_policy` and exact
   `ReliableTerminalPolicy::MarkSessionStale`; known-tuple UnsupportedMessage remains incoherent and
   mutation-free;
4. closing unrelated client B preserves client A's queued bytes and endpoint exactly; closing owner
   A purges that output;
5. `WouldBlock` preserves the queued datagram transactionally, `Complete` consumes it, autonomous
   service performs at most one action, and two simultaneously due clients both make progress;
6. the complete RTO/service/peek/WouldBlock/Complete path performs zero allocations after Ready.

Phase 0 compatibility was independently rechecked. The 15 anti-bypass freeze tests passed, and the
standalone verifier accepted 438 frozen artifacts at tree SHA-256
`9baac6a20db33bcf350066ed533c5581b7117410899d7bc4a6dc24406e47856d`.
`git diff --check` reports no whitespace defect, only the expected Windows LF-to-CRLF notices.

Static diff and symbol scans confirm the implementation boundary:

- the only dynamic allocations in `SessionController` remain its pre-Ready nothrow rate-limiter,
  slot, fixed reliable-window and reassembly allocations; no vector-backed send window was added;
- the only heartbeat references remain the pre-existing configured intervals and WELCOME field;
  there is no heartbeat cadence, probe, offset/minimum-RTT filter, missed-heartbeat timer or long
  session timeout in this REL lot;
- no transaction, manifest, baseline, snapshot, event, metric/export, worker, mutex or thread path
  was added; WP08, WP09 and the functional-thread boundary remain untouched;
- the five exact WP06 storage/budget oracles pass in both configurations, and the isolated global
  allocation interceptor passes 2,000 repeated executions per configuration.

On this evidence WP06-REL is GREEN. This closes only the reliability-composition sub-lot; WP06-HB
and all WP08/WP09 work remain separate gates.

## Post-GREEN WouldBlock/deadline review RED

An independent follow-up review found that the preceding GREEN did not compose an already queued,
transactionally blocked output with the retained tuple's exact deadline. Two new test-first oracles
were added without production edits. The Debug target builds, then this focused command executes
two tests and fails both:

```powershell
& build/bin/Debug/unittests.exe `
  --gtest_filter='TelemetryWp06ReliabilityContract.*WouldBlockAt*' `
  --gtest_brief=1 --gtest_color=no
# 0/2 passed; exit 1
```

Measured RED:

1. a WELCOME is peeked, completed as WouldBlock and remains byte-identical, but
   `service_reliability(first_send + ReliableOrdinaryRetentionUs)` returns early because output is
   pending. The expired WELCOME remains sendable, the owner slot remains active, the retained entry
   remains allocated and `output_queued` stays true. Required behavior is to purge that owned output,
   consume the terminal CloseSession action and reclaim the slot/window at the exact boundary;
2. a SESSION_BEGIN MissingFragments retry is likewise peeked and retained byte-identically by
   WouldBlock. Exact-deadline service leaves the retry sendable and the entry retained, with progress
   still ReadyForState and no terminal marker. Required behavior is to purge the pending retry,
   release the entry and retain the established slot as `Stale` with exact `MarkSessionStale` policy.
   The pending retry packet sequence is not committed before or during this failed service path;
   the strengthened oracle freezes that no-consumption rule for the corrected path as well.

These tests forbid a concurrent replacement retry at the deadline: a pending output may remain
byte-identical only while its tuple is live. Terminal cleanup owns the exact deadline and must clear
the output transaction before applying the class policy. This is REL cleanup/transactional-egress
composition only; it adds no heartbeat probe, timer, offset filter, WP08 state or new wire value.

The prior 6/6 functional result remains historical evidence for the original REL inventory, but
WP06-REL is RED again until these two unchanged oracles pass along with the complete Debug/Release,
allocation, stress, Phase 0 freeze and boundary matrices.

## Final WouldBlock/deadline GREEN

Production was corrected without editing the two follow-up RED tests. Exact-deadline reliability
service now preempts a pending output owned by the expiring tuple: it clears the transactional
egress bookkeeping before consuming the terminal action, so no expired bytes remain sendable and
no pending retry packet sequence is committed.

Fresh Debug and Release builds of both test targets succeeded. Independent results are identical in
both configurations:

| Gate | Debug | Release |
|---|---:|---:|
| follow-up `*WouldBlockAt*` tests | 2/2 | 2/2 |
| complete `TelemetryWp06ReliabilityContract.*` | 8/8 | 8/8 |
| all WP06 + fixed-window parity + general Phase 0 reliable window | 78/78 | 78/78 |
| isolated allocation executable | 2/2 | 2/2 |
| all `Telemetry*` | 585/585, 2 disabled | 585/585, 2 disabled |
| eight REL tests repeated 50 times | 400/400 | 400/400 |
| two allocation tests repeated 1,000 times | 2,000/2,000 | 2,000/2,000 |

The original WELCOME path now purges its blocked output, closes the owner and releases the retained
window at the exact ordinary deadline. The established SESSION_BEGIN path purges its blocked retry,
releases the entry, preserves the uncommitted packet sequence, keeps the slot active and exposes
exact `Stale` plus `MarkSessionStale`. No replacement retry is queued at either boundary.

`git diff --check` remains free of whitespace defects apart from expected Windows line-ending
notices. The final status is limited to the two controller production files, the two test-first
files and this evidence. No heartbeat, WP08 or WP09 oracle or implementation was introduced by the
follow-up correction. On this final evidence WP06-REL is GREEN again.

## Post-GREEN cross-owner deadline review RED

A further independent review exercised a terminal deadline for client A while a reliable retry
owned by client B was already pending and retained transactionally by WouldBlock. Two new tests were
added without production edits. The Debug target builds, then this command fails both tests:

```powershell
& build/bin/Debug/unittests.exe `
  --gtest_filter='TelemetryWp06ReliabilityContract.CrossOwner*' `
  --gtest_brief=1 --gtest_color=no
# 0/2 passed; exit 1
```

Measured behavior isolates the gap precisely. Production preserves B's endpoint and bytes exactly,
leaves B ReadyForState with one retained item, and advances neither B's pending retry sequence nor
A's sequence. It also queues no replacement retry. However, service examines only the pending
output owner B; because B has no terminal action yet, it returns without scanning A:

1. at A's exact WELCOME deadline, A remains AwaitWelcomeApplied with one retained item and both
   slots active instead of consuming CloseSession, resetting A and releasing its window;
2. at A's exact SESSION_BEGIN deadline, A remains ReadyForState with one retained item and no
   terminal marker instead of releasing the entry and exposing exact Stale/MarkSessionStale.

The required correction must consume exactly one terminal action for A while leaving the unrelated
B output transaction byte-identical and fully owned by B. It must not complete, replace or purge B's
retry, advance either live packet sequence, or queue a concurrent retry. CloseSession naturally
resets A's physical slot; the WELCOME oracle therefore checks the default reset sequence rather than
mistaking slot reclamation for a packet commit. The SESSION_BEGIN oracle retains A and checks its
sequence for exact equality.

This is bounded REL scheduler/ownership composition. It introduces no heartbeat timing, WP08 state,
WP09 observability or wire change. The preceding 8/8 result remains historical, but WP06-REL is RED
again until these two unchanged cross-owner tests and the full validation matrix pass.

## Final cross-owner deadline GREEN

Production was corrected without editing the two cross-owner RED tests. Reliability service now
searches for one due terminal action across slots even while an unrelated output transaction is
pending. It applies exactly A's terminal policy without completing, replacing, clearing or otherwise
mutating the B-owned output.

Fresh Debug and Release builds of both test targets succeeded. The final independent matrix is:

| Gate | Debug | Release |
|---|---:|---:|
| cross-owner follow-up tests | 2/2 | 2/2 |
| complete `TelemetryWp06ReliabilityContract.*` | 10/10 | 10/10 |
| all WP06 + fixed-window parity + general Phase 0 reliable window | 80/80 | 80/80 |
| isolated allocation executable | 2/2 | 2/2 |
| all `Telemetry*` | 587/587, 2 disabled | 587/587, 2 disabled |
| ten REL tests repeated 50 times | 500/500 | 500/500 |
| two allocation tests repeated 1,000 times | 2,000/2,000 | 2,000/2,000 |

At A's WELCOME deadline, A is closed and its fixed window reclaimed while B remains ReadyForState
with one retained item and the same session, packet sequence, endpoint and byte-for-byte output. At
A's SESSION_BEGIN deadline, A remains allocated but becomes exact Stale/MarkSessionStale with its
entry released and sequence unchanged; B is again completely unchanged. Neither path queues a
replacement retry or commits the pending B sequence.

`git diff --check` remains clean apart from expected Windows line-ending notices. No test-generated
artifact remains in the status, which is limited to the two controller production files, two
test-first files and this evidence. The correction remains within bounded REL scheduler/ownership
composition and adds no heartbeat, WP08, WP09 or wire behavior. On this final evidence WP06-REL is
GREEN.

## Final reviewer disposition

The final independent reviewer verdict is **APPROVE** with no P0, P1 or P2 finding. The reviewer's
focused local matrix passed 48/48 tests, and the isolated allocation executable passed 2/2. No REL
oracle was weakened and no Phase 0 wire or reliability contract was changed.

This approval closes WP06-REL only. WP06-HB and native runtime/transport integration remain open,
separately owned gates and are not implied complete by this disposition.

## Post-review P1-REQ-013 NACK activity-order RED

A subsequent P1-REQ-013 audit found that the controller-side NACK path refreshed
`last_valid_network_activity_us` immediately after payload decoding, before the shared ACK/NACK
bucket and before exact correlation with the retained reliable tuple. Three additive reliability
tests were authored without production changes:

| Test | Oracle |
|---|---|
| `RejectedForgedIncoherentAndUncorrelatedNacksAreActivityAtomic` | wrong target identity and incoherent NACK input preserve activity, retained window, output, progress, sequence state and the original retry boundary |
| `RateLimitedAndLateDuplicateNacksCannotExtendNetworkTimeout` | a NACK denied after exact exhaustion of the common ACK/NACK bucket and a duplicate terminal NACK after tuple release preserve activity, window/output and progress |
| `FullyAdmittedNacksRefreshActivityForRetryWaitAndTerminalOutcomes` | exact admitted `MissingFragments`, `ResourceLimit` and `StaleBaseline` NACKs refresh activity only after bucket admission and correlation, including queued retry, scheduled-wait and terminal outcomes |

The existing `SessionBeginAckLifecycleIsDuplicateSafeAndMutationAtomic` oracle was strengthened,
not relaxed: exact VALIDATED/APPLIED and the still-admitted duplicate VALIDATED ACK refresh
activity, while forged and late-after-release ACKs preserve it. This ACK analogue remains GREEN.

Debug compilation succeeds, then the focused RED is reproducible:

```powershell
cmake --build build --target unittests --config Debug --parallel 4
# exit 0

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06ReliabilityContract.RejectedForgedIncoherentAndUncorrelatedNacksAreActivityAtomic:TelemetryWp06ReliabilityContract.RateLimitedAndLateDuplicateNacksCannotExtendNetworkTimeout:TelemetryWp06ReliabilityContract.FullyAdmittedNacksRefreshActivityForRetryWaitAndTerminalOutcomes:TelemetryWp06ReliabilityContract.SessionBeginAckLifecycleIsDuplicateSafeAndMutationAtomic' `
  '--gtest_brief=1' '--gtest_color=no'
# exit 1: 2/4 passed

& .\build\bin\Debug\unittests.exe `
  '--gtest_filter=TelemetryWp06ReliabilityContract.*' `
  '--gtest_brief=1' '--gtest_color=no'
# exit 1: 11/13 passed
```

The measured mismatches are limited to premature activity refresh:

1. a decoded `MissingFragments` NACK with `target_message_id + 1` advances activity from `2000` to
   `3000` despite being non-correlated; its retained item, empty output, Ready progress, sequence
   state and exact original retry schedule remain unchanged;
2. after the WELCOME proof, activity is `2000`. The 199 non-correlated burner NACKs that consume the
   remaining common ACK/NACK burst tokens incorrectly advance it to `3000`; the next rate-limited
   NACK then advances it again to `4000`. The oracle never adopts either mutation: it requires the
   original `2000` after the burner loop and again after the denied NACK. The retained window,
   output and progress remain unchanged throughout;
3. an exact terminal `StaleBaseline` NACK correctly refreshes activity to `3000`, releases the
   tuple and marks Stale, but the same NACK once duplicate/late and non-correlated advances activity
   to `4000`; the released window, empty output and Stale progress remain unchanged.

The wire-incoherent `UnsupportedMessage` form is already rejected before activity mutation. All
three fully admitted NACK outcomes are GREEN, as is the strengthened ACK analogue. The required
production correction is therefore narrowly ordered: decode, rate-limit, correlate/decide, then
commit network activity only for an admitted reliable response.

The rate-limited subcase additionally freezes the complete observable reliable state. At the
original jittered RTO, `due-1` produces no output and `due` produces the exact retained tuple:
session, message type, message ID, fragment count, message CRC and logical payload all match the
original SESSION_BEGIN; only packet sequence and the retransmission flag change as specified. Any
later retry already due at `retention_deadline-1` retains that same tuple and payload. At the exact
immutable ordinary retention deadline the entry is released with no sendable output and the slot
becomes exact `Stale`/`MarkSessionStale`. Thus the rejected rate-limited NACK changes only the
premature activity timestamp after earlier burner NACKs have already exposed the same ordering bug;
neither the burners nor the denied NACK reset/postpone the RTO, change bytes or identity, consume
the retained entry, or extend its deadline.

Fresh baseline results from the same Debug executable are:

| Gate | Result |
|---|---:|
| ten predecessor REL tests, including strengthened ACK lifecycle | 10/10 |
| heartbeat contract suite | 17/17 |
| WP06 + composed Phase 0 clock/rate/reliability/session families | 197/199 across 17 suites; exactly the two new rejection tests fail |
| isolated allocation executable | 3/3 |

WP06-REL is RED again only for this P1-REQ-013 NACK activity-order finding. No Phase 0 wire value,
heartbeat oracle, allocation oracle, CMake registration or production file was changed by this
test-first expansion. Production handoff and independent review are required before any GREEN is
claimed.

## Final P1-REQ-013 NACK activity-order GREEN

Independent production changes closed the activity-order finding without editing or relaxing the
test oracle. The contract source hash was
`8f69f85ab18e341ca0f462dde1cd66b397d01bf4` before the final builds and remained exactly that value
after both Debug and Release verification.

The final observed ordering is now exact:

- all 199 non-correlated burner NACKs preserve the original network activity value `2000` while
  consuming only their admitted rate-limit tokens;
- the following rate-limited NACK also preserves `2000` and cannot change the retained tuple,
  output, progress, sequences, RTO or immutable deadline;
- a wrong-target NACK and a duplicate/late terminal NACK preserve network activity and all reliable
  state owned by their respective pre-rejection snapshots;
- exact admitted `MissingFragments`, `ResourceLimit` and `StaleBaseline` NACKs refresh activity at
  their commit point and retain their previously frozen retry/wait/terminal outcomes;
- the strengthened ACK analogue remains GREEN: exact and still-admitted duplicate ACKs refresh,
  while forged and late-after-release ACKs are activity-atomic.

Both final configurations built successfully:

```powershell
cmake --build build --target unittests telemetry_session_controller_allocation_contract_tests `
  --config Debug --parallel 4
# exit 0

cmake --build build --target unittests telemetry_session_controller_allocation_contract_tests `
  --config Release --parallel 4
# exit 0
```

The independent final matrix is:

| Gate | Debug | Release |
|---|---:|---:|
| focused P1-REQ-013 NACK/ACK tests | 4/4 | 4/4 |
| complete reliability contract suite | 13/13 | 13/13 |
| complete heartbeat contract suite | 17/17 | 17/17 |
| WP06 + composed Phase 0 clock/rate/reliability/session families | 199/199 across 17 suites | 199/199 across 17 suites |
| isolated allocation executable | 3/3 | 3/3 |
| complete unit-test executable | 834/834 across 109 suites, 2 disabled | 834/834 across 109 suites, 2 disabled |
| complete REL suite repeated | 1,300/1,300 (13 tests x 100) | 650/650 (13 tests x 50) |
| isolated allocation suite repeated | 1,500/1,500 (3 tests x 500) | 750/750 (3 tests x 250) |

The complete executables emitted only the expected diagnostics from unrelated negative
string/parser tests; every gtest result passed. No flaky repetition, allocation regression,
heartbeat regression or Phase 0 regression was observed. On this evidence the post-review
P1-REQ-013 NACK finding is GREEN; final WP06-REL gate disposition still belongs to the independent
reviewer and contract tracker roles.
