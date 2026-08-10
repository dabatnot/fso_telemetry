# 04 — Modèle de données et règles métier

## 1. Objet

Ce document fixe la manière dont la Phase 3 remplit les records FSTL existants.
Les layouts, enums, bits de présence, bornes et règles de décodage de la
[Phase 0](../0-Contrat-de-protocole/04-modele-de-donnees-v1.md) restent
inchangés et prévalent pour les octets.

## 2. Image canonique

### 2.1 Cardinalité avec joueur

Soient :

- `K >= 1`, le nombre de vaisseaux de la fermeture joueur/support/docking
  héritée ;
- `N`, le nombre total de leurs sous-systèmes ;
- `C`, le nombre de contacts radar autorisés, avec `0 <= C <= 4096`.

Un snapshot `CockpitSensors` contient :

| Record | Cardinalité |
|---|---:|
| matrice `CompleteShip` Phase 2 | `4 + 10K + N` |
| `LOCK_STATE` | 1 pour le joueur |
| `TARGET_STATE` | 1 pour le joueur |
| `RADAR_STATE` | 1 pour le joueur |
| `THREAT_STATE` | 1 pour le joueur |
| `HUD_ALERT_STATE` | 1 pour le joueur |
| `NAVIGATION_STATE` | 1 pour le joueur |
| `RADAR_CONTACTS` | `C` |

Le total est exactement `10 + 10K + N + C`. `CARGO_SCAN_STATE` appartient déjà
à la matrice Phase 2 et n’est pas compté deux fois.

Les records communs à plusieurs domaines apparaissent une seule fois. L’ordre
canonique reste `record_type` croissant puis clé binaire lexicographique.

### 2.2 Joueur absent

Sans joueur observé, l’image contient exactement `SESSION_STATE` et
`MISSION_STATE`. Les records à clé joueur, contacts, locks, menaces et
navigation sont absents. Le manifeste actif reste référencé et gelé ; aucun
record à ID zéro n’est fabriqué.

## 3. Manifestes Phase 3

### 3.1 Ensemble autorisé

La transaction `FullRequired` contient l’union :

1. des classes et armes de la fermeture `CompleteShip` ;
2. des classes de vaisseau effectivement référencées par un champ
   `revealed_class_id` ;
3. des classes d’armes effectivement référencées par un contact ou un
   `IncomingMissileV1`.

Une classe non révélée n’entre pas dans le manifeste. Les entrées restent
exhaustives dans leur génération et utilisent les mêmes IDs stables que les
records dynamiques.

### 3.2 `CLASS_MANIFEST.RADAR_ICON`

Le groupe `RADAR_ICON` est présent si et seulement si :

- la classe elle-même est autorisée dans le manifeste courant ;
- le nom canonique du type dans `objecttypes.tbl`, après trim ASCII et
  comparaison ASCII insensible à la casse, appartient au registre public ;
- `radar_icon_id` est normalisé dans le registre public ;
- sa valeur participe au fingerprint du manifeste.

Une définition présente mais invalide refuse la variante ; elle ne devient pas
une absence opportuniste. Le groupe ne contient jamais de texture, bitmap,
handle ou coordonnées écran.

Le registre est `0=Generic`, `1=Navbuoy`, `2=SentryGun`, `3=EscapePod`,
`4=Cargo`, `5=Support`, `6=Fighter`, `7=Bomber`, `8=Transport`,
`9=Freighter`, `10=Awacs`, `11=GasMiner`, `12=Cruiser`, `13=Corvette`,
`14=Capital`, `15=SuperCapital`, `16=Drydock`, `17=KnossosDevice`. Un type
moddé inconnu omet le groupe ; un consommateur recevant un ID futur inconnu
utilise son icône générique sans refuser le manifeste.

### 3.3 Révélation tardive

