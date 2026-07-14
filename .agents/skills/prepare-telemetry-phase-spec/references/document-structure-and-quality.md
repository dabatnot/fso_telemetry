# Structure and quality contract

## Contents

1. Output layout
2. Document roles
3. Required depth
4. Cross-document consistency
5. Phase boundaries
6. Final quality gate

## 1. Output layout

Every phase specification uses one `README.md` and seven numbered documents. Keep one file per prefix and preserve this order:

| Prefix | Canonical filename | Stable role |
|---|---|---|
| — | `README.md` | status, navigation, sources, frozen decisions, phase boundary, exit gate |
| 01 | `01-cadre-normatif-et-perimetre.md` | objectives, actors, vocabulary, scope, exclusions, inherited contracts, requirement catalogue |
| 02 | `02-architecture-contrats-et-interfaces.md` | components, module boundaries, public contracts, dependencies, ownership, repository layout |
| 03 | `03-flux-cycle-de-vie-et-concurrence.md` | sequences, state machines, ordering, threading, scheduling, shutdown, recovery, backpressure |
| 04 | `04-modele-de-donnees-et-regles-metier.md` | fields, IDs, units, sources, authority, optionality, capture/diff rules, invariants |
| 05 | `05-integration-configuration-et-observabilite.md` | upstream seams, configuration, safe defaults, capabilities, metrics, logs, performance |
| 06 | `06-validation-securite-et-conformite.md` | validation order, errors, threat model, quotas, test plan, negative cases, fuzz/performance |
| 07 | `07-livraison-et-tracabilite.md` | work packages, dependencies, decisions, acceptance, traceability, risks, evidence, checklist |

Documents 02–05 may use a clearer phase-specific filename, but their role and prefix remain unchanged. Phase 0 is the concrete quality exemplar even though some of its role-specific filenames differ.

## 2. Document roles

### README

Include:

- phase title and specification status;
- what the documentation does and does not prove;
- expected outcome in observable terms;
- table linking all seven documents;
- exhaustive list of analysis and prior-spec sources;
- frozen inherited decisions and new phase decisions;
- included and excluded scope;
- exact exit gate and distinction between documents and implementation evidence.

### 01 — Normative frame and scope

Include:

- normative vocabulary;
- actors, roles, authority, trust boundaries, and read-only guarantees;
- assumptions and definitions;
- inherited requirements versus new requirements;
- stable requirement IDs with acceptance evidence;
- explicit exclusions and ownership by later phases;
- configuration/security principles that constrain every other document.

### 02 — Architecture, contracts, and interfaces

Include:

- component and dependency diagram;
- proposed/existing repository paths, clearly distinguished;
- public types and APIs with inputs, outputs, ownership, lifetime, and error contract;
- dependency direction and forbidden coupling;
- data flow from authoritative source through transport/consumer;
- initialization and teardown dependencies;
- extension points without speculative abstraction.

For implementation phases, inspect the real symbols and build files named by the roadmap before proposing seams.

### 03 — Flows, lifecycle, and concurrency

Include:

- nominal and failure sequences;
- complete state machines and transition tables;
- main-thread versus worker-thread responsibilities;
- queue ownership, capacity, overflow policy, and wake-up behavior;
- ordering, deduplication, idempotence, retries, timeouts, cancellation, and cleanup;
- startup, mission changes, death/respawn, reconnect, shutdown, and disabled behavior as applicable;
- consistency with Phase 0 session, ACK, snapshot, baseline, and delta semantics.

### 04 — Data model and business rules

For every value introduced or collected, specify:

- public name and type;
- source symbol/authoritative subsystem;
- unit, coordinate frame, range, precision, and canonicalization;
- ID scope, initialization, monotonicity, wrap, and invalid value;
- presence, absence, cardinality, list key, and deletion semantics;
- capture cadence, atomic grouping, dirty comparison, and baseline behavior;
- authority/visibility filtering and information-leak constraints;
- invalid source behavior and fallback;
- corresponding frozen Phase 0 record/field, or a versioned change decision.

Never serialize a pointer, handle, array index, C++ layout, NaN sentinel, or undocumented magic value.

### 05 — Integration, configuration, and observability

Include:

