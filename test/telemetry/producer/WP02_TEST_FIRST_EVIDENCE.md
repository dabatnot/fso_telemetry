# Phase 1 WP02 test-first evidence

Status: **nominal and allocation-failure RED/GREEN cycles captured**.

## Contract slice

`void telemetry::initialize() noexcept` must register exactly one process-lifetime
callback for each of `EngineUpdate`, `EngineShutdown`, `GameMissionLoad`,
`GameEnterState`, and `GameLeaveState`, even after repeated initialization.
Initialization itself must not start configuration, transport, mission capture, or
serialization work.

The isolated executable starts a fresh process and resets observation counters only.
It calls `initialize()` twice and requires registration attempts `{1,1,1,1,1}` in
the event order above, then calls it eight additional times and requires the same
counts. It emits each event once and requires callback invocations `{1,1,1,1,1}`.
It then calls `initialize()` eight more times, emits each event once, and requires
registrations to remain `{1,1,1,1,1}` and invocations to become `{2,2,2,2,2}`.

## Isolated target

`telemetry_initialize_contract_tests` compiles only:

- `test/src/telemetry/producer/test_telemetry_initialize_instrumented.cpp`;
- `code/telemetry/telemetry.cpp`;
- `code/events/events.cpp`.

It defines `FSO_TELEMETRY_TEST_SEAMS=1` privately and links `gtest_main` plus
`sdl2` for the engine header dependency. It deliberately does **not** link target
`code`. The test forward-declares only the three `telemetry::test_seam` observation
functions. Linking the exact `events.cpp`, which defines only the five contracted
event objects, also prevents an unnoticed dependency on another engine event.

## Public API smoke check

Commands executed from the repository root:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /v:minimal
& .\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryProducerInitialize.PublicApiIsNoexceptAndSafeToCallTwice
```

Exit codes: build `0`; test `0`.

```text
  code.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\code.lib
  gtest.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest.lib
  unittests.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\unittests.exe

