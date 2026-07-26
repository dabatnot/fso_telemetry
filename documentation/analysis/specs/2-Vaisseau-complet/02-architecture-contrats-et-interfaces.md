# 02 — Architecture, contrats et interfaces

## 1. Objet et statut

Ce document définit l’architecture de production, les seams autorisés dans FS2Open, les interfaces proposées et la possession mémoire. Les signatures C++ sont prescriptives quant aux responsabilités et aux types logiques ; l’implémentation PEUT ajuster les noms privés si la traçabilité `P2-REQ-*` reste explicite.

La Phase 2 étend le runtime Phase 1 existant. Elle NE DOIT créer ni second runtime réseau, ni socket de simulation, ni chemin parallèle de session.

## 2. État du dépôt inspecté

Les seams courants à réutiliser sont :

| Seam | Rôle actuel | Extension Phase 2 |
|---|---|---|
| `code/telemetry/engine_adapter.h/.cpp` | validation de `Player`, conversion quaternion, capture cinématique | découverte autorisée puis capture atomique du joueur et de sa closure service/docking dans un DTO possédé |
| `code/telemetry/entity_id_registry.h/.cpp` | signature moteur vers `entity_id` monotone | lifecycle du joueur et de chaque ship de fermeture autorisé |
| `code/telemetry/phase1_state_image.h/.cpp` | image de deux ou quatre records | nouvelle image variable et bornée ; l’image Phase 1 reste disponible |
| `code/telemetry/phase1_snapshot_slot.*` | baseline, candidate et delta cumulatif | généralisation sans modifier les règles d’ACK |
| `code/telemetry/session_controller.*` | slots, handshake, publication par client | sélection du profil, manifeste, closure et lifecycle Phase 2 |
| `code/telemetry/native_session_runtime.cpp` | tick principal, capture, priorité egress | cadence systèmes et capture keyframe forcée |
| `code/playerman/playercontrol.cpp` + autorité cargo extraite | calcul cargo actuellement mêlé au HUD | `CargoScanAuthorityState` persistant, calculé avant reset et lu par HUD + télémétrie |
| `code/telemetry/runtime.cpp` | mission, purge et shutdown | invalidation manifeste et transition de session |
| `code/telemetry/protocol/*` | codecs, validateurs et réplication FSTL | réutilisation des records v1 ; aucune nouvelle disposition filaire |
| `code/source_groups.cmake` | groupe source producteur | déclaration explicite des nouveaux fichiers |
| `test/src/CMakeLists.txt` | cibles de tests | déclaration des suites Phase 2 |

Les appels de lifecycle existants dans `freespace2/freespace.cpp` et `code/mission/missionload.cpp` restent les points d’intégration globaux du runtime. La Phase 2 ajoute deux seams **événementiels** main-thread étroits : `telemetry::OnShipCleanup(signature, cleanup_mode)` au début de `ship_cleanup()` avant purge, et `telemetry::OnSupportTransition(...)` au début de chaque branche `QUEUE`, `ONWAY`, `BEGIN`, `BROKEN`, `END`, `ABORT`, `KILLED` et `COMPLETE` de `ai_do_objects_repairing_stuff()` avant nettoyage des flags/IDs. Ils copient uniquement clés, raison, numéro d’épisode et sample time dans des rings préalloués ; ils ne lisent aucun socket, ne sérialisent rien et n’allouent pas. Un hook dans le chemin de dégâts n’est pas nécessaire tant que le polling et `OnShipCleanup` couvrent les fronts garantis.

