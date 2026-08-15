# Phase 0 — Contrat de protocole

## 1. Objet

Cette phase définit FSTL 1.0, le protocole public qui permet à un producteur FS2Open et à un client distant de communiquer sans dépendre de leur ABI C++ ni de leur plateforme.

Le livrable est un contrat filaire complet, décodable et suffisamment borné pour être implémenté indépendamment.

## 2. Résultat produit attendu

À la fin de la phase :

- chaque message, record, enum, flag, unité et limite possède une représentation filaire non ambiguë ;
- deux implémentations indépendantes peuvent encoder et décoder les mêmes messages ;
- une session négocie sa version, installe ses manifestes et snapshots, applique ses deltas et se resynchronise ;
- les paquets invalides sont rejetés sans état partiel, allocation non bornée ni effet sur la simulation ;
- le mode `Cockpit` définit strictement les seules données publiables ;
- les vues de communication et de cible disposent de capabilities et de messages spécialisés cohérents avec le transport commun.

## 3. Documents actifs

| Document | Contenu produit |
|---|---|
| [01](01-cadre-normatif-et-perimetre.md) | portée, acteurs, autorité, invariants et exigences |
| [02](02-format-filaire-et-registres.md) | types binaires, en-tête, registres, messages et fragmentation |
| [03](03-session-horloges-fiabilite.md) | session, horloges, ACK/NACK, baselines, deltas et resynchronisation |
| [04](04-modele-de-donnees-v1.md) | records et modèle de données FSTL 1.0 |
| [05](05-capabilities-et-vues-specialisees.md) | capabilities, communication et vidéo de cible |
| [06](06-validation-securite-et-conformite.md) | robustesse, sécurité et critères de qualité |
| [07](07-livraison-et-tracabilite.md) | résumé et tests utiles |

Les analyses racines définissent les invariants communs et la terminologie. Cette phase définit le wire. Pour l'encodage binaire, le schéma versionné et les golden vectors prévalent ; roadmap, exemples prospectifs et archives sont non normatifs.

## 4. Qualité attendue

Le contrat est exploitable lorsque :

- les golden vectors canoniques sont décodés de façon identique par le code de référence et un décodeur indépendant ;
- un réencodage canonique reproduit exactement les mêmes octets ;
- les valeurs invalides représentatives sont rejetées avec une cause observable ;
- les limites de taille, fragmentation, transaction et mémoire sont explicites et vérifiables ;
- la compatibilité major/minor et le traitement des extensions inconnues sont déterministes ;
- aucune donnée reçue ne peut commander ou modifier la simulation.

Les résultats observés, leurs écarts et leur impact sont présentés à l'humain. La décision d'accepter un écart ou de demander une correction lui appartient.

## 5. Hors périmètre

Le producteur intégré au jeu, le client applicatif, les dashboards et le streaming effectif appartiennent aux phases suivantes.
