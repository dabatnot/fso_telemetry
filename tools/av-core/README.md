# FSO SimPit AV CORE

`AV CORE` est le service local du Raspberry Pi du simpit. Le lot 2 relie
l'application Web au profil cockpit FSTL de FS2Open et calcule en direct les
warnings, cautions, secteurs de missiles et états de verrouillage. SocketCAN et
les calculateurs ESP32 restent explicitement indisponibles jusqu'aux lots
suivants.

Le client accepte exclusivement une session `CockpitSensors` avec la couverture
`0x07CB`. Toute autre couverture est rejetée avant publication d'un état `LIVE`.

## Développement sous Windows

Prérequis : Python 3.11+, Node.js 20+ et npm.

```powershell
.\tools\av-core\start-av-core.ps1
```

Le script crée l'environnement Python et les fichiers générés sous
`build/av-core`, construit le frontend puis lance le service sur
`http://127.0.0.1:8080`. Il se connecte par défaut à FSTL sur
`127.0.0.1:42042` sans modifier la configuration de FS2Open.

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
active `av-core.service`. Il installe également les deux sources communes du
client FSTL dans `/opt/fsotelemetry/av-core/fstl-client`. La configuration
persistante se trouve dans `/var/lib/fsotelemetry/av-core.json`.

Commandes utiles :

```bash
systemctl status av-core.service
journalctl -u av-core.service -f
sudo systemctl restart av-core.service
```

AV CORE est accessible par `http://<ip-du-raspberry>:8080`. L'alias mDNS
`av-core.local` sera finalisé avec le packaging du lot 6.

L'interface est disponible en français, anglais, espagnol, portugais, italien
et allemand. La liste déroulante de la barre supérieure mémorise le choix dans
le navigateur et pourra accueillir d'autres langues sans modifier le composant
d'interface.

## États FSTL

- `PRÊT` : le transport FSTL est préchauffé au menu ou au briefing, sans donnée cockpit ;
- `LIVE` : un état cockpit valide progresse et les alertes sont calculées ;
- `PAUSE` : la connexion reste `LIVE`, les heartbeats maintiennent la session et les alertes restent figées ;
- `STALE` : les données ne progressent plus depuis le délai configuré, toutes
  les alertes cockpit sont immédiatement effacées et une resynchronisation est
  demandée ;
- `DÉCONNECTÉ` : aucune session durable n'est disponible ou le producteur a
  terminé sa session.

Après une seconde supplémentaire sans aucun trafic valide, l'état de session est recréé sur
le même endpoint UDP avec un nouveau nonce et une nouvelle négociation commence
automatiquement. Tant que le jeu ne répond pas, AV CORE retransmet ce même
`HELLO`, sans générer de nouveaux nonces à chaque expiration de la fenêtre
fiable. Le socket n'est recréé qu'après une erreur réseau locale ou un
changement d'adresse. Avec le délai de fraîcheur par
défaut, une pause de mission ne coupe plus la session : la progression cockpit
est suspendue mais le transport reste surveillé. Seule une absence totale de
trafic pendant deux secondes déclenche une nouvelle session. Une modification de l'hôte, du port
ou du délai FSTL applique le même redémarrage ciblé du client.
Une fois le nouveau `WELCOME` accepté, `SESSION_BEGIN`, le manifeste et le
snapshot initial conservent leur fenêtre fiable complète de cinq secondes ; la
politique `1 s + 1 s` ne crée pas un autre nonce pendant cette reconstruction.
Hors mission, `WELCOME` place AV CORE en `PRÊT`. L'entrée en mission réutilise
ce transport préchauffé ; la reprise après pause reçoit un keyframe immédiat.

Pour utiliser simultanément AV CORE, le dashboard et le radar, configurer le
producteur FS2Open avec `maxClients: 4`. Le défaut moteur reste volontairement à
un client afin de ne pas préallouer quatre slots sur toutes les installations.

## Vérification manuelle du lot 2

1. Démarrer AV CORE avant FS2Open et vérifier que le Web reste disponible avec
   FSTL déconnecté et CAN indisponible.
2. Démarrer une mission et vérifier le passage à `LIVE`, les warnings,
   cautions, secteurs de missiles et états de lock disponibles.
3. Modifier un seuil dans la page Alertes et vérifier son application immédiate.
4. Mettre le jeu en pause, reprendre avant puis après deux secondes, changer de
   mission et redémarrer le jeu : AV CORE doit revenir seul à `LIVE`.
5. Redémarrer `av-core.service` et vérifier la conservation de la configuration.

## Limites du lot 2

- aucun accès SocketCAN ;
- aucun firmware ESP32 ;
- aucun test physique de voyant ;
- aucun profil de luminosité nocturne ;
- aucune authentification ou exposition à Internet.
