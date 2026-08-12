---
name: implement-latest-telemetry-phase
description: Implémenter exclusivement la dernière phase de télémétrie FS2Open déjà spécifiée. Utiliser ce skill pour commencer, reprendre ou terminer l’implémentation de la phase active, ajouter les tests courts nécessaires et consigner les observations produit sans créer de gate, tracker de preuve ou campagne de certification.
---

# Implémenter la dernière phase de télémétrie

Implémenter le contrat produit actif, une phase à la fois. Le document `07`
indexe les exigences et les preuves minimales attendues ; les sorties de test
restent dans CI ou dans le build, et ne servent jamais d’autorisation machine.

Lire [references/implementation-policy.md](references/implementation-policy.md)
avant toute modification.

## 1. Résoudre la phase et préserver le dépôt

1. Lire `AGENTS.md`.
2. Inventorier `git status --short` et préserver tous les changements existants.
3. Exécuter :

```text
python .agents/skills/specify-next-telemetry-phase/scripts/inspect_next_phase.py \
  --repo-root . --json
```

4. Cibler exactement `lastSpecifiedPhase`. Ne jamais implémenter une phase
   absente, la phase suivante ou plusieurs phases dans le même run.
5. Lire `documentation/analysis/README.md`, les huit documents de la phase, les
   passages hérités utiles et `.agents/skills/validate-telemetry-product/SKILL.md`.
6. Valider le package avec `validate_product_phase_spec.py` avant de modifier le
   produit.

Les schémas wire et golden vectors restent les autorités binaires. La roadmap
et `documentation/analysis/archive` restent non normatives.

## 2. Relier le travail au contrat

Lire la table canonique du document `07`. Pour chaque exigence traitée, relever
l’observable exact, les chemins de production concernés, le test court ou
l’observation Release qui peuvent le démontrer. Ne créer aucun JSON de suivi,
hash de fraîcheur ou état déclaratif de complétude.

## 3. Implémenter par exigence

Choisir le plus petit ensemble cohérent d’exigences encore incomplet.

- Corriger la spécification canonique avant le code seulement si une divergence
  réelle est découverte, puis aligner code et tests dans le même lot.
- Passer par les vrais chemins produit. Ne pas introduire de seam, fallback ou
  état produit uniquement pour simplifier une preuve.
- Préserver le wire existant, les bornes, le comportement non bloquant et les
  contrats des phases antérieures.
- Ajouter un test comportemental seulement lorsqu’un observable exigé n’est pas
  déjà prouvé.
- Vérifier le résultat exact prévu : octets, ID, cardinalité, borne, cause ou
  transition. Un simple « ne plante pas » ne suffit pas lorsqu’un résultat est
  défini.
- Distinguer un défaut produit, un défaut d’outil et une observation humaine
  manquante. Ne pas modifier le produit pour faire réussir un outil défectueux.

Utiliser des agents distincts pour l’implémentation, les tests et la revue quand
ils sont disponibles et que le travail se parallélise réellement. Leur avis
n’est ni un gate ni une preuve autonome ; seuls le comportement produit et les
résultats consignés comptent.

## 4. Collecter seulement la preuve nécessaire

Respecter le propriétaire unique des builds et tests défini par `AGENTS.md`.
Sous Windows, transmettre `/m:1 /p:CL_MPCount=1` à MSBuild.

- Exécuter d’abord le test déterministe le plus étroit qui prouve l’exigence.
- Utiliser `telemetry-short` pour la couverture télémétrie transverse requise.
- Garder chaque preuve bornée et les preuves télémétrie sous cinq minutes après
  compilation.
- Ne lancer ni soak, ni campagne probabiliste, ni mesure percentile comme
  condition d’implémentation.
- Écrire les sorties détaillées dans le build ou les artifacts CI, jamais dans
  les sources.
- Laisser à l’utilisateur le lancement du jeu et toute autorisation système.

Ne consigner dans les sources ni hash de fraîcheur ni historique de sortie. Ne
pas versionner les logs bruts. Ne jamais ajouter de score, budget, verdict
d’éligibilité, work package ou gate.

## 5. Consigner les observations Release

Une observation humaine se relève dans le document ou l’artefact Release prévu
par la phase, avec `attendu`, `observé`, `écart` et `impact`. Ne pas simuler le
lancement du jeu, la décision humaine ou une autorisation système.

## 6. Arrêter proprement ou terminer

En cas de blocage durable, préserver le travail et rapporter les exigences
touchées, sa catégorie (produit, outil ou humain) et la prochaine action exacte.
Ne pas compenser par une boucle de tests élargie.

Pour terminer, exécuter :

```text
python test/telemetry/tools/check_no_legacy_certification.py .
git diff --check
```

## 7. Rapporter

Indiquer :

- phase et exigences traitées ;
- comportements produit ajoutés ou corrigés ;
- preuves exécutées et leurs résultats ;
- observations humaines restantes ;
- défauts produit, défauts d’outil et blocages ;
- prochaine action.
