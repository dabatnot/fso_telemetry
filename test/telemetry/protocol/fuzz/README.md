# FSTL 1.1 protocol fuzzing

These engine-independent targets exercise the Phase 0 trust boundaries:

- `fuzz_packet_reader`: bounded scalar, byte, UTF-8 and sub-reader operations;
- `fuzz_datagram`: fixed header, CRC, canonical slice and envelope validation;
- `fuzz_reassembler`: stateful fragment sequences, duplicates, contradictions and quotas;
- `fuzz_control_payloads`: discovery/session control, ACK/NACK, bitmaps and resync payloads;
- `fuzz_records`: record sequences and all 28 business-record validators;
- `fuzz_transactions`: manifest/snapshot transactions, deltas, event batches and baseline publication;
- `fuzz_comm_views`: bundle paths, communication manifest, state and events;
- `fuzz_video_payloads`: all six video payloads and Annex B metadata only. It never invokes an H.264 decoder.

The CMake integration is disabled by default. A Clang/AppleClang build uses
libFuzzer with ASan and UBSan:

```sh
cmake -S . -B build-fuzz -G Ninja \
  -DFSO_BUILD_TELEMETRY_FUZZERS=ON \
  -DFSO_BUILD_QTFRED=OFF -DFSO_BUILD_FRED2=OFF \
  -DFSO_BUILD_WITH_OPENGL=OFF -DFSO_BUILD_WITH_OPENXR=OFF
cmake --build build-fuzz --target telemetry_fuzz_smoke
```

MSVC and GCC can replay the exact same corpus under their available sanitizers
by adding `-DFSO_TELEMETRY_FUZZ_STANDALONE=ON`. Replay mode is a platform
sanitizer check, not a substitute for mutation-based libFuzzer runs.

An older local compiler that cannot enable its sanitizer may additionally use
`-DFSO_TELEMETRY_FUZZ_SANITIZERS=OFF` for deterministic corpus replay. That
mode is a smoke test only and does not satisfy the sanitizer gate; CI and freeze
builds keep the option at its default `ON`.

`prepare_fuzz_corpus.py` scans every checked-in vector metadata file and seeds
target-specific corpora in the build tree. It also stores every golden-vector
input in the `PacketReader` corpus, groups datagram sequences for the stateful
reassembler, and extracts payload/record seeds when framing permits. Generated
corpora and crash artifacts are never source artifacts.

`run_fuzz_smoke.py` accepts either a bounded `--runs` count or
`--max-total-time`. With no additional arguments it preserves the CI smoke
behavior: all eight targets run sequentially for 2,000 mutations each. A
freeze campaign can select one target per parallel job and emit a self-contained
evidence directory:

```sh
python test/telemetry/protocol/fuzz/run_fuzz_smoke.py \
  --binary-dir build-fuzz/bin \
  --corpus build-fuzz/telemetry-fuzz-corpus \
  --artifacts build-fuzz/telemetry-fuzz-artifacts \
  --dictionary test/telemetry/protocol/fuzz/fstl.dict \
  --target fuzz_packet_reader \
  --max-total-time 1800 \
  --seed 4242 \
  --expected-sha "$CANDIDATE_SHA" \
  --evidence-dir build-fuzz/telemetry-fuzz-evidence
```

Repeat the command in parallel for each target. Each target directory records
the checked-out and expected SHA, requested and observed duration, command,
seed, platform, tool versions, ASan/UBSan options, binary and dictionary hashes,
the raw log and parsed `stat::*` values, snapshots plus stable SHA-256 manifests
of the initial/final corpus, and any crash artifacts. A non-zero fuzzer exit or
any crash artifact fails the runner, while still writing the final report.

The Python evidence helpers have standalone unit coverage:

```sh
python test/telemetry/protocol/fuzz/test_run_fuzz_smoke.py
```

CI uses the bounded smoke configuration for pull requests. Longer campaigns
must retain all eight evidence directories and the sanitizer-instrumented build
log as review evidence; temporary CI artifacts must be copied to the final
freeze archive before their retention period expires.
