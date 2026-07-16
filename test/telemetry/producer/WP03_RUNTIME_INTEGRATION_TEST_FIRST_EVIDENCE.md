# Phase 1 WP03 engine/runtime integration test-first evidence

Status: **second integration sub-tranche GREEN and independently FINAL APPROVE
after a preserved RED; G1-B is PASS/CLOSED, while G1-C, WP04 transport and later
lifecycle work remain OPEN**.

This evidence follows the pure-runtime transaction in
`WP03_RUNTIME_TEST_FIRST_EVIDENCE.md`. It closes the remaining WP03 question:
whether the real `EngineUpdate` listener registered by `telemetry::initialize()`
executes that same `Runtime` instance, rather than a parallel registration-only
boolean or test stub.

## Frozen integration boundary

The public header remains unchanged. Its SHA-256 before this RED is:

```text
1B80B739B7B6B0849D71757AEBB4E62A1205D3605B0847854513C287B01F8DDA  code/telemetry/telemetry.h
```

The production-only internal header `telemetry/runtime_adapter.h` must declare
one link-time factory:

```cpp
telemetry::detail::RuntimeStartupServices& runtime_startup_services() noexcept;
```

`telemetry.cpp` must construct one process-lifetime `Runtime` from this factory.
`initialize()` captures the main thread through that instance but performs no
configuration load. The registered `EngineUpdate` callback calls
`on_engine_update()` on the same instance. Under the existing test-seam build,
state and terminal-reason observers must read that instance, not mirrored
booleans.

The public entry point remains only `void telemetry::initialize() noexcept`.
No runtime state, adapter, factory or test seam enters `telemetry.h`.

## One pure runtime in every isolated WP02 executable

`test/src/CMakeLists.txt` now defines one shared source list containing exactly:

```text
code/telemetry/runtime.cpp
test/src/telemetry/producer/telemetry_runtime_disabled_adapter.cpp
```

That same list is inserted into all three isolated targets:

- `telemetry_initialize_contract_tests`;
- `telemetry_initialize_failure_contract_tests`;
- `telemetry_disabled_benchmark`.

They therefore compile the production `Runtime` class itself. They do not link
a copied runtime, a macro-short-circuited runtime, or the production adapter.
The test-only adapter supplies the same factory symbol at link time.

The disabled adapter captures and compares the real `std::thread::id`, returns
only `RuntimeConfigStatus::Absent`, and counts every call after config. It
contains no configuration parser, profile access, registry allocation or
socket primitive. Its later methods are closed sentinels that make any call
after absent configuration observable as a test failure.

## Instrumented sequence

The existing single instrumented GTest is strengthened to require:

1. repeated `initialize()` registers each of the five callbacks exactly once;
2. adapter factory is requested once, main thread is captured once, runtime is
   still `Cold`, and config/thread-check counts remain zero after initialize;
3. first `EngineUpdate` invokes its callback once, checks the captured thread,
   loads config exactly once and changes that same runtime to
   `Disabled/ConfigAbsent`;
4. no identity, candidate, budget, registry or transport adapter method is
   reached; absent config emits at most one diagnostic;
5. second and later `EngineUpdate` calls do not recheck the thread, reload
   config, request the factory, repeat a diagnostic or retry startup;
6. repeated `initialize()` does not recapture the thread or duplicate any
   listener; the four remaining callbacks preserve their WP02 invocation
   accounting.

As a separate GREEN regression reinforcement, the allocation-failure executable
freezes the partial-registration case. Until all five callback additions
succeed, an already-registered `EngineUpdate` callback may be invoked for
accounting but cannot arm startup: runtime stays `Cold/None` and thread-check,
config, post-config and diagnostic adapter counts all remain zero. This
reinforcement is not part of the frozen RED described below.

## Integration test-ID inventory

| Test ID | Oracle | Contract evidence |
|---|---|---|
| `WP03-RTI-001` | all three isolated WP02 targets compile the same production `runtime.cpp` plus the link-time disabled adapter | build/integration slice of `P1-REQ-005`, `P1-REQ-036`, `P1-AC-003` |
| `WP03-RTI-002` | initialize requests one factory, captures once and leaves runtime `Cold` with config count zero | deferred-startup/idempotence slice of `P1-REQ-015` |
| `WP03-RTI-003` | first registered `EngineUpdate` reaches the observable runtime and produces `Disabled/ConfigAbsent` with one config load | `P1-REQ-006`, `P1-REQ-015`, partial `P1-AC-004` |
| `WP03-RTI-004` | second/later updates are terminal no-retry and never reach post-config services | disabled fast-path slice of `P1-REQ-033` |
| `WP03-RTI-005` | public header hash/surface is unchanged and test seams remain internal | seam-scope slice of `P1-REQ-005`, `P1-AC-003` |
| `WP03-RTI-006` | **GREEN regression only:** calibration before an event is `Cold`/config-zero; each injected callback-registration failure `0..4` leaves a partial listener set inert and never starts runtime | exception/idempotence slice of `P1-REQ-015`, partial `P1-AC-004`/`011` |

