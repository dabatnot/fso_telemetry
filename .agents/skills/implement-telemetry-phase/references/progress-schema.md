# Phase progress tracker

## Contents

1. Authority
2. Location and lifecycle
3. Status model
4. Required content
5. Update rules

## 1. Authority

`documentation/analysis/progress/phase-<N>.json` is the canonical status summary. It is not implementation evidence and never overrides the eight-document contract or a gate report.

## 2. Location and lifecycle

Initialize it once when implementation begins. Validate it after meaningful handoffs, at readiness, at checkpoints, on gate reopen/closure, and before pause/delivery. Keep exactly one tracker per phase.

## 3. Status model

- Execution: `active`, `paused`, `blocked`, `certifying`, `complete`.
- Work packages and contract items: `pending`, `in_progress`, `implemented`, `verified`, `blocked`, `deferred`, `suspended`.
- Gates: `pending`, `ready`, `closed`, `reopened`, `blocked`.
- Evidence: `fresh`, `stale`, `missing`, `superseded`.
- Blockers: `active`, `resolved_unverified`, `closed`.

`implemented` means code exists but proof is incomplete. `verified` requires fresh evidence. `suspended` is downstream work retained after a dependency gate closes late or reopens.

## 4. Required content

Track:

- phase, title, profile, execution state, update timestamp;
- source branch/HEAD and contract directory;
- current WP/slice, last closed gate, next gate, summary;
- exhaustive status buckets for work packages, requirement/test IDs, acceptance criteria, and gates;
- decision IDs;
- blockers with owner, affected IDs, state, and next proof;
- evidence records with status, path or command, revision, and scope;
- ordered next actions and notes.

Status buckets must form an exact, duplicate-free partition of the IDs inventoried from the contract.

Keep operational history compact:

- one blocker per root cause;
- one latest diagnostic per active blocker;
- conclusive checkpoint evidence;
- no row per retry, migrated fixture, role message, or superseded intermediate build.

Detailed attempt history belongs in raw logs or a required gate report, not in the status tracker.

## 5. Update rules

1. The coordinator updates current activity and next actions after a meaningful batched handoff.
2. The tracker agent reconciles exhaustive buckets at readiness and checkpoint.
3. A reopened gate moves affected verified rows back to `implemented` or `blocked`, marks only dependency-invalidated evidence stale, and suspends downstream WPs.
4. A closed gate requires an existing evidence path and no active blocker assigned to it.
5. A pause sets execution state to `paused`, records stale or unrun proofs and active processes, validates the file, and emits a self-contained handoff.
6. Never update timestamps or statuses merely to make progress appear newer.
7. After a report-only sealing step, remove or replace any next action that still requests that completed step at the next permitted tracker reconciliation.
