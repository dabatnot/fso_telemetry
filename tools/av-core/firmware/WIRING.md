# Guide de câblage WARN CTRL — ESP32 DevKit V1

Ce guide décrit le câblage du prototype `WARN CTRL` avec :

- un ESP32 DevKit V1 ;
- un module CAN MCP2515/TJA1050 à quartz 8 MHz ;
- un convertisseur de niveau EPLZON TXS0108E ;
- une chaîne de 23 mini-PCB BTF-LIGHTING WS2812B ECO ;
- un potentiomètre linéaire de 10 kΩ pour `BRT` ;
- un poussoir momentané pour `LAMP TEST`.

Effectuer tout le câblage hors tension.

## Nomenclature (BOM)

### Électronique principale

| Qté | Composant | Caractéristiques requises | Remarque |
|---:|---|---|---|
| 1 | ESP32 DevKit V1 | ESP32-WROOM-32, logique 3,3 V | Cible PlatformIO `esp32dev` |
| 1 | Module MCP2515/TJA1050 | alimentation 5 V, quartz 8 MHz, CAN 1 Mbit/s | Choisir une terminaison 120 Ω désactivable par cavalier |
| 1 | Module EPLZON TXS0108E | 8 canaux, broches `VA`, `VB`, `OE`, `A1..A8`, `B1..B8` | Conversion SPI, INT et données WS2812 |
| 23 utilisés | [Mini-PCB BTF-LIGHTING WS2812B ECO](https://www.amazon.fr/dp/B088K6C7TJ) | 5 V, 800 kHz, ordre GRB, conditionnement de 100 pièces | Un mini-PCB par voyant ; `C1` et `R1=75 Ω` intégrés |
| 1 | Potentiomètre linéaire | 10 kΩ | Commande `BRT` |
| 1 | Poussoir momentané normalement ouvert | contact sec | Commande `LAMP TEST` active bas |

### Alimentation et protection des pixels

| Qté | Composant | Caractéristiques requises |
|---:|---|---|
| 1 | Alimentation régulée | 5 V, 2 A minimum ; 3 A conseillé pour garder de la marge |
| 1 | Condensateur électrolytique | environ 1000 µF, tension nominale 10 V ou plus |
| 1 | Résistance de données | 330 à 470 Ω, 1/4 W |
| 1 | Connecteur d'alimentation | adapté à l'alimentation 5 V choisie et à au moins 2 A |

### Câblage et assemblage

| Qté | Élément | Usage |
|---:|---|---|
| 1 | Paire torsadée | `CANH/CANL` entre WARN CTRL et le bus CAN |
| selon besoin | Fil 20 à 22 AWG | distribution `+5V/GND` des pixels |
| selon besoin | Fil 24 à 28 AWG | SPI, INT, bouton, potentiomètre et données LED |
| selon besoin | Barrettes mâles/femelles au pas de 2,54 mm | modules ESP32, TXS0108E et MCP2515 |
| 1 | Plaque de prototypage ou circuit de distribution | maintien mécanique et distribution des signaux |
| 1 | Câble USB de données | alimentation et programmation de l'ESP32 |

Le Raspberry Pi et son Waveshare 2-CH CAN HAT+ constituent l'autre extrémité
du bus et ne sont pas comptés dans cette BOM côté ESP32. Les résistances de
terminaison CAN séparées ne sont pas nécessaires si le Waveshare et le module
MCP2515 possèdent chacun leur terminaison 120 Ω activable.

Ne pas faire passer le courant d'alimentation des 23 pixels dans des fils
Dupont ou dans les rails d'une breadboard sans soudure. Ils restent adaptés aux
signaux et au montage logique basse puissance.

## Broches utilisées par le firmware

| Fonction | ESP32 |
|---|---:|
| SPI SCK | GPIO18 |
| SPI MISO | GPIO19 |
| SPI MOSI | GPIO23 |
| MCP2515 CS | GPIO5 |
| MCP2515 INT | GPIO4 |
| Données WS2812 | GPIO13 |
| Potentiomètre BRT | GPIO34 |
| Poussoir LAMP TEST | GPIO27 |

## 1. Alimentations et masse commune

Pour le premier montage :

- alimenter l'ESP32 par son connecteur USB ;
- alimenter le MCP2515 et le côté 5 V du TXS0108E depuis la broche `5V/VIN`
  de l'ESP32 ;
- alimenter les 23 WS2812 avec une alimentation régulée 5 V séparée, capable
  de fournir au moins 2 A ;
- relier ensemble les masses de l'ESP32, du TXS0108E, du MCP2515, des WS2812
  et du Waveshare/Raspberry Pi.

Ne pas alimenter la chaîne de 23 WS2812 depuis la broche `3V3` ou la sortie
USB de l'ESP32. Ne pas relier la sortie positive de l'alimentation LED à la
broche `5V/VIN` de l'ESP32 lorsqu'il est également alimenté par USB : seule la
masse est commune.

## 2. Alimentation du TXS0108E

| TXS0108E | Connexion |
|---|---|
| `VA` / `VCCA` | ESP32 `3V3` |
| `VB` / `VCCB` | ESP32 `5V/VIN` |
| `OE` | ESP32 `3V3` |
| `GND` | masse commune |

Le côté `A` est toujours le domaine logique 3,3 V de l'ESP32. Le côté `B` est
le domaine logique 5 V du MCP2515 et des WS2812.

## 3. SPI et interruption du MCP2515

| ESP32 | TXS0108E | MCP2515 |
|---|---|---|
| GPIO18 | `A1` vers `B1` | `SCK` |
| GPIO23 | `A2` vers `B2` | `SI` / `MOSI` |
| GPIO19 | `A3` vers `B3` | `SO` / `MISO` |
| GPIO5 | `A4` vers `B4` | `CS` |
| GPIO4 | `A5` vers `B5` | `INT` |

Alimenter directement le MCP2515 :

| MCP2515 | Connexion |
|---|---|
| `VCC` | ESP32 `5V/VIN` |
| `GND` | masse commune |

Le courant d'alimentation du MCP2515 ne traverse pas le TXS0108E. Garder les
liaisons SPI aussi courtes que possible, idéalement sous 10 à 15 cm.

## 4. Chaîne de mini-PCB WS2812B

Utiliser le sixième canal du TXS0108E pour porter le signal de données à 5 V :

| ESP32 | TXS0108E | Premier mini-PCB |
|---|---|---|
| GPIO13 | `A6` vers `B6` | `DIN` du premier pixel |

Ajouter une résistance série de 330 à 470 Ω entre `B6` et `DIN`, au plus près
du premier pixel.

### Brochage d'un mini-PCB

Vu de l'arrière, inscriptions lisibles et flèche orientée de gauche à droite :

| Rangée | Côté entrée, à gauche | Côté sortie, à droite |
|---|---|---|
| haut | `+5V` | `+5V` |
| milieu | `DIN` | `DOUT` |
| bas | `GND` | `GND` |

La flèche indique le sens des données, pas le sens du courant d'alimentation.
Chaîner uniquement les données dans cet ordre :

```text
TXS0108E B6 -- 330 à 470 Ω -- DIN LED 0
DOUT LED 0 ------------------ DIN LED 1
DOUT LED 1 ------------------ DIN LED 2
...
DOUT LED 21 ----------------- DIN LED 22
```

Respecter l'ordre physique des 23 voyants défini par le firmware, de
`MASTER WARNING` à `WARN CTRL`.

### Alimentation et réinjection

Chaîner également `+5V` et `GND` d'un mini-PCB au suivant, puis réinjecter la
même alimentation 5 V aux LED 0, 10 et 20 :

```text
Alimentation 5 V
   +---- +5V/GND LED 0  ---- LED 1  ---- ... ---- LED 9
   +---- +5V/GND LED 10 ---- LED 11 ---- ... ---- LED 19
   +---- +5V/GND LED 20 ---- LED 21 ---- LED 22
```

Toutes les injections proviennent de la même alimentation. Réinjecter à chaque
fois les deux conducteurs `+5V` et `GND`, avec du fil 20 à 22 AWG. Ne pas
couper les liaisons d'alimentation entre les mini-PCB et ne jamais reboucler
`DOUT` de la LED 22 vers la LED 0.

Relier :

- le `+5V` des pixels à l'alimentation LED 5 V séparée ;
- le `GND` des pixels à cette alimentation et à la masse commune ;
- `DOUT` de chaque pixel à `DIN` du suivant, dans le sens des flèches ;
- exactement 23 pixels dans la chaîne.

Un condensateur électrolytique d'environ 1000 µF entre `+5V` et `GND`, près du
premier pixel, est recommandé. Respecter sa polarité. Chaque mini-PCB possède
déjà son condensateur de découplage `C1` et une résistance de données `R1` de
75 Ω ; aucun condensateur de 100 nF séparé n'est nécessaire. Conserver malgré
tout la résistance externe de 330 à 470 Ω avant le premier `DIN`.

## 5. Potentiomètre BRT

Utiliser un potentiomètre linéaire de 10 kΩ :

| Potentiomètre | Connexion |
|---|---|
| borne extérieure 1 | ESP32 `3V3` |
| curseur central | ESP32 GPIO34 |
| borne extérieure 2 | masse commune |

Si le sens de rotation est inversé, permuter uniquement les deux bornes
extérieures. Ne jamais appliquer 5 V à GPIO34.

## 6. Poussoir LAMP TEST

| Poussoir | Connexion |
|---|---|
| borne 1 | ESP32 GPIO27 |
| borne 2 | masse commune |

Le firmware active la résistance de rappel interne. Le bouton est actif bas :
les lampes de test sont allumées tant que le bouton relie GPIO27 à la masse.

Pour un poussoir à quatre pattes, les deux pattes d'un même côté sont déjà
reliées entre elles. Utiliser une patte de chaque côté du contact.

## 7. Bus CAN vers le Waveshare

| MCP2515 | Waveshare Raspberry Pi |
|---|---|
| `CANH` | `CAN0-H` |
| `CANL` | `CAN0-L` |
| `GND` | masse commune |

Utiliser une paire torsadée pour `CANH/CANL`. Le bus doit former une ligne et
porter exactement deux terminaisons de 120 Ω, une à chaque extrémité :

```text
Waveshare [120 Ω] ---- nœuds intermédiaires ---- WARN CTRL [120 Ω]
```

Pour le premier essai à deux nœuds, activer la terminaison du canal CAN0 du
Waveshare et celle du MCP2515. Sur un bus étendu, retirer le cavalier de
terminaison de tous les modules intermédiaires et le conserver uniquement sur
le dernier nœud.

## 8. Contrôles avant mise sous tension

1. Vérifier qu'aucun fil 5 V n'arrive sur une broche GPIO ou sur `VA`.
2. Vérifier `VA = 3,3 V`, `VB = 5 V` et `OE = 3,3 V` dans le schéma de câblage.
3. Vérifier la continuité de toutes les masses.
4. Vérifier que `CANH` ne va qu'à `CANH` et `CANL` qu'à `CANL`.
5. Hors tension, mesurer environ 60 Ω entre `CANH` et `CANL`.
6. Vérifier la polarité de l'alimentation WS2812 et du condensateur.
7. Pour le premier démarrage, laisser éventuellement la chaîne WS2812
   débranchée et valider d'abord la communication CAN.

Après mise sous tension et démarrage des deux nœuds, `can0` doit revenir à
`ERROR-ACTIVE`, ses compteurs RX/TX doivent progresser et AV CORE doit afficher
`WARN CTRL` en ligne.
