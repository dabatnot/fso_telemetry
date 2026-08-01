# 03 — Flux, cycle de vie et concurrence

## 1. Objet

Ce document fixe les machines d’état, l’ordre d’un tick, les transitions de manifeste et de baseline, le lifecycle du joueur et les garanties de concurrence. Il complète sans les remplacer les machines Phase 0 et Phase 1.

## 2. Contexte d’exécution

### 2.1 Callbacks existants

La Phase 2 réutilise les callbacks Phase 1 : initialisation télémétrie, entrée/sortie mission, `EngineUpdate` et shutdown. Le runtime enregistre l’identifiant du thread lors de l’initialisation et refuse toute capture depuis un autre thread.

Les faits suivants sont évalués à chaque `EngineUpdate`, même lorsqu’aucune cadence de publication n’est due :

- présence et signature du joueur ;
- changement de classe/loadout susceptible de modifier le catalogue ;
- état privé `PlayerPresence::{Absent,Present}` et phases filaires observées `SPAWNING`, `ACTIVE`, `DEPARTING`, `DYING`, `DESTROYED`, `REMOVED` ;
- transition du flag autoritaire `DISABLED` ;
- transition terminale de support ;
- changement de mission et demande de shutdown.

Cette surveillance ne sérialise rien et ne parcourt pas les listes lourdes si aucun changement n’est détecté.

### 2.2 Absence de partage concurrent

La collecte, la construction d’image, le diff et le scheduling réseau s’exécutent sur le thread principal comme en Phase 1. Les callbacks ne publient aucun pointeur moteur. Le socket reste non bloquant ; aucun worker n’est ajouté.

Un éventuel hook lifecycle hors `EngineUpdate` ne peut appeler qu’une fonction `noexcept` qui copie une clé stable et un enum borné dans un latch possédé par le runtime sur le même thread moteur. Si l’appel peut venir d’un autre thread, ce hook est interdit en Phase 2.

## 3. Machines d’état runtime et sessions

La machine globale ne dépend d’aucun ACK client :

| État global | Entrée | Travail permis | Sortie |
|---|---|---|---|
| `Disabled` | configuration absente ou `enabled=false` | aucun socket, aucune capture | reconfiguration au redémarrage uniquement |
| `Starting` | configuration valide | provisionnement maximal, identité, bind, vérifications | `Idle` ou `Faulted` |
| `Idle` | runtime prêt hors mission | heartbeat idle, handshake Phase 1 seulement si autorisé | `MissionPreparing` |
| `MissionPreparing` | entrée mission | purge, génération mission, construction des registres | `MissionReady` ou `Idle` |
| `MissionReady` | mission active, joueur présent ou absent | surveillance sources et service indépendant des slots | `Draining` |
| `Draining` | sortie mission/shutdown | `SESSION_END`, abandon ordonné, aucune nouvelle capture | `Idle` ou `Stopped` |
| `Faulted` | erreur globale identité/bind/allocation | aucune publication métier | arrêt/restart processus |

Chaque slot client possède indépendamment la machine suivante ; un ACK lent ne bloque jamais les autres slots :

| État de slot | Entrée | Travail permis | Sortie |
|---|---|---|---|
| `MissionNoPlayer` | mission active sans joueur | singletons/heartbeat, surveillance lifecycle | `ManifestPreparing` ou fermeture |
| `ManifestPreparing` | profil choisi/catalogue nouveau | capture forcée, construction et validation | `ManifestPublishing` ou `FaultedSession` |
| `ManifestPublishing` | candidate manifeste immuable | retransmission fiable jusqu’à `APPLIED` | `SnapshotPreparing` |
| `SnapshotPreparing` | manifeste installé | capture complète forcée et profile gate | `SnapshotPublishing` ou `ManifestPreparing` |
| `SnapshotPublishing` | candidate snapshot immuable | retransmission fiable ; état plus récent conservé séparément | `Live` après `APPLIED` |
| `Live` | baseline installée | captures cadencées, deltas cumulatifs, événements, keyframes | état de préparation approprié ou fermeture |
| `FaultedSession` | erreur locale au slot | fermeture client et purge de ce seul slot | nouveau handshake éventuel |

