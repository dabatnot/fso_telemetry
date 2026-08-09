# 06 — Robustesse, sécurité et qualité produit

## 1. Objet

Ce document décrit le comportement attendu de FSTL face aux entrées valides, inconnues ou invalides. Il complète les layouts normatifs des documents [02](02-format-filaire-et-registres.md), [03](03-session-horloges-fiabilite.md), [04](04-modele-de-donnees-v1.md) et [05](05-capabilities-et-vues-specialisees.md).

## 2. Lecture et écriture bornées

Le protocole fournit des primitives de lecture et d'écriture qui :

- vérifient la capacité avant chaque accès ;
- n'avancent pas leur curseur après un échec ;
- distinguent fin de buffer, valeur invalide et limite dépassée ;
- encodent en little-endian sans dépendre de l'alignement natif ;
- refusent les flottants non finis, les chaînes non UTF-8 et les longueurs incohérentes ;
- construisent un message de manière transactionnelle avant publication.

Un échec ne publie jamais un message ou un record partiel.

## 3. Ordre de validation

Un datagramme entrant est traité dans cet ordre :

1. adresse source et allowlist ;
2. taille minimale et maximale ;
3. magic, version et type ;
4. cohérence de l'en-tête et des champs contextuels ;
5. CRC ;
6. session, endpoint et fenêtre de séquence ;
7. fragmentation et quotas ;
8. taille et structure du message logique ;
9. records, cardinalités et références ;
10. application transactionnelle.

Les vérifications peu coûteuses précèdent toute allocation proportionnelle à une valeur reçue.

## 4. Comportement face aux erreurs

| Situation | Comportement produit |
|---|---|
| paquet tronqué ou surdimensionné | rejet sans mutation |
| CRC invalide | rejet et compteur d'intégrité |
| version majeure incompatible | refus de négociation |
| extension mineure inconnue mais ignorable | record ignoré selon ses flags |
| record obligatoire inconnu | transaction refusée |
| baseline ou manifeste inconnu | état conservé et resynchronisation bornée |
| référence non résolue | transaction refusée |
| quota atteint | nouvel élément refusé avant allocation |
| source non autorisée | aucune réponse |
| message dupliqué | traitement idempotent |

Une erreur locale à une vue optionnelle désactive cette vue lorsque le protocole le permet, sans interrompre la télémétrie d'état.

## 5. Sécurité produit

Le producteur :

- reste en lecture seule vis-à-vis de la simulation ;
- ne désérialise aucune commande de jeu ;
- n'envoie aucune donnée avant validation de la source et négociation ;
- limite l'amplification pré-session ;
- applique les plafonds de débit, de fragments, de transactions et de mémoire définis dans le contrat ;
- n'expose ni pointeur, index brut, chemin local, secret ni donnée cachée hors du mode d'autorité négocié ;
- agrège les diagnostics pour éviter un log par paquet hostile.

## 6. Observabilité

Les implémentations rendent observables au minimum :

- paquets acceptés et rejetés par cause ;
- erreurs de CRC, version, taille, UTF-8 et référence ;
- réassemblages actifs, expirés et refusés ;
- transactions installées ou refusées ;
- resynchronisations et changements de session ;
- usage courant et maximum des ressources bornées.

Chaque compteur indique son unité et sa portée de remise à zéro.

## 7. Critères mesurables de qualité

| Critère | Niveau attendu |
|---|---|
| décodage canonique | 100 % des golden vectors valides produisent la représentation attendue |
| réencodage canonique | octets identiques au vector source |
| entrées invalides représentatives | zéro crash, lecture hors limites, état partiel ou allocation au-delà des quotas |
| compatibilité | résultat identique pour chaque combinaison major/minor documentée |
| bornes | valeur maximale acceptée et valeur immédiatement supérieure refusée |
| idempotence | doublons fiables sans double application |
| isolation | aucune entrée réseau ne modifie la simulation |

La vérification peut employer les outils les plus simples adaptés à ces critères. Le relevé final indique la valeur attendue, la valeur observée et l'impact de tout écart.
