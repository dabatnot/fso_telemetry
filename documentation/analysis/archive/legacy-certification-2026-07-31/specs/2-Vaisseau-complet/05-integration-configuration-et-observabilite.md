# 05 — Intégration, configuration et observabilité

## 1. Objet

Ce document fixe l’intégration build, l’évolution du schéma JSON, les quotas, les métriques, les logs et les budgets Phase 2. Tout comportement Phase 1 non explicitement modifié reste normatif.

## 2. Points d’intégration

### 2.1 Production

Les nouveaux fichiers proposés sont ajoutés au groupe `Telemetry files` de `code/source_groups.cmake`. Le runtime conserve les callbacks actuels dans `freespace2/freespace.cpp` et `code/mission/missionload.cpp`; aucun second point d’entrée global n’est créé.

L’intégration minimale touche :

- `code/telemetry/engine_adapter.h/.cpp` ;
- `code/telemetry/session_controller.h/.cpp` ;
- `code/telemetry/native_session_runtime.cpp` ;
- `code/telemetry/runtime.cpp` si la purge mission requiert le manifeste ;
- de nouveaux composants `phase2_closure`, `phase2_manifest_builder`, `phase2_profile_gate` et `phase2_state_image` ;
- `code/ship/ship.cpp` pour `OnShipCleanup` avant toute purge ;
- `code/ai/aicode.cpp` pour `OnSupportTransition` avant mutation de chaque branche `REPAIR_INFO_*` ;
- `code/playerman/playercontrol.cpp` et `code/hud/hudtargetbox.cpp` pour extraire l’autorité cargo sans changer son point/gating historique ;
- `code/playerman/playercontrol.cpp` pour le latch scalaire ship/caméra dans `read_keyboard_controls()` ;
- `code/source_groups.cmake` et `test/src/CMakeLists.txt`.

Les codecs FSTL existants peuvent recevoir des tests et corrections de conformité, mais aucun registre ou layout ne change. Chacun des quatre seams moteur est `noexcept`, main-thread, sans allocation, injectable en test, initialisé/purgé avec la mission et inerte vis-à-vis du réseau. Les tests off/on prouvent même contrôles, même nombre d’avances cargo, même révélation/HUD et aucun effet supplémentaire lorsque le runtime est désactivé.

### 2.2 Build et plateformes

Les fichiers `.cpp` et `.h` sont listés explicitement. Les builds suivants sont obligatoires :

| Plateforme/configuration | Obligation |
|---|---|
| Windows MSVC Debug | compilation et tests fonctionnels |
| Windows MSVC Release | tests fonctionnels, performance et endurance |
| au moins une plateforme non-Windows CI | compilation et tests portables |
| télémétrie compilée, config absente | fast path désactivé Phase 1 inchangé |
| télémétrie activée, un puis quatre clients loopback | ressources et scheduling |

La Phase 2 n’ajoute aucune dépendance tierce et ne change aucune option de build publique. Jansson, crypto/hash, réseau et tests réutilisent les dépendances déjà approuvées.

### 2.3 Cibles de tests proposées

| Cible | Contenu |
|---|---|
| `telemetry_phase2_unit_tests` | collecteurs, closure, mapping et formules |
| `telemetry_phase2_protocol_tests` | manifestes, images, matrices, goldens et compatibilité |
| `telemetry_phase2_integration_tests` | runtime, mission, lifecycle, support et sessions |
| `telemetry_phase2_loss_harness` | perte, duplication, réordre et convergence |
| `telemetry_phase2_oracle_harness` | comparaison DTO moteur / décodeur indépendant |
| `telemetry_phase2_performance_harness` | allocations, timings et endurance |
| `telemetry_phase2_fuzz_tests` | builds libFuzzer/AFL++ des readers datagramme/transaction et validateurs manifeste/état |

Une cible peut regrouper plusieurs exécutables si CMake le requiert, mais les sept catégories et leurs résultats restent séparables dans les preuves. `telemetry_phase2_fuzz_tests` produit au minimum `telemetry_phase2_packet_reader_fuzz` et `telemetry_phase2_state_validator_fuzz`.

## 3. Schéma JSON fermé

### 3.1 Fichier `telemetry.json`

