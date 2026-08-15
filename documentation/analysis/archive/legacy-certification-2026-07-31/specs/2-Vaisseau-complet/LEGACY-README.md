# Phase 2 — Vaisseau complet

## 1. Statut et objet

Ce dossier est le contrat d’implémentation normatif de la Phase 2 de la roadmap télémétrie. Il spécifie l’extension du producteur Phase 1 vers l’état complet du vaisseau joueur : identité, cycle de vie, coque, boucliers, commandes, énergie, propulsion, armement, sous-systèmes, tourelles et support.

La rédaction de ce dossier ne constitue ni l’implémentation ni la preuve de fermeture de la phase. Les critères et cases de sortie restent ouverts jusqu’à production des artefacts exigés par [07-livraison-et-tracabilite.md](07-livraison-et-tracabilite.md).

Les termes **DOIT**, **NE DOIT PAS**, **DEVRAIT** et **PEUT** sont normatifs. FSTL 1.0 reste gelé ; la Phase 2 consomme exclusivement les types, champs et registres déjà définis par FSTL 1.1 et par le contrat Phase 0.

## 2. Résultat attendu

À la sortie de la phase, un client de preuve indépendant DOIT pouvoir afficher un tableau de bord complet du vaisseau observé et rattacher chaque valeur :

- soit à une valeur autoritaire copiée du moteur au même `producer_sample_time_us` ;
- soit à une valeur dérivée recomputée depuis les seuls champs FSTL reçus ;
- soit à un état explicitement absent selon les bits de présence du schéma v1.

Le profil final de livraison est FSTL 1.1 avec `state_domain_coverage = 0x0583`, soit `PLAYER_KINEMATICS | CORE_SHIP | CONTROL_INPUTS | WEAPONS | CARGO_DOCK_SUPPORT`. Cette valeur résout une contrainte du contrat Phase 0 : `CONTROL_STATE`, `WEAPON_STATE` et `SUPPORT_STATE` sont invalides sans leurs domaines respectifs. La promotion intermédiaire demandée par la roadmap vers `PLAYER_KINEMATICS | CORE_SHIP = 0x0401` reste une gate distincte ; elle ne permet pas encore d’émettre ces trois records.

La fermeture `CARGO_DOCK_SUPPORT` est le moindre point fixe autorisé : joueur observé, support de chaque membre, leaders et composantes de docking atteints transitivement ; une cible cargo n’est incluse que si elle appartient déjà à cet ensemble. La présentation métier du scan reste Phase 3 et le docking global reste hors produit.

Le client DOIT retrouver l’état correct après perte, duplication et réordonnancement artificiels de datagrammes, sans chaîne de deltas et sans intervention manuelle.

## 3. Documents normatifs

| Document | Rôle |
|---|---|
| [01-cadre-normatif-et-perimetre.md](01-cadre-normatif-et-perimetre.md) | portée, exigences `P2-REQ-*`, profils et exclusions |
| [02-architecture-contrats-et-interfaces.md](02-architecture-contrats-et-interfaces.md) | composants, seams moteur, interfaces, possession et arborescence proposée |
| [03-flux-cycle-de-vie-et-concurrence.md](03-flux-cycle-de-vie-et-concurrence.md) | ordonnancement, sessions, manifestes, snapshots, deltas, lifecycle et concurrence |
| [04-modele-de-donnees-et-regles-metier.md](04-modele-de-donnees-et-regles-metier.md) | image canonique, mapping des records, identités, bornes et dérivations |
| [05-integration-configuration-et-observabilite.md](05-integration-configuration-et-observabilite.md) | CMake, configuration, quotas, métriques, logs et budgets de performance |
| [06-validation-securite-et-conformite.md](06-validation-securite-et-conformite.md) | tests, oracles, perte artificielle, sécurité et gate de conformité |
| [07-livraison-et-tracabilite.md](07-livraison-et-tracabilite.md) | lots `P2-WP-*`, décisions `D2-*`, acceptation, risques et traçabilité globale |

Le document 01 est l’index des exigences. En cas de divergence interne, les décisions explicites `D2-*` du document 07 prévalent sur une reformulation descriptive, sans pouvoir modifier le format FSTL hérité.

## 4. Frontière de phase

La Phase 2 inclut :

- l’installation atomique d’un manifeste contenant les ensembles exhaustifs de records `CLASS_MANIFEST` et `WEAPON_MANIFEST` pour la fermeture autorisée du joueur ; l’ensemble armes peut être vide si aucune arme n’est référencée ;
- les records imposés par les cinq domaines du profil final ;
- un unique joueur observé en mode `Cockpit`, plus la closure complète et bornée des vaisseaux de support/docking que ses références FSTL rendent obligatoires ;
- des snapshots exhaustifs, des deltas cumulatifs par atome contre une baseline immuable et des événements de lifecycle dérivés d’état ;
- l’apparition, la mort, la disparition et le respawn avec un nouvel `entity_id` ;
- un harness de preuve et un tableau de bord textuel, sans anticiper le client applicatif de Phase 5.

