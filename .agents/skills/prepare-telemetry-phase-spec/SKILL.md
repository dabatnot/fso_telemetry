---
name: prepare-telemetry-phase-spec
description: Produce exhaustive, implementation-ready specifications for a numbered phase of the FS2Open telemetry roadmap in documentation/analysis/04-implementation-roadmap.md. Use when the user asks in French or English to "préparer la spécification de la phase N", "rédiger les specs de la phase N", specify, detail, plan, or document telemetry roadmap phases 1–7, or explicitly refresh Phase 0. Read every analysis source and prior phase contract, create a documentation/analysis/specs/N-phase-title directory with the same eight-document structure and quality bar as Phase 0, resolve cross-document ambiguities, and validate structure, links, traceability, and Markdown quality.
---

# Prepare a telemetry phase specification

Produce a documentation specification, not an implementation. Preserve the frozen protocol and all earlier phase contracts unless the user explicitly requests a versioned change.

Read [references/document-structure-and-quality.md](references/document-structure-and-quality.md) completely before drafting. Use the bundled validator after writing.

## 1. Resolve the target

1. Locate the repository root and verify these paths exist:
   - `documentation/analysis/04-implementation-roadmap.md`
   - `documentation/analysis/specs/0-Contrat-de-protocole/`
2. Extract the exact heading and deliverables for the requested phase from the roadmap. Do not infer the phase from memory.
3. Name the output folder `documentation/analysis/specs/<N>-<slug>/`, where `<slug>` is the roadmap title after `Phase N —`, transliterated to portable ASCII and joined with hyphens. Preserve the title's capitalization pattern; for example, Phase 1 becomes `1-Squelette-et-premier-flux`.
4. If the target folder already exists, treat the task as an update: inventory it first, preserve valid content, and avoid destructive replacement.

Ask the user only if the requested number does not identify one roadmap phase or if a missing architectural decision would materially change the product. Otherwise make the most conservative decision consistent with prior contracts and record it in document 07.

## 2. Build the evidence base

Before writing:

1. Read repository instructions such as `AGENTS.md` when present and inspect `git status --short` without altering Git state.
2. Inventory `documentation/analysis` with `rg --files`.
3. Read every top-level Markdown file in `documentation/analysis` completely, including the global test plan, observability, risks, and recommended order in the roadmap.
4. Read every document of Phase 0 completely. It is the normative wire contract and the quality exemplar.
5. Read every completed specification for phases lower than the target. Later phases extend those contracts; they do not silently redefine them.
6. Inspect the current repository code, CMake files, APIs, and integration seams named by the target phase. Use repository evidence for paths and symbols, and label proposed artifacts as proposed rather than existing.
7. Search for all target-phase concepts across analysis, prior specs, and relevant code. Follow direct references until requirements, ownership, lifecycle, failure behavior, and tests are understood.

Do not browse the web unless the user requests it or a current external fact is necessary. Prefer the repository and its own documentation.

## 3. Create a requirement and decision inventory

Before drafting, classify every target-phase statement into:

- scope, exclusions, actors, authority, and trust boundary;
- inherited contracts and new guarantees;
- components, public interfaces, ownership, threading, and data flow;
- lifecycle, state machines, ordering, idempotence, retries, and cleanup;
- data fields, IDs, units, ranges, absence, cardinality, and source-of-truth;
- configuration, safe defaults, capabilities, compatibility, and fallbacks;
- resource limits, performance budgets, backpressure, and observability;
- validation order, errors, security controls, and negative behavior;
- tests, evidence, work packages, risks, and exit criteria.

Create stable requirement IDs such as `P1-REQ-001` and decision IDs such as `D1-001`. Maintain a source-to-requirement map while drafting. Resolve contradictions explicitly; never hide one by choosing different wording in separate documents.

## 4. Produce the eight-document structure

Create exactly:

```text
<N>-<phase-title>/
├── README.md
├── 01-<scope-role>.md
├── 02-<architecture-role>.md
├── 03-<flow-role>.md
├── 04-<data-role>.md
├── 05-<integration-role>.md
├── 06-validation-securite-et-conformite.md
└── 07-livraison-et-tracabilite.md
```

Use the canonical filenames from the reference unless a phase-specific filename is materially clearer. Keep exactly one file for each prefix `01` through `07`; preserve the role and ordering even when adapting a filename.

Write in French unless the user requests another language. Use normative `DOIT`, `NE DOIT PAS`, `DEVRAIT`, `NE DEVRAIT PAS`, and `PEUT`. Define every default, limit, timeout, state, error, identifier, unit, optional field, cleanup rule, and acceptance threshold that an implementer would otherwise have to guess.

Link inherited behavior instead of copying it incompletely. When a role is not implemented in the target phase, state the inherited contract, explain why no new behavior is introduced, and identify the later phase that owns it; never leave an empty document.

Do not add production code, build changes, hooks, commits, or generated binaries unless the user separately asks for implementation. The specification may define proposed paths and APIs, clearly marked as future deliverables.

## 5. Audit semantics before mechanical validation

Perform a separate cross-document audit after the first complete draft:

1. Map every roadmap bullet and every relevant analysis requirement to a section, explicit exclusion, inherited contract, or future-phase boundary.
2. Compare all repeated names, IDs, fields, units, bounds, defaults, timeouts, priorities, and state transitions across the eight documents and earlier phases.
3. Verify component ownership, thread boundaries, shutdown order, error propagation, and bounded-resource behavior.
4. Walk nominal, late-join, loss, duplication, reorder, timeout, restart, mission-change, disabled, and partial-capability scenarios as applicable.
5. Ensure every acceptance criterion is measurable and linked to a fixture, test, metric, review, or reproducible observation.
6. Remove `TODO`, `TBD`, placeholders, vague "as needed" behavior, and claims that unimplemented artifacts already exist.
7. Confirm document 07 contains complete traceability to every top-level analysis source and all inherited phase specs.

Fix every contradiction found, then repeat the audit on the changed passages.

## 6. Run the bundled validator

From the repository root, run:

```powershell
& '<skill-directory>\scripts\validate_phase_specs.ps1' `
  -PhaseNumber <N> `
  -PhaseDirectory 'documentation/analysis/specs/<N>-<slug>' `
  -AnalysisDirectory 'documentation/analysis' `
  -RoadmapPath 'documentation/analysis/04-implementation-roadmap.md'
```

Resolve `<skill-directory>` relative to this `SKILL.md`. Fix all reported errors. The script checks structure, strict UTF-8, Markdown fences, local links and anchors, stale markers, required sections, and source traceability. It does not replace the semantic audit.

## 7. Deliver

Report:

- the absolute link to the phase `README.md`;
- the eight documents created or updated;
- the principal decisions and inherited contracts;
- validation results and any deliberately deferred implementation artifacts;
- unresolved blockers, if any.

Do not declare the phase complete merely because its specification exists. Distinguish the documentation delivery from the code, tests, measurements, reviews, and other evidence required by its exit gate.
