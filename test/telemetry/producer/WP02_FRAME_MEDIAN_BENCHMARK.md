# Phase 1 WP02 real engine-frame median benchmark

Status: **PASS** for the `P1-REQ-033` engine-frame median sub-requirement.
The absolute median delta is `0.257409%`, below the strict `< 1%` limit.

## Result

| Metric | Control without WP02 | Phase 1 WP02 disabled | Signed delta | Absolute delta | Gate |
|---|---:|---:|---:|---:|---|
| Pooled `MainFrameTimer` median | 8.857500 ms | 8.834700 ms | -0.257409% | 0.257409% | PASS |
| Median of six per-run medians | 8.862600 ms | 8.829000 ms | -0.379121% | 0.379121% | supporting PASS |

The primary calculation is:

```text
100 * (phase1_pooled_median - control_pooled_median)
    / control_pooled_median
= 100 * (8,834,700 ns - 8,857,500 ns) / 8,857,500 ns
= -0.257409%
```

The gate uses the absolute value. The negative result is measurement variation
in favor of the Phase 1 condition; it is not claimed as a performance
improvement.

This report closes only the real engine-frame median sub-proof. The separate
[disabled fast-path report](WP02_FAST_PATH_BENCHMARK.md) covers the 100,000
callback mean, p99, and recurring allocation checks. Neither report by itself
closes every later Phase 1 performance requirement.

## Reproducibility identity

The timed series ran from `2026-07-16T12:38:40+02:00` through
`2026-07-16T12:42:35+02:00` in `Europe/Paris`.

| Item | Recorded value |
|---|---|
| Branch | `codex/telemetry-phase-1-wp01` |
| Production revision used by both binaries | `ac325072357e5ad7df661a7658628b1beb2f5312` (`Implement telemetry Phase 1 protocol and scaffold`) |
| Later CI revision | `e1ecd51f36e2e4565ea1fd2f255840c0a26e7cc8`; its only changes from the timed revision are two protocol test sources |
| Phase 1 binary | 15,675,904 bytes; SHA-256 `E839DB0951D261D053D0415676B66B8B0857232F5C964CD4D093E51BE6576167` |
| Control binary | 15,674,368 bytes; SHA-256 `82A55139068899341BDECF7425ED637CA650591488F978AC22A2A4B452EE4BC0` |
| Control source diff as Git blob | `000b4859ba9a6efb5e6ea7620603dd7291950b2e` |
| Seed | `424242` |

The control worktree was detached at the same production revision. Its only
source changes removed the six-line Telemetry group from
`code/source_groups.cmake`, the telemetry include from
`freespace2/freespace.cpp`, and the `telemetry::initialize()` call from
`game_init()`. Therefore the control excludes the WP02 module and seam while
retaining the rest of the same revision.

The Phase 1 binary is the normal production build: the telemetry module is
compiled, `game_init()` calls `telemetry::initialize()`, and
`FSO_TELEMETRY_TEST_SEAMS` is not defined. No telemetry configuration file was
present in the sandbox. At WP02, before the WP03 configuration parser exists,
the module's production default is `telemetry_disabled = true`; the measured
condition is consequently config-absent and disabled.

## Host and build

| Item | Recorded value |
|---|---|
| OS | Microsoft Windows 11 Professionnel `10.0.26200`, build `26200`, 64-bit |
| CPU | AMD Ryzen 5 2600, 6 cores / 12 logical processors, reported maximum 3.4 GHz |
| Memory | 33,484,004 KiB visible; 18,717,516 KiB free at environment capture |
| GPU | NVIDIA GeForce GTX 1660 SUPER, driver `32.0.15.9186` |
| Power plan | Windows `Balanced` / `Utilisation normale`, GUID `381b4222-f694-41f0-9685-ff5bb260df2e` |
| Generator | Visual Studio 16 2019, Win32, `host=x64` |
| Compiler | MSVC `19.24.28315`; toolset `14.24.28314`; platform toolset `v142` |
| Windows SDK | `10.0.10586.0` |
| Build | Release, AVX2, whole-program optimization and link-time code generation enabled |

Both executables were built with the same generator, architecture, Release
configuration, AVX2 selection, SDK, compiler, and LTO settings. The benchmark
used `-no_vsync` and `-nosound` to remove display synchronization and audio
timing from the comparison.

## Deterministic workload

The retail FreeSpace 2 installation supplied the normal VP assets. The local
mission was derived from the retail FRED documentation mission
`data/freddocs/shipyard-completed.fs2`:

| Artifact | SHA-256 |
|---|---|
| Unmodified retail source mission | `533359AB5CB9EB01506135BA6C40AD28AA02F248A277EC6995C099DF0862DA97` |
| Local benchmark workload | `54F3AA70E2B26DC62154A3403F2F76D2075FF10019998C04AF0AF1DE5109FAE0` |

The deterministic transformation renamed the mission, set mission flag
`1024` (no briefing), enabled scramble mode, made the player start
invulnerable, and inserted a first event that calls `end-mission` after 15
seconds. The fixed stop precedes the original hostile arrival at 23 seconds.
The runner supplied seed `424242` and restored the same pilot JSON, campaign
save, and HUD profile before every process.

The retail mission and game assets are copyrighted inputs and are not
redistributed in this repository. Only the native frame-timer samples,
workload identities, and exact transformation recipe are archived.

## Execution protocol

Each binary received the same command-line arguments:

```text
-parse_cmdline_only -no_vsync -nosound -window -window_res 640x480
-no_unfocused_pause -portable_mode -benchmark_mode -profile_write_file
-pilot david -start_mission wp02_frame_benchmark -seed 424242
-noninteractive
```

`-parse_cmdline_only` is essential: it prevents user- or installation-level
`cmdline_fso.cfg` files from injecting unrelated flags. The resolved command
line in `multi.log` contained only the arguments above.

