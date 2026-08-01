# 04 — Modèle de données et règles métier

## 1. Objet

Ce document fixe l’image canonique Phase 2, les politiques de présence, les mappings moteur, les identités et les calculs dérivés. Les layouts et bornes filaires restent ceux de [la spécification Phase 0](../0-Contrat-de-protocole/04-modele-de-donnees-v1.md) ; ce document détermine exactement comment la Phase 2 les remplit.

## 2. Profils et ensembles exacts

### 2.1 Gate `CoreGate`

Pour `state_domain_coverage=0x0401`, un snapshot avec joueur présent contient exactement :

| Record | Cardinalité |
|---|---:|
| `SESSION_STATE` | 1 |
| `MISSION_STATE` | 1 |
| `ENTITY_LIFECYCLE` du joueur | 1 |
| `SHIP_IDENTITY` du joueur | 1 |
| `FLIGHT_STATE` du joueur | 1 |
| `DAMAGE_STATE` du joueur | 1 |
| `SHIELD_STATE` du joueur | 1 |
| `SUBSYSTEM_STATE` du joueur | `N`, tous les sous-systèmes déclarés/applicables |
| `ENERGY_STATE` du joueur | 1 |
| `PROPULSION_STATE` du joueur | 1 |

Le total est `9 + N`. `required_manifest_id` est non nul et désigne une transaction installée contenant le catalogue de classe requis.

### 2.2 Profil `CompleteShip`

Pour `state_domain_coverage=0x0583`, soit `K >= 1` le nombre de vaisseaux exportés dans la closure service/docking et `N` le nombre total de leurs sous-systèmes. Le snapshot contient exactement :

| Record | Cardinalité |
|---|---:|
| `SESSION_STATE`, `MISSION_STATE` | 1 chacun |
| `ENTITY_LIFECYCLE`, `SHIP_IDENTITY`, `FLIGHT_STATE`, `DAMAGE_STATE`, `SHIELD_STATE`, `ENERGY_STATE`, `PROPULSION_STATE`, `WEAPON_STATE` | `K` chacun |
| `SUBSYSTEM_STATE` | `N`, somme exhaustive des sous-systèmes des `K` ships |
| `DOCKING_STATE`, `SUPPORT_STATE` | `K` chacun |
| `CONTROL_STATE`, `CARGO_SCAN_STATE` | 1 chacun, pour le joueur observé |

Le total est `4 + 10K + N`. Le cas minimal `K=1` donne `14+N`. Un vaisseau sans banque conserve un `WEAPON_STATE` avec listes vides et sélecteurs zéro ; un vaisseau sans support/docking conserve des records en phase `NONE` et listes vides.

Tout `support_entity_id`, `remote_entity_id`, leader de docking ou cible de scan non nul doit appartenir aux `K` lifecycle atoms. Comme `CORE_SHIP` et `WEAPONS` sont annoncés, chaque référence ship entraîne son état complet ; aucune exception opaque n’existe.

### 2.3 Joueur absent

Après publication fiable de la disparition et suppression cascade, un snapshot sans joueur contient exactement `SESSION_STATE` et `MISSION_STATE`, avec `observed_player_entity_id` absent et le manifeste installé toujours référencé. `active_manifest` et son `catalog_fingerprint` sont gelés : la closure vide NE DOIT provoquer ni catalogue vide ni génération supplémentaire. Les obligations `CONTROL_STATE`, `WEAPON_STATE`, `CARGO_SCAN_STATE`, `DOCKING_STATE` et `SUPPORT_STATE` sont alors vacuement satisfaites, car aucun joueur/vaisseau autorisé n’existe. La session conserve `0x0583`; elle NE DOIT créer aucun record à ID zéro.

Une apparition ultérieure réutilise le manifeste si son `catalog_fingerprint` est exactement celui de la génération active ; sinon la séquence manifeste puis keyframe s’applique avant exposition du nouvel état.

### 2.4 Unicité des atomes

Les records communs à plusieurs domaines ne sont émis qu’une fois. Les clés exactes sont celles de Phase 0 : singletons globaux, `entity_id`, `(entity_id,subsystem_id)` et clés de catalogues. L’ordre canonique dans une transaction est `record_type` croissant puis clé binaire lexicographique.

## 3. Manifeste et catalogues

### 3.1 Transaction exhaustive

Une transaction `MANIFEST_PART`, `ManifestKind.FullRequired`, porte un `manifest_id >= 1` et contient :

- un `CLASS_MANIFEST` par variante de classe effective distincte de la fermeture ;
- un `WEAPON_MANIFEST` par classe d’arme référencée ;
- aucun autre record de manifeste spécialisé.

Les deux ensembles sont installés comme une seule génération atomique. Une variante effective est identifiée par un `ClassDescriptor` canonique **pré-ID** qui contient tous les champs sémantiques de `ClassManifestV1` et les clés logiques de ses références, mais exclut `manifest_generation`, `class_id`, tout ID alloué et `ship_info_index`. Deux instances du même index moteur avec loadouts/capacités différents ont des descripteurs différents ; deux descripteurs byte-identical, même issus de deux indices moteur, partagent le même `class_id`. `SHIP_IDENTITY.ship_class_id` et `ENTITY_LIFECYCLE.class_id` pointent cette variante, jamais l’index brut. Pour un joueur non armé, l’ensemble `WEAPON_MANIFEST` peut être vide, mais la génération d’armes vide est explicitement considérée installée avec la transaction qui contient au moins le `CLASS_MANIFEST` du joueur.

Pour `CoreGate`, la fermeture de profil est exactement `{joueur}` et les définitions statiques référencées par le manifeste de cette racine ; elle ne suit aucune relation support/docking. Pour `CompleteShip`, la fermeture est le plus petit point fixe transitif contenant :

1. le joueur ;
2. le support assigné de chaque membre lorsqu’il existe ;
3. tous les leaders de groupe et vaisseaux de la composante de docking atteints transitivement depuis chaque membre ;
4. une cible de scan seulement si elle est déjà membre de cet ensemble et de l’allowlist `Cockpit` ;
5. les classes de chacun de ces vaisseaux ;
6. les classes d’armes de leurs banques, tourelles et contre-mesures ;
7. toute définition statique référencée par les entrées précédentes.

La fermeture `CompleteShip` contient au plus 64 vaisseaux et 4096 sous-systèmes au total, sans dépasser 1024 par classe/vaisseau. Une relation vers un 65e vaisseau, un 4097e sous-système, une cible de scan extérieure ou un membre non autorisé fait perdre la capacité à produire ce profil : la session `CompleteShip` est terminée avant publication, sans troncature ni fuite, tandis qu’une projection `CoreGate` indépendante reste éligible si sa racine est valide.

### 3.2 Politique `CLASS_MANIFEST`

Les champs de base `manifest_generation`, `class_id`, `internal_name`, `species_id`, `ship_type_id`, `mass` et `center_of_mass` sont obligatoires et validés. `internal_name` copie `Ship_info[ship_info_index].name`; `species_id` et `ship_type_id` viennent des registres canoniques internes décrits en 3.4, jamais d’un index décalé ni d’un ID opaque. `mass` est la masse physique effective calculée par `physics_ship_init()`, soit la masse modèle (ou sa valeur de secours) multipliée par `ship_info::density`; `center_of_mass` vient du `polymodel` effectif. La matrice `INERTIA` est la valeur physique effective après application de la densité, convertie dans l’ordre FSTL. `density` et le choix modèle/secours sont conservés dans la provenance de l’oracle, mais seuls leurs résultats sérialisés canoniques participent au `ClassDescriptor` et au fingerprint. Deux chemins source donnant les mêmes octets sémantiques ne créent donc ni ID ni génération distincts. Un même nom peut apparaître dans plusieurs variantes de loadout, mais leurs `class_id` ne diffèrent que si leur descripteur canonique diffère.

