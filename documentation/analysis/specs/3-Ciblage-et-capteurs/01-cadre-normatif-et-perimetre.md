# 01 — Cadre normatif et périmètre

## 1. Objet et statut

Ce document fixe le résultat produit, le profil FSTL et le catalogue complet des
exigences `P3-REQ-*`. Il est normatif pour toute implémentation de la Phase 3.

La Phase 3 est additive. Elle conserve les contrats actifs des
[Phases 0, 1 et 2](../README.md). Les layouts `RADAR_CONTACTS` v1/v2/v3 et leurs
golden vectors restent byte-identiques. Le profil `CockpitSensors` utilise
explicitement `RADAR_CONTACTS` v4 sous FSTL 1.1 afin d'ajouter au préfixe v3 la
couleur RGBA et le type de blip résolus par le radar FSO ; aucun décodeur ne
détecte la variante par sa longueur.

## 2. Résultat observable

Une session Phase 3 conforme expose au client la projection `COCKPIT` complète
du joueur observé :

- l’image `CompleteShip` héritée ;
- `TARGET_STATE` et `LOCK_STATE` ;
- `RADAR_STATE`, l’ensemble autorisé de `RADAR_CONTACTS`, `THREAT_STATE` et
  `HUD_ALERT_STATE` ;
- `CARGO_SCAN_STATE` avec divulgation contrôlée ;
- `NAVIGATION_STATE` avec les seuls points autorisés ;
- les événements reconstructibles de cible et de scan annoncés ;
- les catalogues strictement nécessaires aux références révélées.

Le produit compare et sérialise une vue déjà filtrée. Un objet caché ne reçoit
aucun ID public observable, aucun record de contact et aucune entrée de
catalogue révélatrice.

## 3. Autorités et confiance

| Acteur | Autorité Phase 3 |
|---|---|
| moteur FS2Open | vérité de ciblage, lock, capteurs, radar, menace, scan et navigation |
| projection cockpit | applique les mêmes décisions de visibilité que le HUD avant exposition publique |
| producteur télémétrie | valide, copie, filtre, attribue les IDs, diff et sérialise sans modifier le gameplay |
| client de référence | valide FSTL indépendamment et calcule uniquement les valeurs dérivées |
| réseau local | transport UDP non fiable et non authentifié, soumis aux défenses héritées |

Le périmètre livré reste `AuthorityMode.SOLO`,
`VisibilityMode.COCKPIT`, `trustedFullState=false`. Les autres autorités,
le headless et le multijoueur ne sont pas déclarés conformes à cette phase.

## 4. Profil FSTL 1.1

### 4.1 Version et masque

Une session `CockpitSensors` négocie exactement FSTL 1.1 et annonce :

```text
state_domain_coverage = 0x00000000000007CB
```

Ce masque ajoute aux domaines Phase 2 :

- `RADAR_SENSORS=0x0008` ;
- `TARGETING=0x0040` ;
- `NAVIGATION=0x0200`.

`CARGO_DOCK_SUPPORT=0x0100` reste annoncé : Phase 3 étend la projection du
`CARGO_SCAN_STATE` existant, sans changer son bit ni son layout.

### 4.2 Couverture événementielle

Le profil annonce exactement :

```text
event_coverage_state_derived =
    ENTITY | DAMAGE | TARGET | CARGO_SCAN = 0x001D
event_coverage_exact = 0
```

Les transitions observées de cible et de scan peuvent produire les événements
reconstructibles FSTL existants. Elles ne sont pas promises exactes lorsqu’elles
apparaissent et disparaissent entre deux captures. L’état cumulatif reste
autoritaire.

### 4.3 Immutabilité

Version, autorité, visibilité, `state_domain_coverage` et les deux couvertures
d’événements sont figées avant `WELCOME`. Toute promotion depuis `0x0583`, toute
perte d’un domaine ou tout changement de mode exige `SESSION_END`, un nouveau
handshake et un nouveau `session_id`.

## 5. Catalogue des exigences

### 5.1 Compatibilité et périmètre

