# Repository agent instructions

## Build and test concurrency

- A single agent owns compilation and test execution at any given time.
- Before starting CMake, MSBuild, `cl.exe`, `link.exe`, or a test runner, check that no other agent-owned build or test process is still active.
- Never launch builds concurrently from multiple agents, even for different targets or configurations. Ask the current build owner for its result or wait for it to finish.
- Reuse the existing configured build tree and already-built artifacts when their timestamps prove they include the required sources.
- On Windows, pass `/m:1 /p:CL_MPCount=1` to MSBuild invocations, including arguments forwarded by `cmake --build`. `/m:1` alone does not disable the compiler's `/MP` fan-out. Monitor the process tree during long builds and stop the exact agent-owned build tree if more than one `cl.exe` appears.
- If an interrupted task leaves build processes behind, identify the exact agent-owned process tree before stopping it. Never terminate unrelated user processes.
- Do not run performance measurements while any compiler, linker, test runner, or other agent workload is active.