Le chemin, la découverte CFile, la limite de 16 Kio, `JSON_REJECT_DUPLICATES`, la profondeur maximale et l’ordre de chargement Phase 1 sont inchangés.

Exemple complet Phase 2 :

```json
{
  "schemaVersion": 1,
  "enabled": false,
  "bindAddresses": ["127.0.0.1", "::1"],
  "bindPort": 42042,
  "allowedClients": ["127.0.0.1/32", "::1/128"],
  "discoveryEnabled": false,
  "visibilityMode": "Cockpit",
  "maxClients": 1,
  "flightHz": 30,
  "systemsHz": 10,
  "keyframeSeconds": 2,
  "missionHeartbeatMs": 500,
  "idleHeartbeatMs": 1000,
  "maxDatagramsPerTick": 64
}
```

L’unique nouvelle clé est :

| Clé | Type | Défaut | Domaine | Effet |
|---|---|---:|---:|---|
| `systemsHz` | entier JSON | `10` | `1..20` | cadence dégâts, boucliers, énergie, propulsion, armes, sous-systèmes, support, cargo et docking |

Les clés héritées conservent exactement leurs défauts et bornes :

| Clé | Défaut | Domaine Phase 2 |
|---|---:|---|
| `schemaVersion` | obligatoire si fichier | exactement `1` |
| `enabled` | `false` | booléen |
| `bindAddresses` | loopback v4/v6 | 1–2 adresses numériques uniques |
| `bindPort` | `42042` | 1024–65535 |
| `allowedClients` | loopback v4/v6 | 1–32 CIDR canoniques |
| `discoveryEnabled` | `false` | exactement `false` |
| `visibilityMode` | `Cockpit` | exactement `Cockpit` |
| `maxClients` | `1` | 1–4 |
| `flightHz` | `30` | 1–60 |
| `keyframeSeconds` | `2` | 1–5 |
| `missionHeartbeatMs` | `500` | 200–5000 |
| `idleHeartbeatMs` | `1000` | 200–5000 |
| `maxDatagramsPerTick` | `64` | 1–256 |

Une clé inconnue, dupliquée, mal typée ou hors borne invalide l’objet entier. L’absence de `systemsHz` dans une configuration Phase 1 existante produit le défaut 10 et ne modifie aucun autre champ. Le schéma reste `schemaVersion=1` car l’ajout est optionnel et rétrocompatible pour le parseur producteur ; le parseur fermé est mis à jour pour reconnaître explicitement la clé.

### 3.2 Relations croisées

- `systemsHz` et `flightHz` sont indépendants ; un tick systèmes capture sa propre cinématique cohérente si nécessaire et ne modifie pas la cadence de publication flight/control.
- `keyframeSeconds × systemsHz >= 1` est naturellement garanti par les bornes et n’ajoute aucune restriction.
- `visibilityMode=Cockpit` et `discoveryEnabled=false` restent obligatoires.
- Une écoute non-loopback exige l’activation explicite et l’allowlist Phase 1 ; elle ne change pas le profil de visibilité.

Une violation ferme le runtime avant identité, allocation ou socket.

### 3.3 Profil producteur

`telemetry-profile.json`, `producerId`, l’écriture atomique, les sources d’entropie et la limite `MaxSessionIdsPerProcess=65 536` sont inchangés. Aucune version, classe, arme ou fingerprint de manifeste n’est persistée dans ce profil.

## 4. Provisionnement et budgets

### 4.1 Bornes wire héritées

| Budget | Borne normative |
|---|---:|
| datagramme / en-tête / payload | 1200 / 68 / 1132 octets |
| message d’état | 1 048 576 octets |
| `record_length` d’un record individuel | 65 535 octets ; un record n’est jamais découpé entre parts |
| fragments par message | 1024 |
| transaction manifeste/snapshot | 16 777 216 octets |
| parts par transaction | 64 |
| `record_count` | 65 535 par message/part |
| candidates transactionnelles | 2 par client |
| mémoire candidates | 33 554 432 octets par client |
| réassemblages actifs | 4 par client |
| mémoire réassemblage | 4 Mio par client |
| clients | 1–4 |
| segments de bouclier | 0–64 |
| banques d’état | 0–64 par famille primaire/secondaire |
| `CLASS_MANIFEST.bank_definitions` | 0–192 agrégées par variante, tourelles incluses |
| sous-systèmes | 0–1024 par classe/vaisseau |
| relations de docking | 0–64 par record |