Deux autorités gameplay persistantes complètent ces seams sans devenir des callbacks réseau : `CargoScanAuthorityState`, possédé et remis à zéro par la mission dans `playercontrol`, est muté exactement au point et sous les conditions d’appel historiques de `hud_cargo_scan_update/player_inspect_cargo`; `ControlTargetLatch`, également mission-scoped, mémorise dans `read_keyboard_controls()` le branchement ship/caméra après sa décision et avant mutation des commandes. Le HUD lit ensuite l’autorité extraite et `EngineUpdate` lit son dernier état committé sans l’avancer ; la télémétrie désactivée conserve donc exactement progression cargo, contrôles et coût historiques. Aucune autorité n’alloue, ne sérialise ni n’appelle le runtime. Les quatre points d’intégration sont injectables dans les tests, initialisés à l’entrée mission et purgés à la sortie mission/runtime.

## 3. Vue d’ensemble

```text
EngineUpdate (thread principal)
  -> FsoEngineReadView + gardes source
  -> capture validée de la racine joueur
  -> tentative de découverte/capture de l'union service-docking CompleteShip
  -> projections pré-ID par profil : CoreGate={joueur}, CompleteShip=point fixe
  -> pour chaque session/slot
       -> sélection de sa projection et de ses fingerprints
       -> allocation/projection de ses IDs et manifeste propres
       -> Phase2StateImage canonique filtrée avec ses IDs
       -> diff cumulatif / candidate keyframe / fences propres au slot
       -> codecs FSTL 1.1 existants
       -> scheduler de datagrammes Phase 1

Client de preuve indépendant
  -> validation en-tête / fragments / message / records
  -> installation manifeste APPLIED
  -> commit snapshot APPLIED
  -> application delta contre baseline
  -> DashboardProjection dérivée
```

Le filtrage de visibilité se termine avant `Phase2StateImage`. Le diff et le transport NE DOIVENT recevoir aucun champ qui n’est pas autorisé à partir vers le client.

## 4. Composants, responsabilités et possession

| Composant | Responsabilité unique | Possède |
|---|---|---|
| `FsoEngineReadView` | lire les autorités FS2Open après gardes | aucune donnée durable |
| `Phase2ClosureDiscovery` | parcourir IDs/signatures et topologie autorisée avant copie lourde | vue légère bornée, visited-set de 64 ships |
| `Phase2ShipCollector` | valider, convertir et copier le joueur puis chaque ship autorisé de la closure | `Phase2ObservationDto` de sortie fourni |
| `Phase2ClosureBuilder` | construire les projections bornées par profil et leurs descripteurs/fingerprints pré-ID | racine `CoreGate` et extension `CompleteShip`, aucun ID de session |
| `Phase2ManifestBuilder` | allouer les IDs du slot et produire sa transaction `FullRequired` | génération candidate immuable du slot |
| `Phase2StateImageBuilder` | matérialiser les atomes FSTL depuis le DTO et le manifeste du slot | slot d’image préalloué par client |
| `Phase2ProfileGate` | vérifier couverture, manifestes, cardinalités et autorité | statut et raison, aucune donnée moteur |
| `SessionController` | posséder sessions, manifestes installés et baselines | un slot borné par client |
| `SupportTransitionLatch` | préserver les raisons terminales avant nettoyage AI puis les distribuer | ring global de 64 faits drainé chaque `EngineUpdate`; table de 64 latches par session |
| `ShipCleanupLatch` | préserver destruction/départ/vanish avant purge objet | ring global de 64 signatures pertinentes, dédupliquées, drainé avant le ring support |
| `CargoScanAuthority` | décider phase, visibilité, timing et validité sans DTO HUD | un `CargoScanAuthorityState` possédé par le joueur |
| `ControlTargetLatch` | conserver l’autorité ship/caméra choisie pendant la lecture des commandes | enum mission-scoped `Ship`/`Camera`, remis à `Ship` à l’entrée/sortie mission |
| `DatagramScheduler` | prioriser fiable, snapshot puis delta | files héritées bornées |
| `ProofOracleSink` | test uniquement, enregistrer DTO + image au même tick | buffer local de preuve, jamais activé en production |