| Groupe de présence | Politique Phase 2 |
|---|---|
| `INERTIA` | présent depuis la matrice physique effective initialisée à partir de `polymodel::moment_of_inertia / ship_info::density`, inversée/convertie selon la sémantique `ClassManifestV1`, dans l’ordre de matrice FSTL |
| `DAMPING` | toujours absent : `damp/rotdamp` ne fournissent pas le vecteur local complet exigé par le groupe all-or-nothing |
| `MOTION` | toujours présent depuis `ship_info::{max_vel,afterburner_max_vel,max_rotvel,max_rear_vel,forward_accel,afterburner_forward_accel,forward_decel,slide_accel,slide_decel}`; booster vaut le zéro canonique si absent |
| `HULL` | toujours présent depuis `ship_info::max_hull_strength`, distinct du maximum dynamique d’état |
| `SHIELD` | présent si `ship_info::max_shield_strength > 0`, depuis cette valeur |
| `ENERGY` | toujours absent : aucune source distincte ne couvre les cinq champs, notamment `reserve_energy_max` |
| `AFTERBURNER` | présent si `Info_Flags::Afterburner` et capacité positive ; champs depuis `afterburner_fuel_capacity`, `afterburner_burn_rate`, `afterburner_recover_rate`, `afterburner_min_start_fuel` et `afterburner_cooldown_time` converti secondes→µs |
| `COUNTERMEASURE` | présent si `ship_info::cmeasure_max>0`; `cmeasure_type` doit alors indexer `Weapon_info` et désigner une contre-mesure installée ; `initial_count = floor(cmeasure_max / Weapon_info[type].cargo_size)` si `Countermeasures_use_capacity` (`cargo_size` strictement positif), sinon `cmeasure_max`; `cmeasure_firewait`, déjà exprimé en millisecondes dans `weapon_info`, est converti par `×1 000` en µs. Si `cmeasure_max==0`, le groupe est absent et le type par défaut éventuel n’est pas référencé ; une capacité négative ou positive avec type invalide refuse la variante |
| `BANKS` | présent depuis le loadout effectif `ship::weapons` et la géométrie `polymodel::gun_banks/missile_banks`; cette définition participe à la clé de variante |
| `SUBSYSTEMS` | obligatoire depuis `ship_info::n_subsystems/subsystems` et chaque `model_subsystem`; liste complète et canonique de `0..1024` définitions |
| `SCAN` | présent selon la convention statique exacte ci-dessous ; l’état pairwise effectif reste dans `CARGO_SCAN_STATE` |
| `GLIDE` | présent si `ship_info::can_glide` et `Objects[objnum].phys_info.glide_cap` effectif après `physics_ship_init()` est fini et strictement positif ; absent si le glide est interdit ou si le cap effectif vaut zéro/négatif (dynamique ou non borné) |
| `AUTOAIM` | présent si `ship::autoaim_fov`, copié de la classe puis éventuellement surchargé comme autorité effective d’instance, est fini et strictement positif ; cette valeur est déjà en radians et doit rester dans `[0,π]` |
| `RADAR_ICON` | absent en Phase 2 ; propriété de la vue capteurs Phase 3 |

Les champs nommés accélérations dans les tables moteur sont convertis en constantes de temps comme prescrit par Phase 0. Aucun cache HUD ou pointeur modèle n’est publié.

Ces politiques de présence sont fermées : un groupe déclaré « toujours absent » reste absent même si un champ voisin existe ; un groupe applicable dont un membre est invalide refuse la variante entière, il n’est pas omis opportunément. Toute modification de modèle effectif, loadout, banque ou définition incluse modifie le `catalog_fingerprint`.

`ClassBankV1` est construit sans heuristique : les banques de coque primaires et secondaires viennent respectivement de `ship::weapons.primary_bank_weapons`/`secondary_bank_weapons`, leurs capacités effectives des tableaux de capacité de la même instance et leurs points de tir de `polymodel::gun_banks`/`missile_banks`. Les banques de tourelle viennent des `ship_subsys::weapons` de l’instance et des `model_subsystem::{primary_banks,secondary_banks,turret_firing_point}` correspondants. Pour `num_tertiary_banks=T` validé dans `0..64`, exactement T définitions `family=TERTIARY`, `bank_index=0..T-1`, `owner=0`, `source_family=None` sont créées : `WEAPON_CLASS` est interdit, `fire_points` est vide faute de géométrie tertiaire autoritaire, et `CAPACITY` est présent avec `tertiary_bank_capacity` si cette valeur est strictement positive, absent si elle vaut zéro. Les T entrées peuvent donc partager la même capacité sémantique mais ont des `bank_id` distincts ; l’état courant réutilise l’ID de `current_tertiary_bank`.

Une banque primaire/secondaire/tourelle moteur invalide, vide ou sans géométrie cohérente refuse la variante. `WEAPON_CLASS` est présent pour chacune d’elles. `CAPACITY` est présent exactement pour une banque consommant des munitions et publie sa capacité cargo effective ; il est absent pour une primaire énergétique et pour une secondaire `SecondaryNoAmmo`. `bank_index` conserve l’index dans sa famille, les points de tir conservent l’ordre du modèle, et les clés/IDs suivent 3.4.

Chaque `ClassSubsystemV1` prend `canonical_index` dans l’ordre `ship_info::subsystems`, `internal_name=model_subsystem::name`, `ALT_NAME=model_subsystem::alt_sub_name` lorsqu’il est non vide, et `HUD_NAME=model_subsystem::alt_dmg_sub_name` lorsqu’il est non vide. `local_position=model_subsystem::pnt`; `ORIENTATION` est toujours absent en Phase 2, faute de quaternion statique autoritaire distinct de l’état animé. `radius`, `max_hits` et `ARMOR` viennent exactement de `model_subsystem::{radius,max_subsys_strength,armor_type_idx}`, l’armure passant par le registre de 3.4. Les flags statiques sont fermés : `TARGETABLE=!Untargetable`, `VISIBLE_BY_DEFAULT=0`, `SCANNABLE_CARGO=(scan_time>0)`, `ROTATES=(Rotates|Stepped_rotate|Ai_rotate|Triggered)`, `TRANSLATES=(Translates|Stepped_translate)`, `TURRET=(type==SUBSYSTEM_TURRET)` et `AWACS=Awacs`; aucun autre flag moteur n’est projeté.

Une variante contient au plus 64 banques par famille wire, 192 `bank_definitions` au total et 1024 `subsystem_definitions`. Après sérialisation, chaque `CLASS_MANIFEST` doit avoir `record_length <= 65 535`; une 65e banque d’une famille, une 193e banque agrégée, un 1025e sous-système ou une longueur 65 536 refuse le profil avant émission, sans troncature.

`ClassScanV1` est une synthèse strictement statique de classe, pas une décision pairwise : `required_time_us = max(Ship_info[class].scan_time, 0) × 1000` décrit la classe comme cible sans multiplicateur du scanner ; `max_distance = max(scan_range_normal, scan_range_capital) × scanning_range_multiplier` décrit l’enveloppe maximale de cette classe comme scanner ; `max_angle_rad = acos(0.95)` reprend `CARGO_MIN_DOT_TO_REVEAL`. Les multiplications se font en double puis sont validées avant conversion `float32`. Les flags runtime `Cannot_perform_scan_*` ne modifient jamais le manifeste. Le client NE DOIT PAS utiliser cette enveloppe pour décider un scan : `CARGO_SCAN_STATE.required_time_us` et ses validity flags portent l’autorité effective cible+sous-système+scanner, y compris le choix normal/capital, `scanning_time_multiplier` et les interdictions d’instance.

### 3.3 Politique `WEAPON_MANIFEST`

Chaque entrée prend ses champs obligatoires dans `weapon_info`: `name`, `subtype`, mapping fermé de `wi_flags`, `max_speed`, `mass`, `gravity_const`, `lifetime` et `vel_inherit_amount`. Les flags moteur sans équivalent public sont ignorés ; les flags publics ne sont jamais inférés d’un nom.

Le sous-type public suit cette priorité fermée : `Cmeasure→COUNTERMEASURE`; sinon `weapon_info::is_beam()→BEAM`; sinon `WP_LASER→PRIMARY`; sinon `WP_MISSILE→MISSILE`. Toute autre valeur moteur est refusée, et `SPECIAL` n’est jamais produit en Phase 2. `WeaponClassFlags` vaut exactement l’OR des prédicats suivants : `BOMB=Bomb`, `BALLISTIC=Ballistic`, `AMMOLESS=SecondaryNoAmmo`, `BEAM=weapon_info::is_beam()`, `SWARM=Swarm`, `COUNTERMEASURE=Cmeasure`, `HOMING=weapon_info::is_homing()` et `REMOTE_DETONATABLE=Remote`. Aucun autre `wi_flags` n’est copié et aucun bit n’est déduit du simple sous-type.

| Groupe | Source et règle Phase 2 |
|---|---|
| `TITLE` | présent si et seulement si `weapon_info::has_display_name()` et `weapon_info::display_name` est non vide ; copie octet pour octet ce `display_name` brut validé, sans fallback vers `name`, appel à `get_display_name()` ni localisation additionnelle |
| `ACCELERATION` | présent si `acceleration_time > 0`, converti secondes → microsecondes |
| `RANGES` | présent depuis `weapon_min_range`, `optimum_range`, `weapon_range` si l’ordre `min <= optimal <= max` est valide |
| `FIRE` | présent pour toute arme tirable depuis `fire_wait` et `energy_consumed`; secondes → microsecondes |
| `DAMAGE` | présent depuis `damage`, `damage_type_idx` mappé en ID local et le mapping fermé `SHOCKWAVE=(shockwave.outer_rad>0)`, `EMP=Emp`, `TAG=Tag`, `SHIELD_PIERCING=Pierce_shields`, `SPAWNS_CHILDREN=Spawn`; les paramètres associés restent validés par leurs bornes FSTL et une incohérence refuse le manifeste |
| `GUIDANCE` | présent si au moins un des flags `Swarm`, `Homing_heat`, `Homing_aspect`, `Homing_javelin` existe ; type selon la priorité `Swarm→SWARM`, `Homing_heat→HEAT`, `(Homing_aspect || Homing_javelin)→ASPECT`; `HOMING` et `SCRIPTED` ne sont jamais produits en Phase 2 ; la source cosinus `weapon_info::fov` doit être finie dans `[-1,1]`, puis `guidance_fov_rad = acos(source)` est validé dans `[0,π]`, sans clamp |
| `LOCK` | présent seulement pour `Homing_aspect || Homing_javelin`; durée depuis `min_lock_time`, source `lock_fov` finie dans `[-1,1]`, puis `lock_fov_rad = acos(source)` validé dans `[0,π]`, sans clamp |
| `CARGO_REARM` | présent si la classe consomme des munitions et n’est pas `AMMOLESS`, depuis `cargo_size`, `rearm_rate` et `reloaded_per_batch` |
| `BURST` | présent si burst configuré ; `burst_count = burst_shots + 1`, intervalle depuis `burst_delay` |
| `SWARM` | présent si `swarm_count > 0` ou `shots > 1`; `swarm_count_wire = (swarm_count > 0 ? swarm_count : 1)` et `shots_per_trigger = (shots > 0 ? shots : 1)`, chacun validé dans `[1,4096]` avant conversion. Ainsi une arme non-swarm à shots multiples ne sérialise jamais le défaut moteur `swarm_count=-1` |
| `COUNTERMEASURE` | toujours absent : le moteur expose plusieurs efficacités/rayons sans scalaire canonique `countermeasure_strength` |

