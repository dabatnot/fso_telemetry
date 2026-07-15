# 04 — Modèle de données et règles métier

## 1. Objet

Ce document fige le profil de données du premier flux. Il complète le [modèle FSTL de phase 0](../0-Contrat-de-protocole/04-modele-de-donnees-v1.md) sans créer de nouveau layout, `MessageType`, `RecordType` ou `record_version`.

Les exigences principales sont `P1-REQ-020` à `P1-REQ-030`. Les décisions associées sont `D1-001` à `D1-006` et `D1-012`.

## 2. Amendement FSTL 1.1 consommé par la phase 1

### 2.1 Domaine `PLAYER_KINEMATICS`

Sous une session dont la mineure négociée vaut `1`, le bit suivant est connu :

| Registre | Nom | Bit | Masque | Sens |
|---|---|---:|---:|---|
| `StateDomainCoverage` | `PLAYER_KINEMATICS` | 10 | `0x0000000000000400` | État de session et de mission, plus lifecycle et cinématique du joueur observé lorsqu'il existe. |

Sous FSTL 1.0, ce bit reste réservé et DOIT produire `ValidationError::ReservedFlag` (`RESERVED_FLAG` dans les rapports). Un en-tête de mineure 1 reçu dans une session négociée en 1.0 DOIT produire `ValidationError::UnsupportedMinor` (`UNSUPPORTED_MINOR`). Aucun parseur ne PEUT inférer la mineure depuis la présence du bit.

Le domaine `PLAYER_KINEMATICS` NE signifie PAS `CORE_SHIP`. Il ne garantit ni identité métier du vaisseau, ni manifeste, ni ressources, ni sous-système. En phase 1, `SESSION_STATE.state_domain_coverage` DOIT être exactement `0x0400`.

### 2.2 Ensemble exact du snapshot

Tout `FULL_SNAPSHOT` de phase 1 contient exactement un `SESSION_STATE` et exactement un `MISSION_STATE`.

Lorsque `SESSION_STATE.observed_player_entity_id` est absent :

- aucun record dont l'identité est un `entity_id` joueur ne DOIT être présent ;
- le snapshot est complet relativement à la couverture annoncée ; la session cliente PEUT être `Live` avec une sous-vue joueur explicitement indisponible ;
- le producteur continue le heartbeat et PEUT envoyer une nouvelle keyframe lorsqu'un joueur devient valide.

Lorsqu'il est présent, le snapshot contient en plus :

- exactement un `ENTITY_LIFECYCLE` dont `entity_id` est égal à `observed_player_entity_id` et `object_type = SHIP` ;
- exactement un `FLIGHT_STATE` portant le même `entity_id` ;
- aucun `CLASS_MANIFEST`, `SHIP_IDENTITY`, `DAMAGE_STATE`, `SHIELD_STATE`, `SUBSYSTEM_STATE`, `ENERGY_STATE`, `PROPULSION_STATE` ni record d'une phase ultérieure.

`SESSION_BEGIN.required_manifest_id` DOIT valoir `0`. `event_coverage_state_derived` et `event_coverage_exact` DOIVENT valoir `0`. Les capabilities visuelles actives DOIVENT valoir `0`.

### 2.3 Définition de l'identité joueur

Dans cette phase, « identité joueur » signifie exclusivement le tuple :

```text
(producer_id, session_id, observed_player_entity_id,
 ENTITY_LIFECYCLE.object_type = SHIP)
```

Il ne signifie ni callsign, ni `ship_name`, ni classe, ni équipe, ni rôle. Ces données restent en phase 2. `Player_obj->signature`, `net_signature`, `objnum`, `instance`, une adresse ou un pointeur NE DOIVENT PAS être copiés comme `entity_id` wire.

## 3. Registres et générations

### 3.1 `producer_id`

`producer_id` est un `u64` non nul, généré par CSPRNG à la première configuration valide puis persisté dans le profil local. Une valeur absente ou nulle DOIT être créée atomiquement. Un échec d'entropie ou de persistance désactive le module avant ouverture du socket.

### 3.2 `session_id`

Chaque session client reçoit un `session_id` CSPRNG non nul. Il NE DOIT PAS être réutilisé pendant le processus. Chaque `GameMissionLoad` ou sortie de mission ferme les sessions Phase 1 ; le handshake suivant reçoit donc un nouvel ID. Le socket d'écoute reste ouvert, mais il n'existe aucune session conservée au menu. Un arrêt du runtime ou un redémarrage du processus impose également un nouvel ID.

