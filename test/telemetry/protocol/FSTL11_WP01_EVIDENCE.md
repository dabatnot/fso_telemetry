# FSTL 1.1 / P1-WP-01 — reproducible evidence

Technical verdict on the stabilized working tree: **PASS**. The independent
test agent reproduced the complete suite and the independent reviewer found no
blocking WP01 defect. Formal gate reconciliation was performed in dependency
order: `G0-G PASS`, then `G1-A PASS`.

This closure is prospective, not retroactive. `P1-REQ-001` remains a
**historical, non-repairable FAIL** because `P1-WP-02` and then `P1-WP-03`
started before documented closure of `G0-G`. The explicit disposition records
that exception without converting it into a pass; after final Phase 1
certification, `P1-AC-020` is acceptable with that recorded limitation.

## Tested revision and artifact identity

- dirty-patch base HEAD:
  `9c0da12ed2f0649648e520107f879b6be5b65b49`;
- tracked non-report binary diff Git blob (complete tracked diff, excluding only
  this report and `PHASE0_CONFORMANCE.md` because they are self-referential
  decision records): `9e9cbda1953462378da668da0eb7025fb6d9a73e`;
- untracked implementation corpus: 5 sorted paths, aggregate SHA-256
  `00c6e861003c6875a21ce5e435c9467e9c101c7bf352d895a425c4641d90c679`.
  The aggregate hashes the UTF-8 concatenation of one entry per sorted path,
  encoded as `path + NUL + binary SHA-256 + LF`, then applies SHA-256;
- frozen tag: `fstl-v1.0.0`, commit
  `900487429bd20e13fcfea1c6e2d163631f1bafe1`;
- evidence date: 2026-07-16, Europe/Paris;
- Python `3.14.0`; CMake `4.1.2`; MSBuild `16.4.0+e901037fe`.

Together, the base HEAD, tracked-diff blob and untracked-corpus aggregate bind
the independently tested dirty tree. The two excluded reports are the decision
layer reviewed separately after the technical reproduction.

Candidate identity reconciliation (2026-07-19, Europe/Paris): the current
Phase 1 candidate changed documents 05 and 07 after the original technical
reproduction. The FSTL 1.1 schema was regenerated from all seven current
normative documents after pinning their derived document-set tree identity.
The schema, document-tree and generator identities in the following tables
therefore bind the current candidate content; this reconciliation does not
change the recorded G0-G/G1-A technical verdict above.

| Artifact or oracle | Bytes | SHA-256 / identity |
|---|---:|---|
| frozen `schema/fstl-v1.yaml` | 499,786 | `1d89c4a95a121c178bf85570cd616568fd939942b8d053835069b2d7d6a1f0d4` |
| additive `schema/fstl-v1.1.yaml` | 505,222 | `9dbe67e18b2d26a1491e8be37433177f61e94f6e4f674a5e993735703c9cd989` |
| `fstl-1.0-artifacts.manifest.json` | 131,635 | `4a437ee1300e86319ebd07a2fc4910cba96c4d15387a30eba5354eec49d1242f` |
| complete frozen FSTL 1.0 set | 438 files | tree `9baac6a20db33bcf350066ed533c5581b7117410899d7bc4a6dc24406e47856d` |
| Phase 1 seven-document FSTL 1.1 set | 7 files | tree `fd3660bdf7e7beb8261a67e0464136755e509d794755b27eee1b13f71a72d1ef` |
| `fstl-1.1-vectors.manifest.json` | 10,328 | `56c2c6d4c3cb662c9d2f780293be173d5d31bbcc2e36612ecd08ab53f581e846` |
| FSTL 1.0 layout lock | — | `d5e7ae20571bc0e08f1d123f7529fd6430466872ad0dd5cb22ea37b24128e0aa` |
| FSTL 1.1 layout lock | — | `416ff38c4549d2d9fba89e114b94c4fa0ef1fb1f27be59e2126db7fd5ea61136` |
| common encoded-probe lock | — | `ee45ad75728442145aa2689211f237868f095a39a2735b89ae5868c3dbe95438` |

