# Icônes du radar externe

Ce dossier contient le vocabulaire visuel par défaut du radar externe : **44 SVG** autonomes, monochromes et composables. Il couvre les types du protocole de télémétrie, les 17 types de vaisseaux fournis par défaut par FS2Open, les armes utiles au radar, les états de piste et les brackets de sélection.

## Convention de rendu

- Tous les fichiers utilisent `viewBox="0 0 32 32"`, sans taille CSS imposée.
- La couleur vient de `currentColor`. Le client applique donc la couleur IFF ou une couleur d'état sans modifier le SVG.
- L'intensité `BRIGHT`/dim est une opacité de rendu, pas une icône distincte.
- La visibilité `DISTORTED` doit idéalement être rendue par jitter, intermittence et opacité. `overlay-distorted.svg` fournit un repli statique.
- Les icônes sont orientées vers le haut. Le client peut faire pivoter la base avec le cap du contact, sans faire pivoter les brackets ni les badges.
- Ordre de composition conseillé : `overlay-warp` derrière la base, base, overlays d'état, brackets au-dessus.
- Les SVG ne contiennent ni couleur IFF fixe, ni police, ni script, ni ressource externe.

## Contacts génériques

| Fichier | Valeur couverte | Usage |
|---|---|---|
| `contact-unknown.svg` | `RadarCategory::Unknown`, `ObjectType::Unknown` | Identité ou nature non révélée |
| `contact-ship.svg` | `RadarCategory::Ship`, `ObjectType::Ship` | Repli vaisseau générique ou type moddé inconnu |
| `contact-weapon.svg` | `RadarCategory::Weapon`, `ObjectType::Weapon` | Repli arme générique ou sous-type inconnu |
| `contact-navigation.svg` | `RadarCategory::Navigation`, `ObjectType::Waypoint` | Waypoint ou contact de navigation |
| `contact-jump-node.svg` | `RadarCategory::JumpNode`, `ObjectType::JumpNode` | Nœud de saut |
| `contact-asteroid.svg` | `RadarCategory::Asteroid`, `ObjectType::Asteroid` | Astéroïde |
| `contact-debris.svg` | `RadarCategory::Debris`, `ObjectType::Debris` | Débris |
| `contact-fireball.svg` | `ObjectType::Fireball` | Explosion ou fireball si exporté par une vue étendue |
| `contact-other.svg` | `RadarCategory::Other`, `ObjectType::Other` | Catégorie connue mais non spécialisée |

`unknown` ne signifie pas la même chose que `other` : le premier protège une information non révélée, le second représente une catégorie révélée mais sans glyphe spécialisé.

## Types de vaisseaux FS2Open par défaut

| Fichier | `$Name` dans `objecttypes.tbl` | Famille visuelle |
|---|---|---|
| `ship-navbuoy.svg` | `Navbuoy` | Balise |
| `ship-sentry-gun.svg` | `Sentry Gun` | Tourelle fixe |
| `ship-escape-pod.svg` | `Escape Pod` | Capsule |
| `ship-cargo.svg` | `Cargo` | Conteneur |
| `ship-support.svg` | `Support` | Maintenance/réarmement |
| `ship-fighter.svg` | `Fighter` | Petit appareil rapide |
| `ship-bomber.svg` | `Bomber` | Petit appareil lourd |
| `ship-transport.svg` | `Transport` | Transport compact |
| `ship-freighter.svg` | `Freighter` | Coque avec modules cargo |
| `ship-awacs.svg` | `AWACS` | Appareil avec antenne radar |
| `ship-gas-miner.svg` | `Gas Miner` | Coque avec réservoirs |
| `ship-cruiser.svg` | `Cruiser` | Vaisseau de guerre moyen |
| `ship-corvette.svg` | `Corvette` | Vaisseau de guerre lourd |
| `ship-capital.svg` | `Capital` | Vaisseau capital |
| `ship-super-capital.svg` | `Super Cap` | Très grand vaisseau capital |
| `ship-drydock.svg` | `Drydock` | Installation ouverte |
| `ship-knossos-device.svg` | `Knossos Device` | Portail/installation annulaire |

Les mods peuvent remplacer ou étendre `objecttypes.tbl`. Un type absent de cette table doit utiliser `contact-ship.svg`, sauf si le manifeste de classe fournit un `radar_icon_id` exploitable par le client.

## Armes