### 3.3 `mission_generation`

`mission_generation` est un `u32` non nul, possédé par le runtime et croissant pendant le processus, y compris à travers les sessions client. La première mission publiable vaut `1`. Chaque `GameMissionLoad` accepté incrémente la génération avant d'accepter un nouveau handshake. L'overflow ferme les sessions, purge l'état et place le runtime `Faulted` jusqu'au redémarrage du processus ; ouvrir seulement une nouvelle session ne remettrait pas cette portée à zéro.

Un changement de génération NE DOIT JAMAIS être encodé par `DELTA`. Il purge l'image métier de la mission précédente et impose un nouveau `FULL_SNAPSHOT` keyframe.

### 3.4 `entity_id`

Le registre d'identités alloue un `u64` non nul et monotone par session. Une correspondance interne peut utiliser `Player_obj->signature` uniquement comme clé d'observation locale ; elle ne traverse pas l'interface wire.

Le même objet joueur conserve son `entity_id` entre deux ticks. Un remplacement incohérent de l'objet, une signature réutilisée ou un passage durable vers un objet non-vaisseau invalide la correspondance et force une keyframe. L'overflow ferme la session ; aucune réutilisation n'est permise.

## 4. Sources moteur et capture atomique

### 4.1 Préconditions de publication

Le collecteur DOIT vérifier sur le thread principal, dans cet ordre :

1. `Game_mode & GM_IN_MISSION` ;
2. `Player != nullptr`, `Player_obj != nullptr`, `Player_ship != nullptr` ;
3. `Player_obj->type == OBJ_SHIP` ;
4. `Player_obj->instance` dans les bornes du tableau des vaisseaux ;
5. cohérence entre `Player->objnum`, `Player_obj` et `Player_ship`.

Une précondition non satisfaite produit un DTO sans joueur observé. Elle NE DOIT PAS provoquer de déréférencement, de lecture partielle ni de réutilisation de la dernière pose comme si elle était actuelle.

Toutes les valeurs d'un DTO proviennent du même callback `EngineUpdate`. Aucun pointeur, span, référence, index moteur ou vue mutable ne survit à ce callback.

### 4.2 Mapping `FLIGHT_STATE`

| Champ wire | Source moteur | Règle |
|---|---|---|
| `entity_id` | registre de session | Non nul, jamais un index moteur. |
| `producer_sample_time_us` | `timer_get_microseconds()` | Horloge monotone ; distincte de `Missiontime`. |
| `position_world` | `Player_obj->pos` | `wu`, repère monde FSO ; aucune conversion implicite en mètres. |
| `orientation_local_to_world` | `Player_obj->orient` | Quaternion `w,x,y,z`, normalisé et canonique. |
| `velocity_world` | `Player_obj->phys_info.vel` | `wu/s`, repère monde. |
| `rotational_velocity_local` | `Player_obj->phys_info.rotvel` | rad/s, repère local, ordre pitch/yaw/roll du contrat v1. |
| `radius` | `Player_obj->radius` | `wu`, réel fini dans `[0;1,0e9]`. |
| `physics_mode_flags` | mapping fermé des flags `physics_info` | Valeur réelle ; bits réservés nuls. |

`FLIGHT_STATE.presence` DOIT valoir `0`. Les groupes optionnels de prédiction, limites, accélérations ou poussée cosmétique NE DOIVENT PAS être émis. Les champs obligatoires `radius` et `physics_mode_flags` sont conservés parce que le layout v1 est inchangé ; leur présence n'annonce aucune ressource de propulsion de phase 2.

Le mapping de `physics_mode_flags` est fermé et ne recopie jamais directement le bitmap moteur :

