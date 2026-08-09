# 07 — Livraison et traçabilité produit

## 1. Contenu livré

La Phase 2 livre deux comportements normatifs du même pipeline : `CoreGate`, domaine racine minimal sous le masque `0x0401`, et `CompleteShip`, domaine complet sous le masque `0x0583`. Les exigences Phase 0 et Phase 1 sont liées comme dépendances et ne sont pas recopiées.

## 2. Table canonique des exigences

| REQ | Invariant ou observable produit | Test automatisé | Observation manuelle éventuelle |
|---|---|---|---|
| `P2-REQ-001` | Les garanties produit Phase 1 restent actives. | suite `telemetry-short` | — |
| `P2-REQ-002` | FSTL 1.0 et les layouts 1.1 existants restent byte-identiques. | `test_fstl_1_0_freeze.py` + vérificateur 1.1 | — |
| `P2-REQ-003` | Une session Phase 2 négocie exactement FSTL 1.1. | `telemetry_phase2_session_transition_tests` | — |
| `P2-REQ-004` | Les masques exacts sont `CoreGate=0x0401` et `CompleteShip=0x0583`. | `TelemetryPhase2ProfileGate.*FrozenCoverage` | `P2-OBS-01`, `P2-OBS-02` |
| `P2-REQ-005` | Le profil est choisi une fois à l'initialisation et reste immuable jusqu'au redémarrage. | `TelemetryConfigContract.VersionTwoRequiresOneClosedPhase2Profile` | `P2-OBS-01` |
| `P2-REQ-006` | Le mode livré reste `SOLO + COCKPIT`, non trusted et non headless. | `TelemetryPhase2ProfileGate.*Eligibility*` | — |
| `P2-REQ-007` | Le producteur est strictement read-only. | `telemetry_phase2_runtime_tests` | — |
| `P2-REQ-008` | Cible, radar, navigation, communication et vidéo restent hors Phase 2. | `telemetry_phase2_business_state_tests` | `P2-OBS-04` |
| `P2-REQ-009` | L'état publié est capturé sur le thread principal dans des DTO possédés ; le produit n'expose aucun seam d'injection de manifeste. | `telemetry_phase2_observation_contract_tests` + contrôle statique | — |
| `P2-REQ-010` | Chaque pointeur, index, signature et type moteur est validé avant lecture. | `telemetry_phase2_observation_contract_tests` | — |
| `P2-REQ-011` | Le DTO ne conserve ni pointeur, ni vue, ni index moteur différé. | `DtoOwnsCopiedStringsAndRejectsDeferredSourceAliasing` | — |
| `P2-REQ-012` | Aucun worker/SPSC ni lecture concurrente des tables moteur n'est introduit. | contrôle des groupes de sources | — |
| `P2-REQ-013` | Le chemin actif est borné, préalloué et non bloquant. | `telemetry_phase2_security_bounds_tests` | `P2-OBS-05` |
| `P2-REQ-014` | Keyframe : capture complète cohérente ; tick ordinaire : seuls les blocs dus sont reconstruits. | `telemetry_phase2_observation_contract_tests` | — |
| `P2-REQ-015` | Chaque manifeste contient exactement les classes et armes de sa projection. | `telemetry_phase2_manifest_tests` | `P2-OBS-01`, `P2-OBS-02` |
| `P2-REQ-016` | Aucun snapshot ne référence un manifeste non appliqué au client. | `telemetry_phase2_runtime_tests` | — |
| `P2-REQ-017` | Les IDs statiques sont non nuls, stables par génération et indépendants du moteur. | `telemetry_phase2_manifest_tests` | — |
| `P2-REQ-018` | Fingerprint catalogue : nouveau manifeste puis keyframe ; topologie seule : keyframe. | `telemetry_phase2_manifest_tests` | — |
| `P2-REQ-019` | `CoreGate={joueur}` ; `CompleteShip` ajoute seulement support et docking transitif. | `telemetry_phase2_closure_tests` | `P2-OBS-01`, `P2-OBS-02` |
| `P2-REQ-020` | Toute référence d'entité autorisée résout un lifecycle ; une cible cargo externe est omise, pas sérialisée comme ID opaque. | `telemetry_phase2_closure_tests` | `P2-OBS-04` |
| `P2-REQ-021` | Le snapshot contient exactement un `SHIP_IDENTITY` par vaisseau exporté et la classe installée. | `telemetry_phase2_core_state_tests` | `P2-OBS-02` |
| `P2-REQ-022` | Coque et protections sont bornées ; seule une quantité courante finie peut être clampée. | `FiniteCurrentQuantitiesClampAndAreCountedSeparately` | — |
| `P2-REQ-023` | Les 0 à 64 segments de bouclier sont complets, sans hypothèse de quatre quadrants. | `ShieldCardinalityAndPhysicalMaximumBoundariesAreClosed` | — |
| `P2-REQ-024` | Chaque sous-système déclaré possède exactement un état ; l'ensemble change par manifeste/keyframe. | `telemetry_phase2_core_state_tests` | — |
| `P2-REQ-025` | Les tourelles restent limitées aux champs Phase 2 et ne divulguent aucune cible/lock. | `telemetry_phase2_control_turret_tests` | — |
| `P2-REQ-026` | ETS reste dans `0..12` et les groupes incompatibles sont omis. | `EnergyEtsAndFiniteBoundariesAreClosed` | — |
| `P2-REQ-027` | Propulsion et flight sont cohérents à sample time égal ; les cadences indépendantes conservent leur âge. | `telemetry_phase2_core_state_tests` | — |
| `P2-REQ-028` | `CONTROL_STATE` reproduit les six axes, flags et groupes conditionnels exacts. | `telemetry_phase2_control_turret_tests` | — |
| `P2-REQ-029` | Chaque vaisseau exporté possède un état d'armes complet et hétérogène. | `telemetry_phase2_weapon_state_tests` | `P2-OBS-02` |
| `P2-REQ-030` | Le support provient de l'état gameplay et ses transitions terminales restent bornées. | `telemetry_phase2_lifecycle_support_tests` | `P2-OBS-02` |
| `P2-REQ-031` | Le cargo est lu depuis le gameplay ; cible externe : `NOT_SCANNABLE/HIDDEN`, aucun groupe optionnel et aucune fin de session. | `CargoTargetOutsidePhase2ClosureIsHiddenWithoutCaptureFailure` | `P2-OBS-04` |
| `P2-REQ-032` | Les valeurs dérivées d'affichage restent calculées côté client. | `telemetry_phase2_business_state_tests` | — |
| `P2-REQ-033` | Les maxima invalides/non finis et structures impossibles sont rejetés ; les normalisations permises sont comptées séparément. | `telemetry_phase2_observation_contract_tests` | `P2-OBS-05` |
| `P2-REQ-034` | Apparition et respawn allouent des IDs monotones distincts. | `telemetry_phase2_lifecycle_support_tests` | — |
| `P2-REQ-035` | Lifecycle et événements respectent leurs fences jusqu'à la baseline connaissable. | `telemetry_phase2_lifecycle_support_tests` | — |
| `P2-REQ-036` | Snapshot initial et keyframes sont exhaustifs et atomiques après `ACK APPLIED`. | `telemetry_phase2_replication_tests` | — |
| `P2-REQ-037` | Chaque delta est cumulatif contre la baseline et se remplace par une keyframe s'il dépasse 1 Mio. | `telemetry_phase2_replication_tests` | — |
| `P2-REQ-038` | L'unité de remplacement est l'atome FSTL complet. | `telemetry_phase2_replication_tests` | — |
| `P2-REQ-039` | `DELETE` n'est utilisé que par les records qui l'autorisent. | `telemetry_phase2_replication_tests` | — |
| `P2-REQ-040` | Une perte unique converge sur le delta cumulatif ou la keyframe suivante, sans campagne probabiliste. | `telemetry_phase2_replication_tests` | `P2-OBS-05` |
| `P2-REQ-041` | Configuration : `systemsHz=1..20`; v2 exige `phase2Profile`; v1 migre explicitement vers `CompleteShip`. | `TelemetryConfigContract.*` | `P2-OBS-01`, `P2-OBS-02` |
| `P2-REQ-042` | Les limites sont appliquées avant cast/allocation ; maxima manifestes : 1033 et 4740, sans image artificielle de 65 535 entrées. | `telemetry_phase2_security_bounds_tests` | `P2-OBS-05` |
| `P2-REQ-043` | Les métriques distinguent collecte, manifeste, image, diff, baseline, rejets et normalisations. | `telemetry_phase2_runtime_tests` | — |
| `P2-REQ-044` | Les logs identifient profil, manifeste et cause sans donnée cachée. | `telemetry_phase2_runtime_tests` | — |
| `P2-REQ-045` | Le produit désactivé est inerte ; activé, son travail reste déterministe et borné sans seuil temporel de certification. | `telemetry_native_runtime_integration_contract_tests` | `P2-OBS-01`, `P2-OBS-02` |
| `P2-REQ-046` | Les plafonds restent 134 217 728 octets partagés, 83 886 080 octets/client et 402 653 184 octets process ; aucune réserve steady-state non bornée n'est ajoutée. | `telemetry_phase2_security_bounds_tests` | `P2-OBS-05` |
| `P2-REQ-047` | La chaîne moteur → projection → manifeste est relue par le décodeur indépendant. | `telemetry_phase2_manifest_codec_tests` | `P2-OBS-02` |
| `P2-REQ-048` | Octets, IDs, cardinalités et transitions exactes sont disponibles au relevé produit. | suite `telemetry-short` | `P2-OBS-01`, `P2-OBS-02`, `P2-OBS-04`, `P2-OBS-05` |
| `P2-REQ-049` | Les sources produit sont déclarées explicitement, sans glob. | contrôle CMake statique | — |
| `P2-REQ-050` | Tout échec est observable et fail-closed ; aucune troncature ou couverture dégradée silencieuse. | `telemetry_phase2_security_bounds_tests` | `P2-OBS-05` |

