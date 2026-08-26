# Phase 4 — Réglages Radar indépendants

## Livrable

Le pilote configure en vol la présentation Radar de chaque MFD avec ses OSB.
Les deux pages Radar peuvent montrer le même état cockpit avec des filtres et
réglages locaux différents.

## Inclus

- état de réglage séparé pour `MFD-L` et `MFD-R` ;
- activation et désactivation des filtres par OSB ;
- indication visible des filtres actifs ;
- application des filtres après validation de l'état cockpit reçu ;
- conservation de la cible et des alertes nécessaires à la cohérence du rendu,
  selon les règles explicites de la maquette Radar ;
- réglages de présentation locaux déjà supportables par le renderer ;
- absence totale d'effet sur le HUD FS2Open et sur l'autre MFD.

Les groupes fonctionnels initiaux sont fondés uniquement sur le type de contact
déjà publié :

| Groupe | Types FSTL |
|---|---|
| `SHIP` | `Ship` |
| `WEAPON` | `Weapon` |
| `NAV` | `JumpNode`, `Waypoint` |
| `ENV` | `Asteroid`, `Debris`, `Fireball` |
| `OTHER` | `Unknown`, `Other` |

Tous les groupes sont visibles par défaut. Cette classification peut être
raffinée plus tard à partir d'informations cockpit déjà autorisées, mais aucun
filtre ne peut révéler un contact absent de la réplique reçue.

## Non inclus

- changement de portée du jeu ;
- modification du mode Radar de FS2Open ;
- ciblage depuis un MFD ;
- page `COM` ;
- commandes vers FS2Open ;
- toute plateforme autre que Windows.

## Vérification directe

1. `MFD-L` peut masquer `WEAPON` sans modifier `MFD-R`.
2. `MFD-R` peut afficher seulement `WEAPON` et `NAV` sans modifier `MFD-L`.
3. Deux configurations identiques produisent le même rendu à état reçu égal.
4. Réactiver un groupe restitue uniquement les contacts correspondants déjà
   présents dans la réplique courante.
5. Aucun filtre ne crée de contact, ne change sa classification et ne modifie
   FS2Open.
6. Les appuis sont réalisés depuis le Cougar affecté au MFD concerné.
7. Les réglages restent indépendants après perte et reprise de la session FSTL.

La phase livre déjà un usage opérationnel : le pilote peut consacrer deux
radars à des lectures différentes du même environnement autorisé.