| Bit FSTL | Condition moteur exacte |
|---|---|
| `AFTERBURNER` | `phys_info.flags & PF_AFTERBURNER_ON` |
| `BOOSTER` | `phys_info.flags & PF_BOOSTER_ON` |
| `GLIDE_ACTIVE` | `phys_info.flags & PF_GLIDING` |
| `GLIDE_FORCED` | `phys_info.flags & PF_FORCE_GLIDE` |
| `NEWTONIAN_DAMPING` | `phys_info.flags & PF_NEWTONIAN_DAMP` |
| `LATERAL_TRANSLATION` | `phys_info.flags & PF_SLIDE_ENABLED` |
| `WARP_IN` | `phys_info.flags` contient `PF_WARP_IN` ou `PF_SUPERCAP_WARP_IN` |
| `WARP_OUT` | `phys_info.flags` contient `PF_WARP_OUT` ou `PF_SUPERCAP_WARP_OUT` |
| `SCRIPTED` | `phys_info.flags & PF_SCRIPTED_VELOCITY` |
| `SHOCKWAVE` | `phys_info.flags & PF_IN_SHOCKWAVE` |
| `IMMOBILE` | `Player_obj->flags` contient `Object::Object_Flags::Immobile` ou `Dont_change_position` |
| `ORIENTATION_LOCKED` | `Player_obj->flags` contient `Object::Object_Flags::Immobile` ou `Dont_change_orientation` |

Tous les autres flags moteur sont sans représentation dans ce bitmap et sont ignorés. Les tests utilisent une table un-bit-à-la-fois, les combinaisons warp supercap et les deux verrous d'objet.

`speed`, `fspeed`, angles d'Euler, matrice, accélération calculée et valeurs graphiques NE DOIVENT PAS être ajoutés. Un client les dérive localement si nécessaire.

### 4.3 Quaternion canonique

La conversion matrice-vers-quaternion est un composant pur testé séparément. Elle DOIT :

1. refuser toute matrice non finie ;
2. calculer le quaternion stable en sélectionnant la branche de trace ou de diagonale dominante ;
3. normaliser en précision suffisante avant conversion float32 ;
4. imposer `w > 0`, ou, si `w == 0`, rendre positif le premier composant non nul parmi `x`, `y`, `z` ;
5. produire l'ordre wire `w,x,y,z` et la convention local-vers-monde ;
6. vérifier après quantification une norme dans `[0.9999, 1.0001]`.

Les tests couvrent l'identité, ±90°, les rotations de 180° autour de chaque axe, les matrices légèrement bruitées, l'équivalence `q/-q` et les entrées non finies.

### 4.4 Valeurs invalides

Tous les floats publiés DOIVENT être finis et respecter les bornes FSTL. Le producteur NE DOIT PAS saturer silencieusement une valeur, remplacer un NaN par zéro ni publier un mélange de valeurs anciennes et nouvelles.

Une valeur source invalide :

- incrémente `source_validation_failures_total` avec une raison fermée ;
- journalise au plus un diagnostic agrégé par fenêtre ;
- empêche la publication de l'image trompeuse ;
- conserve la dernière baseline `APPLIED` sans la modifier ;
- force une keyframe récente après retour à un état valide.

## 5. `SESSION_STATE`, `MISSION_STATE` et lifecycle

### 5.1 `SESSION_STATE`

Le record DOIT contenir :

- le `producer_id` persistant ;
- un `producer_sample_time_us` du même cycle de capture ;
- `authority_mode = SinglePlayer` ;
- `visibility_mode = Cockpit` ;
- `state_domain_coverage = PLAYER_KINEMATICS` ;
- capabilities, couvertures d'événements dérivés et exacts à zéro ;
- `observed_player_entity_id` uniquement si le joueur satisfait les préconditions ;
- une `capability_generation` non nulle, stable tant que les capabilities restent à zéro.

### 5.2 `MISSION_STATE`

Le record DOIT publier la génération courante, la phase de mission exacte, l'état de pause, le facteur de compression et le temps mission selon le contrat v1. `producer_sample_time_us` reste monotone même lorsque le temps mission est en pause ou recule à cause d'un chargement.

### 5.3 `ENTITY_LIFECYCLE`

Le record joueur DOIT avoir `object_type = SHIP`, l'état lifecycle courant et `presence = 0`. En particulier, aucun `class_id`, parent, équipe ou nom n'est inventé. La perte momentanée du joueur est représentée par une nouvelle keyframe sans `observed_player_entity_id`, pas par une suppression isolée ambiguë.

Mort, observer et respawn complets sont hors phase 1. Un scénario qui les exige NE PEUT PAS fermer le critère des trente minutes ; il doit être traité en phase 2.

## 6. Image, baseline et delta

### 6.1 Image canonique

Le producteur construit une image triée selon les clés de records Phase 0. Chaque record est un atome complet. Un changement d'un seul composant de position ou d'orientation remplace l'atome `FLIGHT_STATE` complet ; il n'existe aucun patch intra-record.

L'image candidate est immutable après sa capture. Sa sérialisation, fragmentation, retransmission et validation utilisent les composants `fstl_protocol` existants.

