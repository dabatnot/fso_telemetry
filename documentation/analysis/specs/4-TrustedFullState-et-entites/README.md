# Phase 4 — TrustedFullState et entités

## Objet

La Phase 4 introduit `TrustedFullState`, une réplication explicite de toutes les entités exportables connues du producteur pour un client de confiance. Elle étend `CockpitSensors` sans modifier ses garanties de confidentialité ni les artefacts FSTL existants.

## Résultat produit attendu

Après une connexion au milieu d’une mission, un client autorisé reçoit les catalogues nécessaires, une image complète du graphe d’entités, puis des deltas cumulatifs. Il converge après une perte prise en charge sans conserver une relation, un parent ou une cible vers une entité retirée.

La phase couvre vaisseaux, armes, projectiles, astéroïdes, débris, jump nodes, waypoints, fireballs et objets `OTHER` exportables, leur cycle de vie, leurs relations stables et le docking global. Elle ne crée aucune commande distante, vue de communication, rendu vidéo ou hook d’événement exact.

## Profil

`TrustedFullState` négocie FSTL 1.1 avec `AuthorityMode.SOLO`, `VisibilityMode.TRUSTED_FULL_STATE` et `state_domain_coverage = 0x00000000000007DB`, soit `CockpitSensors (0x07CB) | ALL_ENTITIES (0x0010)`. Ces faits sont fixés avant `WELCOME`; tout changement impose une nouvelle session. Une demande trusted en multijoueur est refusée sans downgrade vers Cockpit.

## Documents

| Document | Contenu |
|---|---|
| [01](01-cadre-normatif-et-perimetre.md) | périmètre et exigences |
| [02](02-architecture-contrats-et-interfaces.md) | projection et interfaces |
| [03](03-flux-cycle-de-vie-et-concurrence.md) | join-in-progress et réplication |
| [04](04-modele-de-donnees-et-regles-metier.md) | graphe et relations |
| [05](05-integration-configuration-et-observabilite.md) | configuration et ressources |
| [06](06-validation-securite-et-conformite.md) | preuves courtes et sécurité |
| [07](07-livraison-et-tracabilite.md) | table canonique et Release |

## Frontière de phase

Les objets non exportables, les événements brefs exacts, le client final, Talking Head et la vidéo cible restent hors Phase 4.
