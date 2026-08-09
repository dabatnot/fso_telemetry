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
- `code/source_groups.cmake`.

Les codecs FSTL existants restent compatibles et aucun registre ou layout ne change. Chacun des quatre seams moteur est `noexcept`, main-thread, sans allocation, initialisé et purgé avec la mission, et inerte vis-à-vis du réseau.

### 2.2 Build et plateformes

Les fichiers `.cpp` et `.h` sont listés explicitement. La Phase 2 reste compatible avec les plateformes et configurations supportées par le dépôt, n'ajoute aucune dépendance tierce et ne change aucune option de build publique.

## 3. Schéma JSON fermé

### 3.1 Fichier `telemetry.json`

Le chemin, la découverte CFile, la limite de 16 Kio, `JSON_REJECT_DUPLICATES`, la profondeur maximale et l’ordre de chargement Phase 1 sont inchangés.

Exemple complet Phase 2 :

```json
{
  "schemaVersion": 2,
  "phase2Profile": "CompleteShip",
  "enabled": false,
  "bindAddresses": ["127.0.0.1", "::1"],
  "bindPort": 42042,
  "allowedClients": ["127.0.0.1/32", "::1/128"],
  "discoveryEnabled": false,
  "visibilityMode": "Cockpit",
  "trustedFullState": false,
  "maxClients": 1,
  "flightHz": 30,
  "systemsHz": 10,
  "keyframeSeconds": 2,
  "missionHeartbeatMs": 500,
  "idleHeartbeatMs": 1000,
  "maxDatagramsPerTick": 64
}
```

Les clés normatives de Phase 2 sont :

| Clé | Type | Défaut | Domaine | Effet |
|---|---|---:|---:|---|
| `systemsHz` | entier JSON | `10` | `1..20` | cadence dégâts, boucliers, énergie, propulsion, armes, sous-systèmes, support, cargo et docking |
| `phase2Profile` | string | aucun en v2 | `CoreGate` ou `CompleteShip` | projection choisie une fois avant bind |

Les clés héritées conservent exactement leurs défauts et bornes :

| Clé | Défaut | Domaine Phase 2 |
|---|---:|---|
| `schemaVersion` | obligatoire si fichier | `1` ou `2` |
| `phase2Profile` | interdit en v1, obligatoire en v2 | `CoreGate` ou `CompleteShip` en v2 |
| `enabled` | `false` | booléen |
| `bindAddresses` | loopback v4/v6 | 1–2 adresses numériques uniques |
| `bindPort` | `42042` | 1024–65535 |
| `allowedClients` | loopback v4/v6 | 1–32 CIDR canoniques |
| `discoveryEnabled` | `false` | exactement `false` |
| `visibilityMode` | `Cockpit` | exactement `Cockpit` |
| `trustedFullState` | `false` | exactement `false` |
| `maxClients` | `1` | 1–4 |
| `flightHz` | `30` | 1–60 |
| `keyframeSeconds` | `2` | 1–5 |
| `missionHeartbeatMs` | `500` | 200–5000 |
| `idleHeartbeatMs` | `1000` | 200–5000 |
| `maxDatagramsPerTick` | `64` | 1–256 |

Une clé inconnue, dupliquée, mal typée ou hors borne invalide l’objet entier. Une configuration v1 interdit `phase2Profile` et migre explicitement vers `CompleteShip`; l’absence de `systemsHz` y produit le défaut 10. Une configuration v2 exige `phase2Profile` et n’applique aucun fallback si le champ manque ou si sa valeur est inconnue. Le profil effectif est figé à l’initialisation du runtime ; le changer exige un redémarrage.

### 3.2 Relations croisées

- `systemsHz` et `flightHz` sont indépendants ; un tick systèmes capture sa propre cinématique cohérente si nécessaire et ne modifie pas la cadence de publication flight/control.
- `keyframeSeconds × systemsHz >= 1` est naturellement garanti par les bornes et n’ajoute aucune restriction.
- `visibilityMode=Cockpit`, `trustedFullState=false` et `discoveryEnabled=false` restent obligatoires.
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
| records dans une image `CoreGate` | `9 + N`, au plus `1033` |
| records dans une image `CompleteShip` | `4 + 10K + N`, au plus `4740` |
| banques cataloguées par variante | 192 au total, chacune dans sa famille à 0–64 |
| événements lifecycle fiables en attente | fenêtre fiable héritée, sans nouvelle croissance |
| images complètes | une image courante projetée et une baseline par client ; la candidate sérialisée vit dans la rétention fiable ; seul le DTO moteur pré-ID est partagé |
| deltas cumulatifs | un par baseline/client |

La limite de 64 classes est supérieure au besoin nominal d’un joueur mais borne les closures de mod et les références transitives. Elle n’élargit pas le contenu autorisé. Toute multiplication `maxClients × slots × taille` utilise une arithmétique vérifiée `size_t` avant bind. `record_length=65 536`, 193 banques de classe ou toute valeur source supérieure au maximum de profil donnent `SourceRecordTooLarge`/`SourceLimitExceeded` avant conversion vers un type plus petit et avant mise en file. Aucune fixture n'a à construire une image artificielle de 65 535 entrées.

### 4.3 Plafond mémoire process

La Phase 2 remplace le plafond provisoire Phase 1 de 256 Mio par le plafond compilé et testé `Phase2KnownBudgetCapBytes = 402 653 184` octets (384 Mio), inclusif de toutes les allocations Phase 1 conservées. Les sous-budgets sont :

