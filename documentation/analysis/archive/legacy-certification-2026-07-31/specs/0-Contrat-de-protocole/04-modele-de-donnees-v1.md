# 04 — Modèle de données FSTL 1.0

## 1. Objet et statut

Ce document est l'annexe normative du schéma métier FSTL 1.0. Il transforme l'[inventaire des données réplicables](../../01-telemetry-data-inventory.md) et les invariants de l'[architecture](../../02-telemetry-architecture.md) en 28 `RecordType` entièrement bornés. Pour une même valeur canonique, un encodeur conforme DOIT produire les champs ci-dessous dans l'ordre indiqué, sans padding.

Le [format filaire et les registres](02-format-filaire-et-registres.md) définissent l'enveloppe de record, les scalaires, l'endianness et les chaînes. La [session, les horloges et la fiabilité](03-session-horloges-fiabilite.md) définissent les transactions paginées de manifestes et snapshots, les baselines et la QoS. Les [capabilities et vues spécialisées](05-capabilities-et-vues-specialisees.md) définissent la négociation, le bundle Talking Head et le flux H.264. Le présent document ne redéfinit ni le transport, ni la fragmentation, ni les payloads vidéo.

Les mots **DOIT**, **NE DOIT PAS**, **DEVRAIT** et **PEUT** ont le sens défini par le cadre normatif de cette phase.

## 2. Conventions de schéma

### 2.1 Types abrégés

| Notation | Encodage et validation v1 |
|---|---|
| `bool8` | `u8`, uniquement `0` ou `1` |
| `entity_id` | `u64`, non nul ; `0` signifie « aucune entité » seulement lorsqu'un champ l'autorise |
| `class_id`, `weapon_class_id` | `u32`, non nul ; portée donnée par la génération de manifeste active |
| `asset_id` | `u64`, non nul ; stable dans un bundle de communication |
| `subsystem_id`, `bank_id` | `u32`, non nul et stable dans l'entité parente |
| `str<N>` | `u16 byte_length` puis UTF-8 valide, au plus `N` octets, sans NUL |
| `vec3f` | trois `float32` finis `(x,y,z)` |
| `quatf` | quatre `float32` finis `(w,x,y,z)`, norme dans `[0,9999 ; 1,0001]`, local vers monde, signe canonique |
| `mat3f` | neuf `float32` finis, ordre lignes puis colonnes |
| `rgba8` | quatre `u8` `(r,g,b,a)` |
| `duration_us` | `u64`, microsecondes, dans `[0 ; 86 400 000 000]` sauf borne plus stricte |
| `sample_time_us` | `u64`, horloge monotone producteur ; ne recule pas dans une session |
| `list<T,N>` | `count:u16`, `item_version:u8=1`, `item_size:u16`, puis `count` items ; `count <= N` |
| `vlist<T,N>` | `count:u16`, puis pour chaque item `item_version:u8=1`, `item_size:u16`, `item_payload:item_size` ; utilisé seulement pour items de taille variable |

Tous les `float32` sont IEEE-754 binary32 finis. Sauf borne métier plus stricte, une quantité de HP, énergie, distance, vitesse, masse, dommage ou capacité utilise `[0 ; 1,0e12]`; une position utilise `[-1,0e12 ; +1,0e12]` world-units et une composante de vitesse `[-1,0e9 ; +1,0e9]` world-units/s. Une valeur négative n'est admise que lorsque sa sémantique le prévoit. Le producteur DOIT valider avant écriture ; une source moteur dépassant un plafond v1 NE DOIT PAS être tronquée silencieusement.

Les HP, unités d'énergie, de carburant, de puissance, de cargo et de masse sont les scalaires natifs normalisés de FS2Open ; FSTL ne les présente pas comme des unités SI. Un champ `*_per_s` utilise l'unité de son numérateur par seconde. Une intensité ou un multiplicateur est sans dimension. Un compte, index, ID, enum, bitset et ratio est sans unité. Les repères, angles, distances et temps suivent le cadre normatif : monde/local explicitement nommé, world-unit, radian et microseconde.

Les IDs opaques `species_id`, `ship_type_id`, `team_id`, `iff_id`, `armor_id`, `damage_type_id`, `radar_icon_id`, `pattern_id`, `tag_id` et `visual_id` sont normalisés par le producteur, stables dans la session ou la génération de manifeste qui les contient et indépendants des indices moteur. `0` signifie « inconnu/générique » uniquement lorsque la table du champ l'autorise. Ils n'impliquent aucun accès à un tableau FS2Open.

Les listes dont la taille moteur n'a pas de maximum public sûr utilisent les plafonds protocolaires de ce document. Lorsque l'ensemble dépasse un plafond d'item par record, le producteur DOIT le répartir en records atomiques distincts lorsque la clé le permet. Pour une liste intrinsèquement atomique non paginable, il DOIT retirer le bit `StateDomainCoverage` concerné ou la capability visuelle spécialisée avant la session et enregistrer la métrique locale non filaire `SOURCE_LIMIT_EXCEEDED`; il NE DOIT PAS publier un faux état « complet ». La transaction paginée de snapshot ou manifeste peut contenir un nombre borné de records sur plusieurs messages de 1 Mio sans changer leur granularité.

`record_length:u16` impose en outre que le payload complet de tout record mesure au plus 65 535 octets. Chaque `count` est borné à la fois par son plafond métier et par la taille encodée exacte restant dans le record. Un atome ne peut pas être coupé en deux records portant la même clé. S'il dépasse 65 535 octets et qu'aucun chunking explicite n'est défini (seul `COMM_ASSET_MANIFEST` en définit un ici), le producteur refuse le domaine ou la session, enregistre le diagnostic local non filaire `SOURCE_RECORD_TOO_LARGE` et, si la session était déjà active, utilise `SESSION_END/ProtocolError`; aucune chaîne, liste ou valeur n'est tronquée. Un récepteur classe une longueur incohérente selon `BAD_RECORD_LENGTH`/`OUT_OF_RANGE` de la spec 06. La conformité DOIT tester un record valide de 65 535 octets et le rejet du même record à 65 536 octets.

### 2.2 Masques de présence

Sauf exception explicitement fixée, un record v1 commence après sa clé par `presence:u64`. Le bit 0 est le bit de poids faible. Un champ conditionnel est sérialisé, dans l'ordre de la table du record, si et seulement si son bit vaut `1`. Les bits réservés DOIVENT être nuls. Les champs obligatoires ne possèdent pas de bit de présence.

À défaut d'une condition plus stricte dans la table, un bit vaut `1` si et seulement si le concept est applicable au type d'entité/classe **et** que le producteur dispose de la source autoritaire correspondante dans le domaine annoncé ; il vaut `0` si le concept n'est pas applicable ou n'est pas couvert. Tous les champs d'un groupe portant le même bit sont présents ou absents ensemble. Un record complet qui remet un bit à zéro efface le groupe précédemment présent. Une chaîne de longueur zéro ne remplace jamais l'absence sauf si sa borne autorise explicitement zéro. Le client ne choisit pas champ par champ : la négociation porte sur domaines et capabilities.

Un masque exprime l'absence sémantique dans une **valeur complète** ; ce n'est pas un patch. `record_flags.PARTIAL` est interdit pour les 28 records v1 et son emploi provoque le rejet commun `RESERVED_FLAG` de la spec 06, enregistré dans les diagnostics locaux. `record_flags=0` remplace l'atome complet. `CREATE` et `DELETE` sont admis uniquement par les tables ci-dessous, sont mutuellement exclusifs et sont fiables selon la table QoS de la spec 03. Un record `DELETE` contient uniquement sa clé canonique ; sa longueur doit être exactement celle de cette clé.

Dans un `FULL_SNAPSHOT`, tous les atomes requis par le mode et les capabilities négociés sont présents avec `record_flags=0`. Dans un `DELTA`, un atome absent reprend sa valeur de baseline ; un atome présent la remplace entièrement. La transaction paginée est validée puis publiée atomiquement.

### 2.3 Nature et visibilité

Chaque champ porte une nature principale : `A` état autoritaire, `C` catalogue, `D` dérivé ou `E` événement. Les champs `D` listés en section 11 ne sont pas sérialisés dans ces records.

Le producteur DOIT filtrer avant sérialisation toute entité, identité, contact,
cargo, navpoint et relation que le joueur observé ne connaît pas. Il NE DOIT
PAS créer un record caché rempli de zéros : l'atome est absent, ou ses champs
révélables sont absents selon le masque défini. Les records de présentation
`COMM_*` appartiennent au processus joueur observé.

## 3. Registre des `RecordType`

`0` est invalide. Tous les records ci-dessous utilisent `record_version=1`.

| Valeur `u16` | Nom | Scope | Atome de remplacement du delta | `CREATE/DELETE` |
|---:|---|---|---|---|
| 1 | `SESSION_STATE` | global producteur | singleton session | non/non |
| 2 | `MISSION_STATE` | global mission | singleton génération mission | non/non |
| 3 | `CLASS_MANIFEST` | global catalogue | `(manifest_generation,class_id)` dans une génération exhaustive | non/non |
| 4 | `WEAPON_MANIFEST` | global catalogue | `(manifest_generation,weapon_class_id)` dans une génération exhaustive | non/non |
| 5 | `ENTITY_LIFECYCLE` | entité | `entity_id` ; sa suppression cascade tous ses records | oui/oui |
| 6 | `SHIP_IDENTITY` | entité vaisseau | `entity_id` | non/non |
| 7 | `FLIGHT_STATE` | entité mobile | `entity_id` | non/non |
| 8 | `CONTROL_STATE` | entité pilotée | `entity_id` | non/non |
| 9 | `DAMAGE_STATE` | entité destructible | `entity_id` | non/non |
| 10 | `SHIELD_STATE` | entité | `entity_id` | non/non |
| 11 | `SUBSYSTEM_STATE` | sous-système | `(entity_id,subsystem_id)` | oui/oui |
| 12 | `ENERGY_STATE` | vaisseau | `entity_id` | non/non |
| 13 | `PROPULSION_STATE` | vaisseau | `entity_id` | non/non |
| 14 | `WEAPON_STATE` | vaisseau | `entity_id` | non/non |
| 15 | `LOCK_STATE` | observateur | `entity_id` et ensemble complet de locks | non/non |
| 16 | `TARGET_STATE` | observateur | `entity_id` | non/non |
| 17 | `RADAR_STATE` | observateur | `entity_id` | non/non |
| 18 | `RADAR_CONTACTS` | contact d'un observateur | `(entity_id,contact_entity_id)` | oui/oui |
| 19 | `THREAT_STATE` | entité menacée | `entity_id` et ensemble complet de missiles | non/non |
| 20 | `CARGO_SCAN_STATE` | observateur | `entity_id` | non/non |
| 21 | `DOCKING_STATE` | entité dockable | `entity_id` et topologie complète de ses liens | non/non |
| 22 | `SUPPORT_STATE` | entité assistée | `entity_id` | non/non |
| 23 | `NAVIGATION_STATE` | joueur observé | `entity_id` et liste autorisée complète | non/non |
| 24 | `EFFECT_STATE` | entité | `entity_id` | non/non |
| 25 | `COMM_ASSET_MANIFEST` | global catalogue | chunk `(bundle_hash,first_asset_index,entry_count)` ; installation atomique du bundle | non/non |
| 26 | `COMM_VIEW_STATE` | global producteur joueur | singleton lecture courante | non/non |
| 27 | `COMM_VIEW_EVENT` | global événement | `event_id` append-only | oui/non |
| 28 | `EVENTS` | global événement | chaque `EventItem.event_id`, batch append-only | oui/non |

### 3.1 Matrice de conteneurs

| Conteneur `MessageType` | `RecordType` autorisés | Contraintes supplémentaires |
|---|---|---|
| `MANIFEST` | 3 `CLASS_MANIFEST`, 4 `WEAPON_MANIFEST`, 25 `COMM_ASSET_MANIFEST` | transaction exhaustive, fiable, tous `record_flags=0` |
| `FULL_SNAPSHOT` | 1 `SESSION_STATE`, 2 `MISSION_STATE`, 5 à 24, 26 `COMM_VIEW_STATE` | transaction paginée atomique, tous `record_flags=0` |
| `DELTA` | 1 `SESSION_STATE`, 2 `MISSION_STATE`, 5 à 24, 26 `COMM_VIEW_STATE` | cumulatif depuis baseline ; `CREATE/DELETE` seulement selon le registre ci-dessus |
| `EVENT_BATCH` classe `Replaceable` | 28 `EVENTS` seulement | tous les items portent `RECONSTRUCTIBLE=1`, `RELIABLE=0` |
| `EVENT_BATCH` classe `Reliable` | 27 `COMM_VIEW_EVENT` ou 28 `EVENTS` | type 27 uniquement ici ; chaque item type 28 porte `RELIABLE=1` |

Un batch ne mélange pas types 27 et 28 ni classes de livraison. Tout autre couple message/record, ou un item dont `EventFlags` contredit `delivery_class`, invalide le message logique complet. Les catalogues ne sont jamais inclus dans un snapshot ou delta ; ceux-ci référencent uniquement une génération de manifeste déjà installée.

## 4. Registres numériques métier communs

### 4.1 Enums

Toutes les valeurs non listées sont invalides en `record_version=1`.

