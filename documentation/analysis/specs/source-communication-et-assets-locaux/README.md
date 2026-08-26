# Source de communication et assets locaux pour AV DS

Ce document définit la source FS2Open, le transport FSTL et les bundles locaux
nécessaires à la [page Communications d'AV DS](../../av-ds/02-page-communications.md).
Il ne définit ni la navigation entre pages, ni les modes permanent et
dynamique, qui relèvent de la spécification fonctionnelle d'AV DS.

## Résultat attendu

Cette évolution permet à AV DS d'afficher la même animation `Talking Head` que
le cockpit FS2Open, au même instant logique et au même rythme, à partir d'un
bundle d'assets installé localement.

FS2Open transmet uniquement l'identité de l'asset et l'état autoritaire de la
lecture. Les images, les fichiers d'animation et l'audio ne traversent jamais
FSTL. Une vue absente ou incompatible ne perturbe ni la télémétrie cockpit, ni
les autres clients FSTL, ni le bus CAN.

L'évolution réutilise strictement les contrats spécialisés déjà définis par
FSTL :

- `COMM_VIEW_LOCAL_ASSETS` côté AV DS ;
- `COMM_VIEW_AUTHORITATIVE_SOURCE` côté producteur ;
- `COMM_ASSET_MANIFEST` ;
- `COMM_VIEW_STATE` ;
- `COMM_VIEW_EVENT` ;
- l'extension `COMM_VIEW_NEGOTIATION` de `HELLO` et `WELCOME`.

Aucun nouveau record FSTL, aucune extension de `CockpitSensors` et aucune vue
omnisciente ne sont ajoutés.

## Autorités et responsabilités

| Composant | Autorité |
|---|---|
| Gauge `Talking Head` | asset final réellement chargé, offset, boucle, couleur et état visible |
| Producteur FSTL | `playback_id`, événements `START/STOP`, état correctif et négociation du bundle |
| Packager | résolution CFile, conversion reproductible, manifeste, hashes et archive de bundle |
| AV DS | validation du bundle local, session FSTL partagée, projection temporelle et rendu de l'animation |

Le producteur reste la seule autorité temporelle. AV DS ne choisit jamais
une variante d'animation, ne reconstruit pas un message moteur et ne déduit pas
un démarrage ou un arrêt depuis un fichier local.

## Périmètre

La première livraison couvre :

- démarrage, remplacement et arrêt d'une communication ;
- lecture unique et boucle ;
- pleine couleur et teinte HUD locale ;
- pause, compression temporelle et vitesse de lecture signée ;
- connexion tardive et resynchronisation en cours de lecture ;
- changement de mission, arrêt de session et retrait de capability ;
- bundle absent, incompatible ou partiellement corrompu ;
- rendu plein écran adapté à un MFD ;
- création et installation hors ligne du bundle avec AV DS.

Sont exclus :

- transport ou restitution de la voix ;
- texte, sous-titres ou historique des messages ;
- streaming de pixels, vidéo H.264 ou capture du HUD ;
- téléchargement automatique du bundle depuis FS2Open pendant une session ;
- contrôle de la communication depuis le MFD ;
- transmission de la vue aux firmwares ESP32 ;
- transit de la vue par AV CORE, son API Web ou le bus CAN ;
- reproduction exacte d'un layout HUD personnalisé.

## Flux produit

```text
Pile de mods + missions
  -> packager CFile
  -> archive de bundle versionnée
       -> copie locale côté FS2Open
       -> installation locale côté AV DS

Gauge Talking Head
  -> hook main-thread borné
  -> état COMM_* dans la session FSTL
  -> session FSTL directe d'AV DS
  -> projection temporelle et rendu sur zéro, un ou deux MFD
```

Le packager s'exécute sur le poste qui possède la pile de mods. La même
archive est installée côté producteur et avec AV DS avant la session. Le hash
canonique du manifeste garantit que les deux extrémités parlent du même
contenu.

AV CORE n'appartient pas à ce flux. Il conserve son rôle de calculateur des
warnings/cautions et de passerelle CAN. AV DS ouvre sa propre session UDP FSTL
vers FS2Open, issue du client radar autonome existant.

## Bundle local

### Formats retenus

Le packager accepte comme sources les formats utilisés par le moteur : ANI,
EFF, APNG et image statique.

Pour la première implémentation MFD, les formats livrés sont limités à :

- `APNG` pour chaque animation ;
- `PNG` pour le cadre facultatif et le placeholder facultatif.

Le packager convertit donc ANI et EFF en APNG reproductible et normalise les
APNG source avec le même encodeur versionné. Ce choix conserve les timings
variables sans inventer une extension au format EFF. Le renderer natif décode
les frames APNG sous le contrôle de sa propre horloge de présentation. Il peut
ainsi effectuer un seek exact, rester en pause et lire en sens inverse. Le
AV DS annonce uniquement les bits de formats `APNG` et `PNG` dans sa
première offre.

La conversion préserve l'ordre des frames, leurs durées, le canal alpha, les
dimensions logiques et la durée totale. Elle suit les règles de timing, de
chemins portables, de hash et d'identité déjà fixées par le contrat FSTL.

### Résolution des assets

Le packager utilise la même pile de mods et la même priorité CFile que le jeu.
Il inclut uniquement les variantes effectivement résolubles dans le périmètre
de missions demandé et exclut les fichiers masqués.

Il produit :

```text
communication-bundle-<hash>/
|-- manifest.json
|-- bundle-info.json
`-- communication/
    |-- animations/
    |   `-- <asset>.png
    |-- frames/
    `-- placeholder/
```

`manifest.json` est le manifeste canonique défini par FSTL. `bundle-info.json`
est un index local généré avec le bundle ; il associe le nom final et le type
de `generic_anim` observables par le gauge à l'`asset_id` correspondant. Il
répète le `bundle_hash`, ne traverse pas FSTL et ne permet aucune ouverture de
fichier par une valeur reçue du réseau.

Le packager refuse une résolution ambiguë, une collision d'ID, un chemin non
portable, une durée incohérente ou une conversion incomplète. Il ne produit pas
de bundle partiel présenté comme valide.

### Utilisation du packager

Le binaire Windows `comm_bundle_packager.exe` est construit uniquement lorsque
`FSO_BUILD_TOOLS=ON`. Il reçoit la racine du jeu, la même pile de mods que le
producteur et un périmètre de missions explicite :

```text
comm_bundle_packager.exe --fs2-root <racine-fs2> [--mod <mods>] --mission <mission.fs2> --output <répertoire>
```

`--all-missions` remplace les options `--mission` lorsqu'il faut volontairement
traiter toutes les missions visibles. `--frame` et `--placeholder` sélectionnent
des images statiques facultatives par leur nom CFile. `--mod-signature` et
`--source-revision` permettent de fixer les champs diagnostiques du manifeste ;
la signature vaut sinon la pile de mods telle qu'elle a été fournie, ou `base`.

L'outil affiche le hash, le chemin de l'archive et le nombre d'assets. Il écrit
d'abord une archive temporaire dans le répertoire de destination puis la publie
par renommage. Une archive existante est acceptée uniquement si ses octets sont
identiques. L'outil ne démarre aucune boucle de jeu et ne nécessite ni Python,
ni Node.js.

### Archive et installation

La sortie distribuable est une archive
`communication-bundle-<hash>.tar.gz`. Elle n'est pas incluse dans la release
générique d'AV DS car elle dépend des mods et missions de l'utilisateur.

La livraison Windows d'AV DS fournit un installateur local :

```text
av-ds.exe --install-communication-bundle <archive.tar.gz>
```

L'installation valide l'archive dans une zone temporaire, recalcule les hashes,
puis l'installe sous la racine de données Windows d'AV DS, dans
`communication-bundles/<bundle_hash>/`. Cette racine appartient à AV DS et
jamais à AV CORE. Le pointeur local `current` désigne
l'unique bundle proposé lors de la prochaine session.
Une installation invalide ne remplace jamais le bundle courant. Réinstaller la
même archive est idempotent. Changer de bundle exige une nouvelle session FSTL.

En développement, un chemin explicite peut remplacer cette racine par variable
d'environnement sans modifier la configuration persistante du client.

## Intégration producteur FS2Open

### Configuration

La configuration de télémétrie accepte un chemin optionnel vers le bundle de
communication. Au démarrage, le producteur valide `manifest.json` et
`bundle-info.json` puis construit une table bornée de correspondance entre
l'identité finale du `generic_anim` et l'`asset_id`.

Si le bundle est absent ou invalide, le producteur n'annonce pas
`COMM_VIEW_AUTHORITATIVE_SOURCE`. Les autres domaines FSTL restent disponibles.

### Hook moteur

Le point d'intégration se trouve dans le gauge `Talking Head`, après la
résolution de l'asset et le choix de l'offset effectivement affiché.

Le hook copie uniquement :

- l'identité finale et le type de `generic_anim` au démarrage ;
- l'identifiant du message moteur et l'émetteur public lorsqu'il existe ;
- l'offset, la durée, le mode de lecture, la couleur et la vitesse signée ;
- la raison d'arrêt.

Il ne charge aucun fichier, ne calcule aucun hash, n'encode aucun paquet et ne
conserve aucun pointeur moteur. Il n'effectue aucune I/O réseau. Lorsque la
télémétrie ou la capability est inactive, son coût se limite à un test rapide.

### Publication

Le producteur :

- émet `START` et `STOP` immédiatement dans un `EVENT_BATCH` fiable ;
- représente un remplacement par `STOP(REPLACED)` puis un nouveau `START` ;
- publie un `COMM_VIEW_STATE` complet après tout changement d'offset, vitesse,
  boucle, couleur ou asset ;
- tente un état correctif au plus à 10 Hz pendant une lecture active ;
- inclut l'état courant dans les keyframes ;
- invalide les anciens `playback_id` lors d'un changement de mission ;
- retire la capability sans arrêter les autres domaines si la source devient
  indisponible.

Les layouts, enums, fréquences et règles de fiabilité restent ceux du contrat
FSTL existant.

## Intégration d'AV DS

### Négociation et cycle de vie

AV DS n'annonce `COMM_VIEW_LOCAL_ASSETS` que si le bundle courant est
entièrement valide. Son `HELLO` porte la version, le hash et les formats
APNG/PNG supportés.

Son modèle de communication reprend les états normatifs :

- `UNSUPPORTED` ;
- `SOURCE_UNAVAILABLE` ;
- `BUNDLE_MISMATCH` ;
- `READY` ;
- `ACTIVE` ;
- `PLACEHOLDER`.

Une télémétrie `STALE` ou `DISCONNECTED`, un changement de mission ou un retrait
de capability masque immédiatement l'ancienne animation. Une pause de mission
conserve la lecture active avec un taux nul. Une connexion tardive utilise
`COMM_VIEW_STATE` pour rejoindre l'offset courant.

AV DS réutilise les codecs, le réassemblage, la synchronisation d'horloge et les
règles de cycle de vie hérités du radar autonome. Une seule session FSTL
alimente les deux unités MFD ; chacune conserve néanmoins son propre état de
page et de présentation. Aucune session, aucun état et aucun asset ne transitent
par AV CORE.

### Vue MFD Communications

La vue native affiche :

- l'animation ou le placeholder dans ses proportions logiques ;
- le cadre local recommandé lorsqu'il existe ;
- une teinte MFD locale lorsque `HUD_TINT` est demandé ;
- l'état `PRÊT`, `ACTIVE`, `PLACEHOLDER` ou la cause d'indisponibilité ;
- le hash court du bundle et un diagnostic concis pour l'installation.

La vue ne possède aucun bouton de lecture, de pause ou de seek. Elle reflète
le cockpit en lecture seule. Les OSB peuvent sélectionner la page et son mode
local sans commander FS2Open. Elle ne présente ni texte de mission, ni chemin
de fichier, ni identifiant moteur brut dans la vue opérateur normale.

Le renderer sélectionne la frame depuis les durées du manifeste et l'offset
projeté. Il utilise une horloge locale seulement pour avancer entre deux
corrections ; le prochain état FSTL reste autoritaire. Un retour de veille ou
une restauration de fenêtre force une nouvelle projection au lieu de rattraper
toutes les frames intermédiaires.

`COM` est une page du catalogue AV DS, sélectionnable indépendamment sur
`MFD-L` et `MFD-R`. AV DS applique les modes permanent et dynamique définis par
sa spécification fonctionnelle ; le modèle de communication partagé reste
indépendant des états de page des deux unités.

## Priorités et erreurs

| Situation | Comportement |
|---|---|
| Bundle MFD absent | capability non annoncée, vue `UNSUPPORTED` |
| Source producteur absente | `SOURCE_UNAVAILABLE`, autres données inchangées |
| Version, hash ou formats incompatibles | `BUNDLE_MISMATCH`, aucun asset affiché |
| Asset absent ou hash invalide dans un bundle accepté | `PLACEHOLDER` et diagnostic local |
| `START` dupliqué | acquitté sans redémarrer la lecture |
| `STOP` ancien | ignoré s'il ne vise plus la lecture active |
| Connexion tardive | seek depuis le dernier `COMM_VIEW_STATE` |
| Télémétrie périmée | animation masquée immédiatement |
| Changement de mission | ancienne lecture invalidée |
| Asset ID inconnu | aucun chemin n'est ouvert, placeholder local |

Une erreur visuelle ne dégrade jamais l'état canonique FSTL `LIVE`, les autres
clients FSTL ou les publications CAN.

## Sécurité et ressources

- Le bundle est installé hors session et n'est jamais téléchargé par UDP.
- Les chemins absolus, `..`, backslashes, liens sortant de la racine et noms de
  périphériques réservés sont refusés.
- Chaque fichier est validé par son SHA-256 avant de devenir présentable.
- Les limites FSTL existantes de 4096 assets, 65 535 frames et 4096 pixels par
  dimension restent les bornes absolues ; l'implémentation peut appliquer une
  limite locale plus basse si elle est annoncée par le packager et documentée.
- Le producteur préalloue sa table de correspondance avant la mission et ne
  grandit pas pendant la lecture.
- Le client ne redécode pas une animation à chaque frame ; les métadonnées
  validées et les petites structures de timing sont préparées au chargement du
  bundle, avec un cache graphique borné.
- Les logs n'impriment ni pixels, ni contenu complet du manifeste, ni chemins
  absolus, ni message à chaque frame.

## Vérification attendue

Les tests automatisés utiles couvrent :

- résolution CFile et priorité entre fichiers libres et archives VP ;
- conversion ANI/EFF/APNG vers APNG/PNG avec timings et alpha conservés ;
- reproductibilité du manifeste, des IDs et du hash global ;
- rejet atomique d'un bundle invalide ;
- négociation acceptée, bundle absent, mismatch de version/hash et formats
  incompatibles ;
- `START`, `STOP`, remplacement, doublons, événements anciens et keyframes ;
- pause, taux positif, nul et négatif, boucle et clamp ;
- connexion tardive, changement de mission, `STALE` et reconnexion ;
- refus des chemins issus du réseau et représentation exacte des u64 ;
- rendu MFD des états, du placeholder et de la frame attendue.

La vérification manuelle en jeu couvre une communication normale, un
remplacement, une pause/reprise, un changement de mission, une connexion MFD
en cours de message et un bundle volontairement absent. Une vérification courte
suffit ; aucune campagne longue ou matrice exhaustive de mods n'est imposée.

## Décisions retenues

- AV DS se connecte directement au producteur avec une session FSTL partagée
  par ses deux unités MFD.
- AV CORE, son interface Web et le bus CAN restent hors du chemin de la vue de
  communication.
- La politique de bascule entre `RADAR` et `COM` appartient à AV DS et ne
  modifie pas le protocole.
- Les formats livrés initiaux sont APNG et PNG.
- La teinte HUD utilise le thème local du MFD, pas une couleur transmise par le
  producteur.
- Le bundle est préparé et installé hors ligne sur les deux machines.
- La release générique d'AV DS ne contient aucun asset de mod ou de mission.
- L'audio, le texte et les commandes vers FS2Open restent hors de cette
  évolution.
- La cible de livraison est Windows ; aucun travail de portabilité n'est inclus.
