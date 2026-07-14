---
name: implement-telemetry-phase
description: Implement a numbered FS2Open telemetry roadmap phase from the exhaustive eight-document specification under documentation/analysis/specs. Use when the user asks to "implémenter la phase N", "commencer l'implémentation de la phase N", build, code, execute, or complete a telemetry phase or one of its work packages. Validate and read the phase contract and all prior contracts, inventory requirement and acceptance IDs, implement code/tests/build integration in dependency order, preserve Phase 0 wire compatibility and phase boundaries, verify every claim with evidence, and report incomplete or blocked gates without silently changing the specification.
---

# Implement a telemetry phase

Implement the requested phase from its specification. Treat the eight documents as the contract and Phase 0 as the frozen wire authority.

Read [references/implementation-contract.md](references/implementation-contract.md) completely before changing files. Use [scripts/inspect_phase_contract.ps1](scripts/inspect_phase_contract.ps1) to inventory the contract.

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

## 3. Build the compliance matrix and plan

Create a working matrix containing every:

- requirement ID such as `P1-REQ-001`, `P1-F-001`, or `P1-NF-001`;
- decision ID `D1-*` that constrains implementation;
- work package `P1.x` and gate `G1-*`;
- acceptance criterion `P1-AC-*`;
- unchecked completion item from document 07.

For each row, record the owning code path, test or other proof, dependencies, and current status: `pending`, `implemented`, `verified`, `deferred`, or `blocked`.

Translate document 07's dependency graph into the task plan. Keep at most one work package in progress. Implement the smallest dependency-complete vertical slice; do not start a later phase merely because its seam is nearby.

## 4. Implement by gate

For each work package:

1. Re-read its owning spec sections and inherited Phase 0 definitions.
2. Write or update the narrowest contract tests first when bytes, parsing, IDs, lifecycle, concurrency, visibility, or failure behavior are involved.
3. Implement production code without copying engine structures or ABI layouts into the public contract.
4. Keep authoritative engine reads on the thread required by the spec. Pass copied public values across queues; never expose engine pointers or mutable handles.
5. Validate untrusted sizes, counts, offsets, enums, UTF-8, arithmetic, quotas, and session context before allocation or mutation.
6. Preserve disabled-mode and failure-mode behavior. Optional communication or video capabilities must not break canonical telemetry.
7. Keep buffers, queues, caches, retransmission windows, logs, and metric cardinality bounded.
8. Run the narrow tests for the package, inspect failures, and fix the implementation rather than weakening an oracle.
9. Mark a row `verified` only after its specified proof succeeds.

Never change a Phase 0 field layout, numeric registry, timeout, reliability rule, baseline semantic, capability, or visibility guarantee as an incidental implementation fix. A required wire change is a specification/versioning blocker.

## 5. Verify progressively

After each gate, run the checks required by documents 06 and 07, in this order where applicable:

1. formatter/static checks limited to changed files;
2. unit and contract tests;
3. golden vectors and independent decoder comparisons;
4. integration and lifecycle harnesses;
5. loss, duplication, reordering, timeout, and resynchronization scenarios;
6. supported build variants and dependency-disabled variants;
7. sanitizers, fuzzing, packaging, and licensing checks;
8. performance/soak measurements using the exact duration and threshold from the spec;
9. `git diff --check`, focused diff review, and final `git status --short`.

Use the repository's documented build and test commands. Do not claim a test, platform, variant, soak duration, performance threshold, sanitizer, or manual review that was not actually executed.

If a broad build is too expensive, run every safe targeted check, explain what remains, and keep the affected gate open.

## 6. Audit conformance

Before declaring completion:

1. Re-read all changed code against documents 01–07.
2. Verify every matrix row has evidence or an explicit blocker.
3. Confirm all repeated constants, IDs, units, timeouts, priorities, and state names still match the specs.
4. Walk nominal, late-join, restart, mission-change, disabled, loss, duplicate, reorder, timeout, overload, and cleanup paths as applicable.
5. Verify no later-phase collector, hook, UI, codec, optimization, or dependency entered the diff except an explicitly specified seam or test double.
6. Confirm no sensitive state leaks across `Cockpit`/`TrustedFullState` boundaries.
7. Confirm all resources are released and disabled telemetry has the specified negligible behavior.

Do not add a ninth Markdown document inside the phase specification folder; that would break the specification structure. Keep the compliance matrix in working context. Persist evidence only at a path required by the spec or explicitly approved by the user.

Do not check document 07 completion boxes automatically. Close a gate only when all its code, tests, measurements, and required reviews exist; user/reviewer approval remains external evidence.

## 7. Deliver

Lead with the implementation outcome. Include:

- files and components implemented;
- work packages and requirement ranges covered;
- tests/builds/measures actually run and their results;
- remaining acceptance criteria, gates, or external reviews;
- any spec conflict or repository drift discovered;
- the current phase status: complete only if every required proof exists, otherwise partially implemented or blocked.

Do not stage, commit, push, or open a pull request unless the user explicitly requests it.
