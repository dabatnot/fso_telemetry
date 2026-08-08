# 03 — Flux, cycle de vie et concurrence

## 1. Objet

Ce document fixe l’ordonnancement des captures Phase 3, les transitions de
pistes, les changements de cible, la coordination avec les manifestes et la
réplication cumulative.

## 2. États runtime

Le runtime hérite de la machine Phase 2 et ajoute les sous-états :

```text
SensorUnavailable
  -> SensorProjecting
  -> CatalogReady
  -> SnapshotPublishing
  -> Live
  -> Resynchronizing
```

`SensorUnavailable` signifie qu’aucune projection complète n’est publiable. Il
ne signifie pas que les capteurs gameplay sont détruits : un `RADAR_STATE`
valide peut déclarer `OFFLINE` ou `DEGRADED` et reste alors un état produit
normal.

## 3. Ordonnancement d’un tick

### 3.1 Tick ordinaire

Sur chaque `EngineUpdate` :

1. drainer les transitions héritées ;
2. valider joueur et génération de mission ;
3. si le bloc ciblage est dû, capturer cible, locks et voyants HUD ;
4. si le bloc systèmes est dû, capturer radar, contacts, menace, cargo et
   navigation ;
5. appliquer le filtre `COCKPIT` à la projection complète ;
6. résoudre les IDs publics et dépendances de catalogue ;
7. valider les références croisées ;
8. reconstruire seulement les atomes dus ;
9. calculer le delta cumulatif contre la baseline acquittée ;
10. encoder dans les buffers préalloués.

Un bloc non dû conserve sa dernière valeur validée et son sample time propre.
Un changement de mission, de joueur, de catalogue ou une keyframe force tous
les blocs au même sample time.

### 3.2 Capture incohérente

Si une signature, cible, liste radar ou route change pendant la copie, la
candidate du bloc est abandonnée. Le runtime conserve l’image précédente et
réessaie au prochain tick dû. Il ne mélange jamais la cible d’un instant avec
les locks, contacts ou menaces d’un autre instant sous un sample time commun.

## 4. Démarrage d’une session

La séquence nominale est :

```text
profil 0x07CB sélectionné
  -> joueur valide
  -> projection COCKPIT complète
  -> dépendances catalogue calculées
  -> manifeste N publié
  -> APPLIED(N)
  -> snapshot S publié
  -> APPLIED(S)
  -> Live
```

Tant que manifeste et snapshot ne sont pas appliqués, aucune donnée nouvelle
révélée n’est exposée en delta.

## 5. Cible et locks

### 5.1 Changement de cible

À un changement observé :

- `TARGET_STATE.current_target_entity_id` prend le nouvel ID ou zéro ;
- `previous_target_entity_id` est présent seulement si le moteur conserve une
  cible précédente autorisée ;
- les groupes d’identité et sous-système sont recalculés après filtrage ;
- `LOCK_STATE` est remplacé intégralement ;
- les flags `CURRENT_TARGET` des contacts sont cohérents au même sample time ;
- lorsque le radar n'est pas dû au même tick, ses contacts conservent leurs
  anciens flags et leur propre sample time sans invalider le nouvel atome cible ;
- le client détermine sélection, emphase et priorité depuis le
  `TARGET_STATE.current_target_entity_id` le plus récent ;
- la couleur HUD brillante de la nouvelle cible est remplacée avec le même
  atome et ne dépend pas de l'existence d'un blip radar ;
- un événement `TARGET_CHANGED` reconstructible PEUT être émis avec la
  dépendance de baseline correspondante.

Le delta contient toutes les différences cumulées depuis la baseline. Si
plusieurs changements surviennent entre deux captures, l’état final est
correct ; aucune exactitude des transitions intermédiaires n’est annoncée.

