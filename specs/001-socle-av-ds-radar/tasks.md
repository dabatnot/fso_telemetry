---

description: "Dependency-ordered implementation tasks for the AV DS single-Radar foundation"
---

# Tasks: Socle AV DS et Radar unique

**Input**: Design documents from `/specs/001-socle-av-ds-radar/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`,
`contracts/av-ds-application.md`, `quickstart.md`

**Tests**: Included because the specification defines independent tests for every user
story and requires automated tests plus a manual in-game check.

**Organization**: Tasks are grouped by user story so the AV DS identity/Radar MVP,
link continuity, and FS2Open autonomy can each be implemented and validated as an
observable increment.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel because it changes different files and does not depend
  on another incomplete task in the same phase.
- **[Story]**: Maps the task to US1, US2, or US3 from `spec.md`.
- Every task names the exact repository path it changes or validates.

## Phase 1: Setup (Shared Build and Test Wiring)

**Purpose**: Prepare the existing Qt project for the in-place AV DS transformation.

- [X] T001 Rename the CMake project and logical executable target to AV DS, set the Windows output to `av-ds` while preserving the non-Windows `FsoSimpitRadar` output, and keep the existing source list, install rule, C++20, Qt 6, and `fstl_protocol` dependencies buildable in `tools/radar/CMakeLists.txt`

---

## Phase 2: Foundational (Shared Test Isolation)

**Purpose**: Provide deterministic settings and widget test isolation used by all
stories without introducing application behavior.

**Recommended prerequisite**: Complete this helper before running story tests that
touch settings so they do not overwrite the developer's real Radar configuration. This
is technical sequencing guidance, not a phase gate.

- [X] T002 Create helpers for temporary `QSettings` paths, application identity setup, and offscreen widget rendering in `tools/radar/tests/av_ds_test_support.h`

**Checkpoint**: AV DS tests can run against isolated settings and UI state.

---

## Phase 3: User Story 1 - Utiliser le Radar sous l'identité AV DS (Priority: P1) 🎯 MVP

**Goal**: Deliver one maximizable AV DS window representing `MFD-L`, with the static
`RADAR` page active, the existing Radar rendering intact, and legacy connection
settings reused.

**Independent Test**: Launch the Windows application with an existing Radar
configuration, verify the AV DS product/window identity and single `MFD-L`/`RADAR`
topology, validate the deterministic `referenceRadarImage` fixture, then compare an
active mission's authorized contacts, icons, and overlays with the reference client.

### Tests for User Story 1

> Write these tests first and confirm that each registered target fails by compilation
> or assertion before implementing the corresponding AV DS shell behavior.

- [X] T003 [US1] Create and register failing tests for a one-entry `RADAR` catalog, unique `MFD-L`, initial active page, normal/maximizable window behavior, and AV DS visible title in `tools/radar/tests/av_ds_shell_tests.cpp` and `tools/radar/CMakeLists.txt`
- [X] T004 [US1] Create and register failing compatibility tests that seed `FS2Open/FsoSimpitRadar` connection, display, and geometry keys and verify AV DS reads them without prompting in `tools/radar/tests/application_settings_tests.cpp` and `tools/radar/CMakeLists.txt`
- [X] T005 [P] [US1] Define the deterministic `referenceRadarImage` fixture and verify contact IDs, resolved icons, IFF colors, overlay order, visual states, resizing, maximization-sized surfaces, hidden-contact exclusion, and a valid zero-contact image with no stale decoration in `tools/radar/tests/radar_widget_tests.cpp`

### Implementation for User Story 1