The 438-file root covers the seven canonical Phase 0 documents, the frozen
schema, 371 `vectors/**` files, 54 `expected/**` files, both vector manifests,
the coverage catalogue, transport seeds and fuzz dictionary. It supersedes the
old `099fffd0…` digest as the complete freeze proof; that older digest covered
only `vectors/**`.

Tool identities used by the final reproduction:

| Tool | SHA-256 |
|---|---|
| `verify_fstl_1_0_freeze.py` | `d381a3bdf67a23415f573df768f4633fce15804f3ef257dbe67ff078aa127284` |
| `test_fstl_1_0_freeze.py` | `fb611174ad44e0e889848c1e2a427021049a1c4fd0ac8c8c9967151bab442883` |
| `fstl_schema.py` | `ae800499ea000bfba6a5d65660ff07ec3c45c1b0325eefda826a6d82aa5356c4` |
| `verify_schema_vectors.py` | `eb9ffaa552d00fc1d6f2c4a5dd4915833851403e2e0ac6243e55fce18d59c75b` |
| `verify_fstl_1_1_amendment.py` | `b47ec78e086d66d5346014bbee8f1c931c5e9d1a7faec5df6cedcbf2f8085eb3` |
| `fstl_reference_decoder.py` | `9821caaa6819d72602ce0303e132ecdb09eb406747551522cf06878a006f87ff` |
| `verify_telemetry_assets.py` | `8ad07e423222e056c6276721b7d2bb1e91ccabcd327006955d915100da533751` |

## RED-to-GREEN freeze repair

The failing baseline remains recorded in
`FSTL10_FREEZE_RED_EVIDENCE.md`. The repair did not rewrite that historical
evidence.

| Baseline finding | Final proof | Result |
|---|---|---|
| seven Phase 0 documents and `fstl-v1.yaml` had drifted | exact tag comparison plus independent byte/hash oracles | `GREEN` |
| the amendment reused the 1.0 schema identity | distinct generated `fstl-v1.1.yaml`, derived from and pinning the frozen base | `GREEN` |
| freeze covered only `vectors/**` | exhaustive 438-file ledger plus independent embedded tree root | `GREEN` |
| no anti-bypass verifier | 15 mutation tests cover bytes, add/delete, duplicate path, traversal and colluding tree/ledger refresh | `GREEN` |
| consumers did not route a distinct 1.1 schema | generator, layout verifier, amendment verifier, reference decoder and asset verifier all bind both versioned paths as applicable | `GREEN` |
| CI did not enforce the repair | both protocol workflows invoke the freeze verifier and the 15-test anti-bypass suite | `GREEN` registration |

Workflow registration was inspected locally; no new remote GitHub Actions run
is claimed by this report.

## Corpus and independent paths

The isolated FSTL 1.1 corpus still has 21 cases: 15 snapshot cases and six
datagram messages. Nine are valid and carry fixed canonical JSON; twelve are
invalid and carry one normative `expectedValidationError` ID. The amendment
generator, Python reference decoder and production C++ tests consume the same
checked-in cases and corpus through independent paths. For the 15 snapshots,
Python and C++ decode the same payload bytes. For the six datagrams, Python
validates the checked-in envelope and CRC before decoding its versioned
`payloadFile`, while C++ traverses and decodes the complete datagram; the
manifest and generator bind both representations.

The two cumulative DELTAs share baseline 1. Sequence 2 changes position,
quaternion, world velocity and local rotational velocity. Sequence 3 returns
those values to baseline. The C++ replication slice demonstrates convergence
after loss, duplication and reordering.

## Final independent commands and results

