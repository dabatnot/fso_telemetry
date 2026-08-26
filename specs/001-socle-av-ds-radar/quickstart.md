# Quickstart: Validation du socle AV DS et Radar unique

## Prérequis

- Windows avec Visual Studio 2022 Build Tools ou un compilateur C++20 compatible.
- CMake 3.22+ et Qt 6.2+ avec Core, Gui, Widgets, Network, Svg et Test.
- Un FS2Open construit avec le producteur FSTL existant.
- Une configuration producteur `CockpitSensors`, par exemple
  `tools/radar/examples/fs2open.telemetry.json`.

Le modèle attendu est décrit dans [data-model.md](data-model.md) et les surfaces
observables dans [contracts/av-ds-application.md](contracts/av-ds-application.md).

## Construire

Réutiliser le build existant si sa configuration correspond aux sources :

```powershell
cmake -S tools/radar -B build/radar -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64
cmake --build build/radar
```

Résultat attendu : `build/radar/bin/av-ds.exe` est produit et aucun exécutable Radar
autonome distinct n'est requis pour la livraison.

## Exécuter les tests automatisés

Avant le lancement, vérifier qu'aucun autre agent ne possède déjà un compilateur ou un
test en cours. Exécuter ensuite les suites en série :

```powershell
ctest --test-dir build/radar --output-on-failure -j 1
```

Résultats attendus :

- le catalogue contient uniquement `RADAR` et `MFD-L` la sélectionne au démarrage ;
- les messages sont rendus par l'unité et non par `RadarWidget` ;
- les états attente, pause, périmé et reconnexion gardent leurs libellés et couleurs ;
- le fixture déterministe `referenceRadarImage` conserve les identifiants autorisés,
  assets d'icônes, couleurs IFF, ordre des superpositions et états visuels attendus sans
  contact supplémentaire ;
- une image complète valide sans contact ne rend aucun contact, trail, marqueur de
  menace ou décoration de cible périmé ;
- le modèle refuse une couverture autre que `CockpitSensors` ;
- les tests Radar de rendu, modèle, manifeste et transport restent passants.

## Vérifier la compatibilité de configuration

1. Lancer l'ancien client avec une destination valide et fermer sa fenêtre afin de
   sauvegarder hôte, port et géométrie.
2. Lancer `build/radar/bin/av-ds.exe`.
3. Vérifier qu'AV DS tente directement la même destination sans demander une nouvelle
   saisie et restaure une géométrie exploitable.
4. Avec un profil utilisateur sans configuration, vérifier que le message de
   configuration apparaît au niveau AV DS et qu'aucun contact n'est présenté courant.

## Vérification manuelle en jeu

1. Lancer AV DS avant FS2Open. Vérifier une seule fenêtre maximisable intitulée
   `AV DS — AV Display System`, correspondant à `MFD-L`, avec le message d'attente.
2. Démarrer une mission. Vérifier que la page `RADAR` affiche les mêmes contacts
   autorisés, icônes et couches que le client Radar de référence, sans contact caché.
3. Utiliser une situation sans contact Radar autorisé. Vérifier que la page reste vide
   et ne conserve aucun contact, trail, marqueur de menace ou décoration de cible de
   l'état précédent.
4. Mettre la mission en pause. Vérifier que l'image ne simule aucun mouvement et que le
   message global de pause est visible.
5. Interrompre la télémétrie. Vérifier que l'image est signalée périmée, puis que le
   message de reconnexion apparaît sans présenter les contacts comme courants.
6. Rétablir la télémétrie. Vérifier la convergence sur le premier état complet valide
   sans redémarrer AV DS.
7. Changer de mission ou d'identité observée. Vérifier qu'aucun contact précédent ne
   subsiste dans le nouvel état courant.
8. Fermer AV DS pendant la mission, puis poursuivre sans lui. Vérifier que le HUD, les
   commandes et la jouabilité de FS2Open restent inchangés.

## Produire le bundle Windows

```powershell
& tools/radar/packaging/deploy-windows.ps1 -BuildDirectory build/radar -OutputDirectory build/av-ds-package -QtRoot C:/Qt/6.10.3/msvc2022_64
```

Résultat attendu : un ZIP AV DS autonome contenant `av-ds.exe`, les DLL Qt nécessaires
dont Qt SVG, et les notices de licence, sans `FsoSimpitRadar.exe` installable à côté.
