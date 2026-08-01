# 07 — Livraison et traçabilité produit

## 1. Contenu livré

La Phase 1 livre l'extension additive FSTL 1.1 `PLAYER_KINEMATICS=0x0400`, le producteur solo read-only, sa configuration, le runtime UDP non bloquant, le premier flux snapshot/delta, le décodeur indépendant et le client console.

Les exigences Phase 0 restent des dépendances liées par référence ; elles ne sont pas dupliquées ci-dessous.

## 2. Table canonique des exigences

| REQ | Invariant ou observable produit | Test automatisé | Observation manuelle éventuelle |
|---|---|---|---|
| `P1-REQ-001` | Les artefacts FSTL 1.0 restent byte-identiques. | `test_fstl_1_0_freeze.py` | — |
| `P1-REQ-002` | FSTL 1.1 est additif et aucun élément 1.1 ne sort en session 1.0. | `verify_fstl_1_1_amendment.py` | — |
| `P1-REQ-003` | Producteur, décodeur indépendant et client console sont livrés ensemble. | `verify_telemetry_assets.py` | `P1-OBS-01` |
| `P1-REQ-004` | Le chemin entrant reste sans effet sur la simulation. | `telemetry_session_controller_contract_tests` | — |
| `P1-REQ-005` | L'intégration au moteur reste explicitement déclarée et limitée au module Telemetry. | contrôle des groupes de sources | — |
| `P1-REQ-006` | Configuration absente ou invalide : module désactivé avant bind. | `TelemetryConfigContract.*` | `P1-OBS-01` |
| `P1-REQ-007` | Les défauts sont loopback-only et fermés. | `TelemetryConfigContract.MinimalObjectAppliesEveryFailClosedDefault` | — |
| `P1-REQ-008` | Toutes les bornes de cadence, clients, heartbeat et budget datagrammes sont fermées. | `TelemetryConfigContract.IntegerBoundsAcceptMinAndMaxAndRejectTheirNeighbours` | — |
| `P1-REQ-009` | `producer_id` est persistant ; chaque `session_id` est non nul et non réutilisé. | `telemetry_phase2_session_transition_tests` | `P1-OBS-01` |
| `P1-REQ-010` | Le transport UDP privé est non bloquant en IPv4 et IPv6. | `telemetry_native_runtime_loopback_contract_tests` | `P1-OBS-01` |
| `P1-REQ-011` | Chaque tick traite au plus le budget configuré sans vider un backlog non borné. | `telemetry_native_runtime_integration_contract_tests` | — |
| `P1-REQ-012` | Les limites FSTL de datagramme, message, fragments et réassemblage sont appliquées. | `telemetry_phase2_security_bounds_tests` | — |
| `P1-REQ-013` | Source, session, allowlist et quotas sont validés avant mutation/allocation proportionnelle. | `telemetry_native_runtime_integration_contract_tests` | — |
| `P1-REQ-014` | Chaque client conserve une baseline, une candidate et des fenêtres bornées. | `telemetry_phase2_replication_tests` | `P1-OBS-02` |
| `P1-REQ-015` | `telemetry::initialize()` est idempotent et n'enregistre les callbacks qu'une fois. | `telemetry_initialize_contract_tests` | — |
| `P1-REQ-016` | Les globals moteur sont lus main-thread et seules des copies possédées survivent. | `telemetry_phase2_observation_contract_tests` | — |
| `P1-REQ-017` | Les transitions chargement, mission, menu et arrêt purgent la bonne portée. | `telemetry_lifecycle_callback_contract_tests` | `P1-OBS-01` |
| `P1-REQ-018` | Aucun état lourd n'est envoyé avant `ACK APPLIED` de `WELCOME`. | `telemetry_native_runtime_integration_contract_tests` | `P1-OBS-01` |
| `P1-REQ-019` | La synchronisation conserve au plus huit sondes/échantillons et sépare les horloges. | `telemetry_protocol_tests` | — |
| `P1-REQ-020` | `PLAYER_KINEMATICS=0x0400` est stable en 1.1 et distinct de `CORE_SHIP`. | `verify_fstl_1_1_amendment.py` | — |
| `P1-REQ-021` | Le snapshot Phase 1 a le record-set exact et `required_manifest_id=0`. | `telemetry_phase2_core_state_tests` | `P1-OBS-02` |
| `P1-REQ-022` | `FLIGHT_STATE` publie exactement les champs cinématiques prescrits. | `telemetry_phase2_core_state_tests` | `P1-OBS-02` |
| `P1-REQ-023` | L'identité joueur relie de façon stable session, lifecycle et flight. | `telemetry_native_runtime_integration_contract_tests` | `P1-OBS-02` |
| `P1-REQ-024` | Sources invalides, hors bornes ou non finies ne sont jamais publiées valides. | `telemetry_phase2_observation_contract_tests` | — |
| `P1-REQ-025` | Les IDs d'entité sont non nuls, monotones et indépendants des adresses/indices moteur. | `telemetry_phase2_lifecycle_support_tests` | — |
| `P1-REQ-026` | Le snapshot fiable est transactionnel, atomique et idempotent après perte d'ACK. | `telemetry_phase2_replication_tests` | `P1-OBS-02` |
| `P1-REQ-027` | Le dernier delta cumulatif de la baseline suffit à converger. | `telemetry_phase2_replication_tests` | `P1-OBS-02` |
| `P1-REQ-028` | La keyframe périodique conserve les mutations arrivées pendant son acquittement. | `telemetry_phase2_replication_tests` | — |
| `P1-REQ-029` | Une discontinuité mission/joueur produit une keyframe ou une nouvelle session. | `telemetry_phase2_session_transition_tests` | `P1-OBS-01` |
| `P1-REQ-030` | La resynchronisation est dédupliquée et ne conserve aucun historique non borné. | `telemetry_phase2_replication_tests` | `P1-OBS-02` |
| `P1-REQ-031` | Les métriques exposent unité, portée et familles de session/transport/réplication. | `telemetry_native_runtime_integration_contract_tests` | — |
| `P1-REQ-032` | Les logs sont agrégés et ne divulguent ni payload, secret ni chemin absolu. | `telemetry_native_runtime_integration_contract_tests` | — |
| `P1-REQ-033` | Désactivé : aucun socket/log récurrent ; actif : travail déterministe, borné et non bloquant. | `telemetry_native_runtime_integration_contract_tests` | `P1-OBS-01` |
| `P1-REQ-034` | Le décodeur indépendant lit 1.0/1.1 et produit/rejette les résultats canoniques. | outils Python FSTL | — |
| `P1-REQ-035` | Le client console affiche la session et sait ACK, resync et arrêt propre. | `test_fstl_console_client_contract.py` | `P1-OBS-01` |
| `P1-REQ-036` | Les variantes supportées compilent sans nouvelle dépendance externe. | builds CI Release | — |
| `P1-REQ-037` | Arrêt et redémarrage renouvellent les identités sans blocage ni croissance non bornée. | `telemetry_phase2_session_transition_tests` | `P1-OBS-02` |
| `P1-REQ-038` | Après une perte unique, le client converge sur le delta cumulatif ou la keyframe suivante. | `telemetry_phase2_replication_tests` | `P1-OBS-02` |

## 3. Observations neutres

| Scénario | Produit réellement observé | Attendu |
|---|---|---|
| `P1-OBS-01` | Configuration v1/v2, bind loopback, handshake, cycle mission et arrêt. | `session_phase`, `SOLO`, identités et `ProducerShutdown` exacts ; aucun socket si configuration refusée. |
| `P1-OBS-02` | Snapshot, delta, source temporairement indisponible et redémarrage. | Dernier état cohérent conservé et marqué stale ; delta cumulatif convergent ; aucune publication partielle. |

## 4. Décision de livraison

Le relevé humain consigne `attendu`, `observé`, `écart` et `impact`. Les observations restent courtes et ne produisent ni score, ni gate, ni verdict automatique.