La présence d’un groupe ne signifie pas que la Phase 2 publie la cible ou un événement de tir. Les conversions se font en double, vérifient finitude, signe et borne avant écriture. Un groupe conditionnel non applicable est absent ; une source présente mais invalide refuse tout le manifeste, elle n’est jamais transformée en absence pour masquer l’erreur.

### 3.4 IDs et fingerprints

`topology_fingerprint` est le SHA-256 local de la liste canonique des instances, supports, leaders et relations. Sa modification force une keyframe. `catalog_fingerprint` est le SHA-256 local des `ClassDescriptor`, `WeaponDescriptor`, clés auxiliaires et définitions imbriquées canoniques pré-ID triées encore requises par le point fixe ; `manifest_generation`, IDs alloués et index moteur en sont exclus. Sa modification entraîne un nouveau `manifest_id`. L’ajout d’une instance d’une classe déjà requise ne change que le premier ; le retrait du dernier utilisateur d’une définition change le second, sauf pendant l’exception `PlayerPresence::Absent` décrite en 2.3. Aucun fingerprint n’est transmis.

Les registres auxiliaires couvrent `species_id`, `ship_type_id`, `iff_id`, `wing_id`, `armor_id`, `damage_type_id` et `pattern_id`. Le matériau source des clés est possédé par la mission, mais il est filtré par projection ; chaque slot de session alloue ses IDs dans sa génération de manifeste. Deux slots ou deux profils ne partagent donc ni allocation numérique ni ensemble implicite. Les registres sont séparés par type. Pour les six registres nommés, la clé canonique est exactement `tag:u8 || byte_length:u16LE || name_bytes`, où la chaîne source est non vide, UTF-8 valide, sans NUL, copiée octet pour octet sans trim, changement de casse, localisation ni normalisation Unicode. Le registre pattern utilise `tag:u8 || firing_pattern_code:u8`. Les tags sont respectivement `1..7` dans l’ordre de la table suivante :

| Registre | Index source validé | Nom/définition canonique et règle de zéro |
|---|---|---|
| `species_id` | `Ship_info[ship_info_index].species` dans `Species_info` | `Species_info[i].species_name`; `-1→0` inconnu, toute autre valeur hors borne refuse la variante |
| `ship_type_id` | `Ship_info[ship_info_index].class_type` dans `Ship_types` | `Ship_types[i].name`; `-1→0` inconnu, toute autre valeur hors borne refuse la variante |
| `iff_id` | `ship::team` dans `Iff_info` | `Iff_info[i].iff_name`; l’index doit être valide et l’ID alloué doit rester dans `1..65535`. En SOLO, `team_id=0` : il n’existe pas de second registre d’équipe |
| `wing_id` | `ship::wingnum` dans `Wings` | `Wings[i].name`; `-1` rend le groupe `WING` absent, toute autre valeur hors borne refuse l’atome |
| `armor_id` | `ship/model_subsystem::armor_type_idx` dans `Armor_types` | `Armor_types[i].GetNamePtr()`; `-1` rend `ARMOR` absent, toute autre valeur hors borne refuse le groupe obligatoire/applicable |
| `damage_type_id` | `weapon_info::damage_type_idx` dans `Damage_types` | `Damage_types[i].name`; `-1→0` type générique, toute autre valeur hors borne refuse le manifeste |
| `pattern_id` | pattern effectif de banque primaire | `STANDARD→0`; sinon code fermé `CYCLE_FORWARD=1`, `CYCLE_REVERSE=2`, `RANDOM_EXHAUSTIVE=3`, `RANDOM_NONREPEATING=4`, `RANDOM_REPEATING=5`. Si `Dyn_primary_linking`, l’index `ship_weapon::dynamic_firing_pattern[bank]` doit résoudre exactement une entrée de `ship_info::dyn_firing_patterns_allowed[bank]`; sinon la source est `Weapon_info[weapon_class].firing_pattern` |

Pour un registre nommé, deux index moteur distincts portant le même nom source sont une ambiguïté et refusent le profil ; aucune propriété privée supplémentaire n’est intégrée à la clé. Les entrées référencées par la projection du profil sont triées lexicographiquement par octets de clé puis reçoivent, dans le slot, des IDs à partir de 1 dans la largeur/borne FSTL du champ (`iff_id` et `pattern_id` au plus 65 535). Deux occurrences de la même clé dans cette projection partagent l’ID. Les allocations sont immuables pendant la génération de manifeste du slot et purgées avec elle ou au changement de `mission_generation`; leurs clés participent au `catalog_fingerprint` pré-ID. Zéro n’est utilisé que selon les cas explicitement listés. Aucun index `Ship_info`, `Species_info`, `Ship_types`, wing, équipe/IFF, armor, damage type ou pattern moteur ne sort sur le fil.

L’ordre de résolution évite toute circularité : (1) construire/trier les clés auxiliaires ; (2) construire des `WeaponDescriptor` pré-ID, leur attribuer `weapon_class_id`; (3) construire les `ClassDescriptor` dont les références utilisent les clés/descripteurs d’armes, puis attribuer `class_id`; (4) allouer `subsystem_id` et `bank_id` dans chaque classe ; (5) matérialiser seulement alors les records FSTL avec génération/IDs ; (6) calculer `catalog_fingerprint` sur les descripteurs pré-ID triés. Une nouvelle génération aux mêmes descriptions conserve donc le même fingerprint même si `manifest_generation` ou les IDs de transaction changent.

Les IDs publics suivent ces règles :

- `class_id` et `weapon_class_id` sont alloués séquentiellement dans l’ordre canonique, à partir de 1 ;
- `subsystem_id` est alloué dans l’ordre `ClassSubsystemV1.canonical_index`, à partir de 1 ;
- `bank_id` est alloué par `(class_id, wire_family, owner_subsystem_id_or_zero, source_family, bank_index)`, à partir de 1 ; `owner=0` vaut pour les banques de coque et `source_family=None`. Pour `wire_family=TURRET`, owner est le `subsystem_id` et `source_family=PRIMARY|SECONDARY`, ce qui distingue les deux index zéro d’une même tourelle et toutes les tourelles entre elles ;
- un même ID statique est réutilisé dans le record d’état et le manifeste de sa génération ;
- aucune valeur négative ou index moteur `-1` n’est castée en entier non signé ; l’absence utilise exactement zéro lorsque le schéma l’autorise.

## 4. Sources, validation et canonisation communes

### 4.1 Capture atomique

Tous les champs d’un bloc portent le `producer_sample_time_us` du tick qui les a capturés. Une keyframe force tous les blocs au même tick. Un delta ordinaire peut combiner des atomes capturés à leurs cadences respectives ; chacun conserve son propre sample time, tandis que le préfixe du delta porte le maximum des samples inclus.

Le collecteur copie d’abord les scalaires et les cardinalités, vérifie les relations, puis remplit les listes. Si une cardinalité change pendant la capture — cas impossible attendu sur un thread unique mais détectable par contrôle — le bloc entier est refusé.

Hors keyframe, la capture reçoit un masque fermé `FlightControl` et/ou `Systems`. Elle ne parcourt et ne remet à zéro que les structures du masque. Les autres blocs conservent leur dernière valeur validée et leur sample time. Une keyframe, une variation de fermeture, lifecycle, manifeste ou identité publique force `All`; aucune valeur conservée d'une ancienne topologie ne peut alors survivre.

### 4.2 Flottants et temps

- Tout `float32` DOIT être fini avant écriture.
- `-0.0` est canonisé en `+0.0`.
- Pour un champ dont le schéma exige un maximum strictement positif, ce maximum est validé fini et positif puis le HP courant fini est canonisé par `clamp(source,0,maximum)`. Pour `ClassSubsystemV1`/`SubsystemStateV1`, dont le schéma autorise zéro, `maximum` est validé dans `[0,1e12]`; s’il vaut zéro, le courant canonique doit être exactement zéro, sinon il est clampé dans `[0,maximum]`. Cela couvre l’overkill négatif sans inventer un maximum positif.
- Les quaternions suivent la normalisation et le signe canonique Phase 1.
- Les timestamps `timestamp()` moteur sont convertis en `duration_us` relative au tick ; une valeur écoulée devient zéro.
- Une durée négative, non représentable ou au-delà de la borne du champ refuse le groupe ou le profil selon son caractère obligatoire ; elle n’est jamais saturée.

