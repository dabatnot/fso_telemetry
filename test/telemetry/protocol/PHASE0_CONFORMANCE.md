# FSTL 1.0 — matrice de conformité de la Phase 0

## Verdict de gel

**Statut global : `BLOCKED`. La Phase 1 n'est pas autorisée par ce document.**

Le contrat, le socle C++ pur, les tests, le catalogue de vectors et les deux
chemins de décodage sont implémentés et verts localement. La gate finale décrite
par le document 07 reste néanmoins ouverte pour des preuves qui ne peuvent pas
être remplacées par un succès local :

- la campagne libFuzzer avec ASan/UBSan et les jobs CI n'ont pas encore tourné
  sur ce worktree ; le replay déterministe local a été exécuté sans sanitizer
  parce que Visual Studio 2019 16.4 rejette `/fsanitize=address` (`MSB8058`) ;
- les quatre approbations humaines, la version/tag de gel et l'autorisation
  explicite de démarrer la Phase 1 sont absents ;
- le worktree n'est pas encore rattaché à un commit final et les deux commits
  nouvellement récupérés d'`upstream/master` ne sont pas encore intégrés.

Ce fichier est une preuve d'audit et non une approbation. Il ne coche pas les
cases normatives du document 07 et n'usurpe aucun signataire.

## Périmètre et instantané audité

| Élément | Valeur observée |
|---|---|
| Date | 2026-07-14, Europe/Paris |
| Branche | `codex/telemetry-phase-0` |
| `HEAD` | `58a02935cfab682e345cd0dfb1d367005505ac5f` |
| `upstream/master` local | `57be2eb3333917e1519b9a9c6520e4551748c070` |
| Révision poussée sur `origin/codex/telemetry-phase-0` | `58a02935cfab682e345cd0dfb1d367005505ac5f` |
| Relation à `upstream/master` | `HEAD` est 5 commits devant et 2 derrière ; merge-base `583192b9f8b22649a921229f567379460343cd99` |
| État du worktree | sale ; code, tests, workflow, vectors et ce rapport ne sont pas tous dans `HEAD` |
| SHA-256 du schéma | `1d89c4a95a121c178bf85570cd616568fd939942b8d053835069b2d7d6a1f0d4` |
| SHA-256 du manifeste messages/records | `21cb0020029b6a6ffa68ea176c781eada4b7848f00ca7cae8031f912dd995931` |
| SHA-256 du manifeste transport | `9f7bd56abb543f82aeee0aa143de40e6f620fb48a99e6df854c8b1375f0ce6e6` |

L'audit inclut les fichiers trackés et non trackés visibles dans le worktree.
Le répertoire non tracké `tools/radar/assets/` est hors du périmètre Phase 0 :
il ne constitue aucune preuve FSTL, mais sera inclus dans le commit final
conformément à l'instruction explicite de l'utilisateur d'ajouter tous les
fichiers modifiés et non trackés.

Le sous-audit P0.12 n'a pas lancé MSBuild. L'agent principal a ensuite effectué
une reconstruction Release sérialisée du worktree courant et exécuté ses 383
cas `TEST`/`TEST_F`. Seuls les résultats reproductibles explicitement listés
ci-dessous sont retenus comme preuves.

## Légende

| Statut | Sens dans ce rapport |
|---|---|
| `PASS` | le critère précis possède une preuve reproductible exécutée ou une preuve statique suffisante pour ce critère |
| `PARTIAL` | l'artefact ou le test existe, mais une preuve obligatoire est absente, ancienne ou non exécutée sur le worktree courant |
| `BLOCKED` | une condition de gel indispensable est absente ou dépend d'une exécution/revue externe non disponible pendant l'audit |

Un `PASS` local n'autorise jamais le gel si un autre critère ou une revue reste
`BLOCKED`.

## Preuves reproduites pendant l'audit

Toutes les commandes partent de la racine du dépôt.