- [X] T006 [US1] Implement explicit legacy-namespace reads and writes for connection, display, and window geometry settings and register the source in `tools/radar/src/application_settings.h`, `tools/radar/src/application_settings.cpp`, and `tools/radar/CMakeLists.txt`
- [X] T007 [US1] Implement `PageId`, the static one-page catalog, `DisplayUnitId::MfdLeft`, active-page validation, and the `MFD-L` page container and register the source in `tools/radar/src/display_unit.h`, `tools/radar/src/display_unit.cpp`, and `tools/radar/CMakeLists.txt`
- [X] T008 [US1] Replace the direct central `RadarWidget` composition with one `MFD-L` display unit hosting the `RADAR` page and using `ApplicationSettings` in `tools/radar/src/main_window.h` and `tools/radar/src/main_window.cpp`
- [X] T009 [US1] Set the visible application/display identity to `AV DS — AV Display System` while retaining the explicit legacy settings namespace in `tools/radar/src/main.cpp`
- [X] T010 [US1] Rename the Windows-deployed executable and ZIP inputs to `av-ds.exe`, remove the standalone Radar executable from the bundle, and keep Qt SVG/runtime/license deployment intact in `tools/radar/packaging/deploy-windows.ps1`
- [X] T011 [US1] Run the US1 Qt tests registered in `tools/radar/CMakeLists.txt` and perform the identity, legacy-settings, single-window, resize, deterministic-fixture, zero-contact, and Radar-reference checks in `specs/001-socle-av-ds-radar/quickstart.md`

**Checkpoint**: User Story 1 is a usable Windows MVP: AV DS launches as one `MFD-L`
window on `RADAR` using the previous Radar configuration.

---

## Phase 4: User Story 2 - Conserver la continuité de la liaison Radar (Priority: P2)

**Goal**: Preserve waiting, pause, stale-feed, resynchronization, reconnection, mission
replacement, and invalid-configuration behavior while presenting all link messages at
the AV DS/unit level rather than inside the Radar page.

**Independent Test**: Start AV DS before and after FS2Open, pause a mission, interrupt
and restore telemetry, change mission/observer, and verify the appropriate global
message and current contact picture at every transition.

### Tests for User Story 2

> Write these tests first and confirm that message ownership and transition assertions
> fail before moving the overlays.

- [X] T012 [P] [US2] Add failing tests for `ClientStatus` to AV DS message text/color/visibility mapping and propagation to two test `DisplayUnit` instances with distinct page widgets, without registering `MFD-R` or another production page, in `tools/radar/tests/av_ds_shell_tests.cpp`
- [X] T013 [P] [US2] Add failing client tests for waiting, pause without invented movement, stale/reconnect thresholds, interrupted snapshots, first-valid-state recovery, mission replacement, and observer replacement in `tools/radar/tests/radar_client_tests.cpp`
- [X] T014 [P] [US2] Add a failing regression test proving `RadarWidget` renders Radar content without owning or painting system-link messages in `tools/radar/tests/radar_widget_tests.cpp`

### Implementation for User Story 2

- [X] T015 [US2] Implement the global `AvDsMessage` value and the existing `ClientStatus` text/color/visibility mapping and register the source in `tools/radar/src/av_ds_message.h`, `tools/radar/src/av_ds_message.cpp`, and `tools/radar/CMakeLists.txt`
- [X] T016 [US2] Implement the transparent global-message renderer using the existing VFNT title/detail resources and stale dimming behavior and register the source in `tools/radar/src/av_ds_message_overlay.h`, `tools/radar/src/av_ds_message_overlay.cpp`, and `tools/radar/CMakeLists.txt`
- [X] T017 [US2] Remove `ClientStatus`, message mapping, VFNT system overlay state, and system-message painting from the `RADAR` page while preserving Radar image effects in `tools/radar/src/radar_widget.h` and `tools/radar/src/radar_widget.cpp`
- [X] T018 [US2] Route each `RadarClient::statusChanged` event through the application-level message mapping and propagate it to every unit in the application-owned `DisplayUnit` collection without changing active pages, while the production collection contains only `MFD-L`, in `tools/radar/src/main_window.h` and `tools/radar/src/main_window.cpp`
- [X] T019 [US2] Present missing, rejected, or invalid connection configuration through the AV DS global-message path and keep settings reopening behavior on Escape and Ctrl+, in `tools/radar/src/main_window.cpp` and `tools/radar/src/settings_dialog.cpp`
- [X] T020 [US2] Ensure session renegotiation, mission changes, and observer changes cannot republish a prior image as `Live` before a new complete validated state in `tools/radar/src/radar_client.cpp`
- [X] T021 [US2] Run the US2 Qt tests registered in `tools/radar/CMakeLists.txt` and execute the wait, pause, telemetry-loss, reconnect, invalid-config, and mission-change scenarios in `specs/001-socle-av-ds-radar/quickstart.md`

