# Phase 2 — Vaisseau complet

## 1. Objet

Cette phase étend le premier flux pour permettre à un client distant de reconstruire le tableau de bord complet du vaisseau joueur : identité, mouvement, commandes, coque, boucliers, sous-systèmes, énergie, propulsion, armement, cargo, docking, support et cycle de vie.

La spec décrit le produit et ses niveaux de qualité observables.

## 2. Résultat produit attendu

Pendant une mission solo en mode `Cockpit` :

- le client installe les catalogues de classes et d'armes nécessaires ;
- il reçoit un snapshot complet et cohérent du joueur et des vaisseaux strictement nécessaires aux relations de support ou docking ;
- chaque valeur affichée provient d'une source moteur documentée ou d'une formule client documentée ;
- les deltas maintiennent l'image sans dépendre des deltas précédents ;
- apparition, mort, disparition et respawn produisent les identités et transitions attendues ;
- une perturbation réseau prise en charge ne corrompt pas l'état et une keyframe permet la convergence ;
- la télémétrie reste en lecture seule et n'expose aucune information appartenant aux phases de ciblage et de capteurs.

## 3. Profils livrés

| Profil | Couverture | Usage produit |
|---|---:|---|
| cœur (`CoreGate` dans les identifiants techniques existants) | `0x0401` | cinématique et état structurel du joueur |
| `CompleteShip` | `0x0583` | tableau de bord complet et relations nécessaires |

Le profil final est `PLAYER_KINEMATICS | CORE_SHIP | CONTROL_INPUTS | WEAPONS | CARGO_DOCK_SUPPORT`. La couverture est négociée avant la session et reste stable pendant celle-ci.

## 4. Documents actifs

| Document | Contenu produit |
|---|---|
| [01](01-cadre-normatif-et-perimetre.md) | périmètre et exigences |
| [02](02-architecture-contrats-et-interfaces.md) | capture, DTO, catalogues et interfaces |
| [03](03-flux-cycle-de-vie-et-concurrence.md) | runtime, profils, lifecycle, support et concurrence |
| [04](04-modele-de-donnees-et-regles-metier.md) | records et règles métier du vaisseau |
| [05](05-integration-configuration-et-observabilite.md) | configuration, ressources, métriques et performance |
| [06](06-validation-securite-et-conformite.md) | sécurité et critères de qualité |
| [07](07-livraison-et-tracabilite.md) | résumé et tests utiles |

## 5. Qualité attendue

| Propriété | Niveau attendu |
|---|---|
| cadence mouvement/commandes | `flightHz` de 1 à 60 Hz, 30 Hz par défaut |
| cadence systèmes | `systemsHz` de 1 à 20 Hz, 10 Hz par défaut |
| récupération | après la perte d'un delta, convergence sur le delta cumulatif ou la keyframe suivante |
| coût d'un tick systèmes | travail déterministe, borné et non bloquant |
| cohérence | zéro référence d'entité, classe ou arme non résolue dans une image acceptée |
| exhaustivité | chaque record et chaque collection requis par le profil est présent |
| ressources | files, images, catalogues et transactions restent dans les plafonds du document 05 |
| isolation | aucune commande distante et aucune modification de la simulation |

Les valeurs observées et les écarts sont présentés à l'humain, qui décide de leur acceptabilité. Aucun percentile, environnement matériel de référence ou campagne longue ne conditionne la livraison.

## 6. Frontière de phase

Ciblage, radar, menaces, navigation, vue de communication et vidéo de cible restent hors de cette phase.
