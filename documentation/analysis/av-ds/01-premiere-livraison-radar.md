# AV DS — Première livraison Radar

## 1. Résultat attendu

La première livraison transforme le client radar autonome existant en
**AV DS — AV Display System**.

AV DS s'exécute sur un second PC Windows et affiche deux radars
indépendants dans deux fenêtres maximisées sans bordure :

- `MFD-L`, associé au MFD Cougar gauche ;
- `MFD-R`, associé au MFD Cougar droit.

Les deux unités reçoivent le même état cockpit autorisé depuis FS2Open, mais le
pilote règle leur présentation séparément avec les OSB. Cette livraison reste
strictement en réception vis-à-vis de la simulation : elle ne change ni la
portée radar du jeu, ni la cible, ni aucun autre état de FS2Open.

Le produit reste facultatif. FS2Open et son HUD conservent leur comportement
complet lorsqu'AV DS est absent, arrêté ou déconnecté.

## 2. Périmètre

La livraison couvre :

- le changement d'identité du client Radar vers AV DS ;
- une application AV DS unique sur le PC d'affichage ;
- une connexion FSTL partagée vers FS2Open ;
- deux unités d'affichage indépendantes `MFD-L` et `MFD-R` ;
- deux fenêtres maximisées sans bordure, chacune affectée à un écran physique ;
- l'affectation d'un MFD Cougar à chaque unité ;
- le routage des OSB du Cougar vers l'unité correspondante ;
- une page fonctionnelle `RADAR` ;
- une infrastructure minimale de sélection de page, même si `RADAR` est la
  seule page disponible dans cette livraison ;
- des réglages et filtres Radar locaux propres à chaque MFD ;
- les états d'attente, de connexion, de perte de télémétrie et de reprise ;
- le lancement et l'exploitation sous Windows.

Sont exclus :

- la page `COM` et les Talking Heads ;
- les modes COM permanent et dynamique ;
- les pages du Central Pedestal ;
- toute commande de simulation envoyée à FS2Open ;
- le changement de portée du radar du jeu ;
- le ciblage depuis AV DS ;
- les menus et ordres de communication ;
- AV CORE, le bus CAN et les firmwares ESP32.

## 3. Déploiement fonctionnel

```text
PC principal
FS2Open
    |
    | FSTL sur le réseau local
    v
PC d'affichage Windows
AV DS
    |-- MFD-L : fenêtre Radar <-> Cougar gauche
    `-- MFD-R : fenêtre Radar <-> Cougar droit