### 4.3 Chaînes

Les chaînes sont UTF-8 valides, sans NUL, limitées en octets selon le schéma. Une chaîne source invalide ou trop longue pour un champ obligatoire refuse la fermeture. Pour un champ conditionnel non essentiel, le groupe complet est absent seulement si cette absence respecte sa règle sémantique ; aucune troncature n’est permise.

## 5. Singletons, identité et lifecycle

### 5.1 `SESSION_STATE`

Pour `CoreGate`, `event_coverage_state_derived=0`, `event_coverage_exact=0` et toutes les capabilities spécialisées valent zéro afin de conserver le golden `0x0401` byte-identical. Pour le profil final :

- `authority_mode=SOLO` ;
- `visibility_mode=COCKPIT` ;
- `session_phase` suit la machine héritée ;
- `state_domain_coverage=0x0583` ;
- `event_coverage_state_derived=0x0005` (`ENTITY | DAMAGE`) ;
- `event_coverage_exact=0` ;
- `observed_player_entity_id` est présent et non nul si et seulement si un joueur valide est matérialisé ; apparition, disparition et respawn modifient ce champ uniquement par keyframe ;
- toutes les capabilities spécialisées valent exactement zéro ; aucun record spécialisé n’est autorisé en Phase 2.

### 5.2 `MISSION_STATE`

`mission_generation`, phase, pause, compression et sample time suivent Phase 1. `mission_name` n’est présent que s’il est autorisé. Un changement de génération n’apparaît jamais dans un delta.

### 5.3 `ENTITY_LIFECYCLE`

Chaque membre de la closure utilise `ObjectType.SHIP`, le `class_id` du manifeste et les flags moteur `DYING`, `DISABLED`, `EXPLODED`, `SHOULD_BE_DEAD`. La phase filaire est résolue dans cet ordre, tant que l’objet reste matérialisable : `Ship_Flags::Exploded -> DESTROYED`; `Ship_Flags::Dying -> DYING`; `Object_Flags::Should_be_dead -> REMOVED`; `ship::is_departing() -> DEPARTING`; `ship::is_arriving() -> SPAWNING`; sinon `ACTIVE`.

`SPAWNING` utilise `TransitMode.DOCKBAY` pour `arrival_location==FROM_DOCK_BAY`, `NONE` pour `No_arrival_warp`, sinon `WARP`. `DEPARTING` utilise `DOCKBAY` pour `Depart_dockbay` ou `departure_location==TO_DOCK_BAY`, `NONE` pour `No_departure_warp`, sinon `WARP`. `SCRIPTED` exige une source explicite ; il n’est jamais déduit.

`SIGNATURE` et `NET_SIGNATURE` sont toujours absents en Phase 2. La signature moteur reste uniquement dans `EngineEntityKey`, les registres et les latches privés ; elle ne fuit jamais comme valeur publique. Le groupe `PARENT` est absent sauf si le parent est un ship déjà membre de la closure et résout un lifecycle public ; aucune nouvelle racine n’est ajoutée pour ce groupe.

Un respawn produit une création logique dans le registre avec nouvel ID, mais les changements de topologie Phase 2 passent toujours par une keyframe `FULL_SNAPSHOT` dont tous les `record_flags` valent zéro. Le retrait est l’absence de l’ancien lifecycle et de ses dépendants dans la keyframe successeur après l’événement fiable ; Phase 2 n’émet donc ni `CREATE` ni `DELETE` filaire dans ces séquences. Le seam main-thread `ship_cleanup()` copie `{signature, cleanup_mode}` avant purge afin de distinguer `DESTROYED`, `DEPARTED` et `VANISHED` si l’objet disparaît entre callbacks ; il ne tente jamais de construire un lifecycle partiel après destruction de la source. Les transitions fiables portent `RECONSTRUCTIBLE|RELIABLE`, jamais `EXACT_CAPTURE` en Phase 2.

Les `EVENTS` Phase 2 n’emploient que le sujet public et les champs obligatoires du kind. Les groupes optionnels `ACTOR`, `WEAPON_CLASS`, `SUBSYSTEM`, `POSITION` et texte sont absents ; ils ne peuvent donc ni référencer une entité hors closure ni révéler une provenance Phase 3. Les fronts garantis sont `ENTITY_APPEARED`, `SHIP_DISABLED`, `SHIP_DYING_STARTED`, `ENTITY_DESTROYED` et `ENTITY_DISAPPEARED` lorsqu’ils sont observés/latchés.

### 5.4 `SHIP_IDENTITY`

| Champ/groupe | Source et règle Phase 2 |
|---|---|
| `ship_class_id` | `class_id` du `ClassDescriptor` pré-ID effectif installé selon 3.1 et 3.4 ; deux loadouts distincts d’un même index moteur restent distincts |
| `internal_name` | `ship::ship_name`, chaîne non vide validée UTF-8 et autorisée en `Cockpit` |
| `DISPLAY_NAME` | présent exactement si `ship::has_display_name()` et `ship::display_name` non vide ; source `ship::display_name`, pas le fallback de `get_display_name()` |
| `CALLSIGN` | présent exactement si `callsign_index>=0` et `mission_parse_lookup_callsign_index(callsign_index)` renvoie une chaîne non vide valide ; index invalide = source invalide, pas absence |
| `species_id`, `team_id`, `iff_id` | `species_id` via `Ship_info[ship_info_index].species` et le registre de 3.4 ; `team_id=0` en SOLO faute d’autorité séparée ; `iff_id` via `ship::team` et `Iff_info` selon 3.4 |
| `role_flags` | OR fermé : `PLAYER=(objnum==Player_obj->objnum)`, `AI=(ai_index>=0)`, `SUPPORT=Ship_info[ship_info_index].flags[Support]`, `MISSION_OBJECT=(Objects[objnum].type==OBJ_SHIP)` ; aucun autre rôle n’est inféré |
| `radius` | `Objects[objnum].radius`, fini dans `[0,1e9]`, copié au même sample que le champ homonyme de `FLIGHT_STATE` |
| `WING` | absent si `wingnum==-1`; sinon `wingnum` doit indexer `Wings`, `wing_id` vient du registre du slot/génération décrit en 3.4, `wing_name=Wings[wingnum].name`, et `wing_position` est l’unique indice `i` dans `Wings[wingnum].ship_index[0..current_count)` tel que `ship_index[i]` désigne le ship, validé `0..4095`; lookup absent/dupliqué refuse l’atome |
| `LOGICAL_SIZE` | toujours absent en Phase 2 : aucun scalaire moteur distinct du rayon objet ne couvre sans ambiguïté cette sémantique |
| `SENSOR_VISIBILITY` | toujours absent du fil Phase 2 ; les sources capteurs peuvent servir au filtre `Cockpit` sans être émises |

## 6. Mouvement et commandes

### 6.1 `FLIGHT_STATE`

Les neuf champs obligatoires conservent le mapping Phase 1 : position monde, quaternion local-vers-monde, vitesse monde, vitesse angulaire locale, rayon et `PhysicsModeFlags`.

La Phase 2 autorise les groupes prédictifs du schéma seulement lorsque l’autorité moteur correspondante est lue au même tick : vitesses désirées, ramp velocity, caps, gravité, constantes de temps, rotdamp, side slip et thrust cosmétique. Le bitmap `PREDICTION` n’est pas annoncé ; ces groupes restent donc absents dans le profil normatif Phase 2 afin d’éviter une couverture implicite. Les modes afterburner, booster, glide, Newtonian, translation, warp, scripted, shockwave et locks physiques restent dans `physics_mode_flags`.

### 6.2 `CONTROL_STATE`

Le record est obligatoire dans `0x0583` et absent de `0x0401`.

| Champ/groupe | Règle Phase 2 |
|---|---|
| six axes | champs homonymes `Player->ci.pitch/heading/bank/vertical/sideways/forward`, chacun fini dans `[-1,1]`; aucun clamp silencieux |
| `control_mode` | priorité : `AutoPilotEngaged -> AUTOPILOT`; sinon `Player_use_ai` ou `Player->control_mode != PCM_NORMAL -> UNKNOWN`; sinon le latch `last_control_target==Camera -> VIEW`; sinon flight cursor effectif -> `FLIGHT_CURSOR`; sinon `SHIP` |
| `control_flags` | construit bit par bit depuis `PLAYER_FLAGS_MATCH_TARGET`, `PLAYER_FLAGS_AUTO_TARGETING`, `PLAYER_FLAGS_AUTO_MATCH_SPEED`, `Ship_Flags::Primary_linked`, `Ship_Flags::Secondary_dual_fire` et `Control_config[AFTERBURNER].continuous_ongoing` en contrôle manuel normal |
| `CRUISE` | toujours présent depuis `Player->ci.forward_cruise_percent`, fini dans `[-100,100]` % |
| `REQUEST_COUNTERS` | toujours présent depuis `Player->ci.fire_primary_count`, `fire_secondary_count`, `fire_countermeasure_count`, chacun validé `0..65535` |
| `FLIGHT_CURSOR` | présent si `Player_flight_mode==FlightMode::FlightCursor` ou `Ship_info[player].aims_at_flight_cursor`, avec `Player_flight_cursor.p/h`, `Player_flight_cursor_sensitivity` validée `[0,1]`, et `cursor_deadzone = Flight_cursor_deadzone / effective_extent` validé `[0,1]` (`effective_extent` = classe aim extent si positive, sinon `Flight_cursor_extent`) |