| Enum (`u8` sauf mention) | Valeurs numériques |
|---|---|
| `AuthorityMode` | `SOLO=0`, `MULTIPLAYER_CLIENT=1`, `MULTIPLAYER_MASTER=2` |
| `VisibilityMode` | `COCKPIT=0` ; `1..255` réservés et rejetés |
| `SessionPhase` | `STARTING=0`, `SYNCHRONIZING=1`, `LIVE=2`, `ENDING=3` |
| `MissionPhase` | `NONE=0`, `LOADING=1`, `ACTIVE=2`, `ENDING=3`, `ENDED=4` |
| `ObjectType` | `UNKNOWN=0`, `SHIP=1`, `WEAPON=2`, `ASTEROID=3`, `DEBRIS=4`, `JUMP_NODE=5`, `WAYPOINT=6`, `FIREBALL=7`, `OTHER=8` |
| `LifecyclePhase` | `SPAWNING=0`, `ACTIVE=1`, `DEPARTING=2`, `DYING=3`, `DESTROYED=4`, `REMOVED=5` |
| `TransitMode` | `NONE=0`, `WARP=1`, `DOCKBAY=2`, `SCRIPTED=3` |
| `ControlMode` | `UNKNOWN=0`, `SHIP=1`, `VIEW=2`, `FLIGHT_CURSOR=3`, `AUTOPILOT=4` |
| `WeaponSubtype` | `UNKNOWN=0`, `PRIMARY=1`, `MISSILE=2`, `BEAM=3`, `COUNTERMEASURE=4`, `SPECIAL=5` |
| `SubsystemType` | `UNKNOWN=0`, `ENGINE=1`, `TURRET=2`, `RADAR=3`, `NAVIGATION=4`, `COMMUNICATION=5`, `WEAPONS=6`, `SENSORS=7`, `REACTOR=8`, `MANEUVERING=9`, `FIGHTERBAY=10`, `CARGO=11`, `AWACS=12`, `OTHER=13` |
| `AnimationState` | `NONE=0`, `STOPPED=1`, `MOVING=2`, `LOOPING=3`, `LOCKED=4` |
| `EtsMode` | `ABSENT=0`, `AVAILABLE=1`, `LOCKED=2` |
| `WeaponFamily` | `PRIMARY=0`, `SECONDARY=1`, `TERTIARY=2`, `TURRET=3` |
| `ValueTrend` | `UNKNOWN=0`, `DECREASING=1`, `STABLE=2`, `INCREASING=3` |
| `RadarMode` | `SHORT=0`, `LONG=1`, `INFINITE=2`, `CUSTOM=3` |
| `SensorState` | `OFFLINE=0`, `DEGRADED=1`, `ONLINE=2` |
| `RadarVisibility` | `NOT_VISIBLE=0`, `VISIBLE=1`, `DISTORTED=2` |
| `RadarCategory` | `UNKNOWN=0`, `SHIP=1`, `WEAPON=2`, `NAVIGATION=3`, `JUMP_NODE=4`, `ASTEROID=5`, `DEBRIS=6`, `OTHER=7` |
| `ThreatLevel` | `NONE=0`, `DUMBFIRE=1`, `LOCK_ATTEMPT=2`, `LOCK_ACQUIRED=3` |
| `GuidanceType` | `NONE=0`, `HEAT=1`, `ASPECT=2`, `HOMING=3`, `SWARM=4`, `SCRIPTED=5` |
| `ScanPhase` | `NOT_SCANNABLE=0`, `IDLE=1`, `SCANNING=2`, `COMPLETED=3` |
| `DisclosureState` | `HIDDEN=0`, `REVEALED=1` |
| `DockingPhase` | `NONE=0`, `APPROACH=1`, `DOCKING=2`, `DOCKED=3`, `UNDOCKING=4` |
| `SupportPhase` | `NONE=0`, `REQUESTED=1`, `APPROACHING=2`, `DOCKING=3`, `REPAIRING=4`, `REARMING=5`, `OBSTRUCTED=6`, `ABORTED=7` |
| `NavPointType` | `POSITION=0`, `ENTITY=1`, `WAYPOINT=2` |
| `AutopilotState` | `DISENGAGED=0`, `AVAILABLE=1`, `ENGAGED=2`, `REFUSED=3` |
| `AutopilotRefusal` | `NONE=0`, `NO_VALID_NAV=1`, `TOO_CLOSE=2`, `HOSTILES=3`, `ENGINE_DISABLED=4`, `SCRIPT_RESTRICTED=5`, `UNKNOWN=6` |
| `SourceFormat` | `INVALID=0` (jamais émis), `ANI=1`, `EFF=2`, `APNG=3`, `STATIC_IMAGE=4` |
| `DeliveredFormat` | `INVALID=0` (jamais émis), `ANI=1`, `EFF=2`, `APNG=3`, `WEBM_NO_AUDIO=4`, `RGBA8_ATLAS=5`, `PNG=6` |
| `CommPlaybackMode` | `ONCE=0`, `LOOP=1` |
| `CommColorMode` | `HUD_TINT=0`, `FULL_COLOR=1` |
| `CommStopReason` | `NONE=0`, `COMPLETED=1`, `INTERRUPTED=2`, `REPLACED=3`, `HUD_DISABLED=4`, `MISSION_CHANGED=5`, `SESSION_STOPPED=6` |
| `CommEventKind` | `START=1`, `STOP=2` |
| `EventReasonCode` (`u16`) | `NONE=0`, `COMPLETED=1`, `INTERRUPTED=2`, `REPLACED=3`, `SCRIPTED=4`, `DESTROYED=5`, `MISSION_CHANGE=6`, `SESSION_STOP=7`, `ABORTED=8`, `UNKNOWN=9` |

### 4.2 Bitsets métier

Les bits non listés DOIVENT être nuls.

| Bitset | Bits v1 |
|---|---|
| `EntityLifecycleFlags:u32` | `DYING=0x00000001`, `DISABLED=0x00000002`, `EXPLODED=0x00000004`, `SHOULD_BE_DEAD=0x00000008`, `BOMB=0x00000010` |
| `ShipRoleFlags:u16` | `PLAYER=0x0001`, `AI=0x0002`, `SUPPORT=0x0004`, `MISSION_OBJECT=0x0008` |
| `SensorVisibilityFlags:u16` | `STEALTH=0x0001`, `CLOAKED=0x0002`, `SENSOR_VISIBLE=0x0004`, `TAGGED=0x0008` |
| `ProtectionFlags:u16` | `INVULNERABLE=0x0001`, `PROTECTED=0x0002`, `GUARDIAN=0x0004` |
| `PhysicsModeFlags:u32` | `AFTERBURNER=0x00000001`, `BOOSTER=0x00000002`, `GLIDE_ACTIVE=0x00000004`, `GLIDE_FORCED=0x00000008`, `NEWTONIAN_DAMPING=0x00000010`, `LATERAL_TRANSLATION=0x00000020`, `WARP_IN=0x00000040`, `WARP_OUT=0x00000080`, `SCRIPTED=0x00000100`, `SHOCKWAVE=0x00000200`, `IMMOBILE=0x00000400`, `ORIENTATION_LOCKED=0x00000800` |
| `ControlFlags:u32` | `MATCH_SPEED=0x00000001`, `AUTO_TARGET=0x00000002`, `AUTO_MATCH_SPEED=0x00000004`, `PRIMARY_LINKED=0x00000008`, `SECONDARY_DOUBLE=0x00000010`, `AFTERBURNER_REQUESTED=0x00000020` |
| `SubsystemFlags:u32` | `PERTURBED=0x00000001`, `TARGETABLE=0x00000002`, `VISIBLE=0x00000004`, `REVEALED=0x00000008`, `GUARDIAN=0x00000010`, `MOVEMENT_LOCKED=0x00000020`, `BEAM_FREE=0x00000040`, `BEAM_LOCKED=0x00000080` |
| `WeaponGlobalFlags:u32` | `PRIMARY_LINKED=0x00000001`, `SECONDARY_DOUBLE=0x00000002`, `PRIMARY_TRIGGER_HELD=0x00000004`, `SECONDARY_TRIGGER_HELD=0x00000008`, `PRIMARY_LOCKED=0x00000010`, `SECONDARY_LOCKED=0x00000020`, `TARGETING_LASER=0x00000040`, `REMOTE_DETONATORS_ACTIVE=0x00000080`, `BEAM_FREE=0x00000100`, `BEAM_LOCKED=0x00000200` |
| `WeaponClassFlags:u64` | `BOMB=0x0001`, `BALLISTIC=0x0002`, `AMMOLESS=0x0004`, `BEAM=0x0008`, `SWARM=0x0010`, `COUNTERMEASURE=0x0020`, `HOMING=0x0040`, `REMOTE_DETONATABLE=0x0080` |
| `WeaponEffectFlags:u32` | `SHOCKWAVE=0x0001`, `EMP=0x0002`, `TAG=0x0004`, `SHIELD_PIERCING=0x0008`, `SPAWNS_CHILDREN=0x0010` |
| `ClassSubsystemStaticFlags:u32` | `TARGETABLE=0x0001`, `VISIBLE_BY_DEFAULT=0x0002`, `SCANNABLE_CARGO=0x0004`, `ROTATES=0x0008`, `TRANSLATES=0x0010`, `TURRET=0x0020`, `AWACS=0x0040` |
| `ContactFlags:u32` | `BRIGHT=0x00000001`, `CURRENT_TARGET=0x00000002`, `STEALTH=0x00000004`, `TAGGED=0x00000008`, `WARP=0x00000010`, `BOMB=0x00000020`, `HOMING=0x00000040`, `THREAT=0x00000080` |
| `EffectFlags:u32` | `CLOAKED=0x00000001`, `STEALTH=0x00000002`, `ELECTRIC_ARCS=0x00000004`, `SPARKS=0x00000008`, `WARP_VISUAL=0x00000010`, `DEATH_ROLL=0x00000020`, `AMMO_WARNING=0x00000040`, `TARGETING_LASER=0x00000080` |
| `EventFamilyBits:u64` | `ENTITY=0x0001`, `WEAPON=0x0002`, `DAMAGE=0x0004`, `TARGET=0x0008`, `CARGO_SCAN=0x0010`, `DOCKING_SUPPORT=0x0020`, `WARP=0x0040`, `MISSION_SESSION=0x0080`, `CONTROL=0x0100`, `COMMUNICATION=0x0200` |
| `StateDomainCoverage:u64` | `CORE_SHIP=0x0001`, `CONTROL_INPUTS=0x0002`, `PREDICTION=0x0004`, `RADAR_SENSORS=0x0008`, `LOW_FREQUENCY_EFFECTS=0x0020`, `TARGETING=0x0040`, `WEAPONS=0x0080`, `CARGO_DOCK_SUPPORT=0x0100`, `NAVIGATION=0x0200` ; `0x0010` réservé |

## 5. Records globaux et catalogues

### 5.1 `SESSION_STATE` — type 1

Scope global. Nature `A`. Atome : singleton session. `presence` autorise `OBSERVED_PLAYER=bit0`; bits 1–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `presence` | `u64` | bit 0 seulement | A | obligatoire |
| 2 | `producer_id` | `u64` | non nul | A | installation/profil producteur, non secret |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | instant de cet état |
| 4 | `authority_mode` | `AuthorityMode` | enum fermé | A | obligatoire |
| 5 | `visibility_mode` | `VisibilityMode` | enum fermé | A | toujours `COCKPIT` |
| 6 | `session_phase` | `SessionPhase` | enum fermé | A | obligatoire |
| 7 | `reserved` | `u8` | `0` | A | obligatoire, validation d'alignement logique uniquement |
| 8 | `negotiated_capability_generation` | `u32` | `>=1`, croissant | A | génération locale, côté producteur, de la vue `negotiated_capabilities` ; commence à 1 et augmente après toute mise à jour appliquée qui change l'intersection |
| 9 | `negotiated_capabilities` | `u64` | bits définis en spec 05 | A | intersection producteur/client ; bits inconnus nuls |
| 10 | `state_domain_coverage` | `u64` | `StateDomainCoverage` | A | domaines garantis complets par rapport au mode ; `CORE_SHIP` obligatoire pour une session d'état |
| 11 | `event_coverage_state_derived` | `u64` | `EventFamilyBits` | A | familles dont les transitions reconstructibles ou échantillonnées sont annoncées |
| 12 | `event_coverage_exact` | `u64` | sous-ensemble du champ précédent ou famille couverte par hook | A | garantit les transitions brèves au point autoritaire ; `COMMUNICATION` exige le hook Talking Head |
| 13 | `observed_player_entity_id` | `entity_id` | non nul | A | bit 0 ; obligatoire en `MULTIPLAYER_CLIENT`, présent en solo si et seulement si `Player_obj` valide existe, interdit pour master headless sans cockpit |

Le `session_id`, les versions, séquences, `frame_id`, temps de mission et snapshot de référence appartiennent à l'en-tête ou au préfixe de message des specs 02/03 et ne sont pas dupliqués ici.

`negotiated_capability_generation` n'est pas le compteur filaire par émetteur porté par `CAPABILITY_UPDATE` en spec 05. Plusieurs mises à jour filaires peuvent ne produire aucun changement de l'intersection et laissent alors cette génération locale inchangée ; inversement, dès que l'application atomique d'une mise à jour modifie `negotiated_capabilities`, le producteur incrémente ce champ exactement une fois avant de publier le nouvel état.

`producer_id`, `authority_mode`, `visibility_mode`, `state_domain_coverage`, `event_coverage_state_derived` et `event_coverage_exact` sont immuables dans une session. Toute modification exige `SESSION_END` puis un nouveau handshake et un nouveau `session_id`. `producer_sample_time_us`, `session_phase` et `observed_player_entity_id` sont des états courants. `negotiated_capabilities` ne peut évoluer que selon le retrait monotone de la spec 05 et seulement lorsque `CAPABILITY_UPDATE` est actif ; sinon son changement exige lui aussi une nouvelle session.

### 5.2 `MISSION_STATE` — type 2

Scope global. Nature `A`. Atome : singleton de `mission_generation`. `MISSION_NAME=bit0`; bits 1–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `presence` | `u64` | bit 0 seulement | A | obligatoire |
| 2 | `mission_generation` | `u32` | `>=1`, croissant | A | change à chaque chargement, même d'un même fichier |
| 3 | `phase` | `MissionPhase` | enum fermé | A | obligatoire |
| 4 | `paused` | `bool8` | 0/1 | A | pause de simulation |
| 5 | `reserved` | `u16` | `0` | A | obligatoire |
| 6 | `time_compression` | `float32` | `[0 ; 64]` | A | facteur effectif ; `0` admis pendant arrêt temporel |
| 7 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 8 | `mission_name` | `str<255>` | 0–255 octets | A | bit 0 ; absent hors mission, non utilisé comme identité |

`mission_time_us:i64` reste dans l'en-tête. Une variation de `mission_generation` est interdite dans un `DELTA` : elle apparaît uniquement dans un `FULL_SNAPSHOT` qui établit une nouvelle baseline après installation des manifestes requis. La publication atomique de ce snapshot invalide et remplace tout l'état entité, les baselines et les événements de la mission précédente selon la spec 03 ; aucun atome de l'ancienne génération ne survit implicitement.

### 5.3 `CLASS_MANIFEST` — type 3

Scope global, une classe de vaisseau par record. Nature `C`. Clé d'atome : `(manifest_generation:u32,class_id:u32)`. Le record apparaît uniquement dans une transaction `MANIFEST` exhaustive et porte `record_flags=0`; `CREATE`, `DELETE` et `PARTIAL` sont interdits. Une définition retirée est absente de la nouvelle génération complète, jamais supprimée par delta.

Presence bits : `INERTIA=0`, `DAMPING=1`, `MOTION=2`, `HULL=3`, `SHIELD=4`, `ENERGY=5`, `AFTERBURNER=6`, `COUNTERMEASURE=7`, `BANKS=8`, `SUBSYSTEMS=9`, `SCAN=10`, `GLIDE=11`, `AUTOAIM=12`, `RADAR_ICON=13`; 14–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `manifest_generation` | `u32` | `>=1` | C | clé ; DOIT égaler `ManifestPartPayload.manifest_id` du conteneur |
| 2 | `class_id` | `class_id` | non nul | C | clé stable dans la génération |
| 3 | `presence` | `u64` | bits 0–13 | C | obligatoire |
| 4 | `internal_name` | `str<255>` | 1–255 | C | obligatoire |
| 5 | `species_id` | `u32` | `0` inconnu, sinon ID local au manifeste | C | obligatoire |
| 6 | `ship_type_id` | `u32` | `0` inconnu | C | obligatoire |
| 7 | `mass` | `float32` | `(0 ; 1,0e12]` | C | world mass unit |
| 8 | `center_of_mass` | `vec3f` | composantes bornées position | C | repère modèle local |
| 9 | `inertia_tensor` | `mat3f` | coefficients `[−1,0e12 ; +1,0e12]` | C | bit 0 |
| 10 | `rotdamp_s` | `float32` | `[0 ; 3600]` s | C | bit 1, constante de temps |
| 11 | `side_slip_time_const_s` | `float32` | `[0 ; 3600]` s | C | bit 1 |
| 12 | `linear_damping_time_const_local` | `vec3f` | composantes `[0;3600]` s | C | bit 1, amortissements par axe |
| 13 | `motion` | `ClassMotionV1` | fixe, ci-dessous | C | bit 2 |
| 14 | `max_hull` | `float32` | `[0 ; 1,0e12]` HP | C | bit 3 |
| 15 | `max_shield_total` | `float32` | `[0 ; 1,0e12]` HP | C | bit 4 ; total de classe, la segmentation dynamique est état |
| 16 | `energy` | `ClassEnergyV1` | fixe | C | bit 5 |
| 17 | `afterburner` | `ClassAfterburnerV1` | fixe | C | bit 6 |
| 18 | `countermeasure` | `ClassCountermeasureV1` | fixe | C | bit 7 |
| 19 | `bank_definitions` | `vlist<ClassBankV1,192>` | max 64 par famille, `bank_id` uniques et record ≤65 535 | C | bit 8 |
| 20 | `subsystem_definitions` | `vlist<ClassSubsystemV1,1024>` | IDs uniques et record ≤65 535 | C | bit 9 |
| 21 | `scan` | `ClassScanV1` | fixe | C | bit 10 |
| 22 | `glide_cap` | `float32` | `[0 ; 1,0e9]` wu/s | C | bit 11 |
| 23 | `autoaim_fov_rad` | `float32` | `[0 ; π]` rad | C | bit 12 |
| 24 | `radar_icon_id` | `u32` | `0` icône générique | C | bit 13 |

