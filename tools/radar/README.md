# Radar SimPit

Client radar autonome pour FS2Open. La V1 reproduit le scope radar du dashboard
en se connectant directement au producteur UDP FSTL 1.1 avec le profil fermé
`CockpitSensors` (`0x07CB`). Le programme est strictement passif : il ne change
ni la cible ni l'état du jeu.

## Construction

Prérequis : CMake 3.22+, un compilateur C++20 et Qt 6.2+ avec Core, Gui,
Widgets, Network, Svg et Test. LinguistTools n'est pas requis.

```sh
cmake -S tools/radar -B build/radar -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/radar
ctest --test-dir build/radar --output-on-failure
```

Sous Windows avec Qt installé hors du `PATH`, ajouter par exemple
`-DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64`.

## Utilisation

Au premier lancement, saisir l'hôte et le port du producteur FS2Open. Les
valeurs par défaut sont `127.0.0.1:42042`. `Échap` ou `Ctrl+,` rouvre les
réglages. Hôte, port et géométrie de fenêtre sont mémorisés par `QSettings`.
L'interface distribuée est exclusivement en anglais, comme celle du jeu.

Le fichier [`examples/fs2open.telemetry.json`](examples/fs2open.telemetry.json)
active le profil requis et autorise deux clients simultanés afin de comparer
le dashboard et ce radar.

Après la première image appliquée, une seconde sans progrès conserve la dernière
image, l'assombrit et la marque `SENSOR FEED LOST`, puis demande une
resynchronisation. Sans reprise pendant la seconde suivante, le client crée un
nouvel endpoint UDP et renégocie une session, sans effacer la dernière image.
Pendant la synchronisation initiale, chaque nouveau fragment de manifeste ou de
snapshot prolonge la transaction et la fenêtre fiable complète de cinq secondes
est conservée afin de ne pas abandonner une session saine en plein transfert.

Les overlays système emploient les fontes bitmap VFNT historiques de FreeSpace :
`font02.vf` pour le titre et `font01.vf` pour le détail. Ils sont colorés selon
l'état du lien (cyan, ambre ou rouge), tandis que le Target Callout et les
réglages conservent leurs fontes Qt.

Les 44 SVG de `assets` sont embarqués dans l'exécutable. Le client choisit les
familles de vaisseaux et d'armes uniquement à partir des informations révélées
par FSTL, applique la couleur IFF autoritaire et conserve les anciens glyphes
géométriques comme secours si une ressource ne peut pas être rendue. Les icônes
restent orientées vers le haut ; `weapon-mine.svg` est embarqué mais réservé à
une future sémantique protocolaire.

## Livraison

- `packaging/deploy-windows.ps1` produit un dossier redistribuable puis un ZIP
  à l'aide de `windeployqt`.
- `packaging/deploy-linux.sh` produit un AppDir puis une AppImage x86_64 à
  l'aide de `linuxdeploy` et de son plugin Qt.

Les bundles dynamiques incluent Qt SVG, les notices du dépôt et les licences Qt
trouvées dans l'installation utilisée pour le déploiement.