Le manifeste, le snapshot candidat et la baseline acquittée sont immuables après publication. Une nouvelle génération utilise un autre slot ; aucun buffer en cours de retransmission n’est réécrit.

## 5. Contrats d’interface proposés

### 5.1 DTO moteur

Le DTO logique suivant est requis :

```cpp
struct ShipObservationDto {
    EngineEntityKey key;
    ShipIdentityObservation identity;
    ShipKinematicsObservation flight;
    DamageObservation damage;
    ShieldObservation shields;
    EnergyObservation energy;
    PropulsionObservation propulsion;
    WeaponObservation weapons;
    SupportObservation support;
    DockingObservation docking;
    BoundedVector<SubsystemObservation, 1024> subsystems;
};

struct Phase2ObservationDto {
    CaptureResult capture;
    PlayerObservationKey player_key;
    std::uint64_t producer_sample_time_us;
    BoundedVector<ShipObservationDto, 64> ships;
    ControlObservation player_controls;
    CargoScanObservation player_cargo_scan;
};
```

Les types `BoundedVector` et chaînes peuvent être des implémentations existantes ou des vecteurs provisionnés. Le contrat exige : capacité calculée avant `Ready`, aucune croissance après warm-up, ownership complet et remise à zéro déterministe entre ticks.

`Phase2ShipCollector::collect()` retourne l’un des résultats fermés :

| Résultat | Effet |
|---|---|
| `Valid` | DTO complet et cohérent ; publication autorisable |
| `NoPlayer` | absence normale ; lifecycle et singletons restent publiables |
| `InvalidSource` | candidate refusée, métrique et log borné |
| `SourceLimitExceeded` | taille/cardinalité hors wire ; session Phase 2 non autorisable |
| `UnsupportedEngineState` | combinaison moteur sans représentation v1 ; fail-closed |

Une erreur ne laisse jamais de sous-DTO partiellement réutilisable.

### 5.2 Préconditions de lecture

La garde distingue l’absence normale avant toute déréférence : hors mission, la machine reste `Idle`; en mission, `Player`, `Player_obj` et `Player_ship` tous trois absents donnent `NoPlayer`; toute présence partielle ou incohérence donne `InvalidSource`. Seulement lorsque les trois sont présents, le collecteur vérifie le joueur puis chaque ship de la closure :

1. `GM_IN_MISSION` ;
2. `Player != nullptr`, `Player_obj != nullptr`, `Player_ship != nullptr` ;
3. `Player_obj->type == OBJ_SHIP` ;
4. `Player_obj->instance` dans `0..MAX_SHIPS-1` ;
5. `Player->objnum` et `Player_ship->objnum` dans `0..MAX_OBJECTS-1` ;
6. cohérence bidirectionnelle de `Objects`, `Ships`, `Player_obj` et `Player_ship` ;
7. signature objet strictement positive ;
8. index `ship_info_index`, armes, sous-systèmes, modèles et armor valides avant lecture ;
9. chaque référence support/docking/leader/cargo résout une signature objet ship autorisée ; les cycles de docking réciproques sont normaux et arrêtés par un visited-set, tandis qu’un 65e membre refuse la fermeture.

Les APIs qui peuvent construire paresseusement un index, notamment les helpers de sous-systèmes indexés, NE DOIVENT PAS être appelées sur le fast path. Le provisioning éventuel se fait avant `Ready`, ou le collecteur parcourt les listes déjà possédées par `ship`.

### 5.3 Sources moteur autorisées