- exact upstream seams and why they are minimal;
- configuration schema, types, defaults, bounds, precedence, reload behavior, and invalid-config policy;
- disabled and loopback-safe defaults;
- capability activation/removal and graceful degradation;
- metrics with name, type, unit, labels/cardinality, update point, and reset scope;
- log events and prohibited high-volume/sensitive contents;
- resource and performance budgets, measurement method, and overload policy;
- build, dependency, packaging, and licensing constraints when relevant.

### 06 — Validation, security, and conformity

Include:

- validation order before allocation, mutation, or publication;
- error taxonomy and externally observable response;
- quotas, rate limits, overflow-safe arithmetic, and bounded memory;
- threat model appropriate to the phase;
- unit, integration, contract, negative, lifecycle, loss/reorder, property, fuzz, performance, build, and packaging tests as applicable;
- deterministic fixtures, harnesses, seeds, and expected results;
- trace from each requirement to at least one proof;
- cross-platform and disabled-mode checks.

### 07 — Delivery and traceability

Include:

- expected artifact tree, labeling proposed artifacts as proposed;
- ordered work packages with inputs, outputs, dependencies, and acceptance;
- dependency graph;
- intermediate and final gates;
- numbered, measurable acceptance criteria;
- decision table resolving ambiguities;
- global traceability table covering every top-level file in `documentation/analysis`;
- detailed mapping of the target roadmap bullets and shared test/observability/risk sections;
- risk register with detection, response, and blocking gate;
- evidence list and unchecked completion checklist.

## 3. Required depth

An implementation team must not have to invent any of the following:

- field type, unit, range, nullability, default, or sentinel;
- identifier scope, uniqueness, reuse, or wrap policy;
- lifecycle transition, ordering rule, retry, timeout, or cleanup;
- queue/buffer limit, backpressure behavior, or drop priority;
- thread ownership or synchronization boundary;
- validation order, error result, or partial-application behavior;
- capability fallback, disabled behavior, or safe network default;
- metric definition, test oracle, or exit threshold.

Use tables for exact mappings, Mermaid only when relationships are materially clearer, and pseudocode only for deterministic algorithms. Avoid decorative diagrams.

Each normative constant must be labeled as one of: fixed protocol value, implementation default, configurable bound, negotiated value, test value, or measurement target.

## 4. Cross-document consistency

Audit repeated concepts as a single registry even if they appear in multiple files:

- component and message names;
- requirement and decision IDs;
- states and transitions;
- field layouts, types, IDs, enums, flags, and units;
- default values, maxima, timeouts, retry windows, and priorities;
- capability prerequisites and removal behavior;
- queue limits and resource budgets;
- acceptance thresholds and test durations.

Prefer one normative definition plus links from consumers. If repetition improves readability, copy it exactly and verify it mechanically or during the semantic audit.

## 5. Phase boundaries

- Phase 0 owns the public wire contract. A later phase implements a subset or uses an extension already reserved there; it does not silently change wire bytes.
- Earlier phase specifications are normative dependencies. Record any necessary change as an explicit compatibility decision and identify all affected docs, vectors, code, and consumers.
- The target phase owns only its roadmap deliverables and interfaces. Mention later features only to define exclusions, extension seams, or test doubles.
- Documentation may name proposed modules, APIs, fixtures, or paths. Never describe them as existing until repository inspection confirms them.
- Do not implement code while producing a specification unless the user separately authorizes implementation.

For Phase 1 specifically, cover the minimal upstream seam, JSON configuration, dedicated nonblocking dual-stack UDP socket, engine-event subscription, session/heartbeat, initial player snapshot, cumulative deltas, baseline renewal, producer metrics, integration tests, independent decoder, and console proof client. Keep later ship systems, sensors, full-state replication, communication UI, and target video outside implementation scope while preserving their contracts.

## 6. Final quality gate

Before delivery, require all of the following:

- exactly eight Markdown documents with one `01`–`07` prefix each;
- every analysis source and prior phase is cited and mapped;
- every target roadmap bullet is implemented by a spec section or explicitly excluded;
- no unresolved `TODO`, `TBD`, placeholder, vague default, or contradictory duplicate definition;
- all local links and anchors resolve;
- strict UTF-8, balanced code fences, and no trailing whitespace;
- nominal and failure lifecycle walkthroughs converge and free bounded resources;
- all acceptance criteria are measurable;
- document 07's checklist remains unchecked until real artifacts and evidence exist;
- the final response distinguishes delivered specifications from unfinished implementation.
