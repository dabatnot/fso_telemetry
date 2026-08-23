# Guide de câblage THREAT PROC — ESP32 DevKit V1

`THREAT PROC` utilise un ESP32 DevKit V1, un MCP2515/TJA1050 à quartz 8 MHz,
un convertisseur de niveau 3,3/5 V et neuf WS2812 alimentées en 5 V. Il ne
possède ni potentiomètre ni poussoir : `WARN CTRL` publie la luminosité et les
tests sur CAN.

## Broches

| Fonction | GPIO ESP32 |
|---|---:|
| VSPI SCK / MISO / MOSI | 18 / 19 / 23 |
| MCP2515 CS / INT | 5 / 4 |
| Données WS2812 | 13 |

Le câblage du MCP2515, du convertisseur de niveau, de l'alimentation 5 V et des
terminaisons CAN suit les mêmes règles électriques que [`WIRING.md`](WIRING.md).
Le nœud ne porte une résistance de terminaison de 120 Ω que s'il se trouve à
une extrémité physique du bus.

## Ordre de la chaîne WS2812

| Pixel | Direction / fonction | Bit CAN |
|---:|---|---:|
| 0 | Avant | 0 |
| 1 | Avant-droite | 1 |
| 2 | Droite | 2 |
| 3 | Arrière-droite | 3 |
| 4 | Arrière | 4 |
| 5 | Arrière-gauche | 5 |
| 6 | Gauche | 6 |
| 7 | Avant-gauche | 7 |
| 8 | `LOCK` central | — |

La chaîne part de GPIO13 vers `DIN` du pixel avant, suit l'ordre du tableau et
se termine sur `LOCK`. Chaque branche d'alimentation doit être protégée, les
masses doivent être communes et le signal 3,3 V doit être adapté au niveau 5 V
attendu par la première WS2812.
