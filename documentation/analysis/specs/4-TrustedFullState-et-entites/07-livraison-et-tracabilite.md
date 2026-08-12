# 07 — Livraison et traçabilité

## Table canonique des exigences

| REQ | Invariant ou observable produit | Test automatisé | Observation manuelle éventuelle |
|---|---|---|---|
| `P4-REQ-001` | Profils et wire antérieurs byte-identiques. | freeze FSTL + `telemetry-short` | — |
| `P4-REQ-002` | Handshake `SOLO + TRUSTED_FULL_STATE`, FSTL 1.1, `0x07DB` immuable; multijoueur refusé sans downgrade. | test de négociation Phase 4 | — |
| `P4-REQ-003` | Refus avant session sans opt-in/allowlist. | test autorisation Phase 4 | — |
| `P4-REQ-004` | Inventaire exact des types exportables. | test projection entités Phase 4 | `P4-OBS-01` |
| `P4-REQ-005` | IDs non nuls, monotones et non réutilisés. | test identités Phase 4 | — |
| `P4-REQ-006` | Lifecycle et présences FSTL fermés. | test codec/validation lifecycle | — |
| `P4-REQ-007` | Classe installée avant référence, seulement pour `SHIP` et `WEAPON`. | test manifestes Phase 4 | — |
| `P4-REQ-008` | Références résolues, parenté acyclique. | test graphe Phase 4 | — |
| `P4-REQ-009` | Docking global réciproque, purge exacte et refus à 65 relations. | test docking Phase 4 | `P4-OBS-01` |
| `P4-REQ-010` | États détaillés seulement pour `SHIP`. | test matrice vaisseaux Phase 4 | — |
| `P4-REQ-011` | Chaque `WEAPON` exporté a lifecycle et `FLIGHT_STATE`. | test projectiles Phase 4 | `P4-OBS-01` |
| `P4-REQ-012` | Toute entité positionnable a `FLIGHT_STATE`; statiques à vitesse nulle/quaternion identité. | test projection entités Phase 4 | — |
| `P4-REQ-013` | Join-in-progress: manifestes puis image atomique. | loopback join Phase 4 | — |
| `P4-REQ-014` | Keyframe/resync reconstruisent le graphe exact. | loopback resync Phase 4 | — |
| `P4-REQ-015` | Deltas cumulatifs remplacent l’état net. | test réplication Phase 4 | — |
| `P4-REQ-016` | Perte unique: convergence sans relation pendante. | loopback `--drop-once delta` | — |
| `P4-REQ-017` | Limites refusées avant allocation/troncature. | test bornes Phase 4 | — |
| `P4-REQ-018` | Aucune omission dans keyframe complète. | test keyframe Phase 4 | — |
| `P4-REQ-019` | Main-thread, non-bloquant, lecture seule. | test runtime Phase 4 | — |
| `P4-REQ-020` | Observabilité fermée sans données sensibles. | test logs/métriques Phase 4 | — |

## Observations neutres

| OBS | Relevé humain |
|---|---|
| `P4-OBS-01` | Pendant une mission locale contrôlée, comparer pendant moins de cinq minutes vaisseaux, projectiles et docking visibles par le client de confiance avec le graphe produit; relever `attendu`, `observé`, `écart` et `impact`. |

## Décision de livraison

Les tests courts observent les résultats exacts définis ci-dessus. L’observation est informative et l’humain décide seul de la livraison; aucun score, tracker, verdict automatique, soak ou campagne de certification ne la décide.
