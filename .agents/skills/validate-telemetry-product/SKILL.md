---
name: validate-telemetry-product
description: Lancer les tests automatisés pertinents de la télémétrie FS2Open et aider l'utilisateur à effectuer une vérification manuelle en jeu. Utiliser ce skill sans produire de preuve, tracker, score, gate ou décision automatique.
---

# Tester la télémétrie

## Principe

La validation se limite aux tests automatisés ordinaires et au test manuel
contrôlé par l'utilisateur. Elle n'autorise ni n'interdit une phase suivante.
L'utilisateur peut poursuivre malgré un résultat rouge après avoir été informé
du risque.

Ne jamais créer ou mettre à jour de tracker, registre de preuves, empreinte de
fichiers, rapport de certification, score ou statut de complétude.

## Tests automatisés

1. Lire `AGENTS.md` et vérifier qu'aucun autre build ou runner n'est actif.
2. Exécuter le test le plus étroit lié au changement.
3. Utiliser `telemetry-short` pour une vérification transverse lorsque cela est
   utile, pas comme gate obligatoire.
4. Exécuter les tests de schéma et golden vectors si le wire a changé.
5. Rapporter les commandes, succès et échecs dans la réponse uniquement.

## Test manuel

Proposer une courte manipulation en jeu centrée sur le comportement modifié.
L'utilisateur lance le jeu, décrit ce qu'il observe et décide si le résultat lui
convient. Aucun formulaire ou compte rendu versionné n'est requis.

## Résultat

Résumer les tests passés, les tests rouges, le retour manuel disponible et les
risques connus. Ne jamais bloquer une autre tâche de sa propre initiative.