Un champ `revealed_class_id` ou `weapon_class_id` n’est sérialisé que si l’ID
existe dans le manifeste installé. Une nouvelle révélation qui nécessite une
définition absente entraîne manifeste `N+1`, puis keyframe. Le contact peut
rester publié sans son groupe de classe uniquement si le moteur ne révèle pas
encore cette classe ; le producteur ne retarde pas artificiellement une
révélation déjà autorisée pour éviter le changement de manifeste.

## 4. IDs publics et références

### 4.1 Catégories

Un même espace `entity_id` couvre :

- les entités matérialisées Phase 2 ;
- les pistes radar ;
- les cibles et anciennes cibles autorisées ;
- les missiles entrants ;
- les liens navigation vers une entité.

Une piste seule ne crée pas de lifecycle. Son ID reste une clé publique stable,
pas une promesse d’état complet.

Une piste en cours de spawn dont `radar_project_contact()` ne fournit pas encore
une projection finie est omise pour ce tick. Elle est réévaluée au tick suivant
et ne ferme jamais la capture complète.

### 4.2 Cohérence

Les relations suivantes sont obligatoires :

- un `TARGET_STATE.current_target_entity_id` égal à une piste radar présente
  utilise le même ID ; son absence de `RADAR_CONTACTS` reste légitime pour une
  cible prise en charge par le Target Box mais non projetée par le radar ;
- un lock vers cette cible utilise ce même ID ;
- à sample time égal, `ContactFlags.CURRENT_TARGET` est posé sur exactement le
  contact cible lorsqu’il existe ; à sample times différents, le flag reste
  l'historique du contact et `TARGET_STATE` reste l'autorité courante ;
- un missile présent à la fois dans `RADAR_CONTACTS` et `THREAT_STATE` conserve
  le même ID ;
- la cible de `CARGO_SCAN_STATE` réutilise l’ID cible ;
- un navpoint lié à une entité publique réutilise son ID.

Une référence à une entité matérialisée supprimée est invalide. Une référence de
piste momentanément absente n’est permise que par un champ dont la sémantique
autorise une dernière observation ou une connaissance de menace distincte.

## 5. `TARGET_STATE`

### 5.0 Version filaire

Le layout v1 reste lisible pour les captures existantes. Le profil live
`CockpitSensors` émet `record_version=5` sous FSTL 1.1. La variante v2 ajoute
après `EXACT_HUD_DISTANCE` le groupe `EXACT_HUD_SPEED`; v3 ajoute ensuite
`HUD_TYPE_LABEL`; v4 ajoute `HUD_TARGET_COLOR`; v5 ajoute enfin les libellés
HUD autoritaires `HUD_TARGET_SUBSYSTEM_LABEL` et `HUD_LOCK_SUBSYSTEM_LABEL`.
La variante est choisie
uniquement par `record_version`, jamais par la longueur du payload. Les
captures v1/v2/v3/v4 restent décodables et byte-identiques.

### 5.1 Champs obligatoires

`entity_id` est le joueur et `current_target_entity_id` vaut zéro en absence de
cible. `producer_sample_time_us` est l’instant de la décision de ciblage.
Le dashboard emploie ce champ comme seule autorité pour sélection, réticule,
emphase et priorité, y compris lorsque le radar conserve un échantillon plus
ancien.

### 5.2 Présences

