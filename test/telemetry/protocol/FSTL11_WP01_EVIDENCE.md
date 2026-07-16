# FSTL 1.1 / P1-WP-01 — reproducible evidence

Technical verdict: **READY FOR INDEPENDENT REVIEW**. Formal gates remain
`PENDING` or `BLOCKED`; this report does not authorize P1-WP-02.

## Tested revision identity

- HEAD: `e433d19220bd7c136f2440b2702d05249d4c7302`
- tracked binary-diff object (excluding this self-referential report):
  `1e55cc4da0bb775912eb78c395cd11b0d0695db9`
- untracked WP01 corpus identity (59 sorted paths, excluding this report; each
  entry is `path + NUL + binary SHA-256 + LF`, then SHA-256):
  `fa70d34bd179cf8da53e102617b3147b2b7c8f4eb73658eb175353ffe7abdd42`
- amendment verifier SHA-256:
  `550424ead66b135bdb968c7d36228e486a79d6b324bb121e63f90f182a2ec44b`
- FSTL 1.1 manifest SHA-256: `8aacb4d69a61441cd21c4498268bd4105e94483d9ad56e44d905c1e6275e4938`
- frozen FSTL 1.0 tree SHA-256:
  `099fffd00ea67ed71b6e345c3256b06f8ebf5c5ca14c518fe8e614a2bd4424aa`
- Python `3.14.0`; CMake `4.1.2`; Visual Studio/MSBuild Release build.

The tracked diff hash, untracked corpus identity, verifier hash and manifest
hash identify the dirty patch and generated corpus. Regeneration must
reproduce all four.

## Corpus and independent paths

The corpus has 21 cases: 15 snapshots and 6 datagram messages. Nine are valid
and have fixed canonical JSON; twelve are invalid and have one normative
`expectedValidationError` numeric ID. The Python reference decoder and the C++
path consume the same `.bin` files. C++ independently projects every field of
all nine valid cases to a Jansson tree and compares structurally with the fixed
JSON. For every invalid, C++ reads the metadata ID and asserts the same primary
`ValidationError`; Python independently asserts that ID.

The two cumulative DELTAs share baseline 1. Sequence 2 changes position,
quaternion, world velocity and local rotational velocity. Sequence 3 returns
those values exactly to baseline. The replication harness loses sequence 1,
accepts the later cumulative return, ignores reordered/duplicate packets and
converges to the baseline image.

## Commands and recorded outputs

| Command | Exit | Stable result |
|---|---:|---|
| `cmake --build build --config Release --target unittests` | 0 | `unittests.vcxproj -> .../unittests.exe` |
| `unittests.exe --gtest_filter=TelemetryProtocolVectors.*` (five consecutive runs) | 0 | each run: 14 tests, 1 suite, 14 passed |
| `unittests.exe --gtest_filter=TelemetryProtocol*` | 0 | 394 tests, 36 suites, 394 passed |
| `unittests.exe --gtest_filter=TelemetryProtocolBusinessStateValidation.Fstl11PlayerKinematicsRequiresCockpitEvenWhenTrustedFullStateIsAuthorized:TelemetryProtocolVectors.MissingCascadeOwnerKeepsBadRecordLengthInFrozenFstl10` | 0 | 2 tests, 2 suites, 2 passed |
| `python -B .../generate_protocol_vectors.py --check` | 0 | 20 MessageType and 28 RecordType vectors verified |
| `python -B .../verify_fstl_1_1_amendment.py --check` | 0 | 15 snapshots, 6 messages, 3 negotiations; frozen hash above |
| `python -B .../fstl_reference_decoder.py --check` | 0 | 21 FSTL 1.1 cases cross-decoded; base corpus and CRC checks passed |
| `python -B .../fstl_schema.py --self-test` | 0 | 20 messages, 28 records; all record-set/version/profile mutation tests passed |
| `python -B .../verify_schema_vectors.py --check` | 0 | layout `416ff38c...1136`; encoded probes `ee45ad75...5438` |
| `python -B .../verify_telemetry_assets.py --require-complete` | 0 | 182 fixtures; messages 20/20; records 28/28 |
| Phase 0 `validate_phase_specs.ps1` | 0 | 8 documents, 5375 lines; all structural checks passed |
| Phase 1 `validate_phase_specs.ps1` | 0 | 8 documents, 1897 lines; all structural checks passed |
| `git diff --check` | 0 | no whitespace error; CRLF conversion warnings only |
| `Get-ChildItem test -Recurse -Directory -Filter __pycache__` | 0 | no result after cleanup |

## Requirement matrix — WP01 scope only

| ID | Concrete evidence | Technical status |
|---|---|---|
| `P0-F-016` | 1.1 schema/corpus, negotiation, minimal snapshots and rejection fixtures | evidence recorded; independent review pending |
| `D0-019` | exact bit `0x0400`, profile 1..1, promotion and self-test mutations | evidence recorded; independent review pending |
| `P0-AC-025` | frozen 1.0 hash, schema vectors and complete C++ vector replay | `PENDING` reviewer decision |
| `P0-AC-026` | negotiation, minimal profile, downgrade/core promotion rejects | `PENDING` reviewer decision |
| `P0.13` | full `TelemetryProtocolVectors.*`, Python cross-decoder, JSON equality | evidence recorded; gate not closed here |
| `P1-REQ-001` | unchanged 1.0 tree and complete 1.0 replay | WP01 evidence recorded |
| `P1-REQ-002` | exact 1.1 negotiation; 1.0 rejection and no downgrade | WP01 evidence recorded |
| `P1-REQ-012` | inherited 1.0 schema/vector/limit replays | WP01 protocol slice only |
| `P1-REQ-020` | versioned `PLAYER_KINEMATICS=0x0400` schema and rejects | WP01 evidence recorded |
| `P1-REQ-021` | exact minimal/no-player/missing-record snapshots, manifest zero | WP01 evidence recorded |
| `P1-REQ-022` | exhaustive FLIGHT_STATE wire/JSON and presence rejection | WP01 wire slice only; engine oracle remains WP07 |
| `P1-REQ-023` | observed/lifecycle/flight ID parity and SHIP_IDENTITY exclusion | WP01 wire slice only; full engine slice later |
| `P1-REQ-024` | finite canonical kinematic wire values only | `BLOCKED` for WP07 engine-source proof |
| `P1-REQ-025` | non-zero wire entity identity only | `BLOCKED` for WP07 session/reuse proof |
| `P1-REQ-034` | Python/C++ identical JSON or identical rejection ID | WP01 interoperability slice recorded; WP10 remains later |
| `P1-AC-001` | frozen hashes plus two independent complete vector paths | `PENDING` reviewer decision |
| `P1-AC-002` | exact bit, record-set, version rejection and self-test mutations | `PENDING` reviewer decision |
| `G0-G` | evidence package above | `BLOCKED` pending tracker/reviewer closure |
| `G1-A` | WP01 evidence package above | `BLOCKED` pending tracker/reviewer closure |

No assertion in this report changes a formal gate state.
