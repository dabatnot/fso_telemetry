# 07 — Résumé et tests

## Contenu

La Phase 2 livre `CoreGate` (`0x0401`) et `CompleteShip` (`0x0583`) sur le même
pipeline : état complet du vaisseau joueur, armement, énergie, dégâts,
sous-systèmes et relations cockpit autorisées. Les exigences détaillées restent
dans le document [01](01-cadre-normatif-et-perimetre.md).

## Tests utiles

- tests unitaires et d'intégration Phase 2 directement concernés ;
- `telemetry-short` lorsqu'une vérification transverse est utile ;
- tests wire si un record ou un masque change.

Le test manuel consiste à comparer quelques valeurs cockpit représentatives et
à vérifier arrêt, reprise et changement de mission.

## Décision

Il n'existe aucun tracker, preuve à enregistrer, gate ou statut de complétude.
Un test rouge n'interdit pas de poursuivre après décision explicite du
propriétaire du projet.