| Command | Exit | Stable result |
|---|---:|---|
| `python -B test/telemetry/protocol/tools/test_fstl_1_0_freeze.py` | 0 | 15/15 tests passed in 16.564 s, including every anti-bypass mutation |
| `python -B test/telemetry/protocol/tools/verify_fstl_1_0_freeze.py --check --repo .` | 0 | 438 files; tree `9baac6a2…856d` |
| `python -B test/telemetry/protocol/tools/fstl_schema.py --self-test` | 0 | 20 messages, 28 records; both versioned layouts and mutation checks passed |
| `python -B test/telemetry/protocol/tools/verify_schema_vectors.py --check` | 0 | FSTL 1.0 layout `d5e7ae20…e0aa`, probe `ee45ad75…5438` |
| previous command with `--schema test/telemetry/protocol/schema/fstl-v1.1.yaml` | 0 | FSTL 1.1 layout `416ff38c…1136`, same encoded probe |
| `python -B test/telemetry/protocol/tools/verify_fstl_1_1_amendment.py --check --repo .` | 0 | 15 snapshots, 6 messages, 3 negotiations; 438-file root matched |
| `python -B test/telemetry/protocol/tools/fstl_reference_decoder.py --check --repo .` | 0 | base 20/28; invalid 22+20 across 40 categories; transport 6+86; 21 FSTL 1.1 cases |
| `python -B test/telemetry/protocol/tools/verify_telemetry_assets.py --repo . --require-complete` | 0 | 182 fixtures (54 valid, 128 invalid), messages 20/20, records 28/28, 126 categories, 4 seeds |
| protocol and transport generators with `--check` | 0 | 20 MessageType, 28 RecordType and 198 transport files verified |
| `python -B test/telemetry/protocol/fuzz/test_run_fuzz_smoke.py` | 0 | 4/4 deterministic smoke tests passed; no sanitizer claim |
| `cmake --build build --config Release --target unittests --parallel 2` | 0 | Release unit-test target built |
| `unittests.exe --gtest_filter=TelemetryProtocolVectors.*` | 0 | 14/14 passed |
| five-test WP01 negotiation/version/profile/convergence filter | 0 | 5/5 passed |
| `unittests.exe --gtest_filter=TelemetryProtocol*` | 0 | 394/394 passed in 36 suites |
| Phase 0 specification validator | 0 | 8 documents, 5,300 lines |
| Phase 1 specification validator | 0 | 8 documents, 1,912 lines |
| tag diff over seven Phase 0 documents plus `fstl-v1.yaml` | 0 | no byte difference |
| `git diff --check` | 0 | no whitespace error |
| recursive `__pycache__` check after cleanup | 0 | no remaining directory |

## Requirement and gate reconciliation — WP01 scope

| ID | Final evidence/status |
|---|---|
| `P0-F-016` | `PASS` — versioned schema/corpus, bit 10, minimal snapshot and no-intersection tests |
| `D0-019` | `PASS` — additive `0x0400` profile with frozen 1.0 base, no manifest and no false `CORE_SHIP` |
| `P0-AC-025` | `PASS` — 438 exact artifacts, unchanged IDs/layout/CRC/fragmentation, two decoder paths and anti-bypass suite |
| `P0-AC-026` | `PASS` — exact `1..1` profile, minimal record-set and downgrade/incomplete-`CORE_SHIP` rejection |
| `P0.13` | `PASS` — amendment delivered without reopening any FSTL 1.0 artifact |
| `P1-WP-01` | `VERIFIED` for its contract scope |
| `P1-REQ-001` | **`FAIL` historical/non-repairable** — byte-freeze sub-clause now passes, but the mandatory WP ordering was violated |
| `P1-REQ-002` | `VERIFIED` for WP01 — additive minor and exact negotiation/rejection |
| `P1-REQ-012` | WP01 protocol slice verified; later implementation uses remain in their owning work packages |
| `P1-REQ-020` | `VERIFIED` for WP01 — versioned `PLAYER_KINEMATICS=0x0400`, immutable and forbidden in 1.0 |
| `P1-REQ-021` | `VERIFIED` for WP01 — exact minimal/no-player record-set and manifest zero |
| `P1-REQ-022`–`023` | WP01 wire slices verified; engine-source and lifecycle proofs remain WP07 |
| `P1-REQ-024`–`025` | not closed by WP01; engine-source/session-reuse proofs remain WP07 |
| `P1-REQ-034` | WP01 interoperability slice verified; the later client/tool package remains WP10 |
| `P1-AC-001` | `PASS` — frozen hashes and two independent vector paths |
| `P1-AC-002` | `PASS` — exact bit, record-set and version rejection |
| `G0-G` | **`PASS`**, closed first after test and reviewer acceptance |
| `G1-A` | **`PASS`**, closed only after `G0-G` |
| `P1-AC-020` | **`PASS`** — all Phase 1 proofs, gates and final review are accepted; `P1-REQ-001` remains a recorded historical/non-repairable `FAIL` under its limited governance disposition |