One normalized warm-up process per condition was completed before timing and
was not counted. The twelve measured processes used this balanced order:

```text
control, phase1, phase1, control,
phase1, control, control, phase1,
control, phase1, phase1, control
```

Before the mission began, the runner restored and focused the 640x480 window,
performed one identical center click, and removed the temporary topmost state.
This happened before gameplay profiling and prevented an unfocused/minimized
window from stalling one condition. Every counted process exited `0` and
produced a non-empty `profiling.csv`.

An earlier exploratory twelve-run series is intentionally excluded and not
archived. It was collected before `-parse_cmdline_only` was added, so global
command-line configuration contaminated that series. Early OS firewall prompt
attempts and all smoke runs are likewise excluded.

## Analysis rule

The native `-profile_write_file` output is a headerless
`timestamp_ns;MainFrameTimer_duration_ns` stream. For every run, the analyzer:

1. verifies the file name, balanced order, strictly increasing timestamps,
   positive durations, and minimum sample count;
2. discards the first 200 samples as the per-process warm-up;
3. discards the last sample, which is the short terminal partial frame;
4. pools all remaining frame durations within each condition;
5. computes the signed and absolute pooled-median delta;
6. also computes the median of six per-run medians as a run-balanced check.

There is no normative minimum frame count in `P1-REQ-033`. This series retains
between 1,501 and 1,514 analyzed frames per run. Recompute the archived proof
with:

```powershell
& .\test\telemetry\producer\analyze_frame_benchmark.ps1
```

The analyzer exits nonzero if the raw set is malformed, the order changes, or
the strict absolute pooled delta is not below 1%.

## Per-run evidence

| Run | Condition | Raw n | Analyzed n | Warm-up s | Capture s | Analyzed s | Median ms | Raw CSV SHA-256 |
|---:|---|---:|---:|---:|---:|---:|---:|---|
| 01 | control | 1,710 | 1,509 | 1.711919 | 14.684664 | 12.972667 | 8.860100 | `B483984CCA9DE09A18FF3F66C38277F3D1AF75F4EB7BA3424119A20CD13CD9C7` |
| 02 | phase1 | 1,708 | 1,507 | 1.736868 | 14.696283 | 12.959349 | 8.838300 | `CAD94BEED38ACCC44DE5769A3B7449062F9E7FAB678889A5DEA2DB6E1EEAA26D` |
| 03 | phase1 | 1,712 | 1,511 | 1.716153 | 14.693871 | 12.977447 | 8.852700 | `21A96773FC4A0EF75C604C65B1D7552021E451E5D0A0018EAD72AD5955F71D52` |
| 04 | control | 1,702 | 1,501 | 1.730864 | 14.686106 | 12.955143 | 8.891700 | `E67918F81256A19D3293A5A895E83C42DD6D9F1552E4A311E35BE691611198E3` |
| 05 | phase1 | 1,714 | 1,513 | 1.714830 | 14.693544 | 12.978620 | 8.819700 | `8BD61BD3F60E2DD2AC6FD97A97EB4BAD4A7E61CBFBA016A12CC5174176865B17` |
| 06 | control | 1,706 | 1,505 | 1.717269 | 14.687534 | 12.970167 | 8.865100 | `CB376C14466F7944C814CA423BAA15D9FD2862383B673AA00FBD410484F2D9A4` |
| 07 | control | 1,715 | 1,514 | 1.694258 | 14.694543 | 13.000193 | 8.819950 | `E9FC98A7C36B7D1FF011F3527B233D63EC99F77D8376DE09D257C313B157F629` |
| 08 | phase1 | 1,708 | 1,507 | 1.728264 | 14.690239 | 12.961914 | 8.851700 | `8C5943D17CC4DC128595AF59DD9AFCAEB4CED04E2393C225E3E745F5557CE189` |
| 09 | control | 1,704 | 1,503 | 1.724708 | 14.689177 | 12.964382 | 8.890000 | `E23BD5E4BF1C403D3F1A75D4F181DF6F897D74042FEA92E0FBBD2EC564D9540A` |
| 10 | phase1 | 1,711 | 1,510 | 1.732072 | 14.691323 | 12.959188 | 8.819000 | `280C633FC2DC765A4303E8F9F272742CC03B193361B18B36EF7621FB8187EFD6` |
| 11 | phase1 | 1,713 | 1,512 | 1.711235 | 14.678130 | 12.966803 | 8.818050 | `AEDA1BD11D29182EFB12A4DA28FD4A4754BB46ED15BD99D1D7CA8DF7D6B4402D` |
| 12 | control | 1,715 | 1,514 | 1.725777 | 14.693755 | 12.967880 | 8.791400 | `CB87207BF86A49EB564CED5F62C00222224085C64ED5D987C24957631E289235` |

## Aggregate evidence

| Condition | Runs | Raw n | Analyzed n | Capture s | Analyzed s | Pooled median | Median of run medians | Run-median range |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| control | 6 | 10,252 | 9,046 | 88.135779 | 77.830431 | 8.857500 ms | 8.862600 ms | 8.791400–8.891700 ms |
| phase1 | 6 | 10,266 | 9,060 | 88.143389 | 77.803321 | 8.834700 ms | 8.829000 ms | 8.818050–8.852700 ms |
| total | 12 | 20,518 | 18,106 | 176.279168 | 155.633752 | — | — | — |

The twelve raw CSV files are stored in
[`frame-benchmark/`](frame-benchmark/). The directory marks the CSVs `binary`
so Git preserves the native CRLF evidence bytes on every platform and does not
apply text-diff whitespace checks to the raw stream. The analysis script
recomputes and prints each archived SHA-256 with the gate calculation.
