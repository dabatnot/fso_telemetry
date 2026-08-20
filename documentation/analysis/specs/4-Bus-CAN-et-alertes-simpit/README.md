# Phase 4 — Bus CAN et alertes du simpit

## Résultat attendu

Cette phase livre le premier sous-système physique du simpit :

- un bus CAN local extensible entre le Raspberry Pi et les calculateurs ESP32 ;
- un panneau physique `MASTER WARNING` et `MASTER CAUTION` ;
- un caution panel pour l'état du vaisseau et des calculateurs du simpit ;
- un indicateur à huit secteurs pour les missiles entrants et l'état de lock ;
- les firmwares initiaux `WARN CTRL` et `THREAT PROC` ;
- la passerelle `AV CORE` qui transforme la télémétrie UDP en états CAN simples ;
- une application Web locale, hébergée par `AV CORE`, pour configurer les
  modules installés, les seuils et l'éclairage.

La livraison est découpée en lots réordonnables dans la
[roadmap de la Phase 4](ROADMAP.md).

Le format FSTL et son transport UDP entre FS2Open et le Raspberry Pi ne changent
pas. CAN est un bus interne au simpit, en aval du client de télémétrie. La phase
utilise exclusivement les records cockpit déjà disponibles ; elle n'ajoute
aucune donnée de mission ni aucun record wire FS2Open.

```text
FS2Open -- FSTL/UDP --> Raspberry Pi / AV CORE
                              |
                           SocketCAN
                              |
========================= CAN 1 Mbit/s =========================
          |                    |                    |
      WARN CTRL           THREAT PROC          nœuds futurs
        ESP32                ESP32
          |                    |
   warnings/cautions      anneau de menace
```

## Panneau physique

La disposition exacte et les dimensions ne font pas partie de cette phase. Elles
seront choisies après plusieurs prototypes en carton et polystyrène extrudé,
puis reprises dans un modèle paramétrique FreeCAD.