Une erreur de fermeture spécifique à un joueur/client NE DOIT PAS arrêter les sessions valides si les budgets globaux restent sûrs. Une erreur d’identité, bind ou allocation globale place le runtime en `Faulted`.

## 4. Portée des identités et purges

| Identité | Portée | Invalidation |
|---|---|---|
| `producer_id` | installation | jamais pendant l’exécution normale |
| `session_id` | handshake client | fermeture, changement de profil ou nouveau handshake |
| `mission_generation` | chargement de mission | incrément à chaque chargement, même nom |
| `manifest_id` | session | strictement croissant à chaque `catalog_fingerprint` différent |
| `snapshot_id` | session | strictement croissant à chaque candidate commencée |
| `entity_id` | session | jamais réutilisé ; nouveau pour chaque signature/respawn |
| IDs classe/arme/sous-système/banque | génération de manifeste | réassignables seulement sous nouvel `manifest_id` |
| `event_id` | session et direction producteur | strictement croissant, jamais réutilisé |

La sortie mission purge closure, manifeste, snapshot, baseline, deltas, événements, références de support et registres d’entités avant que le moteur détruise les tables correspondantes.

## 5. Machine de session et profils

### 5.1 Sélection avant `WELCOME`

Le profil est choisi avant `WELCOME` depuis les capacités locales prouvées :

1. si le producteur ne sait produire que Phase 1, il peut établir `0x0400` selon le contrat Phase 1 ;
2. si le manifeste de la racine joueur et ses records cœur sont entièrement matérialisables, une nouvelle session peut annoncer `0x0401`, indépendamment de toute extension support/docking ;
3. si les deux catalogues, la closure service/docking transitive et tous les records des cinq domaines sont matérialisables, une nouvelle session peut annoncer `0x0583` ;
4. toute autre combinaison est refusée.

Le choix est basé sur la prévalidation de la projection demandée : `CoreGateClosure={joueur}` pour `0x0401`, point fixe `CompleteShipClosure` pour `0x0583`. L’échec propre au second ne dégrade ni ne bloque le premier. L’installation client du manifeste intervient après `SESSION_BEGIN`. « Promotion après installation » signifie que le client ne passe à `Live` et n’expose pas le snapshot avant `ACK APPLIED`; cela ne signifie jamais une mutation du bitmap en cours de session.

### 5.2 Séquence manifeste puis snapshot

```text
Producteur                       Client
SESSION_BEGIN(profile immuable) ->
MANIFEST_PART(id=N, reliable)    -> validation/assemblage
                                 <- ACK VALIDATED
                                 <- ACK APPLIED
FULL_SNAPSHOT(id=S, manifest=N)  -> validation/commit atomique
                                 <- ACK VALIDATED
                                 <- ACK APPLIED
DELTA(baseline=S)                -> application remplaçable
```

`VALIDATED` seul ne rend visible ni manifeste ni snapshot côté producteur. Seul `APPLIED` permet d’émettre l’objet dépendant ou de promouvoir la baseline.

### 5.3 Changements de topologie et de catalogue

Deux fingerprints sont distincts et comparés sur la capture cohérente :

- `topology_fingerprint` couvre les instances, le support assigné et les relations de docking. Chef de groupe et cible cargo en sont exclus. Sa modification seule force une keyframe sous le manifeste N déjà installé ;
- `catalog_fingerprint` couvre exactement les `ClassDescriptor`, `WeaponDescriptor`, clés auxiliaires et définitions imbriquées canoniques pré-ID triées nécessaires ; générations, IDs alloués et index moteur en sont exclus. Sa modification déclenche :

1. gel de l’ancien manifeste et de la baseline pour le trafic déjà en vol ;
2. construction d’un manifeste `N+1` exhaustif ;
3. publication fiable de `N+1` ;
4. capture complète après `APPLIED(N+1)` ;
5. keyframe `S+1` référant `N+1` ;
6. promotion après `APPLIED(S+1)` ;
7. premier delta cumulatif contenant tous les changements survenus pendant la transaction.

Aucun delta contre `S` ne référence un ID de `N+1`. L’ajout d’une instance dont la classe et les armes sont déjà requises par un autre membre change seulement la topologie ; le retrait du dernier utilisateur d’une définition change le catalogue afin de préserver le least-privilege. Si l’ancienne fermeture ne peut plus être représentée sans mentir, la session est terminée au lieu de continuer avec une image incohérente.

