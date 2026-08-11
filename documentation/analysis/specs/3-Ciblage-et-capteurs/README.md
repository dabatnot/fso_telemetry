# Phase 3 — Ciblage et capteurs

## 1. Objet

Cette phase étend le profil `CompleteShip` afin qu’un client distant puisse
reconstruire la vue capteurs autorisée du cockpit : cible courante, sous-système
ciblé, lead, locks missile, radar, contacts, menaces, scan cargo, navigation et
destination d’autopilote.

Le contrat conserve la simulation en lecture seule et ne transforme pas la
télémétrie en vue omnisciente. En mode `COCKPIT`, un client distant ne reçoit
jamais plus d’information que la projection capteurs autorisée du joueur
observé.

## 2. Résultat produit attendu

Pendant une mission solo en mode `Cockpit`, un client conforme :

- installe les catalogues nécessaires avant l’état qui les référence ;
- reçoit le profil exact `CockpitSensors` sous le masque `0x07CB` ;
- reproduit la cible, sa force HUD de coque/bouclier total, le sous-système
  ciblé, le lead et les locks courants ;
- affiche exactement les pistes radar autorisées, y compris leur état visible
  ou distordu, sans révéler un objet caché ;
- reproduit la position des blips du radar standard depuis la position locale
  du contact et la distance de projection capturées atomiquement dans chaque
  `RADAR_CONTACTS` v2/v3/v4, affiche en v3/v4 l'identité HUD exacte des
  vaisseaux `VISIBLE` et, en v4, reprend la couleur et le type de blip résolus
  par FSO sans les déduire du manifeste ou de l'IFF ;
- expose l’état capteurs, AWACS et EMP, tandis que chaque piste porte son état
  `VISIBLE` ou `DISTORTED` ;
- reconstruit les missiles entrants et le niveau de menace dans les bornes
  FSTL, ainsi que les voyants HUD indépendants et l'avertissement textuel
  effectivement retenu par FSO ;
- suit la phase de scan et ne reçoit le texte cargo qu’après divulgation
  autorisée ;
- affiche uniquement les navpoints et waypoints autorisés, la destination
  courante et la décision d’autopilote ;
- converge après une perte de delta grâce au delta cumulatif ou à la keyframe
  suivante.

Les distances géométriques, angles, vitesses relatives, progressions, ETA et
coordonnées d’affichage restent calculés côté client. La décision de visibilité,
le lead complexe, le lock, la phase de scan et le niveau de menace restent
autoritaires côté producteur. La cadence des voyants est dérivée côté client
depuis l'état discret FSO ; aucune frame ou phase d'animation HUD n'est publiée.

## 3. Profil livré

| Profil | Couverture | Usage produit |
|---|---:|---|
| `CockpitSensors` | `0x07CB` | vaisseau complet, ciblage, capteurs, cargo et navigation du cockpit |

`0x07CB` vaut exactement :

```text
PLAYER_KINEMATICS
| CORE_SHIP
| CONTROL_INPUTS
| RADAR_SENSORS
| TARGETING
| WEAPONS
| CARGO_DOCK_SUPPORT
| NAVIGATION
```

Les profils `CoreGate` (`0x0401`) et `CompleteShip` (`0x0583`) restent
disponibles selon leur contrat d’origine. Ils ne deviennent pas implicitement
Phase 3 et aucune promotion n’est permise dans une session existante.

## 4. Documents actifs

| Document | Contenu produit |
|---|---|
| [01](01-cadre-normatif-et-perimetre.md) | périmètre, profil et catalogue des exigences |
| [02](02-architecture-contrats-et-interfaces.md) | collecteurs, filtrage, IDs et interfaces |
| [03](03-flux-cycle-de-vie-et-concurrence.md) | runtime, transitions, manifestes et réplication |
| [04](04-modele-de-donnees-et-regles-metier.md) | records, cardinalités et règles métier |
| [05](05-integration-configuration-et-observabilite.md) | configuration, ressources, métriques et logs |
| [06](06-validation-securite-et-conformite.md) | validation comportementale, sécurité et confidentialité |
| [07](07-livraison-et-tracabilite.md) | traçabilité et observations Release |

## 5. Qualité attendue

| Propriété | Niveau attendu |
|---|---|
| confidentialité | aucun objet, nom, classe, IFF, cargo ou navpoint non autorisé |
| fidélité radar | même ensemble de contacts autorisés que la projection HUD producteur |
| cohérence | IDs identiques entre cible, locks, contacts, menace, cargo et navigation lorsqu’ils désignent le même objet |
| exhaustivité | tous les records requis par `0x07CB` sont présents dans chaque snapshot |
| récupération | perte d’un delta corrigée par un delta cumulatif ou la keyframe suivante |
| ressources | cardinalités, transactions, mémoire et registres restent bornés |
| isolation | aucune commande distante et aucune modification de la simulation |

## 6. Frontière de phase

Cette phase ne livre pas :

- `TrustedFullState`, `ALL_ENTITIES` ou la réplication complète des objets ;
- le docking global hors fermeture joueur/support héritée ;
- la vue de communication et son bundle d’assets ;
- le client applicatif final et ses interfaces graphiques ;
- les hooks exhaustifs d’événements brefs ;
- la vue 3D de cible, le rendu hors écran ou la vidéo H.264 ;
- un worker réseau, une file SPSC ou une lecture concurrente des tables moteur.