| ID | Exigence normative |
|---|---|
| `P3-REQ-001` | La Phase 3 conserve tous les comportements, limites et garanties produit de la Phase 2. |
| `P3-REQ-002` | Les artefacts FSTL 1.0 existants et les records `RADAR_CONTACTS` v1/v2/v3 DOIVENT rester byte-identiques. `CockpitSensors` utilise explicitement `RADAR_CONTACTS` v4 sous FSTL 1.1 ; v1 à v4 ne sont jamais autodétectés par longueur. |
| `P3-REQ-003` | Une session `CockpitSensors` DOIT négocier exactement FSTL 1.1 et refuser tout intervalle qui n’inclut pas la minor 1. |
| `P3-REQ-004` | Le masque `CockpitSensors` DOIT valoir exactement `0x07CB`. |
| `P3-REQ-005` | Le profil et les couvertures DOIVENT être figés avant `WELCOME`; toute promotion ou perte ultérieure exige une nouvelle session. |
| `P3-REQ-006` | Le mode livré DOIT rester `SOLO + COCKPIT`, non trusted et non headless. |
| `P3-REQ-007` | Le producteur DOIT rester strictement en lecture seule et NE DOIT accepter aucune commande de ciblage, navigation, arme, scan, IA ou mission. |
| `P3-REQ-008` | `ALL_ENTITIES`, `LOW_FREQUENCY_EFFECTS`, communication, client applicatif final et vidéo cible DOIVENT rester hors du profil et de ses dépendances de réussite. |

### 5.2 Capture, projection et identités

| ID | Exigence normative |
|---|---|
| `P3-REQ-009` | Toute lecture moteur et toute décision publique Phase 3 DOIVENT être capturées sur le thread principal dans `EngineUpdate`; aucun worker, SPSC, dereference différée ou seam de test produit n’est introduit. |
| `P3-REQ-010` | Joueur, IA, cible, signatures, types, instances, sous-systèmes, armes, navpoints et relations DOIVENT être validés avant lecture. Une référence cible transitoirement périmée ou un slot réutilisé retire la cible et toute piste ambiguë pour ce tick; les autres incohérences refusent atomiquement le bloc concerné. |
| `P3-REQ-011` | Les DTO Phase 3 DOIVENT posséder toutes leurs chaînes, listes et valeurs; aucun pointeur, vue, itérateur, handle ou index moteur ne survit à la capture. |
| `P3-REQ-012` | Cible, locks et voyants HUD sont capturés à `flightHz`; radar, contacts, menace, cargo et navigation à `systemsHz`; toute keyframe force une capture cohérente de tous les blocs au même sample time. Un snapshot ou delta ordinaire peut légalement conserver ces familles à leurs sample times propres. |
| `P3-REQ-013` | Avec joueur présent, chaque snapshot `0x07CB` DOIT contenir exactement la matrice Phase 2 plus les six singletons joueur Phase 3 et tous les contacts autorisés; sans joueur, seuls les singletons globaux hérités subsistent. |
| `P3-REQ-014` | Le filtrage `COCKPIT` DOIT précéder attribution d’ID public, construction de catalogue, diff et sérialisation. |
| `P3-REQ-015` | Tout objet rendu public par ciblage, radar, menace, cargo ou navigation reçoit un `entity_id` non nul, stable pour sa signature dans la session, jamais réutilisé et issu d’un registre borné à 65 536 identités. |
| `P3-REQ-016` | Un ID de piste capteur NE DOIT PAS matérialiser implicitement un `ENTITY_LIFECYCLE` ou l’état complet du contact; seule la fermeture joueur/support/docking héritée reçoit la matrice `CORE_SHIP`. |
| `P3-REQ-017` | Le manifeste actif DOIT contenir toutes les classes de vaisseau ou d’arme effectivement référencées par un ID de classe révélé, et aucune classe cachée; il est installé et acquitté `APPLIED` avant l’état qui le référence. Un libellé HUD autoritaire n'est pas une référence de manifeste. Un missile entrant dont la classe dynamique est absente du catalogue courant est omis jusqu’à installation, sans fermer la session. |
| `P3-REQ-018` | Une nouvelle définition révélable par ID produit un manifeste strictement supérieur puis une keyframe; apparition, disparition, distorsion, nouveau libellé HUD d’une piste ou arme dynamique non installée avec catalogue inchangé ne change pas le manifeste. |

### 5.3 Ciblage et locks

| ID | Exigence normative |
|---|---|
| `P3-REQ-019` | `TARGET_STATE` DOIT reproduire la cible courante et précédente indépendamment de `RADAR_CONTACTS`, pour vaisseau, arme, débris, astéroïde et jump node. Il est l'autorité courante de sélection côté client; `CURRENT_TARGET` décrit seulement la décision au sample time du contact. Le profil live `CockpitSensors` utilise explicitement v5 pour les valeurs visibles D/S, le libellé HUD conditionnel, la couleur HUD brillante et les libellés HUD des sous-systèmes ciblé et lock ; v1/v2/v3/v4 restent des compatibilités de capture. |
| `P3-REQ-020` | Identité, classe, équipe, IFF et sous-systèmes ciblés DOIVENT être présents seulement lorsqu’ils sont révélés et résolubles; les IDs de sous-système exigent le manifeste installé, tandis que leurs libellés HUD v5 proviennent de l'instance live validée et restent indépendants du manifeste. Une cible perdue ne conserve que les groupes de dernière observation explicitement autorisés. |
| `P3-REQ-021` | Le lead publié DOIT être le résultat autoritaire monde associé à une banque valide; distances géométriques, angles, pixels, brackets et progressions restent absents. |
| `P3-REQ-022` | `LOCK_STATE` DOIT publier la liste complète de 0 à 64 points de lock, avec absence explicite de tentative, IDs cohérents, état locked, cône, position monde et durée restante bornée. |

