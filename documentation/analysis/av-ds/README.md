# AV DS — Spécification fonctionnelle

## Documents

- [Première livraison — transformation du client Radar](01-premiere-livraison-radar.md)
- [Évolution suivante — page Communications](02-page-communications.md)
- [Phase 1 — Socle AV DS et Radar unique](phases/01-socle-av-ds-radar.md)
- [Phase 2 — Deux unités MFD](phases/02-deux-unites-mfd.md)
- [Phase 3 — MFD Cougar et OSB](phases/03-cougar-et-osb.md)
- [Phase 4 — Réglages Radar indépendants](phases/04-reglages-radar-independants.md)
- [Phase 5 — Livraison Windows](phases/05-livraison-windows.md)

## 1. Objet

`AV DS` signifie **AV Display System**. C'est l'application d'affichage du
simpit exécutée sur un second PC Windows.

AV DS reçoit de FS2Open les données cockpit autorisées par FSTL et les présente
sur plusieurs écrans physiques. Il ne remplace aucun affichage natif du jeu :
FS2Open doit rester entièrement jouable sans AV DS, et une information peut être
affichée simultanément dans le HUD du jeu et sur un ou plusieurs MFD.

La première livraison concerne exclusivement les deux MFD. Le Central Pedestal
est réservé dans l'architecture mais ses pages, ses entrées et son comportement
restent hors du périmètre courant.

## 2. Place dans le simpit

```text
PC principal
FS2Open et HUD complet
        |
        | réseau local, FSTL
        v
PC d'affichage Windows
AV DS
        |-- fenêtre MFD-L  <-> contrôleur Cougar gauche
        |-- fenêtre MFD-R  <-> contrôleur Cougar droit
        `-- fenêtre PEDESTAL, réservée pour plus tard
