# Politique de preuve proportionnée au risque

## Table des matières

1. Principes
2. Métadonnées obligatoires
3. Réutilisation et invalidation
4. Matrices et campagnes longues
5. Exceptions

## 1. Principes

La spécification décrit ce qui doit être vrai. Elle ne doit pas multiplier les exécutions pour donner une impression de certitude.

Une preuve est suffisante lorsqu'elle :

- observe directement le comportement contractuel ;
- échoue lorsque ce comportement est volontairement rompu ;
- couvre le risque qui lui est attribué ;
- est reproductible ;
- possède des dépendances identifiables ;
- n'est pas dupliquée par une preuve de niveau supérieur.

Une gate est utile lorsqu'elle autorise une nouvelle dépendance, un nouveau périmètre ou une clôture. Une simple répétition de tests n'est pas une gate.

## 2. Métadonnées obligatoires

Chaque preuve coûtant plus de cinq minutes doit être décrite dans un tableau contenant au minimum :

| Champ | Contenu |
|---|---|
| ID | Identifiant stable de la preuve |
| Risque couvert | Défaillance concrète détectée |
| Cadence | `wp-checkpoint` ou `gate-certification` |
| Commande | Commande ou procédure reproductible |
| Durée estimée | Temps mur attendu, en minutes |
| Qualification | Smoke préalable du harness et de l'oracle |
| Arrêt | Première condition imposant l'arrêt |
| Dépendances | Sources, tests, harness, configuration, outils |
| Invalidation | Changements rendant la preuve obsolète |
| Clé de réutilisation | Identifiant partagé par les critères consommateurs |
| Propriétaire | Testeur ou spécialiste indépendant |
| Gate | Gate consommatrice |

Le budget additionne les durées de construction, d'exécution, d'analyse et de revue qui ne peuvent pas s'exécuter en parallèle.

## 3. Réutilisation et invalidation

Une preuve reste fraîche tant qu'aucune de ses dépendances n'a changé.

Classer les changements dans un cône :

- `production`: comportement ou données observées ;
- `test-oracle`: assertion ou interprétation des résultats ;
- `harness`: injection, horloge, transport ou orchestration ;
- `build`: compilation, options ou dépendances ;
- `configuration`: paramètres de la campagne ;
- `report-only`: tracker, rapport ou prose.

Un changement `report-only` n'invalide aucune exécution. Un changement d'oracle n'invalide que les critères observés par cet oracle. Un changement de harness n'invalide que les propriétés produites par ce harness. Ne jamais invalider par simple chronologie.

## 4. Matrices et campagnes longues

Exécuter toute matrice peu coûteuse en smoke court. Sélectionner les scénarios longs par couverture de frontières et de risques, pas par produit cartésien.

Une campagne longue doit avoir :

- deux qualifications courtes, dont le cas historiquement défaillant ;
- des compteurs de progression ;
- un heartbeat au plus toutes les quinze minutes ;
- un arrêt au premier défaut reproductible ;
- un budget restant recalculé après chaque scénario.

Après un défaut, créer un reproducer court. Ne reprendre la campagne qu'une fois ce reproducer vert et la portée d'invalidation établie.

## 5. Exceptions

Dépasser 180 minutes seulement pour un risque critique qui ne peut pas être prouvé autrement. L'exception doit identifier la décision utilisateur. Une préférence générale pour « plus de couverture » n'est pas une justification.