### 6.2 Snapshot initial et renouvellement

Le premier snapshot publiable est un `FULL_SNAPSHOT` fiable et transactionnel. Le producteur :

1. envoie toutes ses parts ;
2. accepte `ACK VALIDATED` sans changer de baseline ;
3. installe la candidate uniquement après `ACK APPLIED` de toutes les parts ;
4. réacquitte un doublon sans republier ni reculer l'image ;
5. retransmet la même candidate tant qu'elle est dans la fenêtre fiable.

Une keyframe périodique est capturée toutes les `keyframeSeconds`, défaut `2 s`, borne `[1,5] s`. Une seule candidate existe par client. Une nouvelle échéance pendant l'attente d'ACK n'écrase pas la candidate.

### 6.3 Delta cumulatif

Après baseline `APPLIED`, chaque `DELTA` est calculé contre cette baseline immutable, jamais contre le delta précédent. Il contient tous les atomes courants différents et toutes les suppressions nécessaires. Il n'est pas acquitté ; le dernier delta sérialisé pour la même baseline PEUT remplacer un delta plus ancien non envoyé.

`delta_sequence` croît sans exiger la continuité à la réception. Une perte, duplication ou réorganisation de deltas ne change pas le résultat : tout delta applicable reconstruit l'image courante déclarée depuis la baseline référencée.

Pendant l'ACK d'une candidate, le producteur conserve la baseline active et la candidate. Après bascule, le premier delta contre la nouvelle baseline DOIT contenir toute mutation survenue depuis la capture de cette candidate.

### 6.4 Resynchronisation

Un `RESYNC_REQUEST` valide, dédupliqué et rate-limité reçoit `ACK VALIDATED` et provoque une keyframe récente. Le producteur NE DOIT PAS conserver un historique non borné de baselines. Une baseline inconnue côté client entraîne le rejet du delta et un resync rate-limité ; elle ne déclenche aucune application partielle.

## 7. Changements de mission et joueur

| Événement | Effet normatif |
|---|---|
| Entrée en mission, joueur valide | Après nouveau handshake, nouvelle `mission_generation`, allocation d'`entity_id`, snapshot complet avec joueur. |
| Entrée en mission, joueur absent | Après nouveau handshake, snapshot session/mission sans joueur ; session `Live`, sous-vue joueur indisponible. |
| Joueur devient valide | Nouvelle keyframe avec `observed_player_entity_id`. |
| Joueur disparaît ou change de manière incohérente | Invalidation du flux, nouvelle keyframe sans joueur ou fermeture/restart si l'identité ne peut être rendue non ambiguë. |
| Retour menu | `SESSION_END/MissionEnded` best effort, fermeture des sessions et purge obligatoire de l'image mission ; listener conservé. |
| Nouvelle mission | Fermeture de toute ancienne session, génération process incrémentée, nouveau handshake et snapshot initial keyframe ; aucun delta intergénération. |
| Shutdown | `SESSION_END/Shutdown` best effort, fermeture socket et purge totale. |

La Phase 1 ferme toujours les sessions client au changement ou à la sortie de mission, conformément au document 03. Aucun record, ID d'entité ou baseline de l'ancienne mission ne peut être réutilisé comme état courant.

## 8. Interdictions de phase

La phase 1 NE DOIT PAS :

- annoncer `CORE_SHIP`, `ALL_ENTITIES` ou une capability visuelle ;
- émettre `SHIP_IDENTITY` pour satisfaire artificiellement le mot « identité » de la roadmap ;
- détourner `EVENT_BATCH` pour transporter une pose ;
- utiliser `RecordFlag.PARTIAL` ;
- remplir des records Phase 2 avec des zéros ou des valeurs estimées ;
- publier une commande ou un accusé comme mutation de simulation ;
- introduire un nouveau record cinématique redondant.

## 9. Preuves minimales

Les preuves de ce document comprennent :

- vecteurs valides 1.1 pour snapshot avec et sans joueur, et delta cumulatif ;
- vecteurs invalides pour bit 10 sous 1.0, mauvais ID, mauvais type, record obligatoire manquant et option `FLIGHT_STATE` non couverte ;
- tests source-vers-DTO, quaternion et non-finis ;
- test de mutation pendant RTT de keyframe ;
- test prouvant qu'un profil `CORE_SHIP` incomplet reste rejeté ;
- non-régression des hashes et octets de tous les vecteurs FSTL 1.0.