Structures imbriquées, dans l'ordre :

- `ClassMotionV1` : `max_vel:vec3f`, `afterburner_max_vel:vec3f`, `booster_max_vel:vec3f`, `max_rotvel:vec3f`, `max_rear_vel:float32`, puis six `float32` en secondes : `forward_accel_time_const`, `afterburner_forward_accel_time_const`, `booster_forward_accel_time_const`, `forward_decel_time_const`, `slide_accel_time_const`, `slide_decel_time_const`. Toutes les vitesses sont des caps locaux ; toutes les constantes sont dans `[0;3600]`.
- `ClassEnergyV1` : `power_output`, `reserve_energy_max`, `weapon_energy_max`, `weapon_regen_per_s`, `shield_regen_per_s`, tous `float32 [0;1,0e12]`.
- `ClassAfterburnerV1` : `fuel_max`, `burn_per_s`, `recover_per_s`, `minimum_to_engage`, tous `float32 [0;1,0e12]`, puis `cooldown_us:duration_us`.
- `ClassCountermeasureV1` : `weapon_class_id:u32` (`0` aucune classe), `initial_count:u32 [0;1 000 000]`, `cooldown_us:duration_us`.
- `ClassBankV1` : `item_presence:u16` (`CAPACITY=bit0`, `WEAPON_CLASS=bit1`; autres bits nuls), `family:WeaponFamily`, `reserved:u8=0`, `bank_index:u16 [0;63]`, `bank_id:u32` non nul, `weapon_class_id:u32` si bit 1, `capacity_raw:float32 [0;1,0e12]` unités cargo si bit 0, `fire_points:list<vec3f,256>`. `WEAPON_CLASS` est requis pour primaire/secondaire et interdit pour tertiaire en v1. `CAPACITY` suit exactement la présence d'un stock de munitions pour cette banque.
- `ClassSubsystemV1` : `item_presence:u16` (`ALT_NAME=bit0`, `HUD_NAME=bit1`, `ORIENTATION=bit2`, `ARMOR=bit3`), `subsystem_id:u32`, `canonical_index:u16 [0;1023]`, `type:SubsystemType`, `reserved:u8=0`, `internal_name:str<255>`, `alternate_name:str<255>` si bit 0, `hud_name:str<255>` si bit 1, `local_position:vec3f`, `local_orientation:quatf` si bit 2, `radius:float32 [0;1,0e9]`, `max_hits:float32 [0;1,0e12]`, `armor_id:u32` si bit 3, `static_flags:ClassSubsystemStaticFlags`.
- `ClassScanV1` : `required_time_us:duration_us`, `max_distance:float32 [0;1,0e12]`, `max_angle_rad:float32 [0;π]`.

Les champs de table nommés `forward_accel`, `forward_decel`, `slide_accel`, `slide_decel` et `afterburner_forward_accel` sont convertis en constantes de temps ; ils NE DOIVENT PAS être exposés comme accélérations.

### 5.4 `WEAPON_MANIFEST` — type 4

Scope global, une classe d'arme par record. Nature `C`. Clé : `(manifest_generation:u32,weapon_class_id:u32)`. Le record apparaît uniquement dans une transaction `MANIFEST` exhaustive et porte `record_flags=0`; `CREATE`, `DELETE` et `PARTIAL` sont interdits. Une définition retirée est absente de la nouvelle génération complète.

Presence bits : `TITLE=0`, `ACCELERATION=1`, `RANGES=2`, `FIRE=3`, `DAMAGE=4`, `GUIDANCE=5`, `LOCK=6`, `CARGO_REARM=7`, `BURST=8`, `SWARM=9`, `COUNTERMEASURE=10`; 11–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `manifest_generation` | `u32` | `>=1` | C | clé ; DOIT égaler `ManifestPartPayload.manifest_id` du conteneur |
| 2 | `weapon_class_id` | `weapon_class_id` | non nul | C | clé |
| 3 | `presence` | `u64` | bits 0–10 | C | obligatoire |
| 4 | `internal_name` | `str<255>` | 1–255 | C | obligatoire |
| 5 | `title` | `str<255>` | 0–255 | C | bit 0 |
| 6 | `subtype` | `WeaponSubtype` | enum fermé | C | obligatoire ; le sens public dépend aussi des flags |
| 7 | `weapon_flags` | `WeaponClassFlags` | bits connus | C | obligatoire |
| 8 | `max_speed` | `float32` | `[0;1,0e9]` wu/s | C | obligatoire |
| 9 | `acceleration_time_us` | `duration_us` | max 1 h | C | bit 1 ; durée, pas accélération |
| 10 | `mass` | `float32` | `[0;1,0e12]` | C | obligatoire |
| 11 | `gravity_multiplier` | `float32` | `[−1024;+1024]` | C | obligatoire, sans dimension |
| 12 | `lifetime_us` | `duration_us` | max 24 h | C | obligatoire |
| 13 | `min_range` | `float32` | `[0;1,0e12]` wu | C | bit 2 |
| 14 | `optimal_range` | `float32` | `[min;max]` wu | C | bit 2 |
| 15 | `max_range` | `float32` | `[optimal;1,0e12]` wu | C | bit 2 |
| 16 | `fire_wait_us` | `duration_us` | max 1 h | C | bit 3 ; intervalle entre tirs |
| 17 | `energy_per_shot` | `float32` | `[0;1,0e12]` | C | bit 3 |
| 18 | `damage` | `float32` | `[0;1,0e12]` HP | C | bit 4 |
| 19 | `damage_type_id` | `u32` | `0` type générique | C | bit 4 |
| 20 | `effect_flags` | `WeaponEffectFlags` | bits connus | C | bit 4 |
| 21 | `guidance_type` | `GuidanceType` | enum fermé | C | bit 5 |
| 22 | `guidance_fov_rad` | `float32` | `[0;π]` | C | bit 5 |
| 23 | `lock_time_us` | `duration_us` | max 1 h | C | bit 6 |
| 24 | `lock_fov_rad` | `float32` | `[0;π]` | C | bit 6 |
| 25 | `velocity_inheritance` | `float32` | `[−16;+16]` | C | obligatoire, facteur sans dimension |
| 26 | `cargo_size` | `float32` | `(0;1,0e9]` | C | bit 7 |
| 27 | `rearm_interval_us` | `duration_us` | max 1 h | C | bit 7 ; conversion du `rearm_rate` moteur |
| 28 | `reloaded_per_batch` | `u32` | `[1;1 000 000]` | C | bit 7 |
| 29 | `burst_count` | `u16` | `[1;4096]` | C | bit 8 |
| 30 | `burst_interval_us` | `duration_us` | max 1 h | C | bit 8 |
| 31 | `swarm_count` | `u16` | `[1;4096]` | C | bit 9 |
| 32 | `shots_per_trigger` | `u16` | `[1;4096]` | C | bit 9 |
| 33 | `countermeasure_strength` | `float32` | `[0;1,0e12]` | C | bit 10 |

Une arme sans munitions omet `CARGO_REARM`. Une bombe reste `ObjectType.WEAPON`; elle est identifiée par `weapon_flags.BOMB`.

## 6. Entités, identité et mouvement

### 6.1 `ENTITY_LIFECYCLE` — type 5

Scope entité. Nature `A`. Atome `entity_id`. `CREATE` fiable annonce une nouvelle identité monotone ; `DELETE` fiable contient uniquement `entity_id` et supprime en cascade tous les records portant cet ID. Un ID supprimé n'est jamais réutilisé dans la session.

Presence bits : `SIGNATURE=0`, `NET_SIGNATURE=1`, `CLASS_REFERENCE=2`, `PARENT=3`, `ARRIVAL_MODE=4`, `DEPARTURE_MODE=5`, `NON_SHIP_NAMES=6`, `NON_SHIP_TEAM_IFF=7`, `NON_SHIP_RADIUS=8`; 9–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–8 | A | obligatoire hors `DELETE` |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | instant de cycle de vie |
| 4 | `object_type` | `ObjectType` | enum fermé | A | une bombe vaut `WEAPON`, jamais un type distinct |
| 5 | `lifecycle_phase` | `LifecyclePhase` | enum fermé | A | état courant reconstructible |
| 6 | `lifecycle_flags` | `EntityLifecycleFlags` | bits connus | A | flags moteur, pas dérivés de ratios |
| 7 | `signature` | `u32` | valeur moteur élargie | A | bit 0, corrélation locale seulement |
| 8 | `net_signature` | `u32` | valeur moteur élargie | A | bit 1, non unique entre sessions |
| 9 | `class_id` | `u32` | non nul | C | bit 2 ; `CLASS_MANIFEST` pour un vaisseau, `WEAPON_MANIFEST` pour une arme ; interdit si le type n'a pas de classe publique |
| 10 | `parent_entity_id` | `entity_id` | non nul | A | bit 3 ; parent réseau ou créateur lorsqu'il a un sens stable |
| 11 | `arrival_mode` | `TransitMode` | enum fermé | A | bit 4, présent pendant `SPAWNING` ou si la provenance reste pertinente |
| 12 | `departure_mode` | `TransitMode` | enum fermé | A | bit 5, présent pendant `DEPARTING` |
| 13 | `non_ship_internal_name` | `str<255>` | 1–255 | A | bit 6, seulement type différent de `SHIP` et seulement si révélé |
| 14 | `non_ship_display_name` | `str<255>` | 0–255 | A | bit 6 |
| 15 | `non_ship_callsign` | `str<127>` | 0–127 | A | bit 6 |
| 16 | `non_ship_team_id` | `u32` | `[0;65535]` | A | bit 7, seulement si révélé |
| 17 | `non_ship_iff_id` | `u32` | `[0;65535]` | A | bit 7 |
| 18 | `non_ship_radius` | `float32` | `[0;1,0e9]` wu | A | bit 8, pour une entité sans `SHIP_IDENTITY`/`FLIGHT_STATE` |

`objnum`, `instance`, index de tableaux et pointeurs sont interdits. Les bits 6–8 sont interdits pour un vaisseau afin de ne pas créer deux sources de vérité. `BOMB` DOIT être cohérent avec le flag de la classe d'arme ; une incohérence rejette la transaction.

### 6.2 `SHIP_IDENTITY` — type 6

Scope entité avec `ObjectType.SHIP`. Nature principale `A`, références de classe `C`. Atome `entity_id`. Le record est obligatoire pour tout vaisseau visible dans un snapshot.

Presence bits : `DISPLAY_NAME=0`, `CALLSIGN=1`, `WING=2`, `LOGICAL_SIZE=3`, `SENSOR_VISIBILITY=4`; 5–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–4 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `ship_class_id` | `class_id` | non nul | C | classe dans la génération acquittée |
| 5 | `internal_name` | `str<255>` | 1–255 | A | nom de mission autorisé par le mode de visibilité |
| 6 | `display_name` | `str<255>` | 0–255 | A | bit 0 |
| 7 | `callsign` | `str<127>` | 0–127 | A | bit 1 |
| 8 | `species_id` | `u32` | `0` inconnu | A | valeur effective, peut surcharger la classe |
| 9 | `team_id` | `u32` | `[0;65535]` | A | équipe effective |
| 10 | `iff_id` | `u32` | `[0;65535]` | A | IFF effectivement révélé |
| 11 | `role_flags` | `ShipRoleFlags` | bits connus | A | joueur, IA, support, objet de mission |
| 12 | `radius` | `float32` | `[0;1,0e9]` wu | A | rayon de collision/affichage canonique |
| 13 | `wing_id` | `u32` | non nul | A | bit 2 |
| 14 | `wing_position` | `u16` | `[0;4095]` | A | bit 2 |
| 15 | `wing_name` | `str<127>` | 1–127 | A | bit 2 |
| 16 | `logical_size` | `float32` | `[0;1,0e9]` wu | A | bit 3, taille d'interface distincte du rayon |
| 17 | `sensor_visibility_flags` | `SensorVisibilityFlags` | bits connus | A | bit 4 ; absent si non révélé en `Cockpit` |

Une modification d'équipe, d'IFF ou de visibilité remplace cet atome ; elle n'autorise jamais le client à déduire une identité que le producteur n'a pas révélée.

### 6.3 `FLIGHT_STATE` — type 7

Scope entité mobile. Nature `A`, sauf les deux thrusts cosmétiques qui restent des échantillons `A` non utilisables pour valider la physique. Atome `entity_id`.

Presence bits : `DESIRED_VEL=0`, `DESIRED_ROTVEL=1`, `PREV_RAMP_VEL=2`, `VELOCITY_CAPS=3`, `ROTATION_CAPS=4`, `REAR_CAP=5`, `GLIDE_CAPS=6`, `GRAVITY=7`, `TIME_CONSTANTS=8`, `ROTDAMP=9`, `SIDE_SLIP=10`, `COSMETIC_THRUST=11`; 12–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–11 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | instant de la pose |
| 4 | `position_world` | `vec3f` | position bornée | A | repère monde |
| 5 | `orientation_local_to_world` | `quatf` | quaternion canonique | A | seule orientation canonique |
| 6 | `velocity_world` | `vec3f` | vitesse bornée | A | source de toutes les vitesses dérivées |
| 7 | `rotational_velocity_local` | `vec3f` | composantes `[-1,0e6;+1,0e6]` rad/s | A | `(pitch,yaw,roll)` local |
| 8 | `radius` | `float32` | `[0;1,0e9]` wu | A | obligatoire, cohérent avec l'identité si présente |
| 9 | `physics_mode_flags` | `PhysicsModeFlags` | bits connus | A | modes courants |
| 10 | `desired_velocity_world` | `vec3f` | vitesse bornée | A | bit 0, prédiction/diagnostic |
| 11 | `desired_rotational_velocity_local` | `vec3f` | borne rotvel | A | bit 1 |
| 12 | `previous_ramp_velocity_local` | `vec3f` | vitesse bornée | A | bit 2 |
| 13 | `max_velocity_local` | `vec3f` | composantes `[0;1,0e9]` | A | bit 3 |
| 14 | `afterburner_max_velocity_local` | `vec3f` | composantes `[0;1,0e9]` | A | bit 3 |
| 15 | `booster_max_velocity_local` | `vec3f` | composantes `[0;1,0e9]` | A | bit 3 |
| 16 | `max_rotational_velocity_local` | `vec3f` | composantes `[0;1,0e6]` rad/s | A | bit 4 |
| 17 | `max_rear_velocity` | `float32` | `[0;1,0e9]` wu/s | A | bit 5 |
| 18 | `glide_cap` | `float32` | `[0;1,0e9]` wu/s | A | bit 6 |
| 19 | `current_glide_cap` | `float32` | `[0;1,0e9]` wu/s | A | bit 6 |
| 20 | `gravity_multiplier` | `float32` | `[−1024;+1024]` | A | bit 7, sans dimension |
| 21 | `time_constants` | `FlightTimeConstantsV1` | six valeurs `[0;3600]` s | A | bit 8, même ordre que `ClassMotionV1` |
| 22 | `rotdamp_s` | `float32` | `[0;3600]` s | A | bit 9 ; sémantique moteur différente joueur/IA documentée, jamais une accélération |
| 23 | `side_slip_time_const_s` | `float32` | `[0;3600]` s | A | bit 10 |
| 24 | `linear_thrust` | `vec3f` | chaque composante `[-1;1]` | A | bit 11, cosmétique uniquement |
| 25 | `rotational_thrust` | `vec3f` | chaque composante `[-1;1]` | A | bit 11, cosmétique uniquement |

