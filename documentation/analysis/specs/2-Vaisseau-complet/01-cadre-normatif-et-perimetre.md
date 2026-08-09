# 01 — Cadre normatif et périmètre

## 1. Objet et statut

Ce document fixe la portée, les acteurs, les profils FSTL et le catalogue complet des exigences `P2-REQ-*`. Il est normatif pour toute implémentation de la Phase 2.

La Phase 2 est additive : elle conserve les garanties des contrats [Phase 0](../0-Contrat-de-protocole/README.md) et [Phase 1](../1-Squelette-et-premier-flux/README.md). Elle NE DOIT modifier aucun octet, registre, golden vector ou comportement normatif FSTL 1.0.

La Phase 2 étend le produit livré en Phase 1 et conserve ses comportements et sa compatibilité FSTL.

## 2. Résultat observable attendu

Une session Phase 2 conforme expose en lecture seule l’état complet du vaisseau joueur dans la visibilité `COCKPIT`. Elle produit :

- une transaction manifeste fiable et atomique ;
- un `FULL_SNAPSHOT` complet référant ce manifeste ;
- des `DELTA` cumulatifs contre la baseline de snapshot acquittée ;
- les renouvellements de keyframe et resynchronisations hérités ;
- un flux lifecycle cohérent pendant absence, apparition, mort, disparition et respawn ;
- un client de référence capable d’afficher les valeurs brutes et dérivées et leur provenance.

Les valeurs moteur capturées sont `A` ou `C` selon [01-telemetry-data-inventory.md](../../01-telemetry-data-inventory.md). Les ratios, pourcentages, progressions, ETA et états de jauge reproductibles sont `D` et NE DOIVENT PAS devenir une seconde vérité filaire.

## 3. Acteurs, autorité et frontière de confiance

| Acteur | Autorité et obligation Phase 2 |
|---|---|
| Moteur FS2Open | source de vérité lue sur le thread principal après validation de `Player`, `Player_obj` et `Player_ship` |
| Producteur télémétrie | copie, valide, normalise, filtre `Cockpit`, diff, sérialise et transmet sans modifier la simulation |
| Client de référence | valide FSTL indépendamment, installe atomiquement manifeste/snapshot, applique les deltas et recalcule les valeurs `D` |
| Comparateur local | rapproche au même tick la copie moteur et l’état décodé sans ajouter d’API publique moteur |
| Réseau local | transport non fiable et non authentifié ; loopback et allowlist hérités restent les défenses par défaut |

Le périmètre de livraison est `AuthorityMode.SOLO`, `VisibilityMode.COCKPIT`, `trustedFullState=false`. Les chemins `MULTIPLAYER_CLIENT` et `MULTIPLAYER_MASTER` restent compatibles avec l’architecture, tandis que leur production métier complète est différée. `DEDICATED_SERVER` sans joueur de cockpit n'annonce pas le profil Phase 2.

Le client NE DOIT envoyer aucune commande de vol, arme, support ou mission. Aucun message reçu ne peut modifier un champ moteur.

## 4. Profils FSTL 1.1

### 4.1 Version

Le producteur Phase 2 DOIT annoncer et accepter exactement `protocol_major=1`, `protocol_minor=1`. Un client limité à FSTL 1.0 est refusé avec la raison héritée `UnsupportedVersion`. Aucun fallback 1.0 n’est autorisé dans une session Phase 2.

Les registres suivants restent inchangés :

- `PLAYER_KINEMATICS=0x0400` ;
- `CORE_SHIP=0x0001` ;
- `CONTROL_INPUTS=0x0002` ;
- `WEAPONS=0x0080` ;
- `CARGO_DOCK_SUPPORT=0x0100`.

### 4.2 Profil cœur de vaisseau

Le premier profil de promotion est exactement `0x0401 = PLAYER_KINEMATICS | CORE_SHIP`. Il DOIT contenir les singletons `SESSION_STATE` et `MISSION_STATE`, puis, si le joueur existe, un unique `ENTITY_LIFECYCLE`, `SHIP_IDENTITY`, `FLIGHT_STATE`, `DAMAGE_STATE`, `SHIELD_STATE`, tous les `SUBSYSTEM_STATE`, `ENERGY_STATE` et `PROPULSION_STATE` applicables.