| Commande | Résultat observé | Statut |
|---|---|---|
| `& '.agents/skills/prepare-telemetry-phase-spec/scripts/validate_phase_specs.ps1' -PhaseNumber 0 -PhaseDirectory 'documentation/analysis/specs/0-Contrat-de-protocole'` | 8 documents, 5 299 lignes ; structure, UTF-8, liens, ancres et traçabilité valides | `PASS` |
| `& '.agents/skills/implement-telemetry-phase/scripts/inspect_phase_contract.ps1' -PhaseNumber 0` | 27 exigences, 24 critères, 18 décisions, 12 lots et 6 gates inventoriés | `PASS` |
| `python -B test/telemetry/protocol/tools/fstl_schema.py --self-test` | 20 messages, 28 records, 5 capabilities, 48 erreurs, 134 registres liés au C++ ; 192/192 champs message, probes 422/422 champs record, 25/25 structures et 241/241 champs imbriqués ; 8 tests négatifs de drift passés | `PASS` |
| `python -B test/telemetry/protocol/tools/generate_transport_vectors.py --check` | 198 fichiers de transport vérifiés | `PASS` |
| `python -B test/telemetry/protocol/tools/generate_protocol_vectors.py --check` | 20 vectors MessageType et 28 vectors RecordType vérifiés | `PASS` |
| `python -B test/telemetry/protocol/tools/verify_schema_vectors.py --check` | 2 479 octets golden reconstruits ; 220/422 champs record présents dans les golden minimaux ; probes figés séparés à 422/422, 25/25 structures, 241/241 champs, `EventItemV1` 30/30 ; 4 mutations wire rejetées | `PASS` |
| `python -B test/telemetry/protocol/tools/fstl_reference_decoder.py --check` | 20 messages, 28 records, 22 messages invalides, 20 records invalides, 40 catégories négatives, 6 transports valides et 86 invalides ; CRC et cross-endian simulé passés | `PASS` |
| `python -B test/telemetry/protocol/tools/verify_telemetry_assets.py --require-complete` | 182 fixtures : 54 valides, 128 invalides ; messages 20/20, records 28/28, 126 catégories invalides, 4 graines transport | `PASS` |
| `python -B test/telemetry/protocol/fuzz/prepare_fuzz_corpus.py --vectors test/telemetry/protocol/vectors --output build/telemetry-fuzz-audit-corpus` | 189 entrées réparties entre 8 corpus | `PASS` pour l'amorçage |
| `cmake --build build --config Release --target unittests -- /m:1 /nr:false /verbosity:minimal` | reconstruction Release terminée avec code de sortie 0 | `PASS` |
| `build/bin/Release/unittests.exe --gtest_filter=TelemetryProtocol*` | 383/383 tests passés dans 36 suites, 494 ms | `PASS` |
| build `telemetry_fuzz_smoke` avec `FSO_TELEMETRY_FUZZ_STANDALONE=ON`, `FSO_TELEMETRY_FUZZ_SANITIZERS=OFF` | 8 replayers compilés et passés : 176/95/91/16/58/36/6/12 graines | `PASS` smoke déterministe ; **pas une preuve sanitizer** |
| configuration locale identique avec sanitizers par défaut | Visual Studio 2019 16.4 ne supporte pas l'option, erreur `MSB8058` | `BLOCKED` localement ; CI Clang requise |
| `git diff --check` | aucune erreur whitespace ; avertissements de conversion LF/CRLF seulement | `PASS` |
| `rg -n '^#include' code/telemetry/protocol` et scan des symboles moteur/réseau | uniquement headers protocole et bibliothèque standard ; aucun collecteur, socket, renderer ou type moteur observé | `PASS` statique pour la frontière Phase 0 |

## Écarts bloquants avant gel

1. **Fuzzing/sanitizers incomplets.** Les huit cibles et le replay déterministe
   sont verts, mais le compilateur local ne fournit pas ASan. Aucun résultat de
   mutation libFuzzer sous ASan/UBSan, aucune campagne longue approuvée et aucun
   rapport sanitizer ne sont encore archivés.