| Bloc | Autorités à encapsuler dans `FsoEngineReadView` |
|---|---|
| identité/lifecycle | `object` signature/type/flags, `ship` classe/noms/flags, registres mission |
| coque/boucliers | `object::hull_strength`, `object::shield_quadrant`, maxima dynamiques de `ship`, APIs `objectshield` |
| contrôles | `Player->ci`, mode de contrôle et flags d’aides/liens du joueur/vaisseau |
| énergie/ETS | indices de recharge `ship`, énergie arme, transferts différés, flags `No_ets` |
| propulsion | `physics_info.flags`, carburant afterburner, classe et timers convertis |
| armes | `ship_weapon`, listes de banques, ammo initial/courant, sélections, cooldowns et contre-mesures |
| sous-systèmes/tourelles | liste `ship_subsys`, agrégats, transforms et état turret déjà possédés |
| support | flags de chaque vaisseau, support assigné, réparation/réarmement et relations de docking |
| catalogues | entrées `Ship_info`, `Weapon_info` et définitions modèle de tous les ships de la fermeture autorisée |

Le mapping détaillé et les règles de présence sont fixés dans [04-modele-de-donnees-et-regles-metier.md](04-modele-de-donnees-et-regles-metier.md).

### 5.4 Catalogues et fermeture

La capture distingue deux projections. `CoreGateClosure` contient exactement le joueur valide et ne lit ni support, ni docking, ni cible cargo pour décider si `0x0401` est matérialisable. `CompleteShipClosure` est une extension indépendante : `Phase2ClosureDiscovery` produit un `Phase2DiscoveryDto` possédé ne contenant que clés moteur, signatures, support assigné, relations distantes et `group_leader` validés, puis suit le point fixe décrit ci-dessous. `Phase2ShipCollector` copie d’abord la racine commune, puis les membres supplémentaires de l’extension. Une erreur propre à l’extension invalide `CompleteShip` sans invalider `CoreGate`, sauf si elle révèle une incohérence de la racine ou d’une source effectivement requise par `CoreGate`.

L’allowlist est le prédicat fermé `is_phase2_cockpit_ship_authorized(candidate, edge)` : le joueur valide est l’unique racine ; un ship candidat est autorisé seulement s’il est atteint depuis un membre déjà autorisé par (a) une affectation support dont objnum, signature et flags AI concordent, (b) une entrée directe de `dock_list` réciproque, ou (c) le leader calculé de cette même composante dockée. Support, leader et docking sont réévalués pour chaque nouveau membre jusqu’au point fixe. Équipe, proximité, cible courante, capteurs, registre global des ships, parent lifecycle, acteur d’événement et cible cargo ne sont jamais des critères d’ajout. Le parcours lit les liens directs avec un visited-set préalloué ; il NE DOIT PAS appeler `dock_evaluate_all_docked_objects()`, qui peut allouer. Un edge incohérent ou non réciproque refuse la session avant copie publique.

`Phase2ClosureBuilder` prend le DTO racine et, si elle est valide, l’extension découverte/capturée, puis produit une projection par profil :

```cpp
struct Phase2Closure {
    Phase2Profile profile;
    BoundedVector<EngineEntityKey, 64> ship_entities;
    BoundedVector<ShipClassKey, 64> ship_classes;
    BoundedVector<WeaponClassKey, 4096> weapon_classes;
    BoundedVector<SubsystemDefinitionKey, 4096> subsystems;
    TopologyFingerprint topology_fingerprint;
    CatalogFingerprint catalog_fingerprint;
};
```

Pour `CoreGate`, `ship_entities` contient uniquement la racine, les classes/armes/sous-systèmes ne couvrent que son `CLASS_MANIFEST` et ses définitions requises par les records cœur, et les deux fingerprints sont calculés sur cette projection. Pour `CompleteShip`, ils couvrent tout le point fixe transitif. La découverte brute peut être partagée, mais ni la closure, ni les descripteurs, ni les registres auxiliaires, ni les fingerprints d’un slot `CoreGate` ne contiennent un support ou un docké. Ainsi, un 65e membre, un edge invalide ou un catalogue non représentable dans l’extension ne bloque pas une session `CoreGate` dont la racine reste représentable et ne fait fuiter aucun catalogue de l’extension.

