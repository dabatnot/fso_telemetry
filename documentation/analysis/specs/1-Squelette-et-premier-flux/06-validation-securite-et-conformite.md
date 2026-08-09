# 06 — Sécurité et qualité produit

## 1. Objet

Ce document rassemble les comportements mesurables qui déterminent la qualité du premier flux. Les détails d'architecture, de cycle de vie et de données restent définis dans les documents [02](02-architecture-contrats-et-interfaces.md), [03](03-flux-cycle-de-vie-et-concurrence.md), [04](04-modele-de-donnees-et-regles-metier.md) et [05](05-integration-configuration-et-observabilite.md).

## 2. Configuration et démarrage

| Situation | Comportement attendu |
|---|---|
| fichier absent | module désactivé, aucun socket |
| JSON invalide, clé inconnue ou valeur hors borne | module désactivé, un diagnostic de démarrage au plus |
| `enabled=false` | aucune session et aucun travail récurrent significatif |
| loopback par défaut | IPv4 et IPv6 disponibles |
| bind non-loopback | activation explicite et allowlist non vide |
| entropie indisponible | module désactivé sans identité faible |

## 3. Session et réplication

- `WELCOME` est acquitté avant tout état lourd.
- Le snapshot initial est installé atomiquement.
- Une seule baseline active et au plus une candidate existent par client.
- Un delta contient tous les changements depuis sa baseline et ne dépend pas d'un delta intermédiaire.
- Un doublon fiable est réacquitté sans double application.
- Une baseline inconnue conserve l'état courant et provoque une resynchronisation bornée.
- Une nouvelle mission ou un nouveau joueur observé produit une nouvelle keyframe ou une nouvelle session.

## 4. Données observées

Le client reçoit :

- `SESSION_STATE` et `MISSION_STATE` ;
- zéro ou un `ENTITY_LIFECYCLE` représentant le joueur observé ;
- zéro ou un `FLIGHT_STATE` portant le même `entity_id`.

Position, quaternion, vitesses, rayon et flags proviennent des sources moteur documentées. Les valeurs non finies ou hors borne ne sont pas publiées comme valides.

## 5. Sécurité et ressources

- Les sockets de télémétrie sont privés, UDP et non bloquants.
- Les messages entrants ne peuvent appeler aucune commande de simulation.
- L'allowlist, l'anti-amplification et les limites FSTL sont appliqués avant allocation proportionnelle.
- Un tick traite au plus `maxDatagramsPerTick`.
- `WOULD_BLOCK` abandonne ou remplace l'état périssable sans attente.
- Les logs ne contiennent ni payload complet, secret, chemin absolu ni donnée cachée.

## 6. Critères mesurables

| Critère | Niveau attendu |
|---|---|
| compatibilité FSTL 1.0 | golden vectors byte-identical |
| négociation Phase 1 | version 1.1 exacte pour `PLAYER_KINEMATICS` |
| cadence nominale | moyenne proche de `flightHz`, sans rafale destinée à rattraper le temps perdu |
| snapshot | publication complète ou aucune publication |
| deltas perdus | le delta cumulatif le plus récent permet la convergence |
| perturbation terminée | état `Live` retrouvé en 10 s au plus |
| saturation d'envoi | aucune attente du thread de jeu |
| arrêt | sockets, sessions, candidates et réassemblages libérés |
| client console | champs requis affichés, ACK et resync fonctionnels |
| isolation | zéro modification de la simulation |

Ces valeurs sont relevées dans une session représentative et par des observations courtes ciblées sur les transitions concernées.

## 7. Présentation des résultats

Pour chaque critère, le résultat indique la cible, la valeur observée, les conditions d'observation et l'impact d'un éventuel écart. L'acceptabilité de cet écart est décidée par l'humain.
