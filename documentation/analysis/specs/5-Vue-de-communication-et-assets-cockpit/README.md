# Phase 5 — Vue de communication et assets cockpit

## Résultat attendu

La Phase 5 permet à AV CORE d'afficher la même animation `Talking Head` que le
cockpit FS2Open, au même instant logique et au même rythme, à partir d'un bundle
d'assets installé localement sur le Raspberry Pi.

FS2Open transmet uniquement l'identité de l'asset et l'état autoritaire de la
lecture. Les images, les fichiers d'animation et l'audio ne traversent jamais
FSTL. Une vue absente ou incompatible ne perturbe ni la télémétrie cockpit, ni
le Web, ni le bus CAN.

La Phase 5 réutilise strictement les contrats spécialisés déjà définis par
FSTL :

- `COMM_VIEW_LOCAL_ASSETS` côté AV CORE ;
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
| AV CORE backend | validation du bundle local, client FSTL, projection temporelle et accès borné aux assets |
| Interface Web | présentation de l'image projetée et des états de disponibilité |

Le producteur reste la seule autorité temporelle. AV CORE ne choisit jamais
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
- page Web dédiée dans AV CORE ;
- création et installation hors ligne du bundle.

Sont exclus :

- transport ou restitution de la voix ;
- texte, sous-titres ou historique des messages ;
- streaming de pixels, vidéo H.264 ou capture du HUD ;
- téléchargement automatique du bundle depuis FS2Open pendant une session ;
- contrôle de la communication depuis AV CORE ;
- transmission de la vue aux firmwares ESP32 ;
- reproduction exacte d'un layout HUD personnalisé.

## Flux produit

```text
Pile de mods + missions
  -> packager CFile
  -> archive de bundle versionnée
       -> copie locale côté FS2Open
       -> installation locale côté AV CORE

Gauge Talking Head
  -> hook main-thread borné
  -> état COMM_* dans la session FSTL
  -> projection temporelle AV CORE
  -> page Web Communications
```

Le packager s'exécute sur le poste qui possède la pile de mods. La même
archive est installée côté producteur et côté AV CORE avant la session. Le hash
canonique du manifeste garantit que les deux extrémités parlent du même
contenu.

## Bundle local

### Formats retenus

Le packager accepte comme sources les formats utilisés par le moteur : ANI,
EFF, APNG et image statique.

Pour la première implémentation AV CORE, les formats livrés sont limités à :

- `APNG` pour chaque animation ;
- `PNG` pour le cadre facultatif et le placeholder facultatif.

Le packager convertit donc ANI et EFF en APNG reproductible et normalise les
APNG source avec le même encodeur versionné. Ce choix conserve les timings variables sans inventer une extension au
format EFF. Le frontend décode les frames APNG et les présente dans un canvas ;
il ne dépend pas de la lecture automatique d'une balise image. Il peut ainsi
effectuer un seek exact, rester en pause et lire en sens inverse. AV CORE
annonce uniquement les bits de formats `APNG` et `PNG` dans sa première offre.

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

Le binaire `comm_bundle_packager` est construit uniquement lorsque
`FSO_BUILD_TOOLS=ON`. Il reçoit la racine du jeu, la même pile de mods que le
producteur et un périmètre de missions explicite :

