# Phase 1 WP03 configuration test-first evidence

Status: **configuration tranche GREEN, including the preserved depth-preflight review RED; WP01/WP02 remain GREEN**.

This evidence covers only the configuration-loader slice of `P1-WP-03`. It does
not claim producer identity, session-ID generation, startup-budget allocation,
socket rollback, or the complete startup diagnostic gate. Those are separate RED
tranches and remain open below.

## Frozen test seam

The tests consume the Phase 1 section 5.3 result contract:

- `ConfigStatus::{Absent, ValidDisabled, ValidEnabled, Invalid}`;
- `ConfigLoadResult::{status, effective, error}`;
- a closed enum `ConfigError` whose success value has underlying value zero;
- a `TelemetryConfig` effective value with the normative snake-case fields.

The pure internal API is deliberately narrower than production CFile I/O:

```cpp
telemetry::detail::parse_telemetry_config_json(std::string_view)
telemetry::detail::load_telemetry_config_from_observation(
    ConfigLocationObservation)
```

These helpers are legitimate producer-internal APIs compiled in `code`, not
`FSO_TELEMETRY_TEST_SEAMS` exports. They are declared only by the internal
`telemetry/config.h`; the engine-facing `telemetry/telemetry.h` remains limited to
`void telemetry::initialize() noexcept`.

`ConfigLocationObservation` freezes `kind`, `offset`, and the exact byte view.
Its kinds are `Absent`, `LooseUserRoot`, `LooseGameRoot`, `LooseActiveMod`, and
`VpOnly`. This gives deterministic evidence for the required resolver decisions
without replacing the production obligation to call
`cf_find_file_location("telemetry.json", CF_TYPE_CONFIG, CF_LOCATION_ALL)`, require
offset zero, and open that exact result through CFile.

Addresses and CIDRs are not compared through a test-only textual representation.
Canonical binary parsing is instead observed through semantic duplicate cases
such as `::1` versus `0:0:0:0:0:0:0:1`, canonical network-address checks, and
prefix bounds.

## Test-ID inventory

| Test ID | GTest oracle | Contract IDs | Expected state before WP03 production |
|---|---|---|---|
| `WP03-CFG-001` | minimal object applies all safe defaults | `P1-REQ-006`–`008`, `D1-008`, `D1-011`, config slice of `P1-AC-004` | RED |
| `WP03-CFG-002` | enabled minimal object remains loopback-only | `P1-REQ-007`, `D1-008` | RED |
| `WP03-CFG-003` | missing schema, malformed JSON and non-object roots fail closed | `P1-REQ-006`, `D1-007`, `P1-AC-004` | RED |
| `WP03-CFG-004` | duplicate root keys rejected | `P1-REQ-006`, `D1-007` | RED |
| `WP03-CFG-005` | exact EOF required; JSON whitespace allowed; second value/NUL rejected | `P1-REQ-006`, `D1-007` | RED |
| `WP03-CFG-006` | 16 KiB accepted, 16 KiB + 1 rejected before schema use | `P1-REQ-006`, `D1-007`, `P1-AC-004` | RED |
| `WP03-CFG-007` | depth five is classified differently from a shallow schema error | `P1-REQ-006`, `D1-007` | RED |
| `WP03-CFG-008` | unknown key invalidates the whole object and publishes no partial `enabled=true` | `P1-REQ-006`, `D1-007` | RED |
| `WP03-CFG-009` | every one of the 14 keys rejects implicit string/number/bool/real conversion | `P1-REQ-006`, `D1-007` | RED |
| `WP03-CFG-010` | all seven bounded integers cover min-1, min, max, max+1 and negative | `P1-REQ-008`, `D1-011` | RED |
| `WP03-CFG-011` | schema, discovery, visibility and trusted-state values are closed for Phase 1 | `P1-REQ-006`–`008`, `D1-008` | RED |
| `WP03-CFG-012` | bind list cardinality, literal-only input, zones, mapped IPv6, bracket/port forms and canonical duplicates | `P1-REQ-007`, `D1-008`, policy slice of `D1-009` | RED |
| `WP03-CFG-013` | allowlist 1/32/33, CIDR prefixes, host bits, mapped IPv6, bracket/port forms and canonical duplicates | `P1-REQ-007`, config slice of `P1-REQ-013` and `P1-AC-005` | RED |
| `WP03-CFG-014` | disabled non-loopback is rejected; enabled non-loopback requires explicit allowlist; wildcard plus catch-all is rejected | `P1-REQ-007`, config slice of `P1-REQ-013`, `P1-AC-004`–`005` | RED |
| `WP03-CFG-015` | absent and VP-only both return `Absent` plus safe effective defaults | `P1-REQ-006`–`007`, `P1-AC-004` | RED |
| `WP03-CFG-016` | loose user, game and active-mod observations at offset zero are accepted | `P1-REQ-006`, `P1-AC-004` | RED |
| `WP03-CFG-017` | a nonzero archive offset is never parsed as loose | `P1-REQ-006`, `D1-007` | RED |
| `WP03-CFG-018` | a loose observation uses the same strict parser and no partial application | `P1-REQ-006`, `D1-007`, `P1-AC-004` | RED |
| `WP03-CFG-019` | truncated input reaching container depth five is rejected by lexical preflight before Jansson | `P1-REQ-006`, `D1-007`, `P1-AC-011` | RED |
| `WP03-CFG-020` | brackets/braces/escaped quote and escaped backslash inside keys/strings do not increment depth | `P1-REQ-006`, `D1-007` | RED |
| `WP03-CFG-021` | opt-in isolated 8,000-container truncated bomb is rejected without entering recursive Jansson | `P1-REQ-006`, `P1-AC-011` | RED |

