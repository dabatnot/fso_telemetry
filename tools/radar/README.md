# Radar SimPit

Client radar autonome pour FS2Open. La V1 reproduit le scope radar du dashboard
en se connectant directement au producteur UDP FSTL 1.1 avec le profil fermé
`CockpitSensors` (`0x07CB`). Le programme est strictement passif : il ne change
ni la cible ni l'état du jeu.

## Construction

Prérequis : CMake 3.22+, un compilateur C++20 et Qt 6.2+ avec Core, Gui,
Widgets, Network et Test. LinguistTools est utilisé lorsqu'il est disponible.

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

Le fichier [`examples/fs2open.telemetry.json`](examples/fs2open.telemetry.json)
active le profil requis et autorise deux clients simultanés afin de comparer
le dashboard et ce radar.

Après trois secondes sans progrès, la dernière image est conservée, assombrie
et marquée `STALE`, puis une resynchronisation est demandée. Après dix secondes,
le client crée un nouvel endpoint UDP et renégocie une session, sans effacer la
dernière image affichée.

## Livraison

- `packaging/deploy-windows.ps1` produit un dossier redistribuable puis un ZIP
  à l'aide de `windeployqt`.
- `packaging/deploy-linux.sh` produit un AppDir puis une AppImage x86_64 à
  l'aide de `linuxdeploy` et de son plugin Qt.

Les bundles dynamiques incluent les notices du dépôt et les licences Qt
trouvées dans l'installation utilisée pour le déploiement. Les SVG du dossier
`assets` ne sont ni liés ni chargés dans cette V1.

