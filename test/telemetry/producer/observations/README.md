# Observations Release Phase 2

Ce dossier contient uniquement les entrées reproductibles des deux observations
produit. Les transcripts et checklists remplis sont écrits dans le build ou dans
les artifacts CI, jamais ici.

## Préparation commune

1. Construire le produit et les tests en Release.
2. Copier `telemetry_phase2_observation_solo.fs2` dans le dossier de missions du
   mod de test.
3. Copier la configuration du profil choisi sous
   `data/config/telemetry.json`.
4. Démarrer la mission avec le vrai exécutable Release.
5. Démarrer le client indépendant depuis la racine du dépôt.

## P2-OBS-01 — CoreGate

Commande client :

```powershell
python test/telemetry/protocol/tools/fstl_console_client.py `
  --host 127.0.0.1 --port 42042 --seconds 60 `
  *> build/telemetry-observations/core-gate.transcript.txt
```

Attendus : masque `0x0401`, manifeste avant le premier snapshot, cardinalité
`9 + N` sans record CompleteShip, snapshot puis delta, arrêt propre.

## P2-OBS-02 et P2-OBS-04 — CompleteShip

Dans la mission, cibler `External Cargo 1`, demander le support, puis attendre
que le vaisseau de support soit docké au player.

Commande client :

```powershell
python test/telemetry/protocol/tools/fstl_console_client.py `
  --host 127.0.0.1 --port 42042 --seconds 120 --drop-once delta `
  *> build/telemetry-observations/complete-ship.transcript.txt
```

Attendus : masque `0x0583`, catalogue complet décodé, support et relation de
docking présents, cible externe `NOT_SCANNABLE/HIDDEN` sans fin de session,
injection unique annoncée, convergence au delta cumulatif ou keyframe suivant,
arrêt puis redémarrage propres.

## Checklist humaine

Copier cette table dans
`build/telemetry-observations/release-observation-checklist.md` et la remplir.

| Observation | Attendu | Observé | Écart | Impact |
|---|---|---|---|---|
| CoreGate | masque, manifeste, snapshot, delta, arrêt | | | |
| CompleteShip | catalogue, support/docking, cargo masqué, perte unique, reprise, restart | | | |

La checklist ne calcule ni score, ni verdict, ni éligibilité. La décision de
livraison reste humaine.