**Checkpoint**: User Story 2 independently demonstrates continuity and correct global
message ownership across ordinary game and network transitions.

---

## Phase 5: User Story 3 - Préserver l'autonomie de FS2Open (Priority: P3)

**Goal**: Keep AV DS a cockpit-filtered, direct, read-only FSTL consumer whose absence,
closure, or disconnection cannot affect FS2Open gameplay.

**Independent Test**: Run the same mission with AV DS connected, disconnected, closed,
and absent; compare HUD, commands, target/range state, and hidden-contact behavior.

### Tests for User Story 3

> Write these tests first and confirm the explicit read-only/profile assertions are
> exercised before finalizing the transport constraints.

- [X] T022 [P] [US3] Extend coverage tests for strict `CockpitSensors` profile acceptance, rejection of insufficient or non-cockpit coverage, and unrevealed-class fallback behavior in `tools/radar/tests/radar_model_tests.cpp`
- [X] T023 [P] [US3] Add transport tests proving HELLO requests the cockpit profile and client outbound traffic is limited to handshake, acknowledgement, heartbeat/reliability, and resynchronization messages in `tools/radar/tests/radar_client_tests.cpp`

### Implementation for User Story 3

- [X] T024 [US3] Make the direct `CockpitSensors` profile request and read-only session capabilities explicit while keeping AV CORE and all business-command paths absent in `tools/radar/src/radar_client.cpp`
- [X] T025 [US3] Run the US3 model/client tests registered in `tools/radar/CMakeLists.txt` and perform the connected/disconnected/closed/absent gameplay and hidden-contact comparisons in `specs/001-socle-av-ds-radar/quickstart.md`

**Checkpoint**: User Story 3 confirms AV DS is optional, passive, and cockpit-only.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Align user-facing documentation, notices, packaging, and final validation
across all three stories.

- [X] T026 [P] Replace standalone Radar product wording, usage paths, Windows build output, settings-compatibility notes, and Windows-only phase scope with AV DS terminology while documenting the preserved non-Windows legacy output in `tools/radar/README.md`
- [X] T027 [P] Update the product attribution from FSO SimPit Radar to AV DS without changing third-party license obligations in `tools/radar/THIRD_PARTY_NOTICES.md`
- [X] T028 Build and inspect the redistributable ZIP from `tools/radar/packaging/deploy-windows.ps1`, confirming it contains `av-ds.exe`, Qt SVG/runtime dependencies, and licenses but no `FsoSimpitRadar.exe`
- [X] T029 After checking that no other agent owns a build or test process, run the complete CTest suite serially from `tools/radar/CMakeLists.txt` and complete the manual in-game validation in `specs/001-socle-av-ds-radar/quickstart.md`

---

## Dependencies & Execution Order

### Recommended Phase Dependencies

- **Phase 1 (Setup)**: No dependency; establishes the buildable AV DS target identity.
- **Phase 2 (Foundational)**: Preferably follows T001 and protects real user settings
  when story tests run.
- **Phase 3 (US1)**: Preferably follows Phase 2 and delivers the MVP shell.
- **Phase 4 (US2)**: Technically depends on the display-unit shell from US1 because global messages
  must be rendered above a page owned by an AV DS unit.
- **Phase 5 (US3)**: Preferably follows Phase 2 for test isolation; it can run alongside
  US1/US2 if edits to `radar_client_tests.cpp` and `radar_client.cpp` are coordinated.
