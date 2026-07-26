---
name: implement-telemetry-phase
description: Implement or resume a numbered FS2Open telemetry roadmap phase from the exhaustive eight-document specification under documentation/analysis/specs with mandatory independent roles for contract tracking, production implementation, testing, and review. Use when the user asks to implement, continue, resume, pause, execute, or complete a telemetry phase or work package. Run a fast targeted inner loop, batch role handoffs, persist machine-validated progress, preserve Phase 0 wire compatibility and phase boundaries, and reserve broad Release evidence and certification campaigns for explicit work-package checkpoints and gates.
---

# Implement a telemetry phase

Implement the requested phase from its specification. Treat the eight documents as the contract and Phase 0 as the frozen wire authority.

Read [references/implementation-contract.md](references/implementation-contract.md) completely before changing files. Use [scripts/inspect_phase_contract.ps1](scripts/inspect_phase_contract.ps1) to inventory the contract.

## 0. Select a profile and cadence

Use `balanced` unless the user explicitly requests certification/release evidence, a final phase-complete claim, or a gate whose contract assigns certification-only campaigns. An ordinary intermediate WP gate remains `balanced`. State the selected profile in the first progress update.

| Profile | Use for | Required proof |
|---|---|---|
| `balanced` (default) | normal implementation/correction and ordinary intermediate WP gates | Fast Debug inner loops; one batched readiness review; Release, affected integration, one broad suite, and tracker reconciliation only when the parent work package is cohesive. |
| `certification` | final phase closure, user-requested release evidence, public boundary changes, or gates assigned long campaigns | full evidence sequence, all required variants, long-running campaigns, and final audit. |

Always use `certification` for an actual Phase 0 wire/schema/vector change, a changed trust/security boundary, a changed cross-thread ownership boundary, or a gate that explicitly requires fuzz/soak/performance/platform proof. A defect fix inside an unchanged boundary remains `balanced` with the narrow high-risk oracle plus its normal WP checkpoint. A `balanced` run may close an ordinary intermediate gate but may not claim final phase completion or a certification-only gate.

An internal slice is not a work package merely because it adds a helper, test file, or lifecycle seam. Keep a single compliance matrix and group dependency-coupled internal slices under their parent `P<N>-WP-nn`.

Use these four cadences. Read [references/execution-cadence.md](references/execution-cadence.md) before choosing commands:

| Cadence | Trigger | Maximum normal scope |
|---|---|---|
| `inner-loop` | every implementation slice | dedicated Debug build, narrow contract tests, changed-file checks |
| `readiness-review` | once a cohesive parent WP is code/test complete | independent review before expensive evidence |
| `wp-checkpoint` | readiness review has no blocking finding | changed-target Release, affected integration, one broad suite, progress reconciliation |
| `phase-certification` | final/certification-only gate or assigned campaign | exact contract evidence, variants, hashes, fuzz/performance/soak as applicable |

In `balanced`, do not run a full Release/LTO build, whole-suite CI, artifact hashing, sidecar generation, engine launch, or gate-report rewrite during `inner-loop`. A high-risk finding expands the narrow proof needed for that risk; it does not automatically authorize the entire certification sequence.

### Balanced delivery budget

Treat `balanced` as a delivery budget, not a lighter certification campaign:

- one dependency-coherent implementation batch before readiness;
- one batched readiness review returning every blocking finding;
- one consolidated correction batch and one focused rereview;
- one Release checkpoint build per invalidated target;
- one broad suite only when the contract or changed dependency cone requires it;
- one tracker reconciliation and one final report seal.

Do not silently start a third review/correction cycle. First apply the scope firewall below. If a directly blocking contract defect still remains, report the root cause and obtain a coordinator decision before continuing.

### Scope firewall

Classify every new failure before assigning work:

1. **Changed-contract regression** — caused by the current production/test diff and violates an owning requirement or acceptance criterion. Fix it in the current WP.
2. **Required-proof defect** — the exact contract-required oracle is false, vacuous, or cannot exercise the changed seam. Repair only that oracle or its dedicated target.
3. **Adjacent harness debt** — a broader or legacy test fails outside the changed dependency cone, or asks for stronger proof than the contract. Record it as nonblocking follow-up; do not expand the WP.
4. **Specification conflict** — normative sources disagree. Stop the affected WP and ask for a decision.

A reviewer must cite the violated requirement, acceptance criterion, or required evidence row for every blocking finding. “Could be stronger”, platform completeness not assigned to this gate, lexical purity, generalized harness cleanup, and unrelated broad-suite failures are not blockers in `balanced`.

Prefer a dedicated target, fixture adapter, or scoped test seam over repository-wide harness migration. A global mechanical migration is allowed only when the current change makes the required checkpoint impossible, no narrower proof is valid, the affected sites are enumerated once, and the coordinator approves one bounded batch. Do not turn each migrated site or intermediate symptom into a separate blocker or review cycle.