`FlightTimeConstantsV1` contient, dans l'ordre, `forward_accel`, `afterburner_forward_accel`, `booster_forward_accel`, `forward_decel`, `slide_accel`, `slide_decel`, tous `float32` en secondes. `speed`, `fspeed`, accélération par différence, matrice, Euler, axes et flight-path marker sont `D` et absents.

### 6.4 `CONTROL_STATE` — type 8

Scope entité pilotée. Nature `A`. Atome `entity_id`. Le record est absent du snapshot si `StateDomainCoverage.CONTROL_INPUTS` n'est pas annoncé ; son absence ne rend pas la physique incomplète.

Presence bits : `CRUISE=0`, `REQUEST_COUNTERS=1`, `FLIGHT_CURSOR=2`; 3–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–2 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | tick source |
| 4 | `pitch` | `float32` | `[-1;1]` | A | entrée normalisée |
| 5 | `heading` | `float32` | `[-1;1]` | A | yaw |
| 6 | `bank` | `float32` | `[-1;1]` | A | roll |
| 7 | `vertical` | `float32` | `[-1;1]` | A | translation verticale |
| 8 | `sideways` | `float32` | `[-1;1]` | A | translation latérale |
| 9 | `forward` | `float32` | `[-1;1]` | A | commande avant/arrière |
| 10 | `control_mode` | `ControlMode` | enum fermé | A | mode courant |
| 11 | `control_flags` | `ControlFlags` | bits connus | A | aides et liens |
| 12 | `forward_cruise_percent` | `float32` | `[-100;100]` % | A | bit 0 |
| 13 | `fire_primary_count` | `u16` | `[0;65 535]` | A | bit 1, demandes du tick, pas tirs créés |
| 14 | `fire_secondary_count` | `u16` | `[0;65 535]` | A | bit 1 |
| 15 | `fire_countermeasure_count` | `u16` | `[0;65 535]` | A | bit 1 |
| 16 | `cursor_pitch_rad` | `float32` | `[-π;π]` | A | bit 2 |
| 17 | `cursor_yaw_rad` | `float32` | `[-π;π]` | A | bit 2 |
| 18 | `cursor_sensitivity` | `float32` | `[0;1]` | A | bit 2 |
| 19 | `cursor_deadzone` | `float32` | `[0;1]` | A | bit 2 |

Les fronts `afterburner_start/stop` sont des `EVENTS` si la couverture `CONTROL` est annoncée exacte. Les tirs réels ne sont jamais déduits des compteurs.
Un compteur source supérieur à 65 535 déclenche `SOURCE_LIMIT_EXCEEDED` et le refus de `CONTROL_INPUTS` avant session ; il n'est ni saturé ni tronqué sur le fil.

### 6.5 `DAMAGE_STATE` — type 9

Scope entité destructible. Nature `A`. Atome `entity_id`. Les labels « endommagé » et « critique » sont dérivés ; seuls les flags de cycle de vie font foi pour `disabled`, `dying` et détruit.

Presence bits : `SIM_HULL=0`, `ARMOR=1`, `GUARDIAN=2`, `CUMULATIVE_DAMAGE=3`, `LAST_DAMAGE=4`, `CONTRIBUTORS=5`; 6–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–5 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `hull_strength` | `float32` | `[0;dynamic_max_hull]` HP | A | HP courants |
| 5 | `dynamic_max_hull` | `float32` | `(0;1,0e12]` HP | A | maximum effectif, pas seulement catalogue |
| 6 | `protection_flags` | `ProtectionFlags` | bits connus | A | invulnérable/protégé/guardian |
| 7 | `sim_hull_strength` | `float32` | `[0;dynamic_max_hull]` HP | A | bit 0 |
| 8 | `armor_id` | `u32` | non nul | A | bit 1, registre d'armor de la mission |
| 9 | `guardian_threshold` | `float32` | `[0;dynamic_max_hull]` HP | A | bit 2 |
| 10 | `cumulative_damage` | `float32` | `[0;1,0e12]` HP | A | bit 3 |
| 11 | `last_damage_source_entity_id` | `entity_id` | `0` inconnu autorisé | A | bit 4 |
| 12 | `last_damage_weapon_class_id` | `u32` | `0` inconnu autorisé | A | bit 4 |
| 13 | `contributors` | `list<DamageContributorV1,64>` | clés uniques | A | bit 5, provenance cumulée bornée |

`DamageContributorV1` : `source_entity_id:u64` (`0` source non-entité), `weapon_class_id:u32` (`0` inconnu), `reserved:u32=0`, `accumulated_damage:float32 [0;1,0e12]`. Si plus de 64 contributeurs existent, ce champ conditionnel est omis entièrement et `cumulative_damage` reste la source complète ; aucune liste tronquée n'est permise.

### 6.6 `SHIELD_STATE` — type 10

Scope entité. Nature `A`. Atome `entity_id`, tableau de segments complet. `DELETE` est interdit : l'absence de bouclier s'encode par `has_shields=0`.

Presence bits : `RECHARGE_MAX=0`, `REGEN_RATE=1`, `DEFERRED_TRANSFER=2`; 3–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–2 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `has_shields` | `bool8` | 0/1 | A | source explicite |
| 5 | `segment_count` | `u16` | `[0;64]` | A | dynamique ; aucune hypothèse de quatre quadrants |
| 6 | `reserved` | `u16` | `0` | A | obligatoire |
| 7 | `segment_current_hits` | `float32[segment_count]` | chaque `[0;segment_max_hits[i]]` HP | A | tableau complet |
| 8 | `segment_max_hits` | `float32[segment_count]` | chaque `(0;1,0e12]` HP | A | même cardinalité |
| 9 | `recharge_max` | `float32` | `[0;1,0e12]` | A | bit 0 |
| 10 | `regeneration_per_s` | `float32` | `[0;1,0e12]` HP/s | A | bit 1 |
| 11 | `deferred_energy_transfer` | `float32` | `[−1,0e12;+1,0e12]` | A | bit 2, signe selon direction moteur normalisée |

Si `has_shields=0`, `segment_count=0`, les deux tableaux sont vides et les bits 0–2 sont nuls. Totaux et ratios sont `D`. Positions/quadrants d'impact appartiennent aux événements, pas à cet état.

## 7. Systèmes du vaisseau

### 7.1 `SUBSYSTEM_STATE` — type 11

Scope sous-système d'un vaisseau. Nature `A`, avec géométrie et identité de base référencées depuis `CLASS_MANIFEST`. Atome `(entity_id,subsystem_id)`. `CREATE/DELETE` autorisés pour les sous-systèmes dynamiques ; un `DELETE` contient les deux IDs.

Presence bits : `NAME_OVERRIDES=0`, `ARMOR=1`, `PERTURBATION=2`, `ANIMATED_TRANSFORM=3`, `ANIMATIONS=4`, `LOCAL_CARGO=5`, `TYPE_AGGREGATE=6`, `TURRET=7`; 8–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | parent vaisseau | A | première partie de clé |
| 2 | `subsystem_id` | `subsystem_id` | non nul | A | seconde partie de clé |
| 3 | `presence` | `u64` | bits 0–7 | A | obligatoire hors `DELETE` |
| 4 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 5 | `canonical_index` | `u16` | `[0;1023]` | A | index public stable dans le parent, pas `ship_subsys*` |
| 6 | `type` | `SubsystemType` | enum fermé | A | obligatoire |
| 7 | `current_hits` | `float32` | `[0;max_hits]` HP | A | obligatoire |
| 8 | `max_hits` | `float32` | `[0;1,0e12]` HP | A | maximum dynamique ; si zéro, current doit être zéro |
| 9 | `subsystem_flags` | `SubsystemFlags` | bits connus | A | perturbation, visibilité et mouvements |
| 10 | `internal_name_override` | `str<255>` | 1–255 | A | bit 0 |
| 11 | `alternate_name_override` | `str<255>` | 0–255 | A | bit 0 |
| 12 | `hud_name_override` | `str<255>` | 0–255 | A | bit 0 |
| 13 | `armor_id` | `u32` | non nul | A | bit 1 |
| 14 | `perturbation_remaining_us` | `duration_us` | max 24 h | A | bit 2 ; requis si `PERTURBED`, interdit sinon |
| 15 | `animated_translation_local` | `vec3f` | position locale bornée | A | bit 3 |
| 16 | `animated_orientation_local` | `quatf` | canonique | A | bit 3 |
| 17 | `animations` | `vlist<SubsystemAnimationV1,64>` | `animation_id` unique | A | bit 4 |
| 18 | `cargo_disclosure` | `DisclosureState` | enum fermé | A | bit 5 |
| 19 | `cargo_text` | `str<511>` | 0–511 | A | bit 5, longueur zéro obligatoire si `HIDDEN` |
| 20 | `aggregate_current_hits` | `float32` | `[0;1,0e12]` HP | A | bit 6, décision moteur par type |
| 21 | `aggregate_max_hits` | `float32` | `[0;1,0e12]` HP | A | bit 6 ; si zéro, current doit être zéro |
| 22 | `turret` | `TurretStateV1` | structure bornée | A | bit 7, autorisé seulement si `type=TURRET` |

`SubsystemAnimationV1` contient : `item_presence:u16` (`ANGLE=bit0`, `TRANSLATION=bit1`, `TARGET=bit2`; autres bits nuls), `animation_id:u16`, `state:AnimationState`, `reserved:u8=0`; si `ANGLE`, `angle_rad:float32` puis `angular_velocity_rad_s:float32`; si `TRANSLATION`, `translation:float32` wu puis `translation_velocity:float32` wu/s ; si `TARGET`, `target_angle_rad:float32` seulement avec `ANGLE`, puis `target_translation:float32` seulement avec `TRANSLATION`; enfin `remaining_us:duration_us`. Au moins `ANGLE` ou `TRANSLATION` est requis et `TARGET` requiert l'un d'eux. Les valeurs angulaires sont dans `[-1,0e6;+1,0e6]`, les translations dans `[-1,0e9;+1,0e9]`.

`TurretStateV1` utilise `turret_presence:u32` avec : `TARGET_SUBSYSTEM=bit0`, `AIM_POINT=bit1`, `NEXT_FIRE_POINT=bit2`, `COOLDOWN=bit3`, `TIME_IN_RANGE=bit4`, `OPTIMAL_RANGE=bit5`, `TARGET_PRIORITY=bit6`, `INACCURACY=bit7`, `RATE_MULTIPLIER=bit8`, `ANIMATION=bit9`, `BANKS=bit10`, `SWARM=bit11`, `AWACS=bit12`; bits 13–31 nuls. Les champs suivent cet ordre :

1. `turret_presence:u32`, `target_entity_id:u64` (`0` aucune), `target_subsystem_id:u32` non nul si bit 0 ; le bit 0 exige une cible non nulle ;
2. `current_direction_local:vec3f`, normalisée à `1 ± 1e-3` ;
3. `aim_point_world:vec3f`, `estimated_target_velocity_world:vec3f` si bit 1 ;
4. `next_fire_point_index:u16 [0;255]` et `reserved:u16=0` si bit 2 ;
5. `cooldown_remaining_us:duration_us`, `target_in_range_us:duration_us`, `optimal_range:float32 [0;1,0e12]` wu selon bits 3–5 ;
6. `target_priority:i16 [-32768;32767]` puis `reserved:u16=0` si bit 6 ;
7. `inaccuracy_rad:float32 [0;π]` si bit 7 ; `fire_rate_multiplier:float32 [0;1024]` si bit 8 ;
8. `animation_state:AnimationState`, `animation_remaining_us:duration_us` si bit 9 ;
9. `banks:vlist<TurretBankV1,64>` aux `bank_id` uniques si bit 10 ;
10. `swarm_remaining:u16 [0;4096]`, `swarm_bank_id:u32` non nul si bit 11 ;
11. `awacs_intensity:float32 [0;1,0e12]`, `awacs_radius:float32 [0;1,0e12]` wu si bit 12.

`TurretBankV1` : `item_presence:u16` (`AMMO=bit0`), `family:WeaponFamily` qui doit valoir `PRIMARY` ou `SECONDARY`, `reserved:u8=0`, `bank_index:u16 [0;63]`, `bank_id:u32` non nul, `weapon_class_id:u32` non nul, `current_ammo:u32 [0;1 000 000 000]` et `capacity_raw:float32 [0;1,0e12]` unités cargo si bit 0, puis `next_fire_remaining_us:duration_us`. Les tirs restent des événements `WEAPON_FIRED`.

Le ratio d'intégrité et `destroyed=(current_hits<=0)` sont `D`. Il n'existe aucun booléen filaire `subsystem_disabled`. La pose monde est dérivée de la pose parent, de la géométrie cataloguée et de la transformation animée.

### 7.2 `ENERGY_STATE` — type 12

Scope vaisseau. Nature `A`. Atome `entity_id`.

Presence bits : `WEAPON_ENERGY=0`, `REGENERATION=1`, `DEFERRED_TRANSFERS=2`, `ENGINE_RESULT=3`, `POWER_OUTPUT=4`, `ENGINE_INTEGRITY=5`; 6–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–5 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `ets_mode` | `EtsMode` | enum fermé | A | absent, disponible ou verrouillé |
| 5 | `ets_shields_index` | `u8` | `[0;12]` | A | `0` obligatoire si ETS absent |
| 6 | `ets_weapons_index` | `u8` | `[0;12]` | A | idem |
| 7 | `ets_engines_index` | `u8` | `[0;12]` | A | idem |
| 8 | `reserved` | `u8` | `0` | A | obligatoire |
| 9 | `weapon_energy_current` | `float32` | `[0;weapon_energy_max]` | A | bit 0 |
| 10 | `weapon_energy_max` | `float32` | `(0;1,0e12]` | A | bit 0 |
| 11 | `weapon_regeneration_per_s` | `float32` | `[0;1,0e12]` | A | bit 1 |
| 12 | `shield_regeneration_per_s` | `float32` | `[0;1,0e12]` | A | bit 1 |
| 13 | `deferred_to_weapons` | `float32` | `[−1,0e12;+1,0e12]` | A | bit 2 |
| 14 | `deferred_to_shields` | `float32` | `[−1,0e12;+1,0e12]` | A | bit 2 |
| 15 | `resulting_engine_power` | `float32` | `[0;1,0e12]` | A | bit 3 |
| 16 | `resulting_max_speed` | `float32` | `[0;1,0e9]` wu/s | A | bit 3, décision moteur |
| 17 | `power_output` | `float32` | `[0;1,0e12]` | A | bit 4 |
| 18 | `aggregate_engine_current_hits` | `float32` | `[0;aggregate_engine_max_hits]` HP | A | bit 5 |
| 19 | `aggregate_engine_max_hits` | `float32` | `(0;1,0e12]` HP | A | bit 5 |

Fractions ETS, ratios d'énergie et intégrité moteur normalisée sont `D`.

### 7.3 `PROPULSION_STATE` — type 13

Scope vaisseau. Nature `A`. Atome `entity_id`.

