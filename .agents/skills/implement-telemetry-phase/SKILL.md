---
name: implement-telemetry-phase
description: Implement a numbered FS2Open telemetry roadmap phase from the exhaustive eight-document specification under documentation/analysis/specs with mandatory independent subagents for contract tracking, production implementation, testing, and review. Use when the user asks to "implémenter la phase N", "commencer l'implémentation de la phase N", build, code, execute, or complete a telemetry phase or one of its work packages. Validate and read the phase contract and all prior contracts, inventory requirement and acceptance IDs, implement code/tests/build integration in dependency order, preserve Phase 0 wire compatibility and phase boundaries, verify every claim with independent evidence, and report incomplete or blocked gates without silently changing the specification.
---

# Implement a telemetry phase

Implement the requested phase from its specification. Treat the eight documents as the contract and Phase 0 as the frozen wire authority.

Read [references/implementation-contract.md](references/implementation-contract.md) completely before changing files. Use [scripts/inspect_phase_contract.ps1](scripts/inspect_phase_contract.ps1) to inventory the contract.

## 0. Select an execution profile

Use `balanced` unless the user explicitly requests certification, gate closure, release evidence, or a final phase-complete claim. State the selected profile in the first progress update.

| Profile | Use for | Required cadence |
|---|---|---|
| `balanced` (default) | normal implementation of a work package | TDD and targeted tests for each internal slice; one independent review, Release build, broad CI, and tracker reconciliation when the work package is cohesive. |
| `certification` | final phase closure, explicit gate closure, protocol/security changes, or user-requested release evidence | full evidence sequence, all required variants, long-running campaigns, and final audit. |

Always use `certification` for a Phase 0 wire/schema/vector change, hostile-input security boundary, cross-thread ownership change, or a required fuzz/soak/performance gate. A `balanced` run may not claim those gates closed.

An internal slice is not a work package merely because it adds a helper, test file, or lifecycle seam. Keep a single compliance matrix and group dependency-coupled internal slices under their parent `P<N>-WP-nn`.

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

In `balanced`, keep the same four agents for the entire phase and reuse them in their fixed roles. Do not require a fresh multi-agent ceremony for every internal slice.

## 4. Build the compliance matrix and plan

Create a working matrix containing every:

- requirement ID such as `P1-REQ-001`, `P1-F-001`, or `P1-NF-001`;
- decision ID `D1-*` that constrains implementation;
- work package `P1-WP-*` (or legacy Phase 0 `P0.x`) and gate `G1-*`;
- acceptance criterion `P1-AC-*`;
- unchecked completion item from document 07.

For each row, record the owning code path, test or other proof, dependencies, and current status: `pending`, `implemented`, `verified`, `deferred`, or `blocked`.

Have the contract tracker translate document 07's dependency graph into the task plan. Have the coordinator assign the smallest dependency-complete parent work package; use internal slices only to sequence its dependencies. Do not start a later phase merely because its seam is nearby.

## 5. Implement by gate

For each parent work package:

1. Have the tracker return the work-package contract slice, dependency state, requirement rows, acceptance criteria, and unresolved questions. Stop on any normative conflict.
2. Have the test agent read the owning spec sections and inherited Phase 0 definitions, then write or update the narrowest contract tests first when bytes, parsing, IDs, lifecycle, concurrency, visibility, or failure behavior are involved. Capture the expected failing result when practical.
3. Give the implementer the contract slice and raw failing evidence. Implement production code without copying engine structures or ABI layouts into the public contract.
4. Keep authoritative engine reads on the thread required by the spec. Pass copied public values across queues; never expose engine pointers or mutable handles.
5. Validate untrusted sizes, counts, offsets, enums, UTF-8, arithmetic, quotas, and session context before allocation or mutation.
6. Preserve disabled-mode and failure-mode behavior. Optional communication or video capabilities must not break canonical telemetry.
7. Keep buffers, queues, caches, retransmission windows, logs, and metric cardinality bounded.
8. Return control to the test agent. For every internal slice, run the narrow Debug/contract checks and preserve the oracle. Route production failures back to the implementer and test defects back to the test agent.
9. At the parent work-package checkpoint, run the required integration checks and have the reviewer independently inspect the contract slice, focused production/test diffs, and raw evidence. Route every finding to its owning role, then repeat targeted testing and one re-review after fixes.
10. In `balanced`, a further review loop is required only for a wire/schema, security, lifecycle, concurrency, data-loss, or demonstrably false-oracle finding. Record other improvements for the parent work-package audit instead of growing an internal slice indefinitely.
11. Have the tracker reconcile every matrix row at the checkpoint. Mark a row `verified` only after its specified independent proof succeeds and no blocking review finding remains.

Never change a Phase 0 field layout, numeric registry, timeout, reliability rule, baseline semantic, capability, or visibility guarantee as an incidental implementation fix. A required wire change is a specification/versioning blocker.

## 6. Verify progressively

Run checks in proportion to the profile and change risk. Do not substitute an unrun required final proof with a shorter run.

For every internal slice, run formatter/static checks on changed files, the narrow Debug tests, and `git diff --check`.

At a `balanced` parent work-package checkpoint, run the changed-target Release build, unit/contract tests, directly affected integration/lifecycle harnesses, and one broad CI workflow if available. Run a full local suite only when the work package changes shared protocol, runtime, build integration, or a test harness used broadly.

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

Do not stage, commit, push, or open a pull request unless the user explicitly requests it.