2. **CI non prouvée.** `.github/workflows/telemetry-protocol.yaml` est non
   tracké et aucun run vert de cette révision n'existe dans les artefacts
   locaux.
3. **Revues externes absentes.** Aucun signataire, décision, date ou lien de
   preuve n'est enregistré pour métier, transport/interop, sécurité ou client.
4. **Version de gel absente.** Aucun tag FSTL/telemetry ne pointe sur `HEAD` et
   aucun identifiant d'artefact gelé n'est enregistré.
5. **Worktree non final.** Les preuves hashées ne correspondent pas encore à
   un commit unique. `upstream/master` a avancé de deux commits déjà récupérés
   mais non intégrés. Le contenu hors périmètre `tools/radar/assets/` sera
   inclus sur instruction utilisateur, sans être compté comme preuve FSTL.

## Matrice des critères d'acceptation P0-AC-001 à P0-AC-024

| ID | Statut | Preuves présentes | Reste à fermer |
|---|---|---|---|
| `P0-AC-001` | `PASS` | docs 02/03/05 ; schéma 20/20 ; 20 payloads binaires et décodage Python | conserver ce résultat après commit final |
| `P0-AC-002` | `PASS` | doc 04 ; schéma 28/28 avec scope/atome/champs de premier niveau ; 28 envelopes binaires décodées | enrichissement machine-readable imbriqué suivi sous AC-024 |
| `P0-AC-003` | `PASS` | 134 registres extraits des docs, tous liés aux constantes C++ ; politiques enum/bitmap/flags vérifiées par self-test | conserver le rapport CI |
| `P0-AC-004` | `PASS` | constantes 68/1200/1132 ; vectors aux frontières 0, 1, 1132, 1133, 2264 et 2265 octets ; suite Release verte | conserver le résultat CI final |
| `P0-AC-005` | `PASS` | check `123456789 == 0xcbf43926`, implémentation Python bitwise indépendante, manifests, décodeur transport et tests C++ | conserver le résultat CI final |
| `P0-AC-006` | `PARTIAL` | reassembler/transaction/security et tests de quotas verts ; recherches d'état et index de chemins validés en O(N log N) sur 1 024 entités et 200 assets ; replayers fuzz verts | joindre fuzz/sanitizers et le rapport final de budgets/allocation |
| `P0-AC-007` | `PASS` | six codecs de contrôle, session, clock et reliability harness exécutés dans les 383 tests | conserver rapport/graines en CI |
| `P0-AC-008` | `PASS` | `AntiAmplificationBudget` et scénarios de session exécutés | revue sécurité finale distincte |
| `P0-AC-009` | `PASS` | transaction manager, limites 1/2/64 parts et tests d'installation atomique exécutés | conserver en CI |
| `P0-AC-010` | `PASS` | promotion conditionnée aux ACK exacts, tests transaction/réplication verts | conserver en CI |
| `P0-AC-011` | `PASS` | delta cumulatif et harness de pertes intermédiaires verts | conserver graines en CI |
| `P0-AC-012` | `PASS` | double dirty-set et mutation post-capture exécutés | conserver en CI |
| `P0-AC-013` | `PASS` | matrice d'inconnus, catalogue invalide complet, deux décodeurs et tests C++ concordants | conserver en CI |
| `P0-AC-014` | `PASS` | NaN/Inf, UTF-8, tailles, offsets et bitmaps incohérents rejetés par vectors/tests ; replay fuzz vert | sanitizer reste suivi sous AC-020 |
| `P0-AC-015` | `PASS` technique | validation de visibilité/références métier et tests Cockpit/Trusted verts | approbation humaine modèle/Cockpit toujours requise pour le gel |
| `P0-AC-016` | `PASS` | négociation bundle, manifeste, lifecycle et fallbacks communication exécutés | approbation client toujours requise pour le gel |
| `P0-AC-017` | `PASS` | lifecycle et fixture `video_old_target_after_change` rejettent l'ancienne génération/cible | conserver en CI |
| `P0-AC-018` | `PASS` | classification QoS, token buckets et test de saturation vidéo exécutés | conserver en CI/fuzz |
| `P0-AC-019` | `PASS` | 54 fixtures valides et 42 invalides messages/records concordent entre Python et C++ ; 86 transports invalides également rejoués | conserver le double replay en CI |
| `P0-AC-020` | `BLOCKED` | 8 replayers déterministes passent le corpus ; workflow libFuzzer ASan/UBSan présent | exécuter la CI sanitizer et une campagne de gel avec durée/corpus archivés |
| `P0-AC-021` | `PASS` technique | defaults loopback, discovery, allowlist et `TrustedFullState` opt-in exécutés | approbation humaine configuration sûre toujours requise |
| `P0-AC-022` | `PASS` | 20 messages read-only, authority mapper et tests d'absence de commande verts | conserver en CI |
| `P0-AC-023` | `PASS` | bibliothèque C++17 pure ; scan sans type moteur/socket/renderer ; suite dédiée verte | revue finale du diff committé |
| `P0-AC-024` | `PASS` | docs → schéma → 134 constantes ; golden minimaux reconstruits ; probes figés 192/192, 422/422, 25/25, 241/241 et 4 mutations wire rejetées | conserver les deux empreintes figées et le self-test en CI |