Ces limites sont testées avant toute allocation proportionnelle. Une valeur annoncée au-delà rejette le bloc ou la session ; elle n’est jamais tronquée.

### 4.2 Limites d’implémentation Phase 2

Le producteur Phase 2 fixe en plus :

| Ressource | Limite |
|---|---:|
| vaisseaux dans la closure service/docking | 64 |
| classes de vaisseau dans la fermeture Cockpit | 64 |
| classes d’armes dans la fermeture | 4096 |
| sous-systèmes agrégés dans la closure | 4096, sans dépasser 1024 par vaisseau |
| records dans une image Phase 2 complète | 65 535, limite d’implémentation distincte du count par part |
| banques cataloguées par variante | 192 au total, chacune dans sa famille à 0–64 |
| événements lifecycle fiables en attente | fenêtre fiable héritée, sans nouvelle croissance |
| images complètes | une image courante projetée et une baseline par client ; la candidate sérialisée vit dans la rétention fiable ; seul le DTO moteur pré-ID est partagé |
| deltas cumulatifs | un par baseline/client |

La limite de 64 classes est supérieure au besoin nominal d’un joueur mais borne les closures de mod et les références transitives. Elle n’élargit pas le contenu autorisé. Toute multiplication `maxClients × slots × taille` utilise une arithmétique vérifiée `size_t` avant bind. `record_length=65 536`, 193 banques de classe ou 65 536 records d’image donnent `SourceRecordTooLarge`/`SourceLimitExceeded` avant mise en file, jamais une pagination interne au record.

### 4.3 Plafond mémoire process

La Phase 2 remplace le plafond provisoire Phase 1 de 256 Mio par le plafond compilé et testé `Phase2KnownBudgetCapBytes = 402 653 184` octets (384 Mio), inclusif de toutes les allocations Phase 1 conservées. Les sous-budgets sont :

| Scope inclusif | Plafond | Contenu maximal |
|---|---:|---|
| partagé process/mission | 67 108 864 octets | DTO/closure/scratch pré-ID préalloués, descripteurs de catalogue partagés, rings, registres sources et métriques |
| par client | 83 886 080 octets | rétention fiable 33 554 432, réassemblage 4 194 304, image courante et baseline jusqu’à 16 777 216 chacune, delta jusqu’à 1 048 576, egress/latches/manifest IDs/métadonnées dans le solde |
| process total | `shared + maxClients × perClient <= 402 653 184` | avec `maxClients<=4`, arithmétique vérifiée avant bind |

Ces plafonds sont simultanés, non des estimations moyennes. Chaque objet préalloué publie capacité et octets possédés ; leur somme doit égaler le budget calculé. Un dépassement d’un sous-budget ou du total donne `StartupBudgetExceeded`, zéro bind et zéro état `Ready`. Le test borne accepte exactement chaque plafond puis rejette `+1`.

### 4.4 Préallocation

Après chargement d’une configuration valide avec `enabled=true`, avant tout bind et avant `Ready` Phase 2 :

1. le collecteur provisionne les maxima absolus Phase 2 : 64 ships, 64 classes, 4096 armes, 4096 sous-systèmes, 192 banques par variante et 65 535 atomes ;
2. les builders provisionnent aux maxima les records, clés, valeurs et scratch de sérialisation ;
3. le process provisionne les descripteurs pré-ID partagés ; chaque slot client provisionne ses deux slots sémantiques `active/staged`, image courante, baseline, rétention fiable, delta et latches, car IDs, `manifest_id`, ACK et late join sont session-scoped ;
4. les capacités et leurs octets possédés sont enregistrés comme high-water initial, puis le budget 384 Mio est validé ; un échec conserve zéro socket ;
5. toute croissance ultérieure est comptée et bloque la gate de performance.

À l’entrée mission, aucune réservation ni croissance n’est autorisée : le runtime remet à zéro les buffers, construit les vues, clés et registres dans les capacités déjà possédées, puis seulement entre dans l’état mission `Ready`. Une configuration absente ou `enabled=false` ne suit pas ce chemin de provisionnement et conserve zéro allocation Phase 2.