Presence bits : `FUEL=0`, `CONSUMPTION=1`, `ENGAGEMENT=2`, `DYNAMICS=3`, `ENGINE_WASH=4`, `RCS=5`; 6–63 réservés.

`PropulsionFlags:u16` fixe `AFTERBURNER_AVAILABLE=0x0001`, `AFTERBURNER_LOCKED=0x0002`, `AFTERBURNER_ACTIVE=0x0004`, `AFTERBURNER_REQUESTED=0x0008`, `BOOSTER_ACTIVE=0x0010`, `GLIDE_ACTIVE=0x0020`, `GLIDE_FORCED=0x0040`, `RCS_ACTIVE=0x0080`; autres bits nuls.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–5 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `propulsion_flags` | `u16` | `PropulsionFlags` | A | obligatoire |
| 5 | `reserved` | `u16` | `0` | A | obligatoire |
| 6 | `fuel_current` | `float32` | `[0;fuel_max]` | A | bit 0 |
| 7 | `fuel_max` | `float32` | `(0;1,0e12]` | A | bit 0 |
| 8 | `consumption_per_s` | `float32` | `[0;1,0e12]` | A | bit 1 |
| 9 | `recovery_per_s` | `float32` | `[0;1,0e12]` | A | bit 1 |
| 10 | `minimum_to_engage` | `float32` | `[0;fuel_max]` | A | bit 2 |
| 11 | `cooldown_remaining_us` | `duration_us` | max 1 h | A | bit 2 |
| 12 | `time_since_last_stop_us` | `duration_us` | max 24 h | A | bit 2 |
| 13 | `fuel_at_last_engagement` | `float32` | `[0;fuel_max]` | A | bit 2 |
| 14 | `forward_accel_time_const_s` | `float32` | `[0;3600]` s | A | bit 3 |
| 15 | `afterburner_max_velocity_local` | `vec3f` | composantes `[0;1,0e9]` wu/s | A | bit 3 |
| 16 | `engine_wash_intensity` | `float32` | `[0;1,0e12]` | A | bit 4 |
| 17 | `rcs_intensity` | `vec3f` | composantes `[−1;1]` | A | bit 5, activité cosmétique par axe |

Le ratio de carburant est `D`. Les flags recoupant `PhysicsModeFlags` DOIVENT être cohérents dans un même snapshot ; `PROPULSION_STATE` est la source pour les ressources, `FLIGHT_STATE` pour le mode physique.
Les bits `CONSUMPTION` et `ENGAGEMENT` exigent `FUEL`; `AFTERBURNER_ACTIVE` exige `AFTERBURNER_AVAILABLE` et interdit `AFTERBURNER_LOCKED`. Une classe sans réservoir omet ces trois groupes et encode leurs flags afterburner à zéro.

### 7.4 `WEAPON_STATE` — type 14

Scope vaisseau. Nature `A`. Atome `entity_id`, incluant l'ensemble global et les trois familles de banques. Les familles restent hétérogènes.

Presence bits : `PREVIOUS_PRIMARY=0`, `PREVIOUS_SECONDARY=1`, `TARGETING_LASER=2`, `SWARM=3`, `REMOTE_DETONATION=4`, `PER_BURST_ROTATION=5`, `TERTIARY=6`, `COUNTERMEASURE=7`; 8–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–7 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `primary_bank_count` | `u16` | `[0;64]` | A | doit égaler la liste primaire |
| 5 | `secondary_bank_count` | `u16` | `[0;64]` | A | doit égaler la liste secondaire |
| 6 | `tertiary_bank_count` | `u16` | `[0;64]` | A | nombre déclaré par le moteur ; seul l'état scalaire de la banque courante est exposé |
| 7 | `reserved` | `u16` | `0` | A | obligatoire |
| 8 | `current_primary_bank_id` | `u32` | `0` aucune | A | sélecteur global |
| 9 | `current_secondary_bank_id` | `u32` | `0` aucune | A | sélecteur global |
| 10 | `current_tertiary_bank_id` | `u32` | `0` aucune | A | sélecteur global |
| 11 | `weapon_flags` | `WeaponGlobalFlags` | bits connus | A | obligatoire |
| 12 | `previous_primary_bank_id` | `u32` | non nul | A | bit 0 |
| 13 | `previous_secondary_bank_id` | `u32` | non nul | A | bit 1 |
| 14 | `targeting_laser_bank_id` | `u32` | non nul | A | bit 2, requis si flag laser |
| 15 | `swarm_remaining` | `u16` | `[0;4096]` | A | bit 3 |
| 16 | `swarm_origin_bank_id` | `u32` | non nul | A | bit 3 |
| 17 | `remote_detonation_enable_remaining_us` | `duration_us` | max 1 h | A | bit 4 |
| 18 | `per_burst_rotation_rad` | `float32` | `[-1,0e9;+1,0e9]` rad | A | bit 5, beams type 5 |
| 19 | `primary_banks` | `vlist<PrimaryBankV1,64>` | IDs uniques | A | obligatoire |
| 20 | `secondary_banks` | `vlist<SecondaryBankV1,64>` | IDs uniques | A | obligatoire |
| 21 | `tertiary` | `TertiaryBankV1` | fixe | A | bit 6 requis si count>0, interdit si count=0 ; décrit la banque courante |
| 22 | `countermeasure` | `CountermeasureStateV1` | fixe | A | bit 7 |

`PrimaryBankV1` utilise `item_presence:u16` : `BALLISTIC_AMMO=bit0`, `REARM=bit1`, `BURST=bit2`, `SUBSTITUTION=bit3`, `ANIMATION=bit4`, `FOF_COOLDOWN=bit5`; bits 6–15 nuls. Ordre exact :

1. `item_presence:u16`, `bank_index:u16 [0;63]`, `bank_id:u32` non nul, `weapon_class_id:u32` non nul ;
2. `next_fire_remaining_us:duration_us`, `next_slot:u16 [0;255]`, `next_fire_point_index:u16 [0;255]`, `simultaneous_slots:u16 [1;256]`, `pattern_id:u16 [0;65 535]` ;
3. si bit 0 : `ammo_current:u32 [0;1 000 000 000]`, `ammo_initial:u32 [0;1 000 000 000]` avec courant inférieur ou égal à initial, `capacity_raw:float32 [0;1,0e12]` unités cargo ;
4. si bit 1 : `rearm_remaining_us:duration_us` ;
5. si bit 2 : `burst_counter:u16 [0;65 535]`, `reserved:u16=0`, `burst_seed:u32` ;
6. si bit 3 : `substitution_pattern_index:u16 [0;65 535]`, `reserved:u16=0` ;
7. si bit 4 : `animation_position:float32 [0;1]`, `animation_remaining_us:duration_us` ;
8. si bit 5 : `fof_cooldown_remaining_us:duration_us`.

Les bits 0 et 1 sont requis si et seulement si `WeaponClassFlags.BALLISTIC` est posé ; ils sont interdits sinon.

`SecondaryBankV1` utilise `item_presence:u16` : `AMMO=bit0`, `REARM=bit1`, `BURST=bit2`, `SUBSTITUTION=bit3`, `ANIMATION=bit4`; bits 5–15 nuls. Ordre exact :

1. `item_presence:u16`, `bank_index:u16 [0;63]`, `bank_id:u32` non nul, `weapon_class_id:u32` non nul ;
2. `next_fire_remaining_us:duration_us`, `next_slot:u16 [0;255]`, `reserved:u16=0` ;
3. si bit 0 : `ammo_current:u32 [0;1 000 000 000]`, `ammo_initial:u32 [0;1 000 000 000]` avec courant inférieur ou égal à initial, `capacity_raw:float32 [0;1,0e12]` unités cargo ;
4. si bit 1 : `rearm_remaining_us:duration_us` ;
5. si bit 2 : `burst_counter:u16 [0;65 535]`, `reserved:u16=0`, `burst_seed:u32` ;
6. si bit 3 : `substitution_pattern_index:u16 [0;65 535]`, `reserved:u16=0` ;
7. si bit 4 : `animation_position:float32 [0;1]`, `animation_remaining_us:duration_us`.

Les bits 0 et 1 sont requis sauf si `WeaponClassFlags.AMMOLESS` est posé ; ils sont alors interdits.

`TertiaryBankV1` contient seulement `bank_id:u32` non nul, `ammo_current:u32 [0;1 000 000 000]`, `ammo_initial:u32 [0;1 000 000 000]` avec courant inférieur ou égal à initial, `capacity_raw:float32 [0;1,0e12]`, `next_fire_remaining_us:duration_us` et `rearm_remaining_us:duration_us`, chacune bornée à une heure. Aucun ID de classe, banque précédente, slot, FOF, burst, substitution ou animation ne peut y être ajouté en v1.

`CountermeasureStateV1` : `item_presence:u16` (`CLASS=bit0`, autres bits nuls), `flags:u16` (`AVAILABLE=0x1`, `LOCKED=0x2`, autres bits nuls), `weapon_class_id:u32` non nul si bit 0, `quantity_current:u32 [0;1 000 000]`, `quantity_max:u32 [0;1 000 000]` avec courant inférieur ou égal au maximum, `cooldown_remaining_us:duration_us` bornée à une heure. `AVAILABLE` et `LOCKED` sont mutuellement exclusifs.

Capacité effectivement chargeable et ratios de munitions sont `D`. Les tirs, beams, détonations et lancements de contre-mesure sont des événements.

### 7.5 `LOCK_STATE` — type 15

Scope observateur. Nature `A`. Atome `entity_id` et ensemble complet des locks. `presence` n'a aucun bit v1 et DOIT valoir zéro.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | observateur | A | clé |
| 2 | `presence` | `u64` | `0` | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `locks` | `vlist<LockItemV1,64>` | cible/point uniques | A | liste complète, vide si aucun lock |

`LockItemV1` : `item_presence:u16` (`SUBSYSTEM=bit0`, `LOCK_ATTEMPT=bit1`), `locked:bool8`, `target_in_lock_cone:bool8`, `target_entity_id:u64` non nul, `subsystem_id:u32` si bit 0, `world_position:vec3f`, puis `time_to_lock_remaining_us:duration_us` si bit 1. L'absence du bit 1 signifie explicitement « aucune tentative » ; aucune sentinelle numérique n'est admise. Le type de point se déduit du bit `SUBSYSTEM`. Progression et validité de portée sont `D` depuis ce record et `WEAPON_MANIFEST`.

### 7.6 `TARGET_STATE` — type 16

Scope observateur. Nature `A`. Atome `entity_id`.

Presence bits : `PREVIOUS_TARGET=0`, `REVEALED_IDENTITY=1`, `TIME_ON_TARGET=2`, `TARGET_SUBSYSTEM=3`, `LOCK_SUBSYSTEM=4`, `LAST_STEALTH_OBSERVATION=5`, `DISTANCE_TREND=6`, `SPEED_TREND=7`, `IN_CONE=8`, `LEAD=9`, `ATTACKER=10`, `DANGEROUS_WEAPON=11`, `NEAREST_LOCKED=12`, `EXACT_HUD_DISTANCE=13`; 14–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | observateur | A | clé |
| 2 | `presence` | `u64` | bits 0–13 | A | si cible nulle, tous les bits doivent être nuls sauf `PREVIOUS_TARGET` |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `current_target_entity_id` | `u64` | `0` aucune cible | A | obligatoire |
| 5 | `previous_target_entity_id` | `entity_id` | non nul | A | bit 0 |
| 6 | `revealed_object_type` | `ObjectType` | enum fermé | A | bit 1 |
| 7 | `revealed_name` | `str<255>` | 0–255 | A | bit 1 |
| 8 | `revealed_class_id` | `u32` | `0` non révélé | A | bit 1 |
| 9 | `revealed_team_id` | `u32` | `[0;65535]` | A | bit 1 |
| 10 | `revealed_iff_id` | `u32` | `[0;65535]` | A | bit 1 |
| 11 | `time_on_target_us` | `duration_us` | max 24 h | A | bit 2 |
| 12 | `target_subsystem_id` | `subsystem_id` | non nul | A | bit 3 |
| 13 | `lock_subsystem_id` | `subsystem_id` | non nul | A | bit 4 |
| 14 | `last_known_position_world` | `vec3f` | position bornée | A | bit 5, observation furtive |
| 15 | `last_known_velocity_world` | `vec3f` | vitesse bornée | A | bit 5 |
| 16 | `distance_trend` | `ValueTrend` | enum fermé | A | bit 6, décision affichée du producteur |
| 17 | `speed_trend` | `ValueTrend` | enum fermé | A | bit 7 |
| 18 | `target_in_cone` | `bool8` | 0/1 | A | bit 8 |
| 19 | `lead_position_world` | `vec3f` | position bornée | A | bit 9, résultat complexe autoritaire |
| 20 | `lead_bank_id` | `bank_id` | non nul | A | bit 9 |
| 21 | `current_attacker_entity_id` | `entity_id` | non nul | A | bit 10 |
| 22 | `dangerous_weapon_entity_id` | `entity_id` | non nul | A | bit 11 |
| 23 | `nearest_locked_entity_id` | `entity_id` | non nul | A | bit 12 |
| 24 | `exact_hud_distance` | `float32` | `[0;1,0e12]` wu | A | bit 13, uniquement si les règles HUD ne sont pas reproductibles ; nom interdit pour une distance géométrique dérivable |

Distance centre-à-centre, vitesses relatives, intégrités, azimut, élévation, temps d'interception et orientation relative sont `D`. Coordonnées écran, triangles, pixels de lock et animations sont exclus. Un changement de cible peut aussi produire `EVENTS.TARGET_CHANGED` selon la couverture annoncée.

## 8. Capteurs, opérations et effets

### 8.1 `RADAR_STATE` — type 17

Scope observateur. Nature `A`. Atome `entity_id`. Le record est requis lorsque `StateDomainCoverage.RADAR_SENSORS` est annoncé.

Presence bits : `BRIGHT_RANGE=0`, `PRIMITIVE_RANGE=1`, `AWACS=2`, `EMP=3`, `JAMMING=4`, `VISIBILITY_TIMES=5`; 6–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | observateur | A | clé |
| 2 | `presence` | `u64` | bits 0–5 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `radar_mode` | `RadarMode` | enum fermé | A | mode sélectionné |
| 5 | `selected_range` | `float32` | `[0;1,0e12]` wu | A | portée effective ; `INFINITE` utilise la borne maximale, pas l'infini IEEE |
| 6 | `sensor_state` | `SensorState` | enum fermé | A | obligatoire |
| 7 | `sensor_current_hits` | `float32` | `[0;sensor_max_hits]` HP | A | agrégat autoritaire |
| 8 | `sensor_max_hits` | `float32` | `(0;1,0e12]` HP | A | agrégat autoritaire |
| 9 | `bright_range` | `float32` | `[0;1,0e12]` wu | A | bit 0 |
| 10 | `primitive_sensor_range` | `float32` | `[0;1,0e12]` wu | A | bit 1 |
| 11 | `awacs_intensity` | `float32` | `[0;1,0e12]` | A | bit 2 |
| 12 | `awacs_range` | `float32` | `[0;1,0e12]` wu | A | bit 2 |
| 13 | `emp_intensity` | `float32` | `[0;1,0e12]` | A | bit 3, effet capteur |
| 14 | `emp_remaining_us` | `duration_us` | max 24 h | A | bit 3 |
| 15 | `jamming_intensity` | `float32` | `[0;1,0e12]` | A | bit 4 |
| 16 | `distortion_intensity` | `float32` | `[0;1,0e12]` | A | bit 4 |
| 17 | `first_visibility_time_us` | `sample_time_us` | `<= producer_sample_time_us` | A | bit 5, première visibilité de la piste radar courante |
| 18 | `last_contact_time_us` | `sample_time_us` | entre première visibilité et sample | A | bit 5 |