| Groupe | Politique Phase 3 |
|---|---|
| `PREVIOUS_TARGET` | présent si le moteur conserve une cible précédente encore autorisée |
| `REVEALED_IDENTITY` | présent seulement pour les attributs effectivement révélés ; `revealed_class_id=0` reste permis selon FSTL |
| `TIME_ON_TARGET` | présent lorsque le compteur autoritaire est valide et borné |
| `TARGET_SUBSYSTEM` | présent si un sous-système public valide est ciblé |
| `LOCK_SUBSYSTEM` | présent si le lock vise un sous-système public valide |
| `LAST_STEALTH_OBSERVATION` | présent seulement si le cockpit conserve cette observation |
| `DISTANCE_TREND`, `SPEED_TREND` | décisions HUD autoritaires si disponibles |
| `IN_CONE` | décision autoritaire de cône, pas projection écran |
| `LEAD` | position monde et banque valide, ensemble all-or-nothing |
| `ATTACKER`, `DANGEROUS_WEAPON`, `NEAREST_LOCKED` | présents seulement si la référence est autorisée et résoluble dans l’espace public |
| `EXACT_HUD_DISTANCE` | valeur D visible : `Player_ai->current_target_distance` après multiplicateur HUD |
| `EXACT_HUD_SPEED` | depuis v2 ; valeur S visible calculée comme le target box, y compris le fallback docké et le multiplicateur HUD |
| `HUD_TYPE_LABEL` | depuis v3 ; seconde ligne exacte du Target Box, y compris la classe affichée d’un vaisseau ciblé même si aucune entrée `CLASS_MANIFEST` ne lui est encore applicable ; aucune classe n’est inventée côté client |
| `HUD_TARGET_COLOR` | depuis v4 ; couleur RGBA brillante retournée par `hud_get_iff_color(target,1)` pour les brackets et accents de cible, accessibilité et overrides inclus |
| `HUD_TARGET_SUBSYSTEM_LABEL` | depuis v5 ; texte exact, localisé et instance-aware retourné par `ship_subsys_get_name_on_hud()` pour le sous-système ciblé ; indépendant de `CLASS_MANIFEST` |
| `HUD_LOCK_SUBSYSTEM_LABEL` | depuis v5 ; même texte HUD autoritaire pour le sous-système de lock validé sur la cible courante ; indépendant de `CLASS_MANIFEST` |

Si la cible vaut zéro, tous les groupes sont absents sauf `PREVIOUS_TARGET`,
comme l’impose FSTL.

### 5.3 Lead

Le lead est la position monde choisie par la logique gameplay/HUD pour la
banque publiée. Il n’est pas recalculé par le collecteur. Si la banque n’est
plus valide ou si le lead n’existe pas, le groupe entier est absent.

Position relative, distance géométrique, temps d’interception, azimut,
élévation et coordonnées d’écran sont `D`.

## 6. `LOCK_STATE`

La liste est complète, ordonnée canoniquement par
`(target_entity_id,subsystem_id ou 0,world_position encodée)` et sans doublon.

Pour chaque `LockItemV1` :

- `target_entity_id` est public et non nul ;
- `SUBSYSTEM` est présent si et seulement si le point vise un sous-système
  public ;
- `locked` et `target_in_lock_cone` copient la décision moteur ;
- `world_position` est finie et bornée ;
- `LOCK_ATTEMPT` est présent si et seulement si une tentative existe ;
- `time_to_lock_remaining_us` est une durée bornée, jamais un timestamp moteur
  brut ni une sentinelle.

La liste vide signifie explicitement qu’aucun lock n’existe. Plus de 64 points
fait perdre le profil.

## 7. `RADAR_STATE`

| Champ ou groupe | Source sémantique |
|---|---|
| `radar_mode`, `selected_range` | mode et portée effective du cockpit |
| `sensor_state` | décision `OFFLINE`, `DEGRADED` ou `ONLINE` |
| hits capteurs | agrégat autoritaire courant/max |
| `BRIGHT_RANGE` | portée bright applicable |
| `PRIMITIVE_RANGE` | portée de secours applicable |
| `AWACS` | intensité et portée effectives de l’observateur |
| `EMP` | effet capteur courant et durée restante |

Les groupes FSTL optionnels `JAMMING` et `VISIBILITY_TIMES` ne sont pas publiés
par le profil `CockpitSensors` de Phase 3. Le moteur ne fournit ni intensité
globale autoritaire de brouillage ou de distorsion, ni horodatages de piste
correspondants. La distorsion est l’état discret `VISIBLE` ou `DISTORTED` de
chaque `RADAR_CONTACT` ; elle n’est donc pas extrapolée en valeur globale.

