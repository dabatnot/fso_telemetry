# Phase 1 WP03 known startup-budget subtotal test-first evidence

Status: **known-subtotal tranche GREEN after a preserved test-first RED; this
tranche remains explicitly incomplete and does not claim the complete Phase 1
startup budget**.

This evidence covers the arithmetic and publication contract for the components
whose representation or inherited quota is already known in `P1-WP-03`. It does
not treat a missing future component as zero. The result is therefore named
`Wp03KnownBudgetSubtotal`, always publishes `is_complete=false`, and carries an
explicit deferred-category mask.

## Frozen partial contract

For `C = maxClients`, with `1 <= C <= 4`, the known subtotal is:

```text
1,048,576 bytes                         session-ID hash representation
+ C * 4,194,304 bytes                   inherited reassembly quota projection
+ C * 33,554,432 bytes                  inherited reliable-retention projection
```

The exact expected subtotals are `38,797,312` bytes for one client and
`152,043,520` bytes for four clients. The session-ID term reflects the actual
`131,072 * sizeof(uint64_t)` hash representation used to retain at most 65,536
logical IDs. The 4 MiB/client and 32 MiB/client terms are accounting
projections inherited from Phase 0 constants; they are not evidence that the
corresponding concrete runtime storage has been allocated.

The provisional cap for this known subtotal is `268,435,456` bytes (256 MiB).
This is a non-normative WP03 implementation guard, not a complete Phase 1 memory
ceiling. It must be revalidated when WP06, WP08 and WP09 provide the concrete
reliable-window, replication/scratch and metrics representations.

The deferred mask must contain all of:

- `ClientSlotStorage`;
- `StateReassemblyStorage`;
- `TransportBuffers`;
- `ReliableWindowStorage`;
- `BaselineStorage`;
- `DeltaStorage`;
- `SerializationScratch`;
- `Metrics`.

`ClientSlotStorage` and `StateReassemblyStorage` are deliberately distinct from
the published logical counts and reassembly quota projection. The current
subtotal knows `C` client slots, `4C` reassembly slots, `2C` baseline slots and
`2C` delta slots, but does not invent the future byte representation of any of
those objects.

Both the native `size_t` calculation and the independently published
`uint64_t` metric value use checked add/multiply paths. Any invalid client count,
arithmetic overflow or static-cap breach must return an error with zero bytes,
zero counts, zero metric subtotal and `is_complete=false`; partial publication
is forbidden.

## Test-ID inventory

| Test ID | GTest oracle | Contract evidence |
|---|---|---|
| `WP03-BUD-001` | provisional 256 MiB guard and actual 131,072-slot session registry representation are explicit | `P1-REQ-009`, support for `P1-REQ-014` |
| `WP03-BUD-002` | checked `size_t` add/multiply publish only successful results and zero their output on overflow | arithmetic slice of `P1-REQ-014`, `P1-AC-011` |
| `WP03-BUD-003` | checked metric add/multiply follow an independent `uint64_t` path | observability support for `P1-REQ-014` |
| `WP03-BUD-004` | default request reuses the inherited 4 MiB/client and 32 MiB/client Phase 0 bounds | `P1-REQ-012`, support for `P1-REQ-014` |
| `WP03-BUD-005` | logical counts scale as `C`, `4C`, `2C`, `2C`; 4 MiB is per client, not per reassembly slot | `P1-REQ-008`, `P1-REQ-012`, partial `P1-REQ-014` |
| `WP03-BUD-006` | exactly eight concrete-storage categories remain deferred (`Count == 8`, mask `0x00ff`) and the result is incomplete | prevents false closure of `P1-REQ-014` |
| `WP03-BUD-007` | one/four-client known subtotals are exact but never complete | partial `P1-REQ-014` |
| `WP03-BUD-008` | client counts 0, 5 and `SIZE_MAX` fail without clamping or partial publication | `P1-REQ-008`, partial `P1-AC-004`/`P1-AC-011` |
| `WP03-BUD-009` | cap minus one and exact cap succeed; cap plus one fails closed | partial `P1-REQ-014` |
| `WP03-BUD-010` | each known component product and each of the two subtotal additions fail closed on overflow | arithmetic slice of `P1-REQ-014`, `P1-AC-011` |

The test source is
`test/src/telemetry/producer/test_telemetry_startup_budget_contract.cpp` and is
registered in `test/src/source_groups.cmake`.

## Requirement and gate state

| Contract or gate | Evidence state after this known-subtotal tranche |
|---|---|
| `P1-REQ-008` | oracle present for `maxClients` 1–4 and exact count scaling; runtime startup use remains open |
| `P1-REQ-009` | the already-implemented session registry's actual byte representation is accounted; construction/allocation ordering remains open |
| `P1-REQ-012` | inherited reassembly and reliable-retention constants are reused; WP04 concrete transport/reassembly storage remains open |
| `P1-REQ-013` | open: this pure arithmetic seam does not prove validation before proportional allocation |
| `P1-REQ-014` | partial only: known-component checked arithmetic and fail-closed publication are specified; complete products, saturation, high-water and real storage are open |
| `P1-AC-004` | strengthened only by a zero-publication oracle; zero socket/session before startup failure still needs runtime evidence |
| `P1-AC-011` | partial arithmetic no-partial-publication oracle; hostile corpus/fuzz and allocation behavior remain open |
| `G1-C` | open: preallocation, allocation failure, pre-bind ordering and transport evidence are missing |
| `G1-F` | open: complete metrics, memory budgets and benchmarks are deferred |
| `G1-G` | open: fuzz, soak, relaunch and leak evidence are deferred |

