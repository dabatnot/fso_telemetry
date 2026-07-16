# Phase 1 WP03 producer and session identity test-first evidence

Status: **identity tranche GREEN after preserved RED, including native backend and OS entropy; configuration remains GREEN**.

This evidence covers the producer-profile and process-lifetime session-ID slice
of `P1-WP-03`. It does not claim checked startup-budget allocation, socket
rollback, or the complete first-`EngineUpdate` startup diagnostic gate.

## Frozen internal contract

The deterministic tests use producer-internal APIs declared by
`telemetry/identity.h`; they do not widen the engine-facing
`telemetry/telemetry.h` API.

- `RandomSource::next_u64` is injectable and `noexcept`.
- `ProducerProfileStore` exposes a tri-state initial/final read and the exact
  same-directory persistence sequence: exclusive temporary creation, write,
  durable flush, close, atomic install-if-absent, cleanup, then systematic
  reread.
- `parse_producer_profile_json` and
  `load_or_create_producer_identity` return a nonzero ID only on complete
  success; an existing invalid profile is never overwritten.
- `SessionIdAllocator` owns a fixed-capacity process-lifetime set. It checks the
  65,536-entry capacity before entropy, treats zero or RNG failure as immediate
  entropy failure, and permits at most 16 collision draws without eviction.

The persistence seam deliberately distinguishes `Published`,
`DestinationExists`, and `Error`. This freezes the race-safe behavior: a losing
creator cleans its temporary, rereads the winner, validates it, and never blindly
replaces the destination.

## Test-ID inventory

| Test ID | GTest oracle | Contract evidence |
|---|---|---|
| `WP03-ID-001` | canonical profile accepts decimal `1` and `UINT64_MAX` | `P1-REQ-009` |
| `WP03-ID-002` | closed strict JSON, duplicate/EOF/1 KiB/depth-two bounds | `P1-REQ-009`, profile part of `P1-AC-004` |
| `WP03-ID-003` | reject non-string, zero, sign, whitespace, leading zero, fractional and overflow IDs | `P1-REQ-009` |
| `WP03-ID-004` | valid existing profile performs no entropy or write | `P1-REQ-009` |
| `WP03-ID-005` | invalid existing profile is never overwritten or regenerated | `P1-REQ-009`, fail-closed part of `P1-AC-004` |
| `WP03-ID-006` | initial read error stops before entropy/temp creation | `P1-REQ-009`, `P1-AC-004` |
| `WP03-ID-007` | producer entropy failure and zero each stop after one draw | `P1-REQ-009` |
| `WP03-ID-008` | absent profile follows exclusive write/flush/close/atomic-install/reread and canonical serialization | `P1-REQ-009` |
| `WP03-ID-009` | every temporary persistence failure fails closed and cleans up | `P1-REQ-009`, `P1-AC-004` |
| `WP03-ID-010` | a published profile is reread, valid, and equal to the generated identity | `P1-REQ-009` |
| `WP03-ID-011` | atomic destination race never clobbers and validates the winner | `P1-REQ-009` |
| `WP03-ID-012` | normal/portable modes select only writable user/game installation roots | `P1-REQ-009` |
| `WP03-ID-013` | session entropy failure and zero stop immediately without retry | `P1-REQ-009` |
| `WP03-ID-014` | 15 collisions then unique draw 16 succeeds | `P1-REQ-009` |
| `WP03-ID-015` | 16 collisions stop at retry limit and never draw 17 | `P1-REQ-009` |
| `WP03-ID-016` | RNG failure during collision retries is distinct and preserves the used set | `P1-REQ-009` |
| `WP03-ID-017` | capacity 65,536 is checked before RNG with no eviction or reuse | `P1-REQ-009`, process slice of `P1-AC-016` |
| `WP03-ID-018` | truncated profile reaching depth three is rejected before recursive Jansson | `P1-REQ-009`, `P1-AC-011` |
| `WP03-ID-019` | brackets/braces/escaped quote and escaped backslash inside producer ID do not increment depth | `P1-REQ-009` |
| `WP03-ID-020` | opt-in isolated 900-container profile bomb is rejected by lexical preflight | `P1-REQ-009`, `P1-AC-011` |
| `WP03-ID-021` | exact-path native store durably flushes, atomically publishes canonical bytes, reopens and leaves no temp | `P1-REQ-009`, `P1-AC-004` |
| `WP03-ID-022` | native race creates temp in the destination directory and never clobbers the concurrent winner | `P1-REQ-009` |
| `WP03-ID-023` | native-store destructor removes an abandoned same-directory temporary | `P1-REQ-009`, cleanup slice of `P1-AC-016` |
| `WP03-ID-024` | exact-path injection rejects relative destinations | `P1-REQ-009` |
| `WP03-ID-025` | OS entropy adapter succeeds and produces a nonzero word without logging its value | `P1-REQ-009` |

