# Phase 1 — Squelette et premier flux

## 1. Statut et objet

Ce paquet est le contrat d'implémentation de la phase 1 de la feuille de route télémétrie. Il définit un producteur UDP FSTL minimal, désactivé par défaut, un premier flux cinématique du joueur, un décodeur indépendant et un client console de preuve.

La phase 1 dépend du contrat de [phase 0](../0-Contrat-de-protocole/README.md). Elle introduit l'amendement additif **FSTL 1.1** défini par `D0-019` : le bit `StateDomainCoverage.PLAYER_KINEMATICS = 0x0400`. FSTL 1.0 reste octet pour octet et sémantiquement inchangé. Un producteur minimal de phase 1 négocie exclusivement la mineure 1.1 et NE DOIT PAS annoncer `CORE_SHIP`.

Le présent paquet spécifie le travail ; il ne constitue pas une preuve que les gates de phase 0 ou de phase 1 sont fermées. L'implémentation de `P1-WP-02` et des lots suivants est bloquée tant que `P1-WP-01` et les gates contractuelles de phase 0 ne sont pas validés par des artefacts exécutables.

## 2. Résultat attendu

À la sortie de la phase :

- `telemetry::initialize()` est raccordé une seule fois au moteur ;
- le mode désactivé n'ouvre aucun socket et son callback est un garde rapide ;
- le mode actif expose un transport UDP dédié, non bloquant, IPv4/IPv6 et limité à une allowlist ;
- une session FSTL 1.1 publie `SESSION_STATE`, `MISSION_STATE` et, lorsqu'un joueur valide existe, son `ENTITY_LIFECYCLE` et son `FLIGHT_STATE` ;
- les deltas sont cumulatifs contre la dernière baseline `ACK APPLIED`, avec renouvellement périodique par keyframe ;
- un décodeur indépendant et un client console observent la mission sans partager les DTO ni le parseur C++ du producteur ;
- une mission solo distante de trente minutes, un retour menu, une nouvelle mission et un redémarrage du processus ne provoquent ni blocage, ni fuite, ni croissance non bornée.

## 3. Documents normatifs

1. [Cadre normatif et périmètre](01-cadre-normatif-et-perimetre.md) — dépendances, exigences `P1-REQ-*`, inclusions et exclusions.
2. [Architecture, contrats et interfaces](02-architecture-contrats-et-interfaces.md) — composants, API, seams moteur et ownership.
3. [Flux, cycle de vie et concurrence](03-flux-cycle-de-vie-et-concurrence.md) — machines d'état, ordre des événements, threading et réplication.
4. [Modèle de données et règles métier](04-modele-de-donnees-et-regles-metier.md) — profil `PLAYER_KINEMATICS`, sources, identités, canonicalisation et deltas.
5. [Intégration, configuration et observabilité](05-integration-configuration-et-observabilite.md) — JSON, CMake, sockets, budgets, logs et métriques.
6. [Validation, sécurité et conformité](06-validation-securite-et-conformite.md) — stratégie de tests, robustesse, performances et acceptation.
7. [Livraison et traçabilité](07-livraison-et-tracabilite.md) — décisions `D1-*`, lots `P1-WP-*`, critères `P1-AC-*`, matrices et gates.

Ces huit fichiers forment un seul contrat. Une règle locale NE DOIT PAS être interprétée en contradiction avec le contrat de phase 0 amendé pour FSTL 1.1. En cas d'ambiguïté, l'ordre de priorité est : décisions gelées de phase 0, exigences de ce paquet, puis analyses de contexte.

## 4. Frontière de phase

La phase 1 inclut uniquement le transport, la session, le heartbeat, la cinématique minimale du joueur, la réplication cumulative, l'observabilité nécessaire et les deux outils de preuve.

Elle exclut notamment `SHIP_IDENTITY`, les manifestes de classes, dommages, boucliers, sous-systèmes, énergie, propulsion, commandes, armes, ciblage, radar, communications, rendu de cible, `ReplicaStore`, client graphique, ESP32 et tout worker réseau. Ces exclusions sont des interdictions de revendication, pas des données à remplir avec des zéros.

## 5. Lecture rapide avant implémentation

Un implémenteur DOIT, dans cet ordre :

1. vérifier les gates Phase 0 et réaliser `P1-WP-01` sans modifier les vecteurs FSTL 1.0 ;
2. inventorier chaque `P1-REQ-*`, `D1-*` et `P1-AC-*` ;
3. suivre l'ordre de dépendance des lots dans le document 07 ;
4. conserver une preuve reproductible pour chaque critère ;
5. laisser toute gate non démontrée explicitement ouverte.

## 6. Conventions

Les termes **DOIT**, **NE DOIT PAS**, **DEVRAIT**, **PEUT**, **existant**, **proposé**, **wire**, **baseline active**, **candidate** et **APPLIED** ont le sens défini par le contrat de phase 0. Les entiers multi-octets sont little-endian ; les limites et validations wire restent celles de FSTL 1.x.

## 7. Sources

La spécification croise les huit sources du dossier `documentation/analysis` : [README](../../README.md), [inventaire](../../01-telemetry-data-inventory.md), [architecture](../../02-telemetry-architecture.md), [protocole UDP](../../03-udp-protocol.md), [roadmap](../../04-implementation-roadmap.md), [réseau et API](../../05-existing-network-and-api.md), [vue communication](../../06-communication-view.md) et [vue cible haute résolution](../../07-high-resolution-target-view.md). Elle consomme également les huit documents normatifs de la [phase 0](../0-Contrat-de-protocole/README.md). La matrice détaillée se trouve dans le [document 07](07-livraison-et-tracabilite.md#9-matrice-de-traçabilité-globale-des-sources).

## 8. Gate et critère de sortie

`P1-WP-01` est autorisé à réaliser l'amendement FSTL 1.1 et à fermer `G0-G`. Aucun lot `P1-WP-02` ou ultérieur ne peut commencer avant cette fermeture. La phase 1 n'est terminée que lorsque `G1-A` à `G1-G` et `P1-AC-001` à `P1-AC-020` sont démontrés par les preuves reproductibles du document 07, notamment le soak solo de trente minutes, les relances propres, la non-régression FSTL 1.0 et l'absence d'artefacts des phases suivantes.
