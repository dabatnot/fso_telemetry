# Politique d’implémentation et de traçabilité

## Autorités

Appliquer cet ordre :

1. schémas wire et golden vectors pour les octets ;
2. invariants actifs de `documentation/analysis` ;
3. phase qui introduit la capacité ;
4. code, tests et exemples.

Le tracker est un index d’état versionné. Il ne peut ni modifier une exigence,
ni transformer un résultat rouge en succès, ni décider d’une livraison.

## Forme du tracker

Utiliser exactement
`documentation/analysis/implementation-status/phase-<N>.json` avec le schéma
`fs2open.telemetry.product-implementation.v1`.

Le tracker contient :

- l’empreinte du contrat actif ;
- une entrée ordonnée par exigence du document `07` ;
- le statut, le résumé et les chemins de l’implémentation ;
- les preuves automatisées et leurs entrées empreintées ;
- les observations définies une seule fois, reliées aux exigences ;
- un blocage catégorisé lorsqu’il existe.

Le tracker ne contient aucun champ global de complétude. Le validateur recalcule
l’état depuis le contrat et le worktree courants.

## États autorisés

`implementationStatus` :

- `pending` : aucun travail produit conclu ;
- `in_progress` : comportement partiellement implémenté ;
- `blocked` : action impossible avec un blocage explicite ;
- `implemented` : comportement présent dans les chemins déclarés.

État d’une preuve ou observation :

- `pending` ;
- `passed` ;
- `failed` ;
- `blocked`.

Un blocage utilise :

```json
{
  "category": "product|tooling|human",
  "summary": "cause factuelle",
  "nextAction": "plus petite action permettant de reprendre"
}
```

## Fraîcheur

Une preuve réussie est actuelle seulement si :

- sa commande et son résultat sont consignés ;
- sa date UTC est valide ;
- ses entrées existent hors build, archive et tracker ;
- l’empreinte de ces entrées correspond au worktree ;
- l’union des preuves réussies couvre les chemins d’implémentation.

Une modification hors de ces entrées n’invalide pas la preuve. Une modification
d’une entrée la rend périmée sans effacer le résultat historique.

L’empreinte du contrat couvre les spécifications actives jusqu’à la phase, les
invariants racine hors roadmap, les schémas wire et les golden vectors. Un
changement de contrat bloque la complétude jusqu’à revue.

## Observations humaines

Une observation `passed` contient :

- ses entrées et leur empreinte ;
- `observedAtUtc` et `reviewedBy` ;
- `expected`, `observed`, `deviation` et `impact`.

Écrire explicitement « aucun » lorsqu’il n’existe ni écart ni impact. Ne jamais
simuler le lancement du jeu, la décision humaine ou une autorisation système.

## Complétude dérivée

Une phase est correctement implémentée seulement si :

- le tracker correspond exactement aux exigences et observations canoniques ;
- l’empreinte du contrat est actuelle ;
- chaque exigence est `implemented`, documentée et reliée à des chemins actifs ;
- les preuves minimales attendues sont `passed`, actuelles et couvrantes ;
- toutes les observations référencées sont `passed` et relues ;
- aucun blocage ne subsiste.

Cette complétude autorise le skill de spécification à examiner la phase suivante.
Elle ne constitue pas une décision de release.