- **Phase 6 (Polish)**: Follows every story selected for delivery; T028 follows T010,
  and T029 follows all implementation and test tasks.

These dependencies describe the safest technical order. They do not prevent the user
from starting, skipping, or reordering a phase; known failures and risks are reported
plainly when the recommended order is not followed.

### Recommended User Story Dependency Graph

```text
Setup T001 -> Foundation T002 -> US1 (MVP) -> US2
                              `-> US3
US1 + US2 + US3 -> Polish and full validation
```

### Within Each User Story

- Add story tests and observe their new assertions fail before implementation.
- Implement value types/settings before widgets and application integration.
- Complete story-specific automated tests before its manual independent scenario.
- Do not launch multiple builds or test runners concurrently; repository rules require
  a single build/test owner and serial test execution.

### Parallel Opportunities

- US1 task T005 can be authored independently while T003 and T004 are sequenced because
  both register targets in `tools/radar/CMakeLists.txt`; T006 and T007 are also sequenced
  when they register their sources.
- US2 test tasks T012, T013, and T014 touch different files and can be authored in
  parallel; T015 and T016 are sequenced when they register sources, while T020 remains
  independent of those UI source additions.
- US3 test tasks T022 and T023 touch different files and can be authored in parallel.
- US3 can be developed alongside the US1 shell, but shared edits in
  `radar_client_tests.cpp`, `radar_client.cpp`, or `tools/radar/CMakeLists.txt` must be
  sequenced.
- Documentation tasks T026 and T027 can run in parallel after observable names settle.

---

## Parallel Example: User Story 1

```text
Sequential CMake registration: T003, then T004
Parallel with either test-authoring stream: T005 in tools/radar/tests/radar_widget_tests.cpp
```

## Parallel Example: User Story 2

```text
Task T012: Global message mapping/overlay tests in tools/radar/tests/av_ds_shell_tests.cpp
Task T013: Link and state-transition tests in tools/radar/tests/radar_client_tests.cpp
Task T014: Message-free Radar page regression in tools/radar/tests/radar_widget_tests.cpp
```

## Parallel Example: User Story 3

```text
Task T022: Cockpit coverage/non-disclosure tests in tools/radar/tests/radar_model_tests.cpp
Task T023: Read-only outbound transport tests in tools/radar/tests/radar_client_tests.cpp
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Prefer T001-T002 before tests that touch application settings.
2. Complete T003-T011 using the marked parallel opportunities where files do not overlap.
3. Validate the AV DS identity, legacy configuration, single `MFD-L`, active
   `RADAR` page, and reference Radar rendering.
4. This is a deployable MVP even before continuity hardening and autonomy validation.

### Incremental Delivery

1. **Foundation**: T001-T002 establish build/test isolation.
2. **MVP**: T003-T011 deliver AV DS with one functional Radar page.
3. **Continuity**: T012-T021 move messages to AV DS and preserve all link transitions.
4. **Autonomy**: T022-T025 make cockpit-only, read-only behavior explicit and tested.
5. **Polish**: T026-T029 align the bundle and complete automated/manual validation.

### Parallel Team Strategy

After T001-T002, one implementer can own the AV DS shell (US1), another can prepare
transport/state tests (US2/US3), and a third can prepare Radar page/model regressions.
Only one agent may own compilation or test execution at a time, and shared source files
must be edited sequentially.

## Notes

- `[P]` means different files and no incomplete same-phase dependency; it never permits
  concurrent compilation or test execution.
- Story labels provide traceability to `spec.md`; setup, foundational, and polish tasks
  intentionally have no story label.
- Internal namespaces may migrate progressively, but every user-visible executable,
  window, title, documentation, and Windows bundle must identify AV DS.
- Do not add `MFD-R`, COM, Cougar/OSB input, plugins, page scripts, migrate or extend Linux packaging,
  AV CORE coupling, simulation commands, new telemetry records, or `TrustedFullState`.
- Tests and manual checks are reported plainly; do not create proof registries, gates,
  scoring systems, or phase eligibility mechanisms.
