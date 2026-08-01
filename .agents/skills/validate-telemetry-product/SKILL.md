---
name: validate-telemetry-product
description: Vérifier ou préparer la livraison du produit de télémétrie FS2Open à partir des spécifications actives des Phases 0, 1 et 2. Utiliser ce skill pour auditer la traçabilité des exigences, exécuter les preuves courtes, contrôler les profils CoreGate/CompleteShip, conduire les observations Release ou confirmer que l'ancien harness de certification ne revient pas.
---

# Valider le produit de télémétrie

## Autorités

1. Lire `AGENTS.md`.
2. Lire `documentation/analysis/README.md`, puis le `README.md` et le `07-livraison-et-tracabilite.md` de la phase concernée.
3. Lire seulement les autres documents actifs nécessaires à l'exigence contrôlée.
4. Traiter les schémas wire et golden vectors comme autorités binaires.
5. Traiter la roadmap, les exemples prospectifs et `documentation/analysis/archive` comme non normatifs.

En cas de divergence, corriger d'abord la spécification canonique, puis le code et les tests dans le même lot.

## Préserver le dépôt

- Inventorier `git status --short` avant toute modification.
- Préserver toutes les modifications existantes et étrangères.
- Ne lancer ni `reset`, ni `clean`, ni restauration destructive.
- Écrire les résultats dans le build ou les artifacts CI, jamais dans les sources.

## Prouver le contrat

1. Relier chaque exigence à sa ligne canonique dans le document 07 :
   - Phase 0 : 27 exigences ;
   - Phase 1 : 38 exigences ;
   - Phase 2 : 50 exigences.
2. Vérifier les observables exacts disponibles : octets, masks, IDs, cardinalités, raisons fermées et transitions d'état.
3. Refuser un test limité à « ne plante pas » lorsque la spécification fournit un résultat exact.
4. Utiliser le vrai chemin moteur → projection → manifeste/snapshot → décodeur indépendant.
5. Conserver les tests courts, déterministes, bornés et non bloquants.

Exécuter le contrôle statique avant tout build :

```text
python test/telemetry/tools/check_no_legacy_certification.py .
```

Le contrôle exclut l'archive historique. Toute dépendance active vers l'archive ou un ancien runner est une régression.

## Interdictions

Ne pas créer, restaurer ou invoquer :

- les étapes, agrégateurs ou runners du workflow historique archivé ;
- un writer de score ou de verdict ;
- une campagne longue, injection probabiliste complexe ou seuil percentile ;
- rapport contenant un score ou un champ d'éligibilité ;
- seam de test ou injection de manifeste dans le code produit.

Ne pas réparer un runner historique : le garder archivé et non exécutable.

## Tests et builds

- Utiliser le label CTest `telemetry-short`.
- Conserver un timeout individuel de 30 à 60 secondes.
- Viser moins de cinq minutes pour les preuves télémétrie après compilation.
- Exécuter le loopback réel sur x64 Linux et x64 Windows ; documenter explicitement une exclusion ARM.
- Sur Windows, vérifier qu'aucun autre build/test agent n'est actif, posséder seul l'exécution et transmettre `/m:1 /p:CL_MPCount=1` à MSBuild.
- Ne lancer aucune mesure de performance pendant une autre charge.

Un succès étroit ne termine pas une phase. Rapporter séparément exigences couvertes, tests non exécutés, observations humaines restantes et écarts.

## Observations Release

Utiliser les scénarios neutres du document 07 :

- Phase 0 : `P0-OBS-01..02` ;
- Phase 1 : `P1-OBS-01..02` ;
- Phase 2 : `P2-OBS-01..05`.

Pour la Phase 2, conduire deux observations Release d'une durée combinée inférieure à cinq minutes :

1. `CoreGate`, environ 60 secondes, mask `0x0401`.
2. `CompleteShip`, 90 à 120 secondes, mask `0x0583`, fermeture player/support/docking et `--drop-once delta`.

Le transcript consigne l'injection neutre sans score. La checklist humaine contient `attendu`, `observé`, `écart` et `impact`. Seul l'humain décide de la livraison.