La cible est résolue une fois par projection avec index, signature, type et
instance. Si sa signature a disparu ou si le slot a été réutilisé, la cible est
vide et la piste de remplacement ambiguë est omise pour ce tick. Cette omission
transitoire ne ferme pas la session et le prochain tick converge normalement.

### 5.2 Perte de cible furtive

Lorsque le moteur conserve une dernière observation autorisée :

- le même ID public est conservé ;
- seuls les groupes `LAST_STEALTH_OBSERVATION` et autres décisions encore
  autorisées subsistent ;
- identité et classe non autorisées sont effacées par remplacement complet ;
- le contact radar peut devenir `DISTORTED` ou `NOT_VISIBLE` selon la piste HUD.

Lorsque le moteur oublie la piste :

- le contact reçoit un `DELETE` cumulatif ;
- cible et locks retirent la référence ;
- aucun atome ne conserve position, nom ou classe ;
- l’ID n’est jamais réattribué à un autre objet.

### 5.3 Locks

La liste `LOCK_STATE` est un atome complet. Ajouter, acquérir, perdre ou changer
un point remplace toute la liste dans le delta. Une tentative absente est
représentée par l’absence du groupe, jamais par un temps sentinelle.

## 6. Pistes radar

### 6.1 Apparition

Une piste nouvellement autorisée :

1. passe le filtre cockpit ;
2. reçoit ou retrouve son ID public ;
3. déclenche un manifeste si une nouvelle classe révélée est nécessaire ;
4. apparaît par `CREATE` dans la première image qui peut résoudre toutes ses
   références.

### 6.2 Mise à jour

Une modification de visibilité, position observée, vitesse, flags, identité,
couleur ou type de blip
remplace l’atome complet `(observer_id,contact_entity_id)`. Un champ auparavant
présent et devenu caché disparaît du nouveau masque de présence.

### 6.3 Retrait

Une piste retirée produit `DELETE(observer_id,contact_entity_id)`. Le delta
cumulatif conserve ce `DELETE` tant que la baseline contient encore la piste.
Après installation d’une keyframe sans cette piste, le tombstone n’est plus
nécessaire.

Une piste apparue puis retirée depuis la même baseline et jamais présente dans
celle-ci ne produit aucun atome net.

## 7. Menaces et missiles

`THREAT_STATE` est remplacé comme un ensemble atomique. Les IDs de missile sont
alloués dans le même registre public :

- un missile visible comme contact réutilise le même ID ;
- un missile uniquement connu par l’autorité de menace peut être référencé sans
  devenir contact radar ni entité complète ;
- sa classe d’arme est installée avant exposition ;
- si sa classe dynamique est absente du catalogue Phase 2 courant, le missile
  est omis pour ce tick sans expansion synthétique du manifeste ni fermeture
  de session ;
- sa disparition le retire de la liste suivante ;
- plus de 256 missiles autorisés refuse l’image et provoque la perte propre du
  profil, jamais une sélection des « plus dangereux ».

`HUD_ALERT_STATE` est un atome distinct rafraîchi à `flightHz`. Il conserve
simultanément le voyant de tir primaire et l'état de verrouillage missile. Son
groupe d'avertissement est présent seulement tant que le texte retenu par FSO
est actif ; son `warning_instance_id` change uniquement lorsqu'un appel est
accepté après les priorités natives. Ce compteur d'instance ne transforme pas
l'état échantillonné en couverture événementielle exacte.

## 8. Cargo et navigation

### 8.1 Scan

La cible de scan peut être hors fermeture Phase 2 si elle est une cible publique
autorisée. Elle réutilise l’ID de ciblage ou de piste. Cela n’ajoute ni
`ENTITY_LIFECYCLE`, ni état complet, ni relation de docking.

Les transitions observées `IDLE -> SCANNING -> COMPLETED` remplacent
`CARGO_SCAN_STATE`. La divulgation peut rester `HIDDEN` après `COMPLETED`. Un
événement reconstructible peut accompagner la transition, mais le texte cargo
ne devient public qu’au premier état `REVEALED`.