### 5.4 Radar, contacts et menaces

| ID | Exigence normative |
|---|---|
| `P3-REQ-023` | `RADAR_STATE` DOIT reproduire mode, portée, état/intégrité capteurs et groupes AWACS et EMP applicables. La visibilité `VISIBLE` ou `DISTORTED` est une propriété de chaque `RADAR_CONTACT`, jamais une intensité globale de radar. |
| `P3-REQ-024` | L’ensemble des `RADAR_CONTACTS` DOIT être exactement l’ensemble de pistes que la projection HUD autorise pour le joueur au même sample time, après AWACS, furtivité, cloak, équipe et règles mission. Chaque record v2/v3/v4 capture dans ce même tick `radar_local_position` dans le repère du radar standard et `radar_projection_distance=RadarContactProjection.distance`; v4 capture aussi la décision visuelle finale et le flag cible propres à ce sample radar. |
| `P3-REQ-025` | Chaque vaisseau `VISIBLE` de `RADAR_CONTACTS` v3/v4 DOIT publier le nom affichable et le libellé de type produits par les mêmes helpers que le Target Box; un nom masqué reste absent. Chaque contact v4 publié porte la couleur RGBA et le type de blip finaux produits par le radar, palettes de mod, accessibilité et overrides inclus. Les pistes `DISTORTED`/`NOT_VISIBLE` omettent nom, classe, libellé, équipe et IFF mais conservent leur décision visuelle autorisée. Position et vitesse restent l’observation capteur autorisée, jamais la vérité cachée. |
| `P3-REQ-026` | Apparition et retrait d’une piste utilisent les atomes `CREATE/DELETE` exacts; les deltas cumulatifs contiennent le remplacement net complet de chaque contact contre la baseline. |
| `P3-REQ-027` | `THREAT_STATE` DOIT rester v1 et reproduire le niveau agrégé, les références autorisées et la liste complète des missiles entrants visant le joueur dont la classe est installée. Un missile à classe dynamique absente est omis pour le tick sans fermeture de session. |
| `P3-REQ-028` | Chaque missile entrant DOIT porter un ID stable, une classe installée, un guidage, une visibilité, une pose et une vitesse valides; plus de 256 missiles autorisés fait perdre le profil sans troncature. |

### 5.5 Cargo, navigation et valeurs dérivées

| ID | Exigence normative |
|---|---|
| `P3-REQ-029` | `CARGO_SCAN_STATE` DOIT suivre la cible autorisée même hors fermeture Phase 2 au moyen du même ID public de piste, sans matérialiser son état complet. |
| `P3-REQ-030` | Phase, divulgation, temps et validités de scan DOIVENT suivre la décision gameplay; `cargo_text` est présent si et seulement si `disclosure=REVEALED`, et `COMPLETED+HIDDEN` reste valide. |
| `P3-REQ-031` | `NAVIGATION_STATE` DOIT contenir la liste complète et ordonnée des navpoints et waypoints autorisés, leurs IDs stables, la destination courante, la route et la décision d’autopilote. |
| `P3-REQ-032` | L’état et le refus d’autopilote DOIVENT être cohérents avec le mode de contrôle hérité sans dupliquer les axes ou modes de vol de `CONTROL_STATE`; aucune commande distante n’est créée. |
| `P3-REQ-033` | Distances, relèvements, vitesses relatives, TTC, temps d’impact/interception, âge de piste, progressions, ETA et cadence visuelle des voyants DOIVENT rester dérivés côté client à partir des sources canoniques. Les lectures D/S, libellés HUD, couleurs radar/cible et états discrets d'alerte sont les exceptions autoritaires. La coordonnée du radar standard est dérivée exclusivement des entrées autoritaires `RADAR_CONTACTS` v2/v3/v4 en live ; la reconstruction depuis `FLIGHT_STATE` est une compatibilité v1 explicite seulement. |
| `P3-REQ-034` | Tout flottant publié DOIT être fini et canonisé pour `-0`; enums, IDs, temps, listes et références hors borne provoquent un échec fermé observable, jamais un clamp ou une troncature non autorisés. Une omission explicitement autorisée pour une référence dynamique périmée ou dont la classe n’est pas installée n'est pas un échec de capture. |

### 5.6 Réplication, ressources et exploitation