Le producteur conserve `active_manifest=N` tant que la baseline S le référence et `staged_manifest=N+1` jusqu'à l'application complète de la keyframe liée. N n’est retiré qu’après disparition de toute référence en vol.

La séquence N/N+1/N+2 est fermée par client :

1. N reste le tuple cohérent appliqué : candidat/génération, fingerprint, `manifest_id`, état `applied` ;
2. N+1 est la seule image `staged` ;
3. si une source N+2 arrive pendant que N+1 est busy, le slot conserve uniquement le dernier fingerprint et une intention de reconstruction, sans construire ni allouer une image N+2 ;
4. après `APPLIED(N+1)`, le producteur émet une keyframe exactement liée à N+1 ;
5. les deltas susceptibles de mélanger N et N+1 sont suspendus ;
6. après application de cette keyframe, le producteur construit le dernier candidat pending et reprend par une nouvelle keyframe sous son manifeste appliqué.

La projection pré-ID immuable peut être partagée entre clients, mais chaque client conserve ses tuples atomiques complets. Un snapshot doit référencer exactement le `manifest_id` effectivement appliqué pour ce client.

## 6. Ordonnancement d’un tick

### 6.1 Préconditions et ordre

Pour chaque `EngineUpdate` :

1. vérifier thread, état runtime, sortie mission et shutdown ;
2. lire le socket non bloquant et traiter contrôles/ACK valides dans les quotas ;
3. surveiller lifecycle, support terminal, composante de docking et `topology_fingerprint` léger ;
4. déterminer `flight_due`, `systems_due` et `keyframe_due` ;
5. si aucun bloc n’est dû, produire seulement heartbeat/egress en attente ;
6. valider les sources moteur ;
7. capturer uniquement les blocs requis par les flags dus ; conserver la dernière copie validée des blocs non dus ; une keyframe ou une invalidation topology/lifecycle/catalogue force tous les blocs ;
8. recalculer la fermeture si `topology_fingerprint` a changé et construire un manifeste seulement si `catalog_fingerprint` change ;
9. remplacer dans l’image filtrée uniquement les atomes des blocs capturés, ou reconstruire l’image exhaustive lorsqu’elle est forcée, puis valider la matrice de couverture ;
10. créer les événements lifecycle reconstructibles observés ;
11. mettre à jour les deltas cumulatifs par client à partir des seuls atomes reconstruits ou invalidés, tout en conservant la comparaison cumulative contre la baseline immuable ;
12. planifier les datagrammes selon les priorités ;
13. mettre à jour métriques et logs bornés.

Toute sortie anticipée laisse les candidats immuables et les files dans un état valide.

### 6.2 Cadences

| Groupe | Cadence | Défaut | Règle |
|---|---|---:|---|
| lifecycle/closure/support terminal | chaque `EngineUpdate` | cadence frame | surveillance légère, capture lourde sur changement |
| flight + control | `flightHz` | 30 Hz | même tick source pour `FLIGHT_STATE` et `CONTROL_STATE` |
| systèmes | `systemsHz` | 10 Hz | identité, dégâts, boucliers, énergie, propulsion, armes, sous-systèmes, support, cargo/docking |
| keyframe | `keyframeSeconds` | 2 s | force une capture complète du profil |
| heartbeat | paramètres hérités | hérités Phase 1 | indépendant de la pause mission |

Le scheduler utilise une deadline monotone et saute les échéances dépassées ; il NE DOIT PAS exécuter plusieurs captures de rattrapage dans la même frame. Les `producer_sample_time_us` proviennent de l’horloge producteur monotone héritée, pas du temps de mission compressé.

La conservation incrémentale n'est jamais une capture partielle filaire : chaque atome modifié reste complet et garde le sample time de sa dernière capture. Un bloc non dû n'est ni relu, ni réencodé, ni marqué dirty. Toute ambiguïté sur l'appartenance d'un atome à un bloc, toute modification de la fermeture ou tout besoin de keyframe bascule transactionnellement vers une capture et une image exhaustives.

### 6.3 Pause et compression temporelle

En pause, les valeurs autoritaires sont toujours échantillonnées aux cadences configurées ; le temps producteur et les heartbeats avancent, tandis que l’état mission indique `paused`. Les timers dérivés du temps mission restent égaux à la source moteur. La compression temporelle n’altère pas les cadences réseau et n’est pas appliquée une seconde fois côté client.