`last_control_target` est un booléen copié dans le seam de `read_keyboard_controls()` au moment où le chemin choisit contrôle vaisseau ou caméra ; le simple test tardif de `Viewer_mode==0` n’est pas une autorité suffisante. Le mapping NE DOIT jamais caster `Player->control_mode` vers `ControlMode` et NE DOIT jamais copier `Player->ci.control_flags`, qui contient les `CIF_*` de physique sans rapport avec les flags FSTL. `afterburner_start` est un front et `PF_AFTERBURNER_ON` l’état actif : ni l’un ni l’autre ne remplace la demande persistante choisie ci-dessus. Les compteurs ne sont pas des événements tirés. Un compteur source au-delà de 65 535 interdit le profil au lieu d’être tronqué. L’état autopilote se limite au mode/flags permis par `CONTROL_STATE`; navpoint, destination et refus restent Phase 3.

## 7. Coque et boucliers

### 7.1 `DAMAGE_STATE`

Les champs obligatoires sont : `hull_strength = clamp(Objects[objnum].hull_strength, 0, Ships[instance].ship_max_hull_strength)`, `dynamic_max_hull = ship_max_hull_strength`, et les `protection_flags` construits depuis `Object_Flags::Invulnerable`, `Object_Flags::Protected` et `ship_guardian_threshold > 0`. Le maximum est validé avant le clamp ; un overkill négatif normal devient zéro. Si guardian est actif, la valeur filaire HP est `0.01 × ship_guardian_threshold × dynamic_max_hull`, calculée en double après validation du pourcentage entier puis convertie/validée ; l’entier source n’est jamais sérialisé comme HP.

| Groupe | Politique Phase 2 |
|---|---|
| `SIM_HULL` | toujours absent en Phase 2 : `object::sim_hull_strength=0` est aussi la valeur d’initialisation normale et aucun marqueur durable ne prouve une autorité training/SEXP |
| `ARMOR` | présent si `ship::armor_type_idx >= 0`, après lookup de sa clé dans le registre du slot/génération de 3.4 ; jamais `index+1` |
| `GUARDIAN` | présent si `ship_guardian_threshold > 0`, avec `guardian_hp = 0.01f × ship_guardian_threshold × ship_max_hull_strength`, soit la formule exacte de `shiphit.cpp`; une valeur au-delà de `dynamic_max_hull` est non représentable et refuse la session, sans clamp |
| `CUMULATIVE_DAMAGE` | absent : aucune autorité durable complète n’est prouvée dans le seam courant |
| `LAST_DAMAGE` | absent : aucun champ durable ne garantit source + arme |
| `CONTRIBUTORS` | absent : propriété d’une instrumentation événementielle ultérieure |

`disabled`, `dying` et détruit proviennent de `ENTITY_LIFECYCLE`, jamais d’un ratio de coque.

### 7.2 `SHIELD_STATE`

Pour chaque `ShipObservationDto`, `has_shields = !Object_Flags::No_shields && shield_get_max_strength(subject, true)>0`. L’argument `true` est normatif : l’overload par défaut applique `max_shield_recharge` et ne représente pas l’existence physique du bouclier. Si `has_shields` est vrai, `segment_count` vaut exactement `Objects[ship.objnum].shield_quadrant.size()` après cohérence objnum/instance/signature et doit rester dans `1..64`; sinon il vaut zéro même si le conteneur moteur conserve des entrées. Les tableaux courant et maximum du même sujet sont complets, même cardinalité et ordre canonique moteur documenté ; aucune valeur du joueur n’est réutilisée pour un support ou un docké.

- Sans bouclier (`Object_Flags::No_shields || shield_get_max_strength(subject, true) <= 0`) : `has_shields=0`, count zéro, tableaux vides, presence zéro.
- Avec bouclier : `segment_max_hits[i] = shield_get_max_quad(subject)` pour tout segment ; `segment_current_hits[i] = clamp(shield_quadrant[i], 0, segment_max_hits[i])`.
- Si `has_shields=1`, `RECHARGE_MAX` est présent avec `shield_get_max_strength(subject, false) = ship_max_shield_strength × max_shield_recharge`, en HP et validé dans `[0,shield_get_max_strength(subject,true)]`; le facteur `ship::max_shield_recharge` n’est jamais sérialisé comme des HP. `REGEN_RATE` porte le taux instantané calculé par `update_ets` en HP/s, et `DEFERRED_TRANSFER` porte `ship::target_shields_delta` selon son signe moteur.
- Si `has_shields=0`, `presence=0` et les trois groupes sont interdits, conformément au validateur Phase 0.

L’implémentation NE DOIT appeler aucune API supposant quatre quadrants. Total, ratio et segment le plus faible sont dérivés.

## 8. Sous-systèmes et tourelles

### 8.1 Exhaustivité

Pour chaque vaisseau exporté, chaque définition `ClassSubsystemV1` de sa classe installée possède exactement un `SUBSYSTEM_STATE`. Sous `CORE_SHIP`, aucun `SUBSYSTEM_STATE` déclaré ne peut être supprimé et aucun ID hors manifeste ne peut être créé. Tout changement de l’ensemble ou d’une définition de sous-systèmes impose un nouveau `CLASS_MANIFEST` installé puis une keyframe exhaustive ; les records d’état restent des upserts complets.

Chaque instance de la liste intrusive `ship::subsys_list` doit résoudre exactement un `canonical_index` par égalité unique `ship_subsys::system_info == &Ship_info[ship_info_index].subsystems[canonical_index]`; inversement chaque définition `0..n_subsystems-1` doit être résolue une fois. Un pointeur nul, étranger, dupliqué ou une définition sans instance refuse l’image ; un changement de cette bijection impose manifeste+keyframe ou refus selon qu’il est représentable. `subsystem_id` est celui de la définition à ce `canonical_index`. `max_hits = ship_subsys::max_hits` est validé dans `[0,1e12]`; `current_hits` copie `ship_subsys::current_hits` puis applique la canonisation 4.2, donc `max_hits==0` impose `current_hits=0`. Type et flags viennent de cette même paire instance/définition et sont obligatoires.

### 8.2 Groupes optionnels

| Groupe | Politique Phase 2 |
|---|---|
| `NAME_OVERRIDES` | toujours absent : `ship_subsys::sub_name` ne fournit pas les trois chaînes atomiques indépendantes et les variantes HUD sont localisées/dérivées |
| `ARMOR` | présent si `ship_subsys::armor_type_idx >= 0`, via le registre public de 3.4 |
| `PERTURBATION` | présent si `!timestamp_elapsed(ship_subsys::disruption_timestamp)`, avec `max(timestamp_until(...),0)` converti en µs |
| `ANIMATED_TRANSFORM` | présent si `submodel_instance_1` existe et si son offset/orientation canonique diffère de l’identité ; copie offset local et matrice→quaternion canonique |
| `ANIMATIONS` | toujours absent : aucun registre complet d’animations logiques, IDs et targets bornés n’est autoritaire dans `ship_subsys` |
| `LOCAL_CARGO` | absent ; affichage cargo détaillé Phase 3 |
| `TYPE_AGGREGATE` | présent si `!Subsystem_Flags::No_aggregate`, type borné et `ship::subsys_info[type].type_count>0`; valeurs depuis `aggregate_current_hits/aggregate_max_hits`, canonisées comme HP |
| `TURRET` | présent pour le type tourelle avec la politique ci-dessous |

Le type moteur est mappé sans heuristique de nom : `0→UNKNOWN`, `1..7→ENGINE/TURRET/RADAR/NAVIGATION/COMMUNICATION/WEAPONS/SENSORS`, `8..10→OTHER`, `11→UNKNOWN`; toute autre valeur refuse le record. Les `SubsystemFlags` proviennent exactement de : `PERTURBED=!timestamp_elapsed(disruption_timestamp)`, `TARGETABLE=!Untargetable`, `VISIBLE=submodel_instance_1 && !blown_off`, `REVEALED=Cargo_revealed`, `GUARDIAN=subsys_guardian_threshold>0`, `MOVEMENT_LOCKED=ship.flags[Subsystem_movement_locked]`, `BEAM_FREE/BEAM_LOCKED=ship_subsys::weapons.flags[Beam_Free/Turret_Lock]`. `No_SS_targeting` n’est pas réinterprété en `TARGETABLE`.

### 8.3 Sous-ensemble tourelle

Le `TurretStateV1` impose `target_entity_id=0`. `current_direction_local` est calculée par la transformation locale courante du `submodel_instance` appliquée à `model_subsystem::turret_norm`, puis normalisée ; `turret_last_fire_direction` et les helpers ignorant l’aim courant sont interdits. Les groupes permis sont fermés :