Invalidate evidence by dependency, not chronology. A later edit makes evidence stale only when it changes production, test code, build inputs, configuration, or artifacts used by that evidence.

## 1. Resolve and validate the phase contract

1. Locate the repository root and `documentation/analysis/specs/<N>-*/` for the requested phase. Require exactly one matching directory.
2. Require `README.md` and one document for each prefix `01` through `07`.
3. If available, run the specification validator from:

   ```text
   .agents/skills/prepare-telemetry-phase-spec/scripts/validate_phase_specs.ps1
   ```

4. Run this skill's inventory script from the repository root:

   ```powershell
   & '<skill-directory>\scripts\inspect_phase_contract.ps1' -PhaseNumber <N>
   ```

5. Read all eight target-phase documents completely. Read Phase 0 and every lower completed phase completely before implementation.
6. Read the target roadmap section and shared testing, observability, risk, and ordering sections.
7. Initialize or validate `documentation/analysis/progress/phase-<N>.json` with the bundled progress scripts. Treat it as status, never as proof.

Do not implement from a roadmap summary when the phase specification is absent or mechanically invalid. Ask the user to prepare or repair it with `$prepare-telemetry-phase-spec` first.

If two normative passages conflict, stop the affected work package. Do not choose whichever is easiest to code. Report the exact conflict and request a specification decision.

## 2. Orient the repository and preserve user work

Before editing:

1. Read repository instructions such as `AGENTS.md`.
2. Inspect `git status --short`, relevant CMake files, test conventions, adjacent modules, APIs, and platform abstractions.
3. Verify every path or symbol described as existing by the spec. Treat proposed paths as proposals.
4. Identify overlapping user changes and work around them. Never discard, reset, overwrite, or reformat unrelated work.
5. Do not change Git remotes, hooks, config, branches, staging, commits, or pushes unless explicitly requested. Never push to the official upstream repository.

Prefer repository-native types, ownership conventions, error handling, logging, test frameworks, and build patterns. Avoid introducing a parallel framework when an established abstraction satisfies the contract.

## 3. Orchestrate independent subagents

Require multi-agent execution. Do not implement a phase as a single agent. Keep the main agent as coordinator and create separate subagent identities for these roles:

| Role | Exclusive responsibility | Forbidden responsibility |
|---|---|---|
| Contract tracker | Inventory requirements, decisions, work packages, gates, acceptance criteria, dependencies, and evidence status. Reconcile the compliance matrix at work-package checkpoints and before a gate claim. | Do not edit production code or tests, run acceptance tests as their owner, or review code quality. |
| Test agent | Design and edit tests, fixtures, fuzz targets, harnesses, test-only build registration, and execute the required verification commands. Report exact commands and raw results. | Do not edit production code, approve requirement coverage, or review its own test adequacy as the final reviewer. |
| Implementer | Edit production code and production build integration for one bounded work package. Perform only compilation, formatting, or static checks needed to prepare the handoff. | Do not create or change tests, execute verification suites, mark evidence verified, or review its own implementation. |
| Reviewer | Independently review the specification slice, production diff, test diff, and raw test evidence. Report findings without editing files. | Do not fix production or test code and do not act as tracker or test owner. |

Keep every role on a different agent for the entire phase. Never rename or reuse one agent under another role. Keep the coordinator distinct from all four roles; the coordinator owns assignments, file boundaries, handoffs, and final reporting but does not substitute for a role or edit production/test code.

Create additional specialist agents when the contract requires security, protocol, build/packaging, platform, performance, fuzzing, or licensing expertise. Keep each specialist independent from the author whose work it evaluates.

Respect these orchestration rules:

1. Let only the coordinator create and assign role agents. Give each agent one bounded role, the relevant specification sections, inherited contracts, paths, requirement IDs, and expected return format.
2. Keep at most one parent work package in progress and one writer at a time. Parallelize read-only analysis only when it cannot become stale; serialize overlapping file-writing handoffs.
3. Use the shared working tree as the handoff artifact. Before and after every writer handoff, inspect the focused diff and preserve unrelated user changes.
4. Return findings to the owning role: production fixes to the implementer, test fixes to the test agent, and mapping/evidence corrections to the tracker. Never let a reviewer repair the work it reviewed.
5. If agent capacity is limited, schedule roles sequentially without collapsing them. If independent subagents are unavailable, stop before implementation and report the phase blocked by the required segregation of duties.

In `balanced`, keep the same four agents for the entire phase and reuse them in their fixed roles. Batch dependency-coupled internal slices. Do not require a fresh multi-agent ceremony, reviewer pass, tracker reconciliation, or evidence manifest for every helper or small slice.