Toute référence ship non nulle est matérialisée par son lifecycle et son état complet `CORE_SHIP | WEAPONS`. Une cible de scan qui n’appartient pas déjà à la closure autorisée termine la session au lieu d’élargir silencieusement la visibilité.

La Phase 2 exclut :

- le ciblage, les locks, le radar, AWACS, la furtivité, les menaces et la navigation de Phase 3 ;
- l’export global de mission et le docking global ;
- `ReplicaStore`, l’UI et le packaging du client applicatif de Phase 5 ;
- le worker/SPSC et la couverture exacte des événements brefs de Phase 6 ;
- `COMM_*`, les bundles visuels, la vidéo H.264 et le rendu de cible des Phases 6 et 7 ;
- toute commande distante de simulation, toute dépendance au HUD, tout pointeur, index moteur ou layout ABI sur le fil.

## 5. Lecture rapide avant implémentation

L’implémenteur DOIT respecter cet ordre :

1. vérifier la fermeture réelle de la gate Phase 1 et les corpus FSTL 1.0/1.1 ;
2. implémenter les collecteurs, le DTO pré-ID et le calcul de fermeture sans publication réseau ;
3. construire et valider les deux catalogues dans une transaction manifeste unique, puis projeter l’image avec les IDs du slot ;
4. fermer d’abord le profil `0x0401`, puis le profil final `0x0583` dans une nouvelle session ;
5. étendre snapshots, deltas, lifecycle, support et observabilité ;
6. exécuter les preuves unitaires, golden, intégration moteur, perte artificielle, performance et endurance ;
7. ne cocher la checklist de sortie qu’avec des artefacts reproductibles.

Un profil incomplet NE DOIT PAS être annoncé. Une taille ou cardinalité source hors borne NE DOIT PAS être tronquée : la session Phase 2 est refusée ou terminée avec une raison observable.

## 6. Conventions

- Les chemins de code cités sont des seams proposés contre le dépôt inspecté le 20 juillet 2026 ; ils doivent être revérifiés à l’implémentation.
- Les nombres hexadécimaux et noms de registres utilisent les valeurs exactes du contrat Phase 0.
- Une « fermeture » est le moindre point fixe transitif de ships autorisés (joueur, supports, leaders et composantes docking), puis de leurs classes, armes, banques et sous-systèmes nécessaires.
- Un « atome » est la plus petite unité remplacée intégralement par le diff FSTL ; aucun sous-tableau partiel n’est autorisé en v1.
- Un « oracle moteur » est une copie de test capturée sur le thread principal au même point que le DTO public, jamais une lecture concurrente du moteur.

## 7. Sources

Les huit sources d’analyse de premier niveau ont été lues et sont tracées :

- [README.md](../../README.md) — objectifs, modes de visibilité et invariants globaux ;
- [01-telemetry-data-inventory.md](../../01-telemetry-data-inventory.md) — autorités moteur, données autoritaires, complexes, dérivées et événementielles ;
- [02-telemetry-architecture.md](../../02-telemetry-architecture.md) — capture main-thread, DTO, baselines, files et cadences ;
- [03-udp-protocol.md](../../03-udp-protocol.md) — transport, ACK, fragmentation, validation et dégradation ;
- [04-implementation-roadmap.md](../../04-implementation-roadmap.md) — livrables et critère de sortie de la Phase 2 ;
- [05-existing-network-and-api.md](../../05-existing-network-and-api.md) — précédents réseau et oracles existants ;
- [06-communication-view.md](../../06-communication-view.md) — frontière explicite de la vue communication ;
- [07-high-resolution-target-view.md](../../07-high-resolution-target-view.md) — frontière explicite de la vidéo de cible.

Le contrat hérite en outre des huit documents de [Phase 0](../0-Contrat-de-protocole/README.md) et des huit documents de [Phase 1](../1-Squelette-et-premier-flux/README.md).

## 8. Gate et critère de sortie

La Phase 2 est fermée uniquement si :

- la Phase 1 est prouvée conforme et aucun artefact FSTL 1.0 n’a changé ;
- le client installe la génération manifeste conjointe classes + armes avant le premier snapshot ;
- les profils `0x0401` et `0x0583` sont chacun acceptés uniquement lorsqu’ils sont complets ;
- chaque valeur autoritaire ou complexe du tableau de bord est comparée à son oracle et chaque valeur dérivée est recomputée ;
- après le scénario de perte normatif, un état source stabilisé converge au plus tard dans `2 × keyframeSeconds + 1 s`, soit 5 secondes avec le défaut de 2 secondes ;
- mort, disparition et respawn convergent, avec un nouvel `entity_id` et sans réutilisation d’une baseline antérieure ;
- les budgets, l’absence de blocage frame, la sécurité `Cockpit` et l’endurance sont prouvés ;
- toutes les cases de [la checklist finale](07-livraison-et-tracabilite.md#12-checklist-de-sortie) sont appuyées par des preuves datées et reproductibles.