| Groupe | Source/règle exacte |
|---|---|
| `NEXT_FIRE_POINT` | absent si `turret_num_firing_points=0`; présent si la valeur vaut `1..64`, avec `turret_next_fire_pos % count`; toute valeur `>64` refuse transactionnellement l’échantillon sans troncature |
| `COOLDOWN` | toujours présent, `max(timestamp_until(turret_next_fire_stamp),0)` en µs |
| `RATE_MULTIPLIER` | toujours présent ; projection sans appel mutateur : `rof_scaler<0 ? 1 : (rof_scaler==0 ? max(num_firing_points,1) : rof_scaler)` |
| `ANIMATION` | présent pour `MA_POS_SET→MOVING` ou `MA_POS_READY→STOPPED`, avec `turret_animation_done_time`; absent pour `MA_POS_NOT_SET` |
| `BANKS` | présent avec les listes primaire/secondaire complètes de `ship_subsys::weapons`; IDs `(class_id,subsystem_id,family,index)`, classe installée, cooldowns, et ammo/capacité seulement si `Turret_use_ammo` et arme munitionnée |
| `SWARM` | toujours absent : `turret_swarm_num/info` ne fournit pas de banque d’origine unique pour la paire FSTL |

`TARGET_SUBSYSTEM`, `AIM_POINT`, `TIME_IN_RANGE`, `OPTIMAL_RANGE`, `TARGET_PRIORITY`, `INACCURACY` et `AWACS` sont toujours absents. Les banques de tourelle figurent aussi dans `CLASS_MANIFEST.bank_definitions` avec `family=TURRET`, clé incluant le `subsystem_id`, fire points et capacité ; le même `bank_id` est réutilisé dans l’état.

Cette politique fournit l’état mécanique de la tourelle sans anticiper cible, lock, capteurs ou AWACS.

## 9. Énergie et propulsion

### 9.1 `ENERGY_STATE`

`props=ets_properties(object)` est capturé une fois. `props==0` ou `Ship_Flags::No_ets` donne `ABSENT` et trois indices zéro. Sinon `popcount(props)<2` donne `LOCKED`; sinon `AVAILABLE`. Pour `LOCKED` sans `No_ets`, seuls les indices des domaines présents copient `ship::{shield,weapon,engine}_recharge_index`, les autres valent zéro ; `AVAILABLE` copie les trois indices applicables. Toute valeur non nulle reste dans `0..12`. Aucun état HUD n’intervient.

| Groupe | Politique Phase 2 |
|---|---|
| `WEAPON_ENERGY` | présent si `ship_has_energy_weapons(subject)`; courant `ship::weapon_energy`, max `ship_info::max_weapon_reserve`, clampés |
| `REGENERATION` | toujours présent ; taux instantanés reproduisant `update_ets`: `ets_power_factor × E[index] × ship.max_*_regen_per_second × max_resource`, avec la table publique `E[i]=i/12` pour `i=0..12` évaluée en `float64`, y compris multiplicateur skill joueur, délai après hit bouclier et zéro si dying/power output nul ; domaine absent = zéro |
| `DEFERRED_TRANSFERS` | présent si armes-énergie et boucliers existent, depuis `target_weapon_energy_delta` et `target_shields_delta`; absent sinon |
| `ENGINE_RESULT` | toujours absent : aucune autorité stockée ne fournit simultanément `resulting_engine_power` et la vitesse résultante |
| `POWER_OUTPUT` | toujours présent depuis `ship_info::power_output`, validé non négatif |
| `ENGINE_INTEGRITY` | présent si `ship::subsys_info[SUBSYSTEM_ENGINE].aggregate_max_hits>0`, depuis les deux agrégats canonisés |

Les indices restent `0..12`; ils ne sont pas normalisés sur le fil.

### 9.2 `PROPULSION_STATE`

Un réservoir existe si `ship_info.flags[Afterburner] && afterburner_fuel_capacity>0`. `FUEL` publie `ship::afterburner_fuel` clampé et la capacité ; `CONSUMPTION` publie `afterburner_burn_rate` et `afterburner_recover_rate`. Ces deux groupes sont présents avec le réservoir et absents sinon. `ENGAGEMENT` n’est présent que si `afterburner_last_end_time>0`. Avec `now_ms=timer_get_milliseconds()`, le mapper calcule `elapsed_ms=now_ms-afterburner_last_end_time`, qui doit être dans `0..86 400 000`; puis `cooldown_remaining_us=max(afterburner_cooldown_time×1000-elapsed_ms,0)×1000` et `time_since_last_stop_us=elapsed_ms×1000`. `minimum_to_engage=afterburner_min_start_fuel` et `fuel_at_last_engagement=afterburner_last_engage_fuel` doivent être dans `[0,afterburner_fuel_capacity]`. Avant le premier arrêt ou si une de ces validations échoue, le groupe est absent seulement dans le premier cas et la session est refusée dans le second ; aucun zéro d’initialisation n’est transformé en historique.

Les flags viennent exactement de : `AVAILABLE=réservoir`, `LOCKED=réservoir && Ship_Flags::Afterburner_locked`, `ACTIVE=PF_AFTERBURNER_ON`, `REQUESTED=réservoir && Ship_Flags::Attempting_to_afterburn`, `BOOSTER_ACTIVE=PF_BOOSTER_ON`, `GLIDE_ACTIVE=PF_GLIDING`, `GLIDE_FORCED=PF_FORCE_GLIDE`. `DYNAMICS` est présent avec le réservoir depuis `object::phys_info.forward_accel_time_const` et `afterburner_max_vel`; `ENGINE_WASH` est présent si `ship::wash_intensity>0`; `RCS` est toujours absent faute de réduction vectorielle canonique. `RCS_ACTIVE` vaut zéro en Phase 2. Les flags afterburner/booster/glide doivent être cohérents avec `FLIGHT_STATE.physics_mode_flags` lorsqu’ils partagent un sample, obligatoirement dans une keyframe. Un flag `AFTERBURNER_ACTIVE` exige disponible et non verrouillé. Le client ne déduit jamais actif de la seule demande `CONTROL_STATE`.

## 10. Armement et contre-mesures

### 10.1 `WEAPON_STATE`

Chaque vaisseau exporté possède un record contenant l’état global et les listes complètes des familles. Les counts égalent les longueurs. Un sélecteur sans banque vaut zéro ; un ID moteur `-1` ne quitte jamais l’adaptateur. Les flags globaux sont construits bit à bit depuis `Ship_Flags::{Primary_linked,Secondary_dual_fire,Primaries_locked,Secondaries_locked}`, `ship_weapon::flags::{Primary_trigger_down,Secondary_trigger_down,Beam_Free,Turret_Lock}`, la validité de `targeting_laser_bank/objnum` et `remote_detonaters_active>0`.

| Groupe global | Présence et source exactes |
|---|---|
| `PREVIOUS_PRIMARY`, `PREVIOUS_SECONDARY` | présents si l’index `ship_weapon::previous_*_bank` résout une banque publique courante de la bonne famille |
| `TARGETING_LASER` | présent, et flag posé, si `ship::targeting_laser_bank` résout une primaire et `targeting_laser_objnum` un beam actif valide |
| `SWARM` | présent si `num_swarm_missiles_to_fire>0` et `swarm_missile_bank` résout une secondaire ; valeurs exactes de ces champs |
| `REMOTE_DETONATION` | présent si `remote_detonaters_active>0`; durée `max(timestamp_until(detonate_weapon_time),0)` |
| `PER_BURST_ROTATION` | présent si une classe installée utilise un beam type 5 ; valeur `ship_weapon::per_burst_rot` |
| `TERTIARY` | présent si et seulement si `num_tertiary_banks>0`; le count doit être dans `1..64` et `current_tertiary_bank` dans `0..count-1`, sinon le record est refusé. Les six champs viennent des scalaires tertiaires et timestamps de `ship_weapon`; si count vaut zéro, le groupe est absent et le sélecteur doit être la valeur neutre du schéma |
| `COUNTERMEASURE` | présent si et seulement si le groupe de classe l’est (`cmeasure_max>0` et type valide) ; classe installée correspondante, courant `ship::cmeasure_count`, max selon `Countermeasures_use_capacity ? floor(cmeasure_max/cargo_size) : cmeasure_max`, cooldown depuis `cmeasure_fire_stamp`; courant doit rester dans `[0,max]`. Si la classe a capacité zéro, count doit être zéro et le groupe est absent. `LOCKED` si `!Countermeasures_enabled`, sinon `AVAILABLE` seulement si count>0 et cooldown écoulé |

Pour chaque banque primaire : classe depuis `primary_bank_weapons`, cooldown `next_primary_fire_stamp`, slot/fire point depuis `primary_next_slot` et `primary_firepoint_next_to_fire_index`, simultané depuis `primary_bank_slot_count`, et `pattern_id` depuis la résolution fermée de 3.4 (`0` pour `STANDARD`). Ammo/capacité/start et `primary_bank_rearm_time` sont présents si et seulement si l’arme est balistique. `BURST` vient de `burst_counter/burst_seed`, `SUBSTITUTION` de `primary_bank_substitution_pattern_index`. `ANIMATION` est toujours absent : l’enum `EModelAnimationPosition` ne fournit pas la position normalisée `[0,1]`. `FOF_COOLDOWN` est présent seulement si `weapon_info::fof_reset_rate>0`, avec `primary_bank_fof_cooldown / fof_reset_rate` converti en durée.

