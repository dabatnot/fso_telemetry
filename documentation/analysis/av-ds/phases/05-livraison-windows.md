# Phase 5 — Livraison Windows

## Livrable

AV DS est distribué sous une forme installable ou directement déployable sur le
second PC Windows du simpit. La configuration des deux écrans, des deux Cougar
et des deux pages Radar peut être restaurée après redémarrage.

## Inclus

- build Windows reproductible d'AV DS, de Qt 6, de SDL3 et des dépendances
  nécessaires ;
- package ne demandant pas un environnement de développement sur le PC cible ;
- documentation du prérequis des pilotes officiels Thrustmaster ;
- configuration de l'adresse et du port FSTL ;
- configuration distincte de `MFD-L` et `MFD-R` ;
- restauration des affectations d'écrans et de Cougar, avec `F16 MFD 1` vers
  `MFD-L` et `F16 MFD 2` vers `MFD-R` comme valeurs par défaut ;
- persistance des filtres et réglages Radar retenus ;
- diagnostic lisible pour écran, Cougar ou endpoint FSTL absent ;
- comportement propre au démarrage avant FS2Open, à la reconnexion et au
  changement de mission ;
- documentation d'installation et de lancement sur le second PC.

Une configuration invalide ne doit pas être remplacée silencieusement par une
affectation fondée sur l'ordre d'énumération USB. AV DS conserve ce qui peut
l'être et demande une correction explicite pour le périphérique manquant ou
ambigu.

## Non inclus

- toute distribution ou qualification sur une plateforme autre que Windows ;
- abstraction ou travail anticipé destiné à un éventuel portage ;
- installation ou configuration de T.A.R.G.E.T ;
- lancement automatique avec Windows, sauf décision ultérieure ;
- mise à jour automatique ;
- page `COM` ;
- Central Pedestal ;
- commandes métier vers FS2Open.

## Vérification directe

1. Le package s'installe ou se déploie sur un Windows sans arbre de sources.
2. AV DS retrouve `MFD-L`, `MFD-R`, leurs écrans et les identités
   `F16 MFD 1/2` après redémarrage.
3. Les filtres propres aux deux MFD sont restaurés sans être intervertis.
4. Démarrer AV DS avant FS2Open affiche un état d'attente puis rejoint la
   session automatiquement.
5. Redémarrer FS2Open ou changer de mission ne nécessite pas de relancer AV DS.
6. Un écran ou Cougar absent est signalé sans empêcher l'autre MFD de
   fonctionner.
7. Désinstaller ou ne pas lancer AV DS ne modifie aucun fichier nécessaire au
   fonctionnement normal de FS2Open.
8. Le package fonctionne avec les pilotes Thrustmaster sans profil T.A.R.G.E.T.

À l'issue de cette phase, la première livraison Radar d'AV DS est exploitable
sur le second PC Windows. La page `COM` peut ensuite être ajoutée comme un
nouveau livrable indépendant.