Le ratio d'intégrité capteur est `D`. Le filtrage capteurs/AWACS/furtivité est appliqué avant création de `RADAR_CONTACTS` en mode `Cockpit`.

### 8.2 `RADAR_CONTACTS` — type 18

Scope contact d'un observateur. Nature `A`. Atome `(entity_id,contact_entity_id)`. `CREATE/DELETE` autorisés. Un `DELETE` contient les deux IDs ; sa perte est réparée par le prochain snapshot.

Presence bits : `ICON_SIZE=0`, `REVEALED_NAME=1`, `REVEALED_CLASS=2`, `REVEALED_TEAM_IFF=3`, `DETECTION_TIMES=4`, `CONFIDENCE=5`; 6–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | observateur | A | première partie de clé |
| 2 | `contact_entity_id` | `entity_id` | contact | A | seconde partie de clé ; stable dans la session |
| 3 | `presence` | `u64` | bits 0–5 | A | obligatoire hors `DELETE` |
| 4 | `producer_sample_time_us` | `sample_time_us` | monotone | A | instant d'observation |
| 5 | `object_type` | `ObjectType` | enum fermé | A | type révélé au niveau autorisé |
| 6 | `category` | `RadarCategory` | enum fermé | A | catégorie radar |
| 7 | `visibility` | `RadarVisibility` | enum fermé | A | état de la piste capteur |
| 8 | `position_world` | `vec3f` | position bornée | A | **observation capteur** autorisée |
| 9 | `velocity_world` | `vec3f` | vitesse bornée | A | même sémantique d'observation |
| 10 | `radius` | `float32` | `[0;1,0e9]` wu | A | rayon révélé |
| 11 | `contact_flags` | `ContactFlags` | bits connus | A | bright, cible, stealth, bombe, menace, etc. |
| 12 | `icon_size` | `float32` | `[0;1,0e9]` | A | bit 0, taille logique et non pixels HUD |
| 13 | `revealed_name` | `str<255>` | 0–255 | A | bit 1 |
| 14 | `revealed_class_id` | `u32` | non nul | A | bit 2, classe vaisseau ou arme selon type |
| 15 | `revealed_team_id` | `u32` | `[0;65535]` | A | bit 3 |
| 16 | `revealed_iff_id` | `u32` | `[0;65535]` | A | bit 3 |
| 17 | `first_detected_time_us` | `sample_time_us` | `<= sample` | A | bit 4 |
| 18 | `last_detected_time_us` | `sample_time_us` | `<= sample` | A | bit 4 |
| 19 | `confidence` | `float32` | `[0;1]` | A | bit 5 |

En `Cockpit`, un record de contact NE DOIT PAS permettre de contourner une absence d'`ENTITY_LIFECYCLE` ou des champs d'identité masqués. `BOMB` n'est valide que pour `ObjectType.WEAPON` et une classe portant le flag correspondant. Position relative, distance, azimut, élévation et âge calculé à l'instant client sont `D`.

### 8.3 `THREAT_STATE` — type 19

Scope entité menacée. Nature `A`. Atome `entity_id` et ensemble complet des missiles entrants autorisés.

Presence bits : `NEAREST_ATTACKER=0`, `DANGEROUS_WEAPON=1`, `NEAREST_HOMING=2`; 3–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | entité menacée | A | clé |
| 2 | `presence` | `u64` | bits 0–2 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `threat_level` | `ThreatLevel` | enum fermé | A | aucune, dumbfire, tentative ou lock acquis |
| 5 | `nearest_attacker_entity_id` | `entity_id` | non nul | A | bit 0 |
| 6 | `dangerous_weapon_entity_id` | `entity_id` | non nul | A | bit 1 |
| 7 | `nearest_homing_entity_id` | `entity_id` | non nul | A | bit 2 |
| 8 | `incoming_missiles` | `vlist<IncomingMissileV1,256>` | IDs uniques | A | liste complète autorisée |

`IncomingMissileV1` utilise `item_presence:u16` (`HOMING_SUBSYSTEM=bit0`, autres bits nuls), `guidance_type:GuidanceType`, `radar_visibility:RadarVisibility`, `missile_entity_id:u64` non nul, `weapon_class_id:u32` non nul, `target_entity_id:u64` non nul et égal à l'`entity_id` parent, `homing_subsystem_id:u32` non nul si bit 0, `position_world:vec3f`, `orientation_local_to_world:quatf`, `velocity_world:vec3f`. Si plus de 256 missiles ciblent l'entité, `StateDomainCoverage.RADAR_SENSORS` ne peut être déclaré complet pour cette session ; aucune troncature silencieuse.

Distance, relèvement, closing speed et temps d'impact sont `D`, y compris pour le missile le plus proche.

### 8.4 `CARGO_SCAN_STATE` — type 20

Scope observateur. Nature `A`. Atome `entity_id`.

Presence bits : `TARGET=0`, `SUBSYSTEM=1`, `TIMING=2`, `VALIDITY=3`, `CARGO_TEXT=4`; 5–63 réservés.

`ScanValidityFlags:u8` fixe `IN_RANGE=0x01`, `IN_ANGLE=0x02`, `LINE_OF_SIGHT=0x04`; autres bits nuls.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | observateur | A | clé |
| 2 | `presence` | `u64` | bits 0–4 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `scan_phase` | `ScanPhase` | enum fermé | A | décision moteur |
| 5 | `disclosure` | `DisclosureState` | enum fermé | A | indépendante de la phase |
| 6 | `target_entity_id` | `entity_id` | non nul | A | bit 0, requis en `SCANNING` ou `COMPLETED` si la cible existe encore |
| 7 | `target_subsystem_id` | `subsystem_id` | non nul | A | bit 1, requiert bit 0 |
| 8 | `elapsed_us` | `duration_us` | max 24 h | A | bit 2 |
| 9 | `required_us` | `duration_us` | `(0;24 h]` | A | bit 2 |
| 10 | `validity_flags` | `ScanValidityFlags` | bits connus | A | bit 3, décisions qui conditionnent la phase |
| 11 | `cargo_text` | `str<511>` | 1–511 | A | bit 4, requis si et seulement si `disclosure=REVEALED` |

`COMPLETED+HIDDEN` est valide, notamment avec `No_scanned_cargo`. Le texte est alors absent, jamais chaîne vide porteuse d'un secret. La progression normalisée est `D`.

### 8.5 `DOCKING_STATE` — type 21

Scope entité dockable. Nature `A`. Atome `entity_id` et topologie complète de ses relations. `presence` n'a aucun bit v1 et vaut zéro.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | entité locale | A | clé |
| 2 | `presence` | `u64` | `0` | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `phase` | `DockingPhase` | enum fermé | A | issue de la topologie et des modes IA |
| 5 | `group_leader_entity_id` | `u64` | `0` aucun | A | leader du groupe docké |
| 6 | `relations` | `vlist<DockingRelationV1,64>` | clé distante+dockpoints unique | A | topologie complète |

`DockingRelationV1` : `remote_entity_id:u64` non nul, `local_dockpoint_index:u16 [0;4095]`, `remote_dockpoint_index:u16 [0;4095]`, `local_dockpoint_name:str<127>`, `remote_dockpoint_name:str<127>`. La relation inverse, si publiée pour l'autre entité, DOIT être cohérente. Le moteur ne possède pas de progression scalaire canonique : progression et ETA sont `D` et absentes.

### 8.6 `SUPPORT_STATE` — type 22

Scope entité assistée. Nature `A`. Atome `entity_id`.

Presence bits : `SUPPORT_ENTITY=0`; 1–63 réservés. `SupportFlags:u8` fixe `AWAITING_REPAIR=0x01`, `BEING_REPAIRED=0x02`, `REPAIRING_OTHER=0x04`; les autres bits sont nuls.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | entité assistée | A | clé |
| 2 | `presence` | `u64` | bit 0 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `phase` | `SupportPhase` | enum fermé | A | obligatoire |
| 5 | `support_flags` | `u8` | `SupportFlags` | A | obligatoire |
| 6 | `reserved` | `bytes[3]` | zéro | A | obligatoire |
| 7 | `support_entity_id` | `entity_id` | non nul | A | bit 0 ; requis dès qu'un support concret est assigné |

ETA d'approche/docking/réarmement et progressions de réparation/munitions sont `D` depuis la phase et les autres états.

### 8.7 `NAVIGATION_STATE` — type 23

Scope joueur observé. Nature `A`. Atome `entity_id`, liste autorisée complète. Le record est requis lorsque `StateDomainCoverage.NAVIGATION` est annoncé.

Presence bits : `CURRENT_NAVPOINT=0`, `AUTOPILOT_REFUSAL=1`, `WAYPOINT_ROUTE=2`; 3–63 réservés.

`NavPointFlags:u8` fixe `HIDDEN=0x01`, `NO_ACCESS=0x02`, `VISITED=0x04`; les autres bits sont nuls. Un point `HIDDEN` peut être envoyé seulement s'il est néanmoins autorisé par les règles de mission pour cet observateur.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | joueur observé | A | clé |
| 2 | `presence` | `u64` | bits 0–2 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `autopilot_state` | `AutopilotState` | enum fermé | A | engagé, disponible, refusé ou non |
| 5 | `navpoints` | `vlist<NavPointV1,1024>` | `navpoint_id` unique | A | seulement points autorisés |
| 6 | `current_navpoint_id` | `u32` | non nul | A | bit 0, doit exister dans la liste |
| 7 | `autopilot_refusal` | `AutopilotRefusal` | enum fermé, pas `NONE` | A | bit 1, requis si état `REFUSED` |
| 8 | `route` | `list<WaypointV1,2048>` | ordre de parcours | A | bit 2 |
| 9 | `current_route_index` | `u16` | `< route.count` | A | bit 2 |
| 10 | `route_speed_limit` | `float32` | `[0;1,0e9]` wu/s | A | bit 2 |

`NavPointV1` : `item_presence:u16` (`ENTITY_LINK=bit0`, `WAYPOINT_LINK=bit1`, autres bits nuls), `type:NavPointType`, `flags:NavPointFlags`, `navpoint_id:u32` non nul, `name:str<255>` de 1 à 255 octets, `position_world:vec3f`, puis `linked_entity_id:u64` non nul si bit 0 ou `waypoint_list_id:u32` non nul et `waypoint_index:u16 [0;65 535]` si bit 1. Les deux bits sont mutuellement exclusifs et doivent correspondre au type.

`WaypointV1` : `waypoint_list_id:u32` non nul, `waypoint_index:u16 [0;65 535]`, `reserved:u16=0`, `position_world:vec3f`. Distance, relèvement et ETA sont `D`.

### 8.8 `EFFECT_STATE` — type 24

Scope entité. Nature `A` pour les paramètres visuels courants. Atome `entity_id`. Le record est absent si `StateDomainCoverage.LOW_FREQUENCY_EFFECTS` n'est pas annoncé.

Presence bits : `EMP_VISUAL=0`, `TAGS=1`, `RCS=2`, `BAY_DOORS=3`, `GLOW_BANKS=4`, `THRUSTERS=5`, `TEAM_COLORS=6`, `AUTOAIM=7`, `TARGETING_LASER_VISUAL=8`; 9–63 réservés.

| Ordre | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---|---|---|---|
| 1 | `entity_id` | `entity_id` | non nul | A | clé |
| 2 | `presence` | `u64` | bits 0–8 | A | obligatoire |
| 3 | `producer_sample_time_us` | `sample_time_us` | monotone | A | obligatoire |
| 4 | `effect_flags` | `EffectFlags` | bits connus | A | états cosmétiques discrets |
| 5 | `emp_visual_intensity` | `float32` | `[0;1]` | A | bit 0, distinct de la décision capteur de `RADAR_STATE` |
| 6 | `emp_visual_remaining_us` | `duration_us` | max 24 h | A | bit 0 |
| 7 | `tags` | `list<TagEffectV1,64>` | `tag_id` unique | A | bit 1 |
| 8 | `rcs_intensity` | `vec3f` | composantes `[-1;1]` | A | bit 2 |
| 9 | `bay_doors` | `list<ScalarVisualV1,64>` | IDs uniques | A | bit 3, valeur `[0;1]` |
| 10 | `glow_banks` | `list<ScalarVisualV1,256>` | IDs uniques | A | bit 4, intensité `[0;1]` |
| 11 | `thruster_intensities` | `float32[6]` | chacune `[0;1]` | A | bit 5, ordre avant/arrière/gauche/droite/haut/bas |
| 12 | `team_primary_color` | `rgba8` | 4 octets | A | bit 6 |
| 13 | `team_secondary_color` | `rgba8` | 4 octets | A | bit 6 |
| 14 | `autoaim_fov_rad` | `float32` | `[0;π]` | A | bit 7 |
| 15 | `targeting_laser_intensity` | `float32` | `[0;1]` | A | bit 8, requiert le flag correspondant |

`TagEffectV1` : `tag_id:u32` non nul, `remaining_us:duration_us`, `intensity:float32 [0;1,0e12]`. `ScalarVisualV1` : `visual_id:u16 [1;65 535]`, `reserved:u16=0`, `value:float32 [0;1]`.

Les transformations de sous-modèles utiles à la 3D restent dans `SUBSYSTEM_STATE`; elles ne sont pas dupliquées ici. Les flags cloak/stealth DOIVENT être cohérents avec `SHIP_IDENTITY`, mais `SHIP_IDENTITY` demeure la source pour la visibilité de sécurité. Les handles, textures, shaders, frames locales, fades et caches de rendu sont exclus.

## 9. Vue de communication et événements

### 9.1 `COMM_ASSET_MANIFEST` — type 25

Scope global, nature `C`. Ce record n'est jamais porté par un `DELTA` : un bundle immuable est envoyé comme transaction fiable de chunks ordonnés, validé par son SHA-256 puis installé atomiquement. L'atome de chunk est `(bundle_hash,first_asset_index,entry_count)` ; l'atome sémantique de catalogue est `(bundle_hash,asset_id)`. `record_flags` DOIT valoir zéro. Le layout byte-à-byte et les chunks sont placés sous l'autorité de la [spec 05](05-capabilities-et-vues-specialisees.md) ; les champs métier ci-dessous sont obligatoires dans cet ordre logique et la spec 05 NE DOIT PAS en omettre.

`ManifestFlags:u16` réserve `FRAME_ASSET=0x0001` et `PLACEHOLDER_ASSET=0x0002`. `AlphaMode:u8` vaut `NONE=0`, `STRAIGHT=1`, `PREMULTIPLIED=2`. `AssetTimingMode:u8` vaut `FORMAT_INTRINSIC=0`, `CONSTANT=1`, `PER_FRAME=2`. Les autres valeurs/bits sont invalides.

Le préfixe fixe du record mesure 64 octets :

