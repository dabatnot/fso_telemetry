---
name: implement-telemetry-phase
description: Implement, resume, pause, or complete any numbered FS2Open telemetry roadmap phase from its canonical eight-document specification. Use for a telemetry work package or phase delivery that needs fast targeted development, independent gate evidence and review, dependency-scoped evidence reuse, bounded retries and certification budgets, and persistent machine-validated progress without rerunning fresh proof.
---

# Implement a telemetry phase

Deliver the smallest dependency-complete work package allowed by the specification. Preserve Phase 0 wire compatibility and all earlier product contracts.

Read [references/execution-policy.md](references/execution-policy.md) and [references/progress-schema.md](references/progress-schema.md) before editing code or the tracker.

## 1. Resolve the current state

1. Locate the single target phase directory and require exactly eight Markdown documents.
2. Read the complete target contract, Phase 0, and directly inherited lower-phase passages.
3. Run the specification validator and `scripts/inspect_phase_contract.ps1`.
4. Read repository instructions, relevant code/test seams, `git status --short`, and the persistent tracker.
5. Run `scripts/validate_phase_progress.ps1`.
6. State the authorized work package, gate, next action, current evidence freshness, and budget before writing.

Stop on a normative conflict. Do not choose a convenient interpretation.

## 2. Use independence at gates

Keep these responsibilities independent:

- **Coordinator:** scope, sequencing, budget, tracker orchestration, and final delivery.
- **Implementer:** production code and production build integration.
- **Test owner:** tests, fixtures, harnesses, test-only build integration, and raw evidence.
- **Reviewer:** readiness and gate-closing review without edits.

The implementer may run short targeted tests for feedback but may not accept its own gate evidence. A test owner must produce or accept gate evidence. A reviewer must independently approve readiness and closure.

Do not require a handoff after every local edit. Batch one cohesive implementation/test slice before readiness. Reuse the same independent identities within a gate when practical.

If independent agents are unavailable, permit targeted implementation but leave the gate open and report the missing independent evidence.

## 3. Run the bounded inner loop

For each cohesive slice:

1. Identify the owning requirements, acceptance criteria, changed dependency cone, and exclusions.
2. Add or repair the smallest deterministic oracle when the contract needs one.
3. Implement the minimum production change.
4. Build the narrow dedicated target.
5. Run only the targeted tests that can disprove the change.
6. Run changed-file checks and `git diff --check`.
7. Repeat until the reproducer is green.

Do not launch Release/LTO, broad CI, fuzz, performance, soak, full matrices, engine sessions, or report sealing in the inner loop.

Classify failures as:

- `production-regression`;
- `required-proof-defect`;
- `adjacent-debt`;
- `specification-conflict`.

Keep one blocker per root cause. Adjacent debt does not expand the work package.

## 4. Qualify expensive proof

Treat any operation expected to exceed five minutes as expensive.

Before it starts, require:

- readiness approval;
- two short deterministic qualifications of its harness/oracle, including the known worst or formerly failing case;
- a fresh dependency fingerprint;
- explicit `completed/total` progress;
- budget and stopping rule.

Stop the campaign at the first failure. Create a short reproducer. Do not resume or restart the matrix while the reproducer is red.

After a correction, rerun only evidence whose production, test, harness, build, or configuration fingerprint changed. Report-only edits invalidate nothing.

After two consecutive failures of the same automated operation, set the tracker to `paused` or `blocked`, record the root cause and exact next action, and stop automatic retries.

## 5. Control time and progress

Use a default certification ceiling of 180 minutes unless the specification contains an approved exception.

Before certification:

1. sum expected build, execution, analysis, and review time;
2. reuse every fresh assigned proof;
3. remove duplicate operations;
4. refuse to start if the projected total exceeds the remaining budget.

Update the tracker:

- when a scenario completes;
- when a gate, blocker, activity, or execution state changes;
- before pause and after resume;
- at most every fifteen minutes during a long operation.

Do not write tracker history after every command.

## 6. Close a work package or gate

At readiness:

1. give the reviewer the contract slice, focused diff, targeted results, and evidence plan;
2. collect all blocking findings together;
3. fix one consolidated batch;
4. run one focused rereview.

A third correction/review loop requires a new cited contract defect. Defer noncontractual improvements.

At the checkpoint:

- build only invalidated Release targets;
- run assigned integration proof;
- reuse fresh broad/security/fuzz/platform evidence;
- have the test owner bind conclusive results;
- have the reviewer decide the gate;
- reconcile and validate the tracker.

The final gate audits traceability, compatibility, scope, freshness, and completeness. It does not rerun fresh campaigns by default.

Do not authorize a downstream work package until both its prerequisite gate report and tracker state are closed.

## 7. Pause and resume safely

On pause:

1. finish only the active atomic write or short command;
2. start no new work;
3. stop owned build/test/harness processes;
4. record current activity, numerical progress, budget, blocker, stale evidence, and exact next action;
5. validate and display the tracker.

On resume, run:

```powershell
& '<skill-directory>\scripts\plan_phase_resume.ps1' -PhaseNumber <N>
```

Resume from its first action. Never restart completed work packages or a whole certification unless the dependency map proves it stale.

## 8. Deliver

Report:

- work package and gate outcome;
- changed components;
- role handoffs completed;
- exact tests and measurements actually run;
- evidence reused and invalidated;
- budget consumed and remaining;
- blocker or next authorized action;
- compact output of `show_phase_progress.ps1`.

Do not stage, commit, push, or open a pull request unless explicitly authorized.
