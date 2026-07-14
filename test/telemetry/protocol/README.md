# FSTL 1.0 protocol conformance assets

This directory contains the machine-readable FSTL 1.0 schema, independent
fixture-generation tools, and the byte-authoritative golden vectors used by
the protocol tests.

## Layout

- `schema/fstl-v1.yaml`: the normative machine-readable registry and schema;
- `tools/fstl_schema.py`: schema validation and C++ registry cross-checks;
- `tools/generate_transport_vectors.py`: independent Phase 0 transport vector
  generator using only Python's standard library;
- `vectors/valid/datagrams`: transport-valid datagram sequences;
- `vectors/invalid/datagrams`: transport-invalid datagram sequences;
- `expected`: canonical descriptions for valid fixtures.

The `.bin` files are authoritative. JSON files beside each fixture describe
the expected validation result; they do not replace the wire bytes.

Regenerate the Phase 0 transport vectors from the repository root with:

```text
python test/telemetry/protocol/tools/generate_transport_vectors.py --write
python test/telemetry/protocol/tools/generate_transport_vectors.py --check
```

The generator deliberately does not import or execute the C++ protocol
implementation. CRC-32/ISO-HDLC is calculated with the Python standard
library and checked against `123456789 == 0xcbf43926` before any fixture is
written.