| Fichier | Sélection conseillée |
|---|---|
| `contact-weapon.svg` | `WeaponSubtype::Unknown` ou repli |
| `weapon-primary.svg` | `WeaponSubtype::Primary` visible au radar |
| `weapon-missile.svg` | `WeaponSubtype::Missile` |
| `weapon-beam.svg` | `WeaponSubtype::Beam` visible au radar |
| `weapon-countermeasure.svg` | `WeaponSubtype::Countermeasure` visible au radar |
| `weapon-special.svg` | `WeaponSubtype::Special` |
| `weapon-bomb.svg` | `ContactFlagBomb` |
| `weapon-mine.svg` | Classe d'arme identifiée comme mine |

Le radar HUD historique range toute arme visible dans sa famille « bomb », à l'exception d'une arme LSSM en transit. Le radar externe peut conserver les distinctions ci-dessus dès que la classe ou le sous-type est effectivement révélé. Le protocole actuel ne possède pas de bit de contact `MINE`; la sélection fiable de `weapon-mine.svg` dépend donc d'un mapping de classe ou d'une future extension.

## Overlays et brackets

| Fichier | État | Placement |
|---|---|---|
| `overlay-tagged.svg` | `ContactFlagTagged` | Au-dessus de la base |
| `overlay-warp.svg` | `ContactFlagWarp` | Derrière la base |
| `overlay-stealth.svg` | `ContactFlagStealth` | Au-dessus de la base |
| `overlay-homing.svg` | `ContactFlagHoming` | Au-dessus de la base |
| `overlay-threat.svg` | `ContactFlagThreat` | Au-dessus de la base, pulsation côté client possible |
| `overlay-distorted.svg` | `RadarVisibility::Distorted` | Repli statique optionnel |
| `brackets-selected-compact.svg` | `ContactFlagCurrentTarget` | Blips denses ou petits |
| `brackets-selected.svg` | `ContactFlagCurrentTarget` | Sélection standard |
| `brackets-selected-large.svg` | `ContactFlagCurrentTarget` | Grande icône ou pulsation |
| `brackets-crosshair.svg` | `ContactFlagCurrentTarget` | Variante proche du radar HUD historique |
| `brackets-lock.svg` | Cible verrouillée | Variante de lock, si l'UI expose cet état |

`ContactFlagBright` est rendu par intensité/opacité. `ContactFlagBomb` sélectionne `weapon-bomb.svg` comme base plutôt qu'un overlay.

## Correspondance avec les six blips du HUD historique

| `BLIP_TYPE_*` | Composition externe recommandée |
|---|---|
| `JUMP_NODE` | `contact-jump-node.svg` |
| `NAVBUOY_CARGO` | `ship-navbuoy.svg` ou `ship-cargo.svg`, sinon `contact-ship.svg` |
| `BOMB` | `weapon-bomb.svg`, `weapon-mine.svg`, un sous-type d'arme ou `contact-weapon.svg` |
| `WARPING_SHIP` | Icône de base + `overlay-warp.svg` |
| `TAGGED_SHIP` | Icône de base + `overlay-tagged.svg` |
| `NORMAL_SHIP` | Type de vaisseau révélé ou `contact-ship.svg` |

Le classement historique donne priorité à `WARP`, puis `TAGGED`, puis `NAVBUOY/CARGO`, puis au vaisseau normal. Sur l'écran externe, les overlays permettent de conserver simultanément le type détaillé et ces états.

## Règle de non-divulgation

Le choix d'une icône ne doit jamais révéler une information masquée par `RADAR_CONTACTS` :

1. utiliser une icône de type/classe seulement si cette information est révélée ;
2. sinon utiliser le générique de la catégorie révélée ;
3. utiliser `contact-unknown.svg` si même la catégorie ne peut pas être divulguée ;
4. ne résoudre un `radar_icon_id` du manifeste qu'après révélation légitime de la classe correspondante.

## Sources de couverture

- Catégories radar, types d'objet et flags : `code/telemetry/protocol/telemetry_protocol_constants.h`.
- Sémantique de `RADAR_CONTACTS` : `documentation/analysis/specs/0-Contrat-de-protocole/04-modele-de-donnees-v1.md`.
- Six familles du radar HUD et filtrage des contacts : `code/radar/radarsetup.h` et `code/radar/radarsetup.cpp`.
- 17 types de vaisseaux par défaut : `code/def_files/data/tables/objecttypes.tbl`.
