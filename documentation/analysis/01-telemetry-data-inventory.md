# Inventaire des données réplicables

## 1. Sources principales

L'état d'un vaisseau n'est pas contenu dans une structure unique.

| Domaine | Structure ou traitement principal | Source |
|---|---|---|
| Identité, pose, coque et boucliers | `object` | [`code/object/object.h`](../../code/object/object.h#L138-L182) |
| Physique et commandes | `physics_info`, `control_info` | [`code/physics/physics.h`](../../code/physics/physics.h#L43-L147) |
| Systèmes du vaisseau | `ship` | [`code/ship/ship.h`](../../code/ship/ship.h#L554-L866) |
| Banques d'armes | `ship_weapon` | [`code/ship/ship.h`](../../code/ship/ship.h#L95-L185) |
| Sous-systèmes | `ship_subsys` | [`code/ship/ship.h`](../../code/ship/ship.h#L343-L450) |
| Cible et menaces | `ai_info` | [`code/ai/ai.h`](../../code/ai/ai.h#L241-L465) |
| État joueur et scan | `player` | [`code/playerman/player.h`](../../code/playerman/player.h#L82-L219) |
| Locks missile | `lock_info` | [`code/ship/ship.h`](../../code/ship/ship.h#L309-L338) |
| Radar et visibilité | génération des blips | [`code/radar/radarsetup.cpp`](../../code/radar/radarsetup.cpp#L562-L672) |
| Navigation | `NavPoint` | [`code/autopilot/autopilot.h`](../../code/autopilot/autopilot.h#L18-L96) |
| Réplication existante | object updates | [`code/network/multi_obj.cpp`](../../code/network/multi_obj.cpp#L1233-L1567) |

### 1.1 Nature des données

L'inventaire distingue quatre natures de données. Cette distinction fait partie du contrat du protocole :

- **état autoritaire transmis (`A`)** : valeur dynamique lue chez le producteur et présente dans les snapshots ou deltas ;
- **catalogue transmis (`C`)** : définition statique ou rarement modifiée, référencée par un ID stable ;
- **valeur dérivée (`D`)** : calculée depuis des champs `A` ou `C` et non sérialisée comme une seconde source de vérité ;
- **événement (`E`)** : transition brève et ordonnée, identifiée par un `event_id`, qui ne doit pas dépendre de l'échantillonnage d'un snapshot.

Sauf mention contraire, les listes d'« état » ci-dessous décrivent des champs `A`. Les agrégats, ratios, coordonnées relatives, progressions et ETA sont `D`. Une extension de diagnostic peut les mettre en cache, mais elle doit alors les marquer explicitement comme dérivés et ne jamais les utiliser pour contredire leurs champs sources.

## 2. Enveloppe de session

Chaque message doit pouvoir être rattaché sans ambiguïté à une session et à un instant :

- version majeure et mineure du protocole ;
- identifiant aléatoire de session ;
- identifiant du producteur ;
- séquence de paquet ;
- identifiant de frame logique ;
- identifiant de snapshot de référence ;
- temps monotone du producteur ;
- temps de mission ;
- facteur de compression temporelle ;
- pause et état général de mission ;
- mode d'autorité : solo, client multijoueur ou serveur/master.

Les temps globaux sont notamment exposés dans [`code/globalincs/systemvars.h`](../../code/globalincs/systemvars.h#L74-L83).

## 3. Identité et cycle de vie des entités

Pour chaque objet exporté :

- `entity_id` de télémétrie ;
- `signature` et `net_signature` d'origine à titre de correspondance ;
- type d'objet : vaisseau, arme, astéroïde, jump node, etc. ; une bombe conserve le type `weapon` correspondant à `OBJ_WEAPON` et se reconnaît par le sous-type de sa classe d'arme et le flag `Weapon::Info_Flags::Bomb`, ce n'est pas un type d'objet distinct ;
- nom interne, nom affiché et callsign ;
- classe, espèce, équipe et IFF ;
- wing et position dans le wing ;
- joueur, IA, support ou objet de mission ;
- rayon et taille logique ;
- présence et phase de cycle de vie ; les transitions d'apparition, départ et disparition sont aussi émises comme événements `E` ;
- arrivée ou départ par warp/dockbay ;
- `dying`, `disabled`, `exploded`, `should_be_dead` ;
- furtivité, cloaking, visibilité capteurs et tag ;
- invulnérabilité, protection et guardian ;
- état scannable, fin de scan et divulgation du contenu cargo, selon les dimensions distinctes de la section 13 ;
- identifiants parent/enfant lorsqu'ils ont un sens réseau.

L'identité externe recommandée est une clé composée de la session et d'un ID monotone attribué par le module. `net_signature` peut être conservé dans le message, mais ne doit pas être supposé unique entre deux sessions.

Ne jamais utiliser comme identité publique :

- `objnum` ;
- `instance` ;
- `ship_info_index` ou `ai_index` ;
- un indice dans `Objects[]`, `Ships[]` ou `Weapon_info[]` ;
- un pointeur C++.

## 4. Pose et physique

### 4.1 État dynamique canonique

Le repère filaire reprend le repère monde de FS2Open. Dans le repère local d'un objet, `+X` est la droite (`rvec`), `+Y` le haut (`uvec`) et `+Z` l'avant (`fvec`). Les positions et vitesses linéaires sont exprimées dans le repère monde ; les vitesses angulaires sont exprimées dans le repère local, avec `x = pitch`, `y = heading/yaw` et `z = bank/roll`.

État autoritaire transmis (`A`) :

- `object::pos`, vecteur monde ;
- `object::orient`, converti en quaternion unitaire qui transforme le repère local vers le repère monde ; l'ordre filaire est obligatoirement `(w, x, y, z)`, `w` étant la composante scalaire ;
- `physics_info::vel`, vitesse linéaire monde, source canonique de toutes les vitesses scalaires ou locales ;
- `physics_info::rotvel`, vitesse angulaire locale en radians par seconde ;
- rayon de collision ou d'affichage.

Comme `q` et `-q` représentent la même rotation, le producteur impose un signe canonique avant comparaison et sérialisation : `w >= 0`, puis, si `w == 0`, la première composante non nulle parmi `x`, `y`, `z` est positive. Il renormalise le quaternion avant envoi.

Valeurs dérivées (`D`), non transmises dans le schéma canonique :

- `speed = norm(vel)` et `fspeed = dot(fvec, vel)` ;
- vitesse locale `(latérale, verticale, avant)`, obtenue en faisant tourner `vel` du monde vers le repère local ;
- accélération estimée par différence de vitesses, si le client en a besoin.

`physics_info::speed` et `physics_info::fspeed` sont des caches moteur de ces calculs. `physics_info::acceleration` est seulement la tendance instantanée de la vitesse en unités monde par seconde carrée et ne détermine pas l'intégration future ; il peut être exposé dans un canal de diagnostic, marqué comme tel, mais n'est pas une seconde source canonique.

### 4.2 Commande et prédiction

Ces champs `A` sont optionnels et servent à une prédiction avancée ou au diagnostic ; la pose et la vitesse de 4.1 restent autoritaires :

- `desired_vel`, vitesse cible monde ;
- `desired_rotvel`, vitesse angulaire cible locale ;
- `prev_ramp_vel`, vitesse de rampe locale ;
- `max_vel`, `afterburner_max_vel` et `booster_max_vel`, caps locaux par axe ;
- `max_rotvel`, caps angulaires locaux en radians par seconde ;
- `max_rear_vel`, cap de marche arrière ;
- `glide_cap` et `cur_glide_cap` ;
- `gravity_const`, multiplicateur sans dimension appliqué à la gravité de mission ;
- `forward_accel_time_const`, `afterburner_forward_accel_time_const`, `booster_forward_accel_time_const`, `forward_decel_time_const`, `slide_accel_time_const` et `slide_decel_time_const`, qui sont des **constantes de temps en secondes**, pas des accélérations ;
- `rotdamp`, constante de temps exponentielle pour le joueur ; pour l'IA et les missiles, le moteur l'utilise comme approximation polynomiale dont environ `2 * rotdamp` donne le temps d'accélération total ;
- `side_slip_time_const`, constante de temps latérale en secondes.

`linear_thrust` et `rotational_thrust` sont deux `vec3<float32>` dont chaque composante est bornée à `[-1, 1]`. Ils sont **uniquement cosmétiques** ; un producteur peut les transmettre pour les propulseurs et animations, mais jamais pour prédire ou valider le mouvement physique.

### 4.3 Modes physiques

- afterburner et booster actifs ;
- glide actif ou forcé ;
- Newtonian damping ;
- translation latérale ;
- warp entrant ou sortant ;
- mouvement scripté ;
- influence de shockwave ;
- objet immobile ou orientation verrouillée.

Le quaternion filaire est la seule orientation canonique. La matrice, le cap, le tangage, le roulis, les vecteurs avant/droite/haut et le flight-path marker sont dérivés côté client.

## 5. Entrées et aides du pilote

La réplication des entrées est optionnelle pour l'affichage, mais utile au replay et au diagnostic :

- `pitch`, `heading`/yaw, `bank`, `vertical`, `sideways` et `forward` : `float32` bornés à `[-1, 1]` ;
- `forward_cruise_percent` : `float32` borné à `[-100, 100]` ;
- `afterburner_start` et `afterburner_stop` : fronts booléens du tick source ; pour un replay exact, ils sont conservés comme événements `E` ;
- `fire_primary_count`, `fire_secondary_count` et `fire_countermeasure_count` : compteurs entiers non négatifs de demandes accumulées pendant le tick source, sérialisés en `uint16` avec saturation ; ils décrivent l'entrée, pas les tirs effectivement créés ;
- match speed ;
- auto-target et auto-match speed ;
- mode de contrôle ;
- mode curseur de vol, angles en radians, sensibilité et deadzone normalisée dans `[0, 1]` ;
- liens primaires et double tir secondaire.

Toute valeur hors plage est rejetée ou bornée par le producteur avant sérialisation. Ces valeurs ne remplacent jamais l'état physique autoritaire. Les tirs réels sont des événements d'armement décrits en section 9, et non déduits de ces demandes.

## 6. Coque, boucliers et dommages

### 6.1 Coque

- `hull_strength` ;
- `sim_hull_strength` ;
- coque maximale dynamique ;
- armor et guardian ;
- invulnérabilité ;
- dommages cumulés et provenance ;
- dernier auteur de dommage ;
- états normal, endommagé, critique, disabled, dying et détruit.

Les HP courants et le maximum dynamique sont les champs `A`. Le ratio de coque, ainsi que les seuils d'affichage « endommagé » et « critique », sont `D`. L'état `disabled` du vaisseau est un flag autoritaire distinct et ne se déduit pas du seul ratio de coque.

### 6.2 Boucliers

- nombre dynamique de segments ;
- HP de chaque segment ;
- maximum de chaque segment ;
- recharge maximale ;
- taux de régénération ;
- transfert d'énergie différé ;
- présence ou absence de boucliers ;
- quadrant/segment et position des impacts.

`shield_quadrant` est dynamique : le protocole ne doit pas supposer quatre quadrants.

Les tableaux de HP courants et maximums sont la source `A`. Le total courant, le maximum total, le ratio total et les ratios par segment sont `D` et ne sont pas dupliqués dans l'état canonique.

### 6.3 Événements de dommage

- impact bouclier ;
- impact coque ;
- segment de bouclier épuisé/restauré ;
- sous-système endommagé, perturbé ou détruit ;
- vaisseau disabled ;
- début de mort et destruction ;
- source, arme, position, normal et quantité de dommage lorsqu'ils sont connus.

Les flashs et timers HUD ne sont pas transmis ; le client produit ses propres animations à partir de ces événements.

## 7. Sous-systèmes

Les types standards sont définis dans [`code/model/model.h`](../../code/model/model.h#L63-L75).

Pour chaque sous-système :

- ID stable dans le parent et index canonique ;
- nom interne, nom alternatif et nom HUD ;
- type : moteurs, tourelle, radar, navigation, communications, armes, capteurs, etc. ;
- HP courants et maximums ;
- perturbé ;
- durée de perturbation restante ;
- guardian, armor et flags ;
- ciblable, visible et révélé ;
- géométrie locale et rayon, référencés depuis le catalogue ;
- transformation animée courante ;
- cargo local et visibilité du cargo ;
- agrégats courants et maximums par type.

Le ratio d'intégrité et `destroyed = (current_hits <= 0)` sont `D`. Il n'existe pas de champ dynamique indépendant « disabled » dans `ship_subsys` : si l'interface emploie ce terme pour un sous-système, il est dérivé de ses HP, de son type et des règles d'agrégation. Le flag `Ship_Flags::Disabled` du vaisseau reste un état `A` séparé. Les agrégats par type sont transmis seulement lorsqu'ils constituent la décision autoritaire du moteur ; leur ratio reste `D`. La pose monde d'un sous-système est `D` depuis la pose du parent, sa géométrie locale et sa transformation animée.

Pour les sous-modèles animés :

- angle et translation courants ;
- vitesse et cible de rotation/translation ;
- état et durée d'animation ;
- verrouillage des mouvements.

Pour les tourelles :

- cible et sous-système ciblé ;
- direction actuelle ;
- point visé et vélocité estimée ;
- prochain point de tir ;
- cooldown restant ;
- temps de cible en portée ;
- portée optimale ;
- priorité de ciblage ;
- imprécision ;
- multiplicateur de cadence ;
- état d'animation ;
- banques et munitions propres ;
- beam free/locked ;
- données swarm ;
- intensité et rayon AWACS.

Les tirs de tourelle suivent eux aussi le modèle d'événement `WEAPON_FIRED` de la section 9 ; les caches « dernier tir » et « dernière classe tirée » ne sont pas répliqués comme état.

## 8. Énergie, ETS et propulsion

Les index ETS sont stockés dans [`code/ship/ship.h`](../../code/ship/ship.h#L662-L685) et vont de 0 à 12.

- index ETS boucliers, armes et moteurs ;
- énergie d'arme courante et maximale ;
- régénération d'arme et de bouclier ;
- transferts différés vers armes et boucliers ;
- puissance moteur résultante ;
- vitesse maximale résultante ;
- `power_output` ;
- ETS disponible, absent ou verrouillé ;
- intégrité agrégée des moteurs.

Les index ETS et les quantités d'énergie sont les sources `A`. Les fractions ETS normalisées et les ratios d'énergie ou d'intégrité sont `D`.

Afterburner et propulsion :

- disponible, verrouillé, actif et demandé ;
- carburant courant et maximum ;
- consommation et récupération par seconde ;
- quantité minimale et cooldown ;
- temps depuis le dernier arrêt ;
- carburant au dernier engagement ;
- constante de temps d'accélération et vitesse maximale afterburner ;
- booster et glide ;
- intensité de l'engine wash ;
- activité RCS.

Le ratio de carburant est `D`.

## 9. Armement

`ship_weapon` ne possède pas un schéma uniforme pour les trois familles de banques. Le protocole utilise donc trois enregistrements distincts et ne fabrique pas de champs tertiaires absents du moteur.

### 9.1 État global autoritaire

- nombres de banques primaires, secondaires et tertiaires ;
- sélecteurs de banque primaire courante et précédente ;
- sélecteurs de banque secondaire courante et précédente ;
- sélecteur de banque tertiaire courant ;
- primaires liées, double tir secondaire, gâchettes maintenues et primaires/secondaires verrouillées ;
- targeting laser et banque utilisée ;
- salves swarm restant à lancer et banque d'origine ;
- `remote_detonaters_active` et durée restante avant autorisation de détonation distante ;
- `per_burst_rot`, rotation accumulée globale des beams de type 5 ;
- flags globaux de `ship_weapon`, notamment beam free/locked pour les tourelles.

Les sélecteurs sont globaux à la famille et ne sont pas répétés dans chaque banque.

### 9.2 Banque primaire

Pour chaque banque primaire :

- index et ID stable de banque ;
- ID de classe d'arme ;
- pour une primaire balistique seulement, munitions courantes, munitions initiales et capacité brute de la banque ;
- `next_fire_remaining_s`, durée en secondes avant le prochain tir autorisé ;
- `rearm_remaining_s`, durée en secondes avant le prochain lot de réarmement, pour une banque balistique ;
- prochain slot et prochain index de point de tir ;
- nombre de slots tirés simultanément et pattern de tir dynamique ;
- compteur et graine de burst ;
- index du pattern de substitution ;
- position d'animation et durée restante ;
- cooldown FOF courant.

Le nombre maximal de projectiles effectivement chargeables et le ratio de munitions sont `D`, calculés depuis la capacité brute, `cargo_size` et les règles de la classe d'arme.

### 9.3 Banque secondaire

Pour chaque banque secondaire :

- index et ID stable de banque ;
- ID de classe d'arme ;
- munitions courantes, munitions initiales et capacité brute, sauf pour une classe explicitement sans munitions ;
- `next_fire_remaining_s` et `rearm_remaining_s`, en secondes ;
- prochain slot ;
- compteur et graine de burst ;
- index du pattern de substitution ;
- position d'animation et durée restante.

Le double tir, le verrouillage et la banque sélectionnée appartiennent à l'état global. Le nombre maximal effectivement chargeable et les ratios de munitions sont `D`.

### 9.4 Banque tertiaire

La représentation tertiaire reflète les champs scalaires disponibles :

- nombre de banques et index courant ;
- munitions courantes, munitions initiales et capacité ;
- `next_fire_remaining_s` ;
- `rearm_remaining_s`.

Il n'existe pas dans `ship_weapon` de tableau tertiaire de classes d'arme, de banque précédente, de prochain slot, de cooldown FOF, de burst, de substitution ou d'animation. Ces champs sont donc absents du schéma tertiaire.

### 9.5 Tirs et autres transitions brèves

Un tir effectif est un événement `WEAPON_FIRED` (`E`) contenant au minimum `event_id`, temps producteur, tireur, famille et ID de banque, classe d'arme et point de tir ; les informations de burst, la cible et les projectiles créés sont ajoutés lorsqu'ils existent. Le début et la fin d'un beam continu sont deux événements explicites.

« Tirs effectués pendant la frame » et « dernière arme tirée » ne sont pas des champs d'état : un delta pourrait ne jamais observer leur valeur brève. Une interface qui en a besoin les dérive du journal d'événements. La détonation distante et le lancement d'une contre-mesure sont également des événements `E`.

Contre-mesures :

- classe sélectionnée ;
- quantité courante et maximum ;
- cooldown restant ;
- disponible ou verrouillée ;
- événement de lancement.

## 10. Cible, lead et verrouillages

État de ciblage issu principalement de [`code/ai/ai.h`](../../code/ai/ai.h#L241-L403) :

- cible courante et précédente ;
- type, nom, classe, équipe et IFF révélés ;
- temps passé sur la cible ;
- sous-système ciblé ;
- sous-système en cours de lock ;
- dernière position/vitesse connue d'une cible furtive ;
- tendances distance et vitesse ;
- cible dans le cône ;
- position de lead monde, validité et banque associée ;
- attaquant courant ;
- arme dangereuse ;
- cible verrouillée la plus proche et sa distance.

La cible et le sous-système, les informations effectivement révélées, la dernière observation furtive et les décisions du producteur telles que le lead complexe sont `A`. La distance centre-à-centre, la vitesse de la cible, les vitesses relatives et les ratios d'intégrité sont `D`, à partir des poses, vitesses et HP canoniques. Une distance HUD dont les règles ne sont pas reproductibles est transmise comme résultat autoritaire distinct et explicitement nommé.

Pour chaque `lock_info`, les champs bruts utiles transmis (`A`) sont :

- IDs de cible et de sous-système, convertis depuis les pointeurs locaux ;
- `world_pos`, point de lock monde ;
- `locked` ;
- `target_in_lock_cone` ;
- `time_to_lock`, durée restante en secondes, avec sa valeur sentinelle explicitement encodée comme absence de tentative.

Le type de point se déduit de la présence d'un sous-système. La progression normalisée se dérive de `time_to_lock` et du `min_lock_time` de la classe d'arme ; la validité de portée se dérive de la distance canonique et des portées de cette classe. Les coordonnées `current_target_s*`, `indicator_*`, `dist_to_lock`, les accumulateurs en pixels, `catching_up`, `maintain_lock_count`, `lock_gauge_time_elapsed` et `lock_anim_time_elapsed` sont des caches HUD locaux et sont exclus du protocole public.

### 10.1 Vue 3D haute résolution de la cible

La couche 3D du moniteur de cible est un flux visuel optionnel, séparé de l'état canonique. FS2Open rend un nouveau viewport hors écran à la résolution négociée ; le protocole n'agrandit pas la vue HUD historique.

État de contrôle du flux :

- actif ou arrêté ;
- `stream_id` ;
- génération de configuration ;
- `target_entity_id` auquel appartient chaque frame ;
- type de cible et disponibilité du chemin de rendu ;
- profil `MfdHigh` ou `HudExact` ;
- codec, profil et niveau H.264 ;
- largeur, hauteur et cadence ;
- bitrate cible ;
- durée maximale du GOP ;
- mode d'overlay, initialement `Client` ;
- identifiant de frame vidéo et temps de présentation monotone ;
- IDR, discontinuité et changement de cible ;
- état `Starting`, `Live`, `Stale`, `Unsupported` ou `Stopped` ;
- statistiques de frames rendues, abandonnées, encodées et reçues.

Les pixels encodés ne sont jamais intégrés à `FULL_SNAPSHOT` ou `DELTA`. Les frames inter utilisent des messages `TARGET_VIDEO_FRAME` non fiables et remplaçables ; la dernière IDR bénéficie d'une retransmission sélective bornée par une échéance. La configuration du flux, son arrêt et les demandes d'IDR sont des messages de contrôle fiables au niveau applicatif.

Le profil `MfdHigh` utilise par défaut le modèle principal avec un LOD adapté à un affichage pouvant atteindre 1024. Le profil `HudExact` conserve `$POF target file` et `$POF target LOD` lorsqu'ils existent. Cette distinction évite d'afficher en haute résolution un modèle qui aurait été simplifié pour les 131 × 112 pixels du gauge original.

La première version encode uniquement le rendu 3D sur fond noir. Le client utilise les données `TARGET_STATE`, dégâts et sous-systèmes pour redessiner localement les textes, brackets, jauges, couleurs IFF et statuts. Une frame n'est présentée que si son `target_entity_id` correspond encore à la cible courante de la réplique.

La spécification complète se trouve dans [07 — Vue de cible 3D haute résolution](07-high-resolution-target-view.md).

Le client dérive position relative, azimut, élévation, vélocité relative, vitesse de rapprochement, temps d'interception et orientation relative.

Les coordonnées écran, triangles, pixels de lock et animations restent locales au client.

## 11. Radar, capteurs et contacts

État global :

- mode et portée radar sélectionnés ;
- portée brillante ;
- état et intégrité des capteurs ;
- portée des capteurs primitifs ;
- état AWACS ;
- EMP, brouillage et distorsion ;
- dates de première visibilité et dernier contact.

Pour chaque contact :

- ID stable ;
- type et catégorie radar ;
- état `NOT_VISIBLE`, `VISIBLE` ou `DISTORTED` ;
- position et vélocité monde ;
- rayon et taille d'icône ;
- équipe/IFF, nom et classe si identifiés ;
- bright/dim ;
- cible courante ;
- stealth, tagged et warp ;
- bombe, homing et menace ;
- âge du contact et dernier instant de détection ;
- niveau de confiance éventuel.

La position et la vélocité monde sont les sources canoniques `A`. Position relative, distance, azimut et élévation sont `D` dans le repère du vaisseau observateur. La catégorie « bombe » d'un contact signifie ici `OBJ_WEAPON` dont la classe porte le flag `Bomb`.

Le filtrage capteurs/AWACS/furtivité doit être effectué côté producteur afin
de ne jamais sérialiser un objet caché. Les outils de diagnostic et de replay
consomment exactement la même projection cockpit.

## 12. Missiles entrants et alertes

- niveau agrégé : aucune, dumbfire, tentative de lock, lock acquis ;
- attaquant le plus proche ;
- arme dangereuse ;
- missile homing le plus proche et distance ;
- liste complète des missiles visant une entité répliquée.

Pour chaque missile entrant :

- ID et classe ;
- type de guidage ;
- position, orientation et vitesse ;
- cible et sous-système homing ;
- visibilité radar.

Distance, relèvement, vitesse de rapprochement et temps d'impact estimé sont `D` depuis les états cinématiques canoniques.

## 13. Cargo, scan, docking et support

Scan :

- phase autoritaire `not_scannable`, `idle`, `scanning` ou `completed` ;
- divulgation du contenu distincte : `hidden` ou `revealed` ;
- cible et sous-système ;
- temps écoulé et temps requis ;
- portée, angle et ligne de vue valides ;
- texte cargo uniquement lorsque la divulgation vaut `revealed`.

La fin du scan et la divulgation ne sont pas synonymes. Avec `Ship_Flags::No_scanned_cargo`, un scan peut être `completed` tout en gardant le contenu `hidden` ; le client affiche alors « scanné » sans recevoir le cargo. La progression normalisée est `D`, calculée depuis les deux durées. Les booléens de portée, d'angle et de ligne de vue sont les décisions autoritaires du producteur lorsqu'ils conditionnent la phase.

Docking :

- topologie complète des objets dockés ;
- IDs des deux objets ;
- dockpoints locaux et distants, index et noms ;
- leader du groupe ;
- phase autoritaire `none`, `approach`, `docking`, `docked` ou `undocking`, issue de la topologie et des modes/sous-modes IA.

Le moteur ne maintient pas de scalaire canonique de progression du docking. Progression et ETA sont donc `D`, estimées depuis la phase, le chemin, les poses et les vitesses ; si le producteur les fournit comme aide d'interface, elles restent explicitement marquées comme estimations dérivées.

Support :

- état `none`, `requested`, `approaching`, `docking`, `repairing`, `rearming`, `obstructed`, `aborted` ;
- ID du support ;
- awaiting repair, being repaired et repairing.

Les ETA d'approche, docking et réarmement ainsi que les progressions de réparation et de munitions sont `D` à partir de la phase, des poses, des HP, des stocks et des intervalles de réarmement. Elles ne concurrencent pas l'état de support autoritaire.

## 14. Navigation et autopilote

- liste des navpoints autorisés ;
- ID, nom, type et position monde ;
- liaison éventuelle à une entité ou un waypoint ;
- hidden, noaccess et visited ;
- navpoint courant ;
- autopilote engagé ou autorisé ;
- motif de refus ;
- liste de waypoints, index courant et limite de vitesse.

Distance, relèvement et ETA vers un navpoint sont `D` depuis les poses et vitesses canoniques.

## 15. Vue de communication (« Talking Head »)

La vue affichée pendant un message n'est pas une vidéo encodée dans le flux réseau. FS2Open sélectionne une animation locale, la fait progresser dans le gauge `HudGaugeTalkingHead` et joue éventuellement une voix séparée. La première version réplique uniquement la vue animée ; la restitution audio reste hors de ce lot.

État courant à exposer :

- `active` ;
- `playback_id` unique dans la session ;
- `engine_message_id` permettant la corrélation avec le message de mission, distinct du `message_id` de fragmentation UDP ;
- `sender_entity_id`, avec la valeur `0` lorsqu'aucun vaisseau émetteur n'existe ;
- `head_asset_id` de l'animation effectivement résolue ;
- temps monotone du producteur au moment de l'échantillon ;
- position courante dans l'animation, en microsecondes ;
- durée totale attendue, à titre de validation ; le nombre d'images appartient au manifeste d'asset ;
- mode boucle ou lecture unique ;
- `playback_rate` signé : `0` pendant une pause, positif en lecture avant, négatif en lecture inverse et intégrant la compression temporelle ;
- animation teintée par la couleur du HUD ou rendue en pleine couleur ;
- raison de fin : terminée, interrompue, remplacée, gauge HUD désactivé, changement de mission ou arrêt de session.

Le nom de base déclaré par la mission ne suffit pas. Le producteur doit référencer l'asset finalement choisi après application des suffixes de persona et du choix éventuel d'une image de départ aléatoire. La sélection est effectuée dans [`message_play_anim()`](../../code/mission/missionmessage.cpp#L1409-L1562), puis le gauge peut encore choisir un offset initial dans [`HudGaugeTalkingHead::render()`](../../code/hud/hudmessage.cpp#L1299-L1314).

Catalogue statique des assets de communication :

- ID stable dans le bundle ;
- nom logique résolu, sans dépendre d'un chemin absolu ;
- format source : ANI, EFF ou APNG ;
- format livré au client et version du convertisseur ;
- nombre d'images, durée totale et cadence ou durées par image ;
- largeur, hauteur et canal alpha ;
- hash du contenu livré ;
- hash/version du bundle ;
- asset de cadre du gauge, si le client reproduit l'habillage FS2Open ;
- asset de remplacement à utiliser si l'animation est absente.

Les formats animés reconnus sont listés dans [`code/bmpman/bmpman.cpp`](../../code/bmpman/bmpman.cpp#L64-L72). Le client peut lire ces formats directement ou recevoir une conversion hors ligne en APNG, WebM sans audio ou atlas d'images. Les fichiers eux-mêmes ne sont pas inclus dans les snapshots et deltas temps réel.

Les événements indispensables sont :

- début d'une lecture ;
- remplacement par un message plus prioritaire ;
- fin ou interruption ;
- correction de l'offset et de `playback_rate` lors d'un changement, d'une keyframe ou d'une resynchronisation.

Un client rejoignant une session en cours reçoit l'état courant dans le snapshot complet. Il calcule la position à afficher à partir de l'offset échantillonné, de l'horloge du producteur et de `playback_rate`, puis applique un modulo euclidien en boucle ou borne la valeur entre zéro et la durée. Un asset manquant ne bloque jamais la réplique : le client affiche un placeholder et conserve l'état du message.

## 16. États spéciaux et visuels

Canal optionnel ou basse fréquence :

- intensité et décroissance EMP ;
- tags et temps restant ;
- cloaking et stealth ;
- arcs électriques et sparks ;
- activité RCS ;
- portes de baie ;
- glow banks ;
- intensités de propulseurs ;
- warp et death roll ;
- targeting laser ;
- alerte de munitions ;
- couleurs d'équipe ;
- autoaim FOV ;
- animations de sous-systèmes utiles à une visualisation 3D.

## 17. Catalogues statiques

Les catalogues sont envoyés au début de session, lors d'une apparition ou lorsqu'une définition change. Ils ne sont pas inclus dans chaque frame.

### 17.1 Classe de vaisseau

- ID stable, nom, espèce et type ;
- masse, centre de masse, inertie et amortissements ;
- vitesses maximales et constantes de temps d'accélération/décélération en secondes ; les champs de table `forward_accel`, `forward_decel`, `slide_accel`, `slide_decel` et `afterburner_forward_accel` alimentent des `*_time_const` et ne sont pas des accélérations en unités monde par seconde carrée ;
- coque et boucliers maximums ;
- puissance, réserves et régénérations ;
- configuration afterburner ;
- contre-mesures ;
- banques, capacités et points de tir ;
- définitions et géométrie des sous-systèmes ;
- temps et paramètres de scan ;
- glide et autoaim ;
- informations d'icône radar.

La structure commence dans [`code/ship/ship.h`](../../code/ship/ship.h#L1151).

### 17.2 Classe d'arme

- ID stable, nom et titre ;
- sous-type et flags ;
- vitesse maximale, `acceleration_time` en secondes, masse et multiplicateur de gravité ;
- durée de vie ;
- portée minimale, optimale et maximale ;
- `fire_wait_s`, intervalle en secondes entre deux tirs, et énergie par tir ;
- dommages et effets ;
- guidage, FOV, temps et type de lock ;
- héritage de vitesse ;
- taille cargo, `rearm_interval_s` correspondant au champ interne `rearm_rate` mais exprimant l'intervalle en secondes entre deux lots, et `reloaded_per_batch` ;
- burst, swarm, shots et paramètres de contre-mesure.

La définition principale se trouve dans [`code/weapon/weapon.h`](../../code/weapon/weapon.h#L378-L536).

## 18. Données à exclure du protocole public

- pointeurs C++ ;
- disposition mémoire brute des structures ;
- listes chaînées et conteneurs internes ;
- indices locaux sans correspondance stable ;
- handles audio, bitmap, texture, modèle ou shader ;
- coordonnées écran dépendantes du HUD ;
- frames, fades et timers d'animation locale, sauf l'offset canonique de la vue de communication ;
- caches de rendu ;
- champs `last_*` uniquement nécessaires à une étape interne ;
- timestamps `TIMESTAMP` locaux : transmettre une durée restante ;
- données statiques répétées à chaque tick.

## 19. Données dérivées côté client

Le client doit calculer autant que possible :

- cap, tangage et roulis depuis le quaternion ;
- matrice et axes locaux depuis le quaternion, puis vitesse locale, vitesse scalaire, vitesse avant et pourcentage de vitesse depuis la vitesse monde canonique ;
- ratios coque, boucliers, énergie, carburant et munitions ;
- position relative, azimut et élévation ;
- vitesse de rapprochement et TTC ;
- coordonnées de radar et de HUD ;
- interpolation entre deux frames ;
- animations de jauges, flashes et sons.

Pour la vue de communication, le client dérive également l'image courante depuis l'asset, l'offset autoritaire, le temps monotone du producteur, `playback_rate` et le mode de boucle.

Les décisions sensibles ou difficiles à reproduire exactement restent autoritaires côté producteur : visibilité radar, distance HUD exacte, acquisition du lock, `time_to_lock`, phase et divulgation du scan cargo, lead complexe et menaces. La progression normalisée du lock demeure `D` depuis `time_to_lock` et le catalogue d'arme.
