# FSTL 1.1 protocol assets

FSTL 1.1 is the only wire format emitted or accepted before the first public
release. The major and minor fields remain in the datagram and handshake
layouts, but both endpoints must advertise and use exactly `1.1`. There is no
version negotiation, fallback, or compatibility path.

## Active assets

- `schema/fstl-v1.1.yaml` describes the current wire contract;
- `vectors-v1.1` contains current valid and invalid protocol cases;
- `expected-v1.1` contains canonical decoded representations;
- `tools/fstl_reference_decoder.py` is the shared standard-library decoder and
  checks the current corpus independently from the C++ implementation;
- `tools/fstl_client_core.py` implements the common live client used by AV Core
  and the Dashboard;
- `tools/fstl_console_client.py` and the scenario client exercise the same
  decoder and client core.

The older `vectors` and `expected` trees remain test payload inputs for record
and message layouts. They are not accepted as FSTL 1.0 datagrams: every
transport decoder validates an exact 1.1 header.

Run the current independent contract check from the repository root with:

```text
python -B test/telemetry/protocol/tools/fstl_reference_decoder.py --check --repo .
```

Production C++ tests consume the same current `.bin` files and verify strict
1.1 ingress, handshake fields, snapshot rules, CRC handling, and canonical
decoding. The fuzz corpus is seeded only from `vectors-v1.1`.

The version number remains 1.1 until a stable MVP is publicly released. A
future protocol change will define its compatibility policy when that need is
concrete.
