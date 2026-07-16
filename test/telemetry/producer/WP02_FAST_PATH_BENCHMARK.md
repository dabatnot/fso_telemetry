# Phase 1 WP02 disabled fast-path benchmark

## Result

The isolated Release benchmark passes the two absolute callback limits and the
recurring C++ allocation check:

| Mode | Process setup | Mean | p99 | Tracked C++ allocations | Result |
|---|---|---:|---:|---:|---|
| baseline | no call to `telemetry::initialize()` | 0.000078256 ms | 0.000100000 ms | 0 | PASS |
| disabled | call `telemetry::initialize()` once | 0.000074172 ms | 0.000100000 ms | 0 | PASS |

Contract limits for the disabled callback are mean `<= 0.01 ms` and p99
`<= 0.05 ms`. The baseline and disabled runs were separate fresh processes.

This isolated harness does not run a deterministic engine mission/frame
workload, so the identical 100 ns callback medians are not represented as
proof of the required `< 1%` engine-frame delta. That formerly blocked
sub-proof was later measured independently and passes in
[WP02_FRAME_MEDIAN_BENCHMARK.md](WP02_FRAME_MEDIAN_BENCHMARK.md).

## Reproducibility identity

Recorded at `2026-07-16T09:02:55.9794595+02:00` (`Romance Standard Time`)
after the allocation-failure production correction and full WP02 rerun.

- branch: `codex/telemetry-phase-1-wp01`;
- HEAD: `e433d19220bd7c136f2440b2702d05249d4c7302`;
- tracked working diff, encoded as a Git blob from `git diff --binary`:
  `9e883ed78ae36d68e79ce673ad9d1b7cf3c120a9`;
- `code/telemetry/telemetry.cpp` SHA-256:
  `1731369A1D26DD42CD647173E88BB56A01960188EC3972A7BAD9A10885652E32`;
- `code/telemetry/telemetry.h` SHA-256:
  `1B80B739B7B6B0849D71757AEBB4E62A1205D3605B0847854513C287B01F8DDA`;
- instrumented contract test SHA-256:
  `6EDFC783569CAED73493E54F984ED139BC6D06DF61213A855884250287A54FB6`;
- allocation-failure contract harness SHA-256:
  `BFBDF71914EE16E23DD7BFDCE2F75A8B36E4B91297A70BA580B882358A5CD36B`;
- benchmark source SHA-256:
  `88CBF651A5627D073B5DD9E9F9320E80EDE4B9BE4B8706303A0A8839927ED16E`;
- benchmark Release executable SHA-256:
  `0855D5A4AD5611C3CCDF486F33620FC845E6681FA6A5A7D1054EFFF021F68BC1`.

The working tree was intentionally dirty with the in-progress WP01 and WP02
implementation. The tracked-diff identity excludes untracked files, so the
relevant untracked production and test sources are identified separately above.

## Environment

| Item | Recorded value |
|---|---|
| OS | Microsoft Windows 11 Professionnel, version `10.0.26200`, build `26200`, 64-bit |
| CPU | AMD Ryzen 5 2600 Six-Core Processor, 6 cores / 12 logical processors, reported max 3400 MHz |
| Power scheme | `381b4222-f694-41f0-9685-ff5bb260df2e` — `Utilisation normale` |
| CMake | `4.1.2` |
| Generator | Visual Studio 16 2019 |
| MSBuild | `16.4.0.56107` |
| Compiler | MSVC `19.24.28315.0`, x86 target |
| Configuration | Release, `/O2 /Ob2 /DNDEBUG`, LTO enabled, static MSVC runtime |
| Timing clock | `std::chrono::steady_clock`; executable import table contains `QueryPerformanceCounter` and `QueryPerformanceFrequency` |

No attempt was made to normalize background system load beyond recording the
environment and power scheme. The 100 ns timer quantization is visible in the
raw samples, including some zero-duration observations. Both limits are orders
of magnitude above that resolution floor.

## Isolated target and method

`telemetry_disabled_benchmark` compiles exactly:

- `test/src/telemetry/producer/telemetry_disabled_benchmark.cpp`;
- `code/telemetry/telemetry.cpp`;
- `code/events/events.cpp`.

The generated Visual Studio project contains zero
`FSO_TELEMETRY_TEST_SEAMS` definitions and zero references to `code.vcxproj`.
It links only the SDL header/runtime dependency needed by the engine headers.

Each mode performs 10,000 warm-up `EngineUpdate` emissions, preallocates the
100,000-element sample buffer, then enables a harness-owned global C++
allocation counter. The counter covers ordinary, array, nothrow, aligned, and
sized global `new`/`delete` forms. Timing and event emission then run for exactly
100,000 callbacks. Allocation tracking is disabled before sorting, formatting,
or writing the CSV. No test-only production seam is compiled into this target.