The source is
`test/src/telemetry/producer/test_telemetry_config_contract.cpp`. The table-driven
oracles expand to more cases than the named GTests: 29 wrong-type cases, 35
integer boundary cases, 15 invalid bind arrays, a 32/33-entry allowlist boundary,
15 invalid CIDR arrays, four location classes, and the strict JSON cases.

## Requirement-state inventory

| Contract ID | Evidence state after this tranche |
|---|---|
| `P1-REQ-006` | GREEN for pure bytes and resolver observations; production CFile path inspected, injected CFile/exception process evidence still open |
| `P1-REQ-007` | GREEN for config-time defaults/exposure/allowlist relations; ingress enforcement remains later |
| `P1-REQ-008` | GREEN for all config defaults, integer bounds and Phase 1-only values |
| `P1-REQ-009` | open: producer profile, OS RNG and session-ID set/collision tests are not in this tranche |
| `P1-REQ-013` | partial: config allowlist preconditions only; packet-order, anti-amplification and rate-limit evidence remains open |
| `P1-REQ-014` | open: checked startup budgets, preallocation and allocation-failure evidence remain open |
| `P1-AC-004` | partial GREEN: absent/invalid config result is fail-closed; socket/session count and unique startup diagnostic remain open |
| `P1-AC-005` | partial GREEN: allowlist configuration is strict; v4/v6 handshake and zero-response ingress behavior belong to later WPs |
| `D1-007` | GREEN for strict/no-partial parser behavior; process restart-only behavior remains runtime evidence |
| `D1-008` | GREEN for config defaults |
| `D1-009` | partial: explicit v4/v6 address policy only; dual-stack sockets belong to WP04 |
| `D1-011` | RED oracle complete for Phase 1 cadence defaults and ranges |

## Expected RED build and complete raw failure

