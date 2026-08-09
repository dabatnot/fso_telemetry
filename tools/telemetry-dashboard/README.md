# FSO Simpit Telemetry Lab

Dashboard local de validation des données FSTL destinées à un futur simpit.
L’interface ne modifie ni FS2Open ni le protocole : un bridge Python joue le
rôle d’un client FSTL normal et sert l’application web sur loopback.

## Lancement live

Activez la télémétrie dans la configuration chargée par FS2Open, démarrez le
jeu, puis exécutez depuis la racine du dépôt :

```powershell
.\tools\telemetry-dashboard\start-dashboard.ps1 `
  -TelemetryHost 127.0.0.1 `
  -TelemetryPort 42042 `
  -TelemetryConfig "D:\Games\GOG Galaxy\Games\Freespace 2\data\config\telemetry.json"
```

Le premier lancement installe les dépendances dans `build/telemetry-dashboard`,
construit l’interface et ouvre `http://127.0.0.1:43100`. Le bridge et le serveur
web n’écoutent que sur `127.0.0.1`.

## Captures et replay

Le menu **Données → Bibliothèque des captures** ouvre l’atelier de capture et
de replay. Les nouvelles captures sont des fichiers SQLite autonomes
`.fstlcap`, versionnés et compressés avec Zstandard. Les datagrammes validés
sont regroupés en blocs d’au plus une seconde ou 4 Mio, protégés par checksum,
et complétés par un checkpoint toutes les cinq secondes. Une interruption du
bridge laisse ainsi lisible tout bloc déjà validé.

La bibliothèque se trouve par défaut dans
`Documents/FSO Simpit Lab/Captures`. `-CaptureDirectory` permet de choisir un
autre emplacement. Les seuils par défaut sont un avertissement à 5 Gio, une
finalisation automatique à 10 Gio et une réserve de sécurité de 2 Gio ; les
paramètres `-CaptureWarningGiB`, `-CaptureStopGiB` et
`-CaptureFreeReserveGiB` les rendent configurables. L’interface affiche la
taille réelle, le débit glissant, les projections à dix minutes et une heure,
l’espace libre et, lorsqu’une durée prévue est fournie, la taille finale
estimée.

Les anciennes captures JSONL `FSTL-dashboard-capture-v1/v2` restent lisibles
en lecture seule. Leur import dans la bibliothèque effectue une conversion
explicite vers `.fstlcap` ; aucun format n’est deviné depuis la longueur des
données.

```powershell
.\tools\telemetry-dashboard\start-dashboard.ps1 `
  -Replay "$HOME\Documents\FSO Simpit Lab\Captures\session.fstlcap"
