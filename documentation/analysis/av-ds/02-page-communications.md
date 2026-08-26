# AV DS — Page Communications

## 1. Objet

La page `COM` permet à chaque unité MFD d'AV DS de reproduire la `Talking
Head` active dans FS2Open. Elle appartient au catalogue de pages d'AV DS au
même titre que `RADAR`.

Cette vue est un affichage cockpit redondant. La `Talking Head` native reste
présente dans le HUD et FS2Open demeure entièrement jouable lorsque AV DS est
absent.

La source réseau, la synchronisation de lecture et les bundles locaux sont
définis par la [spécification technique de la source Communications et des
assets locaux](../specs/source-communication-et-assets-locaux/README.md).

## 2. Indépendance des deux MFD

`MFD-L` et `MFD-R` partagent la session FSTL et l'état autoritaire de la
communication, mais conservent chacun :

- leur page active ;
- leur page précédente ;
- leur mode COM ;
- leur navigation OSB ;
- leur état de présentation et leurs diagnostics locaux.

Les deux MFD peuvent afficher `COM` simultanément avec les mêmes réglages,
utiliser deux modes différents ou laisser la communication sur un seul écran.
Aucune sélection sur un MFD ne modifie l'autre.

## 3. Sélection et OSB

La page `COM` est sélectionnée depuis le catalogue de pages avec les OSB du MFD
concerné. Ses OSB permettent au minimum :

- de sélectionner le mode `PERMANENT` ;
- d'armer ou de désarmer le mode `DYNAMIC` ;
- de revenir au catalogue ou de sélectionner une autre page.

Ces actions modifient uniquement AV DS. Elles ne commandent ni la lecture de
la `Talking Head`, ni le menu de communication de FS2Open.

Le dessin et la position définitive des libellés seront fixés par la maquette
de la page. La correspondance entre les entrées physiques du Cougar et les OSB
est commune à toutes les pages AV DS.

## 4. Mode permanent

En mode `PERMANENT`, le MFD reste sur la page `COM` jusqu'à une nouvelle
sélection explicite du pilote.

- pendant une communication, il affiche la `Talking Head` active ;
- sans communication, il affiche un état d'attente ;
- un remplacement met à jour l'animation sans changer de page ;
- la fin de la communication ne provoque aucun retour automatique.

Le mode permanent est propre à l'unité. Les deux MFD peuvent donc rester en
`COM` en même temps.

## 5. Mode dynamique

Le mode `DYNAMIC` est une préférence persistante propre à une unité. Il reste
armé lorsque le pilote quitte la page `COM` pour afficher une autre page.

Lorsqu'une communication commence sur une unité armée :

1. AV DS mémorise la page actuellement affichée par cette unité ;
2. cette unité bascule automatiquement sur `COM` ;
3. les remplacements successifs restent sur `COM` ;
4. à la fin de la communication, l'unité revient à la page mémorisée.

Zéro, un ou deux MFD peuvent ainsi basculer pour le même événement. Chaque
unité mémorise et restaure sa propre page.

Une sélection manuelle effectuée pendant l'affichage dynamique exprime une
nouvelle décision du pilote : elle annule le retour automatique associé à la
communication en cours pour cette unité, sans désarmer nécessairement le mode
`DYNAMIC` pour les communications suivantes.

## 6. Autorités et rendu

FS2Open reste autoritaire pour :

- l'asset final choisi par le gauge ;
- le début, le remplacement et la fin de la communication ;
- l'offset, la pause, la boucle et la vitesse de lecture ;
- la pleine couleur ou la demande de teinte locale.

AV DS choisit seulement où présenter cet état. Il valide le bundle installé,
résout l'`asset_id` autorisé et calcule la frame depuis l'état temporel reçu.
Les pixels, animations et voix ne traversent pas FSTL.

La vue opérateur affiche l'animation ou son placeholder ainsi qu'un état
d'attente ou d'indisponibilité compréhensible. Les hashes et diagnostics
d'installation peuvent être accessibles dans une vue de diagnostic, sans
encombrer l'utilisation normale en vol.

## 7. Défaillances

- Un bundle absent ou incompatible rend `COM` indisponible sans perturber
  `RADAR`, l'autre MFD ou FS2Open.
- Une communication périmée est masquée dès que la session FSTL devient
  `STALE` ou `DISCONNECTED`.
- Une reconnexion en cours de communication rejoint l'offset autoritaire.
- Un changement de mission invalide toute lecture et tout retour automatique
  issus de la mission précédente.
- La perte d'un écran ou d'un Cougar ne bloque pas l'autre unité.

## 8. Hors périmètre initial

- transport ou restitution de la voix sur le PC AV DS ;
- texte, sous-titres et historique des communications ;
- boutons de lecture, pause ou déplacement dans l'animation ;
- navigation dans le menu de communication de FS2Open ;
- ordres aux alliés ou toute autre commande vers la simulation ;
- transit par AV CORE, son API Web ou le bus CAN ;
- suppression, déplacement ou modification de la `Talking Head` du HUD ;
- toute plateforme autre que Windows.

## 9. Scénarios de vérification

1. `MFD-L` reste sur `COM` permanent pendant que `MFD-R` affiche `RADAR`.
2. Les deux unités en `COM` permanent reproduisent la même communication.
3. Une seule unité armée en `DYNAMIC` bascule puis retrouve sa page précédente.
4. Deux unités armées basculent et reviennent indépendamment.
5. Un remplacement de communication ne provoque pas de retour intermédiaire.
6. Une sélection manuelle pendant une bascule dynamique n'est pas écrasée à la
   fin de la communication.
7. La `Talking Head` du HUD reste présente quel que soit l'état d'AV DS.
8. Un bundle invalide sur AV DS ne dégrade ni le Radar ni la session FS2Open.

Ces scénarios sont des vérifications produit directes, pas un mécanisme de gate
entre livraisons.