| Scope inclusif | Plafond | Contenu maximal |
|---|---:|---|
| partagé process/mission | 134 217 728 octets | DTO/closure/scratch pré-ID préalloués, descripteurs de catalogue partagés, rings, registres sources et métriques |
| par client | 83 886 080 octets | rétention fiable 33 554 432, réassemblage 4 194 304, image courante et baseline jusqu’à 16 777 216 chacune, delta jusqu’à 1 048 576, egress/latches/manifest IDs/métadonnées dans le solde |
| process total | `shared + maxClients × perClient <= 402 653 184` | avec `maxClients<=4`, arithmétique vérifiée avant bind |

Ces plafonds sont simultanés, non des estimations moyennes. Chaque objet préalloué publie capacité et octets possédés ; leur somme égale le budget calculé. Un dépassement d’un sous-budget ou du total donne `StartupBudgetExceeded`, zéro bind et zéro état `Ready`. Chaque plafond est accepté exactement et la valeur `+1` est rejetée.

### 4.4 Préallocation

Après chargement d’une configuration valide avec `enabled=true`, avant tout bind et avant `Ready` Phase 2 :

1. le collecteur provisionne les maxima absolus Phase 2 : 64 ships, 64 classes, 4096 armes, 4096 sous-systèmes, 192 banques par variante et 4740 atomes d'image ;
2. les builders provisionnent aux maxima les records, clés, valeurs et scratch de sérialisation ;
3. le process provisionne les descripteurs pré-ID partagés ; chaque slot client provisionne ses deux slots sémantiques `active/staged`, image courante, baseline, rétention fiable, delta et latches, car IDs, `manifest_id`, ACK et late join sont session-scoped ;
4. les capacités et leurs octets possédés sont enregistrés comme high-water initial, puis le budget 384 Mio est validé ; un échec conserve zéro socket ;
5. toute croissance ultérieure est comptée et produit un diagnostic de limite.

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

Un ordinal inconnu est impossible à exposer comme label ; nombre, orthographe et remise à zéro de chaque enum sont stables.

Les comparaisons détaillées source/décodage restent extérieures au runtime de livraison.

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

Les logs ne contiennent ni payloads, fragments, catalogues, noms/callsigns, texte cargo, chemins absolus, IP complète, IDs aléatoires, pointeurs, handles ou dump par tick.

## 8. Travail borné et allocations

### 8.1 Fast path désactivé

Le module désactivé conserve zéro socket, zéro allocation persistante Phase 2 et zéro log récurrent. `systemsHz` et le profil ne sont pas relus sur le fast path après chargement.

### 8.2 Runtime actif Release

Le runtime réutilise une projection pré-ID immuable et les arènes fiables existantes par client. Chaque tick et chaque transaction possède une borne de cardinalité et de travail ; aucune attente réseau ni allocation steady-state non bornée n'est permise. Les fixtures `193e banque`, `record_length=65 536`, maximum de profil `+1` et chaque cap mémoire `+1` sont refusées avant egress.

### 8.3 Relevé de performance

Deux observations Release courtes utilisent le vrai produit, l'une `CoreGate`, l'autre `CompleteShip`, pour une durée combinée inférieure à cinq minutes. Le relevé indique plateforme, mission, profil, cadences, nombre de clients et cardinalités, puis consigne attendu, observé, écart et impact. Il ne calcule aucun score ni percentile de certification.

## 9. Matrice configuration / résultat

| Cas | Résultat |
|---|---|
| v1 sans `phase2Profile` | migration explicite vers `CompleteShip` |
| v1 avec `phase2Profile` | fichier entier invalide, zéro socket |
| v2 sans `phase2Profile` ou valeur inconnue | fichier entier invalide, zéro socket |
| v2 avec `CoreGate` ou `CompleteShip` | profil figé avant bind |
| `systemsHz` absent | défaut 10, configuration valide |
| `systemsHz=1` ou `20`, quelle que soit la valeur valide de `flightHz` | valide |
| `systemsHz=0`, `21`, non entier | fichier entier invalide, zéro socket |
| `systemsHz>flightHz` | valide ; schedulers indépendants |
| config Phase 1 valide sans nouvelle clé | compatible et déterministe |
| profil source incomplet | runtime actif possible, session Phase 2 refusée |
| closure hors borne | session refusée, métrique source-limit, aucune troncature |
| manifeste >16 Mio ou >64 parts | session refusée avant envoi |
| tentative de croissance après `Ready` | `SourceLimitExceeded`, puis `SESSION_END(Restart,RECONNECT_ALLOWED)` sans croissance non bornée |

## 10. Qualité de l’observabilité

- les métriques indiquent unité, portée et règle de remise à zéro ;
- les gauges reviennent à zéro après purge ;
- records, octets et parts correspondent aux transactions produites ;
- un refus incrémente une raison primaire ;
- saturation et high-water restent représentables sans overflow ;
- aucun label ne dépend d'une chaîne fournie par le pair ou le jeu ;
- les durées sectionnelles restent cohérentes avec le temps total du tick ;
- les allocations et croissances après `Ready` sont visibles.

## 11. Traçabilité

Ce document couvre `P2-REQ-013`, `P2-REQ-041` à `P2-REQ-046`, `P2-REQ-049` et `P2-REQ-050`. Les scénarios de sécurité et de conformité sont détaillés dans [06-validation-securite-et-conformite.md](06-validation-securite-et-conformite.md).
