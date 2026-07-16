# FSTL 1.0 protocol conformance assets

This directory contains the machine-readable FSTL 1.0 schema, independent
fixture-generation tools, and the byte-authoritative golden vectors used by
the protocol tests.

## Layout

- `schema/fstl-v1.yaml`: the normative machine-readable registry and schema;
- `tools/fstl_schema.py`: schema validation and C++ registry cross-checks;
- `tools/verify_schema_vectors.py`: schema-driven reconstruction of all 20
  message payloads and all 28 record envelopes from canonical fields, checked
  byte-for-byte against the golden `.bin` files;
- `tools/generate_transport_vectors.py`: independent Phase 0 transport vector
  generator using only Python's standard library;
- `tools/generate_protocol_vectors.py`: independent generator for one
  canonical payload for every v1 `MessageType` and one canonical envelope for
  every v1 `RecordType`;
- `tools/fstl_reference_decoder.py`: independent decoder/checker which does
  not import either generator or the production C++ implementation;
- `vectors/valid/datagrams`: transport-valid datagram sequences;
- `vectors/invalid/datagrams`: transport-invalid datagram sequences;
- `vectors/valid/messages`: the 20 byte-authoritative message payloads;
- `vectors/valid/records`: the 28 byte-authoritative record envelopes;
- `vectors/invalid/messages` and `vectors/invalid/records`: generated negative
  payload/envelope fixtures with stable `ValidationError` names and numbers;
- `protocol-coverage.json`: machine-checked evidence for every item in document
  06 sections 10.4 and 10.5;
- `expected`: canonical descriptions for valid fixtures.

The `.bin` files are authoritative. JSON files beside each fixture describe
the expected validation result; they do not replace the wire bytes.

## Isolated FSTL 1.1 amendment corpus

`vectors-v1.1`, `expected-v1.1` and `fstl-1.1-vectors.manifest.json` form a
separate additive corpus for `PLAYER_KINEMATICS`. The amendment generator
`tools/verify_fstl_1_1_amendment.py` uses only the Python standard library and
fixed canonical JSON fixtures; it neither imports nor invokes
`fstl_reference_decoder.py`. Conversely, the reference decoder independently
parses the generated wire bytes and compares its output with those fixed
goldens. The production C++ tests consume the same `.bin` files and independently
project every valid 1.1 message to JSON for structural equality with the fixed
oracle. Metadata uses one normative truth: `valid`, numeric
`expectedValidationError`, diagnostic name and `notes`; only valid fixtures
carry `expectedCanonicalJson`.

The 21-case corpus covers minor negotiation, accepted and rejected WELCOME, two
cumulative DELTAs (real change then return to baseline), minimal snapshots,
Phase 2 promotion, all three required-record absences, authority/visibility,
and negative player-profile cases
for duplicate records, owner mismatch, non-ship lifecycle, unexpected
SHIP_IDENTITY and uncovered FLIGHT_STATE presence flags. Pre-session HELLO and
WELCOME capture fields are checked through the normative ingress/context path.

Regenerate the Phase 0 transport vectors from the repository root with:

```text
python test/telemetry/protocol/tools/generate_transport_vectors.py --write
python test/telemetry/protocol/tools/generate_transport_vectors.py --check
python test/telemetry/protocol/tools/generate_protocol_vectors.py --write
python test/telemetry/protocol/tools/generate_protocol_vectors.py --check
python test/telemetry/protocol/tools/fstl_reference_decoder.py --check
python test/telemetry/protocol/tools/verify_schema_vectors.py --check
```

The generator deliberately does not import or execute the C++ protocol
implementation. CRC-32/ISO-HDLC is calculated with the Python standard
library and checked against `123456789 == 0xcbf43926` before any fixture is
written.

`protocol-vectors.manifest.json` records a SHA-256 for every generated
message/record binary, metadata file, and canonical JSON file. The reference
decoder verifies those hashes, exact MessageType coverage `1..20`, exact
RecordType coverage `1..28`, canonical JSON equality, every negative fixture's
stable error code/name pair and category, the transport fixtures, the ISO-HDLC
check value with a separate bitwise implementation, and an explicit
little-/big-endian interpretation probe.

The schema includes the complete field/offset layout, QoS variants, and size
limits of every `MessageType`, plus the top-level fields of every `RecordType`
and all 25 referenced `*V1` nested structures (including `EventItemV1`). The
schema/vector checker also verifies implicit string lengths, list envelopes,
record envelopes, and the nested `AssetEntryV1` and `EventItemV1` bytes present
in the minimal golden fixtures. Separate deterministic layout probes exercise
all 192 message fields, 422 record fields, 25 nested types and 241 nested-field
occurrences (`EventItemV1` 30/30). Frozen projection and encoded-probe hashes,
plus targeted wire-drift self-tests, make unexercised optional-field drift fail
the schema check without misrepresenting the minimal golden coverage.

## Negative-catalogue status

The generated catalogue covers all 20 bullets in document 06 section 10.5.
Transport vectors include every one of the 68 header truncation lengths,
magic/version/header-size/flag mutations, both CRC layers, class-size and
reassembly-quota failures, canonical fragment-layout failures, overlap,
inconsistent metadata and a contradictory duplicate. Message and record
vectors cover fixed framing, record framing and duplicate keys, UTF-8 and field
domains, quaternion rules, replication context, ACK/NACK context,
communication consistency, video negotiation/size/lifecycle and the
read-only/no-command boundary.

Every invalid message/record metadata file also has a `cxxReplay` disposition.
`exact` identifies the production C++ API that can reproduce the declared
`ValidationError`; `divergence` names a real Phase 0 gap where the production
lifecycle currently returns a domain result (or a different validation code)
instead. These entries are deliberately visible to the C++ replay and freeze
audit rather than being silently treated as passing byte-only decoder cases.