| ID | Exigence normative |
|---|---|
| `P3-REQ-035` | Snapshot initial, keyframes périodiques et resync DOIVENT être exhaustifs et installés atomiquement après validation et `ACK APPLIED`. La cohérence cible/contact est stricte à sample time égal; à sample times différents, chaque atome est validé séparément. |
| `P3-REQ-036` | Chaque delta DOIT rester cumulatif contre la baseline acquittée et remplacer des atomes FSTL complets; un delta supérieur à 1 Mio est remplacé par une keyframe. |
| `P3-REQ-037` | Lorsqu’une cible, un lock, un contact, une menace, un scan ou un navpoint désignent le même objet public, ils DOIVENT employer le même ID et respecter les règles de résolution du document 04. |
| `P3-REQ-038` | Changement de mission, disparition du joueur, changement de cible et retrait de piste DOIVENT purger ou remplacer les atomes dépendants sans état ancien observable après commit. Un ancien flag radar peut subsister jusqu'au prochain tick `systemsHz`, mais ne commande jamais la sélection courante. |
| `P3-REQ-039` | La couverture événementielle DOIT valoir exactement `state_derived=0x001D`, `exact=0`; `HUD_ALERT_STATE` reste un état échantillonné et aucun événement bref manqué n’est inventé comme capture exacte. |
| `P3-REQ-040` | Contacts, locks, missiles, navpoints, waypoints, records, IDs et transactions DOIVENT respecter leurs plafonds; un dépassement retire le profil avant session ou termine proprement la session s’il survient ensuite. |
| `P3-REQ-041` | La configuration v3 DOIT sélectionner explicitement un profil fermé; `CockpitSensors` produit `0x07CB`, tandis que les configurations v1/v2 conservent exactement leur comportement antérieur. |
| `P3-REQ-042` | Toutes les ressources Phase 3 DOIVENT être préallouées avant `Ready`; le budget total reste inférieur ou égal à 512 Mio pour quatre clients et aucune croissance steady-state non bornée n’est permise. |
| `P3-REQ-043` | Métriques et logs DOIVENT distinguer capture, filtrage, contacts, alertes HUD, IDs, manifestes, limites et erreurs sans exposer nom, texte d'avertissement, cargo, navpoint, adresse, ID public ou donnée cachée. Avant toute fermeture Phase 3, sélection du manifeste comprise, ils publient un bloc et un statut issus d'enums fermés ainsi qu'un compteur correspondant; le diagnostic terminal reste disponible malgré saturation de la file de logs et purge du runtime. |
| `P3-REQ-044` | Désactivé, le module reste inerte; activé, son chemin frame reste non bloquant, déterministe et borné, sans attente réseau ni allocation après warm-up. |
| `P3-REQ-045` | Le client de référence DOIT décoder indépendamment le chemin moteur → projection filtrée → manifeste/snapshot/delta et exposer valeur brute, sample time et provenance sans recalculer une décision sensible. Le dashboard suit la cible la plus récente de `TARGET_STATE`, jamais un flag `CURRENT_TARGET` retenu à une cadence plus lente, et utilise `HUD_ALERT_STATE` sans reconstruire un avertissement absent. |
| `P3-REQ-046` | Après la perte contrôlée d’un delta contenant un changement Phase 3, le client DOIT converger sur le delta cumulatif suivant ou la prochaine keyframe sans fuite, référence pendante ni perte de session. |
| `P3-REQ-047` | Le profil live `CockpitSensors` DOIT publier `HUD_ALERT_STATE` v1 à `flightHz`, avec menace primaire et verrouillage indépendants ainsi que l'unique avertissement textuel accepté par FSO, son temps restant et son instance. Aucune frame, phase, bitmap, couleur ou cadence d'animation n'est transmise. |

## 6. Exclusions et propriétaires ultérieurs

| Domaine exclu | Propriétaire | Règle Phase 3 |
|---|---|---|
| toutes les entités et projectiles | Phase 4 | les pistes capteurs ne deviennent pas des entités complètes |
| client applicatif et communication | Phase 5 | seul le client de référence est requis |
| hooks exhaustifs et worker/SPSC | Phase 6 | couverture exacte nulle, capture main-thread |
| rendu et vidéo cible | Phase 7 | aucun renderer, FFmpeg ou H.264 |

## 7. Invariants transversaux

1. Le wire public reste indépendant de l’ABI moteur.
2. Le filtrage précède toute exposition d’ID ou de contenu.
3. Une absence de champ signifie absence sémantique, jamais échec silencieux.
4. Les IDs publics sont stables et ne sont jamais des indices moteur.
5. Les données sensibles restent autoritaires côté producteur.
6. Les valeurs reproductibles restent dérivées côté client.
7. Le produit reste strictement en lecture seule.