Pour chaque banque secondaire : classe depuis `secondary_bank_weapons`, cooldown/slot depuis `next_secondary_fire_stamp/secondary_next_slot`; ammo/start/capacité et réarmement sont présents sauf arme `AMMOLESS`; burst et substitution utilisent les portions secondaires des tableaux, et `ANIMATION` est toujours absent pour la même raison. La banque tertiaire publie uniquement les six champs autorisés par `TertiaryBankV1`. Aucune classe, animation ou burst tertiaire n’est inventé. Tout timestamp est converti par `max(timestamp_until(source),0)` et doit rester sous une heure ; toute valeur obligatoire invalide refuse le record complet.

La contre-mesure publie classe si connue, quantité courante/max, cooldown et flags disponible/verrouillé. Les deux flags sont mutuellement exclusifs.

### 10.2 Événements exclus

`WEAPON_FIRED`, beams, remote detonation, lancement de contre-mesure et impacts ne sont pas garantis en Phase 2. Les compteurs/munitions peuvent montrer leurs effets, mais `event_coverage_exact` reste zéro et le client NE DOIT reconstruire un tir exact par différence.

## 11. Cargo, docking et support

### 11.1 `CARGO_SCAN_STATE`

Le record est obligatoire pour fermer `CARGO_DOCK_SUPPORT`. Le calcul actuellement mêlé à `player_inspect_cargo()` est extrait dans `update_player_cargo_scan_authority(frametime, target)` mais reste invoqué exactement une fois depuis le chemin historique `hud_cargo_scan_update -> player_inspect_cargo`, sous les mêmes conditions de rendu/HUD. Cette fonction gameplay possède et mute `CargoScanAuthorityState`, `cargo_inspect_time` et la révélation ; elle n’est donc pas qualifiée de pure. Le HUD consomme le résultat sans recalcul, puis le **prochain** `telemetry::EngineUpdate` copie le dernier état possédé sans l’avancer, après revalidation de la signature/target. Ce choix préserve byte-for-byte la progression gameplay quand la télémétrie est absente ou désactivée. Le collecteur NE DOIT appeler ni `hud_sensors_ok`, ni `hud_targetbox_subsystem_in_view`, ni fabriquer une chaîne HUD.

Le mapping fermé est :

| Condition autoritaire après validation | État filaire |
|---|---|
| autorité non encore appelée, target/signature changée depuis son commit, aucun target ship, `Cannot_perform_scan_hide_cargo`, cible non scannable ou sous-système cargo requis absent | `NOT_SCANNABLE`, `HIDDEN`, aucun groupe optionnel |
| cargo ship/sous-système déjà `Cargo_revealed`, ou scan `No_scanned_cargo` terminé | `COMPLETED` en priorité ; `REVEALED` uniquement si le mode autorise le contenu, sinon `COMPLETED+HIDDEN` |
| cible scannable mais portée, angle, capteurs ou visibilité sous-système ne permettent pas la progression | `IDLE`; TARGET, TIMING et VALIDITY présents si `required_us>0`, disclosure cachée |
| portée, angle, capteurs et éventuelle visibilité sous-système vrais | `SCANNING`; TARGET, éventuel SUBSYSTEM, TIMING et VALIDITY présents |

`elapsed_us = Player->cargo_inspect_time × 1000`. `required_us` vaut l’override strictement positif `model_subsystem::scan_time`, sinon `Ship_info[target].scan_time`, multiplié en double par `Ship_info[player].scanning_time_multiplier`, puis par 1000 ; un résultat nul/non positif refuse `TIMING` et classe la source `NOT_SCANNABLE`. La portée reprend exactement les formules rayon + `scan_range_normal/capital` + `scanning_range_multiplier` du helper gameplay et exige strictement `current_target_distance < scan_dist`. `IN_ANGLE` exige `dot >= CARGO_MIN_DOT_TO_REVEAL`. `LINE_OF_SIGHT` ne réencode jamais les capteurs : il vaut vrai pour un ship entier validé et, pour un sous-système, copie seulement le test géométrique extrait de `hud_targetbox_subsystem_in_view`. Les capteurs indisponibles donnent `IDLE` avec flags géométriques éventuellement tous vrais. Les trois bits sont présents même à zéro dès qu’une cible scannable et un timing valide existent.

La progression et les resets reproduisent le gameplay existant : elle avance de `round(frametime×1000)` uniquement si portée, angle, capteurs et visibilité requise sont vrais ; angle faux ou sous-système hors vue remet le timer à zéro ; cargo révélé, completion ou changement de target dans le chemin HUD remet le timer à zéro ; hors portée, `Cannot_perform_scan_show_cargo` ou capteurs indisponibles le conservent. La completion utilise strictement `cargo_inspect_time > scan_time`, jamais `>=`. Pause ou frame sans appel historique ne mute ni timer ni autorité. Les fixtures couvrent chaque frontière `<`, `>=`, `>` et les resets asymétriques.

Si disclosure vaut `REVEALED`, `cargo_text` est construit sans localisation depuis les données gameplay : `none` pour index cargo zéro, sinon `Cargo_names[index]` après retrait d’un unique préfixe `#`; un `cargo_title` non vide et ne commençant pas par `#` préfixe `title + ": "`. UTF-8/511 octets sont validés sans troncature. `COMPLETED + HIDDEN` n’expose aucun texte. La Phase 2 ne fournit pas d’écran cargo ; le dashboard ne fait qu’afficher ces champs bruts.

Une cible non nulle doit déjà appartenir à la closure service/docking avant le scan. Si le moteur commence ou conserve un scan vers un autre ship, la Phase 2 termine la session avant de publier ce `CARGO_SCAN_STATE`; elle n’ajoute pas la cible comme nouvelle racine et ne révèle pas son état complet. `target_subsystem_id` n’est présent que si le sous-système existe dans le manifeste déjà installé de cette cible.

### 11.2 `DOCKING_STATE`

Chaque vaisseau exporté possède un record, avec `presence=0`, phase, leader public ou zéro et liste exhaustive de ses relations autorisées, maximum 64. Le collecteur parcourt directement `object::dock_list` avec un visited-set préalloué ; `dock_evaluate_all_docked_objects()` et `dock_find_dock_root()` sont interdits sur le fast path. Pour chaque lien, les index viennent des `dock_instance` locaux/inverses, sont validés `0..4095`, et les noms sont copiés depuis les dock-bays du `polymodel` correspondant.

La phase unique d’un sujet utilise la précédence suivante : `UNDOCKING` pour `AIM_DOCK + AIS_UNDOCK_0..4`, même si la relation existe encore ; sinon `DOCKED` si au moins une relation directe existe, y compris `AIS_DOCK_4/4A`; sinon `DOCKING` pour `AIS_DOCK_2/3`; sinon `APPROACH` pour `AIS_DOCK_0/1`; sinon `NONE`. La topologie matérialisée domine donc une seconde approche éventuelle, sauf pendant l’undocking actif.

`group_leader_entity_id` vaut l’ID de l’unique membre de la composante portant `Ship_Flags::Dock_leader`; il vaut zéro si aucun membre ne porte le flag. Deux leaders dans la même composante refusent la candidate. Le collecteur NE DOIT ni assimiler `dock_get_hub` à ce leader, ni inventer un leader par tri d’IDs ; la valeur peut donc revenir à zéro après la séquence d’arrivée/warp qui retire ce flag.

Chaque relation distante résout un autre lifecycle de la closure. Puisque chaque membre possède son propre `DOCKING_STATE`, la relation inverse est obligatoire et exacte. La fermeture suit transitivement ces relations avant construction du snapshot.

### 11.3 `SUPPORT_STATE`

Chaque vaisseau exporté possède un record. Pour un sujet assisté, `AWAITING_REPAIR` et `BEING_REPAIRED` copient les flags `Ai_info[subject]`; `SUPPORT_ENTITY` est présent seulement si `support_ship_objnum` est borné, son type/signature égale `support_ship_signature` et le ship appartient à la closure. Phase et terminal suivent la table du document 03. `NONE` et `ABORTED` omettent l’ancien support sauf si une affectation valide existe encore ; `OBSTRUCTED` peut le conserver si l’affectation persiste.

Pour un sujet qui assiste un autre ship, `REPAIRING_OTHER` copie `AI_Flags::Repairing` après validation de `goal_objnum`; sa phase reste `NONE` et `SUPPORT_ENTITY` est absent s’il n’est pas lui-même assisté. Le self-repair moteur est explicitement valide : si `support_objnum==subject.objnum` et signature concordante, `SUPPORT_ENTITY` contient le propre `entity_id`, la phase/les flags assistés suivent l’autorité, mais `REPAIRING_OTHER` reste nul. Cette auto-référence résout donc un lifecycle déjà membre et n’élargit pas la closure. Sans rôle dans aucun sens : `phase=NONE`, flags zéro, presence zéro.