L'inventaire fonctionnel initial est :

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                            MASTER WARNING                               │
│   FIRE       MISSILE       BLAST       COLLISION       EMP              │
├───────────────────────────────────────────┬─────────────────────────────┤
│              CAUTION PANEL                │      THREAT INDICATOR       │
│             MASTER CAUTION                │            ↑                │
│                                           │        ↖       ↗           │
│ ENG      SENS       SHIELD      HULL      │      ←   LOCK   →           │
│ WEP EN   AB FUEL    AMMO        CM LOW    │        ↙       ↘           │
│ SUBSYS                                    │            ↓                │
│                                           │                             │
│ AV CORE  FLT DATA   AV BUS      SENS PROC │                             │
│ THREAT PROC          INST PROC  WARN CTRL │                             │
├───────────────────────────────────────────┴─────────────────────────────┤
│                         LAMP TEST       BRT                             │
└─────────────────────────────────────────────────────────────────────────┘
```

Il n'y a ni écran, ni alarme sonore ajoutée, ni bouton `ACK`. Les sons restent
ceux du jeu. Aucun voyant n'est mémorisé localement après disparition de sa
condition.

## Comportement des warnings

Les légendes `FIRE`, `MISSILE`, `BLAST`, `COLLISION` et `EMP` clignotent en
orange tant que leur condition est active. `MASTER WARNING` clignote dès qu'au
moins une de ces légendes est active et s'éteint avec la dernière condition.

Les sources sont les états cockpit existants :

- `FIRE` reprend la menace de tir primaire ;
- `MISSILE` est actif tant que la liste des missiles entrants n'est pas vide ;
- `BLAST`, `COLLISION` et `EMP` reprennent les alertes HUD correspondantes.

## Comportement des cautions du vaisseau

Une caution active est orange fixe. `MASTER CAUTION` est allumé si au moins une
caution du vaisseau ou du simpit est active. Les seuils initiaux sont des
dérivations locales du Raspberry avec hystérésis :

| Voyant | Activation | Extinction |
|---|---:|---:|
| `ENG` | intégrité moteur `< 50 %` | intégrité `> 55 %` |
| `SHIELD` | bouclier total `< 30 %` | bouclier total `> 35 %` |
| `HULL` | coque `< 40 %` | coque `> 45 %` |
| `WEP EN` | énergie armes `< 20 %` | énergie armes `> 30 %` |
| `AB FUEL` | carburant postcombustion `< 20 %` | carburant `> 25 %` |
| `AMMO` | banque sélectionnée finie `< 20 %` | banque `> 25 %` |
| `CM LOW` | reste `<= max(3, 20 % de la capacité)` | après réapprovisionnement au-dessus du seuil |
| `SUBSYS` | sous-système fonctionnel hors moteurs/capteurs `< 40 %` | intégrité `> 45 %` |
| `SENS` | capteurs `DEGRADED` ou `OFFLINE` | capteurs `ONLINE` |

Les armes sans réserve de munitions sont ignorées. Les seuils sont des valeurs
de configuration de `AV CORE` modifiables depuis l'application Web locale. Ils
ne sont pas codés dans les ESP32.

## Indicateur de menace

`LOCK` clignote lentement pendant une tentative de verrouillage et rapidement
quand le lock est acquis. La cadence visuelle est produite localement par
`THREAT PROC` depuis l'état discret reçu.

Tous les missiles entrants sont affichés :

- le Raspberry transforme leur position monde en direction locale au joueur ;
- la direction est projetée dans le plan droite/avant du vaisseau ;
- chacun des huit secteurs couvre 45 degrés ;
- chaque secteur contenant au moins un missile clignote ;
- plusieurs secteurs peuvent être actifs simultanément ;
- plusieurs missiles dans le même secteur ne changent ni la luminosité ni la
  cadence.

La composante verticale n'est pas représentée dans ce premier instrument. La
télémétrie ne fournit pas la direction de l'émetteur d'un lock : `LOCK` reste
donc central et le panneau n'invente aucun état `SAM`, `AAA`, `SEARCH` ou
`TRACK`.

## Noms et surveillance des calculateurs

Les légendes décrivent une fonction du chasseur spatial et jamais le composant
informatique réel :

| Légende | Fonction réelle |
|---|---|
| `AV CORE` | Raspberry Pi et service passerelle |
| `FLT DATA` | réception et fraîcheur de la télémétrie FS2Open |
| `AV BUS` | état du contrôleur et du bus CAN |
| `SENS PROC` | futur calculateur radar/capteurs |
| `THREAT PROC` | calculateur de l'indicateur de menace |
| `INST PROC` | futur calculateur des jauges |
| `WARN CTRL` | calculateur du caution panel |

Chaque nœud installé publie `NODE_STATUS` environ une fois par seconde. La liste
des rôles réellement installés est configurée dans l'application Web et
persistée sur le Raspberry. Un voyant réservé à un futur nœud ne constitue pas
une panne tant que ce rôle n'est pas déclaré installé.

Un nœud qui se déclare dégradé allume sa légende en orange fixe. Un nœud attendu
qui ne publie plus de heartbeat pendant trois secondes la fait clignoter. Si la
télémétrie FS2Open devient périmée pendant une seconde, `FLT DATA` clignote et
les indications tactiques périmées sont effacées. Une resynchronisation est
demandée immédiatement ; sans reprise pendant la seconde suivante, `AV CORE`
ouvre une nouvelle session FSTL sur le même endpoint UDP avec un nouveau nonce.
Si le producteur reste indisponible, ce `HELLO` logique est retransmis avec le
même nonce au lieu de créer des générations de sessions successives.
Pour le poste de développement réunissant AV CORE, dashboard et radar, la
configuration FS2Open recommandée est `maxClients: 4`; le défaut moteur reste à
un client. `WARN CTRL` détecte localement
un état CAN `bus-off` et allume `AV BUS` même si le Raspberry n'est plus
joignable.

Un calculateur `WARN CTRL` totalement arrêté ne peut pas annoncer sa propre
panne. Un éventuel voyant électrique indépendant `WARN PWR` reste hors phase.

## Application Web de configuration

L'application Web est intégrée au processus `AV CORE`. Le même service possède
le client FSTL/UDP, SocketCAN, la configuration et le serveur HTTP :

```text
Navigateur -- HTTP + événements --> AV CORE
                                      |-- FSTL / UDP
                                      |-- SocketCAN / can0
                                      `-- configuration JSON
```

Le backend est écrit en Python avec FastAPI. Le frontend est écrit en
React/TypeScript et construit avec Vite. FastAPI sert directement les fichiers
statiques produits par le build ; Node.js n'est pas requis sur le Raspberry en
fonctionnement. Il n'y a ni second service Web, ni nginx, ni conteneur Docker.