`WP03-RTI-006` was added by the independent test agent during GREEN, after the
production `callbacks_ready` implementation was visible. No pre-production
failure was captured for this case, so it is regression evidence only and MUST
NOT be reported as a RED or test-first oracle. The frozen RED was limited to the
instrumented test, the test-only adapter and the common isolated-target CMake
source list.

## Expected RED build and raw failure

The test-only adapter, instrumented assertions and common isolated-target source
list were built from the repository root with serialized MSBuild:

```powershell
cmake --build build --config Release --target telemetry_initialize_contract_tests -- /m:1 /nr:false
```

Exit code: `1`.

```text
  gtest.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest.lib
  gtest_main.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest_main.lib
  test_telemetry_initialize_instrumented.cpp
  runtime.cpp
  telemetry_runtime_disabled_adapter.cpp
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\telemetry_runtime_disabled_adapter.cpp(1,10): fatal error C1083: Impossible d'ouvrir le fichier include : 'telemetry/runtime_adapter.h' : No such file or directory [D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\test\src\telemetry_initialize_contract_tests.vcxproj]
```

MSVC also emitted existing non-fatal deprecation warnings while compiling the
instrumented engine headers. The only build-stopping error is the intended
missing production adapter contract. No production file was edited for this
RED.

Independent RED review found one test-only over-constraint: the first oracle
required zero diagnostics for absent configuration although the normative and
pure-runtime contracts allow zero or one. The final oracle now accepts at most
one on the first update and freezes that count across all later updates. The
serialized target was rebuilt after this correction and reproduced the same
single C1083 adapter-header failure with exit code `1`.

## Integration GREEN verification

Production delivered `runtime_adapter.{h,cpp}`, registered them in the
production source group, and routed `telemetry.cpp` through one process-lifetime
`Runtime`. The three isolated targets rebuilt together under serialized Release
MSBuild with exit code `0`:

```powershell
cmake --build build --config Release --target telemetry_initialize_contract_tests telemetry_initialize_failure_contract_tests telemetry_disabled_benchmark -- /m:1 /nr:false
```

The instrumented same-instance oracle passed `1/1`. It observed one factory
request, one main-thread capture, zero config calls after repeated initialize,
then `Disabled/ConfigAbsent` and exactly one config call after the first
registered `EngineUpdate`; subsequent updates did not retry.

The allocation calibration still observed exactly five C++ allocation attempts
for five callback registrations. Before any event it also observed runtime
state/reason `0/0` (`Cold/None`) and zero thread-check/config/post-config/
diagnostic calls. Each fresh-process injected failure index `0..4` reported
`contract_check=PASS`; even when the partially registered `EngineUpdate`
listener was invoked, every runtime observation remained the same zero-startup
tuple.

Fresh baseline and disabled benchmarks each measured 100,000 callbacks with
zero tracked C++ allocations and passing timing checks. The integrated disabled
runtime measured mean `0.000074132 ms` and p99 `0.000100000 ms`; raw samples
remain under the ignored build tree.

The ordinary Release `unittests` target rebuilt and linked with exit code `0`.
The pure runtime selection remained `10/10`; the combined protocol/WP03/public
selection passed `463/463` in 638 ms; both opt-in depth bombs passed `2/2`.
The complete binary passed:

```text
[==========] Running 690 tests from 97 test suites.
[==========] 690 tests from 97 test suites ran. (3495 ms total)
[  PASSED  ] 690 tests.
YOU HAVE 2 DISABLED TESTS
```

Disposable full-suite settings, preset and pilot fixtures were checked under
`test/test_data` and removed afterward; that subtree was clean.

GCC 11.4 strict syntax checks passed for both production and test adapters. The
exact final commands were:

```powershell
wsl.exe g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Icode -Ibuild/generated_source -Ibuild/generated_source/code -Ibuild/lib/prebuilt/sdl2/include -include strings.h -D_stricmp=strcasecmp -D_strnicmp=strncasecmp -include globalincs/pstypes.h -fsyntax-only code/telemetry/runtime_adapter.cpp
wsl.exe g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Icode -fsyntax-only test/src/telemetry/producer/telemetry_runtime_disabled_adapter.cpp
```

Both returned exit code `0`. The production command supplied the generated
Windows platform header, SDL include and the repository's normal `pstypes`
precompiled-header context; the test adapter needed no engine PCH context.