### 8.2 Navigation

`NAVIGATION_STATE` est un atome complet :

- toute modification de liste remplace navpoints et route ensemble ;
- les IDs de navpoint restent stables dans `mission_generation` ;
- le navpoint courant doit appartenir à la liste courante ;
- un refus d’autopilote porte exactement une raison fermée ;
- un changement de mission purge tous les IDs et routes.

Une liaison vers une entité ne force jamais son exposition. Le lien est omis si
le navpoint reste autorisé sans lui ; sinon le point entier est filtré.

## 9. Manifestes et keyframes

Un changement de piste sans nouvelle définition utilise `CREATE/DELETE` et ne
change pas de manifeste. Une nouvelle classe révélée suit :

```text
image courante sous N
  -> manifeste N+1 candidat
  -> APPLIED(N+1)
  -> keyframe S+1 exhaustive
  -> APPLIED(S+1)
  -> exposition de la nouvelle classe
```

Les anciennes baselines restent valides jusqu’au commit. Une nouvelle décision
de visibilité pendant l’aller-retour ne modifie jamais les octets candidats ;
elle est coalescée dans une candidate successeur.

## 10. Événements et couverture

La Phase 3 ajoute les familles `TARGET` et `CARGO_SCAN` à la couverture
state-derived. Les événements utilisent les kinds et payloads FSTL existants,
sont ordonnés par `event_id` et liés à une baseline connaissable.

`event_coverage_exact=0` interdit d’affirmer qu’un changement cible-retour-cible
ou un scan très bref a été capturé s’il se produit entièrement entre deux
samples. Aucun hook n’est ajouté pour corriger cette limite dans cette phase.

## 11. Joueur absent, respawn et mission

Sans joueur :

- `observed_player_entity_id` est absent ;
- aucun record Phase 3 à clé joueur ni contact n’est présent ;
- le manifeste actif reste gelé comme en Phase 2 ;
- le masque de session reste `0x07CB`.

Au respawn, le joueur reçoit un nouvel ID. Les registres de cible, piste,
missile, navpoint, baseline et événement sont purgés ou recréés selon la
génération de mission. Aucune piste de l’ancien joueur ne survit implicitement.

Une sortie mission termine les sessions avant nettoyage des sources. La mission
suivante utilise une nouvelle `mission_generation`, de nouveaux IDs et un
nouveau snapshot initial.

## 12. Perte, désordre et resync

- Un delta perdu est remplacé par un delta plus récent de la même baseline.
- Un `DELETE` perdu reste présent dans les deltas cumulatifs suivants.
- Une keyframe candidate n’est jamais utilisée comme baseline avant `APPLIED`.
- Un `RESYNC_REQUEST` valide produit une keyframe exhaustive.
- Un ACK ancien, doublon ou impossible suit les règles Phase 0/2.
- Un changement de visibilité ne peut jamais être rétroactivement injecté dans
  une transaction déjà publiée.

## 13. Concurrence et ressources

Toutes les captures se font sur le thread principal. Le transport réutilise le
socket non bloquant et les buffers existants. Aucun pointeur moteur n’est lu
depuis le chemin réseau.

Le runtime conserve par client au plus :

- une baseline, une image courante et une keyframe candidate ;
- un delta cumulatif ;
- un manifeste actif et un staged ;
- 4096 contacts courants ;
- 65 536 entrées d’identité privées ;
- 64 locks, 256 missiles, 1024 navpoints et 2048 waypoints ;
- les files fiables et métriques bornées héritées.

## 14. Traçabilité

Ce document couvre `P3-REQ-005`, `P3-REQ-009` à `P3-REQ-018`,
`P3-REQ-022`, `P3-REQ-026`, `P3-REQ-028` à `P3-REQ-032`,
`P3-REQ-035` à `P3-REQ-040` et `P3-REQ-046`.