Les bornes plus petites que le wire sont des limites d’implémentation Phase 2 : 64 ships, 1024 sous-systèmes par ship/classe et 4096 sous-systèmes agrégés. La formule `4 + 10K + N` DOIT aussi rester inférieure ou égale à 65 535 records. Si une borne est dépassée, `SourceLimitExceeded` interdit le profil. Les limites wire restent les autorités ultimes.

Chaque paire de fingerprints est profil-scoped. `topology_fingerprint` couvre les membres, références support, leaders et relations de la projection choisie ; sa modification force une keyframe. `catalog_fingerprint` couvre uniquement les `ClassDescriptor`, `WeaponDescriptor`, clés auxiliaires et définitions imbriquées canoniques pré-ID de cette projection, triés ; générations, IDs alloués et index moteur en sont exclus. Sa modification force d’abord un manifeste N+1. Une apparition d’instance d’une classe déjà installée change donc la topologie mais pas le manifeste du profil complet ; elle ne touche pas les fingerprints `CoreGate` si la racine est inchangée.

La clé moteur n’est jamais sérialisée. Chaque membre de `CoreGateClosure` reçoit un `entity_id` public, `ENTITY_LIFECYCLE` et toute la matrice `CORE_SHIP`. Chaque membre de `CompleteShipClosure` reçoit en plus `WEAPON_STATE`, `DOCKING_STATE` et `SUPPORT_STATE`; le joueur de ce profil reçoit aussi les records globaux pilotés exigés par `0x0583`. Les IDs statiques sont assignés en ordre canonique :

- classes par leur `ClassDescriptor` pré-ID canonique ;
- armes par leur `WeaponDescriptor` pré-ID canonique ;
- sous-systèmes selon l’ordre canonique du `CLASS_MANIFEST`, avec `subsystem_id` non nul ;
- banques par `(wire_family,owner_subsystem_id_or_zero,source_family,bank_index)`, avec un `bank_id` non nul distinct pour chaque entrée.

Une collision de nom avec définitions différentes DOIT produire deux clés internes distinctes et une raison de diagnostic ; aucune fusion silencieuse n’est permise. La stabilité est garantie dans le `manifest_id`, pas entre installations ni exécutions. Seuls la capture moteur et les DTO pré-ID immuables sont partageables entre slots : après allocation des IDs de sa session, chaque slot projette sa propre image FSTL, son manifeste, sa baseline et ses ACK.

### 5.5 Builder de manifeste

L’interface doit séparer construction et commit :

```cpp
ManifestBuildResult build_phase2_manifest(
    const Phase2ObservationDto& observation,
    const Phase2Closure& closure,
    Phase2ManifestCandidate& output) noexcept;
```

`output` contient exactement une transaction `FullRequired`, paginée si nécessaire, avec tous les atomes `CLASS_MANIFEST` et `WEAPON_MANIFEST`. Son SHA-256, sa taille, ses parts et ses records sont figés avant mise en file.

La candidate est installée côté producteur seulement après validation locale. Côté client, elle devient visible seulement après validation complète et commit atomique ; l’ACK `APPLIED` autorise ensuite le snapshot dépendant.

Le contrôleur conserve au plus deux générations sémantiques simultanées : `active_manifest` N, encore référencé par la baseline acquittée, et `staged_manifest` N+1, déjà `APPLIED` mais non promu tant que son snapshot dépendant n’est pas `APPLIED`. Après promotion de ce snapshot, N n’est libéré que lorsqu’aucun message fiable, delta ou baseline ne le référence. Si une troisième mutation catalogue arrive, elle est coalescée en un fingerprint/rebuild-intent borné ; elle ne crée jamais un troisième catalogue et sera reconstruite après promotion de N+1. Les ACK et paquets tardifs de N restent idempotents ou stale selon Phase 0.