```text
comm_bundle_packager \
  --fs2-root <racine-fs2> \
  [--mod <mod-principal,mod-secondaire,...>] \
  --mission <mission.fs2> [--mission <autre.fs2> ...] \
  --output <répertoire>
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
générique d'AV CORE car elle dépend des mods et missions de l'utilisateur.

AV CORE fournit un installateur local :

```text
install-communication-bundle.sh <archive.tar.gz>
```

L'installation valide l'archive dans une zone temporaire, recalcule les
hashes, puis installe le bundle sous :

```text
/var/lib/fsotelemetry/communication-bundles/<bundle_hash>/
```

Le lien `current` désigne l'unique bundle proposé lors de la prochaine session.
Une installation invalide ne remplace jamais le bundle courant. Réinstaller la
même archive est idempotent. Changer de bundle exige une nouvelle session FSTL.

En développement, un chemin explicite peut remplacer cette racine par variable
d'environnement sans modifier la configuration persistante du Raspberry.

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

## Intégration AV CORE

### Négociation et cycle de vie

AV CORE n'annonce `COMM_VIEW_LOCAL_ASSETS` que si le bundle courant est
entièrement valide. Son `HELLO` porte la version, le hash et les formats
APNG/PNG supportés.

Le sous-état de communication exposé par AV CORE reprend les états normatifs :

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

AV CORE réutilise les codecs et règles du client FSTL existant. Les u64
transportés vers JavaScript sont sérialisés en chaînes décimales pour éviter
toute perte de précision.

### API locale

Le statut général ajoute un bloc `communication` contenant au minimum :

- état de disponibilité ;
- résultat de négociation ;
- hash du bundle local et hash requis ;
- lecture active, `playback_id`, `head_asset_id` et raison du dernier arrêt ;
- mode, couleur, durée, offset projeté et vitesse ;
- métadonnées d'affichage validées de l'asset ou du placeholder.

L'API expose les fichiers uniquement par un identifiant présent dans le
manifeste validé. Une route de type
`GET /api/communication/assets/{asset_id}` ne résout jamais un chemin fourni par
le client. Les assets sont servis depuis la racine canonique du bundle avec un
type MIME fermé et une politique de cache liée au hash du bundle.

Le flux SSE existant signale les changements de communication. Le frontend
peut relire le statut courant après reconnexion ou retour d'un onglet masqué ;
aucune nouvelle socket temps réel n'est ajoutée.

### Page Communications

Une cinquième page `Communications` est ajoutée à l'interface Web. Elle affiche :

- l'animation ou le placeholder dans ses proportions logiques ;
- le cadre local recommandé lorsqu'il existe ;
- la teinte de l'interface AV CORE lorsque `HUD_TINT` est demandé ;
- l'état `PRÊT`, `ACTIVE`, `PLACEHOLDER` ou la cause d'indisponibilité ;
- le hash court du bundle et un diagnostic concis pour l'installation.

La page ne possède aucun bouton de lecture, de pause ou de seek. Elle reflète
le cockpit en lecture seule. Elle ne présente ni texte de mission, ni chemin de
fichier, ni identifiant moteur brut dans la vue opérateur normale.

Le frontend sélectionne la frame depuis les durées du manifeste et l'offset
projeté par AV CORE. Il utilise une horloge locale seulement pour avancer entre
deux corrections ; le prochain état FSTL ou SSE reste autoritaire. Un retour de
veille ou d'onglet masqué force une nouvelle projection au lieu de rattraper
toutes les frames intermédiaires.

## Priorités et erreurs

| Situation | Comportement |
|---|---|
| Bundle AV CORE absent | capability non annoncée, vue `UNSUPPORTED` |
| Source producteur absente | `SOURCE_UNAVAILABLE`, autres données inchangées |
| Version, hash ou formats incompatibles | `BUNDLE_MISMATCH`, aucun asset affiché |
| Asset absent ou hash invalide dans un bundle accepté | `PLACEHOLDER` et diagnostic local |
| `START` dupliqué | acquitté sans redémarrer la lecture |
| `STOP` ancien | ignoré s'il ne vise plus la lecture active |
| Connexion tardive | seek depuis le dernier `COMM_VIEW_STATE` |
| Télémétrie périmée | animation masquée immédiatement |
| Changement de mission | ancienne lecture invalidée |
| Route d'asset inconnue | réponse 404 sans accès au système de fichiers |

Une erreur visuelle ne dégrade jamais l'état canonique FSTL `LIVE` et ne coupe
jamais les publications CAN.

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
- Le backend ne décode pas une animation à chaque requête Web ; les métadonnées
  validées et les petites structures de timing sont préparées au chargement du
  bundle.
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
- protection des routes d'assets et représentation exacte des u64 ;
- rendu frontend des états, du placeholder et de la frame attendue.

La vérification manuelle en jeu couvre une communication normale, un
remplacement, une pause/reprise, un changement de mission, une connexion AV CORE
en cours de message et un bundle volontairement absent. Une vérification courte
suffit ; aucune campagne longue ou matrice exhaustive de mods n'est imposée.

## Décisions retenues

- AV CORE est le client applicatif livré ; aucun second client graphique n'est
  créé.
- La page Web est dédiée à la communication et reste strictement en lecture
  seule.
- Les formats livrés initiaux sont APNG et PNG.
- La teinte HUD utilise le thème local AV CORE, pas une couleur transmise par le
  producteur.
- Le bundle est préparé et installé hors ligne sur les deux machines.
- La release générique AV CORE ne contient aucun asset de mod ou de mission.
- L'audio, le texte et la vidéo cible restent hors Phase 5.