Static evidence collection found the real adapter calls
`load_telemetry_config()`, owns `OsRandomSource` and `SessionIdRegistry`, creates
`NativeProducerProfileStore` with the selected portable root, draws the
candidate, calculates the known subtotal, and delegates registry
allocate/register/release. `start_transport()` alone returns `Unavailable`.
Search found no socket/bind/receive/send/PSNET primitive and no old
`telemetry_disabled` guard. The independent reviewer statically verified these
real production mappings and approved them; the test-agent collection alone
was not used as a substitute for that review.

The public-header SHA-256 remained exactly
`1B80B739B7B6B0849D71757AEBB4E62A1205D3605B0847854513C287B01F8DDA`.

## Requirement and gate state after integration GREEN

| Contract or gate | Evidence state |
|---|---|
| `P1-REQ-005`, `P1-REQ-036` | GREEN for this integration slice: one shared runtime source and link-time adapter layout build in all three isolated targets; wider supported build matrix remains open |
| `P1-REQ-006` | GREEN for absent-config routing through a real registered `EngineUpdate`; strict loader component tests remain GREEN |
| `P1-REQ-015` | partial GREEN: idempotent registration, full-registration arming and first-update same-instance routing pass; WP05 mission/menu/shutdown runtime behavior remains open |
| `P1-REQ-016` | partial GREEN: initialize captures and first update checks the actual thread through the adapter; broader engine-global/DTO review remains open |
| `P1-REQ-033` | GREEN for the disabled 100,000-callback allocation and timing slice after real-runtime integration; active-runtime performance remains open |
| `P1-AC-003`, `P1-AC-004` | partial GREEN for public seam, build/link, absent-config zero-socket state and terminal no-retry; wider variants and real config-file integration remain open |
| `P1-REQ-007`, `P1-REQ-009`, `P1-REQ-013`, `P1-REQ-014`, `P1-REQ-016`, `P1-REQ-033` | remain OPEN phase-wide; this WP03 integration evidence closes none of their remaining transport, queue, lifecycle, concurrency or active-runtime/performance scope |
| `P1-AC-005`, `P1-AC-014`, `P1-AC-016` | remain OPEN phase-wide; no claim in this evidence closes their later-work-package acceptance scope |
| `G1-B` | **PASS/CLOSED** after independent FINAL APPROVE of the runtime integration slice and its reproduced evidence |
| `G1-C` | **OPEN**: WP04 transport, complete quotas and socket evidence are absent |

## Delivered production contract and final independent review

- add the internal `runtime_adapter.h` factory declaration and a production
  `runtime_adapter.cpp` implementation; register them in the production source
  group;
- make `telemetry.cpp` own exactly one `Runtime`, created from that factory;
- call `Runtime::capture_main_thread()` from the idempotent initialize path and
  route the registered `EngineUpdate` callback to that same instance;
- expose runtime state/reason only in the existing `FSO_TELEMETRY_TEST_SEAMS`
  namespace used by the isolated instrumented target;
- remove the parallel `telemetry_disabled` guard; do not add a
  registration-only branch or compile-time runtime bypass;
- preserve all five callback registrations, their allocation-failure contract,
  the public header and the existing no-throw boundary;
- keep the production adapter fail-closed and WP03-only: it MUST call
  `load_telemetry_config()`, own `NativeProducerProfileStore` plus
  `OsRandomSource`, call `draw_session_id_candidate()` and the known-budget
  calculation, and own the `SessionIdRegistry` used for
  allocation/registration/release; only `start_transport()` remains
  `Unavailable` with zero sockets before WP04;
- treat the disabled adapter as wiring evidence only; the independent reviewer
  must statically verify every real production mapping in this same handoff,
  not defer those mappings to a third implementation lot;
- do not edit the test adapter, instrumented test or test CMake during the
  production handoff.

The implementer delivered these production items without editing the frozen RED
oracle. The independent reviewer then inspected the real mappings, one-runtime
ownership, `callbacks_ready` guard, no-socket boundary, public-header stability,
test/CMake diff and raw GREEN evidence. The reviewer independently reproduced:

- the three isolated Release targets and the Release `unittests` build/link;
- the same-instance oracle `1/1`;
- the five-allocation calibration and all five injected failures `0..4`;
- both 100,000-callback benchmarks with zero tracked C++ allocations;
- pure runtime `10/10`, combined protocol/WP03/public `463/463`, opt-in depth
  bombs `2/2`, and the complete suite `690/690` with two disabled tests;
- both exact GCC 11.4 strict commands above with exit code `0`.

Final independent verdict: **APPROVE, with no remaining finding**. The approved
evidence preserves `WP03-RTI-006` as a GREEN-only regression added after the
production `callbacks_ready` implementation was visible; no pre-production RED
was captured for that case. This approval closes G1-B only. G1-C and the
phase-wide remaining scope of `P1-REQ-007`, `P1-REQ-009`, `P1-REQ-013`,
`P1-REQ-014`, `P1-REQ-016`, `P1-REQ-033`, `P1-AC-005`, `P1-AC-014` and
`P1-AC-016` remain OPEN.
