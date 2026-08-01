# 01 — Cadre normatif et périmètre

## 1. Objet

Ce document transforme la [Phase 1 de la feuille de route](../../04-implementation-roadmap.md#3-phase-1--squelette-et-premier-flux) en exigences produit vérifiables.

Les mots **DOIT**, **NE DOIT PAS**, **DEVRAIT**, **NE DEVRAIT PAS** et **PEUT** ont le sens normatif défini par le [contrat Phase 0](../0-Contrat-de-protocole/README.md). La Phase 0 reste normative pour FSTL 1.0.

## 2. Résultat observable attendu

Sur une mission solo, un client console indépendant peut :

1. joindre un producteur configuré sur IPv4 ou IPv6 ;
2. négocier FSTL 1.1 sans modifier FSTL 1.0 ;
3. recevoir un snapshot atomique du joueur observé ;
4. afficher temps, identité, position, quaternion, vitesse linéaire et vitesse angulaire ;
5. maintenir cet état avec des deltas cumulatifs ;
6. renouveler sa baseline et demander une resynchronisation ;
7. rester borné et non bloquant lorsque le réseau perd, duplique ou désordonne des datagrammes ;
8. libérer et recréer ses ressources lors d'un arrêt, d'une transition de mission ou d'une relance.

## 3. Acteurs et autorité

| Acteur | Responsabilité |
|---|---|
| moteur FS2Open | source de vérité solo, lue sur le thread principal |
| producteur `code/telemetry` | copie, valide, canonise, sérialise et publie |
| transport UDP | transporte FSTL sans autorité métier |
| pair réseau | source non fiable des messages de contrôle autorisés |
| décodeur indépendant | vérifie l'interopérabilité sans structures C++ du producteur |
| client console | présente l'état reçu en lecture seule |

FSTL 1.1 vise un LAN explicitement autorisé. Une exposition Internet emploie un tunnel authentifié externe.

## 4. Extension FSTL 1.1

`StateDomainCoverage.PLAYER_KINEMATICS = 0x0000000000000400` est défini en FSTL 1.1.

Dans un `FULL_SNAPSHOT` :

- `SESSION_STATE` et `MISSION_STATE` sont présents ;
- si un joueur est observé, un `ENTITY_LIFECYCLE` de type `SHIP` et un `FLIGHT_STATE` portent son ID ;
- `FLIGHT_STATE.presence` vaut `0` ;
- `required_manifest_id` vaut `0` ;
- les records Phase 2 restent absents ;
- sans joueur valide, l'identifiant observé et les records joueur sont absents.

Le producteur Phase 1 annonce FSTL 1.1. Un client limité à 1.0 reçoit `UnsupportedVersion` sans session durable. Tous les vectors FSTL 1.0 conservent leurs octets.

## 5. Exigences produit

### 5.1 Périmètre et intégration

| ID | Exigence |
|---|---|
| `P1-REQ-001` | La Phase 1 conserve le contrat Phase 0 et les vectors FSTL 1.0 octet-identiques. |
| `P1-REQ-002` | FSTL 1.1 est une extension additive ; aucun élément 1.1 n'est émis dans une session 1.0. |
| `P1-REQ-003` | Le produit livré comprend un producteur solo, un décodeur indépendant et un client console. |
| `P1-REQ-004` | Aucun message entrant ni callback de télémétrie ne modifie la simulation ou les structures métier existantes. |
| `P1-REQ-005` | L'intégration au socle se limite au groupe `Telemetry` et à l'initialisation du module. |

### 5.2 Configuration

| ID | Exigence |
|---|---|
| `P1-REQ-006` | `data/config/telemetry.json` est optionnel ; une configuration absente ou invalide laisse le module désactivé avant ouverture d'un socket. |
| `P1-REQ-007` | Les défauts sont `enabled=false`, loopback IPv4/IPv6, port `42042`, découverte désactivée, `Cockpit`, `maxClients=1` et allowlist loopback. |
| `P1-REQ-008` | `maxClients` vaut 1–4, `flightHz` 1–60, `keyframeSeconds` 1–5, heartbeat 200–5000 ms et `maxDatagramsPerTick` 1–256. |
| `P1-REQ-009` | `producer_id` est non nul et persistant ; `session_id` est non nul, imprévisible et non réutilisé dans le processus. |

### 5.3 Transport et ressources

| ID | Exigence |
|---|---|
| `P1-REQ-010` | Le transport utilise des sockets UDP privés et non bloquants, indépendants du multijoueur, avec IPv4 et IPv6. |
| `P1-REQ-011` | Chaque tick traite au plus `maxDatagramsPerTick` et ne boucle jamais sur un backlog non borné. |
| `P1-REQ-012` | Les limites FSTL héritées sont respectées : datagramme 1200 octets, en-tête 68, payload 1132, message d'état 1 Mio, 1024 fragments, quatre réassemblages et 4 Mio par client. |
| `P1-REQ-013` | Source, session, allowlist, anti-amplification et limites sont validées avant allocation proportionnelle ou mutation. |
| `P1-REQ-014` | Chaque client possède une baseline active, au plus une candidate, un dernier delta remplaçable et des fenêtres bornées. |

### 5.4 Cycle de vie

| ID | Exigence |
|---|---|
| `P1-REQ-015` | `telemetry::initialize()` est idempotent et enregistre une seule fois les callbacks requis. |
| `P1-REQ-016` | Les globals moteur sont lus sur le thread principal ; seules des copies possédées survivent au tick. |
| `P1-REQ-017` | Chargement, mission, menu et arrêt purgent ou renouvellent les ressources et identités selon leur portée. |
| `P1-REQ-018` | Aucun état lourd n'est envoyé avant `ACK APPLIED` de `WELCOME`. |
| `P1-REQ-019` | Le heartbeat conserve au plus huit sondes et échantillons et distingue horloge monotone et temps mission. |

### 5.5 Données et réplication

| ID | Exigence |
|---|---|
| `P1-REQ-020` | `PLAYER_KINEMATICS=0x0400` est valide uniquement en 1.1, reste stable pendant la session et ne signifie pas `CORE_SHIP`. |
| `P1-REQ-021` | Le snapshot respecte exactement le record-set de la section 4 avec `required_manifest_id=0`. |
| `P1-REQ-022` | `FLIGHT_STATE` publie ID, temps, position, quaternion, vitesse monde, vitesse angulaire locale, rayon et flags physiques. |
| `P1-REQ-023` | L'identité joueur est la relation stable entre `observed_player_entity_id` et les records lifecycle/flight. |
| `P1-REQ-024` | Les sources joueur sont validées avant lecture ; une valeur non finie ou hors borne n'est pas publiée comme valide. |
| `P1-REQ-025` | `entity_id` est non nul, monotone dans la session et indépendant des pointeurs et indices moteur. |
| `P1-REQ-026` | Un `FULL_SNAPSHOT` est fiable, transactionnel, atomique et idempotent après perte d'ACK. |
| `P1-REQ-027` | Un `DELTA` est cumulatif depuis la baseline immuable ; le dernier delta reçu suffit à converger. |
| `P1-REQ-028` | Une keyframe candidate est créée toutes les 1–5 s ; les mutations survenues pendant son acquittement figurent dans le premier delta suivant. |
| `P1-REQ-029` | Une discontinuité de mission ou de joueur produit une keyframe ou une nouvelle session. |
| `P1-REQ-030` | Une resynchronisation est dédupliquée, acquittée et produit une keyframe récente sans historique non borné. |

### 5.6 Observabilité et qualité

| ID | Exigence |
|---|---|
| `P1-REQ-031` | Les métriques de session, transport, validation, réplication, files et coût exposent leur unité et leur portée. |
| `P1-REQ-032` | Les logs sont agrégés et ne contiennent ni payload complet, chemin absolu, secret ni donnée cachée. |
| `P1-REQ-033` | Désactivé, le module n'ouvre aucun socket et ne produit aucun log récurrent ; actif, son travail reste court, déterministe, borné et non bloquant, sans seuil temporel de certification. |
| `P1-REQ-034` | Le décodeur indépendant lit FSTL 1.0 et 1.1, produit le JSON canonique et rejette les mêmes invalides. |
| `P1-REQ-035` | Le client console affiche session, mission, joueur, pose, vitesses, baseline, âge et état de synchronisation ; il envoie ACK/resync et s'arrête proprement. |
| `P1-REQ-036` | Le module compile dans les variantes supportées sans nouvelle dépendance externe ; Jansson est réutilisé. |
| `P1-REQ-037` | Une session représentative peut être arrêtée et relancée sans blocage ni croissance non bornée, avec renouvellement correct des identités. |
| `P1-REQ-038` | Après la perte volontaire d'un unique delta, le client converge sur le delta cumulatif ou la keyframe suivante sans intervention ni historique non borné. |

## 6. Frontière

Les états complets du vaisseau, ciblage, radar, réplication globale, communication visuelle, événements optimisés et vidéo de cible appartiennent aux phases suivantes.

## 7. Invariants

- Toute mémoire, file, fenêtre et boucle possède une borne explicite.
- Aucune entrée réseau n'attend le thread principal.
- Une valeur absente utilise les mécanismes FSTL prévus.
- Une erreur de source reste observable et ne devient pas une valeur plausible.
- Le client expose `Live` uniquement après installation d'un état complet.
