---
name: implement-latest-telemetry-phase
description: Implémenter ou explorer une phase de télémétrie FS2Open choisie par l'utilisateur. Utiliser ce skill pour modifier le produit, ajouter les tests unitaires utiles et préparer une vérification manuelle, sans tracker, preuve, gate ni blocage entre phases.
---

# Implémenter une phase de télémétrie

## Règle principale

La décision de l'utilisateur prévaut. Il peut commencer, poursuivre, réordonner,
sauter ou explorer une phase, même si une phase précédente ou des tests sont
incomplets. Signaler clairement les risques connus, puis suivre sa décision.

Ne jamais créer de tracker, registre de preuves, empreinte de fraîcheur, score,
gate, statut de complétude ou workflow de certification.

## Travailler

1. Lire `AGENTS.md` et vérifier `git status --short`.
2. Identifier la phase ou l'expérience demandée. Si elle n'est pas précisée,
   utiliser la dernière phase active pertinente.
3. Lire uniquement la spécification et les fichiers produit nécessaires.
4. Préserver le wire existant, le filtrage cockpit, les bornes et le caractère
   non bloquant du chemin jeu.
5. Implémenter le plus petit changement cohérent répondant au besoin simpit.

`TrustedFullState` reste définitivement exclu. Toute nouvelle donnée doit servir
un besoin cockpit concret et passer par `CockpitSensors` avec filtrage producteur.

## Vérifier

- Respecter le propriétaire unique des builds et tests défini par `AGENTS.md`.
- Exécuter les tests unitaires ou d'intégration directement concernés.
- Exécuter les contrôles wire existants seulement si le protocole change.
- Laisser à l'utilisateur le lancement du jeu et la vérification manuelle.
- Un test rouge est rapporté avec son impact. Il n'interdit pas de poursuivre si
  l'utilisateur décide explicitement de le faire.

Ne recopier aucun résultat dans les sources. Les sorties du runner et le retour
manuel de l'utilisateur suffisent.

## Rapporter

Indiquer simplement : comportement modifié, fichiers principaux, tests lancés,
résultats, vérification manuelle restante et risques connus.
