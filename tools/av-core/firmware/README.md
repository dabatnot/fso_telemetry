# Firmware AV CORE

Le projet produit deux firmwares fixes pour ESP32 DevKit V1 : `warn_ctrl`, avec
23 WS2812 et les commandes physiques, et `threat_proc`, avec huit secteurs et
un neuvième pixel `LOCK`. Les deux utilisent un MCP2515/TJA1050 8 MHz à
1 Mbit/s. `WARN CTRL` publie la luminosité et les tests effectifs appliqués par
`THREAT PROC`.

## Câblage du prototype

Les guides détaillés sont [`WIRING.md`](WIRING.md) pour `WARN CTRL` et
[`THREAT_PROC_WIRING.md`](THREAT_PROC_WIRING.md) pour l'indicateur de menace.

| Fonction | GPIO |
|---|---:|
| VSPI SCK / MISO / MOSI | 18 / 19 / 23 |
| MCP2515 CS / INT | 5 / 4 |
| Données WS2812 | 13 |
| Potentiomètre BRT | 34 |
| Poussoir LAMP TEST actif bas | 27 |

Le MCP2515/TJA1050 5 V nécessite une adaptation push-pull 3,3/5 V pour SPI et
INT. Le prototype utilise un TXS0108E, également employé pour porter à 5 V le
signal de données de la chaîne WS2812. Celle-ci possède sa propre alimentation
5 V correctement protégée et partage la masse commune.

```sh
pio test -e native
pio run -e warn_ctrl
pio run -e threat_proc
pio run -e warn_ctrl -t upload
pio run -e threat_proc -t upload
```
