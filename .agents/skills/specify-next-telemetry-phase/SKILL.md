---
name: specify-next-telemetry-phase
description: Spécifier exclusivement la prochaine phase de télémétrie FS2Open à partir du contrat produit actif. Utiliser ce skill lorsque l'utilisateur demande de préparer, créer ou lancer la phase suivante. Auditer d'abord l'implémentation complète de la dernière phase spécifiée, quitter sans écrire si une exigence ou observation produit manque, sinon produire uniquement la phase N+1 sans recréer de gates, campagnes de certification ou validation auto-entretenue du harness.
---

# Spécifier la prochaine phase de télémétrie

Produire un contrat documentaire orienté produit pour une seule phase. Ne jamais
anticiper une phase supplémentaire et ne jamais implémenter le code produit.

Lire [references/product-specification-policy.md](references/product-specification-policy.md)
et
[../implement-latest-telemetry-phase/references/implementation-policy.md](../implement-latest-telemetry-phase/references/implementation-policy.md)
avant le préflight.

## 1. Préserver et inventorier

1. Lire `AGENTS.md`.
2. Inventorier `git status --short` et préserver tout changement existant.
3. Exécuter :

```text
python .agents/skills/specify-next-telemetry-phase/scripts/inspect_next_phase.py --repo-root .
```

4. Lire `documentation/analysis/README.md`, le package complet de la dernière
   phase et `.agents/skills/validate-telemetry-product/SKILL.md`.
5. Traiter le wire et les golden vectors comme autorités binaires. Traiter la
   roadmap et `documentation/analysis/archive` comme non normatives.

Le script inventorie la dernière phase, ses exigences, son tracker attendu et
la phase suivante. Il ne prouve jamais que l'implémentation est complète.

## 2. Bloquer avant toute écriture si la phase courante est incomplète

Le tracker canonique doit exister à :

```text
documentation/analysis/implementation-status/phase-<N>.json
```

Le skill de spécification ne crée, ne réinitialise et ne corrige jamais ce
tracker. Exécuter :

```text
python .agents/skills/implement-latest-telemetry-phase/scripts/phase_tracker.py \
  validate --repo-root . --phase-number <N> --require-complete --json
```

Le tracker ne possède aucun champ de complétude déclaratif. Le validateur
recalcule l’état à partir :

- des exigences et observations canoniques du document `07` ;
- de l’empreinte du contrat actif ;
- des chemins d’implémentation présents ;
- des preuves courtes et de l’empreinte actuelle de leurs entrées ;
- des observations produit relues par un humain ;
- de l’absence de blocage.

Utiliser le tracker comme index, jamais comme preuve autonome. Pour chaque
preuve marquée réussie, lire la commande et le résultat consignés, inspecter les
chemins concernés et vérifier que l’observable exact est réellement couvert.
Si le résultat ne peut pas être corroboré, réexécuter uniquement la preuve
courte correspondante en respectant `AGENTS.md`.

Ne pas lancer de campagne longue. Ne pas lancer le jeu ou répondre à une
autorisation système à la place de l’utilisateur.

Si le tracker manque, est invalide, périmé, incomplet ou si une preuve reste
invérifiable :

1. ne créer ni modifier aucun fichier de spécification ;
2. ne pas réparer le code, le test ou le harness ;
3. quitter immédiatement ;
4. rapporter les exigences concernées, le comportement manquant et la preuve ou
   observation attendue à partir de la sortie du validateur ;
5. distinguer clairement défaut produit, défaut d'outil et preuve humaine
   absente.

Une défaillance du harness n'est un défaut produit que si elle démontre un
écart observable : données incorrectes, crash, incompatibilité wire, ressource
non bornée ou impact gameplay. Si elle empêche seulement de conclure, indiquer
que la complétude n'est pas démontrée et s'arrêter.

## 3. Cibler exactement la phase suivante

Si la dernière phase complète est `N`, spécifier uniquement `N+1`.

- Ne jamais accepter un numéro fourni qui saute une phase.
- Ne jamais créer `N+2` dans le même run.
- Dériver le titre et le périmètre candidat de la roadmap et des analyses
  prospectives, sans leur donner d'autorité normative.
- Si le périmètre de `N+1` n'est pas identifiable sans décision produit,
  demander cette décision avant toute écriture.

## 4. Écrire le contrat produit

Écrire en français sauf demande contraire. Créer un seul dossier
`documentation/analysis/specs/<N+1>-<slug>` avec exactement :

```text
README.md
01-*.md
02-*.md
03-*.md
04-*.md
05-*.md
06-validation-securite-et-conformite.md
07-livraison-et-tracabilite.md
```

Préserver les phases antérieures. Mettre à jour seulement l'index actif
strictement nécessaire. Si un invariant racine doit changer, arrêter et
présenter l'impact avant de réécrire plusieurs phases.

Pour chaque exigence :

- décrire un comportement, une borne, une transition ou un échec observable ;
- utiliser un ID stable `P<N+1>-REQ-nnn` ;
- attribuer l'exigence à la phase qui introduit la capacité ;
- lier les dépendances antérieures sans les recopier ;
- séparer obligation produit et moyen de preuve ;
- définir le résultat exact quand le contrat le permet.

Dans le document 07, créer une ligne canonique par exigence :

```text
REQ | invariant ou observable produit | test automatisé | observation manuelle éventuelle
```

Définir peu d'observations Release, représentatives et courtes. Leur relevé
contient `attendu`, `observé`, `écart` et `impact`; seul l'humain décide de la
livraison.

## 5. Garder la preuve au service du produit

- Préférer les tests courts utilisant le vrai chemin de production.
- Utiliser un client ou décodeur indépendant quand le produit pourrait
  autrement se valider lui-même.
- Vérifier octets, IDs, cardinalités, bornes, causes et transitions exactes.
- Ne pas imposer de seam produit pour faciliter un test.
- Ne pas tester la forme du code quand un comportement mesurable existe.
- Ne créer ni lots de certification, ni gates, ni budget de campagne, ni
  verdict automatique.
- Garder performance, fuzz et endurance comme diagnostics sauf risque produit
  concret explicitement justifié.

Le skill définit la preuve minimale attendue mais ne collecte pas une campagne
de livraison. La collecte et les observations relèvent de
`validate-telemetry-product`.

## 6. Valider la nouvelle phase

Exécuter :

```text
python .agents/skills/specify-next-telemetry-phase/scripts/validate_product_phase_spec.py \
  --repo-root . \
  --phase-number <N+1> \
  --phase-directory documentation/analysis/specs/<N+1>-<slug>
python test/telemetry/tools/check_no_legacy_certification.py .
git diff --check
```

Corriger toute erreur documentaire. Ne pas transformer cette validation
mécanique en preuve d'implémentation.

## 7. Livrer

Rapporter :

- la phase antérieure et la preuve de sa complétude ;
- le chemin et l’état dérivé de son tracker ;
- le numéro et le périmètre de la phase créée ;
- le nombre d'exigences et d'observations ;
- les invariants wire préservés ;
- le résultat des validateurs documentaires ;
- les impacts attendus sur le code et les tests ;
- les décisions produit encore ouvertes.

Ne jamais annoncer que la nouvelle phase est implémentée ou livrée.
