# Politique d’exécution bornée

## Table des matières

1. Cône de dépendances
2. Empreintes
3. Qualification
4. Budget et arrêt
5. Décisions de gate

## 1. Cône de dépendances

Associer chaque preuve aux entrées qu’elle consomme :

- production ;
- test et oracle ;
- harness et fixtures ;
- CMake et options de build ;
- configuration de campagne ;
- outils externes et version ;
- exigences et critères observés.

Une modification n’invalide que les preuves dont au moins une entrée pertinente change.

## 2. Empreintes

Une empreinte de preuve contient :

```text
sourceHash
testHash
harnessHash
buildHash
configurationHash
commandHash
```

Utiliser `null` lorsqu’une dimension ne s’applique pas. Ne pas fabriquer de hash pour des sorties éphémères. Le rapport conserve la commande et l’empreinte des entrées, pas l’intégralité des logs.

## 3. Qualification

Avant une campagne longue :

1. prouver que l’oracle échoue sur un cas négatif ou une fixture volontairement invalide ;
2. exécuter deux cas courts déterministes ;
3. inclure le cas historiquement défaillant ;
4. confirmer les compteurs, délais et prédicats du rapport ;
5. obtenir une décision indépendante `qualified`.

Une qualification ne clôt pas la gate ; elle autorise seulement la campagne.

## 4. Budget et arrêt

Le budget de 180 minutes inclut :

- build non réutilisable ;
- campagnes ;
- extraction et validation des rapports ;
- revue indépendante.

Arrêter lorsque :

- un scénario échoue ;
- le heartbeat dépasse quinze minutes ;
- deux tentatives identiques échouent ;
- le temps restant ne permet plus la prochaine opération ;
- l’empreinte change pendant l’exécution ;
- la spec devient contradictoire.

## 5. Décisions de gate

États permis :

- `pending`: dépendances non satisfaites ;
- `ready`: readiness approuvée, preuve finale autorisée ;
- `blocked`: défaut contractuel ou preuve manquante ;
- `closed`: preuve fraîche et revue indépendante acceptées.

Une gate fermée ne se rouvre que si une preuve qui la soutient est invalidée ou si un défaut contractuel est démontré.
