---
name: prepare-telemetry-phase-spec
description: Prepare, rewrite, or migrate an implementation-ready specification for any numbered FS2Open telemetry roadmap phase. Use when the user asks to specify, plan, refresh, or reduce the proof burden of a telemetry phase. Preserve the product and wire contracts, keep exactly eight canonical documents, assign evidence by risk, require explicit cost and invalidation metadata for expensive proof, and cap certification at three hours unless a documented exception is approved.
---

# Prepare a telemetry phase specification

Produce a product contract that is strict about observable behavior and economical about proving it. Do not implement production code.

Read [references/evidence-policy.md](references/evidence-policy.md) before changing a phase specification. Run the bundled validator after editing.

## 1. Establish the contract boundary

1. Locate the repository root, roadmap, Phase 0 protocol contract, target phase, and all lower completed phases.
2. Read the eight target documents, the target roadmap section, and the directly inherited contracts completely.
3. Inspect the named code and test seams. Mark future artifacts as proposed.
4. Inventory the existing requirements, decisions, acceptance criteria, work packages, gates, and evidence.
5. Preserve functional requirements, acceptance thresholds, phase boundaries, and wire compatibility unless the user explicitly authorizes a product-contract change.

When migrating an existing phase, change the execution and evidence contract only. Do not regenerate the product specification or renumber stable IDs.

## 2. Keep the canonical structure

Require exactly:

```text
README.md
01-*.md
02-*.md
03-*.md
04-*.md
05-*.md
06-validation-securite-et-conformite.md
07-livraison-et-tracabilite.md
```

Write in French unless requested otherwise. Use normative `DOIT`, `NE DOIT PAS`, `DEVRAIT`, `NE DEVRAIT PAS`, and `PEUT`.

Maintain traceability:

```text
roadmap/source -> requirement -> acceptance criterion -> work package -> gate -> evidence
```

Do not create a ninth phase document.

## 3. Design proof from risk

Classify each criterion as `low`, `medium`, `high`, or `critical`.

- `low`: local deterministic unit or static proof.
- `medium`: focused integration or lifecycle proof.
- `high`: representative matrix, hostile input, concurrency, or bounded endurance proof.
- `critical`: public wire compatibility, trust boundary, security, corruption, or irreversible-state proof.

Assign one of three cadences:

- `inner-loop`: seconds to a few minutes, author-owned and diagnostic;
- `wp-checkpoint`: focused Release/integration proof after readiness;
- `gate-certification`: independent evidence required to close a high-risk gate.

For every operation expected to exceed five minutes, record:

- risk and contract rows covered;
- command or reproducible procedure;
- estimated wall time;
- qualification prerequisite;
- stopping rule;
- source/test/harness/configuration dependencies;
- invalidation rule;
- evidence-reuse key;
- owner and gate.

Reject an expensive operation that lacks any field above, duplicates fresher evidence, or has no unique risk.

## 4. Bound certification

Add this machine-readable marker to document 06:

```text
<!-- certification-budget-minutes: 180 -->
```

The default phase-certification budget is 180 minutes including builds, campaigns, and independent review. A larger value requires a nearby `Certification budget exception` section containing the risk, alternatives rejected, expected duration, and user decision.

Prefer:

- short coverage over every cheap matrix cell;
- long runs on representative boundary cases;
- one composite soak instead of several overlapping soaks;
- one control/active performance comparison;
- reuse of fresh security, fuzz, protocol, and platform evidence.

Do not use certification as a debugging loop. Require harness and oracle qualification with short deterministic cases before a long campaign.

## 5. Define work packages and gates

For every work package, define:

- owned requirements and acceptance criteria;
- dependency-complete scope and explicit exclusions;
- fast inner-loop command;
- readiness condition;
- checkpoint evidence;
- gate-certification evidence, or `none`;
- risk escalation trigger;
- evidence invalidation cone;
- tracker mapping.

A gate must make one new acceptance decision. Merge or remove a gate that only repeats prior proof.

Require independent test evidence and review at gate readiness/closure. Do not require independent handoffs for every implementation edit.

## 6. Audit and validate

Before running the validator:

1. Walk nominal, disabled, failure, restart, mission-change, loss, reorder, timeout, overload, and cleanup paths when applicable.
2. Verify repeated IDs, constants, units, timeouts, states, ownership, and thread boundaries.
3. Confirm every acceptance criterion has measurable proof.
4. Confirm every expensive proof has one owning risk and no redundant gate.
5. Sum the certification budget and keep it within the marker.
6. Remove placeholders and ambiguous proof language.

Run:

```powershell
& '<skill-directory>\scripts\validate_phase_specs.ps1' `
  -PhaseNumber <N> `
  -PhaseDirectory 'documentation/analysis/specs/<N>-<slug>'
```

Fix all errors. Mechanical validation does not replace the semantic audit.

## 7. Deliver

Report the preserved product boundary, changed execution contract, gate count, long campaigns, total certification budget, reuse rules, validator result, and unresolved conflicts. Do not claim implementation completion.