`AV CORE` est installé comme service `systemd`, démarre avec le Raspberry et
redémarre automatiquement après une erreur du processus. L'application reste
disponible lorsque le jeu, le bridge UDP, la télémétrie ou CAN sont arrêtés.
L'adresse usuelle est `http://av-core.local:8080` lorsque mDNS est disponible,
avec l'adresse IP du Raspberry comme solution de repli.

L'interface est en français, sombre, lisible et responsive afin de fonctionner
depuis un ordinateur, une tablette ou un téléphone. Une barre d'état commune
reste visible sur toutes les pages et résume `TÉLÉMÉTRIE`, `CAN` et chaque rôle
installé, par exemple :

```text
TÉLÉMÉTRIE  LIVE    CAN  OK    WARN CTRL  ONLINE    THREAT PROC  ONLINE
```

L'application est destinée au réseau local de confiance du cockpit et ne
dépend d'aucun service cloud. Cette première version ne comporte ni compte
utilisateur, ni authentification, ni certificat TLS et ne doit pas être exposée
directement à Internet.

### Page Modules

La page présente une carte pour chaque rôle fixe connu. Une carte indique :

- si le rôle est déclaré installé dans le cockpit ;
- son état `ONLINE`, `DEGRADED`, `ABSENT` ou `CAN ERROR` ;
- son identifiant protocolaire fixe ;
- l'identité matérielle de l'ESP32 ;
- la version de son firmware ;
- l'âge de son dernier heartbeat ;
- son diagnostic CAN.

Le statut `installé` est le seul réglage de cette page. Les rôles et les
identifiants CAN sont affichés mais jamais modifiables.

### Page Alertes

La page présente les seuils d'activation et d'extinction dans un tableau
éditable. Les unités sont affichées avec les valeurs et l'interface refuse les
couples incohérents, notamment un seuil d'extinction qui ne crée aucune
hystérésis au-dessus du seuil d'activation.

Un aperçu indique quelles cautions seraient actives avec le dernier état
cockpit reçu. Il sert uniquement à comprendre le réglage et n'ajoute aucune
donnée à la télémétrie.

### Page Éclairage

La page configure :

- la couleur RGB des warnings et des cautions ;
- la luminosité maximale ;
- la cadence de clignotement lente ;
- la cadence de clignotement rapide ;
- une éventuelle luminosité nocturne.

Elle permet également de tester tous les voyants, un calculateur donné ou un
voyant sélectionné. Ces commandes produisent un état de test temporaire sur le
bus CAN et ne sont pas enregistrées comme des alertes. Le relâchement du test
rend immédiatement la priorité aux états cockpit courants.

### Page Système

La page configure :

- l'adresse et le port du producteur FS2Open ;
- le délai avant de déclarer la télémétrie périmée ;
- le délai avant de déclarer un module absent ;
- le port HTTP de l'application, avec `8080` comme valeur initiale.

Elle affiche en lecture seule :

- l'état de `can0` ;
- le débit CAN fixé à 1 Mbit/s ;
- les compteurs d'erreurs et l'état `bus-off` éventuel ;
- la version et la durée de fonctionnement d'`AV CORE`.

Le débit CAN, les identifiants protocolaires et les rôles restent fixés par le
système et les firmwares. Ils ne sont jamais attribués par l'application Web.

### Persistance et application

Les réglages sont stockés dans un fichier JSON unique :
`/var/lib/fsotelemetry/av-core.json`. Les valeurs par défaut sont intégrées à
`AV CORE`.

Chaque page possède un bouton `Enregistrer et appliquer`. Le backend valide le
document complet avant de remplacer atomiquement la configuration active. Une
configuration invalide est refusée sans modifier le fichier ni l'état courant.

Les seuils, l'éclairage, les timeouts et l'adresse FSTL valides sont appliqués
immédiatement. Un changement d'adresse FSTL provoque une reconnexion propre. Un
changement de port HTTP est enregistré mais prend effet au prochain redémarrage
du service afin de ne pas interrompre silencieusement la requête en cours.

Si CAN est indisponible, la configuration reste enregistrable et les états
correspondants sont publiés lorsque le bus revient. Si la télémétrie est
indisponible, l'application et les diagnostics restent accessibles.

### API locale

L'interface utilise une API HTTP minimale :

```text
GET  /api/config
PUT  /api/config
GET  /api/status
GET  /api/events
POST /api/lamp-test
```

