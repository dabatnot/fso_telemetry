# 07 — Livraison et traçabilité

## 1. Contenu livré

La Phase 3 livre un seul nouveau comportement de session :
`CockpitSensors`, couverture `0x07CB`. Les exigences des phases antérieures sont
des dépendances et ne sont pas recopiées.

## 2. Table canonique des exigences

| REQ | Invariant ou observable produit | Test automatisé | Observation manuelle éventuelle |
|---|---|---|---|
| `P3-REQ-001` | Toutes les garanties Phase 2 restent actives. | suite `telemetry-short` | — |
| `P3-REQ-002` | FSTL 1.0 et `RADAR_CONTACTS` v1/v2/v3 restent byte-identiques ; le layout live v4 est explicite sous FSTL 1.1. | `test_fstl_1_0_freeze.py` + vérificateur 1.1 + golden v2/v3/v4 | — |
| `P3-REQ-003` | Une session Phase 3 négocie exactement FSTL 1.1. | `telemetry_phase3_session_tests` | — |
| `P3-REQ-004` | Le profil `CockpitSensors` annonce exactement `0x07CB`. | `TelemetryPhase3Profile.FrozenCoverage` | — |
| `P3-REQ-005` | Profil et couvertures restent immuables pendant la session. | `telemetry_phase3_session_tests` | — |
| `P3-REQ-006` | Le mode livré est exactement `SOLO + COCKPIT`. | `TelemetryPhase3Profile.EligibilityIsClosed` | — |
| `P3-REQ-007` | Le producteur reste en lecture seule et aucune commande gameplay n’existe. | `telemetry_phase3_read_only_tests` | — |
| `P3-REQ-008` | Entités globales, effets, communication et vidéo restent hors profil. | `telemetry_phase3_profile_tests` | — |
| `P3-REQ-009` | Toute capture Phase 3 se fait sur le thread principal sans worker ni seam de test. | `telemetry_phase3_capture_contract_tests` | — |
| `P3-REQ-010` | Toutes les sources et références moteur sont validées avant lecture. | `telemetry_phase3_capture_contract_tests` | — |
| `P3-REQ-011` | Les DTO possèdent toutes leurs données et ne conservent aucun index ou pointeur moteur. | `TelemetryPhase3Dto.OwnsCapturedData` | — |
| `P3-REQ-012` | Ciblage suit `flightHz`, capteurs/opérations suivent `systemsHz`, keyframe capture tout au même instant. | `telemetry_phase3_capture_schedule_tests` | — |
| `P3-REQ-013` | Le snapshot contient exactement la matrice `0x07CB` et ses cardinalités. | `telemetry_phase3_snapshot_matrix_tests` | — |
| `P3-REQ-014` | Le filtrage cockpit précède IDs, catalogue, diff et sérialisation. | `telemetry_phase3_visibility_tests` | — |
| `P3-REQ-015` | Les IDs capteurs sont non nuls, stables, monotones, non réutilisés et bornés à 65 536. | `telemetry_phase3_identity_tests` | — |
| `P3-REQ-016` | Une piste ne matérialise pas implicitement l’état complet d’une entité. | `telemetry_phase3_visibility_tests` | — |
| `P3-REQ-017` | Toute classe révélée par ID est installée avant référence ; un libellé HUD exact reste indépendant du manifeste. | `telemetry_phase3_manifest_tests` | — |
| `P3-REQ-018` | Nouvelle définition référencée : manifeste puis keyframe; nouveau libellé HUD ou changement de piste seul : aucun manifeste. | `telemetry_phase3_manifest_tests` | — |
| `P3-REQ-019` | `TARGET_STATE` v4 reproduit les cibles Target Box avec ou sans blip radar, groupes conditionnels, D/S, libellé et couleur HUD ; v1/v2/v3 restent décodables. | `telemetry_phase3_targeting_tests` | — |
| `P3-REQ-020` | Identité et sous-systèmes ciblés ne sont présents que s’ils sont révélés et résolubles. | `telemetry_phase3_targeting_tests` | — |
| `P3-REQ-021` | Le lead autoritaire est associé à une banque valide, sans données HUD dérivées. | `telemetry_phase3_targeting_tests` | — |
| `P3-REQ-022` | `LOCK_STATE` contient la liste complète de 0 à 64 locks avec absence de tentative explicite. | `telemetry_phase3_lock_tests` | — |
| `P3-REQ-023` | `RADAR_STATE` publie mode, portée, capteurs, AWACS et EMP applicables ; `VISIBLE`/`DISTORTED` reste porté par chaque contact. | `telemetry_phase3_sensor_state_tests` | — |
| `P3-REQ-024` | Les contacts décodés égalent exactement les pistes autorisées du HUD producteur et portent position locale, distance et décision visuelle du même tick en v4. | `telemetry_phase3_visibility_tests` + `TelemetryPhase3RadarProjection.*` | — |
| `P3-REQ-025` | Un vaisseau `VISIBLE` v3/v4 porte le nom affichable et le type exact du Target Box; chaque contact v4 porte la couleur et le type de blip FSO, les autres pistes restant anonymes. | `telemetry_phase3_visibility_tests` + golden v2/v3/v4 | — |
| `P3-REQ-026` | Contacts `CREATE/DELETE` et remplacements complets restent cumulatifs contre la baseline. | `telemetry_phase3_replication_tests` | — |
| `P3-REQ-027` | `THREAT_STATE` reproduit niveau et références autorisées. | `telemetry_phase3_threat_tests` | — |
| `P3-REQ-028` | La liste complète de missiles est exacte jusqu’à 256 et refuse 257 sans troncature. | `telemetry_phase3_threat_tests` | — |
| `P3-REQ-029` | Le scan peut référencer une cible publique hors fermeture sans matérialiser son état complet. | `telemetry_phase3_cargo_tests` | — |
| `P3-REQ-030` | Phase et validités sont exactes; texte cargo présent si et seulement si révélé. | `telemetry_phase3_cargo_tests` | — |
| `P3-REQ-031` | Navigation contient exactement les navpoints, route et destination autorisés. | `telemetry_phase3_navigation_tests` | — |
| `P3-REQ-032` | Autopilote et refus sont cohérents avec `CONTROL_STATE`, sans commande distante. | `telemetry_phase3_navigation_tests` | — |
| `P3-REQ-033` | Les dérivations tactiques restent client sauf D/S, libellés et couleurs autoritaires ; la coordonnée radar live utilise strictement les entrées v2/v3/v4, avec fallback seulement pour capture v1. | `telemetry_phase3_derived_value_tests` | — |
| `P3-REQ-034` | Valeurs non finies, enums, IDs, temps et références invalides échouent fermés. | `telemetry_phase3_security_bounds_tests` | — |
| `P3-REQ-035` | Snapshot, keyframe et resync sont exhaustifs et atomiques après `ACK APPLIED`. | `telemetry_phase3_replication_tests` | — |
| `P3-REQ-036` | Les deltas sont cumulatifs, remplacent des atomes complets et basculent en keyframe au-delà de 1 Mio. | `telemetry_phase3_replication_tests` | — |
| `P3-REQ-037` | Toutes les références croisées emploient le même ID public pour le même objet. | `telemetry_phase3_reference_tests` | — |
| `P3-REQ-038` | Mission, joueur, cible et retrait de piste purgent tout état dépendant devenu ancien. | `telemetry_phase3_lifecycle_tests` | — |
| `P3-REQ-039` | Couvertures événements exactes : state-derived `0x001D`, exact `0`. | `telemetry_phase3_event_coverage_tests` | — |
| `P3-REQ-040` | Toutes les cardinalités et transactions respectent max/max+1 sans troncature. | `telemetry_phase3_security_bounds_tests` | — |
| `P3-REQ-041` | La configuration v3 choisit un profil fermé et les versions v1/v2 restent inchangées. | `TelemetryConfigContract.VersionThreeProfileIsClosed` | — |
| `P3-REQ-042` | Préallocation et budgets 160 Mio partagé, 88 Mio/client, 512 Mio total sont respectés. | `telemetry_phase3_resource_tests` | — |
| `P3-REQ-043` | Métriques et logs exposent les causes sans données sensibles ni labels libres. | `telemetry_phase3_observability_tests` | — |
| `P3-REQ-044` | Désactivé, le module est inerte; activé, le chemin reste borné et non bloquant. | `telemetry_native_runtime_integration_contract_tests` | — |
| `P3-REQ-045` | Le client indépendant reconstruit les valeurs brutes, sample times et provenance sans redéfinir les décisions sensibles. | `telemetry_phase3_loopback_tests` | — |
| `P3-REQ-046` | Après perte d’un delta Phase 3, le client converge sans fuite ni référence pendante. | `telemetry_phase3_replication_tests` + loopback `--drop-once delta` | — |

## 3. Observations neutres et facultatives

Les scénarios cockpit réels nécessitent une campagne longue, plusieurs sessions
et des événements qui ne peuvent pas être reproduits de manière courte et
déterministe. Ils ne constituent donc pas une exigence de la phase ni une
condition de complétude. Ils peuvent être consignés ultérieurement comme retours
produit, sans modifier le statut des exigences.

## 4. Décision de livraison

Les tests automatisés établissent les résultats exacts et les bornes. Les
observations de campagne, lorsqu’elles sont disponibles, restent informatives.
Aucun score, verdict automatique, percentile, soak ou campagne de certification
ne décide de la livraison.
