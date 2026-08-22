# Tests manuels de télémétrie

Ce dossier contient des missions et configurations pratiques pour tester la
télémétrie dans le vrai jeu. Leur utilisation est facultative et ne produit
aucun rapport, preuve, score ou statut de phase.

## Test rapide `CockpitSensors`

1. Copier `telemetry_p3_cockpit_sensors.fs2` dans les missions du mod de test.
2. Utiliser une configuration `schemaVersion=4` sans clé de profil, par exemple
   celle de `tools/radar/examples/fs2open.telemetry.json`.
3. Lancer la mission puis le client radar ou le dashboard.
4. Vérifier que les informations distantes correspondent au cockpit, qu'aucun
   objet caché n'apparaît et que l'arrêt est propre.

Le propriétaire du projet décrit simplement ce qu'il observe et décide si le
résultat lui convient. Aucune checklist ou conservation de transcript n'est
requise.