## 7. Priorités et budget de datagrammes

L’ordre décroissant reste :

1. fermeture/session et ACK de contrôle ;
2. manifeste fiable ;
3. événements fiables déjà engagés dont la dépendance est satisfaite ;
4. snapshot/keyframe fiable, y compris celui qui satisfait une dépendance d’événement ;
5. heartbeat ;
6. dernier delta cumulatif ;
7. trafic spécialisé futur, absent de Phase 2.

Le budget `maxDatagramsPerTick` est commun. Un delta ancien est remplacé avant mise en file ; une transaction fiable engagée n’est ni réordonnée ni modifiée. La famine d’un delta est réparée par la keyframe, mais le scheduler DOIT exposer son âge et ses abandons. La priorité ne contourne jamais une fence : `ENTITY_APPEARED` attend `APPLIED` du snapshot qui crée son sujet ; les événements `SHIP_DISABLED`, `SHIP_DYING_STARTED` et `ENTITY_DESTROYED` peuvent ensuite être émis en ordre contre cette baseline connaissable même si une keyframe de phase est en vol ; `ENTITY_DISAPPEARED` est envoyé et acquitté `APPLIED` contre une baseline où son sujet existe encore avant la keyframe de retrait.

## 8. Lifecycle du joueur

### 8.1 Absence initiale

Sans joueur valide :

- `SESSION_STATE.observed_player_entity_id` est absent ;
- `MISSION_STATE` reste publié selon le profil actif ;
- aucun record à clé joueur n’est présent ;
- le manifeste déjà installé reste référencé et aucun ID sentinelle n’est fabriqué ;
- les obligations par joueur/vaisseau sont vides tant qu’aucun joueur observé n’existe, comme l’autorise la matrice Phase 0 ; la session `0x0583` peut donc rester active avec ses deux singletons.

Une première session Phase 2 est normalement créée après apparition d’un joueur valide afin de disposer d’une fermeture non vide. Après une disparition, la session peut rester synchronisée sans joueur ; une nouvelle apparition réutilise le manifeste seulement si son `catalog_fingerprint` est exactement celui du manifeste actif.

### 8.2 Apparition

À la première signature valide :

1. allouer un nouvel `entity_id` ;
2. calculer la fermeture et installer le manifeste ;
3. créer un snapshot initial avec lifecycle filaire `SPAWNING` si la source d’arrivée le prouve, sinon `ACTIVE` ;
4. attendre `APPLIED` de ce snapshot, puis émettre `ENTITY_APPEARED` avec `RECONSTRUCTIBLE|RELIABLE`, sans `EXACT_CAPTURE` ;
5. rendre l’état visible seulement après commit atomique et ne jamais modifier `observed_player_entity_id` par delta.

Le même atome `ENTITY_LIFECYCLE` et le même `FLIGHT_STATE` satisfont simultanément `PLAYER_KINEMATICS` et `CORE_SHIP`; ils ne sont jamais dupliqués.

### 8.3 Mort et destruction

Le détecteur conserve séparément `PlayerPresence` et la phase filaire. Le chemin terminal nominal est :

`ACTIVE -> DYING -> DESTROYED -> REMOVED`, puis absence de l’atome après suppression cascade. `SPAWNING` peut précéder `ACTIVE`; `DEPARTING` suit les règles transit documentées Phase 0.

Si le moteur saute un état entre deux observations, le producteur publie seulement les faits observés, dans l’ordre valide, et ne pose pas `EXACT_CAPTURE`. Les événements possibles sont `SHIP_DISABLED` au front du flag autoritaire, `SHIP_DYING_STARTED`, `ENTITY_DESTROYED`, puis `ENTITY_DISAPPEARED`, chacun fiable et reconstructible lorsqu’il est émis. Le retour de `DISABLED` à faux est réparé par l’état cumulatif ; aucun événement inverse n’est inventé.

