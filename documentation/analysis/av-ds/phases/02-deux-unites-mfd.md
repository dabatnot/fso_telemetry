# Phase 2 — Deux unités MFD

## Livrable

AV DS affiche simultanément la page `RADAR` dans deux fenêtres maximisées sans
bordure, `MFD-L` et `MFD-R`, affectées à deux écrans Windows.

Les fenêtres partagent une seule session FSTL et une seule réplique validée de
l'état cockpit. Elles possèdent néanmoins deux états de présentation distincts.

## Inclus

- unités `MFD-L` et `MFD-R` explicites ;
- une fenêtre sans bordure par unité ;
- sélection séparée des deux écrans physiques ;
- une connexion FSTL commune ;
- un modèle Radar partagé en lecture seule ;
- une page active et un état local distincts par unité ;
- signalement d'un écran absent sans condamner l'autre unité ;
- mode de développement permettant de distinguer et manipuler les deux unités
  sans Cougar, par exemple au clavier ou à la souris.

La configuration ne repose pas uniquement sur le numéro d'écran Windows. Elle
doit pouvoir retrouver ou faire reconfigurer l'affectation après un changement
d'énumération.

## Non inclus

- lecture des MFD Cougar ;
- libellés OSB définitifs ;
- filtres de contacts différents ;
- commandes vers FS2Open ;
- page `COM` ;
- Central Pedestal ;
- toute plateforme autre que Windows.

## Vérification directe

1. AV DS ouvre une fenêtre sans bordure sur chacun des deux écrans choisis.
2. Les deux fenêtres affichent le même état Radar reçu pendant une mission.
3. Une seule connexion cliente apparaît côté producteur FSTL.
4. Une action de présentation sur `MFD-L` ne change pas l'état local de
   `MFD-R`, et inversement.
5. La perte ou la mauvaise affectation d'un écran n'empêche pas l'autre fenêtre
   de recevoir et d'afficher le Radar.
6. Une reconnexion FSTL met à jour les deux fenêtres depuis la même nouvelle
   réplique.

La phase est testable avec deux moniteurs Windows, sans MFD Cougar.
