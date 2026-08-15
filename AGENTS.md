# Repository agent instructions

## Product scope

- The telemetry product is cockpit-only. `TrustedFullState` is permanently
  abandoned and must not be restored, specified, implemented, migrated, or
  kept as a compatibility option.
- New telemetry data must be justified by a concrete simpit cockpit need and
  added to the existing `CockpitSensors` projection with producer-side
  visibility filtering.
- A dashboard, replay tool, test harness, or diagnostic need does not justify
  exposing hidden mission state or adding an omniscient profile.

## Development process

- The repository has no phase gate, proof ledger, evidence tracker, freshness
  fingerprint, certification workflow, or mandatory phase-completion state.
- Automated unit/integration tests and a manual in-game check are sufficient.
  Test results are reported plainly; they are never copied into a proof
  registry or used to calculate eligibility.
- The user owns product direction and may start, skip, reorder, explore, or
  continue a phase even when tests are failing or a previous phase is
  unfinished. Agents must report known failures and risks, then follow the
  user's explicit decision.
- Do not recreate a tracker, gate, scoring system, proof workflow, mandatory
  checklist, or mechanism that blocks later work based on earlier validation.

## Build and test concurrency

- A single agent owns compilation and test execution at any given time.
- Before starting CMake, MSBuild, `cl.exe`, `link.exe`, or a test runner, check that no other agent-owned build or test process is still active.
- Never launch builds concurrently from multiple agents, even for different targets or configurations. Ask the current build owner for its result or wait for it to finish.
- Reuse the existing configured build tree and already-built artifacts when their timestamps prove they include the required sources.
- On Windows, pass `/m:1 /p:CL_MPCount=1` to MSBuild invocations, including arguments forwarded by `cmake --build`. `/m:1` alone does not disable the compiler's `/MP` fan-out. Monitor the process tree during long builds and stop the exact agent-owned build tree if more than one `cl.exe` appears.
- If an interrupted task leaves build processes behind, identify the exact agent-owned process tree before stopping it. Never terminate unrelated user processes.
- Do not run performance measurements while any compiler, linker, test runner, or other agent workload is active.