```

AV DS utilise une seule réplique de l'état cockpit et la rend sur deux unités.
L'ajout d'une seconde fenêtre ne doit pas créer une seconde interprétation des
données ni imposer une seconde session FSTL.

La future unité `PEDESTAL` peut être réservée dans le modèle de configuration,
mais aucune fenêtre, page, entrée ou exigence de validation correspondante
n'est requise dans cette livraison.

## 4. Indépendance des MFD

`MFD-L` et `MFD-R` vivent chacun de leur côté. Pour chaque unité, AV DS conserve
séparément :

- l'écran physique affecté ;
- le contrôleur physique affecté ;
- la page active ;
- les filtres Radar ;
- les réglages de présentation ;
- l'état local des OSB et menus de page.

Une action locale sur un MFD ne modifie jamais l'autre. Les deux configurations
suivantes doivent notamment être possibles simultanément :

```text
MFD-L                         MFD-R
RADAR                         RADAR
vaisseaux visibles            armes et missiles visibles
armes masquées                vaisseaux masqués
réglages visuels L            réglages visuels R
```

Les deux unités peuvent également utiliser exactement les mêmes réglages. AV DS
n'impose aucune complémentarité entre le MFD gauche et le MFD droit.

## 5. Fenêtres et écrans

Chaque unité MFD est présentée dans une fenêtre :

- maximisée sans bordure sur l'écran affecté ;
- sans barre de titre ni décoration du système ;
- dimensionnée à la surface disponible de cet écran ;
- capable de retrouver son affectation après un redémarrage lorsque l'écran est
  encore disponible ;
- indépendante de la résolution et de l'échelle de l'autre écran.

La configuration ne doit pas dépendre uniquement de l'ordre courant des
écrans, susceptible de changer après un branchement ou une mise à jour du
système. Si un écran affecté est absent, l'autre unité doit rester utilisable et
AV DS doit signaler clairement l'affectation manquante.

Le mode de configuration initiale des écrans et le comportement exact d'une
fenêtre en l'absence de son moniteur restent à préciser avec l'expérience
d'installation sur le matériel réel.

## 6. Contrôleurs Cougar et OSB

Chaque MFD Cougar est affecté explicitement à une unité AV DS. Les événements
d'un Cougar ne sont transmis qu'à son unité.

Sous Windows avec les pilotes officiels Thrustmaster, les deux contrôleurs sont
identifiés de façon persistante comme `F16 MFD 1` et `F16 MFD 2`. L'affectation
par défaut est `F16 MFD 1` vers `MFD-L` et `F16 MFD 2` vers `MFD-R`. Une
réaffectation manuelle reste possible, mais AV DS ne doit jamais utiliser la
position d'énumération « premier/deuxième joystick » comme identité permanente.

AV DS lit les 28 entrées de chaque Cougar avec SDL3. Les entrées `1` à `20`
correspondent aux OSB autour de l'écran ; les basculeurs `SYM`, `CON`, `BRT` et
`GAIN` occupent respectivement `21/22`, `23/24`, `25/26` et `27/28`. Le détail
de leur orientation est fixé dans la phase 3.

T.A.R.G.E.T n'est ni requis ni utilisé : sa fusion de plusieurs contrôleurs en
un périphérique virtuel unique irait à l'encontre de l'indépendance des deux
MFD. Le pilotage logiciel du rétroéclairage et des LED n'est pas inclus.

La page dessine près des bords de l'écran les libellés correspondant aux OSB
physiques. Dans cette livraison, les OSB assurent :

- l'accès à la sélection de page ;
- la sélection de `RADAR` ;
- l'activation ou la désactivation des filtres Radar locaux ;
- les réglages de présentation retenus pour la page ;
- l'affichage des commandes futures déjà prévues, lorsqu'elles sont clairement
  indiquées comme inopérantes.

Un contrôleur absent ou débranché ne doit pas interrompre la réception FSTL ni
le fonctionnement de l'autre MFD. Une méthode d'entrée de développement au
clavier ou à la souris peut être fournie, sans devenir une exigence du cockpit
final.

## 7. Infrastructure minimale de pages

AV DS n'est plus une application dédiée au seul Radar. Il possède dès cette
livraison une notion explicite :

- d'unité d'affichage ;
- de catalogue de pages ;
- de page active par unité ;
- d'entrée OSB routée vers la page active.

Le catalogue ne contient initialement qu'une page fonctionnelle, `RADAR`. Une
présentation de sélection peut donc sembler triviale, mais elle doit permettre
d'ajouter ultérieurement `COM` sans reconstruire la gestion des fenêtres,
contrôleurs et unités.

Cette exigence ne demande ni chargement dynamique de plugins, ni moteur de
scripts, ni abstraction générale destinée à des pages hypothétiques. Un
catalogue statique de pages intégrées à AV DS est suffisant.

## 8. Page RADAR

### 8.1 Données affichées

La page réutilise le comportement fonctionnel du radar autonome existant. Elle
consomme uniquement les états et catalogues autorisés par le profil
`CockpitSensors`, notamment :

- l'état et la portée radar publiés par FS2Open ;
- les contacts visibles ou déformés autorisés ;
- leurs positions dans le repère radar ;
- les types, couleurs et informations affichables publiés ;
- les effets de capteurs utiles au rendu.

AV DS ne reconstruit jamais une piste depuis un état caché et ne rend jamais un
contact absent de la projection cockpit reçue.

### 8.2 Filtres locaux

Le pilote peut afficher ou masquer localement des catégories de contacts à
l'aide des OSB. Chaque MFD possède son propre jeu de filtres.

Un filtre local :

- agit seulement sur le rendu de l'unité concernée ;
- ne modifie pas les données partagées reçues ;
- ne change pas le radar du HUD FS2Open ;
- ne change pas la configuration de l'autre MFD ;
- ne peut que masquer un contact déjà autorisé, jamais en révéler un autre.

La liste exacte des catégories et leur affectation aux OSB seront fixées avec
la maquette de la page Radar. Elles devront être fondées sur des catégories
réellement disponibles dans les records cockpit reçus.

### 8.3 Réglages de présentation

Les réglages purement locaux peuvent être modifiés indépendamment sur chaque
MFD. Ils peuvent inclure, selon les capacités déjà présentes dans le client
Radar :

- visibilité de couches ou d'overlays ;
- options de libellés ;
- présentation ou intensité locale ;
- autres choix n'altérant pas la simulation.

La liste définitive appartient à la conception de la page. Un réglage annoncé
comme local ne doit jamais être interprété comme une commande de FS2Open.

### 8.4 Portée du radar

Les OSB de portée préparent l'évolution future mais restent inopérants dans
cette livraison.

- La portée affichée comme état du jeu reste celle reçue depuis FS2Open.
- Une pression sur un OSB de portée ne modifie pas la simulation.
- AV DS ne simule pas localement une nouvelle portée en la présentant comme
  acceptée par le jeu.
- L'interface distingue clairement une action indisponible d'une action active.

Le changement réel de portée sera spécifié avec la future voie de commandes
cockpit d'AV DS vers FS2Open.

## 9. Réception seule vis-à-vis de la simulation

La première livraison ne contient aucune commande métier d'AV DS vers
FS2Open.

Le trafic retour FSTL reste limité aux besoins techniques du protocole :

- ouverture et négociation de session ;
- heartbeats et synchronisation d'horloge ;
- acquittements ;
- demandes de fragments ou de resynchronisation.

Ces messages ne constituent pas des commandes de pilotage. Aucun appui OSB ne
doit générer de message capable de :

- modifier une option du jeu ;
- changer la portée ou la cible ;
- déclencher une action de vaisseau ;
- parcourir un menu FS2Open ;
- transmettre un ordre de communication.

Les filtres, pages et réglages AV DS restent locaux au PC d'affichage.

## 10. Cycle de vie visible

Chaque unité affiche un état cohérent avec la session partagée :

- avant FS2Open ou hors mission, Radar indique qu'il attend un état cockpit ;
- lorsque la session devient active, chaque MFD affiche sa propre page Radar à
  partir de la même réplique ;
- pendant une pause, le comportement de disponibilité reste celui du client
  Radar existant ;
- lorsqu'une télémétrie devient périmée, les deux unités le signalent sans
  inventer de nouveaux contacts ;
- après reconnexion ou keyframe, les deux unités convergent depuis l'état reçu
  tout en conservant leurs réglages locaux ;
- un changement de mission purge les contacts de l'ancienne mission.

Une erreur de fenêtre, d'écran, de contrôleur ou de filtre sur un MFD ne doit
pas modifier le modèle Radar partagé ni les réglages de l'autre MFD.

## 11. Configuration et persistance

La configuration doit pouvoir décrire séparément `MFD-L` et `MFD-R` :

- écran affecté ;
- contrôleur Cougar affecté ;
- page initiale ;
- filtres Radar initiaux ;
- réglages de présentation initiaux.

Les paramètres de connexion FSTL appartiennent à l'application AV DS et sont
partagés par les unités.

La persistance exacte des changements réalisés en vol reste une décision à
prendre. Quelle que soit cette décision, les réglages d'un MFD ne doivent jamais
écraser ceux de l'autre.

## 12. Plateforme Windows

La première livraison cible exclusivement Windows :

- Qt 6 pour les fenêtres, le rendu, le réseau et la configuration ;
- SDL3 pour la lecture des contrôleurs Cougar ;
- découverte et sélection des écrans ;
- fenêtres maximisées sans bordure ;
- découverte et affectation des Cougar ;
- réception FSTL ;
- rendu Radar ;
- commandes OSB et réglages indépendants.

Les autres plateformes ne font partie d'aucune phase. Aucun travail
d'abstraction ou de portabilité n'est demandé dans cette livraison ; une étude
ultérieure resterait une décision produit indépendante.

## 13. Migration du client Radar

Le client Radar existant fournit la base de cette livraison. Sa transformation
doit préserver :

- les règles de session, de reconnexion et de resynchronisation ;
- le décodage FSTL ;
- le modèle Radar et son filtrage de visibilité producteur ;
- le rendu des contacts, icônes, overlays et états de lien ;
- le fonctionnement courant avec un seul écran pendant la transition.

Ces capacités deviennent des composants d'AV DS plutôt qu'un produit Radar
isolé. La migration ne doit pas recopier deux fois le client FSTL ou le modèle
Radar pour créer les deux fenêtres.

## 14. Vérification fonctionnelle

Une vérification courte couvre au minimum :

1. AV DS reçoit une mission depuis FS2Open et affiche Radar sur les deux MFD.
2. Chaque Cougar agit uniquement sur son écran affecté.
3. Les deux MFD peuvent utiliser des filtres Radar différents.
4. Deux configurations identiques produisent deux vues identiques à réglages
   locaux égaux.
5. Un filtre masque uniquement les contacts de sa catégorie déjà reçus.
6. Une pression sur un OSB de portée ne modifie ni l'état affiché comme accepté,
   ni FS2Open.
7. Le débranchement d'un Cougar ou l'absence d'un écran ne coupe pas l'autre
   MFD ni la session FSTL.
8. Une perte de télémétrie puis une reconnexion restaure les données sans perdre
   l'indépendance des réglages locaux.
9. FS2Open reste jouable avec son HUD complet lorsque AV DS n'est pas lancé.
10. Le package Windows permet d'installer et de relancer la même configuration
    à deux MFD.

Ces scénarios sont des vérifications produit directes, pas un gate, un registre
de preuve ou un mécanisme conditionnant les évolutions suivantes.

## 15. Suite prévue

Après cette livraison, l'étape suivante pourra ajouter la [page `COM`
d'AV DS](02-page-communications.md), alimentée par la
[source de communication et les assets locaux](../specs/source-communication-et-assets-locaux/README.md).

Les commandes vers FS2Open, les autres pages MFD et le Central Pedestal restent
des évolutions distinctes.

## 16. Découpage en phases livrables

La première livraison Radar est réalisée en cinq phases. Chacune produit un
résultat autonome que le propriétaire peut lancer et vérifier directement :

| Phase | Livrable testable |
|---|---|
| [1 — Socle AV DS et Radar unique](phases/01-socle-av-ds-radar.md) | AV DS remplace le nom du client Radar et conserve son affichage dans une fenêtre |
| [2 — Deux unités MFD](phases/02-deux-unites-mfd.md) | deux fenêtres Radar sans bordure partagent une session mais gardent leur état local |
| [3 — MFD Cougar et OSB](phases/03-cougar-et-osb.md) | SDL3 route les identités Windows persistantes `F16 MFD 1/2` vers leur MFD et les commandes futures restent visiblement inopérantes |
| [4 — Réglages Radar indépendants](phases/04-reglages-radar-independants.md) | les deux radars appliquent des filtres et réglages locaux différents |
| [5 — Livraison Windows](phases/05-livraison-windows.md) | un package Windows installable conserve affectations et réglages et résiste aux périphériques absents |

Cet ordre minimise les changements simultanés. Il constitue une recommandation
de livraison, pas un mécanisme bloquant : les phases peuvent être explorées ou
réordonnées à la demande du propriétaire.