`PlayerPresence::Absent` est une exception explicite au retrait least-privilege : la session gèle `active_manifest` et son `catalog_fingerprint`, ne construit jamais un catalogue vide et continue de référencer ce manifeste depuis le snapshot à deux singletons. Cette rétention est nécessaire à `CORE_SHIP` FSTL 1.1 et ne révèle rien que le client n’ait déjà installé. À la réapparition, la nouvelle closure est comparée au fingerprint gelé : égalité = réutilisation de N ; différence = séquence N+1 puis keyframe avant toute exposition.

### 5.6 Image canonique

`Phase2StateImageBuilder` reçoit le DTO, les IDs publics, le profil exact et le manifeste installé. Il doit fournir :

```cpp
Phase2ImageResult build_phase2_state_image(
    const Phase2ObservationDto& observation,
    const InstalledPhase2Manifest& manifest,
    Phase2Profile profile,
    Phase2StateImageSlot& output) noexcept;
```

L’image contient des `StateAtom` triés selon `(record_type, identity bytes)` pour rendre diff, hash et goldens déterministes. Aucun ordre d’itération d’une table moteur ou d’un conteneur non ordonné ne doit influencer les octets.

Les profils acceptés sont fermés :

| `Phase2Profile` | Mask | Records spécifiques |
|---|---:|---|
| `CoreGate` | `0x0401` | matrice `PLAYER_KINEMATICS + CORE_SHIP` |
| `CompleteShip` | `0x0583` | `CoreGate` + contrôle + armes + cargo/docking/support |

### 5.7 Session et runtime

Le `SessionController` DOIT posséder pour chaque client :

- profil immuable ;
- `active_manifest`, éventuel `staged_manifest`, leurs références de baseline et un rebuild-intent coalescé ;
- dernière observation validée ;
- image courante ;
- snapshot candidat ;
- baseline acquittée ;
- dernier delta cumulatif remplaçable ;
- file fiable d’événements lifecycle, chaque entrée portant sa baseline/snapshot de dépendance ;
- table de 64 latches support par session, indexée par entité assistée, et candidate terminale immuable ;
- registre d’IDs et compteur monotone.

Le runtime Phase 1 reste l’unique propriétaire des sockets et callbacks. Les nouveaux composants ne reçoivent aucune référence à un socket.

Le ring global est vidé une seule fois par `EngineUpdate`; chaque fait met à jour le latch de l’entité assistée dans les sessions concernées, puis le slot global est immédiatement réutilisable. La table contient au plus les 64 ships de la closure. Une candidate keyframe copie les latches courants sans les consommer ; `APPLIED` efface seulement les générations de latch incluses, de sorte qu’un terminal plus récent reste destiné à une candidate successeur. Un débordement du ring global ferme toutes les sessions Phase 2 actives, car l’appartenance d’un fait perdu est inconnue. Ainsi, aucun pair lent ne retient le ring global ni ne bloque les autres clients.

`OnShipCleanup` consulte sans allocation l’union bornée des signatures présentes dans les registres de sessions ; un cleanup hors closure est ignoré avant insertion. Le mapping est fermé : `SHIP_DESTROYED→Destroyed`, `SHIP_DESTROYED_REDALERT→Vanished`, `SHIP_DEPARTED`, `SHIP_DEPARTED_WARP`, `SHIP_DEPARTED_BAY` et `SHIP_DEPARTED_REDALERT→Departed`, `SHIP_VANISHED→Vanished`. Un mode nul, inconnu ou combinant plusieurs familles refuse le fait, journalise `InvalidCleanupMode` et ferme les sessions concernées avant réutilisation de la signature. Le ring contient au plus 64 signatures distinctes, déduplique plusieurs callbacks du même objet et conserve la raison selon `Destroyed > Departed > Vanished`. Il est drainé avant les faits support afin que les suppressions invalident les références du même tick. Un 65e cleanup pertinent journalise `SourceLimitExceeded` et ferme toutes les sessions Phase 2 avec `SESSION_END(Restart, RECONNECT_ALLOWED)` ; aucun fait n’est écrasé.

