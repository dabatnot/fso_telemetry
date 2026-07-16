# Phase 1 WP02 build-matrix evidence

Status: **PARTIAL / BLOCKED**. The local Windows producer subset is verified,
but this document does not close `P1-REQ-036`, `P1-AC-003`, or `G1-B`.

## Scope and conclusion

This evidence was recorded for the WP02 producer skeleton. It covers the
existing Win32 Release build, clean x64 Debug/FastDebug/Release builds, a
separate x64 tools-enabled build, and a separate x64 runtime-only build. It
also records the available WSL environment and the unavailable platform and
runtime evidence instead of treating them as successes.

Verified locally:

- Win32 Release links `unittests` and `Freespace2` with tests ON/tools OFF;
- x64 Debug, FastDebug, and Release link `unittests` and `Freespace2` with
  tests ON/tools OFF;
- the public and isolated WP02 initialize contracts pass in all three x64
  configurations;
- the allocation-failure initialize contract passes for every registration
  prefix in Win32 Release and x64 Release;
- x64 Release links `Freespace2` when tools are ON and tests are OFF;
- x64 Release links `Freespace2` in a separate runtime-only configuration with
  tests OFF and tools OFF.

Still blocked or external:

- the tools-enabled `ALL_BUILD` cannot complete with the installed VS2019
  toolchain because the downloaded Qt6 package requires VS2022;
- `strings_tool`, the repository's optional C++ tool target, is not generated
  because Clang is not installed;
- WSL has no CMake, Ninja, pkg-config, or Clang, so no Linux configure/build or
  dependency proof was possible without installing packages;
- macOS and ARM environments are unavailable;
- no usable game data or mission is present, so no real engine-frame or
  mission runtime claim is made;
- the WPR/ETW dynamic network trace remains unavailable.

The minimum matrix in Phase 1 section 05.3.3 requires Windows and non-Windows
CI platforms plus tools-enabled/tools-disabled coverage. Consequently, the
successful Windows rows below are evidence, not a gate-level PASS.

## Reproducibility identity

Recorded on `2026-07-16` in the `Europe/Paris` time zone.

| Item | Recorded value |
|---|---|
| Branch | `codex/telemetry-phase-1-wp01` |
| HEAD | `e433d19220bd7c136f2440b2702d05249d4c7302` |
| Tracked working diff as Git blob from `git diff --binary` | `9e883ed78ae36d68e79ce673ad9d1b7cf3c120a9` |
| `code/telemetry/telemetry.cpp` | 3,366 bytes; SHA-256 `1731369A1D26DD42CD647173E88BB56A01960188EC3972A7BAD9A10885652E32` |
| `code/telemetry/telemetry.h` | 91 bytes; SHA-256 `1B80B739B7B6B0849D71757AEBB4E62A1205D3605B0847854513C287B01F8DDA` |
| OS | Microsoft Windows 11 Professionnel `10.0.26200`, build `26200`, 64-bit |
| CPU | AMD Ryzen 5 2600 Six-Core Processor, 6 cores / 12 logical processors |
| CMake | `4.1.2` |
| Generator | Visual Studio 16 2019 |
| MSBuild | `16.4.0+e901037fe` |
| Compiler | MSVC `19.24.28315.0`; toolset path `14.24.28314` |
| Windows SDK | `10.0.10586.0` |
| Multi-config variants | `Debug;Release;FastDebug` |
| SIMD selection | host AVX2, host optimizations ON |

The tree was intentionally dirty with the Phase 1 implementation. The tracked
diff identity excludes untracked files, so the two production source hashes
above are recorded independently.

Fresh Windows build directories automatically downloaded and extracted the
repository's official prebuilt package
`bin-62c89ef6/bin-win64.zip` from the `scp-prebuilt` GitHub release during
configure. No package manager or manually added dependency was used.

## Windows matrix

`unittests` was fully built in every test-enabled row, but only the narrow WP02
filter was executed per configuration. The complete test suite was not rerun
for every configuration. `Freespace2` was linked but not launched because the
repository has no usable game data.

