# Data Model: Socle AV DS et Radar unique

## Application AV DS

Racine de composition et propriétaire du cycle de vie de la liaison.

| Champ | Type | Règles |
|---|---|---|
| `productName` | chaîne constante | `AV DS — AV Display System` |
| `pageCatalog` | `PageCatalog` | Intégré à l'exécutable, non vide |
| `displayUnits` | collection de `DisplayUnit` | Exactement une entrée dans cette phase |
| `connection` | `ConnectionConfiguration` | Partagée par l'application |
| `clientStatus` | `ClientStatus` | Produit par le client FSTL existant |
| `activeMessage` | `AvDsMessage` optionnel | Dérivé du statut et diffusé à toutes les unités |
| `radarImage` | référence immuable optionnelle | Dernier état complet validé reçu |

## DisplayUnit

Surface d'affichage logique associée à une fenêtre.

| Champ | Type | Règles |
|---|---|---|
| `id` | `DisplayUnitId` | Valeur unique `MFD-L` dans cette phase |
| `window` | référence de fenêtre | Fenêtre Windows normale, redimensionnable et maximisable |
| `activePage` | `PageId` | Doit appartenir au catalogue ; initialement `RADAR` |
| `message` | `AvDsMessage` optionnel | Copie en lecture seule de l'état global courant |

Une unité rend d'abord le contenu de sa page active, puis le message global visible en
superposition. Le message ne modifie jamais la page active.

## PageCatalog

Catalogue statique des pages compilées.

| Champ | Type | Règles |
|---|---|---|
| `pages` | liste ordonnée de `PageDescriptor` | Exactement une entrée dans cette phase |

Entrée autorisée :

| `PageId` | Libellé | Contenu |
|---|---|---|
| `RADAR` | `RADAR` | `RadarWidget` alimenté par `RadarImage` |

Le catalogue ne charge ni plugin, ni script, ni page externe.

## RadarPage

Adaptateur entre une unité et le renderer Radar existant.

| Champ | Type | Règles |
|---|---|---|
| `image` | référence immuable vers `RadarImage`, optionnelle | Seulement une image validée par le client |
| `displaySettings` | `RadarDisplaySettings` | Valeurs existantes conservées |

La page affiche les contacts, icônes et couches Radar. Elle ne possède aucun
`ClientStatus`, texte de liaison ou message global.

## AvDsMessage

Présentation globale dérivée de l'état de liaison existant.

| Champ | Type | Règles |
|---|---|---|
| `kind` | enum | attente, synchronisation, pause, périmé, reconnexion, erreur |
| `title` | chaîne | Texte existant du client Radar |
| `detail` | chaîne | Texte existant ou détail d'erreur/configuration |
| `color` | couleur | Couleur cyan, ambre ou rouge existante |
| `visible` | booléen | Faux en état `Live`, vrai pour les états messageables |

### Correspondance des états

| `ClientStatus` | Message global | Image Radar |
|---|---|---|
| `Disconnected` | Message de configuration si nécessaire | Aucun contact présenté comme courant |
| `Resolving`, `Connecting` | Établissement de liaison | Pas de nouvelle image |
| `Synchronizing` | Construction de l'image tactique | Pas de publication avant état complet |
| `Ready` | Attente de mission | Aucun contact de mission précédente comme courant |
| `Live` | Aucun | Image complète courante |
| `Paused` | Mission en pause | Dernière image conservée sans évolution inventée |
| `Stale` | Flux perdu/périmé | Dernière image assombrie et explicitement signalée périmée |
| `Reconnecting` | Réacquisition | Aucune image présentée comme courante |
| `Error` | Échec de liaison | Aucune image présentée comme courante |

## RadarImage

Réplique immuable déjà construite par `makeRadarImage`.

Champs déterminants pour cette phase : `sessionId`, `playerEntityId`, temps producteur,
état de pause, pose du joueur, contacts autorisés, cible, verrouillages, capteurs et
menaces. Tous les contacts proviennent du profil `CockpitSensors`; aucune entité cachée
n'est reconstruite côté client.

### Remplacement et purge

1. Un manifeste et un snapshot complets sont validés atomiquement.
2. Une nouvelle `RadarImage` est construite pour une seule session et un seul
   observateur.
3. La référence courante est remplacée en bloc ; les contacts ne sont jamais fusionnés
   avec l'image précédente.
4. Lors d'une nouvelle mission, d'un nouvel observateur ou d'une renégociation, aucune
   ancienne image ne redevient `Live` avant réception du nouvel état complet valide.

## ConnectionConfiguration

| Champ | Type | Règles |
|---|---|---|
| `configured` | booléen | Indique qu'une destination a été acceptée |
| `host` | chaîne | Hôte existant ; défaut `127.0.0.1` |
| `port` | entier non signé 16 bits | Entre 1 et 65535 ; défaut `42042` |

Les clés restent lues et écrites sous `FS2Open/FsoSimpitRadar` :
`connection/configured`, `connection/host`, `connection/port`, `display/*` et
`window/geometry`.

## State transitions

```text
Disconnected -> Resolving -> Connecting -> Ready
                                      Ready -> Synchronizing -> Live
Live -> Paused -> Live
Live/Paused/Ready -> Stale -> Synchronizing -> Live
Stale -> Reconnecting -> Ready/Synchronizing -> Live
Any non-terminal state -> Error/Reconnecting
```

Les transitions ne produisent que des accusés de réception, demandes de
resynchronisation et messages de fiabilité FSTL ; aucune commande métier n'est envoyée
à FS2Open.