## Matrice des exigences P0-F et P0-NF

| ID | Statut | Chemin de preuve principal | Écart principal |
|---|---|---|---|
| `P0-F-001` | `PASS` | `fstl-v1.yaml`, `telemetry_protocol_constants.h`, self-test 134/134 | aucun écart de registre observé |
| `P0-F-002` | `PASS` | docs 02/04/05 ; 20 layouts messages ; 28 records ; 25 structures `*V1` ; `verify_schema_vectors.py` | aucun écart machine-readable observé |
| `P0-F-003` | `PASS` | datagram/constants, 198 fichiers transport et tests C++ verts | conserver en CI |
| `P0-F-004` | `PASS` | `packet_writer.*`, `packet_reader.*` et tests Release verts | conserver en CI |
| `P0-F-005` | `PARTIAL` | datagram/reassembler/transaction/security, quotas et tests verts | sanitizer/allocation instrumentée finale absente |
| `P0-F-006` | `PASS` | control/reliability messages et harness exécutés | conserver en CI |
| `P0-F-007` | `PASS` | session, capabilities et clock exécutés | conserver en CI |
| `P0-F-008` | `PASS` | reliable receive/window/rate limiter et replay stateful verts | sanitizer suivi sous P0.11 |
| `P0-F-009` | `PASS` | transaction/replication et harness exécutés | conserver en CI |
| `P0-F-010` | `PASS` | validators 1–28, schéma 422/422 et tests métier exécutés | revue modèle distincte pour le gel |
| `P0-F-011` | `PASS` | specialized views/lifecycle/comm manifest et SPS/VUI exécutés | conserver en CI |
| `P0-F-012` | `PASS` | catalogue 54 valides/128 invalides ; double décodage exact des 42 invalides message/record | conserver manifests et replays en CI |
| `P0-F-013` | `PASS` | docs 02 §3/7.4, 06 §8, `unknown_value_policy` | conserver en CI |
| `P0-F-014` | `PASS` | 48 `ValidationError` extraites et liées au C++ | métriques d'intégration appartiennent aux phases ultérieures |
| `P0-F-015` | `PASS` | registre, authority mapper, scan d'API et tests Release | conserver en CI |
| `P0-NF-001` | `PARTIAL` | quotas/budgets/containers bornés et tests exécutés | fuzz/sanitizers et rapport de budgets final absents |
| `P0-NF-002` | `PASS` | aucune opération socket dans Phase 0 | non applicable au socle pur au-delà de l'absence |
| `P0-NF-003` | `PASS` | max datagram 1200 et fragmentation applicative | vectors transport passés |
| `P0-NF-004` | `PARTIAL` | generators déterministes, manifests/empreintes hashés, MSVC Release vert | exécution CI Clang/macOS non archivée |
| `P0-NF-005` | `PASS` | simulation little/big-endian Python et comparaison C++/Python passées | conserver en CI multi-plateforme |
| `P0-NF-006` | `PARTIAL` | catalogue négatif complet et replay déterministe vert | mutation libFuzzer/sanitizers manquants |
| `P0-NF-007` | `PARTIAL` | quotas client/globaux et tests source | sanitizer/fuzz et rapport de pic absents |
| `P0-NF-008` | `PASS` | scheduler QoS et scénario de saturation exécutés | conserver en CI |
| `P0-NF-009` | `PASS` | règles major/minor/record dans docs et schéma | aucun drift détecté |
| `P0-NF-010` | `PASS` | docs, Python stdlib, `.bin`, JSON canonique et catalogue complet | conserver en CI |
| `P0-NF-011` | `PASS` technique | config sécurité et tests opt-in exécutés | revue sécurité humaine absente |
| `P0-NF-012` | `PASS` | bibliothèque pure sans collecteur ni hook moteur | confirmer sur le diff final committé |