```

Le replay fonctionne uniquement à vitesse réelle. La timeline est exprimée en
microsecondes et permet lecture, pause et seek. Pendant un glissement, les
instruments sont reconstruits localement sans toucher aux sessions UDP ; le
seek réseau FSTL est validé au relâchement. Les
plages nommées possèdent un point d’entrée et de sortie, restent dans une seule
session source et peuvent être lues une fois ou en boucle. Chaque seek, retour
arrière, boucle ou frontière de session ferme les sessions UDP de replay avec
`SESSION_END(Restart)` avant une nouvelle négociation.

Le menu **Replay** peut démarrer un producteur UDP FSTL 1.1 sur loopback. Le
bind LAN doit être activé explicitement. Jusqu’à quatre clients cockpit sont
acceptés ; chacun reçoit son propre `session_id`, ses séquences, sa baseline,
sa fenêtre fiable, les heartbeats et un snapshot synthétique correspondant au
curseur courant. Les ACK, resync et retransmissions sont régénérés, sans
reproduire les pertes ou doublons réseau de la capture. Une pause conserve
l’horloge virtuelle figée et envoie des keyframes périodiques afin que les
clients restent `Live`.

Le menu **Données → Exporter** produit dans `build/telemetry-dashboard/exports` :

- le snapshot et les mesures de la session en JSON ;
- l’historique des échantillons par canal en CSV ;
- les instruments ND et interruptions observées en JSON.

Ces exports ne donnent aucun verdict d’aptitude au simpit.

## États des instruments

| État | Signification |
|---|---|
| `LIVE` | valeur autoritaire reçue et fraîche |
| `ND` | donnée prévue, mais source non produite actuellement |
| `—` | source connue mais sans objet pour l’entité actuelle |
| `STALE` | dernière valeur conservée après silence du producteur |
| `ERR` | valeur reçue mais invalide |
| `EN ATTENTE` | négociation ou keyframe initiale non terminée |

Une vraie valeur zéro reste toujours affichée comme `0`.

Après trois secondes sans état valide, le bridge conserve la dernière image et
la marque `Stale` tout en demandant une resynchronisation. Après dix secondes
sans progrès FSTL, il abandonne l’ancienne session, ouvre un nouvel endpoint UDP
et renégocie automatiquement jusqu’au retour du jeu. Le menu **Session** permet
également de demander une resynchronisation douce ou une reconnexion complète ;
ces actions redémarrent uniquement l’observateur du dashboard, jamais FS2Open.

La barre inférieure regroupe les actions dans **Session**, **Données** et
**Affichage**. Figer l’affichage immobilise seulement les instruments : le
statut de connexion et la réception des snapshots continuent en arrière-plan.
En replay, le menu **Replay** remplace **Session** et contient lecture, pause,
boucle, état du producteur UDP et retour au direct. La timeline persistante
porte la position et les marqueurs.

## Onglet Pilotage

La sphère centrale projette le quaternion `orientation_local_to_world` dans un
repère inertiel synthétique de mission (`+Y` vertical, plan `XZ` horizontal).
Elle ne représente donc pas un horizon gravitationnel ou planétaire. Le
marqueur de trajectoire utilise la vitesse locale et affiche `—` lorsque la
vitesse est quasi nulle.

Les compteurs primaire, secondaire et contre-mesure représentent les demandes
du tick source, pas des tirs confirmés. Les modes physiques et aides au contrôle
sont décodés depuis les registres fermés FSTL ; leur valeur brute reste visible
dans le panneau d’inspection. Sous 1450 pixels de large, l’onglet se replie et
autorise le défilement vertical. La cible 1920×1080 reste sans défilement.

## Onglet Propulsion & énergie

La vue principale regroupe les réserves d’énergie armes et de carburant
afterburner, les trois indices ETS et les états de propulsion. Les autonomies,
temps de recharge et délais avant disponibilité sont des valeurs dérivées
explicitement à partir des quantités et taux FSTL.

Le bandeau de performances utilise les valeurs nominales du `CLASS_MANIFEST`;
il ne représente jamais la puissance ou la vitesse maximale dynamique du
vaisseau courant. Ces dernières restent `ND`, comme la poussée effective, les
charges énergétiques instantanées et le RCS. Une absence de réservoir
afterburner ou de ressource énergétique compatible affiche `—`, pas zéro.

## Onglet Intégrité

La vue centrale adopte pour la V1 la disposition standard FS2Open à quatre
quadrants : droite, avant, arrière et gauche pour les indices moteur 0 à 3.
Chaque quadrant est normalisé avec son propre maximum ; le quadrant le plus
faible et les totaux restent visibles séparément. Une session présentant un
autre nombre de segments affiche une configuration non prise en charge.

Le plafond rechargeable est affiché en HP. Le débit de régénération instantané
est la seule valeur exprimée en HP/s et le temps de recharge est une estimation
au taux courant. La matrice principale est limitée aux douze sous-systèmes les
plus critiques du joueur ; le panneau d’inspection donne accès à la liste
exhaustive. Un cooldown de tourelle nul signifie seulement que son délai est
écoulé, pas qu’elle possède une cible ou une autorisation de tir.

## Onglet Armement

La vue spécialisée résout les banques du joueur vers leur `WEAPON_MANIFEST`.
Les armes primaire et secondaire sélectionnées affichent leurs munitions ou
coût énergétique, cooldown, portée, dégâts, cadence, guidage, verrouillage,
burst et swarm nominaux. Ces caractéristiques décrivent la classe installée :
elles ne constituent ni un DPS réel, ni une promesse qu’un tir sera accepté.

Les racks principaux sont limités à six banques par famille, en conservant
toujours la banque sélectionnée ; la liste exhaustive reste accessible depuis
le panneau d’inspection. Les tourelles n’occupent une zone que lorsqu’elles
existent. Les compteurs de commande restent des intentions, jamais des tirs
confirmés. La progression du lock, la solution de tir, les événements exacts
et l’animation normalisée des banques restent explicitement `ND`.

## Onglet Support · Docking · Cargo

La vue reste centrée sur le joueur. Une affectation de support est représentée
par une liaison pointillée et ne devient une relation d’amarrage pleine que
lorsque `DOCKING_STATE` publie réellement cette relation. Le diagramme conserve
le joueur, le support assigné et les relations directes parmi les huit entités
visibles ; la composante exhaustive reste inspectable au clavier.

Les états coque, boucliers, sous-systèmes, banques et contre-mesures sont
présentés séparément. Aucun score global de remise en état n’est créé. Distance,
vitesse relative et rapprochement sont des mesures géométriques et ne servent
jamais à inventer une ETA. Le scanner cargo utilise sa durée autoritaire
`required_us`; une cible extérieure volontairement masquée affiche `— cible non
exposée`, pas `ND`. Le retour du support à `NONE` n’est pas présenté comme une
preuve de réussite.

## Développement et tests

```powershell
python -B test\telemetry\protocol\tools\test_fstl_console_client_contract.py
python -B tools\telemetry-dashboard\backend\test_dashboard_runtime.py
python -B tools\telemetry-dashboard\backend\test_capture_store.py
python -B tools\telemetry-dashboard\backend\test_replay_udp.py

Set-Location tools\telemetry-dashboard\frontend
npm ci
npm test
npm run build
npx playwright test
```
