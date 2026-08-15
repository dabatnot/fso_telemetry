# Phase 1 — Squelette et premier flux

## 1. Objet

Cette phase livre le premier producteur de télémétrie intégré à FS2Open, un décodeur indépendant et un client console capable d'observer le joueur en mission solo.

La spec décrit le produit attendu et ses niveaux de qualité observables.

## 2. Résultat produit attendu

Avec une configuration valide et `enabled=true` :

- le module démarre sans modifier la simulation ;
- un client loopback IPv4 ou IPv6 négocie FSTL 1.1 ;
- le producteur publie session, mission, lifecycle du joueur et cinématique ;
- le client installe un snapshot atomique puis maintient son état avec des deltas cumulatifs ;
- une keyframe répare un état incomplet ou ancien ;
- le client affiche clairement `Synchronizing`, `Live` ou `Stale` ;
- sortie de mission, retour menu, arrêt et redémarrage libèrent les ressources et renouvellent les identités appropriées.

Avec une configuration absente ou invalide, le module reste désactivé et le jeu continue normalement.

## 3. Documents actifs

| Document | Contenu produit |
|---|---|
| [01](01-cadre-normatif-et-perimetre.md) | périmètre et exigences |
| [02](02-architecture-contrats-et-interfaces.md) | architecture et interfaces |
| [03](03-flux-cycle-de-vie-et-concurrence.md) | runtime, session, cadence et cycle de vie |
| [04](04-modele-de-donnees-et-regles-metier.md) | profil `PLAYER_KINEMATICS` et sources moteur |
| [05](05-integration-configuration-et-observabilite.md) | configuration, intégration et métriques |
| [06](06-validation-securite-et-conformite.md) | sécurité et critères de qualité |
| [07](07-livraison-et-tracabilite.md) | résumé et tests utiles |

La Phase 0 reste normative pour le wire. La Phase 1 ajoute uniquement le domaine `PLAYER_KINEMATICS` de FSTL 1.1.

## 4. Qualité attendue

| Propriété | Niveau attendu |
|---|---|
| cadence de vol | `flightHz` configurable de 1 à 60 Hz, 30 Hz par défaut |
| keyframe | intervalle configurable de 1 à 5 s, 2 s par défaut |
| fraîcheur nominale | le client reçoit des mises à jour au rythme configuré lorsque l'état change et que le réseau local les transporte |
| récupération | après la perte d'un delta, convergence sur le delta cumulatif ou la keyframe suivante |
| coût actif ordinaire | travail court, déterministe, borné et non bloquant |
| coût désactivé | aucun socket, aucune allocation persistante et aucun log récurrent |
| ressources | files, réassemblages, candidates et historique restent dans les bornes configurées |
| intégrité | aucune donnée d'une ancienne mission ou session n'apparaît dans la nouvelle |

Ces propriétés sont vérifiées par des tests courts et une observation produit. Aucun percentile ni campagne longue ne conditionne la livraison. Le relevé des résultats et des écarts est soumis à l'humain pour décision.

## 5. Frontière de phase

Cette phase ne publie ni état complet du vaisseau, ni armes, ni support, ni ciblage, ni radar, ni communication visuelle, ni vidéo de cible.
