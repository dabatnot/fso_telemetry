# Spécifications actives de télémétrie

Ce dossier contient les spécifications de phase actives :

- [Phase 0 — contrat de protocole](0-Contrat-de-protocole/README.md) ;
- [Phase 1 — squelette et premier flux](1-Squelette-et-premier-flux/README.md) ;
- [Phase 2 — vaisseau complet](2-Vaisseau-complet/README.md) ;
- [Phase 3 — ciblage et capteurs](3-Ciblage-et-capteurs/README.md).

## Décision produit irréversible

`TrustedFullState` est abandonné définitivement. Il ne fait partie ni du
produit, ni du protocole accepté, ni de la configuration, ni d'une phase
future. Une évolution ajoute uniquement les données nécessaires au simpit
dans la projection `CockpitSensors`, après filtrage producteur. Les besoins du
dashboard, du replay, des tests ou du diagnostic ne peuvent pas élargir ce
périmètre.

## Processus de développement

Les documents historiques `01..07` décrivent les comportements existants, mais
ils ne constituent ni un système de preuve ni une autorisation de travailler.
Leurs tables de tests sont des indications utiles, jamais des gates.

Une nouvelle évolution peut être documentée dans un simple `README.md`. Les
tests unitaires pertinents et une vérification manuelle en jeu suffisent. Le
propriétaire du projet peut commencer, réordonner, sauter ou explorer une phase
à tout moment, y compris lorsqu'un test connu reste rouge.

Il n'existe aucun tracker de phase, aucune empreinte de fichiers et aucun état
de complétude à maintenir.

Les documents racine de [`documentation/analysis`](../README.md) fixent les
invariants communs et la terminologie. Les schémas wire et golden vectors
versionnés restent l’autorité pour les octets. Les roadmaps, exemples
prospectifs et archives sont non normatifs.