| Offset | Champ | Wire | Cardinalité/borne | Nature | Présence et sémantique |
|---:|---|---:|---|---|---|
| 0 | `bundle_version` | `u16` | exactement `1` | C | version fonctionnelle du bundle v1 |
| 2 | `manifest_flags` | `ManifestFlags` | bits 0–1 | C | optionalité explicite des deux assets globaux |
| 4 | `bundle_hash` | `bytes[32]` | SHA-256 | C | hash du manifeste canonique complet |
| 36 | `converter_id_length` | `u8` | `[1;63]` | C | longueur du champ variable |
| 37 | `converter_version_length` | `u8` | `[1;63]` | C | longueur du champ variable |
| 38 | `frame_asset_id` | `u64` | `0` iff flag absent, non nul iff présent | C | habillage recommandé du gauge |
| 46 | `placeholder_asset_id` | `u64` | `0` iff flag absent, non nul iff présent | C | remplacement recommandé |
| 54 | `total_asset_count` | `u32` | `[1;4096]` | C | total du bundle non vide |
| 58 | `first_asset_index` | `u32` | `< total_asset_count` | C | premier index du chunk |
| 62 | `entry_count` | `u16` | `[1;4096]`, plage dans total | C | nombre d'entrées du chunk |
| 64 | `converter_id` | `UTF-8[converter_id_length]` | 1–63, sans NUL | C | `identity` si fichier livré identique à la source |
| variable | `converter_version` | `UTF-8[converter_version_length]` | 1–63, sans NUL | C | version reproductible du packager/convertisseur |
| variable | `entries` | `AssetEntryV1[entry_count]` | ordre canonique | C | suit immédiatement les deux chaînes |

Chaque `AssetEntryV1` possède un préfixe fixe de 72 octets :

| Offset relatif | Champ | Wire | Cardinalité/borne | Nature | Sémantique |
|---:|---|---:|---|---|---|
| 0 | `entry_length` | `u16` | `72 + name_len + path_len + 4*duration_count` | C | inclut son propre champ |
| 2 | `asset_id` | `u64` | non nul | C | huit premiers octets du SHA-256 interprétés big-endian, sérialisés little-endian |
| 10 | `content_hash` | `bytes[32]` | SHA-256 | C | hash complet du fichier livré |
| 42 | `source_format` | `SourceFormat` | enum fermé, non `INVALID` | C | format moteur/source |
| 43 | `delivered_format` | `DeliveredFormat` | enum fermé, non `INVALID` | C | format présent dans le bundle |
| 44 | `alpha_mode` | `AlphaMode` | enum fermé | C | absence, alpha droit ou prémultiplié |
| 45 | `timing_mode` | `AssetTimingMode` | enum fermé | C | timing intrinsèque, constant ou par frame |
| 46 | `reserved` | `u16` | `0` | C | obligatoire |
| 48 | `width` | `u16` | `[1;4096]` pixels | C | largeur logique |
| 50 | `height` | `u16` | `[1;4096]` pixels | C | hauteur logique |
| 52 | `frame_count` | `u32` | `[1;65535]` | C | nombre d'images |
| 56 | `duration_us` | `u64` | `[1;86 400 000 000]` µs | C | durée totale |
| 64 | `logical_name_length` | `u16` | `[1;255]` | C | longueur UTF-8 |
| 66 | `file_path_length` | `u16` | `[1;1024]` | C | longueur UTF-8 |
| 68 | `frame_duration_count` | `u32` | `0`, `1` ou `frame_count` | C | dicté par `timing_mode` |
| 72 | `logical_name` | `UTF-8[logical_name_length]` | sans NUL | C | nom logique, jamais identité réseau |
| variable | `relative_path` | `ASCII[file_path_length]` | chemin relatif portable | C | grammaire ASCII, mots réservés et unicité après casefold définis en spec 05 |
| variable | `frame_durations_us` | `u32[frame_duration_count]` | valeurs strictement positives | C | tableau brut little-endian, sans préfixe `list<>` |

`FORMAT_INTRINSIC` impose un count nul et des timings internes dont la somme vaut `duration_us`. `CONSTANT` impose un count de 1 et `frame_count * frame_durations_us[0] = duration_us`. `PER_FRAME` impose un count égal à `frame_count` et une somme exacte égale à `duration_us`. Les calculs sont en `u64` sans overflow.

Pour `DeliveredFormat.EFF`, le descripteur ASCII canonique, la convention de nommage des frames PNG, `PER_FRAME` et le calcul entier exact des durées sont ceux de la spec 05. Toute autre grammaire EFF, référence implicite ou entrée PNG manquante invalide atomiquement le bundle.

`entry_length` et le record complet restent dans 65 535 octets ; une entrée n'est jamais coupée entre chunks. Les plages de chunks sont contiguës, non recouvrantes et couvrent exactement le bundle, dont les entrées sont triées par `(asset_id,logical_name UTF-8 bytewise)`. Les fichiers ne transitent pas dans FSTL. Un bundle incompatible désactive seulement la vue de communication ; un fichier local absent ou de hash incorrect produit le placeholder et une métrique, jamais l'échec de la session d'état.

### 9.2 `COMM_VIEW_STATE` — type 26

Scope global du processus joueur, nature `A`, atome singleton. `record_flags=0`. Le payload v1 fixe mesure 60 octets et n'utilise pas de masque de présence.

| Offset | Champ | Wire | Borne | Nature | Règle |
|---:|---|---:|---|---|---|
| 0 | `active` | `bool8` | 0/1 | A | état courant |
| 1 | `playback_mode` | `CommPlaybackMode` | enum fermé | A | `ONCE` ou `LOOP` |
| 2 | `color_mode` | `CommColorMode` | enum fermé | A | teinte HUD ou pleine couleur |
| 3 | `stop_reason` | `CommStopReason` | enum fermé | A | `NONE` lorsque actif |
| 4 | `playback_id` | `u64` | non nul si actif | A | strictement croissant dans la session |
| 12 | `engine_message_id` | `u32` | `0` si corrélation absente | A | distinct du `message_id` de fragmentation |
| 16 | `sender_entity_id` | `u64` | `0` aucun émetteur | A | source du message |
| 24 | `head_asset_id` | `asset_id` | non nul si actif | C | asset finalement résolu après persona/aléatoire |
| 32 | `producer_sample_time_us` | `u64` | monotone si actif | A | base d'extrapolation |
| 40 | `animation_time_us` | `u64` | `[0;duration]` | A | offset réellement échantillonné |
| 48 | `duration_us` | `u64` | `(0;24 h]` si actif | C | validation contre manifeste |
| 56 | `playback_rate` | `float32` | `[-64;+64]` | A | `0` pause, négatif inverse, compression effective déjà incluse |

Si `active=0`, `playback_id`, `engine_message_id`, `sender_entity_id`, `head_asset_id`, les trois temps et `playback_rate` sont zéro ; mode et couleur valent leurs zéros canoniques. `stop_reason=NONE` est autorisé avant toute lecture de la session ; après un arrêt, il porte la raison la plus récente jusqu'à la prochaine lecture. Si `active=1`, `stop_reason=NONE`. Le client extrapole avec l'horloge de la spec 03, modulo euclidien pour `LOOP`, clamp pour `ONCE`. Une modification d'offset, taux, mode, couleur ou asset émet immédiatement ce record remplaçable ; les échantillons réguliers sont plafonnés à 10 Hz.

### 9.3 `COMM_VIEW_EVENT` — type 27

Scope global, nature `E`, un événement par record. `record_flags.CREATE` est obligatoire. Le payload fixe mesure 68 octets. Les `COMM_VIEW_EVENT` et `EVENTS` partagent la même séquence `event_id:u64`, strictement croissante dans la session.

Dans un `EventBatchPayload Reliable` contenant un ou plusieurs records type 27, les records sont triés par `event_id`, `first_event_id` égale le premier et le temps du préfixe est supérieur ou égal à chaque `producer_sample_time_us`.

| Offset | Champ | Wire | Borne | Nature | Règle |
|---:|---|---:|---|---|---|
| 0 | `event_id` | `u64` | non nul, strictement croissant | E | déduplication et ordre inter-familles |
| 8 | `event_kind` | `CommEventKind` | `START=1`, `STOP=2` | E | événement fiable |
| 9 | `playback_mode` | `CommPlaybackMode` | enum fermé | E | état pour START, `ONCE=0` pour STOP |
| 10 | `color_mode` | `CommColorMode` | enum fermé | E | état pour START, `HUD_TINT=0` pour STOP |
| 11 | `stop_reason` | `CommStopReason` | enum fermé | E | `NONE` pour START, non-`NONE` pour STOP |
| 12 | `playback_id` | `u64` | non nul | E | lecture concernée |
| 20 | `engine_message_id` | `u32` | `0` autorisé START, zéro STOP | E | corrélation moteur éventuelle |
| 24 | `sender_entity_id` | `u64` | `0` autorisé | E | émetteur START, zéro STOP |
| 32 | `head_asset_id` | `u64` | non nul START, zéro STOP | C | asset final |
| 40 | `producer_sample_time_us` | `u64` | monotone | E | instant de transition |
| 48 | `animation_time_us` | `u64` | `[0;duration]` START, zéro STOP | E | offset au démarrage |
| 56 | `duration_us` | `u64` | `(0;24 h]` START, zéro STOP | C | validation |
| 64 | `playback_rate` | `float32` | `[-64;+64]` START, zéro STOP | E | taux au démarrage |

Un `START` contient l'état complet de la lecture. Pour un `STOP`, seuls `event_id`, `kind`, `stop_reason`, `playback_id` et `producer_sample_time_us` sont non nuls ; mode, couleur, `engine_message_id`, émetteur, asset, temps d'animation, durée et taux sont leurs zéros canoniques. Un remplacement est `STOP(REPLACED)` suivi d'un `START` d'`event_id` supérieur. Les corrections scalaires ne créent pas un troisième type d'événement : elles utilisent `COMM_VIEW_STATE` immédiat. START/STOP sont fiables ; l'état courant dans les keyframes répare toujours une perte ou un join en cours.

### 9.4 `EVENTS` — type 28

Scope global, nature `E`. Un record contient un batch append-only de 1 à 256 événements, triés par `event_id`. `record_flags.CREATE` est obligatoire ; `DELETE` et `PARTIAL` sont interdits. Chaque item est un atome idempotent. Un même `event_id` reçu plusieurs fois doit avoir des octets identiques, sinon la transaction est rejetée.

Dans le conteneur `EventBatchPayload`, `first_event_id` égale l'ID du premier item du premier record et `producer_sample_time_us` est supérieur ou égal au temps de chaque item. Plusieurs records type 28 peuvent suivre ; leurs plages d'IDs sont strictement croissantes, disjointes et respectent la même `delivery_class`.

`EventKind:u16` :

| Valeur | Nom | Valeur | Nom |
|---:|---|---:|---|
| 1 | `ENTITY_APPEARED` | 18 | `SHIP_DYING_STARTED` |
| 2 | `ENTITY_DEPARTURE_STARTED` | 19 | `ENTITY_DESTROYED` |
| 3 | `ENTITY_DISAPPEARED` | 20 | `TARGET_CHANGED` |
| 4 | `WEAPON_FIRED` | 21 | `SCAN_COMPLETED` |
| 5 | `BEAM_STARTED` | 22 | `CARGO_REVEALED` |
| 6 | `BEAM_ENDED` | 23 | `DOCKING_STARTED` |
| 7 | `REMOTE_DETONATION` | 24 | `DOCKED` |
| 8 | `COUNTERMEASURE_LAUNCHED` | 25 | `UNDOCKING_STARTED` |
| 9 | `SHIELD_IMPACT` | 26 | `UNDOCKED` |
| 10 | `HULL_IMPACT` | 27 | `WARP_STARTED` |
| 11 | `SHIELD_SEGMENT_DEPLETED` | 28 | `WARP_ENDED` |
| 12 | `SHIELD_SEGMENT_RESTORED` | 29 | `MISSION_STARTED` |
| 13 | `SUBSYSTEM_DAMAGED` | 30 | `MISSION_ENDED` |
| 14 | `SUBSYSTEM_PERTURBED` | 31 | `SESSION_ENDED` |
| 15 | `SUBSYSTEM_RESTORED` | 32 | `AFTERBURNER_STARTED` |
| 16 | `SUBSYSTEM_DESTROYED` | 33 | `AFTERBURNER_STOPPED` |
| 17 | `SHIP_DISABLED` |  |  |

`EventFlags:u16` fixe `RECONSTRUCTIBLE=0x0001`, `EXACT_CAPTURE=0x0002`, `RELIABLE=0x0004`; les autres bits sont nuls. `EXACT_CAPTURE` ne peut être posé que si la famille correspondante est dans `SESSION_STATE.event_coverage_exact`. Les événements non reconstructibles (`WEAPON_FIRED`, beams, détonation, contre-mesure, impacts et transitions complètes entre deux captures) portent toujours `RELIABLE`; ils portent en plus `EXACT_CAPTURE` seulement lorsque leur famille est annoncée exacte.

Le payload est `events:vlist<EventItemV1,256>`. Chaque item utilise `item_presence:u32` : `ACTOR=bit0`, `TARGET=bit1`, `SUBSYSTEM=bit2`, `WEAPON_CLASS=bit3`, `BANK=bit4`, `FIRE_POINT=bit5`, `PROJECTILES=bit6`, `POSITION=bit7`, `NORMAL=bit8`, `AMOUNT=bit9`, `SHIELD_SEGMENT=bit10`, `BURST=bit11`, `CARGO_TEXT=bit12`, `REASON_CODE=bit13`, `PREVIOUS_TARGET=bit14`, `TRANSIT_MODE=bit15`; bits 16–31 nuls.

Ordre de `EventItemV1` :

1. `item_presence:u32`, `event_id:u64`, `producer_sample_time_us:u64`, `kind:EventKind`, `event_flags:EventFlags`, `subject_entity_id:u64` (`0` seulement pour mission/session) ;
2. `actor_entity_id:u64` non nul si bit 0 ;
3. `target_entity_id:u64` si bit 1 ; `0` est autorisé uniquement pour `TARGET_CHANGED` signifiant « cible effacée » ;
4. `subsystem_id:u32` non nul si bit 2 ; `weapon_class_id:u32` non nul si bit 3 ;
5. `weapon_family:WeaponFamily`, `reserved:u8=0`, `bank_index:u16 [0;63]`, `bank_id:u32` non nul si bit 4 ;
6. `fire_point_index:u16 [0;255]`, `reserved:u16=0` si bit 5 ;
7. `created_projectiles:list<entity_id,256>` non nuls et uniques si bit 6 ;
8. `position_world:vec3f` si bit 7 ; `normal_world:vec3f` normalisée si bit 8 ;
9. `amount:float32 [0;1,0e12]` si bit 9 ; `shield_segment_index:u16 [0;63]` et `reserved:u16=0` si bit 10 ;
10. `burst_counter:u16 [0;65 535]`, `reserved:u16=0`, `burst_seed:u32` si bit 11 ;
11. `cargo_text:str<511>` de 1 à 511 octets si bit 12 ; `reason_code:EventReasonCode` si bit 13 ;
12. `previous_target_entity_id:u64` (`0` autorisé) si bit 14 ;
13. `transit_mode:TransitMode` et `reserved:bytes[3]=0` si bit 15.

Règles de présence par kind :

