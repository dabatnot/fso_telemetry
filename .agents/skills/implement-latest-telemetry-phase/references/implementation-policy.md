# Politique d’implémentation et de traçabilité

## Autorités

Appliquer cet ordre :

1. schémas wire et golden vectors pour les octets ;
2. invariants actifs de `documentation/analysis` ;
3. phase qui introduit la capacité ;
4. code, tests et exemples.

## Traçabilité légère

Le document `07` de chaque phase est l’index canonique des exigences, de leurs
observables et des preuves minimales attendues. Les tests courts servent au
développement et à la révision du comportement concerné ; leurs sorties vivent
dans CI ou dans le build, sans JSON d’état, hash de fraîcheur ni autorisation
machine de poursuivre le travail.

Une observation Release est un relevé humain concis : `attendu`, `observé`,
`écart` et `impact`. Écrire explicitement « aucun » lorsqu’il n’existe ni écart
ni impact. Ne jamais simuler le lancement du jeu, la décision humaine ou une
autorisation système.

Un test rouge doit être qualifié comme défaut produit, défaut d’outil ou preuve
insuffisante pour l’exigence examinée. Il motive la correction ou l’investigation
de cette exigence, mais ne bloque jamais à lui seul la spécification de la phase
suivante.