## Décisions D0-001 à D0-018

| Décision | Statut | Artefact principal | Observation de gel |
|---|---|---|---|
| `D0-001` | `PASS` | transaction + manifest/snapshot validation | tests Release verts |
| `D0-002` | `PASS` technique | business state/visibility validation | revue métier Cockpit absente |
| `D0-003` | `PASS` | session/producer IDs séparés ; aucune fusion | conforme à la frontière pure |
| `D0-004` | `PASS` | `SESSION_STATE` et registries EventFamily | validation C++ verte |
| `D0-005` | `PASS` | COMM manifest ne transporte que métadonnées | aucun transfert de fichier dans le module |
| `D0-006` | `PASS` | deux bits communication distincts | self-test registry passé |
| `D0-007` | `PASS` | deux bits vidéo distincts + messages spécialisés | self-test registry passé |
| `D0-008` | `PASS` | `TARGET_VIDEO_STATS` client → producteur | tests codec/lifecycle verts |
| `D0-009` | `PASS` technique | `RADAR_CONTACTS.visibility` et validator | revue métier/visibilité absente |
| `D0-010` | `PASS` | transactions 64 parts/16 MiB | tests de bornes verts |
| `D0-011` | `PASS` | classification des constantes dans schéma/docs | self-test passé |
| `D0-012` | `PASS` | matrices version/inconnu docs + schéma + catalogue invalide | double replay vert |
| `D0-013` | `PASS` | fields render-profile dans SUBSCRIBE + vector | decoder Python passé |
| `D0-014` | `PASS` | STOP bidirectionnel 24 octets + vector | decoder Python passé |
| `D0-015` | `PASS` | CapabilityUpdate monotone + lifecycle | tests finaux verts |
| `D0-016` | `PASS` | constantes 200 ms / 500 ms | registries/constants vérifiés |
| `D0-017` | `PASS` | presence/ID/enum validators | catalogue et tests verts ; sanitizer séparé |
| `D0-018` | `PASS` technique | config loopback, allowlist, rate limits | tests verts ; revue sécurité absente |

La clarification validée pendant l'implémentation est reflétée dans les docs et
le code : un delta d'une baseline candidate connue peut être retenu de façon
bornée ; un delta d'une baseline totalement inconnue est abandonné et déclenche
un `ResyncRequest UnknownBaseline` rate-limité. Les tests source
`KnownCandidateKeepsOnlyHighestDeltaWithinAbsoluteTwoSecondWindow` et
`TotallyUnknownBaselineDropsAndUsesExistingPerSessionResyncBucket` matérialisent
les deux branches et passent dans la suite Release du worktree courant.

## Lots P0.1 à P0.12 et gates

