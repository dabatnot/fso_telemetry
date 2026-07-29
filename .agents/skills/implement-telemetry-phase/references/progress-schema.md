# Tracker persistant v2

Le tracker canonique est `documentation/analysis/progress/phase-<N>.json`.

## Champs racine obligatoires

```json
{
  "schema": "fs2open.telemetry.phase-progress.v2",
  "phaseNumber": 2,
  "title": "Phase 2 — ...",
  "workflowContractVersion": 2,
  "profile": "balanced",
  "executionState": "paused",
  "updatedAtUtc": "2026-07-29T00:00:00Z",
  "source": {},
  "current": {},
  "campaign": {},
  "budget": {},
  "statusBuckets": {},
  "blockers": [],
  "evidence": [],
  "nextActions": [],
  "decisionHistory": [],
  "notes": []
}
```

## État courant

`current` contient :

- `workPackage`;
- `gate`;
- `activity`;
- `summary`;
- `nextAction`;
- `startedAtUtc`;
- `lastHeartbeatAtUtc`.

## Campagne

`campaign` contient :

- `id`;
- `state`: `not-started`, `qualified`, `running`, `paused`, `failed`, `complete`;
- `completed`;
- `total`;
- `unit`;
- `startedAtUtc`;
- `updatedAtUtc`;
- `estimatedRemainingMinutes`.

Hors campagne, utiliser `id: null`, `state: not-started`, `completed: 0`, `total: 0`.

## Budget

`budget` contient :

- `limitMinutes`;
- `plannedMinutes`;
- `consumedMinutes`;
- `remainingMinutes`;
- `exceptionDecisionId`.

## Blocker

Chaque blocker actif contient :

- `id`;
- `state`;
- `category`;
- `owner`;
- `summary`;
- `affects`;
- `nextAction`;
- `consecutiveFailures`.

## Preuve

Chaque preuve conclusive contient :

- `id`, `kind`, `state`, `path`, `command`, `observedAtUtc`;
- `requirements`, `acceptanceCriteria`, `gate`;
- `reuseKey`, `invalidationScope`;
- `fingerprint` avec les six dimensions de la politique d’exécution ;
- `note`.

Les anciennes preuves peuvent conserver leurs champs v1. La migration est additive et ne réécrit pas l’historique.

## Historique de décision

Conserver uniquement les transitions de gate, migrations, pauses, reprises et changements de blocker :

```json
{
  "atUtc": "...",
  "kind": "migration",
  "subject": "workflow-v2",
  "decision": "preserve",
  "reason": "..."
}
```
