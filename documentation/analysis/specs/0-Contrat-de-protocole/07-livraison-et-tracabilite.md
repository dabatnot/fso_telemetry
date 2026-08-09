# 07 — Livraison et traçabilité produit

## 1. Contenu livré

La Phase 0 livre le contrat filaire FSTL 1.0, ses schémas, ses golden vectors byte-identiques, un décodeur indépendant et les règles bornées de session, fiabilité, sécurité et évolution. Elle ne livre ni producteur intégré au jeu, ni procédure autonome de certification.

## 2. Table canonique des exigences

Chaque exigence active apparaît une seule fois dans cette table. Une preuve automatisée désigne un test court ou un vérificateur déterministe ; l'observation manuelle ne remplace jamais une preuve binaire lorsque celle-ci est possible.

| REQ | Invariant ou observable produit | Test automatisé | Observation manuelle éventuelle |
|---|---|---|---|
| `P0-F-001` | Toutes les valeurs numériques FSTL 1.0 sont attribuées sans collision. | `verify_telemetry_assets.py` | — |
| `P0-F-002` | Chaque champ possède ordre, type, unité, borne et règle d'absence. | `verify_telemetry_assets.py` | — |
| `P0-F-003` | L'en-tête mesure exactement 68 octets et ne dépend d'aucun padding ABI. | `test_fstl_1_0_freeze.py` | — |
| `P0-F-004` | Les lecteurs et writers sont bornés et sans cast réseau implicite. | `telemetry_protocol_tests` | — |
| `P0-F-005` | CRC, tailles, offsets et quotas sont validés avant allocation proportionnelle. | `telemetry_phase2_packet_reader_corpus_replay` | — |
| `P0-F-006` | Les six messages de contrôle de base s'encodent et se décodent sans dépendre du moteur. | `telemetry_protocol_tests` | `P0-OBS-01` |
| `P0-F-007` | Handshake, négociation et synchronisation d'horloge suivent la machine normative. | `telemetry_protocol_tests` | `P0-OBS-01` |
| `P0-F-008` | Fenêtre fiable, backoff, expiration, déduplication et resync restent bornés. | `telemetry_protocol_tests` | `P0-OBS-01` |
| `P0-F-009` | Snapshot candidat, `ACK APPLIED`, baseline immuable et delta cumulatif sont atomiques. | `telemetry_phase2_replication_tests` | `P0-OBS-02` |
| `P0-F-010` | Les 28 records actifs et leurs groupes conditionnels sont décodables. | `verify_telemetry_assets.py` | — |
| `P0-F-011` | Les capabilities et sous-états spécialisés restent indépendants. | `verify_fstl_1_0_freeze.py` | — |
| `P0-F-012` | Les golden vectors valides et invalides sont utilisables sans FS2Open. | `test_fstl_1_0_freeze.py` | — |
| `P0-F-013` | La compatibilité major, minor et record inconnu est déterministe. | `telemetry_protocol_tests` | — |
| `P0-F-014` | Chaque rejet, drop et métrique utilise une taxonomie fermée. | `telemetry_protocol_tests` | `P0-OBS-01` |
| `P0-F-015` | Aucun message client ne peut atteindre une commande de simulation. | `telemetry_protocol_tests` | `P0-OBS-01` |
| `P0-NF-001` | Aucune allocation ni file active n'est non bornée. | `telemetry_phase2_security_bounds_tests` | — |
| `P0-NF-002` | Aucun appel réseau actif n'est bloquant. | `telemetry_native_runtime_loopback_contract_tests` | `P0-OBS-01` |
| `P0-NF-003` | Le protocole n'utilise pas la fragmentation IP. | `verify_telemetry_assets.py` | — |
| `P0-NF-004` | Une valeur canonique et une version données produisent les mêmes octets. | `test_fstl_1_0_freeze.py` | — |
| `P0-NF-005` | L'encodage explicite little-endian ne dépend pas de l'architecture hôte. | `telemetry_protocol_tests` | — |
| `P0-NF-006` | Troncatures, incohérences, non-finis et valeurs critiques inconnues sont rejetés sans crash. | corpus fuzz minimal + `telemetry_protocol_tests` | — |
| `P0-NF-007` | La mémoire de réassemblage est bornée par client et globalement. | `telemetry_phase2_security_bounds_tests` | — |
| `P0-NF-008` | L'état conserve la priorité sur les vues optionnelles sous congestion. | `telemetry_protocol_tests` | — |
| `P0-NF-009` | Une évolution minor reste additive et une rupture major est explicite. | `telemetry_protocol_tests` | — |
| `P0-NF-010` | Schémas, fixtures et décodeur sont exploitables hors C++. | outils Python FSTL | `P0-OBS-01` |
| `P0-NF-011` | La configuration et l'exposition réseau sont fermées hors loopback. | `telemetry_config_contract_tests` | — |
| `P0-NF-012` | La Phase 0 ne touche pas la boucle moteur. | contrôle statique des groupes de sources | — |

## 3. Observations neutres

| Scénario | Produit réellement observé | Attendu |
|---|---|---|
| `P0-OBS-01` | Bootstrap wire avec le décodeur/client indépendant. | En-tête et messages valides ; ACK/NACK inconnus, tardifs ou incohérents ignorés et comptabilisés sans fermeture. |
| `P0-OBS-02` | Snapshot/manifeste artificiel court et erreur structurelle. | Cardinalité annoncée, manifeste appliqué et snapshot exactement égaux ; une structure non représentable se termine par `SESSION_END(Restart, RECONNECT_ALLOWED)` avant publication partielle. |

## 4. Décision de livraison

Le relevé humain consigne `attendu`, `observé`, `écart` et `impact`. Les golden vectors et hashes restent des preuves automatiques ; la décision de publication appartient à l'humain et ne produit aucun score ni verdict d'éligibilité.