Toute transition vers `DYING`, `DESTROYED`, `REMOVED` ou `PlayerPresence::Absent` force une keyframe avec exactement `SnapshotFlagPeriodicKeyframe`; seuls le premier snapshot d’une session et une réparation de protocole utilisent respectivement `SnapshotFlagInitial` et `SnapshotFlagResync`. Le snapshot candidat est retenu jusqu’à `APPLIED`; les changements ultérieurs ne mutent jamais cette candidate et sont coalescés dans une keyframe successeur. Pour le retrait, le batch `ENTITY_DISAPPEARED` est d’abord acquitté contre la dernière baseline contenant l’ID, puis la keyframe successeur retire l’atome, ses dépendants et `observed_player_entity_id`.

Pendant `PlayerPresence::Absent`, `active_manifest` et son `catalog_fingerprint` sont gelés : aucun catalogue vide n’est construit malgré la closure vide. Le snapshot à deux singletons continue de référencer ce manifeste. La réapparition compare la nouvelle closure au fingerprint gelé et réutilise N uniquement en cas d’égalité byte-for-byte ; sinon N+1 est installé avant la keyframe.

### 8.4 Respawn

Une nouvelle signature après absence alloue obligatoirement un nouvel `entity_id`, même si le nom et la classe sont identiques. Si le `catalog_fingerprint` ne change pas, le manifeste courant peut être réutilisé ; une keyframe reste obligatoire. Si une définition de classe/loadout change, le nouveau manifeste précède la keyframe.

Le client purge tous les atomes de l’ancien joueur par lifecycle/cascade selon Phase 0, puis crée le nouvel ensemble atomiquement. Il NE DOIT associer les historiques de dégâts ou baselines à l’entité réapparue.

### 8.5 Membres non joueurs de la closure

La même machine vaut pour support et dockés. À l’ajout d’un membre : nouvel `entity_id`, éventuel manifeste si `catalog_fingerprint` change, keyframe créatrice `APPLIED`, puis `ENTITY_APPEARED`. Au retrait d’une relation alors que l’objet moteur reste vivant : `ENTITY_DISAPPEARED` est acquitté contre l’ancienne baseline, puis une keyframe supprime en cascade le membre ; son ID est définitivement retiré. Si le retrait du dernier utilisateur modifie le catalogue, le manifeste N+1 est installé entre l’événement et la keyframe de retrait. Une réentrée ultérieure de la même signature reçoit donc un nouvel ID.

Mort ou cleanup d’un membre utilise les mêmes phases/fences que le joueur. Une disparition pendant ACK ne mutile pas la candidate : événement et éventuel manifeste sont ordonnés, puis une candidate successeur porte la closure recalculée. La fermeture reste un point fixe ; retirer un support ou une relation peut retirer transitivement plusieurs membres, chacun avec son événement et sa cascade.

### 8.6 Changement de mission

Une sortie mission termine les sessions avant purge des objets. La mission suivante possède une nouvelle `mission_generation`, de nouvelles sessions et de nouveaux registres d’IDs. Réutiliser une signature moteur ne peut donc pas réutiliser une identité publique précédente.

## 9. Cycle de support

### 9.1 Machine brute

Les seules phases filaires sont :

`NONE`, `REQUESTED`, `APPROACHING`, `DOCKING`, `REPAIRING`, `REARMING`, `OBSTRUCTED`, `ABORTED`.

Le mapping main-thread normatif est :

| Source moteur observée avant mutation | Phase/effet filaire |
|---|---|
| `REPAIR_INFO_QUEUE` ou `AI_Flags::Awaiting_repair` sans support en route | `REQUESTED`, flag `AWAITING_REPAIR` |
| `REPAIR_INFO_ONWAY`, support assigné, ou support en `AIM_DOCK` sous-mode `AIS_DOCK_0/1` | `APPROACHING` |
| support en `AIM_DOCK`, sous-mode `AIS_DOCK_2/3`, ou `AIS_DOCK_4/4A` sans relation directe encore matérialisée et avant `Being_repaired` | `DOCKING` |
| relation directe matérialisée en `AIS_DOCK_4/4A`, avant travail effectif | `DOCKING` pour `SUPPORT_STATE`; `DOCKED` uniquement dans le `DOCKING_STATE` du même tick |
| `REPAIR_INFO_BEGIN`/`Being_repaired`, `repair_work_remaining=true` selon le prédicat fermé ci-dessous | `REPAIRING` |
| `Being_repaired`, `repair_work_remaining=false` et `rearm_work_remaining=true` selon le prédicat fermé ci-dessous | `REARMING` |
| `REPAIR_INFO_BROKEN` | latch terminal `OBSTRUCTED` |
| `REPAIR_INFO_ABORT` ou `REPAIR_INFO_KILLED` | latch terminal `ABORTED` |
| `REPAIR_INFO_COMPLETE` | latch terminal `NONE`, raison privée `Completed` |
| `REPAIR_INFO_END` | latch terminal `NONE`, raison privée `Ended`; aucune réussite n’est déduite |