The baseline mode intentionally does not initialize telemetry and therefore
measures event-dispatch plus timer overhead with no listener. The disabled mode
initializes telemetry before warm-up and dispatches through its registered
disabled callback.

## Commands and raw summaries

Build:

```powershell
cmake --build build --config Release --target telemetry_disabled_benchmark -- /m:1 /v:minimal
```

Exit code: `0` after correcting the test harness's SDL `main` macro handling.
The first harness-only link attempt failed with unresolved `_main`; no production
change was made for that setup error.

Baseline fresh process:

```powershell
& .\build\bin\Release\telemetry_disabled_benchmark.exe baseline test\telemetry\producer\WP02_FAST_PATH_BASELINE_SAMPLES.csv
```

Exit code: `0`.

```text
mode=baseline
warmup_callbacks=10000
measured_callbacks=100000
mean_ms=0.000078256
median_ns=100
p99_ms=0.000100000
tracked_cpp_allocations=0
allocation_check=PASS
timing_check=PASS
raw_samples=test\telemetry\producer\WP02_FAST_PATH_BASELINE_SAMPLES.csv
```

Disabled fresh process:

```powershell
& .\build\bin\Release\telemetry_disabled_benchmark.exe disabled test\telemetry\producer\WP02_FAST_PATH_DISABLED_SAMPLES.csv
```

Exit code: `0`.

```text
mode=disabled
warmup_callbacks=10000
measured_callbacks=100000
mean_ms=0.000074172
median_ns=100
p99_ms=0.000100000
tracked_cpp_allocations=0
allocation_check=PASS
timing_check=PASS
raw_samples=test\telemetry\producer\WP02_FAST_PATH_DISABLED_SAMPLES.csv
```

## Raw samples

Both CSV files contain one header plus exactly 100,000 samples:

| Artifact | Lines | Bytes | SHA-256 |
|---|---:|---:|---|
| `WP02_FAST_PATH_BASELINE_SAMPLES.csv` | 100001 | 1042849 | `3CF007D564D12387D3D5A685B99C77B1503A1DB4FFA892E0354438C2E1AB4809` |
| `WP02_FAST_PATH_DISABLED_SAMPLES.csv` | 100001 | 1036115 | `5988D5499A3801529662C00068C113A7A1605298C4091291E20D86CAE5CC891B` |

A separate Python 3 calculation over the persisted CSVs reproduced the sample
count, mean, median, and nearest-rank p99 exactly:

```text
WP02_FAST_PATH_BASELINE_SAMPLES.csv 100000 mean_ms=0.000078256 median_ns=100 p99_ms=0.000100000
WP02_FAST_PATH_DISABLED_SAMPLES.csv 100000 mean_ms=0.000074172 median_ns=100 p99_ms=0.000100000
RECORDED_EXIT_CODE=0
```

## Allocation, socket, syscall, and log evidence

The allocation counter observed zero global C++ allocation calls in both
measured loops. The Release binary was then inspected with Microsoft COFF/PE
Dumper `14.24.28315.0`:

```powershell
dumpbin.exe /imports build/bin/Release/telemetry_disabled_benchmark.exe
dumpbin.exe /disasm:nobytes build/bin/Release/telemetry_disabled_benchmark.exe
```

The import table contains only `KERNEL32.dll`; a case-insensitive scan for
`WS2_32`, `WSOCK32`, `socket`, `send`, `sendto`, `recv`, `recvfrom`,
`OutputDebugString`, `EventWrite`, and `TraceLogging` returned no match.
File and console imports belong to the harness's post-measurement CSV/report
output and are not used by the callback.

The optimized callback disassembly is exactly:

```text
`anonymous namespace'::on_engine_update:
  00424C00: ret
```

That build-specific instruction body contains no call, allocation, syscall,
socket, timer, or log operation. It is stronger for this exact binary than an
import-table inference alone.

Windows Performance Recorder `10.0.26100` and `xperf` are installed, but a
separate, non-timed Network-profile attempt could not start:

```text
Failed to enable the policy to profile system performance.
Profile Id: Network.Verbose.File
Error code: 0xc5585011
WPR_START_EXIT=-984068079
```

Runtime ETW evidence is therefore **BLOCKED by local profiling policy**. No WPR
session remained active, and its failed start did not contaminate either timed
process. Static import and disassembly evidence remains available as recorded
above.

## Scope conclusion

This report proves the WP02 isolated disabled-callback mean, p99, and recurring
C++ allocation checks for the identified Release/x86 binary. It also provides
static binary evidence for the absence of callback socket/syscall/log work.
It does not itself close the engine-frame median-delta requirement,
build-variant matrix, mission scenario, or later Phase 1 performance gates.
The independent real-engine measurement is linked above; the two reports keep
their distinct workloads and claims separate.