Le ratio d’intégrité capteur reste dérivé. `RadarMode.INFINITE` encode la borne
maximale finie prévue par FSTL, jamais un infini IEEE.

## 8. `RADAR_CONTACTS`

### 8.0 Version filaire

Les layouts v1/v2/v3 restent gelés. La version 2 insère
après `velocity_world` :

- `radar_local_position:vec3f`, calculé exactement comme le radar standard :
  `projected.world_position - Player_obj->pos`, puis rotation par l'orientation
  retournée par `object_get_eye(..., Player_obj, false)` ;
- `radar_projection_distance:float32`, copie exacte de
  `RadarContactProjection.distance`.

Les deux valeurs sont capturées dans le même appel de collecte et le même tick
que `radar_project_contact()`. La version 3 conserve ce préfixe byte-identique
et ajoute en fin de payload le groupe `HUD_TYPE_LABEL`, bit de présence `0x40`,
encodé par `hud_type_label:str<255>`. La version 4 ajoute ensuite le groupe
`RADAR_VISUAL`, bit `0x80`, composé de `radar_blip_color:rgba8` et
`radar_blip_type:u8` selon l'enum fermé FSO 0..5. Le profil live
`CockpitSensors` émet explicitement `record_version=4` sous FSTL 1.1. Un
décodeur choisit le layout par `record_version`, jamais par longueur. Les
records v2/v3/v4 sous FSTL 1.0 sont rejetés.

### 8.1 Ensemble exact

Le contact existe si et seulement si le pipeline radar/HUD conserve une piste
autorisée pour l’observateur au sample time. Le produit ne parcourt pas tous les
objets pour reconstruire une vérité omnisciente.

Les pistes visibles et distordues sont publiées. Une piste `NOT_VISIBLE` est
permise uniquement lorsqu’elle représente une connaissance retenue par le
cockpit ; un objet jamais détecté est absent.

### 8.2 Champs

- `object_type` et `category` décrivent seulement ce qui est révélé ;
- `visibility` copie `NOT_VISIBLE`, `VISIBLE` ou `DISTORTED` ;
- position et vitesse sont l’observation capteur autorisée ;
- la position locale radar et la distance de projection sont autoritaires,
  finies, capturées atomiquement et obligatoires en v2 ;
- `radius`, flags et taille logique suivent la décision radar ;
- pour un vaisseau `VISIBLE` en v3/v4, `REVEALED_NAME` contient la première ligne
  affichable du Target Box lorsqu'elle n'est pas masquée, et `HUD_TYPE_LABEL`
  contient son libellé de classe exact, type alternatif et traduction inclus ;
- `REVEALED_CLASS` reste indépendant et n'est présent que si son ID appartient
  déjà au manifeste installé ; le libellé HUD ne force aucun manifeste ;
- les groupes d'identité sont absents pour `DISTORTED`, `NOT_VISIBLE` et les
  objets non-vaisseaux, hors groupes historiques explicitement autorisés ;
- temps de détection et confiance sont présents seulement si l’autorité les
  fournit.

En v4, chaque contact projeté porte le groupe `RADAR_VISUAL` capturé juste
après `radar_project_contact()` : couleur RGBA finale, alpha compris, et type
de blip exact après les priorités warp, tagged, navbuoy/cargo, IFF, bombe/LSSM
et jump node. La sélection force `BRIGHT` mais ne remplace jamais cette couleur
par une teinte de dashboard. Une piste distordue conserve sa teinte et son
alpha autoritaires ; seule son animation reste une présentation client.

