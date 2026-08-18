# FSO SimPit AV CORE

`AV CORE` est le service local du Raspberry Pi du simpit. Le lot 1 fournit
l'application Web de configuration et sa persistance. FSTL, SocketCAN et les
calculateurs ESP32 restent explicitement indiqués comme indisponibles jusqu'aux
lots suivants.

## Développement sous Windows

Prérequis : Python 3.11+, Node.js 20+ et npm.

```powershell
.\tools\av-core\start-av-core.ps1
```

Le script crée l'environnement Python et les fichiers générés sous
`build/av-core`, construit le frontend puis lance le service sur
`http://127.0.0.1:8080`. Il ne modifie pas la configuration de FS2Open.

Pour développer le frontend avec rechargement automatique, lancer le backend
puis exécuter séparément :

```powershell
Set-Location tools\av-core\frontend
npm ci
npm run dev
```

Vite écoute alors sur `http://127.0.0.1:5174` et transmet `/api` au backend.

## Tests

```powershell
python -m pip install -r tools\av-core\backend\requirements-dev.txt
python -m unittest discover -s tools\av-core\tests\backend -p "test_*.py"

Set-Location tools\av-core\frontend
npm ci
npm test
npm run build
```

## Installation sur Raspberry Pi OS Bookworm 64 bits

Depuis une copie du dépôt sur le Raspberry :

```bash
sudo bash tools/av-core/packaging/install.sh
```

L'installateur construit le frontend, installe le service dans
`/opt/fsotelemetry/av-core`, crée l'utilisateur système `fsotelemetry` et
active `av-core.service`. La configuration persistante se trouve dans
`/var/lib/fsotelemetry/av-core.json`.

Commandes utiles :

```bash
systemctl status av-core.service
journalctl -u av-core.service -f
sudo systemctl restart av-core.service
```

Le lot 1 est accessible par `http://<ip-du-raspberry>:8080`. L'alias mDNS
`av-core.local` sera finalisé avec le packaging du lot 6.

L'interface est disponible en français, anglais, espagnol, portugais, italien
et allemand. La liste déroulante de la barre supérieure mémorise le choix dans
le navigateur et pourra accueillir d'autres langues sans modifier le composant
d'interface.

## Limites du lot 1

- aucune connexion FSTL ;
- aucun accès SocketCAN ;
- aucun firmware ESP32 ;
- aucun test physique de voyant ;
- aucun profil de luminosité nocturne ;
- aucune authentification ou exposition à Internet.
