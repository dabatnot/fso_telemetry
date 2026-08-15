# Tests manuels de télémétrie

Ce dossier contient des missions et configurations pratiques pour tester la
télémétrie dans le vrai jeu. Leur utilisation est facultative et ne produit
aucun rapport, preuve, score ou statut de phase.

## Test rapide `CoreGate`

1. Copier `telemetry_p2_core.fs2` dans les missions du mod de test.
2. Utiliser `core-gate.telemetry.json` comme `data/config/telemetry.json`.
3. Lancer la mission puis le client console ou le dashboard.
4. Vérifier que les données du joueur apparaissent et que l'arrêt est propre.

## Test rapide `CompleteShip`

1. Copier `telemetry_p2_complete.fs2` dans les missions du mod de test.
2. Utiliser `complete-ship.telemetry.json` comme configuration.
3. Lancer la mission, cibler `External Cargo 1` et demander le support.
4. Vérifier les données cockpit attendues, puis arrêter et relancer la mission.

Le propriétaire du projet décrit simplement ce qu'il observe et décide si le
résultat lui convient. Aucune checklist ou conservation de transcript n'est
requise.
