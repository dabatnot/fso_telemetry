# 01 — Cadre normatif et périmètre

## 1. Objet et statut

Ce document spécifie le périmètre normatif de la **Phase 1 — Squelette et premier flux**. Il transforme la [Phase 1 de la feuille de route](../../04-implementation-roadmap.md#3-phase-1--squelette-et-premier-flux) en exigences vérifiables sans prétendre que le code, les tests ou les mesures existent déjà.

Les mots **DOIT**, **NE DOIT PAS**, **DEVRAIT**, **NE DEVRAIT PAS** et **PEUT** ont le sens normatif défini par le [contrat Phase 0](../0-Contrat-de-protocole/README.md). En cas d'écart avec une analyse, la Phase 0 reste normative pour FSTL 1.0 et les décisions FSTL 1.1 enregistrées dans [07](07-livraison-et-tracabilite.md#6-décisions-de-clarification) portent seules les extensions de cette phase.

La livraison de ces documents ne ferme aucune gate d'implémentation. Toute preuve citée ci-dessous reste **à produire** jusqu'à son archivage selon [07](07-livraison-et-tracabilite.md#11-preuves-à-produire).

## 2. Résultat observable attendu

À la sortie d'implémentation de la Phase 1, un client console indépendant DOIT pouvoir, sur une mission solo :

1. joindre un producteur explicitement configuré sur IPv4 ou IPv6 ;
2. négocier FSTL 1.1 sans modifier les octets FSTL 1.0 ;
3. recevoir un snapshot atomique du joueur observé ;
4. afficher temps, identité stable, position, quaternion, vitesse linéaire et vitesse angulaire ;
5. maintenir cet état avec des deltas cumulatifs contre une baseline acquittée par `ACK APPLIED` ;
6. renouveler cette baseline et demander une resynchronisation ;
7. rester borné et non bloquant sous perte, duplication, désordre, client lent et `WOULD_BLOCK` ;
8. fonctionner trente minutes, puis libérer et recréer proprement toutes ses ressources lors d'un arrêt et d'une relance.

Le client demeure en lecture seule. Ce résultat ne constitue ni le vaisseau complet de Phase 2, ni le client distant applicatif de Phase 5.

## 3. Acteurs, autorité et frontière de confiance

| Acteur | Autorité et responsabilité Phase 1 |
|---|---|
| moteur FS2Open | source de vérité solo ; ses structures sont lues uniquement sur le thread principal |
| producteur `code/telemetry` | copie, valide, canonicalise, sérialise et publie la vue cinématique |
| transport UDP | transporte FSTL sur des sockets dédiés ; ne possède aucune autorité métier |
| pair réseau | source non fiable de `HELLO`, `ACK`, `NACK`, `HEARTBEAT` et `RESYNC_REQUEST` uniquement |
| décodeur indépendant | oracle d'interopérabilité sans réutilisation des structures C++ producteur |
| client console | consommateur de preuve, lecture seule, sans préjuger du client Phase 5 |

Tout datagramme entrant est hostile jusqu'à validation complète. Une allowlist n'authentifie pas cryptographiquement le pair. FSTL 1.1 reste destiné à un LAN explicitement autorisé ; une exposition Internet exige un tunnel authentifié externe.

## 4. Extension compatible FSTL 1.1

### 4.1 Motif

FSTL 1.0 impose `CORE_SHIP` à toute session d'état et rend alors obligatoires des records de coque, boucliers, sous-systèmes, énergie et propulsion réservés à la Phase 2. Un snapshot partiel est interdit. Anticiper `CORE_SHIP` violerait la feuille de route ; rester indéfiniment `Synchronizing` ne satisferait pas le premier flux.

La Phase 1 introduit donc une extension mineure FSTL 1.1. Elle NE DOIT PAS modifier le layout, la valeur ou l'interprétation d'un octet FSTL 1.0.

### 4.2 Domaine `PLAYER_KINEMATICS`

Le bit `StateDomainCoverage.PLAYER_KINEMATICS = 0x0000000000000400` est défini en FSTL 1.1. Il suit `NAVIGATION = 0x0200` et n'est valide que si la version négociée est au moins 1.1.

Dans un `FULL_SNAPSHOT` :

- `SESSION_STATE` et `MISSION_STATE` sont toujours présents ;
- si `SESSION_STATE.observed_player_entity_id` est présent, un unique `ENTITY_LIFECYCLE` de type `SHIP` et un unique `FLIGHT_STATE` portent exactement cet ID ;
- `FLIGHT_STATE.presence` vaut `0` et seuls ses champs obligatoires sont émis ;
- `required_manifest_id` vaut `0` ;
- `SHIP_IDENTITY`, `CLASS_MANIFEST` et tous les records `CORE_SHIP` additionnels sont absents, sauf si un autre domaine négocié les exige dans une phase ultérieure ;
- si aucun `Player_obj` valide n'existe, l'identifiant observé et les deux records joueur sont absents.

L'identité minimale du joueur est la relation entre `observed_player_entity_id`, `ENTITY_LIFECYCLE.entity_id` et `FLIGHT_STATE.entity_id`. Aucun nom, classe, callsign ou index moteur n'est inventé.

### 4.3 Compatibilité

Le producteur Phase 1 annonce uniquement FSTL 1.1. Un client limité à FSTL 1.0 reçoit un `WELCOME` portant `WelcomeStatus::UnsupportedVersion`, sans création de session durable. Le décodeur indépendant DOIT continuer à décoder tous les vectors FSTL 1.0 inchangés. Un pair NE DOIT jamais émettre `PLAYER_KINEMATICS` dans une session négociée 1.0.

## 5. Catalogue des exigences

### 5.1 Gate, périmètre et intégration

| ID | Exigence | Preuve minimale attendue |
|---|---|---|
| `P1-REQ-001` | La Phase 1 DOIT hériter de tout le contrat Phase 0 et conserver les vectors FSTL 1.0 octet-identiques. Seul `P1-WP-01` PEUT produire l'amendement 1.1 nécessaire à `G0-G` ; `P1-WP-02` et les lots suivants NE DOIVENT PAS commencer avant fermeture documentée de cette gate. | hashes des vectors, compte rendu des gates |
| `P1-REQ-002` | FSTL 1.1 DOIT être une extension additive de même majeure ; le profil Phase 1 exige la négociation exacte 1.1 et n'émet aucun élément 1.1 dans une session 1.0. | matrice de compatibilité et tests croisés |
| `P1-REQ-003` | Le périmètre exécutable se limite à un producteur solo, un décodeur indépendant et un client console de preuve. | revue d'arborescence et scénario nominal |
| `P1-REQ-004` | Aucun message entrant ni callback de télémétrie NE DOIT modifier la simulation ou ajouter de champ à `object`, `ship`, `physics_info` ou `ai_info`. | revue statique et tests négatifs |
| `P1-REQ-005` | Le seam du socle DOIT se limiter au groupe `Telemetry` dans `code/source_groups.cmake` et à l'include/appel `telemetry::initialize()` dans `freespace2/freespace.cpp`. | diff de portée et builds |

### 5.2 Configuration et démarrage

| ID | Exigence | Preuve minimale attendue |
|---|---|---|
| `P1-REQ-006` | Le module DOIT lire le JSON optionnel `data/config/telemetry.json` avec Jansson. Absence, erreur syntaxique, type, borne ou clé inconnue DOIVENT laisser le module désactivé avant ouverture d'un socket et produire au plus un diagnostic de démarrage. | tests de table de configuration |
| `P1-REQ-007` | Les défauts DOIVENT être `enabled=false`, loopback IPv4 et IPv6, port `42042`, découverte désactivée, `Cockpit`, `maxClients=1` et allowlist loopback. Toute écoute non-loopback exige une activation explicite et une allowlist non vide. | inspection de config effective et tests source |
| `P1-REQ-008` | La configuration DOIT borner `maxClients` à 1–4, `flightHz` à 1–60 (défaut 30), `keyframeSeconds` à 1–5 (défaut 2), les heartbeats à 200–5000 ms (défauts 500 ms en mission et 1000 ms hors mission), et `maxDatagramsPerTick` à 1–256 (défaut 64). Elle n'est relue qu'au prochain démarrage en Phase 1. | tests limites et redémarrage |
| `P1-REQ-009` | `producer_id` DOIT être un `u64` non nul persistant ; `session_id` DOIT être non nul, imprévisible et non réutilisé pendant le processus. Un échec d'entropie désactive le module. | fixture de profil et injection d'échec RNG |

### 5.3 Transport, sécurité et ressources

| ID | Exigence | Preuve minimale attendue |
|---|---|---|
| `P1-REQ-010` | Le transport DOIT posséder ses sockets UDP dédiés et privés, non bloquants, sans `psnet_send()`, `multi_io_send()` ni socket multijoueur. Il DOIT fournir IPv4 et IPv6 par socket dual-stack sûr ou par deux sockets dédiés. | intégration loopback v4/v6 et revue API |
| `P1-REQ-011` | Chaque tick traite au plus `maxDatagramsPerTick`. `WOULD_BLOCK` remplace ou abandonne l'état ancien sans attente ; aucune boucle ne dépend d'un backlog non borné. | harness `WOULD_BLOCK` et mesure de tick |
| `P1-REQ-012` | Le producteur et les outils DOIVENT respecter les constantes FSTL héritées : datagramme 1200 octets, en-tête 68, payload 1132, little-endian, CRC-32/ISO-HDLC, message d'état 1 Mio, 1024 fragments, quatre réassemblages et 4 Mio par client. | golden vectors, limites et fuzzing |
| `P1-REQ-013` | L'ordre de validation Phase 0, la liaison endpoint/session, l'allowlist, l'anti-amplification `3×` et les rate limits hérités DOIVENT précéder toute allocation proportionnelle ou mutation. | tests sécurité et compteurs de drops |
| `P1-REQ-014` | Par client, il existe exactement une baseline active, au plus une candidate, un dernier delta remplaçable par baseline et des fenêtres bornées. Le produit `maxClients × budgets` DOIT être contrôlé sans overflow au démarrage. | test de saturation et pic mémoire |

### 5.4 Cycle de vie et threading

| ID | Exigence | Preuve minimale attendue |
|---|---|---|
| `P1-REQ-015` | `telemetry::initialize()` DOIT être idempotent, enregistrer exactement une fois `EngineUpdate`, `EngineShutdown`, `GameMissionLoad`, `GameEnterState` et `GameLeaveState`, puis retourner. | test double initialisation et compteur callbacks |
| `P1-REQ-016` | Les globals moteur DOIVENT être lus uniquement sur le thread principal. Seules des copies sans pointeur, handle ou index local peuvent survivre au tick. | assertions de thread et revue DTO |
| `P1-REQ-017` | Chargement, entrée/sortie de mission, retour menu et shutdown DOIVENT renouveler ou purger mission, baselines, sessions, sockets et IDs selon leur portée ; aucun état d'une ancienne session ne survit. | tests de cycle de vie et détecteur de fuite |
| `P1-REQ-018` | Les machines producteur/client et les timeouts Phase 0 DOIVENT être respectés. Aucun manifeste, snapshot ou état lourd n'est envoyé avant `ACK APPLIED` de `WELCOME`. | transitions horodatées et tests timeout |
| `P1-REQ-019` | Le heartbeat NTP-style DOIT conserver au plus huit sondes et huit échantillons, sélectionner le RTT minimal, lisser l'offset par huit et distinguer horloge monotone et temps mission. | tests d'horloge, overflow et pause |

### 5.5 Données, snapshot, delta et resynchronisation

| ID | Exigence | Preuve minimale attendue |
|---|---|---|
| `P1-REQ-020` | FSTL 1.1 DOIT définir `PLAYER_KINEMATICS=0x0400`, valide uniquement en 1.1 et immuable après `SESSION_BEGIN`. Il NE DOIT PAS signifier `CORE_SHIP`. | schéma, tables et vectors 1.1 |
| `P1-REQ-021` | Un snapshot du domaine DOIT respecter exactement la cardinalité et le record-set de la section 4.2, avec `required_manifest_id=0`. | golden snapshot minimal et cas sans joueur |
| `P1-REQ-022` | Le `FLIGHT_STATE` Phase 1 DOIT avoir `presence=0` et contenir ses champs obligatoires réels : ID, temps, position, quaternion, vitesse monde, vitesse angulaire locale, rayon et flags physiques. | comparaison source/JSON canonique |
| `P1-REQ-023` | L'identité joueur DOIT être la relation stable entre l'ID observé et les records lifecycle/flight ; `SHIP_IDENTITY`, classe, nom et manifeste sont exclus. | test d'absence et unicité des IDs |
| `P1-REQ-024` | `Player`, `Player_obj` et `Player_ship` DOIVENT être validés avant lecture. Position, quaternion local-vers-monde `(w,x,y,z)`, vitesse monde et rotation locale `(pitch,yaw,roll)` suivent les unités Phase 0. Une source non finie ou hors borne ne peut être tronquée ni publiée comme valide. | oracle moteur et cas source invalide |
| `P1-REQ-025` | `entity_id` DOIT être un `u64` non nul, monotone dans la session et indépendant de `objnum`, `instance`, index et pointeurs. | test de réutilisation et changement session |
| `P1-REQ-026` | Un `FULL_SNAPSHOT` DOIT être fiable, transactionnel et atomique ; la baseline ne change qu'après tous les `ACK APPLIED`. Un doublon après ACK perdu est réacquitté sans republication. | harness ACK perdu et commit unique |
| `P1-REQ-027` | Un `DELTA` DOIT être cumulatif depuis la baseline immuable, remplaçable et non acquitté. Le dernier delta suffit ; baseline inconnue entraîne drop et resync limité. | scénario perte/désordre/delta récent |
| `P1-REQ-028` | Une keyframe candidate est créée toutes les 1–5 secondes, défaut 2. L'ancienne baseline reste servie jusqu'à `APPLIED`; les mutations pendant l'aller-retour apparaissent dans le premier delta de la nouvelle baseline. | scénario de course déterministe |
| `P1-REQ-029` | La Phase 1 NE DOIT PAS revendiquer d'`EVENT_BATCH` métier. Une discontinuité mission/joueur force une keyframe ou une nouvelle session ; mort, observer et respawn complets restent Phase 2. | test lifecycle et revue exclusion |
| `P1-REQ-030` | Une resynchronisation DOIT être dédupliquée, acquittée `VALIDATED`, produire une keyframe récente et ne conserver aucun historique non borné. | resync répété et mesure mémoire |

### 5.6 Observabilité, performance et outils

| ID | Exigence | Preuve minimale attendue |
|---|---|---|
| `P1-REQ-031` | Les métriques de session, transport, validation, fragmentation, ACK/NACK, retransmission, resync, baseline, drops, files, réassemblage, collecte, diff, sérialisation et allocations définies dans [05](05-integration-configuration-et-observabilite.md) DOIVENT être exposées avec unité et scope de reset. | test d'incrément et export de métriques |
| `P1-REQ-032` | Les logs DOIVENT être agrégés et ne contenir ni payload complet, dump par frame, chemin absolu, secret ni donnée cachée. | capture et scan des logs |
| `P1-REQ-033` | Désactivé : zéro socket, allocation, syscall et log récurrent ; benchmark de 100 000 callbacks avec moyenne ≤0,01 ms, p99 ≤0,05 ms et écart de frame médiane <1 %. Actif hors première keyframe : collecte, diff, sérialisation et réseau du tick p99 ≤0,25 ms. | rapport benchmark reproductible |
| `P1-REQ-034` | Un décodeur indépendant DOIT décoder les vectors 1.0 inchangés et 1.1, produire le JSON canonique, rejeter les mêmes invalides et participer au harness de contrôle. | rapport d'interopérabilité croisée |
| `P1-REQ-035` | Le client console DOIT afficher session, mission, joueur, temps, pose, vitesses, baseline et âge, exposer `Synchronizing/Live/Stale`, envoyer ACK/resync et s'arrêter proprement. | transcript déterministe du client |
| `P1-REQ-036` | Le groupe CMake et les tests DOIVENT compiler dans les variantes supportées sans dépendance externe nouvelle ; Jansson existant est réutilisé. | matrice de builds |
| `P1-REQ-037` | Une mission solo DOIT être observée trente minutes, arrêtée et relancée dans le même processus puis après relance du processus, sans blocage, fuite détectée ni croissance non bornée. | rapport soak/leak et nouveaux IDs |
| `P1-REQ-038` | Les tests reproductibles DOIVENT couvrir sérialisation, fuzz, IPv4/IPv6, pertes indépendante et en rafales 1/5/20 % pendant dix minutes par profil, duplication, désordre, jitter, coupure, fragments/ACK/delta perdus, `WOULD_BLOCK`, client lent et resync répété ; convergence `Live` ≤10 s après fin d'impairment. | seeds, traces et rapport d'intégration |

## 6. Exclusions et propriété des phases suivantes

| Phase | Éléments explicitement exclus de Phase 1 |
|---:|---|
| 2 | `CORE_SHIP`, `SHIP_IDENTITY`, catalogues de classes, coque, boucliers, énergie, ETS, propulsion, afterburner complet, contrôles, armes, sous-systèmes, support, mort/observer/respawn |
| 3 | cible, lead, locks, radar, contacts, menaces, cargo, navigation et validation multijoueur métier |
| 4 | vue de communication et client distant utilisable |
| 5 | Talking Head, bundles, `ReplicaStore`, client graphique, API thread-safe et ESP32 |
| 6 | hooks d'événements exacts, worker réseau/SPSC, quantification, compression et adaptation dynamique |
| 7 | rendu cible, readback GPU, H.264, FFmpeg et QoS vidéo |

La découverte réseau, le hot reload de configuration, l'authentification/chiffrement natifs, l'audio, le transfert de bundles et un historique de replay non borné sont également exclus.

## 7. Invariants transversaux

- Toute mémoire, file, fenêtre et boucle DOIT avoir une borne explicite.
- Aucune entrée réseau ne peut provoquer une attente du thread principal.
- Une valeur absente utilise uniquement les mécanismes Phase 0 ; jamais NaN, pointeur, index ou chaîne magique.
- Une erreur de source ne doit jamais devenir une valeur filaire plausible mais fausse.
- Le client ne publie jamais un état partiel comme `Live`.
- Les checklists de [07](07-livraison-et-tracabilite.md#12-checklist-de-sortie) restent non cochées tant que les preuves réelles n'existent pas.
