# Implementation contract

## Contents

1. Source-of-truth order
2. Mapping the eight documents to implementation
3. Compatibility invariants
4. Work-package execution
5. Evidence rules
6. Phase-specific boundaries
7. Completion gate

## 1. Source-of-truth order

Use this precedence:

1. explicit user instruction for the current task;
2. repository and workspace instructions;
3. target phase specification documents 01–07;
4. Phase 0 and earlier phase specifications;
5. top-level analysis documents and roadmap;
6. current implementation, only as evidence of what exists.

Existing code never overrides a normative public contract by accident. If code and spec differ, determine whether the code is incomplete, the repository drifted, or the spec is impossible. Do not hide the mismatch.

The phase specification is documentation, not proof that code, tests, measurements, packaging, or reviews already exist.

## 2. Mapping the eight documents to implementation

| Document | Implementation use |
|---|---|
| `README.md` | phase result, frozen decisions, sources, included/excluded scope, final gate |
| `01-*` | requirement catalogue, actors, authority, security boundary, forbidden behavior |
| `02-*` | components, APIs, dependencies, paths, ownership, allowed coupling |
| `03-*` | sequences, state machines, threading, queues, retries, shutdown and recovery |
| `04-*` | field types, IDs, units, sources, capture/diff rules, visibility and invariants |
| `05-*` | integration seams, config, defaults, metrics, logging, capabilities, performance |
| `06-*` | parser order, errors, quotas, tests, fuzzing, security and conformance oracles |
| `07-*` | artifact tree, work packages, dependencies, gates, acceptance, risks and evidence |

Read all roles; implementation details are deliberately distributed. Never implement document 02 while ignoring the lifecycle in 03 or validation in 06.

## 3. Compatibility invariants

### Wire and data

- Serialize only Phase 0 public types and exact layouts.
- Preserve little-endian encoding, field widths, canonical floats, UTF-8, limits, CRCs, fragmentation, IDs, flags, enums, and unknown-value behavior.
- Preserve cumulative deltas from an immutable acknowledged baseline. Never convert them to deltas from the previous packet.
- Preserve atomic manifest/snapshot application and reliable-event semantics.
- Never serialize a pointer, engine handle, array index, C++ padding, ABI layout, NaN sentinel, or hidden information.

### Authority and safety

- Keep telemetry read-only. No client message may mutate simulation state.
- Apply `Cockpit` visibility before serialization. Enable `TrustedFullState` only when the target phase and explicit configuration allow it.
- Bind and discovery behavior must retain safe defaults from the specs.
- Treat network payloads, local bundles, configuration, and codec data as untrusted at their boundary.

### Threading and resources

- Read authoritative engine structures only on the permitted engine thread.
- Transfer copied public values through bounded ownership-safe structures.
- Use nonblocking network/GPU behavior where specified; never wait synchronously on the game frame for socket, encoder, decoder, or readback work.
- Bound every allocation, queue, cache, client, transaction, log label, retry, and shutdown wait.

### Optional features

- Keep canonical telemetry independent of communication assets and target video.
- A missing bundle, renderer, readback, codec, dependency, or client capability degrades only the optional feature.
- Video traffic remains below canonical state and reliable control traffic in priority.

## 4. Work-package execution

For every `P<N>-WP-nn` work package (or legacy Phase 0 `P0.x` lot):

1. List input requirements and decisions.
2. Verify dependency gates are closed by actual evidence.
3. Identify existing and proposed paths from document 02.
4. Add a failing test or deterministic fixture for the behavior when practical.
5. Implement the minimum complete behavior.
6. Run package-local tests and inspect the diff.
7. Exercise error, cleanup, disabled, and overload paths.
8. Run the gate-level suite.
9. Record exact evidence for each acceptance criterion.

Do not combine unrelated work packages merely to reduce file count. Do not refactor neighboring systems unless required to expose the minimal seam defined by the spec.

When the codebase offers multiple viable seams, prefer the one that:

- changes the fewest upstream call sites;
- keeps protocol logic inside the telemetry module;
- copies values at the boundary;
- remains a no-op when telemetry is disabled;
- is testable without a running renderer or mission when the spec requires a pure test.

## 5. Evidence rules

Use these statuses:

| Status | Meaning |
|---|---|
| `pending` | no implementation or proof yet |
| `implemented` | code exists but required proof is incomplete |
| `verified` | implementation and every specified proof succeeded |
| `deferred` | explicitly outside the user's requested sub-scope; not phase-complete |
| `blocked` | cannot proceed without a spec decision, authority, dependency, platform, or external review |

Valid evidence includes:

- exact test command and passing result;
- golden-vector hash and independent decoder agreement;
- build variant and successful target;
- reproducible harness seed/profile and observed result;
- sanitizer/fuzzer configuration, duration, and result;
- performance command, hardware/context, duration, metric, and threshold;
- packaging/license artifact;
- named human review when the gate explicitly requires it.

Invalid evidence includes:

- code inspection alone when the spec requires a test;
- a short run substituted for a required soak duration;
- a single platform substituted for all supported platforms;
- generated vectors validated only by their generator;
- an unrun command listed as though it passed;
- an unchecked assumption that a dependency or encoder exists.

Do not persist evidence as an extra file inside `documentation/analysis/specs/<N>-*/`. If persistent evidence is required, use the artifact location specified by document 07 or request approval for a separate evidence location.

## 6. Phase-specific boundaries

| Phase | Implementation boundary |
|---:|---|
| 0 | protocol/schema, bounded readers/writers, control messages, vectors, decoder and pure tests; no engine collector |
| 1 | minimal module/build seam, safe config/socket, session/heartbeat, initial player snapshot, cumulative deltas, metrics, integration harness and console proof client |
| 2 | complete observed player ship, systems, catalogs and player lifecycle; no sensor/full-world expansion |
| 3 | target, radar, sensors, threats, cargo and navigation with strict `Cockpit` visibility |
| 4 | opt-in `TrustedFullState`, all authorized entities, join-in-progress, keyframes, graph consistency and bandwidth control |
| 5 | authoritative Talking Head hook, CFile-equivalent packager/bundle, `ReplicaStore`, usable remote client, local communication playback and ESP32 adapter |
| 6 | exact hooks only for events state diff can miss; measure before adding quantization, compression, SPSC or adaptive rates |
| 7 | native offscreen target render, async readback, bounded H.264 pipeline, negotiation/recovery, client decode/composition, build/license variants and loss/performance tests |

If a requested phase specification narrows these boundaries further, follow the specification.

## 7. Completion gate

A phase is complete only when:

- all required work packages and dependency gates are complete;
- every requirement has an implementation mapping;
- every acceptance criterion has valid evidence;
- all required build variants, negative tests, lifecycle tests, fuzzing, soak/performance tests, packaging checks, and reviews exist;
- disabled and failure modes meet the contract;
- no unresolved spec conflict or compatibility deviation remains;
- the final diff contains no later-phase functionality or unrelated user changes;
- documentation, generated schema/constants/vectors, code, tests, and observed behavior agree.

Otherwise report the phase as partially implemented, even if the available code compiles.
