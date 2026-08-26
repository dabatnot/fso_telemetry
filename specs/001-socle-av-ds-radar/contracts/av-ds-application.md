# Contract: Application AV DS, phase Radar unique

## Identité et livraison

- Le seul exécutable utilisateur livré est `av-ds.exe`.
- Le nom visible du produit et le titre de la fenêtre sont
  `AV DS — AV Display System`.
- Le bundle Windows remplace le bundle `FsoSimpitRadar`; il n'installe pas les deux
  applications côte à côte.
- La fenêtre est une fenêtre desktop normale, redimensionnable et maximisable.

## Topologie d'affichage

- L'application crée exactement une unité `MFD-L`.
- Le catalogue intégré expose exactement l'identifiant de page `RADAR`.
- `MFD-L.activePage` vaut `RADAR` dès le lancement.
- Aucune sélection d'écran, entrée Cougar, OSB ou navigation vers une autre page n'est
  exposée dans cette phase.

## Contrat de la page RADAR

Entrée : une référence immuable optionnelle vers le dernier `RadarImage` complet et
validé, plus les `RadarDisplaySettings` existants.

Sortie : le rendu des contacts autorisés, icônes, superpositions tactiques et effets
visuels du client de référence.

La page ne reçoit ni ne dessine les messages de connexion, d'attente, de pause, de
péremption, de reconnexion, d'erreur ou de configuration. Elle ne produit aucune
commande de simulation.

## Contrat des messages globaux

- Le statut du client est converti une seule fois en `AvDsMessage` au niveau de
  l'application.
- Chaque message actif est transmis à toutes les unités d'affichage ; avec la topologie
  de cette phase, `MFD-L` doit toujours le présenter.
- L'unité dessine le message au-dessus de la page active sans changer cette page.
- Les textes, couleurs et conditions de visibilité existants du Radar sont conservés.
- En état `Live`, aucun message global de liaison ne masque le contenu Radar.
- Une configuration absente ou invalide produit un message global et aucun contact
  n'est affirmé courant.

## Contrat de configuration

- AV DS accepte sans ressaisie les valeurs stockées par le client Radar sous le
  namespace `QSettings` `FS2Open/FsoSimpitRadar`.
- Les clés et leurs types restent compatibles :
  `connection/configured` (booléen), `connection/host` (chaîne),
  `connection/port` (1..65535), `display/*` (booléens) et
  `window/geometry` (octets Qt).
- `Escape` et `Ctrl+,` continuent d'ouvrir les réglages.
- Le dialogue peut conserver son rôle Radar, mais toute identité produit visible dans
  la fenêtre principale, l'exécutable, la documentation de livraison et le bundle est
  AV DS.

## Contrat FSTL

- Transport : UDP FSTL 1.1 direct entre AV DS et FS2Open.
- Profil demandé : `CockpitSensors` (`0x07CB`) uniquement.
- Sens retour autorisé : handshake, accusés de réception, resynchronisation et autres
  messages nécessaires à la fiabilité du transport existant.
- Commandes métier interdites : portée, cible, contacts, pause, contrôles ou tout autre
  état de simulation.
- Aucun changement de schéma, record, manifeste ou producteur n'appartient à cette
  phase.

## Compatibilité observable

- Une image n'est publiée qu'après validation atomique d'un état complet.
- Un changement de mission ou d'observateur remplace l'image courante ; les contacts ne
  sont pas fusionnés entre contextes.
- Une pause conserve l'image sans simuler de mouvement.
- Un état périmé est explicitement signalé et ne peut pas être interprété comme courant.
- La reconnexion converge sur le premier nouvel état complet valide sans redémarrage
  d'AV DS.
- L'absence ou l'arrêt d'AV DS n'affecte ni le HUD, ni les commandes, ni le déroulement
  de FS2Open.