Une croissance topologique ou un manifeste N+1 réutilise ces capacités : elle NE DOIT ni drainer une session valide ni réallouer. Une troisième mutation catalogue reste un rebuild-intent coalescé jusqu’à promotion de N+1. Si une source dépasse une capacité maximale, la session reçoit `SESSION_END(Restart, RECONNECT_ALLOWED)` avec raison privée `SourceLimitExceeded`; aucun `realloc` n’a lieu après `Ready`, même dans `ManifestPreparing`.

## 5. Transport et sécurité d’intégration

Bind, dual-stack, endpoint canonique, socket non bloquant, anti-amplification, allowlist et ordre de validation restent identiques à Phase 1. Le manifeste plus volumineux utilise la fragmentation et la transaction fiable Phase 0 ; il NE DOIT entraîner ni fragmentation IP supérieure à 1200 octets, ni taille socket non bornée.

Les catalogues et états peuvent contenir noms de classes, loadout et état de survie. Ils sont donc filtrés `Cockpit` avant sérialisation. Les logs n’en reproduisent aucun contenu libre.

## 6. Surface d’observabilité

### 6.1 Métriques héritées

Toutes les métriques Phase 1 restent présentes avec leurs types, scopes, unités, buckets et enums. En particulier : callbacks, sockets, datagrammes, validation, sessions, heartbeats, fragments, réassemblages, ACK/NACK, retransmissions, resync, captures, diff, sérialisation, tick, lifecycle joueur, snapshots, baselines, deltas, pending items et budgets.

Les scopes restent `process`, `mission` et `session`. Aucun label ne peut contenir IP, port, session ID, entity ID, nom de classe, arme, sous-système ou texte pair.

### 6.2 Métriques Phase 2 obligatoires

| Nom | Type | Scope | Sémantique exacte |
|---|---|---|---|
| `telemetry_phase2_profile` | gauge enum | session | `None`, `CoreGate`, `CompleteShip` |
| `telemetry_phase2_profile_rejections_total{reason}` | counter | process | refus par enum fermé de matrice/source/budget |
| `telemetry_phase2_capture_duration_us{block}` | histogram | mission + process | durée par bloc fixe |
| `telemetry_phase2_capture_failures_total{block,reason}` | counter | mission + process | un échec primaire par bloc |
| `telemetry_phase2_closure_builds_total{result}` | counter | mission + process | fermeture créée, inchangée ou refusée |
| `telemetry_phase2_closure_classes` | gauge | mission | classes de vaisseau autorisées |
| `telemetry_phase2_closure_ships` | gauge | mission | vaisseaux exportés, 0–64 |
| `telemetry_phase2_closure_weapons` | gauge | mission | classes d’armes autorisées |
| `telemetry_phase2_closure_subsystems` | gauge | mission | définitions sous-systèmes |
| `telemetry_phase2_manifest_builds_total{result}` | counter | session + process | candidate manifeste complète |
| `telemetry_phase2_manifest_bytes` | gauge | session | taille transaction courante |
| `telemetry_phase2_manifest_parts` | gauge | session | nombre de parts courant |
| `telemetry_phase2_manifest_duration_us` | histogram | session + process | closure + sérialisation manifeste |
| `telemetry_phase2_image_records` | gauge | session | nombre d’atomes courants |
| `telemetry_phase2_image_bytes` | gauge | session | somme clés + valeurs courantes |
| `telemetry_phase2_image_duration_us` | histogram | mission + process | construction/validation image |
| `telemetry_phase2_dirty_atoms` | gauge | session | mutations du dernier delta cumulatif |
| `telemetry_phase2_lifecycle_events_total{kind}` | counter | mission + process | événements reconstructibles émis |
| `telemetry_phase2_support_transitions_total{kind}` | counter | mission + process | terminal success, aborted, obstructed |
| `telemetry_phase2_support_coalesced_total{kind}` | counter | mission + process | `CompleteThenEndSameEpisode`, `SupersededByNewEpisode`, `NewerGeneration` |
| `telemetry_phase2_support_ring_depth` / `_high_water` | gauge | mission | profondeur courante/max du ring global, 0–64 |
| `telemetry_phase2_cleanup_ring_depth` / `_high_water` | gauge | mission | profondeur courante/max du ring cleanup, 0–64 |
| `telemetry_phase2_ring_overflows_total{ring}` | counter | process | overflow `Support` ou `Cleanup`, jamais écrasement silencieux |
| `telemetry_phase2_support_latches` / `_high_water` | gauge | session | latches courants/max du slot, 0–64 |
| `telemetry_phase2_forced_keyframes_total{reason}` | counter | session + process | lifecycle, support, manifeste, resync |
| `telemetry_phase2_sample_age_us{block}` | gauge | session | âge du dernier bloc publié |
| `telemetry_phase2_allocations_after_ready_total{kind}` | counter | process | toute croissance après warm-up |
| `telemetry_phase2_source_limit_total{kind}` | counter | mission + process | cardinalité/taille source hors borne |
| `telemetry_phase2_memory_bytes{scope}` / `_high_water` | gauge | process | octets possédés `Shared`, `ClientTotal`, `ProcessTotal` |
| `telemetry_phase2_manifest_generations` | gauge | session | générations sémantiques résidentes, 0–2 |
| `telemetry_phase2_manifest_rebuild_coalesced_total` | counter | session + process | troisième mutation ramenée au rebuild-intent |