### 5.8 Erreurs

Les erreurs privées sont regroupées par couche : `CaptureReason`, `ManifestBuildError`, `Phase2ImageError`, `Phase2ProfileError`. Chaque enum est fermé, possède `Count`, se mappe vers une métrique et un log, et n’est jamais sérialisé comme nouvelle valeur FSTL.

Les réponses filaires utilisent exclusivement les raisons définies par Phase 0 : version, validation, resync, limite de ressources, protocole ou fermeture de session.

## 6. Dépendances autorisées

| Couche | Peut dépendre de | NE DOIT PAS dépendre de |
|---|---|---|
| adaptateur moteur | headers objet, ship, player, physics, mission, AI strictement nécessaires | HUD, rendu, scripting Lua comme source principale, réseau multi comme codec |
| manifestes | DTO et codecs FSTL | pointeurs/tables globales après capture |
| image/diff | DTO public, manifeste installé, réplication Phase 0 | APIs moteur, UI, renderer |
| runtime | configuration, session, transport dédié, builders | socket de gameplay, FFmpeg, OpenGL |
| client de preuve | schéma/goldens et son propre décodeur | bibliothèque codec C++ du producteur |

Les APIs Lua et le protocole multijoueur existant PEUVENT servir d’oracles de test ou de précédents sémantiques, mais NE DOIVENT être copiés comme wire format public.

## 7. Invariants d’état et de propriété

1. Un DTO appartient au tick qui l’a produit et est immuable après publication au builder.
2. Un manifeste installé est immuable pour son `manifest_id`.
3. Un snapshot référence exactement un manifeste déjà installé.
4. Une baseline acquittée et ses octets ne changent jamais.
5. Un delta ne contient que des atomes autorisés par le profil de la session.
6. Un `entity_id` ne change pas tant que la signature reste continûment membre de la closure ; sortie de closure, disparition ou suppression retire définitivement l’ID, même si la même signature redevient autorisée plus tard.
7. La disparition d’un ship référencé déclenche sa suppression lifecycle et recalcule la closure avant construction de l’image suivante.
8. Le client n’expose aucun état partiellement validé.
9. Un événement d’apparition n’est libéré qu’après `APPLIED` du snapshot qui introduit son ID ; un événement terminal qui exige encore l’ancien ID est acquitté avant la keyframe qui le retire.
10. `SESSION_STATE.observed_player_entity_id` ne change jamais par delta : apparition, disparition et respawn utilisent une keyframe.
11. Un delta sérialisé supérieur à 1 048 576 octets est abandonné au profit d’une keyframe exhaustive ; si celle-ci dépasse 16 777 216 octets, le producteur journalise `SourceLimitExceeded` puis envoie `SESSION_END(Restart, RECONNECT_ALLOWED)`.

## 8. Flux de données nominal

1. `EngineUpdate` appelle le runtime sur le thread enregistré.
2. Le scheduler détermine si le tick flight, systems ou keyframe est dû.
3. Le collecteur valide et copie la racine joueur, puis tente indépendamment la découverte/capture de l’extension `CompleteShip`.
4. Le builder produit `CoreGateClosure={joueur}` et, si possible, `CompleteShipClosure=point fixe`, avec descripteurs et fingerprints pré-ID séparés ; aucune projection ne porte d’ID de session.
5. Pour chaque slot, le profile gate choisit sa projection ; si son `catalog_fingerprint` a changé, son builder prépare son manifeste et alloue ses IDs, et si seul son `topology_fingerprint` change, il force une keyframe sous son manifeste actif.
6. Toujours dans la boucle du slot, le profile gate valide manifeste, cardinalités, autorité et ensemble de records, puis projette l’image canonique filtrée avec les IDs de ce slot.
7. Les événements lifecycle observés sont projetés dans la voie fiable du slot avec leur fence de dépendance ; aucun événement dépendant n’est émis avant sa baseline.
8. Le contrôleur du slot choisit manifeste, snapshot ou delta cumulatif à partir de sa propre image/baseline.
9. Le scheduler respecte la priorité et le budget commun de datagrammes.
10. Chaque client valide, commit et acquitte ses transactions ; le producteur ne promeut que la baseline du slot concerné après son `APPLIED`.