| Build directory | Platform/config | Relevant options | Built targets | Build result | Narrow WP02 result |
|---|---|---|---|---|---|
| `build` | Win32 Release | tests ON, tools OFF, FRED2 ON | `unittests`, `Freespace2`, nominal contract, failure contract | exit `0` | public 1/1, nominal 1/1, failure probe + 0..4 PASS |
| `build-wp02-win64-tests` | x64 Debug | tests ON, tools OFF, FRED2 OFF | `unittests`, `Freespace2`, nominal contract | exit `0` | public 1/1, nominal 1/1 PASS |
| `build-wp02-win64-tests` | x64 FastDebug | tests ON, tools OFF, FRED2 OFF | `unittests`, `Freespace2`, nominal contract | exit `0` | public 1/1, nominal 1/1 PASS |
| `build-wp02-win64-tests` | x64 Release | tests ON, tools OFF, FRED2 OFF | `unittests`, `Freespace2`, nominal contract, failure contract | exit `0` | public 1/1, nominal 1/1, failure probe + 0..4 PASS |
| `build-wp02-win64-tools` | x64 Release | tests OFF, tools ON, FRED2 OFF, qtFRED ON | `Freespace2` | exit `0` | no test target by design |
| `build-wp02-win64-tools` | x64 Release | same tools-enabled cache | `ALL_BUILD` | exit `1` | BLOCKED by Qt6/VS2019; `strings_tool` absent |
| `build-wp02-win64-runtime` | x64 Release | tests OFF, tools OFF, FRED2 OFF | `Freespace2` | exit `0` | no test target by design |

The existing Win32 build was incremental. All three `build-wp02-*` directories
were newly configured, separate x64 build trees. No existing build directory
was reconfigured for another architecture or option set.

### Configure commands

x64 tests ON/tools OFF:

```powershell
cmake -S . -B build-wp02-win64-tests -G "Visual Studio 16 2019" -A x64 `
  -DFSO_BUILD_TESTS=ON -DFSO_BUILD_TOOLS=OFF -DFSO_BUILD_FRED2=OFF
```

Final incremental configure exit: `0`. Cache evidence:
`CMAKE_GENERATOR_PLATFORM=x64`, `FSO_BUILD_TESTS=ON`,
`FSO_BUILD_TOOLS=OFF`, `FSO_BUILD_FRED2=OFF`.

x64 tools ON/tests OFF:

```powershell
cmake -S . -B build-wp02-win64-tools -G "Visual Studio 16 2019" -A x64 `
  -DFSO_BUILD_TESTS=OFF -DFSO_BUILD_TOOLS=ON -DFSO_BUILD_FRED2=OFF
```

Configure exit: `0`. CMake also recorded `FSO_BUILD_QTFRED=ON` and emitted
`Clang was not found, not building strings_tool`.

x64 runtime-only tests OFF/tools OFF:

```powershell
cmake -S . -B build-wp02-win64-runtime -G "Visual Studio 16 2019" -A x64 `
  -DFSO_BUILD_TESTS=OFF -DFSO_BUILD_TOOLS=OFF -DFSO_BUILD_FRED2=OFF
```

Configure exit: `0`. Cache evidence:
`CMAKE_GENERATOR_PLATFORM=x64`, `FSO_BUILD_TESTS=OFF`,
`FSO_BUILD_TOOLS=OFF`, `FSO_BUILD_FRED2=OFF`.

### Build commands

Win32 Release:

```powershell
cmake --build build --config Release --target `
  unittests Freespace2 `
  telemetry_initialize_contract_tests `
  telemetry_initialize_failure_contract_tests -- /m
```

Exit: `0`.

x64 Debug:

```powershell
cmake --build build-wp02-win64-tests --config Debug --target `
  unittests Freespace2 -- /m
cmake --build build-wp02-win64-tests --config Debug --target `
  telemetry_initialize_contract_tests -- /m
```

Exits: `0`, `0`.

x64 FastDebug:

```powershell
cmake --build build-wp02-win64-tests --config FastDebug --target `
  unittests Freespace2 telemetry_initialize_contract_tests -- /m
```

Exit: `0`.

x64 Release:

```powershell
cmake --build build-wp02-win64-tests --config Release --target `
  unittests Freespace2 `
  telemetry_initialize_contract_tests `
  telemetry_initialize_failure_contract_tests -- /m
```

Exit: `0`.

x64 tools ON:

```powershell
cmake --build build-wp02-win64-tools --config Release --target Freespace2 -- /m
cmake --build build-wp02-win64-tools --config Release --target ALL_BUILD -- /m
```

Exits: `0`, `1`. The first command proves that telemetry and the runtime link
with `FSO_BUILD_TOOLS=ON`. The second command is not a success; see the exact
blockers below.

x64 runtime-only:

```powershell
cmake --build build-wp02-win64-runtime --config Release --target Freespace2 -- /m
```

Exit: `0`.

### Narrow WP02 commands

For Win32 Release and each x64 test configuration, the public smoke was run as:

```powershell
& <bin-dir>\unittests.exe --gtest_filter=TelemetryProducerInitialize.*
```

Every invocation ran one test from one suite and exited `0`.

