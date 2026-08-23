# Firmware AV CORE

Le premier environnement PlatformIO est `warn_ctrl` pour un ESP32 DevKit V1.
Il utilise un MCP2515/TJA1050 8 MHz à 1 Mbit/s et une chaîne de 23 WS2812.

## Câblage du prototype

Le guide détaillé de montage, d'alimentation et de contrôle est disponible dans
[`WIRING.md`](WIRING.md).

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
pio run -e warn_ctrl -t upload
```