Ce profil contient la cinématique et l'état structurel du joueur. `CONTROL_STATE`, `WEAPON_STATE` et `SUPPORT_STATE` appartiennent au profil final, car FSTL lie ces records à leurs domaines respectifs.

### 4.3 Profil final de livraison

Le profil final est exactement `0x0583 = PLAYER_KINEMATICS | CORE_SHIP | CONTROL_INPUTS | WEAPONS | CARGO_DOCK_SUPPORT`.

Il ajoute :

- un `CONTROL_STATE` lorsque le joueur observé existe ;
- un `WEAPON_STATE` pour chaque vaisseau exporté : le joueur et toute entité de la fermeture service/docking autorisée ;
- un `CARGO_SCAN_STATE` pour le joueur ;
- un `DOCKING_STATE` et un `SUPPORT_STATE` pour chaque vaisseau exporté, avec phase `NONE` et listes vides lorsque non applicables.

Cette fermeture du domaine `CARGO_DOCK_SUPPORT` est imposée par la matrice et la validation référentielle Phase 0 : toute entité ship publiée DOIT posséder `ENTITY_LIFECYCLE`, puis tous les records `CORE_SHIP` et `WEAPONS`. La fermeture contient donc le joueur et la composante support/docking transitivement référencée et autorisée, sans devenir `ALL_ENTITIES`. Une cible cargo extérieure n'est pas publiée comme entité : `CARGO_SCAN_STATE` devient `NOT_SCANNABLE/HIDDEN`, sans groupe optionnel, extension de fermeture ni fin de session. La Phase 3 conserve la propriété du scan/ciblage étendu et la Phase 4 celle du docking global.

### 4.4 Immutabilité et promotion

`state_domain_coverage`, `event_coverage_state_derived` et `event_coverage_exact` sont immuables après `SESSION_BEGIN`. Passer de `0x0400` ou `0x0401` à `0x0583` exige `SESSION_END`, un nouveau handshake et un nouveau `session_id`.

Le profil final DOIT annoncer `event_coverage_state_derived = ENTITY | DAMAGE (0x0005)` et `event_coverage_exact = 0`. `ENTITY` couvre apparition/disparition et `DAMAGE` les transitions observées disabled/dying/destroyed ; la Phase 2 NE DOIT PAS prétendre couvrir exactement tous les événements brefs d’une famille.

## 5. Catalogue des exigences

### 5.1 Compatibilité et périmètre

| ID | Exigence normative |
|---|---|
| `P2-REQ-001` | La Phase 2 conserve les comportements, limites et garanties produit de la Phase 1. |
| `P2-REQ-002` | Les artefacts FSTL 1.0 DOIVENT rester byte-identical ; aucun type, champ, bit ou layout v1.0/1.1 existant ne peut changer. |
| `P2-REQ-003` | Une session Phase 2 DOIT négocier exactement FSTL 1.1 et refuser tout intervalle sans minor 1. |
| `P2-REQ-004` | Le profil cœur utilise exactement `0x0401` et le profil final exactement `0x0583`. |
| `P2-REQ-005` | La couverture DOIT être figée avant `WELCOME`; toute promotion ou perte ultérieure exige une nouvelle session. |
| `P2-REQ-006` | Le mode livré DOIT être `SOLO + COCKPIT`; `TrustedFullState`, headless et les autres autorités NE DOIVENT PAS être annoncés comme conformes Phase 2. |
| `P2-REQ-007` | Le producteur DOIT rester strictement read-only et NE DOIT exposer aucune commande distante. |
| `P2-REQ-008` | Les données de cible, radar, navigation, communication et vidéo DOIVENT rester absentes du profil et des dépendances de réussite. |

### 5.2 Capture, architecture et concurrence