Les enums de labels sont compilées, fermées et terminées par `Count` :

- `block={Identity,Flight,Control,DamageShield,EnergyPropulsion,Weapons,Subsystems,SupportCargoDocking}` ;
- `ProfileRejectionReason={UnsupportedAuthority,UnsupportedVisibility,IncompleteCoverage,InvalidSource,SourceLimit,RecordTooLarge,TransactionTooLarge,BudgetExceeded,ManifestUnavailable}` ;
- `CaptureFailureReason={Guard,NonFinite,OutOfRange,InvalidReference,IncoherentTopology,InvalidEnum,StaleAuthority}` ;
- `ClosureResult={Created,Unchanged,TopologyChanged,CatalogChanged,Rejected}` et `ManifestResult={Built,Reused,Rejected}` ;
- lifecycle `kind={Appeared,Disabled,DyingStarted,Destroyed,Disappeared}` ; support `kind={Requested,Approaching,Docking,Repairing,Rearming,Obstructed,Aborted,CompletedPrivate,EndedPrivate}` ;
- keyframe `reason={Periodic,Topology,Catalog,Lifecycle,Support,Resync}`, allocation `kind={VectorGrowth,StringGrowth,HeapFallback,LazyIndex,Other}`, limite `kind={Ships,Classes,Weapons,Subsystems,ClassBanks,ImageRecords,RecordBytes,TransactionBytes,Parts,Memory}`.

Un ordinal inconnu est impossible à exposer comme label ; les tests vérifient nombre, orthographe et reset de chaque enum.

Les métriques d’oracle/mismatch appartiennent aux exécutables de preuve et ne sont pas compilées dans le runtime de livraison.

### 6.3 Cohérence des compteurs

- Une capture Phase 2 incrémente exactement un résultat global et un résultat pour chaque bloc tenté.
- Une candidate manifeste refusée n’incrémente pas les compteurs `snapshots_created`.
- Une keyframe forcée incrémente une seule raison primaire.
- `manifest_bytes`, `image_records`, `image_bytes` et `dirty_atoms` reviennent à zéro à la purge session.
- `sample_age_us` utilise l’horloge monotone producteur et ne devient jamais négatif.
- `ring_depth<=ring_high_water<=64`, `support_latches<=support_latches_high_water<=64` et un overflow incrémente exactement une fois avant fermeture.
- `manifest_generations<=2` et chaque troisième mutation incrémente le compteur coalescé sans croissance mémoire.
- `memory_bytes<=memory_high_water<=Phase2KnownBudgetCapBytes`; la valeur process égale partagé + somme des clients, pas une estimation RSS.

## 7. Logs normatifs

Les règles Phase 1 de fréquence et de contenu restent valides. La Phase 2 ajoute les événements agrégés suivants :

