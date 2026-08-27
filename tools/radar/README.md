# AV DS — AV Display System

AV DS est l'application d'affichage cockpit Windows de FS2Open. Cette première
version fournit une unité `MFD-L` et sa page intégrée `RADAR`, connectées
directement au producteur UDP FSTL 1.1 avec le profil fermé `CockpitSensors`
(`0x07CB`). L'application est strictement passive : elle ne change ni la cible,
ni la portée, ni aucun autre état du jeu, et FS2Open reste autonome lorsqu'elle
est absente ou fermée.

## Construction

Prérequis : CMake 3.22+, un compilateur C++20 et Qt 6.2+ avec Core, Gui,
Widgets, Network, Svg et Test. LinguistTools n'est pas requis.

```sh
cmake -S tools/radar -B build/radar -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/radar --parallel 1
ctest --test-dir build/radar --output-on-failure -j 1
```

Sous Windows avec Qt installé hors du `PATH`, ajouter par exemple
`-DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64`. L'exécutable produit est
`build/radar/bin/av-ds.exe`.

## Utilisation

Au premier lancement, saisir l'hôte et le port du producteur FS2Open. Les
valeurs par défaut sont `127.0.0.1:42042`. `Échap` ou `Ctrl+,` rouvre les
réglages. AV DS réutilise volontairement le stockage `QSettings`
`FS2Open/FsoSimpitRadar` : les hôte, port, options Radar et géométrie d'une
installation Radar existante restent donc disponibles sans migration ni
ressaisie. L'interface distribuée est exclusivement en anglais, comme celle du
jeu.

Le fichier [`examples/fs2open.telemetry.json`](examples/fs2open.telemetry.json)
active le producteur `CockpitSensors` et autorise quatre slots afin d'utiliser
simultanément plusieurs consommateurs cockpit tout en gardant une marge de
redémarrage. AV DS se connecte directement à FS2Open et ne dépend pas d'AV CORE.

Au menu et au briefing, le handshake reste préchauffé et AV DS affiche
`SENSOR LINK READY · WAITING FOR MISSION`. Pendant une pause, les heartbeats
maintiennent la session, la dernière image reste visible avec l'indicateur
`MISSION PAUSED`, et la reprise applique un keyframe immédiat.

En dehors de ces états explicites, une seconde sans progrès conserve la dernière
image, l'assombrit et la marque `SENSOR FEED LOST`, puis demande une
resynchronisation. Sans reprise pendant la seconde suivante, le client conserve
son endpoint UDP, renouvelle son nonce et renégocie une session, sans effacer la
dernière image. Il retransmet ensuite ce même `HELLO` tant que le jeu reste
indisponible, sans accumuler de nouvelles générations de session. Le socket
n'est recréé qu'après une erreur réseau locale ou un changement de destination.
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

## Livraison Windows

- `packaging/deploy-windows.ps1` produit un dossier redistribuable puis un ZIP
  AV DS à l'aide de `windeployqt`. Le bundle contient `av-ds.exe` et ne livre
  pas `FsoSimpitRadar.exe` en parallèle.
- La transformation AV DS de cette phase est limitée à Windows. Sur les autres
  plateformes, la sortie existante reste volontairement nommée
  `FsoSimpitRadar` et `packaging/deploy-linux.sh` continue de produire l'ancien
  AppImage Radar ; cette compatibilité hors périmètre n'est pas une seconde
  application incluse dans le bundle Windows.

Les bundles dynamiques incluent Qt SVG, les notices du dépôt et les licences Qt
trouvées dans l'installation utilisée pour le déploiement.
