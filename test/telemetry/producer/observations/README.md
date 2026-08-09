# Observations Release Phase 2

Ce dossier contient uniquement les entrées reproductibles des quatre observations
produit. Les transcripts, sorties de tests et checklists remplis sont écrits
dans le build ou dans les artifacts CI, jamais ici.

## Préparation commune

1. Construire le produit et les tests en Release.
2. Créer `build/telemetry-observations`.
3. Copier la mission indiquée ci-dessous dans le dossier de missions du mod de
   test.
4. Copier la configuration du profil choisi sous
   `data/config/telemetry.json`.
5. Démarrer la mission avec le vrai exécutable Release. Le lancement du jeu et
   toute décision de pare-feu restent sous contrôle humain.
6. Démarrer le client indiqué depuis la racine du dépôt.

## P2-OBS-01 — CoreGate, 60 secondes

Utiliser `telemetry_p2_core.fs2` et
`core-gate.telemetry.json`, puis exécuter :

```powershell
python test/telemetry/protocol/tools/fstl_console_client.py `
  --host 127.0.0.1 --port 42042 --seconds 60 `
  *> build/telemetry-observations/core-gate.transcript.txt
```

Attendus : masque `0x0401`, manifeste avant le premier snapshot, cardinalité
`9 + N` sans record CompleteShip, snapshot puis delta, arrêt propre.

## P2-OBS-02, P2-OBS-04 et P2-OBS-05 — CompleteShip, 120 secondes

Utiliser `telemetry_p2_complete.fs2` et
`complete-ship.telemetry.json`. Lancer immédiatement la mission et le client.
Avant 60 secondes, cibler `External Cargo 1`, demander le support, puis attendre
que le vaisseau de support soit docké au joueur.

```powershell
python test/telemetry/protocol/tools/fstl_console_client.py `
  --host 127.0.0.1 --port 42042 --seconds 120 --drop-once delta `
  *> build/telemetry-observations/complete-ship.transcript.txt
```

Attendus communs :

- masque `0x0583` et catalogue complet décodé indépendamment ;
- support et relation de docking présents ;
- cible externe `NOT_SCANNABLE/HIDDEN`, sans extension de fermeture ni fin de
  session ;
- perte unique annoncée par le client, puis convergence au delta cumulatif ou à
  la keyframe suivante ;
- arrêt puis redémarrage propres.

Les changements de générations de manifeste, leur staging, leur promotion et
leur coalescence sont vérifiés par les tests comportementaux du runtime. Ils ne
sont pas déduits de cette observation humaine. Le transcript rapporte des faits ;
il ne décide pas la livraison.

## P2-OBS-05 — relecture des bornes

La partie min/max/max+1, NaN, infini, cast impossible et budget exact/+1 reste
injectée par les tests courts qui appellent les validateurs de production, sans
seam dans le moteur :

```powershell
ctest --test-dir build -C Release --output-on-failure `
  -R "telemetry_phase2_(observation_contract|manifest|replication|security_bounds)_tests" -j 1 `
  *> build/telemetry-observations/phase2-bounds-tests.txt
```

Relire cette sortie avec le transcript CompleteShip. Consigner séparément les
rejets avant cast/allocation et la récupération après perte du delta.

## Redémarrage

Après l'arrêt propre de CompleteShip, relancer la mission et observer une
nouvelle session pendant 15 secondes :

```powershell
python test/telemetry/protocol/tools/fstl_console_client.py `
  --host 127.0.0.1 --port 42042 --seconds 15 `
  *> build/telemetry-observations/complete-ship-restart.transcript.txt
```

## Checklist humaine

Copier cette table dans
`build/telemetry-observations/release-observation-checklist.md` et la remplir.
Écrire `aucun` lorsqu'il n'existe ni écart ni impact.

| Observation | Attendu | Observé | Écart | Impact |
|---|---|---|---|---|
| P2-OBS-01 | CoreGate : masque, manifeste, snapshot, delta, arrêt | | | |
| P2-OBS-02 | CompleteShip : catalogue, support/docking, arrêt/restart | | | |
| P2-OBS-04 | cargo extérieur masqué, fermeture inchangée, session vivante | | | |
| P2-OBS-05 | bornes rejetées, perte unique, convergence, session récupérable | | | |

Ajouter `observedAtUtc` et `reviewedBy` au-dessus de la table. La checklist ne
calcule ni score, ni verdict, ni éligibilité. La décision de livraison reste
humaine.
