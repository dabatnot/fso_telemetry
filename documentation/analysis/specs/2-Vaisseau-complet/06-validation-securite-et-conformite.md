# 06 — Sécurité et qualité produit

## 1. Objet

Ce document rassemble les comportements mesurables qui qualifient le tableau de bord complet. Les mappings et invariants détaillés restent définis dans les documents [02](02-architecture-contrats-et-interfaces.md), [03](03-flux-cycle-de-vie-et-concurrence.md), [04](04-modele-de-donnees-et-regles-metier.md) et [05](05-integration-configuration-et-observabilite.md).

## 2. Catalogues et profils

- Une session FSTL 1.1 sélectionne une couverture connue avant `WELCOME`.
- Les manifestes de classes et d'armes sont complets pour les entités publiées.
- Un manifeste est installé atomiquement avant le snapshot qui le référence.
- Un changement de catalogue produit un nouvel identifiant de manifeste et une keyframe.
- Le profil cœur décrit uniquement le joueur.
- `CompleteShip` contient le joueur et la fermeture minimale nécessaire aux relations autorisées de support et docking.

## 3. Exactitude des données

Pour chaque valeur du tableau de bord :

- les valeurs de nature `A` proviennent directement de la source moteur documentée ;
- les valeurs de nature `C` sont canonisées sans changer leur sens ;
- les valeurs de nature `D` sont calculées côté client par la formule documentée ;
- les unités et repères suivent FSTL ;
- les flottants sont finis et `-0` est canonisé ;
- les collections conservent leur cardinalité réelle dans les bornes du protocole.

Une valeur non représentable produit un état fermé et observable plutôt qu'une valeur inventée.

## 4. Cycle de vie

- Une apparition reçoit un `entity_id` non nul et monotone dans la session.
- Un respawn reçoit un nouvel `entity_id`.
- Mort, disparition et respawn sont reflétés dans l'état cumulatif.
- Le changement de joueur observé produit une keyframe.
- Une transition terminale de support reste observable même si l'objet moteur est nettoyé au même moment.
- Les relations de docking publiées forment une fermeture cohérente et bornée.

## 5. Snapshot, delta et récupération

- Le snapshot et chaque keyframe sont exhaustifs pour le profil choisi.
- L'installation est atomique après validation et `ACK APPLIED`.
- Un delta remplace des atomes FSTL complets et reste cumulatif contre la baseline.
- Les listes de boucliers, banques, tourelles, animations et relations ne sont jamais partiellement remplacées.
- Un delta trop grand est remplacé par une keyframe complète.
- Après stabilisation de la source et installation du manifeste requis, le client converge dans `2 × keyframeSeconds + 1 s`.

Une source momentanément indisponible conserve la dernière image cohérente et
son horodatage. Avant la première image complète, la session reste connectée en
attente de données représentables. Dès que la source redevient disponible, le
producteur reprend la publication et renouvelle le manifeste ou la keyframe si
la topologie l'exige. L'indisponibilité est exposée par les métriques, les logs
et l'âge calculé côté client.

## 6. Sécurité et confidentialité

- Le mode livré est `SOLO + COCKPIT`.
- Les données de ciblage, radar, menace, navigation, communication et vidéo restent absentes.
- Les références publiques utilisent des IDs FSTL, jamais des pointeurs ou indices moteur bruts.
- La capture moteur s'effectue sur le thread principal et les DTO possèdent leurs données.
- Le transport reste non bloquant et les plafonds sont vérifiés avant allocation.
- Les logs ne révèlent ni secret, ni adresse inutile, ni information cachée.

## 7. Critères mesurables

| Critère | Niveau attendu |
|---|---|
| couverture finale | exactement `0x0583` |
| catalogue | 100 % des références de classe et d'arme résolues |
| tableau de bord | 100 % des champs ont une source ou une formule documentée |
| cohérence de l'image | zéro record obligatoire absent et zéro référence pendante |
| lifecycle | apparition, mort, disparition et respawn visibles avec nouvel ID au respawn |
| cadence | mouvement à `flightHz`, systèmes à `systemsHz` |
| récupération | perte d'un delta corrigée par le delta cumulatif ou la keyframe suivante |
| coût | travail déterministe, borné et non bloquant ; aucune attente réseau |
| mémoire | respect simultané des plafonds partagés, par client et process du document 05 |
| frame de jeu | aucune attente réseau ni opération non bornée |

Ces propriétés sont relevées dans une session représentative et par des observations courtes ciblées sur les transitions concernées. Aucun oracle d'éligibilité, seuil percentile, scénario de trente minutes ou campagne de pertes complexes ne fait partie de l'acceptation.

## 8. Présentation des résultats

Chaque résultat associe une cible, une valeur observée, les conditions d'observation et l'impact de l'écart. L'humain décide ensuite si le produit est livrable, si une issue suffit ou si une correction est nécessaire.