| Événement | Niveau | Fréquence maximale | Contenu permis |
|---|---|---:|---|
| profil sélectionné/refusé | info/warning | une fois par session | `CoreGate`/`CompleteShip`, mask hex, enum raison |
| manifeste construit/installé | info | une fois par génération | ID local, nombre de records/parts, octets, durée |
| manifeste refusé | warning | une fois puis résumé | enum raison et limite, sans nom de classe/arme |
| transition lifecycle | info/debug | une fois par transition | kind fermé et ordinal de slot, aucun callsign |
| transition support terminale | debug/info | une fois par transition | kind fermé, keyframe forcée |
| source incohérente/hors borne | warning agrégé | au plus une fois/s | bloc, enum raison, compteur |
| bilan Phase 2 | info | fin session/shutdown | profils, high-water, tailles, durées, drops et resync agrégés |

Sont interdits : payloads, fragments, catalogues, noms/callsigns, texte cargo, chemins absolus, IP complète, IDs aléatoires, pointeurs, handles et dump par tick. Les tests scannent les logs.

## 8. Performance et allocations

### 8.1 Fast path désactivé

Les seuils Phase 1 restent : sur 100 000 callbacks, moyenne inférieure ou égale à 0,01 ms, p99 inférieur ou égal à 0,05 ms et écart de frame médiane inférieur à 1 % face au témoin. `systemsHz` n’est pas lu sur le fast path désactivé après chargement.

### 8.2 Runtime actif Release

Sur la machine de référence documentée, un joueur, un client loopback, après warm-up :

| Scénario | Seuil |
|---|---:|
| tick flight/control sans systèmes | p99 télémétrie total `<= 0,25 ms` |
| tick systèmes nominal | p99 télémétrie ajouté `<= 0,75 ms` |
| keyframe complète, hors attente réseau | p99 construction `<= 2,0 ms`, max `<= 5,0 ms` |
| quatre clients réutilisant le même DTO pré-ID et la même capture moteur, avec image/baseline propres à chaque slot | p99 tick systèmes `<= 1,50 ms` |
| frame médiane actif vs témoin mission | régression `< 2 %` |
| steady-state de 30 minutes | zéro allocation/croissance après `Ready` |

Le scénario nominal comporte deux vaisseaux exportés (joueur + support), au moins 4 segments de bouclier, 32 sous-systèmes agrégés, 4 banques joueur, 4 banques tourelles et un support actif. Un scénario de borne séparé mesure 64 vaisseaux, 4096 sous-systèmes agrégés, 64 segments par ship, 64 banques par famille d’état et exactement 192 définitions de banque de classe ; il doit rester sans allocation ni blocage et sous les bornes transactionnelles. Les fixtures `193e banque`, `record_length=65 536`, `65 536e atome` et chaque cap mémoire `+1` sont refusées avant egress. Les timings du scénario borne sont rapportés séparément du seuil nominal.

Les seuils ne sont évalués qu’en Release. Debug doit exécuter les mêmes chemins fonctionnels.

### 8.3 Protocole de mesure

La preuve coordonnée utilise une trace canonique unique de 600 s, seed 4242, découpée en dix époques contiguës de 60 s. Avant toute mesure, quatre runtimes indépendants sont provisionnés jusqu’à `Ready`, puis restent persistants sans reset, restart ni reprovisionnement : `control` sans module, `nominal` actif avec un client, `four-clients-one-slow`, et `bounds-loss-resync-manifest` qui porte les injections déterministes bornes, `WOULD_BLOCK`, perte/resync et changement de manifeste. Chaque runtime reçoit exactement 60 s de warm-up hors séries de timings ; toute allocation ou croissance après `Ready`, y compris pendant ce warm-up, reste néanmoins bloquante et figure dans les high-water.

À l’époque d’indice `e` dans `[0,9]`, les quatre tranches de 60 s sont exécutées en série, une seule à la fois, selon la rotation gauche de `e mod 4` de la liste de base `[control, nominal, four-clients-one-slow, bounds-loss-resync-manifest]`. Chaque runtime rejoue ainsi exactement une fois les 600 s de la même trace et conserve son état entre ses dix tranches. La fenêtre mesurée est donc exactement de 2 400 s physiques : 600 s de contrôle et trois séries actives de 600 s, soit 1 800 s actives agrégées. Les quatre warm-ups ajoutent 240 s hors mesure ; démarrage, orchestration et validation de rapport disposent d’au plus 60 s supplémentaires. La campagne n’allonge aucune fenêtre : elle est invalide, pas PASS, si elle n’a pas réuni simultanément 100 000 frames/callbacks, 54 000 ticks flight/control au défaut 30 Hz, 18 000 ticks systèmes au défaut 10 Hz et 900 keyframes au défaut 2 s sur l’union active.

