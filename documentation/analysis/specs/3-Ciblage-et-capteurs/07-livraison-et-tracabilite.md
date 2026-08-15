# 07 — Résumé et tests

## Contenu

La Phase 3 livre `CockpitSensors` (`0x07CB`) : cible, locks, radar, contacts,
menaces, alertes HUD, scan cargo et navigation visibles depuis le cockpit. Les
exigences détaillées restent dans le document [01](01-cadre-normatif-et-perimetre.md).

## Tests utiles

- tests unitaires Phase 3 directement concernés ;
- tests du radar et du dashboard lorsque leur affichage change ;
- `telemetry-short` lorsqu'une vérification transverse est utile ;
- tests wire si un record ou un masque change.

Le test manuel consiste à vérifier en mission les données cockpit modifiées sur
le radar, le dashboard ou le périphérique simpit concerné.

## Décision

Il n'existe aucun tracker, preuve à enregistrer, gate ou statut de complétude.
Le propriétaire du projet peut poursuivre, réordonner ou explorer une autre
phase quel que soit l'état courant des tests, après information sur les risques.