| ID | Exigence normative |
|---|---|
| `P2-REQ-009` | Toute lecture lourde et toute capture d’état publié DOIT se faire sur le thread principal dans `EngineUpdate`. Les autorités gameplay existantes peuvent déposer des faits scalaires bornés dans des latches possédés ; le produit NE DOIT exposer aucun seam de test ni injection de manifeste/catalogue et aucune déréférence moteur différée. |
| `P2-REQ-010` | `Player`, `Player_obj`, `Player_ship`, types, instances, signatures et index DOIVENT être validés avant toute déréférence ; une incohérence produit un statut fermé et observable. |
| `P2-REQ-011` | Le DTO Phase 2 DOIT posséder toutes ses chaînes et listes ; aucun pointeur, référence, itérateur ou index moteur ne survit à la capture. |
| `P2-REQ-012` | La Phase 2 NE DOIT introduire ni worker, ni SPSC, ni lecture concurrente des tables moteur ; cette architecture reste Phase 6. |
| `P2-REQ-013` | Le chemin actif DOIT être non bloquant, borné et préalloué en régime permanent ; aucun appel réseau bloquant n’est permis dans la frame. |
| `P2-REQ-014` | Un tick de keyframe DOIT forcer une capture cohérente de tous les blocs Phase 2 au même `producer_sample_time_us`. Un tick ordinaire DOIT capturer et reconstruire uniquement les blocs dus selon `flightHz`/`systemsHz`, conserver atomiquement les derniers atomes validés des blocs non dus avec leur sample time propre, et limiter le diff aux atomes reconstruits ou invalidés par un changement de topologie/lifecycle. |

### 5.3 Manifestes, identités et références

| ID | Exigence normative |
|---|---|
| `P2-REQ-015` | Une transaction `ManifestKind.FullRequired` DOIT contenir les ensembles exhaustifs de records `CLASS_MANIFEST` et `WEAPON_MANIFEST` de la fermeture autorisée ; l’ensemble armes PEUT être vide seulement si aucune classe d’arme n’est référencée. |
| `P2-REQ-016` | Le manifeste DOIT être validé, installé atomiquement et acquitté `APPLIED` avant tout snapshot qui le référence. |
| `P2-REQ-017` | Les IDs de classe, arme, sous-système et banque DOIVENT être non nuls, stables dans la génération de manifeste et indépendants des pointeurs ou indices bruts moteur. |
| `P2-REQ-018` | Un changement de classe, de composition de loadout ou de descripteur sémantique canonique pré-ID qui modifie le fingerprint catalogue DOIT produire un `manifest_id` strictement supérieur, puis une keyframe ; un simple changement d’instance/topologie avec catalogue inchangé force seulement une keyframe. Aucun delta ne change `required_manifest_id`. |
| `P2-REQ-019` | Pour `CompleteShip`, la fermeture DOIT être le plus petit point fixe partant du joueur et ajoutant, pour chaque membre déjà autorisé, son support assigné et toutes ses relations de docking transitives, avec les définitions classes/armes nécessaires. Elle NE DOIT suivre ni chef de groupe, ni cible cargo. `CoreGateClosure` reste exactement `{joueur}`. |
| `P2-REQ-020` | Dans `CompleteShip`, toute référence d’entité publiée non nulle DOIT résoudre un `ENTITY_LIFECYCLE` dans le snapshot et tout ship matérialisé DOIT recevoir la matrice complète `CORE_SHIP` et `WEAPONS`. Une cible cargo extérieure est omise et représentée `NOT_SCANNABLE/HIDDEN` sans ID opaque, extension de fermeture ni fin de session. Dans `CoreGate`, le seul ship matérialisé est le joueur. |

### 5.4 Données du vaisseau