Sous v1/v2/v3, `ContactFlags.BOMB` exige un objet arme et une classe manifestée
portant le flag bombe. Sous v4, `BOMB`, `TAGGED` et `WARP` sont validés contre
`radar_blip_type` et n'exigent aucun manifeste. `CURRENT_TARGET` est unique dans
un même échantillon radar et implique `BRIGHT`. Sa comparaison avec
`TARGET_STATE` est stricte uniquement lorsque les deux records portent le même
`producer_sample_time_us`.
`STEALTH`, `HOMING` et `THREAT` ne peuvent révéler aucun champ conditionnel
supplémentaire.

### 8.3 Remplacement

Hors `DELETE`, chaque record est une valeur complète. Retirer un groupe
d’identité efface ses anciennes valeurs. Un `DELETE` contient seulement les
deux IDs de clé prévus par FSTL.

## 9. `THREAT_STATE`

Le niveau suit exactement l’enum fermé :

- `NONE` ;
- `DUMBFIRE` ;
- `LOCK_ATTEMPT` ;
- `LOCK_ACQUIRED`.

Les groupes nearest attacker, dangerous weapon et nearest homing sont présents
seulement si leur référence est autorisée. La liste des missiles contient tous
les missiles autorisés visant le joueur, au plus 256, triés par
`missile_entity_id`.

Chaque `IncomingMissileV1` respecte :

- classe d’arme installée ;
- cible égale au joueur parent ;
- sous-système homing seulement si applicable ;
- guidage fermé et visibilité radar ;
- position, quaternion et vitesse finis ;
- aucune estimation de distance, TTC ou impact sur le fil.

## 10. `HUD_ALERT_STATE`

Le profil live `CockpitSensors` publie `record_type=29`, `record_version=1`
uniquement sous FSTL 1.1. Le record est un singleton joueur à `flightHz` :

| Ordre | Champ | Wire | Présence et sémantique |
|---:|---|---|---|
| 1 | `entity_id` | `u64` | joueur observé non nul |
| 2 | `presence` | `u64` | bit 0 `ACTIVE_WARNING`, tous les autres réservés |
| 3 | `producer_sample_time_us` | `u64` | instant de capture du snapshot HUD |
| 4 | `primary_fire_threat_active` | `u8` | booléen canonique indépendant du lock |
| 5 | `missile_lock_state` | `u8` | `NONE`, `ATTEMPT` ou `ACQUIRED` |
| 6 | `warning_kind` | `u8` | bit 0 ; `LAUNCH`, `EVADED`, `COLLISION`, `BLAST`, `ENGINE_WASH`, `EMP` ou `OTHER` |
| 7 | `warning_instance_id` | `u64` | bit 0 ; non nul, renouvelé après acceptation native |
| 8 | `warning_remaining_us` | `u64` | bit 0 ; durée strictement positive au sample time |
| 9 | `warning_text` | `str<511>` | bit 0 ; texte UTF-8 exact, localisé et non vide |

L'absence du bit 0 signifie qu'aucun avertissement textuel n'est actif et omet
les quatre champs conditionnels. Un appel refusé par la priorité native ne
modifie pas l'instance. Les voyants primaire et lock peuvent être actifs
simultanément. La cadence d'affichage est dérivée côté client : 180 ms pour le
tir primaire ou `ATTEMPT`, 90 ms pour `ACQUIRED`. Aucune donnée d'animation
native ne traverse le wire.

## 11. `CARGO_SCAN_STATE`

La Phase 3 remplace la politique minimale Phase 2 :

- `TARGET` est présent dès qu’une cible de scan publique existe ;
- `SUBSYSTEM` est présent pour un sous-système public ciblé ;
- `TIMING` copie temps écoulé et requis effectifs ;
- `VALIDITY` copie les décisions portée, angle et ligne de vue ;
- `CARGO_TEXT` est présent si et seulement si la divulgation est `REVEALED`.

Une cible hors fermeture Phase 2 peut être référencée par son ID de piste sans
ajouter son état complet. Si elle cesse d’être autorisée, le record revient à
`NOT_SCANNABLE/HIDDEN` sans groupes révélateurs.

