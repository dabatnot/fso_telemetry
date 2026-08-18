# Roadmap de livraison de la Phase 4

Cette roadmap découpe la Phase 4 en lots verticaux qui produisent chacun un
résultat visible et utilisable. Elle guide l'implémentation mais ne constitue
ni un gate, ni une preuve, ni une obligation d'ordre. Les lots peuvent être
réordonnés, explorés ou poursuivis malgré un test connu en échec après
présentation du risque.

Chaque lot peut être livré par un commit et un push dédiés sur la branche de la
feature. Les tests automatisés directement utiles et une vérification manuelle
suffisent.

| Lot | Livraison visible | Vérification simple |
|---|---|---|
| 1 | Application Web et configuration persistante | Interface accessible sur le Raspberry |
| 2 | Connexion FSTL et calcul des alertes | Les états du jeu apparaissent dans le Web |
| 3 | Bus CAN et firmware commun ESP32 | Un ESP32 échange avec le Raspberry |
| 4 | Caution panel `WARN CTRL` | Les warnings et cautions physiques fonctionnent |
| 5 | Indicateur `THREAT PROC` | Missiles et lock apparaissent physiquement |
| 6 | Installation et intégration complète | Redémarrages et pertes de liaison correctement gérés |

## Lot 1 — Socle AV CORE et application Web

Le premier lot crée la structure du produit :

```text
tools/av-core/
|-- backend/
|-- frontend/
|-- packaging/
`-- tests/
```

Il livre :

- le backend FastAPI ;
- le frontend React/TypeScript construit avec Vite ;
- les quatre pages avec leur disposition initiale ;
- la barre d'état globale ;
- les routes `/api/config`, `/api/status`, `/api/events` et
  `/api/lamp-test` ;
- la configuration JSON validée et écrite atomiquement ;
- les valeurs par défaut ;
- l'interface française, sombre et responsive ;
- un premier service `systemd` ;
- des états `INDISPONIBLE` propres tant que FSTL et CAN ne sont pas intégrés.

Le résultat visible est une application qui démarre sur le Raspberry, reste
accessible et conserve ses réglages après un redémarrage.

Les tests utiles couvrent la validation de la configuration, la lecture et
l'écriture JSON, les routes HTTP et les formulaires du frontend.

## Lot 2 — Connexion FSTL et moteur d'alertes

Ce lot livre :

- la connexion au producteur FS2Open avec le profil `CockpitSensors` ;
- la réutilisation du client Python existant sans les fonctions de capture et
  de replay propres au dashboard ;
- les états `LIVE`, `STALE` et `DISCONNECTED` ;
- le calcul des warnings ;
- le calcul des cautions et de leur hystérésis ;
- le calcul des huit secteurs à partir de tous les missiles entrants ;
- le calcul des états de lock lent et rapide ;
- l'aperçu réel dans la page Alertes ;
- la reconnexion après pause, changement de mission ou redémarrage du jeu.

Ce lot n'ajoute aucune donnée FSTL. L'application Web reflète le cockpit en
direct même si aucun matériel CAN n'est encore raccordé.

Les tests utiles couvrent les mappings des warnings, les seuils, l'hystérésis,
les missiles multiples, la péremption des données ainsi que l'arrêt et la
reprise du flux FSTL.

## Lot 3 — Contrat CAN et firmware commun

Ce lot livre :

- l'attribution définitive des identifiants CAN standards sur 11 bits ;
- la disposition octet par octet des cinq trames initiales ;
- les encodeurs et décodeurs Python ;
- le backend SocketCAN ;
- la publication des états à 10–20 Hz ;
- la réception de `NODE_STATUS` ;
- l'état de `can0`, les compteurs d'erreurs et `bus-off` ;
- la base commune des firmwares ESP32 ;
- le MCP2515 configuré pour un quartz de 8 MHz et CAN à 1 Mbit/s ;
- le rôle et les identifiants fixes dans chaque firmware ;
- le heartbeat contenant le rôle, l'identité matérielle et le diagnostic ;
- la page Modules alimentée par les véritables heartbeats.

L'outillage firmware proposé est PlatformIO avec le framework Arduino, adapté
aux usages MCP2515 et WS2812. ESP-IDF peut être retenu à la place si cette
décision change avant l'implémentation.

Le résultat visible est un ESP32 de test qui communique réellement avec le HAT
du Raspberry.

Les tests utiles couvrent les codecs CAN, les entrées invalides, les timeouts
de heartbeat et SocketCAN avec `vcan`. La vérification matérielle utilise le HAT
et un ESP32.

## Lot 4 — Caution panel WARN CTRL

Ce lot livre :

- le firmware fixe `WARN CTRL` ;
- le mapping physique des WS2812 ;
- `FIRE`, `MISSILE`, `BLAST`, `COLLISION` et `EMP` ;
- `MASTER WARNING` ;
- les cautions du vaisseau et des calculateurs ;
- `MASTER CAUTION` ;
- les clignotements produits localement ;
- l'extinction des états périmés ;
- les commandes physiques `BRT` et `LAMP TEST` ;
- les tests Web de tous les voyants, d'un calculateur ou d'un voyant ;
- les réglages Web de couleur, luminosité et cadence.

Le résultat visible est le premier panneau prototype en carton ou polystyrène
fonctionnant réellement en jeu.

La vérification manuelle active plusieurs alertes, coupe la télémétrie,
débranche CAN, redémarre l'ESP32 et vérifie le retour automatique à l'état
cockpit courant.

## Lot 5 — Indicateur de menace THREAT PROC

Ce lot livre :

- le firmware fixe `THREAT PROC` ;
- l'anneau à huit secteurs ;
- l'affichage simultané de tous les missiles entrants ;
- leur combinaison dans un bitmask de secteurs ;
- `LOCK` lent pendant l'acquisition ;
- `LOCK` rapide après acquisition ;
- le partage de la luminosité et des commandes de test ;
- le heartbeat et le diagnostic du second ESP32.

Le résultat visible est le caution panel et l'indicateur de menace fonctionnant
simultanément sur le même bus.

La vérification manuelle couvre plusieurs directions de missile, plusieurs
missiles dans un secteur, les changements rapides de secteur, le passage de la
tentative de lock au lock acquis et la disparition des indications périmées.

## Lot 6 — Livraison Raspberry et intégration complète

Ce lot livre :

- le script d'installation sur Raspberry Pi ;
- un environnement Python reproductible ;
- le frontend précompilé ;
- le service `systemd` final ;
- la création de `/var/lib/fsotelemetry` ;
- le démarrage de `can0` à 1 Mbit/s ;
- l'accès `av-core.local:8080` lorsque mDNS est disponible ;
- la reconnexion propre à FSTL et CAN ;
- le comportement attendu après redémarrage du Raspberry ;
- la documentation d'installation, de lancement et de mise à jour ;
- le retrait des éléments de développement inutiles sur le Raspberry.

La vérification manuelle couvre :

- le Raspberry démarré avant ou après le jeu ;
- la pause et la reprise ;
- le changement de mission ;
- l'arrêt et le redémarrage du jeu ;
- la déconnexion et la reconnexion CAN ;
- le redémarrage séparé de chaque ESP32 ;
- la modification d'un seuil depuis un téléphone ;
- la persistance de la configuration après reboot.

## Ordre de départ recommandé

Les lots 1 puis 2 constituent le chemin de départ recommandé. Ils fournissent
rapidement une application visible et alimentée par le jeu sans dépendre du
câblage CAN. Cette recommandation n'empêche pas de commencer un prototype
matériel ou un firmware plus tôt si cela facilite l'exploration.