The isolated nominal contract was run as:

```powershell
& <bin-dir>\telemetry_initialize_contract_tests.exe
```

Every invocation ran one test from one suite and exited `0`.

For Win32 Release and x64 Release, the failure harness was run in six fresh
processes:

```powershell
& <bin-dir>\telemetry_initialize_failure_contract_tests.exe probe
& <bin-dir>\telemetry_initialize_failure_contract_tests.exe 0
& <bin-dir>\telemetry_initialize_failure_contract_tests.exe 1
& <bin-dir>\telemetry_initialize_failure_contract_tests.exe 2
& <bin-dir>\telemetry_initialize_failure_contract_tests.exe 3
& <bin-dir>\telemetry_initialize_failure_contract_tests.exe 4
```

All processes exited `0`. The calibration process observed exactly five
allocations and registration attempts `{1,1,1,1,1}`. Each injected process
failed allocation at its requested index, retained the expected registration
prefix after a second `initialize()`, and invoked only the successfully
registered callback prefix. The detailed RED/GREEN history remains in
[WP02_TEST_FIRST_EVIDENCE.md](WP02_TEST_FIRST_EVIDENCE.md).

## Windows artifacts

| Variant | Runtime artifact | Bytes | SHA-256 |
|---|---|---:|---|
| Win32 Release, tests ON/tools OFF | `build/bin/Release/fs2_open_26_1_0_AVX2.exe` | 15,675,904 | `E839DB0951D261D053D0415676B66B8B0857232F5C964CD4D093E51BE6576167` |
| x64 Debug, tests ON/tools OFF | `build-wp02-win64-tests/bin/Debug/fs2_open_26_1_0_x64_AVX2-DEBUG.exe` | 39,444,480 | `09B5010B5DC74FBF93ADB48198A943E82B2F0C9C924E6CDF3546D105B514C2C5` |
| x64 FastDebug, tests ON/tools OFF | `build-wp02-win64-tests/bin/FastDebug/fs2_open_26_1_0_x64_AVX2-FASTDBG.exe` | 23,131,648 | `434DA2A1EC807F0FC3457B64DE05C1B31CB439A6EF78B38A7C06D37E74308001` |
| x64 Release, tests ON/tools OFF | `build-wp02-win64-tests/bin/Release/fs2_open_26_1_0_x64_AVX2.exe` | 19,067,904 | `919ACA8F4D6D970ABAE71EB8BFE14B84BFFF142B0AD22DB035C0E6C8F2AE79A2` |
| x64 Release, tools ON/tests OFF | `build-wp02-win64-tools/bin/Release/fs2_open_26_1_0_x64_AVX2.exe` | 19,067,904 | `B314918C243D5915BCB5BB8745267F65B5303D430E36630E6B4BC9C9FC98B595` |
| x64 Release, runtime-only | `build-wp02-win64-runtime/bin/Release/fs2_open_26_1_0_x64_AVX2.exe` | 19,067,904 | `2613AF7A0E56381ADCF48357B5396D3BD8F4F145386091B87D684F745D139208` |

The three x64 Release executables have the same size but different hashes.
They were linked in separate build directories, so no byte-identity claim is
made.

Additional test artifacts:

| Variant | `unittests.exe` bytes | Nominal isolated bytes | Failure harness bytes |
|---|---:|---:|---:|
| Win32 Release | 16,455,680 | 730,112 | 112,128 |
| x64 Debug | 44,468,736 | 855,040 | not built |
| x64 FastDebug | 25,067,520 | 484,352 | not built |
| x64 Release | 21,025,280 | 338,944 | 22,528 |

MSVC emitted extensive existing C4244 warnings. Runtime links also emitted
manifest warning `81010002` for the `dpiAwareness` element. Neither warning was
promoted to an error in these configurations.

## Tools-enabled blockers

The tools-enabled cache contains 43 unique Visual Studio projects. It contains
`Freespace2`, `embedfile`, `qtfred`, and `qtfred_help`, but no
`strings_tool`, `unittests`, or WP02 isolated test project.

`strings_tool` was not attempted because CMake did not generate that target.
The configure output gives the exact cause:

```text
Clang was not found, not building strings_tool
```

`embedfile` built successfully, but it is the repository's always-on build
helper and is not evidence that the optional tools target built.

The tools-enabled runtime target linked successfully. The subsequent
`ALL_BUILD` failed with exit `1` while compiling `qtfred`. Multiple QtFRED
translation units reported the same fatal preprocessor error from the
downloaded Qt6 headers:

```text
Qt requires at least Visual Studio 2022 (MSVC version 19.30 or newer). Please upgrade.
```