Le collecteur calcule les deux booléens sur les mêmes sources validées que `ship_do_rearm_frame()`, sans appeler ce mutateur :

- `repair_work_remaining` est l’OR de : (a) bouclier autorisé, `shield_get_max_strength(subject)>0`, `ship_info::sup_shield_repair_rate>0` et force courante strictement inférieure à ce maximum ; (b) `Mission_Flags::Support_repairs_hull`, taux `sup_hull_repair_rate>0` et coque courante strictement inférieure à `ship::ship_max_hull_strength × The_mission.support_ships.max_hull_repair_val/100`; (c) taux `sup_subsys_repair_rate>0` et au moins un `ship_subsys` dont `current_hits < max_hits × max_subsys_repair_val/100`. Les pourcentages mission doivent être finis dans `[0,100]`, les taux finis dans `[0,1]`, les maxima non négatifs et les calculs en `float64`; une source invalide refuse le profil ;
- `rearm_work_remaining` vaut l’OR de : (a) le groupe contre-mesure applicable a `quantity_current < quantity_max`, car `ship_do_rearm_frame()` le remplit indépendamment du pool et de `disallow_rearm`; (b) `The_mission.support_ships.disallow_rearm=false` et une banque secondaire, ou une banque primaire balistique, a `ammo_current < ammo_initial` **et** sa classe est valide, `weapon_info::disallow_rearm=false` et `get_mission_rearm_pool_for_weapon(class,team) != 0` (`-1` signifie illimité, une valeur positive stock disponible). Un pool nul, une arme interdite ou une banque non balistique est considéré non livrable comme dans le compteur `banks_full`; une substitution par precedence n’est pas spéculée : si le chemin gameplay l’effectue, le prochain sample évalue la nouvelle classe ;
- si les deux booléens sont faux tandis que `Being_repaired` reste posé pendant le délai de libération, `SUPPORT_STATE` reste `DOCKING` jusqu’au terminal, sans inventer de travail.

Ces prédicats sont extraits dans un helper main-thread pur `evaluate_support_work(const object&, SupportWorkSummary&) noexcept`, sans HUD, mutation, allocation ni timestamp créé. `ship_do_rearm_frame()` et l’adaptateur télémétrie consomment ce même helper pour leurs décisions de travail restant ; la Phase 2 NE DOIT PAS maintenir une copie divergente de la logique `banks_full`, plafonds ou pools.

Si réparation et réarmement progressent simultanément, `REPAIRING` a priorité jusqu’à épuisement du travail coque/bouclier/sous-système, puis `REARMING`; les records armes montrent parallèlement les munitions réelles. Le producteur NE DOIT inventer ni `COMPLETED`, ni `FAILED`, ni pourcentage. `Completed` et `Ended` sont des raisons privées d’oracle/métrique : toutes deux deviennent `NONE` sur le fil et le client NE DOIT afficher ni réussite ni échec à partir de cette distinction invisible.

### 9.2 Garantie des transitions terminales

`telemetry::OnSupportTransition()` est appelé au début de toutes les branches `QUEUE`, `ONWAY`, `BEGIN`, `BROKEN`, `END`, `ABORT`, `KILLED` et `COMPLETE` de `ai_do_objects_repairing_stuff()`, avant nettoyage des flags et identifiants. Une table globale préallouée est indexée **uniquement** par `assisted_signature` et possède `{episode_sequence, episode_state, current_support_signature_or_zero}`. Le premier `QUEUE`, `ONWAY` ou `BEGIN` après `Inactive`/terminal incrémente la séquence et ouvre l’épisode ; tout autre `QUEUE`, `ONWAY` ou `BEGIN` avant un terminal appartient par définition au même épisode, conserve cette séquence et peut remplacer l’attribut support, notamment `QUEUE(0)→ONWAY(support)`. Le seam pousse `{assisted_signature, current_support_signature_or_zero, episode_sequence, reason, sample_time}` dans un ring global préalloué de 64 faits. Au prochain `EngineUpdate`, le runtime vide ce ring une fois et met à jour une table de 64 latches par session, indexée par entité assistée. Le latch devient l’autorité persistante de `SUPPORT_STATE` au tick de capture suivant et reste prioritaire sur le polling jusqu’à `APPLIED` de la keyframe qui le contient ; tous les blocs de cette keyframe portent donc bien le même sample time de capture.

