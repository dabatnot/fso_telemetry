# Phase 3 — MFD Cougar et OSB

## Livrable

Chaque contrôleur MFD Cougar pilote exclusivement l'unité AV DS à laquelle il
est affecté. Les libellés affichés au bord de la page correspondent aux OSB
physiques disponibles.

Cette phase valide la chaîne matérielle Windows avant d'introduire les réglages
Radar détaillés.

## Inclus

- pilotes Windows officiels Thrustmaster comme prérequis matériel ;
- lecture des contrôleurs de jeu avec SDL3, sans accès DirectInput ou XInput
  propre à AV DS ;
- découverte des identités Windows persistantes `F16 MFD 1` et `F16 MFD 2` ;
- association par défaut `F16 MFD 1` vers `MFD-L` et `F16 MFD 2` vers
  `MFD-R` ;
- réaffectation manuelle de secours lorsqu'une identité attendue est absente ou
  que le propriétaire choisit une autre disposition ;
- routage de chaque bouton vers une seule unité ;
- correspondance fixe entre les 28 entrées matérielles et leur position
  physique ;
- retour visuel permettant de vérifier l'OSB reçu ;
- accès à la sélection de page et sélection de `RADAR` ;
- libellés des commandes Radar prévues ;
- OSB de portée présents mais explicitement inopérants ;
- débranchement d'un Cougar sans perte de la session FSTL ni arrêt de l'autre
  MFD.

L'appui sur un OSB reste une commande locale d'AV DS. Aucun événement de bouton
ne produit une commande métier vers FS2Open.

L'identité et le numéro sont mémorisés dans chaque Cougar. AV DS ne dépend donc
ni de l'ordre d'énumération USB, ni du port USB utilisé. Il ne renumérote pas
les contrôleurs : avec deux unités, les numéros Thrustmaster 1 et 2 sont
conservés.

### Correspondance matérielle

| Commandes physiques | Entrées Cougar |
|---|---|
| OSB du haut, de gauche à droite | `1` à `5` |
| OSB de droite, de haut en bas | `6` à `10` |
| OSB du bas, de droite à gauche | `11` à `15` |
| OSB de gauche, de bas en haut | `16` à `20` |
| `SYM` haut/bas | `21` / `22` |
| `CON` haut/bas | `23` / `24` |
| `BRT` haut/bas | `25` / `26` |
| `GAIN` haut/bas | `27` / `28` |

Les 20 premiers boutons sont les OSB qui bordent la zone d'affichage. Les
quatre basculeurs fournissent chacun deux entrées supplémentaires. La gestion
fonctionnelle de `BRT`, `CON`, `SYM` et `GAIN` reste limitée aux actions prévues
par la page active.

## Non inclus

- filtres Radar définitifs ;
- changement réel de portée ;
- ciblage ou commande du jeu ;
- page `COM` ;
- Central Pedestal et MegaJoy ;
- utilisation de T.A.R.G.E.T ou fusion des Cougar dans un périphérique USB
  virtuel unique ;
- pilotage par AV DS du rétroéclairage ou des LED d'identification ;
- toute plateforme autre que Windows.

## Vérification directe

1. Les deux Cougar apparaissent séparément comme `F16 MFD 1` et `F16 MFD 2`.
2. Sans configuration supplémentaire, `F16 MFD 1` pilote uniquement `MFD-L`
   et `F16 MFD 2` uniquement `MFD-R`.
3. Les 28 entrées de chaque Cougar correspondent à la disposition documentée.
4. Les affectations restent correctes après un redémarrage normal de Windows,
   d'AV DS et après un changement de port USB.
5. Débrancher un Cougar ne coupe ni l'autre MFD ni la télémétrie.
6. Appuyer sur une portée affiche une action indisponible et ne change pas la
   portée reçue de FS2Open.
7. Le HUD et les commandes natives de FS2Open restent inchangés.
8. Aucun profil T.A.R.G.E.T n'est nécessaire pour utiliser les deux MFD.

La phase est testée avec les deux MFD Cougar réels sous Windows.