Use compact handoffs. Each role reports only changed paths, exact failing or passing commands, requirement IDs, blockers, and the next owner. Do not restate the phase history or reproduce already persisted evidence at every handoff.

## 4. Build the compliance matrix, progress tracker, and plan

Create a working matrix containing every:

- requirement ID such as `P1-REQ-001`, `P1-F-001`, or `P1-NF-001`;
- decision ID `D1-*` that constrains implementation;
- work package `P1-WP-*` (or legacy Phase 0 `P0.x`) and gate `G1-*`;
- acceptance criterion `P1-AC-*`;
- unchecked completion item from document 07.

For each row, record the owning code path, test or other proof, dependencies, and current status: `pending`, `implemented`, `verified`, `deferred`, or `blocked`.

Have the contract tracker translate document 07's dependency graph into the task plan. Have the coordinator assign the smallest dependency-complete parent work package; use internal slices only to sequence its dependencies. Do not start a later phase merely because its seam is nearby.

Persist the user-visible summary in `documentation/analysis/progress/phase-<N>.json` according to [references/progress-schema.md](references/progress-schema.md). Use:

```powershell
& '<skill-directory>\scripts\initialize_phase_progress.ps1' -PhaseNumber <N>
& '<skill-directory>\scripts\validate_phase_progress.ps1' -PhaseNumber <N>
& '<skill-directory>\scripts\show_phase_progress.ps1' -PhaseNumber <N>
```

Update the tracker after a meaningful implementation batch, at readiness review, at every WP checkpoint, on gate reopen/closure, and before a pause. Do not update it after every command, individual finding, retry, or role message. A `verified` row requires fresh evidence; stale evidence moves the row back to `implemented` or `blocked`.

Keep one blocker per root cause, not per symptom, file, test, or review iteration. Keep only conclusive evidence and the latest diagnostic that explains an active blocker in the tracker. Raw attempts and superseded command history belong in transient logs or the final gate report when the contract requires them.

## 5. Implement by gate

For each parent work package:

1. Have the tracker return the work-package contract slice, dependency state, requirement rows, acceptance criteria, and unresolved questions. Stop on any normative conflict.
2. Have the test agent read the owning spec sections and inherited Phase 0 definitions, then write or update the narrowest contract tests first when bytes, parsing, IDs, lifecycle, concurrency, visibility, or failure behavior are involved. Capture the expected failing result when practical.
3. Give the implementer the contract slice and raw failing evidence. Implement production code without copying engine structures or ABI layouts into the public contract.
4. Keep authoritative engine reads on the thread required by the spec. Pass copied public values across queues; never expose engine pointers or mutable handles.
5. Validate untrusted sizes, counts, offsets, enums, UTF-8, arithmetic, quotas, and session context before allocation or mutation.
6. Preserve disabled-mode and failure-mode behavior. Optional communication or video capabilities must not break canonical telemetry.
7. Keep buffers, queues, caches, retransmission windows, logs, and metric cardinality bounded.
8. Return control to the test agent. For every internal slice, run only the narrow Debug/contract checks and preserve the oracle. Route production failures back to the implementer and test defects back to the test agent.
9. When the parent work package is cohesive, have the reviewer perform one batched readiness review of the contract slice, focused production/test diffs, and targeted results before any expensive Release or evidence campaign. Require each blocking finding to cite its contract row and route all findings together.
10. Fix readiness findings in one consolidated batch. Run one focused rereview covering only those dispositions. Do not let the rereview introduce new proof standards or adjacent cleanup unless it identifies a changed-contract regression that could not reasonably have been observed in the first review.
11. After readiness findings are fixed and narrow tests are green, run the WP-checkpoint Release and integration evidence. Have the reviewer inspect only the changed disposition and fresh evidence, then reconcile the tracker.
12. A third review/correction loop in `balanced` requires a coordinator decision. Default to deferring adjacent harness debt and noncontractual improvements. Continue only for a cited changed-contract regression, specification-required proof defect, or new defect introduced by the consolidated correction batch.
13. Have the tracker reconcile every matrix row at the checkpoint. Mark a row `verified` only after its specified independent proof succeeds and no blocking review finding remains.

Never change a Phase 0 field layout, numeric registry, timeout, reliability rule, baseline semantic, capability, or visibility guarantee as an incidental implementation fix. A required wire change is a specification/versioning blocker.

## 6. Verify progressively

Run checks in proportion to the profile and change risk. Do not substitute an unrun required final proof with a shorter run.

For every internal slice, run formatter/static checks on changed files, the dedicated Debug target, the narrow contract tests, and `git diff --check`. Prefer a dedicated target that does not relink the monolithic `unittests` or engine binary. If no narrow target exists, add or repair the test-only target as part of the WP instead of repeatedly paying a broad link.