| ID | Exigence normative |
|---|---|
| `P2-REQ-021` | Le snapshot DOIT contenir exactement un `SHIP_IDENTITY` par vaisseau exporté, dont un pour le joueur présent, et lier chacun à une classe installée. |
| `P2-REQ-022` | `DAMAGE_STATE` DOIT publier coque courante, maximum dynamique, protections et seulement les groupes optionnels réellement autoritaires et bornés. |
| `P2-REQ-023` | `SHIELD_STATE` DOIT publier la liste complète de 0 à 64 segments, sans hypothèse de quatre quadrants ; absence de bouclier = `has_shields=0` et listes vides. |
| `P2-REQ-024` | Pour chaque vaisseau exporté, tous les sous-systèmes déclarés par sa classe DOIVENT avoir exactement un `SUBSYSTEM_STATE`. Sous `CORE_SHIP`, tout changement d’ensemble/définition exige un nouveau manifeste et une keyframe ; `CREATE/DELETE` de sous-système est interdit. |
| `P2-REQ-025` | Les tourelles DOIVENT publier leur état permis par `SUBSYSTEM_STATE`, sans cible, lock, AWACS ou information cachée appartenant à la Phase 3. |
| `P2-REQ-026` | `ENERGY_STATE` DOIT respecter ETS `0..12`; un vaisseau `No_ets` encode les indices à zéro et omet les groupes incompatibles. |
| `P2-REQ-027` | `PROPULSION_STATE` DOIT rester cohérent avec les modes de `FLIGHT_STATE` lorsqu’ils proviennent de la même capture, obligatoirement dans toute keyframe ; un delta à cadences indépendantes conserve ses sample times et NE DOIT PAS être déclaré incohérent du seul fait qu’un bloc est plus ancien. L’afterburner suit les présences fermées du document 04. |
| `P2-REQ-028` | `CONTROL_STATE` DOIT reproduire les six axes, le mode, les flags et les groupes conditionnels v1 ; les compteurs sont des demandes du tick, pas des tirs. |
| `P2-REQ-029` | Chaque vaisseau exporté DOIT posséder un `WEAPON_STATE` publiant les listes complètes et hétérogènes primaires/secondaires, le scalaire de banque tertiaire courante et la contre-mesure selon les bornes et présences v1. |
| `P2-REQ-030` | Chaque vaisseau exporté DOIT posséder un `SUPPORT_STATE`; phase, flags et entité de support suivent la source, tandis que les quantités et délais bruts proviennent des autres records, jamais d’un pourcentage inventé. Le dernier terminal par entité DOIT être capturé avant nettoyage moteur, conservé dans une table bornée et publié par keyframe immuable ; plusieurs terminaux du même épisode sont coalescés selon une précédence fermée. |
| `P2-REQ-031` | Le joueur DOIT posséder le `CARGO_SCAN_STATE` minimal lu depuis l’autorité gameplay ; chaque vaisseau exporté DOIT posséder `DOCKING_STATE`. La fermeture player/support/docking est transitive, filtrée `Cockpit` avant diff et bornée à 64 vaisseaux. Une cible cargo extérieure publie `NOT_SCANNABLE/HIDDEN`, sans groupes optionnels ni fin de session. |
| `P2-REQ-032` | Ratios, ETA, progressions, vitesses scalaires, angles d’affichage et coordonnées HUD DOIVENT être calculés côté client et absents du fil. |
| `P2-REQ-033` | Tout flottant publié DOIT être fini et canonisé pour `-0`. Seules les quantités courantes finies de HP, boucliers, énergie et carburant PEUVENT être clampées dans `[0,max]`, avec compteur de normalisation séparé ; maximum invalide/non fini ou structure impossible est rejeté. Tout timer moteur est converti en durée microseconde bornée. |

### 5.5 Cycle de vie, snapshot et delta

| ID | Exigence normative |
|---|---|
| `P2-REQ-034` | Une apparition DOIT allouer un `entity_id` monotone de session ; un respawn, même du même joueur, DOIT allouer un nouvel ID. |
| `P2-REQ-035` | Mort, disparition et respawn DOIVENT être reflétés par `ENTITY_LIFECYCLE`, l’état cumulatif et un `EVENT_BATCH Reliable` dérivé des transitions observées. Tout événement référencé DOIT rester sous une fence de dépendance jusqu’à la baseline fiable qui rend son entité connaissable ; un changement d’`observed_player_entity_id` exige une keyframe. |
| `P2-REQ-036` | Le snapshot initial et chaque keyframe DOIVENT être exhaustifs pour le profil et committés atomiquement seulement après validation et `ACK APPLIED`. |
| `P2-REQ-037` | Chaque delta DOIT être cumulatif contre le dernier snapshot immuable acquitté ; il NE DOIT dépendre d’aucun delta précédent. Si son payload dépasse 1 Mio, il est remplacé par une keyframe exhaustive ; aucune troncature n’est permise. |
| `P2-REQ-038` | L’unité de remplacement est l’atome FSTL complet ; listes de segments, banques, tourelles, animations et relations NE DOIVENT jamais être partielles. |
| `P2-REQ-039` | Une suppression DOIT utiliser `DELETE` seulement pour les records qui l’autorisent ; les autres absences suivent leur sémantique de record et sont réparées par keyframe. |
| `P2-REQ-040` | Après `APPLIED` du dernier manifeste requis et la perte volontaire d’un unique delta, le client DOIT converger sur le delta cumulatif ou la keyframe suivante, sans dépendre d’un delta intermédiaire ni d’une campagne probabiliste. |

### 5.6 Configuration, observabilité et performance

