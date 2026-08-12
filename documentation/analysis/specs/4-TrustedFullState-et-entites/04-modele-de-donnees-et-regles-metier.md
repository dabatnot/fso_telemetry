# 04 — Modèle de données et règles métier

Le graphe Phase 4 a pour nœuds les `ENTITY_LIFECYCLE` exportés. Les arêtes sont les parents stables, les relations docking et les références de cible autorisées par les records existants. Toute arête doit résoudre dans la même image; un graphe parent contient zéro cycle.

| Type | Représentation minimale |
|---|---|
| `SHIP` | lifecycle, identité et états vaisseau hérités, relations autorisées |
| `WEAPON` | lifecycle, `WEAPON_MANIFEST` installé et `FLIGHT_STATE` obligatoire |
| `ASTEROID`, `DEBRIS`, `JUMP_NODE`, `WAYPOINT`, `FIREBALL`, `OTHER` | lifecycle, `FLIGHT_STATE` obligatoire et seuls champs publics du layout FSTL |

Les présences `signature`, `net_signature`, nom non-vaisseau, équipe/IFF ou rayon suivent exactement les bits d’`ENTITY_LIFECYCLE`; elles ne sont jamais inférées d’une longueur de payload. Une bombe reste `WEAPON`. Seuls `SHIP` et `WEAPON` portent `class_id`, respectivement avec `CLASS_MANIFEST` et `WEAPON_MANIFEST`; tout autre type omet ce champ.

`FLIGHT_STATE` est la pose commune Phase 4. Une entité immobile porte vitesse nulle et quaternion identité; les champs de vol non applicables suivent le layout existant et ne deviennent pas une simulation client.

Une relation de docking est publiée dans les deux directions exactes. Au-delà de 64 relations, le profil est refusé avant `WELCOME`; après session, l’image n’est pas publiée et la session se ferme avec une cause fermée. Toute référence de classe, arme, sous-système ou parent doit être installée ou résolue avant commit. Valeur non finie, ID zéro, doublon, enum/bit inconnu, temps décroissant et cardinalité hors borne font échouer l’image concernée avant publication.