Running main() from gtest_main.cc
Note: Google Test filter = TelemetryProducerInitialize.PublicApiIsNoexceptAndSafeToCallTwice
[==========] Running 1 test from 1 test suite.
[----------] Global test environment set-up.
[----------] 1 test from TelemetryProducerInitialize
[ RUN      ] TelemetryProducerInitialize.PublicApiIsNoexceptAndSafeToCallTwice
[       OK ] TelemetryProducerInitialize.PublicApiIsNoexceptAndSafeToCallTwice (0 ms)
[----------] 1 test from TelemetryProducerInitialize (0 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (0 ms total)
[  PASSED  ] 1 test.
```

## Build command and raw result

Command executed from the repository root:

```powershell
cmake --build build --config Release --target telemetry_initialize_contract_tests -- /m:1 /v:minimal
```

Exit code: `0`.

```text
Microsoft (R) Build Engine version 16.4.0+e901037fe pour .NET Framework
Copyright (C) Microsoft Corporation. Tous droits réservés.

  gtest.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest.lib
  gtest_main.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest_main.lib
  telemetry_initialize_contract_tests.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\telemetry_initialize_contract_tests.exe
```

## Expected failing command and complete raw result

Command executed from the repository root in a fresh process:

```powershell
& .\build\bin\Release\telemetry_initialize_contract_tests.exe --gtest_color=no
```

Exit code: `1`.

```text
Running main() from D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\gtest\src\gtest_main.cc
[==========] Running 1 test from 1 test suite.
[----------] Global test environment set-up.
[----------] 1 test from TelemetryProducerInitializeInstrumented
[ RUN      ] TelemetryProducerInitializeInstrumented.RegistersExactlyOnceAndRepeatedInitializeDoesNotDuplicateCallbacks
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_initialize_instrumented.cpp(65): error: Expected equality of these values:
  one_registration_each
    Which is: { 1, 1, 1, 1, 1 }
  registration_counts()
    Which is: { 0, 0, 0, 0, 0 }

D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_initialize_instrumented.cpp(71): error: Expected equality of these values:
  one_registration_each
    Which is: { 1, 1, 1, 1, 1 }
  registration_counts()
    Which is: { 0, 0, 0, 0, 0 }

D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_initialize_instrumented.cpp(78): error: Expected equality of these values:
  one_invocation_each
    Which is: { 1, 1, 1, 1, 1 }
  invocation_counts()
    Which is: { 0, 0, 0, 0, 0 }

D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_initialize_instrumented.cpp(85): error: Expected equality of these values:
  one_registration_each
    Which is: { 1, 1, 1, 1, 1 }
  registration_counts()
    Which is: { 0, 0, 0, 0, 0 }

D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_initialize_instrumented.cpp(87): error: Expected equality of these values:
  two_invocations_each
    Which is: { 2, 2, 2, 2, 2 }
  invocation_counts()
    Which is: { 0, 0, 0, 0, 0 }

[  FAILED  ] TelemetryProducerInitializeInstrumented.RegistersExactlyOnceAndRepeatedInitializeDoesNotDuplicateCallbacks (1 ms)
[----------] 1 test from TelemetryProducerInitializeInstrumented (1 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (1 ms total)
[  PASSED  ] 0 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] TelemetryProducerInitializeInstrumented.RegistersExactlyOnceAndRepeatedInitializeDoesNotDuplicateCallbacks

 1 FAILED TEST
```

The failure was the frozen production handoff: the Stage A no-op scaffold
reported zero registration attempts and received zero callback invocations.
The section above is retained verbatim as test-first evidence.

## Green verification after production implementation

The same isolated target was rebuilt without changing its test oracle:

```powershell
cmake --build build --config Release --target telemetry_initialize_contract_tests -- /m:1 /v:minimal
& .\build\bin\Release\telemetry_initialize_contract_tests.exe --gtest_color=no
```

Exit codes: build `0`; test `0`.

```text
Running main() from D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\gtest\src\gtest_main.cc
[==========] Running 1 test from 1 test suite.
[----------] Global test environment set-up.
[----------] 1 test from TelemetryProducerInitializeInstrumented
[ RUN      ] TelemetryProducerInitializeInstrumented.RegistersExactlyOnceAndRepeatedInitializeDoesNotDuplicateCallbacks
[       OK ] TelemetryProducerInitializeInstrumented.RegistersExactlyOnceAndRepeatedInitializeDoesNotDuplicateCallbacks (0 ms)
[----------] 1 test from TelemetryProducerInitializeInstrumented (0 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (0 ms total)
[  PASSED  ] 1 test.
```

This single fresh-process test verifies all frozen values: registrations
`{1,1,1,1,1}` immediately after two calls and still after eight additional
calls; invocations `{1,1,1,1,1}` after one emission of each event and
`{2,2,2,2,2}` after another initialize series plus one more emission; out-of-
range index `5` returns zero for both observer functions.

## Post-handoff public smoke

The real `code` target and `unittests` executable were rebuilt after the
production change. The public test does not define the test seam:

```powershell
& .\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryProducerInitialize.PublicApiIsNoexceptAndSafeToCallTwice
```

Exit code: `0`.

```text
Running main() from gtest_main.cc
Note: Google Test filter = TelemetryProducerInitialize.PublicApiIsNoexceptAndSafeToCallTwice
[==========] Running 1 test from 1 test suite.
[----------] Global test environment set-up.
[----------] 1 test from TelemetryProducerInitialize
[ RUN      ] TelemetryProducerInitialize.PublicApiIsNoexceptAndSafeToCallTwice
[       OK ] TelemetryProducerInitialize.PublicApiIsNoexceptAndSafeToCallTwice (0 ms)
[----------] 1 test from TelemetryProducerInitialize (0 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (0 ms total)
[  PASSED  ] 1 test.
```

## WP01 regression

```powershell
& .\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryProtocol*
```

Exit code: `0`.

```text
[==========] Running 394 tests from 36 test suites.
[==========] 394 tests from 36 test suites ran. (495 ms total)
[  PASSED  ] 394 tests.
```

The disabled fast-path timing, allocation, binary inspection, environment, raw
samples, and explicitly blocked engine-frame sub-proof are recorded separately
in `test/telemetry/producer/WP02_FAST_PATH_BENCHMARK.md`.

## Reviewer correction: allocation failure during event registration

The nominal green test did not exercise the five allocations performed by the
first `util::event::add` on each independent event. A separate executable now
compiles the same `telemetry.cpp` and exact `events.cpp` with
`FSO_TELEMETRY_TEST_SEAMS`, but without target `code`. Its global `new` override
is armed only immediately before `telemetry::initialize()`.

Target:

```text
telemetry_initialize_failure_contract_tests
```

Build command:

```powershell
cmake --build build --config Release --target telemetry_initialize_failure_contract_tests -- /m:1 /v:minimal
```

Exit code: `0`.

### Allocation calibration

A fresh process with injection armed beyond the observed range proves that the
current initialization path makes exactly five C++ allocation attempts, one for
each first `event.add`, with no intervening allocation:

```powershell
& .\build\bin\Release\telemetry_initialize_failure_contract_tests.exe probe
```

```text
calibration_probe=1
allocation_attempts=5
registration_attempts={1,1,1,1,1}
calibration_check=PASS
PROBE_EXIT=0
```

Therefore fail indices `0..4` target, in order, `EngineUpdate`,
`EngineShutdown`, `GameMissionLoad`, `GameEnterState`, and `GameLeaveState`.

### Expected failing command and complete raw result

Each loop iteration below launches a new executable process:

```powershell
foreach($i in 0..4) {
    "CASE_FAIL_AFTER=$i"
    & .\build\bin\Release\telemetry_initialize_failure_contract_tests.exe "$i"
    "CASE_EXIT=$LASTEXITCODE"
}
```

```text
CASE_FAIL_AFTER=0
terminate_observed=1
CASE_EXIT=86
CASE_FAIL_AFTER=1
terminate_observed=1
CASE_EXIT=86
CASE_FAIL_AFTER=2
terminate_observed=1
CASE_EXIT=86
CASE_FAIL_AFTER=3
terminate_observed=1
CASE_EXIT=86
CASE_FAIL_AFTER=4
terminate_observed=1
CASE_EXIT=86
```

This is the required new RED oracle: every injected `std::bad_alloc` escapes
the current `noexcept` function and reaches the harness terminate handler.

### Frozen post-fix matrix

After the production fix, every case must return exit code `0` in its fresh
process and satisfy this matrix. Registration counts include the failed attempt
because the observation counter is incremented immediately before `event.add`.

| `fail_after` | Failing add | Alloc attempts after first and second call | Registrations after first and second call | Callback entries after emitting all five events |
|---:|---|---:|---|---|
| 0 | `EngineUpdate` | 1 / 1 | `{1,0,0,0,0}` / unchanged | `{0,0,0,0,0}` |
| 1 | `EngineShutdown` | 2 / 2 | `{1,1,0,0,0}` / unchanged | `{1,0,0,0,0}` |
| 2 | `GameMissionLoad` | 3 / 3 | `{1,1,1,0,0}` / unchanged | `{1,1,0,0,0}` |
| 3 | `GameEnterState` | 4 / 4 | `{1,1,1,1,0}` / unchanged | `{1,1,1,0,0}` |
| 4 | `GameLeaveState` | 5 / 5 | `{1,1,1,1,1}` / unchanged | `{1,1,1,1,0}` |

The executable checks that injection fired, `initialize()` returned instead of
terminating, the process-lifetime guard prevented a retry on the second call,
registration observations did not change, and every successfully installed
prefix listener was entered exactly once when all five events were emitted.
`callback_invocations` is defined at callback entry, so those entries are
expected; the callbacks remain functionally inert because the module stays
disabled and performs no startup, transport, mission, or serialization work.
No production file was changed to create this RED evidence.

### Green verification after allocation-failure correction

The failure target was rebuilt against the corrected production source:

```powershell
cmake --build build --config Release --target telemetry_initialize_failure_contract_tests -- /m:1 /v:minimal
```

Exit code: `0`.

The calibration and all five cases were then run again as separate fresh
processes. Complete raw output:

```text
calibration_probe=1
allocation_attempts=5
registration_attempts={1,1,1,1,1}
calibration_check=PASS
PROBE_EXIT=0
CASE_FAIL_AFTER=0
fail_after=0
injection_fired=1
allocation_attempts_after_first=1
allocation_attempts_after_second=1
registrations_after_first={1,0,0,0,0}
registrations_after_second={1,0,0,0,0}
invocations_after_emission={0,0,0,0,0}
contract_check=PASS
CASE_EXIT=0
CASE_FAIL_AFTER=1
fail_after=1
injection_fired=1
allocation_attempts_after_first=2
allocation_attempts_after_second=2
registrations_after_first={1,1,0,0,0}
registrations_after_second={1,1,0,0,0}
invocations_after_emission={1,0,0,0,0}
contract_check=PASS
CASE_EXIT=0
CASE_FAIL_AFTER=2
fail_after=2
injection_fired=1
allocation_attempts_after_first=3
allocation_attempts_after_second=3
registrations_after_first={1,1,1,0,0}
registrations_after_second={1,1,1,0,0}
invocations_after_emission={1,1,0,0,0}
contract_check=PASS
CASE_EXIT=0
CASE_FAIL_AFTER=3
fail_after=3
injection_fired=1
allocation_attempts_after_first=4
allocation_attempts_after_second=4
registrations_after_first={1,1,1,1,0}
registrations_after_second={1,1,1,1,0}
invocations_after_emission={1,1,1,0,0}
contract_check=PASS
CASE_EXIT=0
CASE_FAIL_AFTER=4
fail_after=4
injection_fired=1
allocation_attempts_after_first=5
allocation_attempts_after_second=5
registrations_after_first={1,1,1,1,1}
registrations_after_second={1,1,1,1,1}
invocations_after_emission={1,1,1,1,0}
contract_check=PASS
CASE_EXIT=0
```

All five `noexcept`, fail-closed, process-guard, no-retry, no-duplication, and
partial-listener callback-entry checks now pass. The nominal instrumented target
and public smoke were also rebuilt and remained `1/1 PASS`. The final benchmark
rerun confirms that the exception guard did not change the disabled fast path;
its regenerated values and hashes are in `WP02_FAST_PATH_BENCHMARK.md`.