Sources:

- `test/src/telemetry/producer/test_telemetry_identity_contract.cpp`;
- `test/src/telemetry/producer/test_telemetry_identity_native_contract.cpp`.

## Expected RED build and raw failure

The identity test was registered only after the configuration tranche had built
and passed independently. CMake regenerated the unit-test source list, then the
Release target was built:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /v:minimal
```

Exit code: `1`.

```text
  code.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\code.lib
  gtest.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest.lib
  test_telemetry_identity_contract.cpp
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_identity_contract.cpp(1,10): fatal error C1083: Impossible d'ouvrir le fichier include : 'telemetry/identity.h' : No such file or directory [D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\test\src\unittests.vcxproj]
```

This is the intended first production dependency: WP03 configuration contains
no producer/session identity header or implementation. It does not regress or
mask the already-GREEN 18-test configuration tranche.

## Pure identity GREEN verification

After the identity implementation and depth preflight were delivered, the
Release unit-test target built with exit code `0`. The normal pure selection
passed:

```text
[==========] Running 19 tests from 3 test suites.
[----------] 6 tests from TelemetryProducerProfileContract (2 ms total)
[----------] 8 tests from TelemetryProducerIdentityContract (0 ms total)
[----------] 5 tests from TelemetrySessionIdentityContract (5 ms total)
[==========] 19 tests from 3 test suites ran. (8 ms total)
[  PASSED  ] 19 tests.
YOU HAVE 1 DISABLED TEST
```

The disabled 900-container truncated profile was then run alone in a fresh
process with `--gtest_also_run_disabled_tests`; it passed `1/1` in 0 ms with
exit code `0`. This proves the two-container lexical preflight returns before
the recursive Jansson parser even for a high-depth malformed profile.

## Native backend and OS entropy GREEN verification

Only after the pure identity gate was GREEN, the native exact-path tests were
registered and the Release unit-test target rebuilt with exit code `0`.

```powershell
.\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryProducerProfileNativeContract*:TelemetryProducerOsRandomContract*
```

Exit code: `0`.

```text
[==========] Running 5 tests from 2 test suites.
[----------] 4 tests from TelemetryProducerProfileNativeContract (32 ms total)
[----------] 1 test from TelemetryProducerOsRandomContract (1 ms total)
[==========] 5 tests from 2 test suites ran. (33 ms total)
[  PASSED  ] 5 tests.
```

The isolated native fixtures prove same-directory exclusive temporary creation,
write plus durable flush and close, atomic no-clobber install, canonical final
reread, abort cleanup, destructor cleanup, and rejection of a relative injected
destination. A second native-store instance reopened the published profile
without consuming entropy. The OS adapter smoke asserted success and nonzero
output without emitting the random word or a filesystem path. A post-run scan
found zero `fs2open-telemetry-wp03-*` temporary directories remaining.

## Independent review reproduction

An independent reviewer rebuilt the current Release target with exit code `0`
and reproduced the focused results: pure identity `19/19 PASS` in 10 ms, the
isolated profile bomb `1/1 PASS` in 0 ms, and native backend plus OS entropy
`5/5 PASS` in 16 ms. The reviewer also found zero temporary fixture directory
remaining after the run.

A GCC 11 POSIX syntax build of `identity.cpp` with
`-Wall -Wextra -Wpedantic -Werror` exited `0`. Static review confirmed `_commit`
then `MoveFileEx(..., MOVEFILE_WRITE_THROUGH)` on Windows, `fsync` then
`link`/`unlink` no-clobber publication on POSIX, exact writable-root CFile flags,
and no path or random identity logging. Dynamic production CFile resolver tests
and full non-Windows CI remain explicitly open below.

## Baseline held before identity registration

Immediately before registering this RED source, the newly linked configuration
production was independently verified as follows:

- configuration: 18/18 tests passed;
- immutable protocol: 394/394 tests passed;
- complete last-GREEN unit-test binary: 640/640 tests passed;
- public initialization and instrumented registration: 1/1 each passed;
- allocation failure calibration and indices `0..4`: all `PASS`;
- baseline and disabled fast-path benchmarks: 100,000 callbacks each, zero
  tracked C++ allocations and timing checks passed.

## Gates still open

- production writable-root CFile resolver integration for user/game roots;
- non-Windows CI coverage of the OS entropy and native persistence adapters;
- injected real CFile resolver/filesystem failure and exception evidence in a
  fresh process;
- checked startup-budget arithmetic and allocation failures before bind;
- deferred first-`EngineUpdate` startup, zero socket/session before success,
  rollback, terminal state, and exactly one path-free diagnostic.
