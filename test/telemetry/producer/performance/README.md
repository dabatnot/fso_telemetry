# Phase 1 performance raw-artifact layout

This directory is the stable P9.3 input/output layout for `P1-REQ-033`,
`D1-014`, and `P1-AC-014`.  It contains harness code and analysis rules, not
measured evidence.  A measured campaign writes its raw CSVs outside the
repository or into an explicitly reviewed evidence change, then invokes the
analyzer to produce a JSON report.

## Required raw workload files

The runner writes exactly these files to one directory:

- `baseline.csv` — production control without the module;
- `config-absent.csv` — module compiled, configuration file absent;
- `disabled.csv` — valid `enabled=false` configuration;
- `active-one-client.csv` — loopback, one negotiated client, after initial keyframe;
- `active-four-clients.csv` — four maximum-state clients, after initial keyframes;
- `would-block-loss-resync.csv` — repeated `WOULD_BLOCK`, loss and resync.

`telemetry_disabled_benchmark` produces the first three callback workloads in
fresh processes.  Its CSV schema is `sample_index,duration_ns` and it records
exactly 100,000 samples after 10,000 warm-up callbacks.  The active runner
uses the tick schema below.  The thirty-minute soak is deliberately excluded:
it is P1-WP-11 evidence.

## Active tick CSV schema

The header is exactly:

```text
sample_index,tick_duration_ns,collect_ns,diff_ns,state_image_build_ns,state_image_fill_ns,state_image_publish_validate_ns,state_image_adopt_ns,state_image_semantic_validate_ns,delta_build_ns,serialization_ns,network_ns,allocation_events,syscall_count,queue_depth,baselines_active,is_keyframe
```

All fields are non-negative decimal integers. `is_keyframe` is `0` or `1`.
The six `state_image_*`/`delta_build_ns` fields are optional diagnostic
subcomponents emitted by the current runner; the analyzer accepts legacy raw
files without them and includes their steady p99 values in `profile_p99_ms`
when present. The historical required fields retain their ordering and limits.
Rows marked `1` are reported separately and excluded from the active p99,
because the first keyframe has no invented timing threshold. `allocation_events`
is a cumulative production-owned allocation-observer counter. The steady
window must not increase it. `queue_depth` and `baselines_active` are captured
to make bounded-resource proof reproducible; the analyzer rejects more than
the workload's active-baseline total: one for `active-one-client`, four for
`active-four-clients`, and one for the single-client impairment scenario.
The value is deliberately a total because the production seam does not expose
per-client samples; each slot itself still owns at most one active baseline.

The active runner must call
`NativeSessionRuntimeTestAccess::begin_performance_observation(runtime)` after
native startup and warm-up, drive the actual `NativeSessionRuntime::service_tick`
path, then copy `last_performance_sample(runtime)` after every tick. The seam
reports monotonic `collect`, `diff`, serialization-egress, network-I/O and full
tick durations, plus production allocation observation, actual transport I/O
syscall count, output/reliable queue depth, active baselines and
candidate-keyframe state. It does not replace the
engine mission source or transport impairment fixture; those remain runner
inputs and must be recorded in the report metadata.

Run:

```powershell
& .\test\telemetry\producer\performance\analyze_phase1_performance.ps1 `
  -RawDirectory <directory> -OutputReport <report.json> `
  -Revision <git-revision> -BuildType Release -Compiler <compiler> `
  -Platform <platform> -Cpu <cpu> -PowerMode <mode> -Mission <mission> `
  -FlightHz 30 -StateBytes <bytes> -Duration <duration> -Warmup <warmup> `
  -Command <command-line>
```

The command validates the exact workload set, computes nearest-rank p99 from
raw samples, verifies disabled callback limits, active no-keyframe p99, and
steady allocation/baseline bounds, then writes a self-describing JSON report.
It does not invent a passing result for missing actual-mission, socket/syscall,
or impairment evidence.

`run_phase1_performance_campaign.ps1` is the reproducible campaign entry
point. It runs the three fresh disabled-process workloads through
`telemetry_disabled_benchmark`, invokes a supplied native-runtime runner for
the three active workloads, writes the six CSVs plus `campaign-manifest.json`,
and invokes the analyzer. Its default outputs are the stable `raw/` and
`reports/` subdirectories here; callers may parameterize both paths for a
clean evidence directory. The native runner must implement
`--workload --output --samples --warmup --flight-hz --seed` and use the real
`NativeSessionRuntime` fixture/seam described above. The campaign records
`native integration harness` as mission unless an actual engine mission was
run; it never fabricates a mission identity.