```

AV DS est distinct d'AV CORE :

- AV DS gère les écrans, les pages et les entrées locales des MFD ;
- AV CORE calcule les warnings/cautions et assure la passerelle avec le bus CAN ;
- AV CORE ne relaie ni les pages, ni les animations, ni la session FSTL d'AV DS.

Le bundle des Talking Heads est installé sur le PC principal pour permettre au
producteur d'identifier l'asset sélectionné, et sur le PC d'affichage pour
permettre à AV DS de le rendre localement.

## 3. Unités d'affichage

AV DS peut créer jusqu'à trois fenêtres maximisées sans bordure :

| Unité | Périmètre courant | Contrôleur prévu |
|---|---|---|
| `MFD-L` | oui | MFD Cougar gauche |
| `MFD-R` | oui | MFD Cougar droit |
| `PEDESTAL` | différé | contrôleur de jeu dédié, de type MegaJoy ou équivalent |

Les deux MFD partagent la connexion FSTL et l'état cockpit reçu, mais vivent
fonctionnellement de manière indépendante. Chacun possède notamment :

- sa page active ;
- sa navigation et sa page précédente ;
- son mode de page Communications ;
- ses filtres radar ;
- ses futurs réglages propres à chaque page ;
- son affectation à un écran et à un contrôleur physique.

Aucune règle n'interdit d'afficher deux fois la même page. Les configurations
suivantes sont toutes valides :

- deux pages Communications avec les mêmes réglages ;
- deux radars avec des filtrages différents ;
- un radar et une page Communications ;
- à terme, deux fonctionnalités entièrement différentes.

La sélection d'une page ou d'un réglage sur un MFD ne modifie jamais l'autre.
Le pilote compose librement son environnement en vol.

## 4. Entrées et OSB

Les boutons périphériques des MFD Cougar sont traités comme des **OSB**. AV DS
affiche sur le bord de chaque page les libellés associés aux boutons physiques.

Dans le périmètre initial :

- un OSB permet de sélectionner la page `RADAR` ;
- un OSB permet de sélectionner la page `COM` ;
- les autres OSB portent les actions propres à la page affichée ;
- toutes les actions de navigation et de présentation sont locales à AV DS.

Les événements d'un Cougar sont routés uniquement vers le MFD auquel ce
contrôleur est affecté. L'ordre d'énumération des périphériques par le système
d'exploitation ne doit pas constituer l'identité fonctionnelle gauche/droite.

Le dessin exact des libellés et l'affectation détaillée de chaque OSB seront
définis avec les maquettes des pages.

## 5. Catalogue initial de pages

AV DS fournit initialement deux pages fonctionnelles :

- `RADAR` ;
- `COM`.

Une présentation de sélection ou des commandes de navigation peut être ajoutée
sans constituer une troisième fonction cockpit. Le catalogue est destiné à
s'étendre ultérieurement avec des pages telles que objectifs, alliés,
navigation, armement ou état du vaisseau.

Chaque unité MFD sélectionne librement une page du catalogue disponible. AV DS
n'attribue pas une fonction fixe au MFD gauche ou au MFD droit.

## 6. Page RADAR

La page `RADAR` reprend les fonctions visuelles du client radar autonome
existant et devient une page d'AV DS.

Elle affiche les contacts et informations radar autorisés par la projection
cockpit de FS2Open. Chaque MFD applique ses propres réglages de présentation.

### 6.1 Filtres locaux

Des OSB permettent d'afficher ou masquer localement des catégories de contacts.
Ces filtres :

- n'altèrent pas les données reçues ;
- ne changent pas le radar du HUD FS2Open ;
- n'affectent pas l'autre MFD ;
- ne nécessitent aucune commande vers FS2Open.

Deux pages Radar peuvent donc afficher simultanément des sélections de contacts
différentes à partir du même état cockpit.

### 6.2 Portée radar

La page présente des OSB de sélection de portée. Dans la première livraison,
ils sont visibles mais inopérants, car AV DS ne dispose pas encore d'une voie de
commande de simulation vers FS2Open.

Ils ne doivent ni simuler un changement de portée du jeu, ni laisser croire
qu'une commande a été acceptée. Leur activation future nécessitera une commande
explicite et bornée d'AV DS vers FS2Open.

## 7. Page COM

La [spécification dédiée de la page Communications](02-page-communications.md)
est l'autorité fonctionnelle pour ses modes, ses OSB et le retour automatique
propre à chaque MFD. La présente section en résume le comportement.

La page `COM` affiche le Talking Head correspondant à la communication active
dans FS2Open.

Le Talking Head natif reste simultanément disponible dans le HUD du jeu. AV DS
est un affichage redondant ; il ne désactive, ne déplace et ne remplace aucun
élément de FS2Open.

FS2Open reste l'autorité pour :

- l'asset final sélectionné ;
- le début, le remplacement et la fin de la communication ;
- l'offset courant ;
- la pause, la boucle et la vitesse de lecture ;
- la couleur pleine ou la demande de teinte HUD.

AV DS valide le bundle local, sélectionne l'asset par son identifiant et rend
l'animation à partir de l'état temporel reçu. Les pixels et les fichiers
d'animation ne sont pas transmis pendant la session. La voix continue d'être
jouée par FS2Open et reste hors de la première version d'AV DS.

### 7.1 Mode permanent

En mode permanent, le MFD reste sur la page `COM` jusqu'à une nouvelle sélection
du pilote.

- lorsqu'une communication est active, le Talking Head est affiché ;
- en l'absence de communication, la page présente son état d'attente ;
- la fin d'une communication ne provoque aucun changement de page.

Chaque MFD peut utiliser indépendamment ce mode. Les deux MFD peuvent donc
rester simultanément sur `COM` et afficher la même communication.

### 7.2 Mode dynamique

Le mode dynamique est une préférence propre à un MFD. Une fois armé sur la page
`COM`, il reste actif lorsque le pilote sélectionne une autre page.

Lorsqu'une communication commence sur un MFD dont le mode dynamique est armé :

1. AV DS mémorise la page alors affichée par ce MFD ;
2. ce MFD bascule automatiquement sur `COM` ;
3. les remplacements successifs de communication restent sur `COM` ;
4. à la fin de la communication, le MFD revient à la page mémorisée.

Chaque MFD applique cette règle séparément : zéro, un ou deux MFD peuvent
basculer pour le même événement selon leur configuration respective.

Une sélection manuelle pendant un appel COM dynamique annule le retour
automatique de l'appel en cours pour ce MFD. Le mode dynamique reste armé pour
les communications suivantes tant que le pilote ne le désactive pas.

## 8. Communication avec FS2Open

### 8.1 Première livraison

AV DS est initialement passif vis-à-vis de la simulation. Le trafic retour
nécessaire au fonctionnement de FSTL reste limité au protocole de session,
notamment `HELLO`, acquittements, heartbeats et demandes de resynchronisation.

Les actions suivantes restent entièrement locales :

- sélection des pages ;
- mode COM permanent ou dynamique ;
- filtres visuels du radar ;
- réglages de présentation des MFD.

Les commandes affichées mais non encore supportées ne produisent aucun message
de commande vers FS2Open.

### 8.2 Évolution bidirectionnelle

Une évolution ultérieure pourra ajouter des commandes cockpit explicites et
bornées d'AV DS vers FS2Open. Les besoins identifiés sont notamment :

- modifier la portée du radar ;
- ouvrir et parcourir le menu de communication du jeu ;
- sélectionner un destinataire ;
- transmettre un ordre existant, par exemple demander à un ailier d'attaquer
  la cible du joueur.

Cette évolution n'est pas une API de scripting générale. Elle devra exposer
uniquement des actions cockpit concrètes et préserver le fonctionnement normal
des commandes et menus natifs de FS2Open.

## 9. Configuration fonctionnelle

AV DS doit pouvoir associer, pour chaque unité MFD :

- un écran physique ;
- un contrôleur physique ;
- une page initiale ;
- un mode COM initial ;
- les réglages propres aux pages, notamment les filtres Radar.

La configuration d'un MFD ne doit pas imposer celle de l'autre. Le pilote doit
également pouvoir changer de page et modifier les réglages proposés par les OSB
pendant le vol.

La persistance exacte des choix effectués en vol, la restauration au démarrage
et l'interface de configuration hors mission restent à préciser.

## 10. Indisponibilités

- L'absence d'AV DS ne change jamais le comportement de FS2Open.
- L'absence d'un MFD ou de son contrôleur ne doit pas empêcher l'autre MFD de
  fonctionner.
- Une page `COM` sans bundle compatible n'affiche aucun asset non validé et ne
  dégrade pas la page `RADAR`.
- Une perte de session FSTL retire ou marque périmées les informations cockpit ;
  elle ne conserve pas une ancienne communication comme si elle était active.
- Un filtre Radar ou une navigation locale reste propre au MFD concerné.
- Une action future refusée par FS2Open ne doit jamais être présentée comme
  réussie.

## 11. Périmètre explicitement différé

Les éléments suivants ne font pas partie de la première livraison fonctionnelle :

- pages et commandes du Central Pedestal ;
- commandes de simulation d'AV DS vers FS2Open ;
- manipulation du menu de communication du jeu ;
- pages objectifs, alliés, navigation, armement ou systèmes ;
- restitution audio des communications ;
- suppression ou remplacement d'un affichage natif de FS2Open ;
- transit par AV CORE ou par le bus CAN.

## 12. Scénarios fonctionnels de référence

Une vérification courte doit permettre de constater les comportements suivants :

1. `MFD-L` affiche Radar et `MFD-R` COM permanent sans interaction entre eux.
2. Les deux MFD affichent Radar avec des filtres locaux différents.
3. Les deux MFD affichent COM permanent et reproduisent la même communication.
4. Seul le MFD dont le mode COM dynamique est armé bascule lors d'une
   communication puis retrouve sa page précédente.
5. Lorsque les deux modes dynamiques sont armés, les deux MFD basculent et
   reviennent indépendamment.
6. Une communication reste visible dans le HUD FS2Open pendant sa reproduction
   sur zéro, un ou deux MFD.
7. Les OSB de portée sont présents mais ne modifient pas FS2Open dans cette
   version.
8. Un bundle COM absent ou incompatible ne perturbe ni Radar, ni FS2Open, ni
   l'autre MFD.

Ces scénarios décrivent le comportement attendu ; ils ne constituent ni une
gate de phase ni un système de preuve.