Les latches terminales et leur coalescing suivent [03-flux-cycle-de-vie-et-concurrence.md](03-flux-cycle-de-vie-et-concurrence.md). Les raisons privées `Completed`/`Ended` alimentent uniquement métriques et oracle producteur : le fil contient `NONE` dans les deux cas, et le client NE DOIT afficher ni succès ni échec à partir de cette distinction invisible. Aucune nouvelle phase n’est créée.

### 11.4 Progressions client

Le tableau de bord calcule, quand les dénominateurs existent :

- réparation coque : `hull_strength / dynamic_max_hull` ;
- réparation boucliers : `sum(segment_current_hits) / sum(segment_max_hits)` ;
- réparation sous-système : `current_hits / max_hits` par sous-système ;
- rearm primaire/secondaire : `ammo_current / ammo_initial` ;
- rearm tertiaire : même formule ;
- contre-mesures : `quantity_current / quantity_max` ;
- délai banque : `rearm_remaining_us`, déjà brut et non normalisé.

Ces ratios décrivent l’état courant, pas une promesse d’achèvement à 100 %. Si la mission plafonne une réparation ou qu’un dénominateur est nul, le client affiche « indisponible » au lieu d’inventer un progrès. Une ETA ne peut être affichée que depuis une durée filaire ou un taux stable observé explicitement ; sinon elle est absente.

## 12. Image, baseline et delta

### 12.1 Atomes complets

Chaque `StateAtom` conserve valeur sérialisée complète, lifecycle, clé et éventuel owner cascade. Les listes à l’intérieur de `SHIELD_STATE`, `SUBSYSTEM_STATE`, `WEAPON_STATE`, `CARGO_SCAN_STATE` et `DOCKING_STATE` sont remplacées en bloc.

`RecordFlag.PARTIAL` reste interdit. Un retour exact à la valeur de baseline supprime l’atome du delta cumulatif ; il ne produit pas une mutation artificielle.

L'image courante peut être maintenue incrémentalement : une capture `FlightControl` remplace seulement les atomes flight/control concernés ; une capture `Systems` remplace seulement les atomes systèmes concernés. Le builder conserve les octets canoniques des autres atomes sans les réencoder. Le diff reçoit l'ensemble exact des clés reconstruites et ne rescane l'image entière que pour une reconstruction exhaustive, une cascade lifecycle, une variation de topologie/manifeste ou une vérification de keyframe. Cette optimisation ne modifie ni l'ordre canonique final, ni l'exhaustivité d'un snapshot, ni la sémantique cumulative contre baseline.

Le premier snapshot d’une session utilise exactement `SnapshotFlagInitial`. Toute keyframe planifiée ou forcée par période, topology, catalogue, lifecycle ou support utilise exactement `SnapshotFlagPeriodicKeyframe`. `SnapshotFlagResync` est réservé à une demande/réparation de baseline Phase 0. Aucun snapshot ne combine ces bits et `SnapshotFlagNone` est interdit.

### 12.2 Diff et suppressions

- `SUBSYSTEM_STATE` reste un upsert complet en Phase 2 ; changement d’ensemble = nouveau manifeste + keyframe, jamais `CREATE/DELETE` sous `CORE_SHIP`.
- Les records singleton par entité sont `UPSERT` et disparaissent par cascade lifecycle, pas par `DELETE` isolé.
- `SHIELD_STATE` sans bouclier est un upsert explicite, jamais une suppression.
- Les banques sont une liste interne de `WEAPON_STATE`; leur disparition remplace l’atome complet.
- Un changement de manifeste ne passe jamais par delta.

### 12.3 Cohérence croisée

Dans une même keyframe :

- classe lifecycle = classe `SHIP_IDENTITY` = classe installée ;
- rayon identity = rayon flight dans la tolérance de sérialisation ;
- flags propulsion = modes flight compatibles ;
- banks et weapon classes existent dans le manifeste ;
- chaque subsystem ID et canonical index correspondent à la définition de classe ;
- support/docking utilisent des IDs publics valides ;
- toutes les sample times sont égales au tick forcé.

Une incohérence refuse la candidate entière.

## 13. Tableau de bord de preuve

Le tableau de bord minimum comporte :

| Groupe | Valeurs brutes | Valeurs dérivées |
|---|---|---|
| identité/lifecycle | nom, classe, phase, flags | libellé vivant/mourant/absent |
| vol/commande | pose, vitesses, modes, axes, flags | vitesse scalaire et locale |
| coque/boucliers | HP/max, segments | ratios et totaux |
| énergie/propulsion | mode et indices ETS, énergie, carburant, flags, cooldown | ratios ; parts ETS marquées indisponibles en Phase 2 |
| armes | banques, classes, ammo, cooldown, sélection, contre-mesures | ratios d’ammo et disponibilité |
| sous-systèmes/tourelles | HP, flags, transforms, banks | ratios et état détruit |
| support | phase, flags, support ID, quantités/délais des autres blocs | ratios de remise en état, ETA seulement calculable |
| transport | baseline, manifest, sample ages | stale/converged |

Les formules de référence, évaluées en `float64`, sont fermées :

- `speed = sqrt(vx²+vy²+vz²)` ; `velocity_local = conjugate(orientation_local_to_world) ⊗ velocity_world`, avec le même ordre quaternion/vecteur que le convertisseur Phase 1 ;
- `hull_ratio = clamp(hull_strength/dynamic_max_hull,0,1)` si le maximum est strictement positif ;
- `shield_current_total = Σ segment_current_hits`, `shield_max_total = Σ segment_max_hits`, puis ratio si le total max est positif ; sans bouclier, totaux zéro et ratio indisponible ;
- `weapon_energy_ratio` et `fuel_ratio` utilisent courant/max de leurs groupes seulement si max>0 ;
- la table de calcul producteur/oracle reste `E=[0/12,1/12,2/12,3/12,4/12,5/12,6/12,7/12,8/12,9/12,10/12,11/12,12/12]`, chaque rationnel étant évalué en `float64` sans lecture du HUD. Toutefois le fil Phase 2 ne porte pas le masque exact de `ets_properties`: un indice zéro peut être applicable et `has_shields` ne couvre pas `Intrinsic_no_shields`. Le client indépendant affiche donc les trois indices bruts mais marque **toutes** les `ets_share` indisponibles ; il ne devine jamais l’applicabilité depuis `SHIELD_STATE`, les groupes optionnels ou la valeur de l’indice ;
- chaque ratio ammo/quantité/intégrité utilise `clamp(current/max,0,1)` si max>0 ; `subsystem_destroyed = (max_hits>0 && current_hits<=0)` ;
- le libellé lifecycle mappe directement `SPAWNING/ACTIVE/DEPARTING/DYING/DESTROYED/REMOVED`; l’absence signifie uniquement qu’aucun lifecycle de cet ID n’existe après cascade ;
- `block_period_us = ceil(1 000 000 / flightHz)` pour Flight/Control, `ceil(1 000 000 / systemsHz)` pour les systèmes et `missionHeartbeatMs×1000` pour les singletons. Le client calcule `estimated_producer_now_us = client_monotonic_time_us + smoothed_offset` avec le filtre NTP-style Phase 0, puis `age_us = estimated_producer_now_us-producer_sample_time_us`; un bloc est `stale` si `age_us > 3×block_period_us+100 000`. Filtre invalide, overflow ou âge négatif donnent âge/stale indisponibles et une erreur de preuve, jamais un âge clampé ;
- le rapport de preuve marque `converged` seulement si le dernier manifeste requis et la dernière keyframe sont `APPLIED`, aucune candidate/reliable dependency n’est en attente, la baseline du delta est connue, la source/closure est stable et le hash canonique décodé égale celui de l’oracle moteur. Le dashboard sans oracle affiche seulement `synchronized` pour les quatre premières conditions.

Toute division exige un dénominateur strictement positif ; le clamp ne modifie que l’affichage, jamais la valeur brute. Une entrée ou un groupe absent donne « indisponible ».

## 14. Interdictions de phase

Sont explicitement absents : `LOCK_STATE`, `TARGET_STATE`, `RADAR_STATE`, `RADAR_CONTACTS`, `THREAT_STATE`, `NAVIGATION_STATE`, `EFFECT_STATE`, records `COMM_*`, vidéo et tout record d’entité globale. Les cibles de tourelle, cargo local de sous-système, icône radar et événements de tir exacts restent également absents.

## 15. Preuves minimales

Le modèle est accepté seulement si les tests prouvent :

- cardinalité `9+N` de la gate cœur et formule `4+10K+N` du profil complet, dont le minimum `14+N` ;
- matrices de domaines positives et négatives ;
- manifestes vides d’armes, mono/multi-banques et fermeture modifiée ;
- zéro, une et 64 sections de bouclier, puis rejet à 65 ;
- zéro, un et 1024 sous-systèmes, puis rejet au-delà ;
- banques absentes, dynamiques, balistiques, énergétiques et tertiaires ;
- `No_ets`, sans afterburner, sans support et support terminal ;
- IDs statiques stables dans la génération et nouvel `entity_id` au respawn ;
- cohérences croisées et refus de chaque incohérence ;
- formules dérivées avec zéro/absence/non-fini et comparaison à l’oracle.

Ce document couvre principalement `P2-REQ-015` à `P2-REQ-033`, `P2-REQ-034`, `P2-REQ-036` à `P2-REQ-040` et `P2-REQ-047`.