### Historical ordering violation

Commit `ac3250723` added the Phase 1 scaffold (`P1-WP-02`) while the evidence
committed with it explicitly said that `G0-G` and `G1-A` were blocked and did
not authorize WP02. Commits `2c4916ae6` and `e1ecd51f3` continued WP02 work,
and `9c0da12ed` started WP03 before the present gate closure. No waiver covered
those packages.

Closing `G0-G` and `G1-A` on the repaired tree authorizes future dependent
work. It does not convert this historical process failure into a pass and does
not by itself complete Phase 1.

### Disposition de gouvernance limitée — 2026-07-19

**Décision explicite de l'utilisateur, 2026-07-19 (Europe/Paris) :
acceptée avec limitation.** Cette disposition reconnaît la chronologie
immuable ci-dessus : `P1-WP-02`, puis `P1-WP-03`, ont commencé avant la
fermeture documentée de `G0-G`. Elle ne transforme donc pas
`P1-REQ-001` en `PASS` : son statut demeure **`FAIL` historique,
non-réparable**.

L'évaluation technique attachée à cette décision est restreinte aux
preuves présentes : `G0-G` est maintenant `PASS`, puis `G1-A` est `PASS`,
conformément aux preuves de gel FSTL 1.0 et d'amendement FSTL 1.1 référencées
dans ce rapport et dans `PHASE0_CONFORMANCE.md`. La décision n'autorise
aucune modification du wire FSTL 1.0/1.1, aucun changement de périmètre de
sécurité, ni aucune extension de fonctionnalité.

L'acceptation est limitée au traitement de cet écart historique dans
`P1-AC-020` ; elle ne constitue ni une dérogation rétroactive, ni un
précédent pour une dépendance ou une gate future. Mesure corrective : avant
l'ouverture de tout futur lot dépendant, le coordinateur consignera dans la
matrice de conformité le résultat de chaque gate prérequise, avec la preuve
associée ; le tracker en contrôlera la présence au point de passage du lot.

Effet précis : la disposition explicite exigée pour l'écart
`P1-REQ-001` est satisfaite. Elle n'était pas, à elle seule, une clôture de
Phase 1 ; la clôture intervient seulement avec la certification finale
ci-dessous.

## Certification finale Phase 1 — 2026-07-19

La certification du candidat `911de1f6e59ab309924806f91fc7e6fdaf4d0aaa`
est **PASS**. La revue indépendante a approuvé la clôture après le run GitHub
Actions [29698743284](https://github.com/dabatnot/fso_telemetry/actions/runs/29698743284)
(`Build Test package`, branche `test/telemetry-phase-1-certification`) :

- 14/14 jobs de compilation et tests ont réussi ;
- 7/7 jobs de paquets ZIP ont réussi, avec `Create Distribution package`
  réussi ;
- 7/7 étapes `Upload result package` ont été `skipped` dans le fork, comme
  requis par la garde SFTP réservée au dépôt officiel.

| ID | Disposition finale |
|---|---|
| `P1-REQ-036` | `PASS` — toutes les variantes supportées de la matrice certifiée compilent et exécutent leurs tests au SHA ci-dessus |
| `P1-AC-003` | `PASS` — matrice, seam du socle et fast path disabled acceptés ; la publication SFTP externe ne fait pas partie de ce critère |
| `G1-B` | `PASS/CLOSED` |
| `G1-G` | `PASS/CLOSED` — preuves WP11 (fuzz, réseau, soak, relances) et matrice build acceptées |
| `P1-AC-020` | `PASS` — 38 exigences, 20 critères, 11 lots et 7 gates réconciliés ; aucune réserve bloquante de revue |
| `P1-REQ-001` | **`FAIL` historique/non-réparable, disposition acceptée** — ce statut ne devient jamais `PASS` |

Cette certification ne modifie pas les checkboxes du contrat de livraison ;
elle archive la preuve et la décision de clôture qui permettent leur mise à
jour formelle par le tracker.