La table ne rejoue pas un historique comme de faux états courants : elle garantit seulement le terminal le plus récent par entité assistée et `episode_sequence`. `ABORTED`/`OBSTRUCTED` remplacent une valeur active ; `COMPLETE` produit `NONE(Completed)` ; seul un `END` portant la même `assisted_signature` et le même `episode_sequence` est coalescé sans dégrader `Completed`, quel que soit l’attribut support courant ou nul. Un nouveau `QUEUE`/`ONWAY`/`BEGIN` après ce terminal incrémente la séquence : son futur `END` devient donc `NONE(Ended)`. Si un nouveau terminal arrive pendant une candidate, sa génération de latch reste pour la candidate successeur. Un overflow du ring global journalise `SourceLimitExceeded`, incrémente le compteur puis ferme toutes les sessions Phase 2 avec `SESSION_END(Restart, RECONNECT_ALLOWED)` ; la table par session ne peut dépasser la closure de 64 ships.

Cette règle conserve les transitions signalées par le seam sans créer de nouvel `EventKind`. Un simple polling `EngineUpdate` ne garantit pas l'observation de la terminalité support.

### 9.3 Closure de docking

En profil `0x0583` :

- `CARGO_SCAN_STATE` du joueur est toujours présent et suit la vérité moteur/visibilité ;
- la closure commence au joueur, suit le support assigné, chaque `group_leader_entity_id` et toutes les relations de docking transitives ;
- chaque ship membre reçoit un `entity_id` stable, `ENTITY_LIFECYCLE`, la matrice complète `CORE_SHIP`, `WEAPON_STATE`, `DOCKING_STATE` et `SUPPORT_STATE` ;
- chaque `DOCKING_STATE` contient la topologie complète de son sujet et toute relation inverse publiée est exacte ;
- la closure contient au plus 64 ships et tous doivent appartenir à l’allowlist `Cockpit` calculée avant copie publique ;
- une cible de scan est référencée seulement si elle appartient déjà à cette closure ; sinon la session se termine avant publication du nouvel état ;
- aucun `support_entity_id`, `remote_entity_id` ou `target_entity_id` non nul ne reste une référence opaque.

Si une référence obligatoire ne peut être matérialisée avec son état complet sans élargir la visibilité, la session Phase 2 est terminée et purgée. Elle NE DOIT ni omettre un bit requis, ni substituer un ID zéro, ni exporter une entité cachée.

## 10. Baseline, ACK et courses logiques

### 10.1 Snapshot initial

Le snapshot candidat est une transaction exhaustive et immuable. Les mises à jour moteur qui surviennent pendant son aller-retour ne le modifient pas. Après `APPLIED`, la baseline devient cette candidate et le premier delta compare l’image la plus récente à cette baseline.

### 10.2 Keyframe périodique

La keyframe périodique force une capture complète. Tant qu’elle n’est pas `APPLIED`, l’ancienne baseline reste la référence des deltas. Le producteur PEUT suspendre les deltas si le budget l’exige, mais ne peut les rebaser sur une candidate non acquittée.

### 10.3 ACK perdu ou doublon

Un ACK perdu provoque retransmission de la même transaction. Un doublon `VALIDATED` ou `APPLIED` est idempotent. Un ACK pour un ancien ID ne fait pas régresser la baseline ; un ACK impossible ferme la session selon Phase 0.

### 10.4 Resynchronisation

`RESYNC_REQUEST` valide déclenche une keyframe `RESYNC` complète, référant le manifeste installé. Les requêtes sont coalescées et rate-limitées. Un client ne peut forcer un manifeste nouveau si le `catalog_fingerprint` n’a pas changé.

### 10.5 Fences d’événements et candidates concurrentes