`GET /api/events` utilise Server-Sent Events pour pousser les changements de
statut au navigateur. Les écritures restent de simples requêtes HTTP. Il n'est
pas nécessaire d'ajouter un WebSocket bidirectionnel.

L'application Web ne commande jamais le jeu et ne change pas la projection de
télémétrie. Elle ne transforme que les données cockpit déjà reçues en états de
voyants et en diagnostics du simpit.

### Identité et rôle fixes des modules

Chaque firmware correspond à un organe unique du simpit, par exemple
`WARN CTRL` ou `THREAT PROC`. Il fixe à la compilation :

- son rôle fonctionnel ;
- les identifiants CAN qu'il est autorisé à émettre et à consommer ;
- les capacités qu'il annonce dans `NODE_STATUS`.

Il n'existe pas d'adresse logique configurable, de découverte automatique ni
de bail comparable à DHCP. Pour remplacer un calculateur, il suffit de flasher
le firmware de l'organe concerné sur le nouvel ESP32. Le module reprend alors
le même rôle et les mêmes identifiants CAN sans configuration d'adresse.

L'identifiant matériel d'usine de l'ESP32 reste inclus dans son diagnostic. Il
ne participe pas à l'adressage et n'est jamais modifié. `AV CORE` l'utilise
uniquement pour distinguer le matériel effectivement présent et signaler si
deux ESP32 différents annoncent accidentellement le même rôle, ce qui indique
que le même firmware a été installé deux fois.

L'application Web configure seulement quels rôles fixes sont effectivement
installés dans le cockpit. Cette liste permet de signaler un module attendu
mais absent sans considérer les organes futurs comme étant en panne.

## Bus CAN du simpit

Le bus utilise :

- CAN classique 2.0 ;
- des identifiants standards sur 11 bits ;
- un débit de `1 000 000 bit/s` ;
- des trames de huit octets ;
- une topologie linéaire avec des dérivations courtes ;
- une paire torsadée `CAN-H`/`CAN-L` ;
- exactement deux terminaisons de 120 ohms, aux extrémités physiques.

Le Raspberry utilise un Waveshare `2-CH CAN HAT+` isolé, basé sur deux MCP2515
et deux transceivers SN65HVD230. La phase utilise uniquement `CAN0` via
SocketCAN ; `CAN1` reste inutilisé. Le cavalier de terminaison de `CAN0` est
installé seulement si le Raspberry est à une extrémité physique.

Les nœuds ESP32 utilisent les modules disponibles basés sur un MCP2515, un
quartz de 8 MHz et un transceiver TJA1050. Ils sont alimentés en 5 V et reliés à
l'ESP32 par SPI à travers des convertisseurs de niveau logique :

```text
ESP32 3,3 V                         MCP2515/TJA1050 5 V
MOSI  ---- adaptation 3,3 -> 5 V --> SI
SCK   ---- adaptation 3,3 -> 5 V --> SCK
CS    ---- adaptation 3,3 -> 5 V --> CS
MISO  <--- adaptation 5 -> 3,3 V --- SO
INT   <--- adaptation 5 -> 3,3 V --- INT
GND   ------------------------------ GND
```

Les convertisseurs doivent convenir à des signaux push-pull SPI. Les petits
adaptateurs MOSFET BSS138 destinés à l'I2C ne conviennent pas. Le SPI peut être
limité à 2 ou 4 MHz ; le pilote MCP2515 doit être configuré pour le quartz de
8 MHz et pour CAN à 1 Mbit/s. Le contrôleur TWAI intégré à l'ESP32 n'est pas
utilisé avec ce matériel.

Le cavalier `J1` de chaque module ESP32 connecte sa terminaison de 120 ohms. Il
est retiré sur tous les nœuds intermédiaires et installé uniquement si ce nœud
est l'extrémité opposée au Raspberry. Bus hors tension, la résistance mesurée
entre `CAN-H` et `CAN-L` doit être proche de 60 ohms lorsque les deux
terminaisons sont actives.

## Messages CAN initiaux

Le Raspberry publie des états complets remplaçables à une cadence de 10 à 20 Hz.
Une trame perdue est remplacée par l'état suivant ; aucun ACK, historique ou
mécanisme de retransmission applicatif n'est ajouté à ces états périodiques.

Le vocabulaire initial est limité à :

