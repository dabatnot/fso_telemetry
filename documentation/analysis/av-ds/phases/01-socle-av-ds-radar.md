# Phase 1 — Socle AV DS et Radar unique

## Livrable

Le client Radar devient un premier exécutable Windows **AV DS** capable
d'afficher la page `RADAR` dans une fenêtre unique. Les fonctions Radar déjà
disponibles, la connexion FSTL et les comportements de reconnexion sont
préservés.

Cette phase introduit le vocabulaire minimal nécessaire aux pages futures sans
changer l'usage courant : une application AV DS, une unité d'affichage
`MFD-L`, un catalogue statique contenant `RADAR` et une page active.

## Inclus

- identité produit, fenêtre, titres et packaging renommés AV DS ;
- exécutable Windows `av-ds` ;
- conservation de Qt 6 pour les fenêtres, le rendu, le réseau et la
  configuration ;
- reprise du client FSTL et du modèle Radar existants ;
- reprise du renderer, des icônes et des états de lien existants ;
- une unité `MFD-L` dans une fenêtre normale ou maximisable ;
- une page `RADAR` sélectionnée au lancement ;
- paramètres de connexion conservés ;
- fonctionnement sans AV CORE et sans commande de simulation.

Le modèle de page reste volontairement minimal : catalogue intégré à
l'exécutable, page active et réception d'un état Radar immuable. Aucun système
de plugins ou de scripts n'est introduit.

## Non inclus

- seconde fenêtre ou `MFD-R` ;
- mode plein écran sans bordure imposé ;
- contrôleurs Cougar et OSB ;
- nouveaux filtres Radar ;
- page `COM` ;
- toute plateforme autre que Windows.

Cette phase ne crée pas de couche graphique DirectX spécifique et ne prépare
pas un portage vers une autre plateforme. Elle transforme le client existant
avec sa pile Qt 6 actuelle.

## Vérification directe

1. L'application s'identifie comme AV DS et démarre sous Windows.
2. Elle se connecte à FS2Open avec la configuration du client Radar existant.
3. Une mission affiche le même Radar et les mêmes contacts autorisés qu'avant
   la transformation.
4. Une pause, une perte de télémétrie et une reconnexion conservent le
   comportement existant.
5. Fermer ou ne pas lancer AV DS ne change rien au HUD ni au jeu FS2Open.

La phase est utile seule : elle livre un client Radar Windows sous sa nouvelle
identité, même sans second écran ni matériel Cougar.
