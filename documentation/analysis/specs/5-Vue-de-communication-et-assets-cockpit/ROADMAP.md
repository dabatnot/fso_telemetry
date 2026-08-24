# Roadmap de livraison de la Phase 5

Cette roadmap découpe la vue de communication en lots verticaux. Elle guide
l'implémentation sans constituer un gate, une preuve ou une obligation d'ordre.
Les tests utiles sont rapportés directement et une vérification manuelle courte
en jeu suffit.

| Lot | Livraison visible | Vérification simple |
|---|---|---|
| 1 | Bundle reproductible à partir d'une pile de mods | Deux exécutions produisent le même manifeste et le même hash |
| 2 | Source `Talking Head` publiée par FS2Open | `START`, corrections et `STOP` apparaissent dans un client de référence |
| 3 | AV CORE négocie et projette la communication | Une connexion tardive rejoint le bon offset |
| 4 | Page Web Communications | Le portrait suit lecture, pause et remplacement |
| 5 | Installation et intégration complète | Bundle valide/invalide et redémarrages sont gérés proprement |

## Lot 1 — Packager et bundle local

Créer un packager utilisant la résolution CFile réelle et la pile de mods du
jeu.

Il livre :

- sélection d'un périmètre de missions ou de mods explicite ;
- résolution des variantes finales susceptibles d'être utilisées ;
- exclusion des fichiers masqués par la priorité CFile ;
- décodage des sources ANI, EFF, APNG et images statiques ;
- conversion reproductible des animations en APNG et des images statiques en PNG ;
- conservation des dimensions, de l'alpha et des timings ;
- `manifest.json` conforme au contrat FSTL ;
- `bundle-info.json` avec la table locale source vers `asset_id` ;
- archive `communication-bundle-<hash>.tar.gz` ;
- sortie atomique et refus d'un bundle partiel.

Le packager est un outil de développement/livraison. Il n'est jamais exécuté
dans la frame du jeu ni sur le Raspberry pendant une session.

Les tests couvrent au minimum la priorité loose/VP, les variantes de même nom,
les conversions, les timings non uniformes, les chemins portables, les
collisions et la reproductibilité.

L'interface initiale est l'exécutable C++ `comm_bundle_packager`, activé par
`FSO_BUILD_TOOLS`. Il exige `--fs2-root`, `--output` et soit une ou plusieurs
options `--mission`, soit `--all-missions`. Il accepte la pile `--mod`, les
métadonnées diagnostiques et les assets statiques facultatifs de cadre et de
placeholder. Le répertoire de sortie ne reçoit que l'archive validée ; le jeu
n'est jamais lancé par cet outil.

## Lot 2 — Producteur FS2Open

Ajouter la configuration optionnelle du bundle et charger au démarrage une
table bornée de correspondance entre identité finale `generic_anim` et
`asset_id`.

Intégrer dans le gauge `Talking Head` un hook main-thread minimal qui copie
l'état autoritaire après le choix réel de l'asset et de l'offset.

Le runtime producteur livre :

- annonce conditionnelle de `COMM_VIEW_AUTHORITATIVE_SOURCE` ;
- sélection `COMM_VIEW_NEGOTIATION` dans `WELCOME` ;
- transaction fiable `COMM_ASSET_MANIFEST` ;
- `COMM_VIEW_EVENT START/STOP` fiable ;
- remplacement ordonné `STOP(REPLACED)` puis `START` ;
- `COMM_VIEW_STATE` immédiat sur changement et périodique à 10 Hz maximum ;
- état courant dans les keyframes ;
- arrêt sur fin de mission, désactivation HUD ou fin de session ;
- retrait de capability sans interrompre les autres domaines.

Le hook ne fait aucune I/O, aucun hash et aucun encodage. Les tests moteur
couvrent les transitions visibles, les assets inconnus, la pause, la vitesse
signée et les changements de mission.

## Lot 3 — Bundle et client FSTL dans AV CORE

Ajouter l'installation locale du bundle et son chargement sûr :

- `install-communication-bundle.sh <archive.tar.gz>` ;
- validation dans une zone temporaire ;
- installation versionnée sous `/var/lib/fsotelemetry` ;
- lien `current` remplacé atomiquement ;
- réinstallation idempotente ;
- conservation du bundle précédent si l'archive est invalide.

Étendre le client FSTL d'AV CORE pour :

- annoncer `COMM_VIEW_LOCAL_ASSETS` avec APNG et PNG ;
- encoder l'offre et décoder la sélection de bundle ;
- recevoir et installer atomiquement `COMM_ASSET_MANIFEST` ;
- appliquer `COMM_VIEW_EVENT` et `COMM_VIEW_STATE` ;
- projeter l'offset avec l'horloge de session ;
- gérer les six états de cycle de vie normatifs ;
- masquer immédiatement une lecture périmée ;
- reprendre après reconnexion ou keyframe.

Les tests couvrent les résultats de négociation, les manifestes fragmentés,
les u64, les événements anciens/dupliqués, la projection en boucle et lecture
inverse ainsi que l'indépendance du statut FSTL et du CAN.

## Lot 4 — API et page Web Communications

Étendre le statut AV CORE avec le sous-état de communication et réutiliser le
flux SSE existant pour les changements.

Ajouter une route d'asset bornée qui :

- accepte uniquement un `asset_id` du manifeste validé ;
- résout le chemin sous la racine canonique du bundle ;
- vérifie le type livré ;
- renvoie un type MIME fermé et un cache lié au hash ;
- retourne 404 sans fuite de chemin pour toute valeur inconnue.

La nouvelle page `Communications` affiche :

- la frame courante ou le placeholder ;
- le cadre recommandé s'il existe ;
- pleine couleur ou teinte locale ;
- l'état de la capability et le diagnostic du bundle ;
- un hash court utile à l'installation.

La page ne contient aucun contrôle de lecture. Elle recharge l'état courant
après reconnexion SSE, réveil ou retour d'onglet et saute directement à la frame
projetée.

Les tests frontend/backend couvrent les états disponibles, les transitions, le
choix de frame, le placeholder, le retour d'onglet et la protection des routes.

## Lot 5 — Livraison et intégration complète

Étendre le constructeur de release AV CORE avec l'installateur de bundle, sans
inclure d'asset de jeu.

Documenter :

- génération du bundle avec la pile de mods utilisée ;
- copie et activation côté FS2Open ;
- copie et installation sur le Raspberry ;
- lecture des hashes requis et installés ;
- remplacement et retour au bundle précédent ;
- diagnostics producteur et AV CORE.

La vérification manuelle courte couvre :

- démarrage d'AV CORE avant et après FS2Open ;
- communication simple et remplacement rapide ;
- pause, reprise et compression temporelle ;
- connexion en cours de lecture ;
- changement de mission et redémarrage du jeu ;
- bundle absent, hash différent et asset local supprimé ;
- continuité de la télémétrie et du bus CAN dans chaque cas d'erreur visuelle.

## Hypothèses retenues

- Le contrat FSTL existant est suffisant et n'est pas étendu.
- Le producteur et AV CORE utilisent exactement le même bundle hashé.
- APNG/PNG est l'unique représentation livrée requise dans la première version.
- Un seul bundle est actif à la fois sur AV CORE.
- L'installation du bundle se fait hors session et un changement prend effet à
  la session suivante.
- Le Raspberry dispose de l'espace nécessaire aux assets du périmètre choisi ;
  aucune synchronisation réseau automatique n'est ajoutée.
- Le rendu Web reste local au navigateur et ne produit aucune nouvelle trame
  CAN.