| Lot | Gate | Statut | Justification |
|---|---|---|---|
| `P0.1` conventions | `G0-A` | `PASS` | docs/schéma/scalaires/registres cohérents automatiquement |
| `P0.2` registres | `G0-A` | `PASS` | 134/134 registries liés au C++, tests de drift passés |
| `P0.3` header/CRC/fragmentation | `G0-B` | `PASS` | vectors et C++ Release verts |
| `P0.4` session/horloges | `G0-B` | `PASS` | suite session/clock/harness verte |
| `P0.5` fiabilité | `G0-D` | `PASS` | reliability window/receive/harness verts |
| `P0.6` réplication | `G0-D` | `PASS` | convergence et clarification baseline exécutées |
| `P0.7` schéma métier | `G0-C` | `PASS` technique | 28 validators/vectors, probes complets et tests verts ; revue métier séparée |
| `P0.8` vues | `G0-E` | `PASS` technique | codecs/lifecycle/QoS/SPS-VUI verts ; revue client séparée |
| `P0.9` sécurité | `G0-F` | `PARTIAL` | modèle, catalogue et tests verts ; sanitizer/revue absents |
| `P0.10` vectors/decoder | `G0-F` | `PASS` | catalogue complet et double décodage exact |
| `P0.11` fuzz/CI | `G0-F` | `BLOCKED` | 8 replayers verts ; ASan/UBSan mutation et CI absents |
| `P0.12` gel | gel | `BLOCKED` | upstream non intégré, approbations, tag, commit final et autorisation Phase 1 absents |

Synthèse des gates techniques : `G0-A`, `G0-B`, `G0-C`, `G0-D` et `G0-E PASS`;
`G0-F BLOCKED` faute de sanitizer/CI et d'approbations de gel.

## Preuves exigées par la revue finale

| Preuve du document 07 §11 | Statut | Artefact/commande |
|---|---|---|
| version et hash du schéma | `PASS` | hash enregistré dans l'instantané ci-dessus |
| IDs sans collision | `PASS` | `fstl_schema.py --self-test` |
| couverture messages/records/enums/flags | `PASS` | 20/20, 28/28, 134 registres ; probes 422/422 et 25/25 |
| inventaire et hash des vectors | `PASS` | manifests hashés ; 182 fixtures inventoriées |
| résultats des deux décodeurs | `PASS` | Python et C++ concordent sur valides et 42 invalides message/record |
| résultats unitaires/propriétés | `PASS` local | 383/383 tests Release, 36 suites |
| graines/résultats harness | `PASS` local | 4 graines transport et harness Release verts |
| durée/corpus/configuration fuzz | `PARTIAL` | corpus 189 entrées et replay déterministe archivables ; campagne mutation absente |
| rapport sanitizers | `BLOCKED` | MSVC local incompatible ; jobs Clang non exécutés |
| budgets au nombre max de clients | `PARTIAL` | calculs/code/tests exécutés ; rapport de pic final absent |
| revue filtrage Cockpit | `BLOCKED` | aucun signataire/artefact humain |
| revue configuration sûre | `BLOCKED` | aucun signataire/artefact humain |
| divergences résolues | `PASS` technique | D0-001..018, clarification baseline et replays contextuels cohérents ; revue finale absente |
| absence collecteur/hook moteur | `PASS` | scan statique du module ; à répéter sur diff committé final |

## Approbations requises — volontairement non remplies

| Revue | Approbateur nommé | Preuve liée | Décision | Date |
|---|---|---|---|---|
| modèle FS2Open | — | — | **NON APPROUVÉE / preuve absente** | — |
| protocole / interop | — | — | **NON APPROUVÉE / preuve absente** | — |
| sécurité / robustesse | — | — | **NON APPROUVÉE / preuve absente** | — |
| client / fallbacks | — | — | **NON APPROUVÉE / preuve absente** | — |

Une même personne peut cumuler des rôles, mais chaque cellule doit alors porter
son nom, la décision, la date et un lien vers les preuves reproductibles. Un
nom ajouté par l'implémenteur sans décision explicite n'est pas une approbation.