Le chrono est `std::chrono::steady_clock`; sa résolution mesurée est archivée. Aucun échantillon n’est retiré, winsorisé, normalisé ou remplacé. Les durées télémétrie sont exclusives : collecte, manifeste, image, diff et sérialisation ne sont jamais additionnés à un total qui les contient déjà. L'événement keyframe est attribué à toute la fenêtre de service du sample, y compris drainage préalable des ACK et `service_periodic`, afin de compter la keyframe réellement commencée pendant cette frame.

Les durées de frame contrôle/actif proviennent de la boucle native d'une même mission FS2Open automatisée, avec rendu, simulation et cadence identiques ; un workload synthétique de quelques microsecondes n'est pas une référence admissible pour le seuil `<2 %`. Le contrôle utilise le module absent ou désactivé, l'actif le module Phase 2, et chaque échantillon couvre la même frontière de frame moteur. Pour N valeurs triées croissantes, p99 est le nearest-rank `x[ceil(0,99×N)-1]`; médiane est la moyenne des deux centres pour N pair et le centre pour N impair. `median_control` est calculée sur toute la série brute `control`. `median_active` est calculée après concaténation multiensemble des trois séries brutes actives, avec un poids unitaire par échantillon ; une médiane de médianes ou une pondération par scénario est interdite. La régression frame vaut `100×(median_active/median_control-1)` et les médianes de chaque scénario actif sont également rapportées.

Le rapport archive : révision, compilateur, flags, plateforme, CPU, mode alimentation fixé, températures/throttling, mission/mod, profil, `flightHz`, `systemsHz`, `keyframeSeconds`, clients, cardinalités, warm-up, durée, nombre de chaque échantillon, séries brutes, ordre des quarante tranches et commandes. Les qualifications courtes obligatoires `fichier absent` et `enabled=false` sont liées au même rapport mais exécutées hors des fenêtres mesurées et ne contribuent à aucune médiane. L’endurance appartient au soak composite unique du document 06. Le premier chargement/provisionnement est mesuré séparément du steady-state. La campagne est invalide, pas PASS, si les minima, l’ordre normatif, la persistance des quatre runtimes, le mode alimentation, la stabilité d’horloge ou l’absence de throttling ne sont pas prouvés.

## 9. Matrice configuration / résultat

| Cas | Résultat |
|---|---|
| `systemsHz` absent | défaut 10, configuration valide |
| `systemsHz=1` ou `20`, quelle que soit la valeur valide de `flightHz` | valide |
| `systemsHz=0`, `21`, non entier | fichier entier invalide, zéro socket |
| `systemsHz>flightHz` | valide ; schedulers indépendants |
| config Phase 1 valide sans nouvelle clé | compatible et déterministe |
| profil source incomplet | runtime actif possible, session Phase 2 refusée |
| closure hors borne | session refusée, métrique source-limit, aucune troncature |
| manifeste >16 Mio ou >64 parts | session refusée avant envoi |
| tentative de croissance après `Ready` | aucune allocation ; `SourceLimitExceeded`, `SESSION_END(Restart,RECONNECT_ALLOWED)` et gate performance en échec |

## 10. Vérification de l’observabilité

Les tests DOIVENT :

1. déclencher chaque métrique et enum atteignable ;
2. vérifier unités, scopes, reset et agrégats process ;
3. vérifier les gauges à zéro après purge ;
4. comparer records/bytes/parts aux transactions décodées ;
5. vérifier qu’un refus incrémente une seule raison primaire ;
6. provoquer saturation et high-water sans overflow ;
7. confirmer l’absence de labels issus du pair ou du jeu ;
8. scanner les logs pour contenu interdit ;
9. comparer les durées sectionnelles au tick total avec tolérance d’instrumentation ;
10. prouver zéro allocation après warm-up dans les scénarios nominal et borné.

## 11. Traçabilité

Ce document couvre `P2-REQ-013`, `P2-REQ-041` à `P2-REQ-046`, `P2-REQ-049` et `P2-REQ-050`. Les scénarios de sécurité et de conformité sont détaillés dans [06-validation-securite-et-conformite.md](06-validation-securite-et-conformite.md).