| Kinds | Champs requis | Champs conditionnels autorisés |
|---|---|---|
| `ENTITY_APPEARED`, `ENTITY_DEPARTURE_STARTED`, `ENTITY_DISAPPEARED` | sujet non nul | `REASON_CODE` |
| `WEAPON_FIRED` | sujet=tireur, `WEAPON_CLASS`, `BANK`, `FIRE_POINT` | `TARGET`, `PROJECTILES`, `BURST` |
| `BEAM_STARTED`, `BEAM_ENDED` | sujet=tireur, `WEAPON_CLASS`, `BANK` | `TARGET`, `SUBSYSTEM`, `FIRE_POINT`, `POSITION` |
| `REMOTE_DETONATION` | sujet=propriétaire | `TARGET`, `PROJECTILES`, `POSITION` |
| `COUNTERMEASURE_LAUNCHED` | sujet=lanceur, `WEAPON_CLASS` | `POSITION`, `PROJECTILES` |
| `SHIELD_IMPACT` | sujet=victime, `POSITION`, `NORMAL`, `AMOUNT`, `SHIELD_SEGMENT` | `ACTOR`, `WEAPON_CLASS` |
| `HULL_IMPACT` | sujet=victime, `POSITION`, `NORMAL`, `AMOUNT` | `ACTOR`, `WEAPON_CLASS` |
| `SHIELD_SEGMENT_DEPLETED`, `SHIELD_SEGMENT_RESTORED` | sujet=victime, `SHIELD_SEGMENT` | `ACTOR`, `WEAPON_CLASS`, `POSITION` |
| `SUBSYSTEM_DAMAGED`, `SUBSYSTEM_PERTURBED`, `SUBSYSTEM_RESTORED`, `SUBSYSTEM_DESTROYED` | sujet=vaisseau, `SUBSYSTEM` | `ACTOR`, `WEAPON_CLASS`, `POSITION`, `AMOUNT` |
| `SHIP_DISABLED`, `SHIP_DYING_STARTED`, `ENTITY_DESTROYED` | sujet non nul | `ACTOR`, `WEAPON_CLASS`, `POSITION`, `REASON_CODE` |
| `TARGET_CHANGED` | sujet=observateur, `TARGET`, `PREVIOUS_TARGET` | aucun autre |
| `SCAN_COMPLETED` | sujet=observateur, `TARGET` | `SUBSYSTEM` |
| `CARGO_REVEALED` | sujet=observateur, `TARGET`, `CARGO_TEXT` | `SUBSYSTEM` |
| `DOCKING_STARTED`, `DOCKED`, `UNDOCKING_STARTED`, `UNDOCKED` | sujet et `TARGET` non nuls | `REASON_CODE` |
| `WARP_STARTED`, `WARP_ENDED` | sujet non nul, `TRANSIT_MODE` | `REASON_CODE` |
| `MISSION_STARTED`, `MISSION_ENDED`, `SESSION_ENDED` | sujet `0` | `REASON_CODE` |
| `AFTERBURNER_STARTED`, `AFTERBURNER_STOPPED` | sujet non nul | aucun autre |

Tout bit non autorisé par cette table rejette l'item. Les événements peuvent être répartis en plusieurs records/messages fiables ; leur ordre logique est donné uniquement par `event_id`, pas par l'ordre des datagrammes. Un événement reconstructible perdu est réparé par l'état `A` ; un client NE DOIT PAS conclure à l'absence d'une transition brève sans couverture exacte.

## 10. Règles de couverture, optionalité et autorité

### 10.1 Matrice de couverture d'un snapshot

La présence d'un domaine n'est jamais implicite. Le bitmap `SESSION_STATE.state_domain_coverage` fixe les obligations suivantes par rapport aux entités autorisées par le mode :

| Bit de domaine | Records requis dans chaque `FULL_SNAPSHOT` |
|---|---|
| `CORE_SHIP` | `SESSION_STATE`, `MISSION_STATE`, `ENTITY_LIFECYCLE`; génération `CLASS_MANIFEST` référencée déjà installée ; pour chaque vaisseau : `SHIP_IDENTITY`, `FLIGHT_STATE`, `DAMAGE_STATE`, `SHIELD_STATE`, tous ses `SUBSYSTEM_STATE`, `ENERGY_STATE`, `PROPULSION_STATE` |
| `CONTROL_INPUTS` | `CONTROL_STATE` pour le joueur observé ; son absence est autorisée seulement si aucun joueur observé n'existe |
| `PREDICTION` | autorise les bits 0–10 de `FLIGHT_STATE`; chaque bit est présent si et seulement si les champs moteur correspondants existent pour ce type d'entité |
| `RADAR_SENSORS` | `RADAR_STATE`, tous les `RADAR_CONTACTS` autorisés et `THREAT_STATE` pour le joueur observé |
| `LOW_FREQUENCY_EFFECTS` | `EFFECT_STATE` pour chaque entité possédant au moins un effet public ; autorise le bit 11 cosmétique de `FLIGHT_STATE` |
| `TARGETING` | `LOCK_STATE` et `TARGET_STATE` pour le joueur observé |
| `WEAPONS` | génération `WEAPON_MANIFEST` référencée déjà installée et `WEAPON_STATE` pour chaque vaisseau exporté |
| `CARGO_DOCK_SUPPORT` | `CARGO_SCAN_STATE` du joueur, `DOCKING_STATE` des entités concernées et `SUPPORT_STATE` des entités assistées |
| `NAVIGATION` | `NAVIGATION_STATE` du joueur observé |

Un bit déclaré signifie « complet dans le périmètre autorisé », pas « le moteur possède nécessairement chaque champ conditionnel ». L'absence d'un champ conditionnel suit exclusivement sa règle de présence. Les trois bitmaps `state_domain_coverage`, `event_coverage_state_derived` et `event_coverage_exact` sont immuables après `SESSION_BEGIN`. Un producteur qui ne peut respecter une cardinalité ou une taille retire le bit avant `WELCOME`; une perte ultérieure de couverture exige `SESSION_END` puis une nouvelle session, jamais un retrait silencieux dans un delta.

Les capabilities de la spec 05 sont réservées aux vues et mises à jour spécialisées ; elles ne remplacent ni `state_domain_coverage`, ni les deux bitmaps de couverture d'événements.

### 10.2 Règles d'autorité

- En `SOLO`, le processus local est source de vérité des états qu'il annonce.
- En `MULTIPLAYER_CLIENT`, le producteur publie la pose locale/prédite, les corrections reçues et les seules informations permises par ses capteurs.
- En `MULTIPLAYER_MASTER`, l'autorité serveur ne devient pas un flux produit : la télémétrie reste attachée au cockpit observé.
- Les vues `COMM_*` et cible H.264 appartiennent au processus du joueur observé. Un master headless n'annonce pas ces capabilities visuelles.
- `producer_id`, `session_id` et `observed_player_entity_id` permettent d'identifier les producteurs ; FSTL 1.0 ne fusionne pas automatiquement un état serveur et les vues d'un client. Un consommateur qui les associe doit utiliser une configuration externe explicite, jamais une égalité de `net_signature`.

### 10.3 Sécurité de visibilité

En mode `COCKPIT`, avant calcul du snapshot et du dirty-set, le producteur :

1. retire les entités et relations non connues ;
2. transforme la vérité d'un contact en observation capteur autorisée ;
3. retire noms, classes, équipe et IFF non révélés ;
4. retire le texte cargo tant que `DisclosureState.HIDDEN` ;
5. retire navpoints et waypoints non autorisés ;
6. retire les relations parent, docking, support, cible ou projectile qui révéleraient une entité cachée, ou remplace uniquement les références explicitement nullables par `0` ;
7. recalcule les comptes après filtrage ;
8. compare et sérialise seulement cette vue filtrée.

Le filtrage après sérialisation, les placeholders remplis de zéros et les IDs
internes stables d'objets cachés sont interdits. `RadarVisibility` et
`DisclosureState` représentent exclusivement ce que le cockpit observé sait.

### 10.4 Manifestes et snapshots paginés

Les manifestes requis sont installés et acquittés avant publication d'un snapshot qui les référence. Le préfixe de transaction de la spec 03 porte leurs générations/hashes. Une transaction de snapshot peut être paginée en plusieurs messages logiques de 1 Mio ; les records restent inchangés et aucun record n'est appliqué partiellement. Toutes les pages, tous les CRC et toutes les références sont validés avant publication atomique et `ACK APPLIED`.

Une mise à jour de catalogue crée une nouvelle génération immuable et exhaustive. Tous les records de catalogue `CLASS_MANIFEST`, `WEAPON_MANIFEST` et `COMM_ASSET_MANIFEST` portent `record_flags=0`; aucun n'utilise `CREATE`, `DELETE` ou `PARTIAL`. Une définition supprimée est simplement absente de la nouvelle génération complète. Un ID n'est jamais réinterprété dans une génération déjà installée. Les catalogues sont envoyés au début, lors d'une apparition qui introduit une nouvelle définition ou lors d'un changement de génération ; ils ne sont jamais répétés à chaque tick.

## 11. Données dérivées et données interdites

### 11.1 Valeurs `D` calculées côté client

Les valeurs suivantes sont absentes du canon v1 ; un canal de diagnostic futur devrait les marquer `D` et ne pourrait pas contredire leurs sources :

- matrice, axes droite/haut/avant, cap, tangage et roulis depuis le quaternion ;
- `speed=norm(velocity_world)`, `fspeed=dot(fvec,velocity_world)`, vitesse locale et accélération estimée ;
- pourcentage de vitesse et flight-path marker ;
- totaux et ratios de coque, boucliers, énergie, carburant, munitions et intégrité ;
- seuils visuels « endommagé » et « critique » ;
- `subsystem_destroyed=(current_hits<=0)`, ratio de sous-système et tout pseudo-flag `subsystem_disabled` ;
- pose monde d'un sous-système depuis parent, géométrie et animation ;
- nombre maximal de projectiles chargeables depuis capacité brute, `cargo_size` et règles de classe ;
- progression de lock depuis `time_to_lock_remaining_us` et `lock_time_us`, validité de portée et type de point ;
- distance géométrique, position relative, azimut, élévation, orientation relative et vitesses relatives ;
- vitesse de rapprochement, TTC, temps d'interception et temps d'impact missile ;
- âge courant d'un contact depuis `last_detected_time_us` ;
- coordonnées radar/HUD, triangles, brackets, pixels et accumulateurs de lock ;
- progression et ETA de scan, docking, support, réparation, réarmement et navigation ;
- interpolation entre états, animations de jauges, flashes et sons ;
- image Talking Head courante depuis asset, offset, horloge, taux et mode ;
- textes, brackets, couleurs IFF, dégâts et overlays du moniteur cible depuis `TARGET_STATE`, `DAMAGE_STATE` et `SUBSYSTEM_STATE`.

Les décisions difficiles ou sensibles restent `A` : visibilité/contact capteur, acquisition et durée de lock, lead complexe, distance HUD explicitement nommée, phase/validité/divulgation de scan et niveau de menace.

### 11.2 Données interdites

Ne sont jamais sérialisés dans les 28 records :

- pointeurs C++, adresses, layout brut, padding, vtables, listes chaînées et conteneurs moteur ;
- `objnum`, `instance`, `ship_info_index`, `ai_index` ou tout indice local sans clé publique stable ;
- handles audio, bitmap, texture, modèle, shader, renderer ou encodeur ;
- coordonnées écran dépendantes du HUD ;
- frames, fades et timers locaux d'animation, sauf l'offset canonique Talking Head ;
- caches de rendu, `physics_info::speed/fspeed`, champs `last_*` purement internes ;
- `TIMESTAMP` moteur brut : seule une durée restante ou un temps monotone explicitement défini est admis ;
- données statiques répétées à chaque tick ;
- audio Talking Head, fichiers d'assets, pixels, surfaces GPU, images du HUD ou access units H.264 dans un snapshot/delta ;
- commande pilote distante, ordre IA, modification de mission, abonnement capable de modifier la simulation ou toute entrée client autre que contrôle de session/vue défini par les specs 03/05.

### 11.3 Relation avec la vue cible H.264

`TARGET_STATE`, `DAMAGE_STATE`, `SUBSYSTEM_STATE` et les catalogues fournissent les overlays locaux. Le rendu 3D hors écran, ses `stream_id`, génération, profil `MfdHigh/HudExact`, codec, résolution, cadence, bitrate, GOP, PTS, IDR, discontinuités, statistiques et états `Starting/Live/Stale/Unsupported/Stopped` appartiennent aux messages spécialisés de la spec 05. Les pixels ne sont jamais un champ des 28 records. Une frame n'est présentée que si son `target_entity_id` correspond encore au `TARGET_STATE` courant.

## 12. Validation croisée et critères de conformité

Après que l'enveloppe de la spec 02 a identifié un des 28 types avec `record_version=1`, un décodeur valide son payload sans dépendre de FS2Open et rejette atomiquement la transaction si l'une des règles suivantes échoue. Un `RecordType` inconnu est sauté de manière bornée. Une `record_version` inconnue d'un type connu est également sautée, puis la transaction échoue si l'absence de la version v1 rend incomplet l'ensemble exigé par la matrice statique de la section 3.1, `state_domain_coverage`, le mode d'autorité/visibilité ou les capabilities actives. Aucun manifeste de schéma dynamique n'existe en v1. Une extension terminale n'est lue que si sa compatibilité est explicitement autorisée par la mineure négociée, jamais par simple supposition de préfixe.

1. payload v1 trop court ou de longueur incohérente, taille logique calculée supérieure à 65 535, flag interdit ou bit réservé non nul ;
2. la longueur réelle ne correspond pas exactement aux champs requis par le masque, à une `list` fixe ou à une `vlist` ;
3. un compte, index, chaîne, float, durée, ID ou somme de tailles sort de sa borne ;
4. un ID requis vaut zéro, une référence pointe vers une entité supprimée ou un manifeste/génération non installé ;
5. deux atomes de même clé dans une transaction complète se contredisent ; une répétition byte-identique est également interdite hors mécanisme explicite de déduplication transport ;
6. `ENTITY_LIFECYCLE.object_type` est incompatible avec le record ou le manifeste référencé ;
7. classe bombe/type arme, counts de banques/listes, segments courant/max, relations docking inverses ou flags redondants sont incohérents ;
8. une condition de présence (`REVEALED`, `PERTURBED`, lock attempt, active communication, etc.) n'est pas respectée ;
9. un record `Cockpit` expose une identité, un cargo, un navpoint ou une relation non autorisés ;
10. un événement emploie un champ non permis par son kind, n'est pas strictement ordonné dans son batch ou contredit un doublon déjà accepté ;
11. un asset possède chemin hostile, hash/ID incohérent, timing dont la somme diffère de `duration_us` ou chunk discontinu ;
12. un record prétend être complet alors que la source dépassait une borne et a été tronquée.

Le producteur applique les mêmes validations avant écriture. Il borne/clamp uniquement les entrées analogiques dont la table l'autorise explicitement (`CONTROL_STATE`); pour un état moteur hors borne, il signale un diagnostic et retire le domaine ou refuse la session plutôt que de falsifier la valeur.

Les fixtures obligatoires de cette annexe comprennent, pour chacun des 28 types : valeur minimale, valeur maximale, toutes les combinaisons de présence valides, chaque bit réservé, enum inconnu, chaîne UTF-8 invalide, float non fini, ID nul interdit, compte trop grand, item tronqué, ordre de champs erroné, `PARTIAL` et `CREATE/DELETE` illégal. Une fixture de record variable valide atteint exactement 65 535 octets et son homologue synthétique à 65 536 est rejeté avant conversion en `record_length:u16`. Des scénarios croisés vérifient aussi :

- suppression cascade d'entité ;
- update de manifeste puis snapshot paginé atomique ;
- delta cumulatif où un champ revient à la baseline ;
- filtrage `Cockpit` sans fuite d'ID ;
- `COMPLETED+HIDDEN` sans texte cargo ;
- bouclier à 0, 1, 4 et 64 segments ;
- tertiaire sans champs inventés ;
- lock sans tentative et avec durée zéro valide ;
- communication inactive initiale, START, pause, inversion, remplacement et STOP ;
- événements fiables dupliqués/désordonnés ;
- perte vidéo sans effet sur l'état canonique.

Le présent document, le schéma machine-readable et les golden vectors DOIVENT être identiques sur les ordres, offsets, valeurs numériques, bornes, masques et conditions. Toute divergence échoue la gate de Phase 0 ; aucun artefact ne corrige ou ne surclasse implicitement un autre. Après gel, le triplet versionné et hashé forme la référence FSTL 1.0 indivisible.