## Expected RED build and raw failure

The final reviewed oracle, including the two additional concrete-storage
categories, was built from the repository root with serialized MSBuild:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /nr:false /v:minimal
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
  test_telemetry_startup_budget_contract.cpp
D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\test\src\telemetry\producer\test_telemetry_startup_budget_contract.cpp(1,10): fatal error C1083: Impossible d'ouvrir le fichier include : 'telemetry/startup_budget.h' : No such file or directory [D:\david\Documents\Dev\fsotelemetry\fs2open.github.com\build\test\src\unittests.vcxproj]
```

This is the intended first production dependency. Configuration and identity
remain separately GREEN; their implementations do not provide a startup-budget
header. The failure therefore demonstrates a genuine test-first handoff rather
than a regression in an already-implemented tranche.

## Independent GREEN verification

After production delivered only `telemetry/startup_budget.{h,cpp}` and its code
source-group registration, the Release unit-test target was rebuilt in a
serialized MSBuild process:

```powershell
cmake --build build --config Release --target unittests -- /m:1 /nr:false /v:minimal
```

Exit code: `0`; the final link produced
`build/bin/Release/unittests.exe`.

The reviewed known-subtotal suite then passed with exit code `0`:

```powershell
.\build\bin\Release\unittests.exe --gtest_color=no --gtest_filter=TelemetryWp03KnownBudgetContract*
```

```text
[==========] Running 10 tests from 1 test suite.
[----------] 10 tests from TelemetryWp03KnownBudgetContract (0 ms total)
[==========] 10 tests from 1 test suite ran. (0 ms total)
[  PASSED  ] 10 tests.
```

This covers the independent native/metric arithmetic paths, invalid client
counts, exact logical counts, one/four-client subtotals, cap minus/equal/plus
one, each multiplication and both subtotal additions, zero publication on every
failure, and the exact eight-bit deferred mask (`0x00ff`). Every successful
result remained `is_complete=false` as required.

Portable review also compiled the production implementation under GCC 11.4
with strict warnings. Its syntax pass found that the test-only invalid-client
fixture originally mixed `unsigned int` literals with `size_t::max()` in an
initializer list; the final oracle uses an explicit
`std::array<std::size_t, 3>`. The Release target rebuilt and the same ten-test
selection remained GREEN after that portability correction.

### Regression verification

A single focused process selected configuration, pure and native identity,
protocol and the public initialization smoke. It passed `439/439` tests in
505 ms with exit code `0`. That count includes the immutable
`TelemetryProtocol*` regression (`394` tests), all 20 normal configuration
tests, 24 normal/native identity tests, and the public initialization test. The
two opt-in truncated-container bombs were then run explicitly with
`--gtest_also_run_disabled_tests`; both passed (`2/2`) with exit code `0`.

The complete current Release unit-test binary was also executed independently:

```text
[==========] 676 tests from 96 test suites ran. (3685 ms total)
[  PASSED  ] 676 tests.
YOU HAVE 2 DISABLED TESTS
```

The full suite generated its normal disposable settings/preset fixtures under
`test/test_data`; each resolved path was checked under that fixture root and
removed after the run. `git status --short -- test/test_data` was empty
afterwards.

### WP02 isolation and fast-path regression

The three isolated WP02 targets rebuilt together with exit code `0`:

```powershell
cmake --build build --config Release --target telemetry_initialize_contract_tests telemetry_initialize_failure_contract_tests telemetry_disabled_benchmark -- /m:1 /nr:false /v:minimal
```

The instrumented initialization oracle passed `1/1`. Allocation calibration
observed exactly five registration allocations, and fresh-process failure
indices `0` through `4` each reported `contract_check=PASS`; repeated
`initialize()` calls made no further registration attempt after failure.

Fresh baseline and disabled benchmarks each measured 100,000 callbacks. Both
reported zero tracked C++ allocations and passed their timing checks. The
disabled result measured mean `0.000074582 ms` and p99 `0.000100000 ms`; raw
samples were retained only under the ignored `build` tree.

## Explicitly open implementation and verification gates

- connect the now-GREEN pure WP03 known subtotal to the real startup decision;
- prove the session-ID registry allocation itself succeeds before bind, with
  injected allocation failure and zero socket/session publication;
- preallocate the concrete client slots, reassembly storage, transport buffers,
  reliable window storage, baselines, deltas, serialization scratch and metrics
  in their owning work packages;
- revalidate the 256 MiB provisional guard at WP06, WP08 and WP09 instead of
  silently treating deferred components as zero;
- exercise the real first-`EngineUpdate` startup path, including arithmetic and
  allocation failure, before any bind;
- retain failure injection, runtime-before-bind, rollback, unique diagnostic,
  high-water, benchmark, fuzz and soak gates as open until independently
  evidenced.