Command executed from the repository root after CMake regenerated the changed
test source list:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /v:minimal
```

Exit code: `1`.

```text
Microsoft (R) Build Engine version 16.4.0+e901037fe pour .NET Framework
Copyright (C) Microsoft Corporation. Tous droits réservés.

  RocketCore.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\RocketCore.lib
  RocketControls.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\RocketControls.lib
  lua51.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\lua51.lib
  RocketCoreLua.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\RocketCoreLua.lib
  RocketControlsLua.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\RocketControlsLua.lib
  RocketDebugger.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\RocketDebugger.lib
  anl.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\anl.lib
  antlr4_static.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\antlr4-runtime-static.lib
  discord-rpc.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\discord-rpc.lib
  embedfile.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\embedfile.exe
  fstl_protocol.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\fstl_protocol.lib
  glad.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\glad.lib
  hidapi_winapi.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\hidapi.lib
  imgui.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\imgui.lib
  jansson.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\jansson.lib
  jpeg.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\jpeg.lib
  lz4.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\lz4.lib
  md5.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\md5.lib
  openxr_loader.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\openxr_loader.lib
  parsers.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\parsers.lib
  pcpnatpmp.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\pcpnatpmp.lib
  zlib.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\zlib.lib
  png.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\png.lib
  code.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\bin\Release\code.lib
  gtest.vcxproj -> D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\lib\Release\gtest.lib
  test_telemetry_config_contract.cpp
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_config_contract.cpp(1,10): fatal error C1083: Impossible d'ouvrir le fichier include : 'telemetry/config.h' : No such file or directory [D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\test\src\unittests.vcxproj]
```

This is the expected handoff failure: WP02 contains no configuration header or
implementation. The test source itself is discovered and compilation reaches its
first production dependency.

## Independent GREEN verification

After the production implementation was delivered, the Release unit-test target
was rebuilt from the same generated build tree:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /v:minimal
```

Exit code: `0`. The final link produced
`build/bin/Release/unittests.exe`.

The complete configuration tranche was then selected by suite name:

```powershell
.\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryConfig*:TelemetryConfigLocation*
```

Exit code: `0`.

```text
[==========] Running 18 tests from 2 test suites.
[----------] 14 tests from TelemetryConfigContract
[----------] 14 tests from TelemetryConfigContract (3 ms total)
[----------] 4 tests from TelemetryConfigLocationContract
[----------] 4 tests from TelemetryConfigLocationContract (0 ms total)
[==========] 18 tests from 2 test suites ran. (3 ms total)
[  PASSED  ] 18 tests.
```

This GREEN result covers the pure parser and deterministic location-observation
contract. The production CFile path was independently inspected to confirm that
it uses the exact `cf_find_file_location` result, requires offset zero, opens that
location through CFile, and maps read/exception failures to the fail-closed
invalid result. An injected CFile/allocation exception process oracle remains an
explicit open item rather than being inferred from this inspection.

## Preserved depth-preflight RED after review

Independent review found that the original maximum-depth check traversed the
tree only after `json_loadb`. The vendored Jansson parser is recursive and has
no matching four-container input limit, so a bounded lexical preflight is a
security requirement rather than only an error classification detail.

Two normal-suite guards and one disabled process-isolation bomb were added. The
new Release binary built successfully, then this safe focused filter was run:

```powershell
.\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryConfigContract.DepthPreflight*:TelemetryConfigContract.StrictJsonDistinguishesDepthFiveFromAValidlyParsedShallowTypeError
```

Exit code: `1`.

```text
[==========] Running 3 tests from 1 test suite.
[ RUN      ] TelemetryConfigContract.DepthPreflightRejectsTruncatedDepthFiveBeforeCallingJansson
Expected equality of these values:
  telemetry::ConfigError::MaximumDepthExceeded
    Which is: 1-byte object <04>
  result.error
    Which is: 1-byte object <03>
A truncated over-depth input must be rejected by the bounded lexical preflight, not the recursive parser.
[  FAILED  ] TelemetryConfigContract.DepthPreflightRejectsTruncatedDepthFiveBeforeCallingJansson (2 ms)
[ RUN      ] TelemetryConfigContract.DepthPreflightIgnoresStructuralCharactersAndEscapesInsideStrings
[       OK ] TelemetryConfigContract.DepthPreflightIgnoresStructuralCharactersAndEscapesInsideStrings (0 ms)
[ RUN      ] TelemetryConfigContract.StrictJsonDistinguishesDepthFiveFromAValidlyParsedShallowTypeError
[       OK ] TelemetryConfigContract.StrictJsonDistinguishesDepthFiveFromAValidlyParsedShallowTypeError (0 ms)
[  PASSED  ] 2 tests.
[  FAILED  ] 1 test.
```

The opt-in bomb remains disabled in ordinary suite selection and is run only in
a fresh process with `--gtest_also_run_disabled_tests` and its exact suite/test
filter. This prevents the pre-fix recursive parser from compromising the test
runner while retaining a post-fix regression oracle.

### Depth-preflight GREEN

