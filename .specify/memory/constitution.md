<!--
Sync Impact Report
- Version change: uninitialized template -> 1.0.0
- Modified principles:
  - Template principle 1 -> I. Cockpit-Only Telemetry
  - Template principle 2 -> II. Producer-Side Visibility Filtering
  - Template principle 3 -> III. Passive and Optional Consumers
  - Template principle 4 -> IV. Proportionate Validation Without Gates
  - Template principle 5 -> V. User-Directed Development Order
- Added sections:
  - Build and Test Concurrency
  - Development Workflow
- Removed sections: none; template placeholders were concretized
- Deferred items: none
-->

# FS2Open SimPit Telemetry Constitution

## Core Principles

### I. Cockpit-Only Telemetry

The telemetry product MUST expose only information justified by a concrete simpit
cockpit need. `TrustedFullState` is permanently abandoned and MUST NOT be restored,
specified, implemented, migrated, or retained as a compatibility option. Dashboards,
replay tools, tests, and diagnostics do not justify hidden mission state or an
omniscient profile. This boundary preserves the simulation's intended information
model for every consumer.

### II. Producer-Side Visibility Filtering

New telemetry data MUST be added to the existing `CockpitSensors` projection and MUST
be filtered by the FS2Open producer before serialization. Consumers MUST NOT infer,
reconstruct, or request information hidden from the observed cockpit. Tests MUST cover
non-disclosure whenever a change affects contact visibility or other restricted state.

### III. Passive and Optional Consumers

External cockpit displays and tools MUST remain optional consumers. Their absence,
disconnection, failure, or shutdown MUST NOT change FS2Open's HUD, controls, or mission
execution. Return traffic MUST be limited to transport establishment, acknowledgement,
reliability, and resynchronization unless a separately specified cockpit requirement
explicitly authorizes a bounded command. No consumer may acquire implicit authority
over simulation state.

### IV. Proportionate Validation Without Gates

Automated unit or integration tests plus a focused manual in-game check are sufficient
for feature validation. Results MUST be reported plainly. The project MUST NOT create
proof registries, evidence ledgers, freshness fingerprints, certification workflows,
scores, eligibility calculations, or mandatory phase-completion gates. Validation
serves engineering judgment; it does not control whether later work may begin.

### V. User-Directed Development Order

The user owns product direction and MAY start, skip, reorder, explore, or continue any
phase even when tests fail or earlier work remains incomplete. Agents MUST report known
failures, dependencies, and risks, then follow the user's explicit decision. Plans and
task lists MAY recommend a technically safe order but MUST NOT represent that order as
an authorization or eligibility barrier.

## Build and Test Concurrency

- Exactly one agent MUST own compilation and test execution at a time.
- Before starting CMake, MSBuild, `cl.exe`, `link.exe`, or a test runner, the agent MUST
  verify that no other agent-owned build or test process remains active.
- Builds and tests MUST NOT run concurrently across agents, even for different targets
  or configurations.
- Existing configured build trees and artifacts SHOULD be reused when timestamps show
  that they contain the required sources.
- Windows MSBuild invocations MUST use `/m:1 /p:CL_MPCount=1`, including arguments
  forwarded through `cmake --build`.
- During a long Windows build, the owning agent MUST monitor the process tree and stop
  only its own exact build tree if more than one `cl.exe` appears.
- Performance measurements MUST NOT run while a compiler, linker, test runner, or other
  agent workload is active.

## Development Workflow

Specifications, plans, and tasks MUST preserve the cockpit-only boundary and identify
producer-side filtering for any new telemetry. Implementation work SHOULD use the
simplest structure that satisfies current cockpit requirements and MUST NOT introduce
speculative omniscient profiles, extension systems, proof mechanisms, or phase gates.

Before reporting a change complete, agents MUST run relevant automated tests when the
build environment permits and MUST report failures or omitted checks. Features that
affect observable cockpit behavior SHOULD include a focused manual in-game scenario.
Neither a failed nor an omitted check removes the user's authority to direct subsequent
work.

## Governance

This constitution is the normative project governance document for Spec Kit artifacts.
Repository-level `AGENTS.md` instructions remain operationally binding and MUST be kept
consistent with it. If the two conflict, work MUST stop long enough to surface the
conflict to the user rather than silently weakening either rule.

Amendments require an explicit user request or approval, a documented Sync Impact
Report, and a semantic-version change:

- **MAJOR** for removal or incompatible redefinition of a principle;
- **MINOR** for a new principle or materially expanded governance requirement;
- **PATCH** for clarifications that do not change obligations.

Feature specifications, plans, and task lists MUST be reviewed for alignment with this
constitution, but that review MUST remain advisory and MUST NOT become a phase gate or
eligibility mechanism.

**Version**: 1.0.0 | **Ratified**: 2026-08-27 | **Last Amended**: 2026-08-27
