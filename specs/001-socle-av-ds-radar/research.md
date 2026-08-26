# Research: Socle AV DS et Radar unique

## Transformation du produit

**Decision**: Transformer `tools/radar` en place et renommer la cible livrée en
`av-ds.exe`, sans conserver un second exécutable Radar installable.

**Rationale**: Le projet existant contient déjà le client FSTL, le décodage atomique,
le rendu, les icônes, les fontes, les tests et le packaging nécessaires. Une
transformation en place satisfait le remplacement demandé et réduit le risque de
divergence fonctionnelle.

**Alternatives considered**:

- Créer `tools/av-ds` en copiant le Radar : rejeté car cela dupliquerait le code et
  laisserait deux produits à maintenir.
- Ajouter AV DS comme second exécutable sur le même cœur : rejeté car la livraison ne
  doit plus proposer le client Radar autonome.

## Coque d'affichage minimale

**Decision**: Introduire une unité `MFD-L` possédant un identifiant, un catalogue de
pages statique et une page active. Le catalogue est une collection compilée contenant
seulement `RADAR`; la sélection initiale est déterministe et ne requiert ni registre
dynamique ni mécanisme d'extension.

**Rationale**: Ce modèle couvre exactement FR-002, FR-003, FR-004 et FR-015 tout en
offrant le vocabulaire nécessaire aux phases suivantes. Un widget de composition peut
héberger le `RadarWidget` existant sans modifier son modèle de rendu.

**Alternatives considered**:

- Garder directement `RadarWidget` comme widget central sans modèle d'unité : rejeté
  car l'unité, le catalogue et la page active sont des exigences explicites.
- Introduire plugins, fabriques extensibles ou chargement de pages par script : rejeté
  car ces mécanismes sont hors périmètre et spéculatifs.

## Messages globaux AV DS

**Decision**: Déplacer la traduction `ClientStatus` → message et son rendu hors de
`RadarWidget`. La fenêtre AV DS maintient un `AvDsMessage` global et le transmet à
chaque unité ; l'unité superpose un widget de message au contenu de sa page.

**Rationale**: Le statut de liaison appartient à l'application et doit être visible sur
toutes les unités indépendamment de la page active. Le `RadarWidget` redevient ainsi un
renderer de contenu Radar seulement, tandis que les libellés et couleurs existants sont
préservés.

**Alternatives considered**:

- Laisser l'overlay dans `RadarWidget` : rejeté car les futures pages ne recevraient pas
  les messages globaux.
- Dupliquer l'overlay dans chaque page : rejeté car cela répartirait une règle
  transversale dans les pages et risquerait des divergences.

## Compatibilité de configuration

**Decision**: Afficher partout l'identité AV DS, mais continuer à ouvrir les paramètres
avec le namespace `QSettings` historique `FS2Open/FsoSimpitRadar` pendant cette phase.

**Rationale**: Changer `QCoreApplication::applicationName` change le chemin de stockage
Windows et ferait apparaître une configuration existante comme absente. Un accès
explicite au namespace historique garantit la compatibilité sans migration destructive
ni ressaisie.

**Alternatives considered**:

- Renommer le namespace et copier les valeurs au premier lancement : viable, mais plus
  complexe et exposé aux lancements partiels ; aucune exigence n'impose de déplacer les
  données internes dans cette phase.
- Renommer le namespace sans migration : rejeté car cela viole FR-006.

## Transport et état Radar

**Decision**: Conserver `RadarClient`, `RadarImage`, le catalogue de manifeste et le
protocole FSTL 1.1 sans modification de contrat. Continuer à demander uniquement la
couverture `CockpitSensors` (`0x07CB`) et publier des images immuables après validation
atomique.

**Rationale**: Les comportements d'attente, pause, perte de flux, resynchronisation et
reconnexion sont déjà implémentés et testés. Le remplacement complet d'une image lors
d'un nouvel état évite de fusionner les contacts de missions ou d'observateurs
différents. Aucun besoin de la phase ne requiert une nouvelle donnée producteur.

**Alternatives considered**:

- Passer par AV CORE : rejeté par FR-011 et inutile au transport Radar existant.
- Ajouter un profil ou des records plus riches : rejeté faute de besoin cockpit et en
  raison de l'interdiction de tout état omniscient.
- Ajouter une voie de commandes métier : rejeté car AV DS reste passif.

## Construction, tests et livraison

**Decision**: Conserver CMake/C++20/Qt 6 et les quatre suites Qt Test, ajouter des tests
de coque AV DS, puis adapter `deploy-windows.ps1`, les noms visibles, les notices et le
README au bundle unique AV DS. Sous Windows, la sortie devient `av-ds.exe`; hors
Windows, le nom historique `FsoSimpitRadar` reste inchangé afin de préserver le script
Linux existant sans le migrer dans cette phase.

**Rationale**: La pile est déjà configurée et le build Windows existant démontre Qt
6.10.3 avec MSVC/Ninja, tout en gardant le minimum supporté Qt 6.2. Le contrôle manuel
en jeu complète les tests unitaires et d'intégration sans nouveau dispositif de preuve.

**Alternatives considered**:

- Changer de toolkit graphique ou ajouter DirectX : rejeté car aucun bénéfice requis et
  risque élevé de régression visuelle.
- Étendre simultanément le packaging Linux : rejeté car la phase cible uniquement
  Windows.