At a `balanced` parent work-package checkpoint, run the changed-target Release build, unit/contract tests, directly affected integration/lifecycle harnesses, and at most one broad CI workflow when the contract or changed dependency cone requires it. Do not run a broad suite merely because test-only fixtures or a dedicated target changed. Run a full local suite only when production changes shared protocol/runtime/build integration used outside the WP, or the gate explicitly assigns that proof.

Run at most one broad Release/full-suite attempt per readiness-approved checkpoint unless it reveals a real defect. After a defect, rerun the narrow reproducer first and repeat only the broad proof invalidated by the fix. Do not retain every inconclusive attempt in the final gate report; retain raw logs if required and record the final conclusive command plus a concise supersession note.

Before repeating a build, compare its production, test, CMake, configuration, and generated inputs with the last successful build. Reuse valid artifacts when those inputs are unchanged. Never relink the monolithic unit-test binary solely to refresh tracker metadata, hashes, reports, or reviewer prose.

In `certification`, and only then when applicable, run the complete sequence below:

1. formatter/static checks limited to changed files;
2. unit and contract tests;
3. golden vectors and independent decoder comparisons;
4. integration and lifecycle harnesses;
5. loss, duplication, reordering, timeout, and resynchronization scenarios;
6. supported build variants and dependency-disabled variants;
7. sanitizers, fuzzing, packaging, and licensing checks;
8. performance/soak measurements using the exact duration and threshold from the spec;
9. `git diff --check`, focused diff review, and final `git status --short`.

Use the repository's documented build and test commands. Keep test execution and evidence ownership with the test agent or a distinct named specialist. Do not claim a test, platform, variant, soak duration, performance threshold, sanitizer, or manual review that was not actually executed.

Do not generate per-file hashes or claim a gate closed while production/test files remain unstable. Bind final gate evidence once, after readiness approval and a stable revision or explicitly recorded worktree fingerprint. Avoid self-referential tracker/report sealing: the gate report records the stable tracker hash, while the delivery message records the final report hash. Git mutations still require explicit user authorization.

Do not dispatch remote CI for every internal slice. Dispatch it at parent work-package checkpoints, after a wire/security change, and at final certification. Do not block an independent next internal slice on a still-running non-blocking CI job when targeted local evidence is green; record the pending result and stop immediately if it fails.

If a broad build is too expensive, run every safe targeted check, explain what remains, and keep the affected gate open.

## 7. Audit conformance

Before declaring a work package verified or a phase complete:

1. Have a reviewer who did not author code or tests re-read all changed code against documents 01–07.
2. Have the contract tracker verify every matrix row has evidence or an explicit blocker.
3. Confirm all repeated constants, IDs, units, timeouts, priorities, and state names still match the specs.
4. Walk nominal, late-join, restart, mission-change, disabled, loss, duplicate, reorder, timeout, overload, and cleanup paths as applicable.
5. Verify no later-phase collector, hook, UI, codec, optimization, or dependency entered the diff except an explicitly specified seam or test double.
6. Confirm no sensitive state leaks across `Cockpit`/`TrustedFullState` boundaries.
7. Confirm all resources are released and disabled telemetry has the specified negligible behavior.

For `balanced` work packages, do not require fuzz campaigns, supported-platform matrices, ten-minute profiles, or thirty-minute soaks unless that work package changes the relevant boundary or the specification assigns the evidence directly to it. Schedule those proofs for their designated final work package and keep the corresponding gate open.

Do not add a ninth Markdown document inside the phase specification folder; that would break the specification structure. Keep the compliance matrix in working context. Persist evidence only at a path required by the spec or explicitly approved by the user.

Do not check document 07 completion boxes automatically. Close a gate only when all its code, tests, measurements, and required reviews exist; user/reviewer approval remains external evidence.

Do not authorize the next work package from an intermediate reviewer opinion. Require both the final gate report and the progress tracker to say `closed`. If a gate reopens, immediately mark downstream work `suspended`, mark its evidence stale, and stop new downstream writes.

Do not let the coordinator, tracker, test agent, implementer, or reviewer attest to a responsibility owned by another role. Close a gate only from the combined tracker matrix, test evidence, and independent review result.

## 8. Deliver

Lead with the implementation outcome. Include:

- files and components implemented;
- the agent-role roster and completed handoffs;
- work packages and requirement ranges covered;
- tests/builds/measures actually run and their results;
- remaining acceptance criteria, gates, or external reviews;
- any spec conflict or repository drift discovered;
- the current phase status: complete only if every required proof exists, otherwise partially implemented or blocked.

Also include the compact output of `show_phase_progress.ps1`. On a pause request, finish only the current atomic write or short command, stop new work, collect fixed-role handoffs, update and validate the tracker, report running processes and stale evidence, then stop.

Do not stage, commit, push, or open a pull request unless the user explicitly requests it.
