---
name: specify-next-telemetry-phase
description: Définir simplement une prochaine évolution de télémétrie FS2Open à partir du besoin simpit choisi par l'utilisateur. Utiliser ce skill pour proposer ou documenter une phase sans auditer ni bloquer sur les phases précédentes.
---

# Définir la prochaine évolution

## Règle principale

La roadmap aide à proposer un sujet, mais l'utilisateur choisit la direction.
Il peut démarrer une phase exploratoire, sauter un numéro, revenir en arrière ou
continuer malgré des tests rouges. Aucune phase précédente n'est un gate.

Ne jamais créer de tracker, preuve, empreinte, score, statut de complétude,
certification ou mécanisme d'autorisation automatique.

## Choisir le périmètre

1. Lire `AGENTS.md`, `documentation/analysis/README.md` et l'index des specs.
2. Partir d'un besoin cockpit ou simpit concret exprimé par l'utilisateur.
3. Vérifier que la capacité n'expose aucune information cachée de mission.
4. Préférer une petite évolution testable à une architecture prospective.
5. Si le besoin n'est pas assez précis pour choisir entre deux produits
   différents, présenter brièvement ce choix à l'utilisateur.

`TrustedFullState` ne peut jamais être proposé ou réintroduit.

## Documenter

Par défaut, une nouvelle phase tient dans un seul `README.md` contenant :

- le résultat attendu ;
- le périmètre et les exclusions ;
- les impacts wire éventuels ;
- quelques comportements vérifiables ;
- les décisions produit encore ouvertes.

Ajouter un document séparé uniquement si le détail technique l'exige réellement.
Ne pas imposer la structure historique `01..07`, une table de traçabilité ou une
preuve par exigence.

## Validation attendue

Les tests unitaires pertinents et une vérification manuelle en jeu suffisent.
Ils informent l'utilisateur et ne conditionnent jamais la création ou le début
d'une autre phase.