- `WARNING_STATE` : bitmask des cinq warnings et état master ;
- `CAUTION_STATE` : bitmask des cautions du vaisseau ;
- `THREAT_STATE` : huit secteurs et état de lock ;
- `LIGHTING_STATE` : luminosité globale et état momentané de `LAMP TEST` ;
- `NODE_STATUS` : rôle fixe, identité matérielle, état et heartbeat d'un
  calculateur.

Les identifiants CAN exacts et l'encodage octet par octet seront fixés avec le
code commun Raspberry/ESP32. Ils ne modifient pas le registre FSTL.

## Éclairage et alimentation

Le matériel disponible comprend des WS2812B en ruban et des WS2812 traversantes
de 5 mm. Chaque calculateur pilote une chaîne locale : `WARN CTRL` pilote le
caution panel et `THREAT PROC` l'anneau. Les traversantes sont privilégiées
pour les cellules déplaçables des prototypes ; le ruban convient aux grandes
surfaces et aux ensembles dont son pas mécanique convient.

L'alimentation retenue est une alimentation centrale 5 V correctement
dimensionnée. Le 5 V et la masse sont distribués en étoile ou en arbre et
réinjectés à plusieurs endroits depuis les mêmes bornes d'alimentation afin de
limiter les chutes de tension. Plusieurs sorties 5 V d'alimentations ordinaires
ne sont jamais mises en parallèle.

Chaque branche de panneau comporte un fusible adapté et un condensateur de
réservoir à son arrivée. Chaque WS2812 traversante reçoit son condensateur de
découplage de 100 nF ; les rubans utilisent leur découplage intégré. Le signal
LED 3,3 V de l'ESP32 est adapté au niveau logique attendu par la première LED
5 V.

`BRT` règle la luminosité globale et `LAMP TEST` allume temporairement tous les
voyants. `WARN CTRL` applique ces commandes localement et publie
`LIGHTING_STATE` pour les autres nœuds.

## Périmètre et exclusions

La phase ne livre pas :

- de nouvelle donnée de télémétrie FS2Open ;
- de vue omnisciente ou d'information de mission cachée ;
- de reproduction d'un véritable RWR ou de classification d'émetteur ;
- de service cloud, compte distant, base de données ou administration depuis
  Internet ;
- d'attribution dynamique d'adresse, de découverte ou de protocole de bail des
  modules CAN ;
- de mise à jour OTA des ESP32, console CAN brute, historique ou graphiques ;
- de duplication du dashboard de télémétrie dans l'application de
  configuration ;
- de Docker, nginx ou service Web séparé sur le Raspberry ;
- d'alarme sonore, d'acquittement ou de mémorisation des alertes ;
- les jauges physiques, le radar ou les MFD futurs ;
- les dimensions, pièces FreeCAD ou fichiers de fabrication définitifs ;
- un protocole CAN généraliste ou la réplication complète de FSTL sur CAN.

## Vérification attendue

Les tests unitaires utiles couvrent :

- la conversion des états cockpit en warnings et cautions ;
- les seuils et leur hystérésis ;
- la conversion de tous les missiles entrants en bitmask de secteurs ;
- l'encodage et le décodage des messages CAN ;
- les heartbeats, timeouts et retraits d'états périmés ;
- la logique de `LAMP TEST` et de luminosité ;
- la validation, la persistance et l'application à chaud de la configuration
  Web ;
- les routes HTTP, la diffusion SSE des changements de statut et la continuité
  de l'application lorsque FSTL ou CAN est indisponible ;
- les commandes temporaires de test des voyants et le retour à l'état cockpit ;
- la reconnaissance des rôles fixes, des identités matérielles et d'un doublon
  de firmware dans `NODE_STATUS`.

La vérification manuelle couvre la réception à 1 Mbit/s entre le HAT et au moins
deux ESP32, la mesure d'environ 60 ohms du bus hors tension, l'allumage des
voyants en jeu et la disparition des informations tactiques après arrêt de la
télémétrie. Elle vérifie aussi l'accès depuis un téléphone ou une tablette, la
persistance après redémarrage, l'application d'un seuil et les tests de voyants
avec et sans télémétrie active.

## Décisions encore ouvertes

- référence exacte des convertisseurs de niveau SPI ;
- identifiants CAN et disposition précise des huit octets de chaque message ;
- modèle de connecteur verrouillable et pinout du faisceau ;
- valeurs RGB, luminosité maximale et cadences visuelles ;
- dimensions et disposition finales après les prototypes physiques.