## 9. Démarrage et arrêt

Au démarrage, le runtime charge d’abord la configuration, l’identité et les quotas Phase 1. Seulement si la configuration est valide et `enabled=true`, il réserve/provisionne ensuite toutes les capacités Phase 2 maximales, valide le plafond process avant tout bind et valide le schéma fermé. Avec configuration absente ou `enabled=false`, aucun buffer Phase 2 n’est alloué. Aucune lecture des tables mission n’a lieu avant entrée mission ; celle-ci ne fait que remettre à zéro et construire des vues dans les capacités déjà possédées.

À l’entrée mission, le premier joueur valide déclenche fermeture et manifeste. Le producteur NE DOIT envoyer le snapshot Phase 2 avant l’ACK `APPLIED` du manifeste. Pendant l’attente, heartbeat et trafic fiable de contrôle restent actifs.

À la sortie mission, toute candidate manifeste/snapshot, baseline, registre d’entités et fermeture Phase 2 sont purgés avant libération des objets. L’arrêt ferme les sessions, vide les files et détruit les buffers dans l’ordre hérité ; aucun callback tardif ne peut lire la simulation.

## 10. Arborescence de code proposée

```text
code/telemetry/
  engine_adapter.h/.cpp                 # extension DTO et collecte
  phase2_closure.h/.cpp                 # fermeture et IDs logiques
  phase2_manifest_builder.h/.cpp        # CLASS + WEAPON transaction
  phase2_state_image.h/.cpp             # image canonique variable
  phase2_profile_gate.h/.cpp            # matrice et raisons fermées
  session_controller.h/.cpp             # extension des slots
  native_session_runtime.cpp            # cadence et orchestration

test/src/telemetry/producer/
  phase2_engine_adapter_tests.cpp
  phase2_closure_tests.cpp
  phase2_manifest_tests.cpp
  phase2_state_image_tests.cpp
  phase2_lifecycle_tests.cpp
  phase2_oracle_tests.cpp
  phase2_security_corpus_tests.cpp

test/src/telemetry/protocol/
  phase2_profile_tests.cpp
  phase2_manifest_codec_tests.cpp
  phase2_state_codec_tests.cpp
```

Une généralisation de `phase1_state_image.*` est acceptable si elle conserve explicitement les tests et le fast path Phase 1. La suppression opportuniste de composants Phase 1 ou la réécriture du transport est hors scope.

## 11. Évolutivité sans anticipation

Les builders manipulent les `RecordType` existants et peuvent accepter de futurs blocs, mais la Phase 2 ne provisionne et n’annonce que les cinq domaines `0x0583`. Aucun placeholder de cible, radar, communication ou vidéo ne doit être créé.

La closure Cockpit prépare le filtrage des phases suivantes sans annoncer `ALL_ENTITIES`. Tout ship support/docking effectivement référencé est toutefois un vaisseau exporté complet, car la validation FSTL exige son lifecycle, `CORE_SHIP` et `WEAPONS`. La closure est transitive, limitée à 64 ships et refusée dès qu’un membre n’est pas autorisé.

## 12. Traçabilité

Ce document satisfait principalement `P2-REQ-009` à `P2-REQ-020`, `P2-REQ-034` à `P2-REQ-039` et `P2-REQ-049`. Les preuves correspondantes sont routées par [06-validation-securite-et-conformite.md](06-validation-securite-et-conformite.md) et les lots par [07-livraison-et-tracabilite.md](07-livraison-et-tracabilite.md).