## 3. Observations neutres

| Scénario | Produit réellement observé | Attendu |
|---|---|---|
| `P2-OBS-01` | Runtime Release `CoreGate`, environ 60 s. | Configuration v2 explicite, masque `0x0401`, manifeste avant snapshot, uniquement le domaine racine, snapshot puis delta, arrêt propre. |
| `P2-OBS-02` | Runtime Release `CompleteShip`, environ 90 à 120 s. | Masque `0x0583`, catalogue complet décodé indépendamment, support/docking et arrêt/redémarrage propres. |
| `P2-OBS-04` | Fermeture player/support/docking avec cible cargo extérieure. | La cible extérieure est omise et publiée `NOT_SCANNABLE/HIDDEN`, sans extension de fermeture ni fin de session. |
| `P2-OBS-05` | Bornes min/max/max+1, NaN, infini, cast impossible et perte unique d'un delta. | Rejet avant cast/allocation, session récupérable, convergence sur delta cumulatif ou keyframe. |

La rétention de N, le staging de N+1, la coalescence de N+2, l'indépendance
des slots clients et les fences `ACK APPLIED` sont des états internes du
producteur. Ils sont vérifiés directement par les tests comportementaux
`telemetry_phase2_runtime_tests`, `telemetry_phase2_manifest_tests` et
`telemetry_phase2_replication_tests` ; ils ne constituent pas une observation
humaine neutre.

## 4. Décision de livraison

Les deux exécutions Release ont une durée combinée inférieure à cinq minutes. Le client peut annoncer l'injection neutre `--drop-once delta` dans son transcript, sans score ni éligibilité. La fixture, les deux configurations v2 et la checklist reproductible sont définies dans [`test/telemetry/producer/observations`](../../../../test/telemetry/producer/observations/README.md). La checklist humaine consigne `attendu`, `observé`, `écart` et `impact`, puis décide de la livraison.