| ID | Exigence normative |
|---|---|
| `P2-REQ-041` | Le schéma fermé conserve `systemsHz` entier `1..20`, défaut `10`, et `flightHz` entier `1..60`, défaut `30`. `schemaVersion=2` exige `phase2Profile` exactement `CoreGate` ou `CompleteShip`; la v1 interdit ce champ et migre explicitement vers `CompleteShip`. Le profil est choisi une fois avant bind et ne change qu’après redémarrage. |
| `P2-REQ-042` | Les limites héritées de 1200 octets/datagramme, 1 Mio/message delta, 16 Mio/transaction fiable, 64 parts, 1024 fragments, 65 535 records par message/part et 32 Mio de candidates/client DOIVENT être appliquées avant allocation et avant cast. L’image contient au plus `9+N=1033` records en `CoreGate` et `4+10K+N=4740` en `CompleteShip`; aucune image artificielle de 65 535 entrées n’est exigée. |
| `P2-REQ-043` | Les métriques DOIVENT distinguer collecte par bloc, construction manifeste, image, diff, sérialisation, baseline, événements, rejets, tailles, retransmissions et âge des données. |
| `P2-REQ-044` | Les logs DOIVENT identifier profil, manifeste, cause de refus, transition lifecycle et resync sans exposer de secret, adresse non nécessaire ou contenu caché. |
| `P2-REQ-045` | Désactivé, le module reste inerte. Activé, le travail ajouté DOIT rester déterministe, non bloquant et borné ; aucun seuil temporel ni campagne de performance ne conditionne la livraison. |
| `P2-REQ-046` | La mémoire inclusive respecte simultanément 134 217 728 octets partagés, 83 886 080 octets par client et 402 653 184 octets process pour quatre clients, avec les sous-quotas hérités. Une croissance après `Ready` ferme proprement la session concernée sans réallocation non bornée. |
| `P2-REQ-047` | Un décodeur indépendant et un tableau de bord de référence rapprochent chaque valeur `A/C` de la source du même tick et chaque valeur `D` de sa formule documentée. |
| `P2-REQ-048` | Sérialisation, cardinalités, lifecycle, manifestes, baseline, récupération et ressources exposent les octets, IDs et transitions exacts nécessaires au relevé produit court. |
| `P2-REQ-049` | Les nouveaux fichiers du produit sont déclarés explicitement dans `code/source_groups.cmake`, sans glob implicite. |
| `P2-REQ-050` | Tout échec de source, budget, manifeste ou validation DOIT être observable et fail-closed ; aucune troncature ou dégradation silencieuse de couverture n’est admise. |

## 6. Exclusions et propriété des phases suivantes

| Domaine exclu | Propriétaire | Règle Phase 2 |
|---|---|---|
| cible, locks, lead, radar, contacts, AWACS, stealth, menaces, navigation | Phase 3 | aucun record 15–19 ou 23, aucune cible de tourelle publiée |
| UI cargo/scan | Phase 3 | le record 20 n’est présent que pour fermer le domaine ; aucune vue applicative anticipée |
| toutes les entités, projectiles, docking global | Phase 4 | seuls le joueur et le point fixe autorisé support/docking/leader sont matérialisés |
| client applicatif, `ReplicaStore`, UI | Phase 5 | seulement un client et tableau de bord de référence |
| worker/SPSC, hooks exhaustifs des événements brefs, communication | Phase 6 | couverture exacte à zéro, aucun `COMM_*` |
| rendu/encodage vidéo de cible | Phase 7 | aucune dépendance FFmpeg, render target ou H.264 |

## 7. Invariants transversaux

1. Le format public est indépendant de l’ABI du moteur.
2. Filtrage et autorisation précèdent copie publique, diff et sérialisation.
3. Les manifestes précèdent tout état qui les référence.
4. Une baseline de snapshot n’est jamais mutée après envoi.
5. Une session ne survit pas à une modification de son bitmap de couverture.
6. Les identités publiques sont monotones ou stables dans leur scope ; les indices locaux ne quittent jamais l’adaptateur.
7. Un champ optionnel absent signifie exactement l’absence définie par le schéma, jamais zéro par commodité.
8. La livraison exige la totalité des `P2-REQ-001` à `P2-REQ-050`, les preuves courtes de la table de traçabilité et une décision humaine ; aucun critère de certification séparé ne s'ajoute.