`COMPLETED+HIDDEN` est un état valide. Une chaîne vide ne remplace jamais une
absence de texte cargo.

## 12. `NAVIGATION_STATE`

### 12.1 Navpoints

La liste contient tous et seulement les points autorisés par la mission :

- `navpoint_id` non nul, unique et stable dans `mission_generation` ;
- nom UTF-8 autorisé ;
- type et flags fermés ;
- position monde finie ;
- liaison entité ou waypoint conforme au type et mutuellement exclusive.

Un point marqué `HIDDEN` n’est transmis que si la logique de mission l’autorise
néanmoins pour cet observateur. `NO_ACCESS` peut être transmis : il décrit une
décision publique, pas un secret.

### 12.2 Autopilote et route

- `current_navpoint_id` existe dans la liste ;
- `autopilot_refusal` est présent si et seulement si l’état vaut `REFUSED` ;
- route et index courant sont all-or-nothing ;
- `current_route_index < route.count` ;
- `route_speed_limit` est finie et bornée ;
- l’état engagé est cohérent avec `CONTROL_STATE.control_mode=AUTOPILOT`.

Le record ne contient aucun axe de commande, ordre IA ou commande distante.

## 13. Valeurs dérivées

Le client calcule, sans modifier les atomes bruts :

- position relative, distance, azimut et élévation ;
- vitesses relatives et vitesse de rapprochement ;
- temps d’interception et temps d’impact ;
- progression de lock et de scan ;
- âge courant d’une piste ;
- distance, relèvement et ETA de navigation ;
- coordonnées radar, brackets et animations ; la coordonnée radar
  live utilise exclusivement les deux champs v2 selon :
  `transverse=hypot(local.x,local.y)`,
  `radius=0` si `distance<local.z`, sinon
  `acos(local.z/distance)/PI`, puis
  `(local.x*radius/transverse,-local.y*radius/transverse)` ;
- ratios d’intégrité capteurs.

Une décision de visibilité, de lead, de lock, de validité de scan ou de menace
ne peut pas être remplacée par un calcul client.

Lorsque `transverse < 0.01`, la position centrale reste représentable pour un
contact avant, mais aucune direction latérale/verticale n'est définie. La
portée sélectionnée ne participe jamais au rayon : le collecteur a déjà omis
les contacts hors portée. La reconstruction monde + `FLIGHT_STATE` est
autorisée seulement pour lire une ancienne capture v1 et ne doit jamais être
  prioritaire sur un record v2. Les couleurs radar et cible v4 sont des
  décisions autoritaires consommées telles quelles, jamais des dérivations IFF
  du client.

La compatibilité dashboard est déclarative : une capture
`FSTL-dashboard-capture-v1` est une capture historique sans
`HUD_ALERT_STATE`, tandis que `FSTL-dashboard-capture-v2` déclare la feature
`HUD_ALERT_STATE/v1`. Le fallback `legacy-aggregated` depuis `THREAT_STATE`
n'est autorisé que pour le schéma v1 ; l'absence du singleton dans une capture
v2 ou une session live reste une absence de donnée autoritaire.

## 14. Validation croisée

Avant publication, le producteur vérifie :

1. cardinalités et tailles encodées ;
2. IDs non nuls, stables et non réutilisés ;
3. classes présentes dans le manifeste actif ;
4. cohérence cible/contact/lock/menace/cargo/navigation ;
5. unicité des contacts, locks, missiles et navpoints ;
6. masques de présence et enums fermés ;
7. finitude, bornes, ordre des temps et quaternions ;
8. absence de champ caché en `COCKPIT`.

Le client indépendant applique les mêmes règles et rejette atomiquement la
transaction en cas d’écart.

## 15. Traçabilité

Ce document couvre `P3-REQ-013` à `P3-REQ-018`, `P3-REQ-019` à
`P3-REQ-034`, `P3-REQ-037`, `P3-REQ-040` et `P3-REQ-047`.