After the production lexical preflight was added, the Release unit-test target
built with exit code `0`. The complete normal configuration selection then
passed, including exact default IPv4/IPv6 families, address bytes, CIDR networks
and prefixes, and `VisibilityMode::Cockpit`:

```text
[==========] Running 20 tests from 2 test suites.
[----------] 16 tests from TelemetryConfigContract (3 ms total)
[----------] 4 tests from TelemetryConfigLocationContract (0 ms total)
[==========] 20 tests from 2 test suites ran. (3 ms total)
[  PASSED  ] 20 tests.
YOU HAVE 1 DISABLED TEST
```

The disabled adversarial test was then launched alone in a fresh process:

```powershell
.\build\bin\Release\unittests.exe --gtest_color=no --gtest_also_run_disabled_tests --gtest_filter=TelemetryConfigDepthIsolationContract.DISABLED_DeepTruncatedContainerBombIsRejectedByPreflight
```

Exit code: `0`.

```text
[==========] Running 1 test from 1 test suite.
[ RUN      ] TelemetryConfigDepthIsolationContract.DISABLED_DeepTruncatedContainerBombIsRejectedByPreflight
[       OK ] TelemetryConfigDepthIsolationContract.DISABLED_DeepTruncatedContainerBombIsRejectedByPreflight (0 ms)
[==========] 1 test from 1 test suite ran. (0 ms total)
[  PASSED  ] 1 test.
```

An independent reviewer then rebuilt the same Release target and reproduced the
result: normal configuration selection `20/20 PASS` in 5 ms and the isolated
bomb `1/1 PASS` in 0 ms, both with exit code `0`.

## WP01/WP02 already-GREEN distinction

The isolated WP02 targets do not compile the new RED source. They were rebuilt:

```powershell
cmake --build build --config Release --target telemetry_initialize_contract_tests telemetry_initialize_failure_contract_tests telemetry_disabled_benchmark -- /m:1 /v:minimal
```

Exit code: `0`.

The nominal process-lifetime registration oracle then passed `1/1`. The allocation
calibration reported five attempts, and every injected failure index `0..4`
reported `contract_check=PASS`; command exit code was `0`. The pre-existing public
API smoke also passed `1/1` from the last green `unittests.exe`.

Finally, the immutable Phase 0/Phase 1 protocol regression was executed from that
same last green binary:

```powershell
& .\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryProtocol*
```

Exit code: `0`.

```text
[==========] Running 394 tests from 36 test suites.
[==========] 394 tests from 36 test suites ran. (583 ms total)
[  PASSED  ] 394 tests.
```

Thus the single failing build is intentionally attributable to the new WP03
production contract. It is not evidence of a WP01 wire or WP02 initialization
regression.

## Post-configuration regression verification

After the configuration production files were linked, the isolated WP02 targets
were rebuilt together with exit code `0`. The public initialization smoke and the
instrumented process-lifetime registration oracle each passed `1/1`. The
allocation calibration still observed exactly five attempts, and every injected
failure index `0..4` returned `contract_check=PASS` with process exit code `0`.

The immutable protocol regression was rerun from the newly linked unit-test
binary:

```text
[==========] Running 394 tests from 36 test suites.
[==========] 394 tests from 36 test suites ran. (622 ms total)
[  PASSED  ] 394 tests.
```

Finally, fresh baseline and disabled callback benchmark processes each measured
100,000 callbacks. Both reported `tracked_cpp_allocations=0`,
`allocation_check=PASS`, and `timing_check=PASS`; the raw regression samples were
written under the ignored `build` tree rather than replacing the frozen WP02
artifacts.

The complete last-GREEN binary was also run before registering the next RED
identity source:

```text
[==========] Running 640 tests from 90 test suites.
[==========] 640 tests from 90 test suites ran. (3548 ms total)
[  PASSED  ] 640 tests.
```

## Open RED tranches

- producer-profile schema, 1 KiB/depth-two limits, storage-root selection, atomic
  temporary write/flush/rename/reread, and every injected failure;
- producer and session OS entropy, nonzero values, 16 collision attempts,
  65,536-entry process set, and no reuse;
- checked startup-budget arithmetic, allocation failure, and pre-bind ordering;
- deferred first-EngineUpdate startup, zero socket/session before success,
  idempotence, partial-open rollback, and exactly one path-free startup diagnostic.
