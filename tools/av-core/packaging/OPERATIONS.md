# Exploitation AV CORE sur Raspberry Pi

## Installation et mise à jour

Construire l'archive sur le poste de développement :

```text
python tools/av-core/packaging/build_release.py
```

Copier `build/av-core/av-core-<version>.tar.gz` sur le Raspberry, puis :

```bash
tar -xzf av-core-<version>.tar.gz
cd av-core-<version>
sudo ./install.sh
```

Pour attribuer explicitement le nom mDNS `av-core.local` au Raspberry :

```bash
sudo ./install.sh --set-hostname av-core
```

Une mise à jour réutilise la même commande. Elle installe une release complète
sous `/opt/fsotelemetry/av-core/releases`, bascule le lien `current` et conserve
toujours `/var/lib/fsotelemetry/av-core.json`.

## Services et journaux

```bash
systemctl status av-core.service av-core-can.service
journalctl -u av-core.service -f
journalctl -u av-core-can.service -f
sudo systemctl restart av-core.service
sudo systemctl restart av-core-can.service
ip -details -statistics link show can0
```

`av-core.service` continue de fonctionner si `av-core-can.service` échoue parce
que le HAT ou `can0` est absent. Le backend retente alors SocketCAN chaque
seconde. Le service CAN fixe le débit à 1 Mbit/s et demande au noyau de relancer
le contrôleur 100 ms après un état `bus-off`.

## Préparation du HAT CAN

L'installateur ne modifie pas `/boot/firmware/config.txt`. L'overlay adapté au
HAT doit créer l'interface `can0` avant le démarrage de `av-core-can.service`.
La référence exacte de l'overlay sera validée avec le matériel. Ne configurez
pas `can1` pour AV CORE.

## Vérification sans ESP32

Après installation, le Web doit être accessible et afficher CAN indisponible
si aucun HAT n'est présent. Une modification enregistrée doit rester dans
`/var/lib/fsotelemetry/av-core.json` après `systemctl restart av-core.service`
ou un reboot.

Sur une machine Linux sans interface `can0`, le bundle fournit un smoke test
optionnel et non destructif envers le matériel existant :

```bash
sudo ./smoke-test-vcan.sh
```

Le script refuse de s'exécuter dès qu'une interface réelle ou virtuelle
`can0` existe déjà.
