---
name: implement-latest-telemetry-phase
description: Implémenter exclusivement la dernière phase de télémétrie FS2Open déjà spécifiée et tenir son tracker produit par exigence. Utiliser ce skill pour commencer, reprendre ou terminer l’implémentation de la phase active, ajouter les tests courts nécessaires, consigner les observations produit et fournir au skill de spécification un état vérifiable sans recréer de gates ni de campagnes de certification.
---

# Implémenter la dernière phase de télémétrie

Implémenter le contrat produit actif, une phase à la fois. Le tracker indexe les
preuves ; il ne remplace ni les résultats observés ni la décision humaine de
livraison.

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

## 2. Ouvrir et valider le tracker

Le tracker canonique est :

```text
documentation/analysis/implementation-status/phase-<N>.json
```

S’il manque, l’initialiser une seule fois :

```text
python .agents/skills/implement-latest-telemetry-phase/scripts/phase_tracker.py \
  init --repo-root . --phase-number <N>
```

S’il existe, ne jamais le réinitialiser ni écraser son historique factuel.
Exécuter :

```text
python .agents/skills/implement-latest-telemetry-phase/scripts/phase_tracker.py \
  validate --repo-root . --phase-number <N> --json
```

Corriger d’abord une erreur de structure ou de correspondance avec la table
canonique. Une empreinte de contrat différente impose de relire l’impact,
d’invalider les preuves touchées, puis seulement d’adopter la nouvelle
empreinte.

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

Après un résultat, calculer l’empreinte de ses entrées :

```text
python .agents/skills/implement-latest-telemetry-phase/scripts/phase_tracker.py \
  fingerprint --repo-root . --path <fichier-produit> --path <fichier-test>
```

Ne marquer une preuve `passed` qu’après son exécution réelle. Son champ
`inputs` doit couvrir tous les fichiers d’implémentation concernés et son
`inputFingerprint` doit être celui imprimé après l’exécution.

## 5. Tenir le tracker à jour

Mettre à jour le JSON avec `apply_patch` après chaque lot cohérent :

- `updatedAtUtc` indique la date UTC de la dernière mise à jour factuelle ;
- `implementationStatus` reflète le produit, pas l’état du test ;
- `implementationPaths` contient seulement les sources ou artefacts actifs ;
- `proofs` consigne commande, résultat, date, entrées et empreinte ;
- `requiredObservations` reste identique au document `07` ;
- une observation réussie contient attendu, observé, écart, impact et relecteur ;
- un blocage contient `category`, `summary` et `nextAction`.

Ne jamais rafraîchir une empreinte de preuve sans réexécuter la preuve. Ne pas
versionner les logs bruts. Ne jamais ajouter de score, budget, verdict
d’éligibilité, work package ou gate.

## 6. Arrêter proprement ou terminer

En cas de blocage durable, préserver le travail, renseigner les exigences
touchées et leur catégorie de blocage, valider le tracker puis quitter avec la
prochaine action exacte. Ne pas compenser par une boucle de tests élargie.

Pour terminer, exécuter :

```text
python .agents/skills/implement-latest-telemetry-phase/scripts/phase_tracker.py \
  validate --repo-root . --phase-number <N> --require-complete
python test/telemetry/tools/check_no_legacy_certification.py .
git diff --check
```

Le validateur dérive `complete` uniquement si chaque exigence est implémentée,
chaque preuve requise est actuelle et chaque observation obligatoire est
consignée. Cela signifie « phase correctement implémentée », jamais « livraison
automatiquement autorisée ».

## 7. Rapporter

Indiquer :

- phase et exigences traitées ;
- comportements produit ajoutés ou corrigés ;
- preuves exécutées et preuves réutilisées avec leur empreinte ;
- observations humaines restantes ;
- défauts produit, défauts d’outil et blocages ;
- état dérivé du tracker et prochaine action.