The installed compiler is MSVC `19.24`. No alternate compiler was installed,
no CMake option was changed to suppress QtFRED, and no production change was
made to appease this platform. The tools-enabled gate is therefore BLOCKED,
even though the runtime itself passes in that cache.

## Non-Windows and architecture audit

The available WSL distro was inspected without installing anything:

| Item | Observed value |
|---|---|
| Distribution | Ubuntu `22.04.5 LTS` (Jammy) under WSL2 |
| Kernel/architecture | `5.15.167.4-microsoft-standard-WSL2`, `x86_64` |
| C++ compiler | GNU g++ `11.4.0` |
| Python | `3.10.12` |
| CMake | missing |
| Ninja | missing |
| pkg-config | missing |
| Clang | missing |
| Repository mount | `/mnt/d/david/Documents/Dev/fsotelemetry/fs2open.github.com` available |

Because pkg-config is absent, SDL2/OpenAL/PNG/JPEG/Jansson/Ogg/Vorbis/Theora/
FreeType/cURL dependency availability could not be established. Because CMake
and Ninja are absent, no `build-wp02-linux` configure or build was attempted.
No `sudo`, package installation, or WSL network operation was performed.

Linux is BLOCKED on the local environment, not failed by telemetry source.
macOS and ARM are EXTERNAL/UNAVAILABLE: no host, runner, cross-toolchain, or
emulator for either was present. Neither platform may be claimed from this
matrix.

## Game assets, frame metric, and runtime launch

The repository asset inventory found exactly one VP/mission-like file:

```text
test/test_data/cfile/list_files_in_vps_and_dirs/test.vp   290 bytes
```

No `.fs2` or `.fc2` mission and no retail/mod game data were found outside
generated build directories. The 290-byte VP is a CFile unit-test fixture, not
a playable data set. Consequently:

- no runtime executable was launched into a mission;
- no deterministic real engine-frame benchmark was possible;
- the required `< 1%` engine-frame median delta remains BLOCKED;
- no substitute callback-only number is represented as a frame result.

## Disabled fast path and ETW status

The separate Release callback benchmark remains valid for its narrower claim:
100,000 disabled callbacks completed with zero tracked recurring C++
allocations; mean `0.000074172 ms`, median `0.000100000 ms`, and p99
`0.000100000 ms`. The baseline mean was `0.000078256 ms` with the same median
and p99. Full method, raw CSV identities, and limits are in
[WP02_FAST_PATH_BENCHMARK.md](WP02_FAST_PATH_BENCHMARK.md).

Static evidence for that isolated executable remains:

- the generated project has zero `FSO_TELEMETRY_TEST_SEAMS` definitions and
  zero `code.vcxproj` references;
- the import-DLL audit reported only `KERNEL32`;
- the forbidden network/log symbol scan passed;
- disassembly of the disabled `on_engine_update` callback is a direct `ret`.

The dynamic WPR/ETW network trace did not start. WPR returned
`0xc5585011` (process exit `-984068079`) and final status reported that no trace
was recording. Static proof is retained, but dynamic ETW proof remains BLOCKED.

## Gate reconciliation

| Contract/gate item | Evidence state | Status |
|---|---|---|
| Windows Win32 Release producer/test linkage | built and narrow WP02 contracts run | VERIFIED |
| Windows x64 Debug/FastDebug/Release producer/test linkage | clean builds and narrow WP02 contracts run | VERIFIED |
| Release allocation-failure `noexcept` behavior | Win32 and x64 probe + fail indices 0..4 | VERIFIED |
| Runtime with tests OFF/tools OFF | separate x64 Release build | VERIFIED |
| Runtime with tools ON | separate x64 Release `Freespace2` target | VERIFIED |
| All generated tools-enabled targets | `ALL_BUILD` exit 1; Qt6 requires VS2022 | BLOCKED |
| Optional `strings_tool` | target not generated; Clang absent | BLOCKED |
| Linux build and WP02 smoke | build tools/dependency probe unavailable in WSL | BLOCKED |
| macOS and ARM builds | no environment | EXTERNAL |
| Real mission/runtime frame metric | no playable assets | BLOCKED |
| Dynamic ETW no-network trace | WPR start failure `0xc5585011` | BLOCKED |

Therefore the Windows subset supplies concrete evidence for WP02, but
`P1-REQ-036`, `P1-AC-003`, and `G1-B` remain open. Closing them requires at
least a supported non-Windows CI build, a viable tools toolchain or an approved
matrix decision for `strings_tool`/QtFRED, and the remaining runtime/performance
evidence. No documentation checkbox or phase gate should be marked complete
from this report alone.