Chaque événement conserve `dependency_snapshot_id` et le fait de devoir être appliqué avant ou après cette baseline. Apparition : snapshot créateur `APPLIED`, puis événement. Les fronts `DISABLED`, `DYING` et `DESTROYED` sont ensuite ordonnés par `event_id` et peuvent être acquittés contre cette même baseline connaissable ; leurs keyframes d’état peuvent être coalescées vers la phase la plus récente si une candidate est déjà en vol. Disparition : événement fiable `APPLIED` tandis que la baseline connaît encore le sujet, puis keyframe de suppression. Un événement collecté pendant une candidate snapshot n’est jamais injecté dans ses octets ; il attend sa dépendance ou provoque une candidate successeur. Cette règle évite une FIFO supplémentaire de snapshots de 16 Mio tout en préservant tous les faits fiables. Un ACK retardé, doublon ou stale ne peut donc ni ressusciter un ancien joueur ni faire régresser `observed_player_entity_id`.

## 11. Ressources bornées et files

Par client, le runtime conserve au plus :

- un manifeste candidat fiable ;
- deux catalogues sémantiques au plus, actif et staged, plus un rebuild-intent sans catalogue tiers ;
- un snapshot candidat fiable ;
- une baseline acquittée ;
- une image courante ;
- un delta cumulatif remplaçable ;
- une file fiable bornée d’événements lifecycle ;
- une table propre de 64 latches support terminaux, alimentée par le ring global drainé à chaque frame ;
- un registre d’IDs borné par la closure Phase 2.

Les limites Phase 0 de 16 Mio/transaction, 64 parts et 32 Mio de candidates/client s’appliquent avant allocation. Si la candidate dépasse une limite, elle est rejetée entière. La file événementielle utilise les limites et la politique terminale fiables héritées ; elle ne croît jamais sans borne.

## 12. Nettoyage et convergence des erreurs

| Cause | Action obligatoire |
|---|---|
| source temporairement absente | conserver la dernière image cohérente et reprendre sur une capture valide |
| topologie modifiée entre discovery et copie | abandon atomique du tick, diagnostic observable et nouvelle observation au tick systèmes suivant |
| index, signature, référence, flottant obligatoire ou invariant incohérent | ne pas publier la candidate concernée ; conserver la baseline et exposer l’écart |
| cardinalité hors borne | conserver la baseline, exposer la limite atteinte et attendre une fermeture représentable |
| manifeste non installable | conserver le manifeste actif et attendre une candidate valide |
| timeout transaction | abandon candidate et fermeture/resync selon Phase 0 |
| perte de couverture | `SESSION_END`, purge et nouveau handshake éventuel |
| sortie mission | drainage, purge complète, retour `Idle` |
| shutdown | fermeture ordonnée, destruction des sockets après sessions |

## 13. Séquences de référence

### 13.1 Premier flux complet

```text
MissionPreparing
  -> joueur valide
  -> fermeture stable
  -> ManifestPreparing(N)
  -> ManifestPublishing(N)
  -> APPLIED(N)
  -> SnapshotPreparing(S, N)
  -> SnapshotPublishing(S)
  -> APPLIED(S)
  -> Live, delta cumulatif contre S
```

### 13.2 Respawn avec nouvelle classe

```text
old entity DYING/DESTROYED -> événement fiable contre baseline connaissable; keyframe coalesçable
old entity REMOVED         -> ENTITY_DISAPPEARED APPLIED contre ancienne baseline
PlayerPresence::Absent     -> keyframe de suppression et observed_player absent
new signature              -> new entity_id
catalog fingerprint changé -> manifest N+1
APPLIED(N+1)               -> keyframe S+1
APPLIED(S+1)               -> Live, aucune référence à l’ancien ID
```

### 13.3 Fin de support pendant perte réseau

```text
REARMING -> NONE détecté
  -> candidate keyframe terminale immuable
  -> fragments perdus/retransmis
  -> client commit + APPLIED
  -> état moteur plus récent publié en delta cumulatif
```

## 14. Traçabilité

Ce document couvre `P2-REQ-005`, `P2-REQ-009` à `P2-REQ-014`, `P2-REQ-016` à `P2-REQ-020`, `P2-REQ-030` à `P2-REQ-040`, `P2-REQ-042`, `P2-REQ-046` et `P2-REQ-050`.
