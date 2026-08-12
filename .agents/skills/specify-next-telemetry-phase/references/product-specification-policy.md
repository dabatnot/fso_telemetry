# Politique de spécification produit

## Autorités

Appliquer cet ordre :

1. schémas wire et golden vectors pour les octets ;
2. invariants racine de `documentation/analysis` ;
3. phase qui introduit la capacité ;
4. code, tests et exemples.

Utiliser la roadmap pour découvrir le prochain périmètre, pas pour imposer une
exigence. Ne jamais utiliser l'archive historique comme modèle de procédure.

## Complétude de la phase précédente

Évaluer toutes les exigences de la table canonique du document 07. Une phase
est complète seulement quand le produit réalise chaque ligne et que les preuves
minimales prévues permettent de l'observer sur la révision courante.

Accepter comme preuve :

- test unitaire ou contractuel court vérifiant un résultat exact ;
- intégration courte utilisant le vrai runtime ;
- décodage indépendant des octets produits ;
- observation Release représentative consignée par un humain ;
- inspection statique pour une propriété structurelle réellement statique.

Ne pas accepter comme preuve suffisante :

- présence d'un fichier, d'une classe ou d'un nom de fonction ;
- succès « ne plante pas » quand un résultat exact est spécifié ;
- rapport sans révision ou inputs identifiables ;
- score, verdict ou éligibilité produits par l'outil ;
- réussite d'un chemin synthétique qui contourne le runtime produit ;
- ancienne campagne dont les entrées ont changé.

Un test ou outil rouge peut signifier :

1. **défaut produit démontré** : l'observable viole la spec ;
2. **défaut d'outil** : le produit n'est pas mis en cause ;
3. **complétude indémontrable** : l'outil manque et aucune autre preuve ne
   permet de conclure.

Dans les trois cas, qualifier et rapporter la catégorie sans réparer pendant ce
workflow documentaire. Un résultat de preuve ne constitue pas une autorisation
ni une interdiction de spécifier la phase suivante.

## Forme d'une exigence

Une exigence doit répondre à quatre questions :

1. Quel comportement produit est promis ?
2. Dans quelles conditions et limites ?
3. Quel résultat observable distingue succès et échec ?
4. Quelle preuve minimale peut observer ce résultat sans redéfinir le produit ?

Écrire des exigences atomiques. Séparer deux comportements ayant des causes
d'échec ou des preuves différentes. Ne pas dupliquer une exigence héritée.

## Politique de preuve

Choisir la preuve la moins coûteuse qui démontre réellement le comportement :

- octets et compatibilité : golden vectors et décodeur indépendant ;
- règles métier : projection pure puis chemin de sérialisation produit ;
- lifecycle et reprise : transitions déterministes courtes ;
- réseau : loopback réel et perturbation unique explicitement contrôlée ;
- ressource : bornes, comptabilité et refus avant allocation ;
- expérience en jeu : observation Release courte contrôlée par l'utilisateur.

Une campagne de plus de cinq minutes est diagnostique par défaut. Elle ne peut
devenir une obligation de phase que si une exigence produit explicite ne peut
pas être démontrée autrement et que l'utilisateur approuve ce choix.

Ne jamais demander au produit de qualifier le harness. Ne jamais ajouter au
code produit une injection, une seam ou un état uniquement pour faire réussir
un outil de preuve.

## Frontière documentaire

Conserver la structure `README + 01..07` pour rester cohérent avec les phases
actives :

- `README` : résultat produit, périmètre, frontières et qualité attendue ;
- `01` : cadre normatif et exigences ;
- `02` : architecture, contrats et interfaces ;
- `03` : flux, lifecycle et concurrence ;
- `04` : modèle de données et règles métier ;
- `05` : intégration, configuration, ressources et observabilité ;
- `06` : validation, sécurité et conformité produit ;
- `07` : livraison, traçabilité et observations.

Ne pas ajouter de document de gate, de tracker, de registre de fraîcheur ou de
campagne dans le package de spécification. Le document `07` reste la source de
vérité : une ligne canonique par exigence, son observable et le test court ou
l’observation manuelle qui peuvent le démontrer. Les résultats d’exécution
restent dans les sorties CI ou de build, jamais dans un mécanisme d’autorisation
pour la phase suivante.

Les éventuels exemples futurs restent explicitement non normatifs.