## Checklist de passage à la Phase 1

| Item du document 07 §12 | Statut |
|---|---|
| docs 01–07 cohérentes et liens valides | `PASS` |
| schéma couvre 20 messages et 28 records | `PASS` — probes 192/192, 422/422, 25/25 et 241/241 |
| valeurs numériques gelées et vérifiées | `PASS` |
| writer/reader/CRC/fragmenter/réassembleur bornés | `PASS` local — suite Release verte |
| six messages de contrôle implémentés hors moteur | `PASS` local — codecs et harness verts |
| handshake/anti-amplification/horloges passent le harness | `PASS` local |
| ACK/NACK/backoff/expiration/resync déterministes | `PASS` local |
| snapshot/manifeste/delta convergent sous perte | `PASS` local |
| champs d'inventaire tracés | `PARTIAL` — doc complète, revue métier absente |
| capabilities visuelles se dégradent indépendamment | `PASS` local |
| fixtures valides et invalides complètes | `PASS` |
| deux décodeurs indépendants concordent | `PASS` |
| fuzzing/sanitizers/overflow verts | `BLOCKED` |
| valeurs sûres par défaut testées | `PASS` local |
| quatre revues approuvées | `BLOCKED` |
| Phase 0 versionnée | `BLOCKED` |

## Commandes requises pour fermer les preuves techniques

Les commandes Python suivantes doivent rester vertes sur le commit final :

```powershell
& '.agents/skills/prepare-telemetry-phase-spec/scripts/validate_phase_specs.ps1' `
  -PhaseNumber 0 `
  -PhaseDirectory 'documentation/analysis/specs/0-Contrat-de-protocole'
& '.agents/skills/implement-telemetry-phase/scripts/inspect_phase_contract.ps1' `
  -PhaseNumber 0
python -B test/telemetry/protocol/tools/fstl_schema.py --self-test
python -B test/telemetry/protocol/tools/generate_transport_vectors.py --check
python -B test/telemetry/protocol/tools/generate_protocol_vectors.py --check
python -B test/telemetry/protocol/tools/fstl_reference_decoder.py --check
python -B test/telemetry/protocol/tools/verify_telemetry_assets.py `
  --require-complete --report build/telemetry-coverage.json
```

La reconstruction locale courante a utilisé :

```powershell
cmake --build build --config Release --target unittests -- /m:1 /nr:false /verbosity:minimal
build/bin/Release/unittests.exe --gtest_filter=TelemetryProtocol*
```

Elle a produit 383/383 succès. Après commit et intégration d'upstream, la CI
doit reconstruire la révision finale ; un binaire plus ancien que les sources
n'est pas accepté.

Pour les fuzzers, utiliser les commandes documentées dans
`test/telemetry/protocol/fuzz/README.md`, puis archiver toolchain, options
ASan/UBSan, durée ou nombre de runs, hash du corpus et dossier d'artefacts. Le
smoke CI de 2 000 runs est une gate de PR ; une campagne de gel doit fournir la
durée explicitement approuvée par la revue sécurité.

Enfin :

1. intégrer les deux commits récupérés d'`upstream/master`, créer le commit
   final incluant code, tests, vectors, workflow et documentation, puis pousser ;
2. faire tourner les jobs Clang/macOS/Windows, en particulier libFuzzer avec
   ASan/UBSan, et archiver durée, corpus et rapports ;
3. rejouer les checks et tests sur le commit final ;
4. faire approuver les quatre revues avec preuves ;
5. enregistrer l'identifiant/version de l'artefact et créer le tag convenu ;
6. seulement ensuite remplacer le verdict global par `PASS` et consigner
   l'autorisation explicite de démarrer la Phase 1.

## Version et autorisation de gel

| Champ | Valeur |
|---|---|
| Version d'artefact FSTL 1.0 | — |
| Commit gelé | — |
| Tag | — |
| Hash du bundle de preuves | — |
| Autorisation explicite de démarrer la Phase 1 | **NON** |
