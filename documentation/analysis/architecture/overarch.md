# Audit DRY, YAGNI et KISS de la télémétrie

## 1. Portée et principes

Cet audit couvre le producteur C++, le protocole FSTL, les tests, la
documentation active et les clients Radar, AV Core et Dashboard.

Les trois principes sont appliqués ensemble :

- **DRY** : centraliser uniquement les règles qui doivent réellement évoluer
  ensemble ; ne pas rechercher la réduction mécanique du nombre de fichiers ;
- **YAGNI** : ne conserver une fonctionnalité, une configuration ou une
  compatibilité que si elle répond à un besoin cockpit actuel, à une contrainte
  réelle ou à un contrat externe publié ;
- **KISS** : privilégier un chemin produit direct et explicite, sans remplacer
  l'héritage actuel par un framework générique ou une nouvelle couche
  d'indirection.

La télémétrie est un produit cockpit. Une donnée nouvelle doit répondre à un
besoin concret du simpit, rejoindre la projection `CockpitSensors` et être
filtrée côté producteur avant attribution d'une identité publique et avant
sérialisation. `TrustedFullState` reste définitivement abandonné.

Aucune release officielle de FSTL n'existe encore. Le produit émet et accepte
exclusivement FSTL 1.1. Les champs de version restent présents dans le format
wire, mais aucune négociation, migration ou compatibilité 1.0, 1.2 ou future
n'est entretenue avant qu'une première release stable impose un contrat
externe réel.

## 2. État actuel

Quatre décisions racines sont désormais appliquées au produit :

1. `CockpitSensors` est l'unique profil producteur. La configuration publique
   utilise `schemaVersion: 4` sans sélection de profil. Toute session active
   produit la couverture `0x07CB` en solo graphique ; multijoueur, dédié et
   headless sont refusés avant bind.
2. FSTL 1.1 est l'unique version wire émise et acceptée par le jeu, le Radar,
   AV Core, le Dashboard et le producteur de replay. Les anciens chemins de
   négociation, manifests, schémas et vecteurs de compatibilité ont été
   retirés.
3. Le producteur construit directement l'image canonique `CockpitSensors` dans
   son propre pool préalloué. Le runtime et le contrôleur ne connaissent plus
   `Phase2Profile` et ne passent plus par une image `CompleteShip` transitoire.
4. Le builder préalloué `CockpitSensors` est désormais le seul constructeur
   d'image produit. Les builders, pools, profils, compositions et tests des
   anciennes projections ont été retirés.

Ces décisions ont supprimé la variabilité publique, l'empilement du chemin
producteur et les implémentations orphelines. La prochaine simplification porte
sur les fonctionnalités wire non publiées sans producteur ni client, et non sur
une nouvelle abstraction de projection.

## 3. Verdict

**Simplifier l'approche racine.**

La cible reste un seul pipeline : collecte cockpit filtrée, construction directe
d'une image canonique `CockpitSensors`, réplication FSTL 1.1, puis consommation
par des clients qui appliquent le même contrat exact.

Les garanties de sécurité, de fiabilité réseau et de mémoire bornée restent des
contraintes réelles. Les profils historiques, work packages, fonctionnalités
wire sans producteur et panneaux de roadmap ne le sont pas.

## 4. Findings actuels

### 4.1 Résolu — l'image `CockpitSensors` est construite directement

**Élément**

`NativeSessionRuntime` utilise exclusivement
`build_cockpit_sensors_state_image_preallocated()`. Le builder acquiert un slot
du `CockpitSensorsStateImagePool`, dimensionne le backing final puis y encode
directement les records communs, le cargo filtré cockpit et les records
capteurs.

**Évidence**

La couverture `0x07CB` est écrite dès la création de `SESSION_STATE`. Le builder
trie, valide et publie une seule image terminée. Le patch incrémental travaille
sur ce même backing et peut être annulé exactement ; une modification de
topologie utilise un autre slot préalloué avant publication.

**Alternative minimale**

Conserver ce chemin explicite sans introduire de framework de profils. Les
primitives d'encodage communes appartiennent désormais au seul composant
cockpit ; aucun builder ni pool historique ne doit être réintroduit.

**Réduction attendue**

Les branches de profil, la copie Phase 2 vers Phase 3, le backing intermédiaire
et les remplacements tardifs de `SESSION_STATE` et du cargo ont été supprimés
du chemin produit.

### 4.2 Résolu — un seul builder et un seul pool d'image subsistent

**Élément**

`cockpit_sensors_state_image` est le composant cockpit explicite. Il possède le
seul pool d'image, la préparation transactionnelle, l'encodage des records
communs et capteurs, le patch et le rollback. Les anciens fichiers d'image,
les builders `CoreGate`, WP05, WP06 et `CompleteShip`, leur adaptateur cockpit,
leurs pools et `Phase2Profile` ont été supprimés.

**Évidence**

Le code actif ne peut construire qu'une image FSTL 1.1 de couverture `0x07CB`,
y compris sans joueur. Le validateur rejette les anciennes couvertures au lieu
de maintenir leurs branches historiques.

**Alternative minimale**

Conserver ce propriétaire unique et explicite. Ne factoriser son cycle de vie
que si un second besoin produit réel apparaît ; aucun framework de profil ou de
projection ne doit remplacer les chemins supprimés.

**Réduction attendue**

Le stockage redondant, les cycles de vie parallèles et les tests de projections
inaccessibles ont été retirés. Les tests cockpit conservent les garanties de
capacité, transaction, patch et rollback.

### 4.3 La comptabilité mémoire décrit encore les work packages

**Élément**

`startup_budget.h/.cpp`, `Runtime`, `SessionController` et
`NativeSessionRuntime` conservent les budgets `WP03`, `WP04` et `WP06`, leurs
sous-totaux, catégories différées et comparaisons successives.

**Évidence**

Les contraintes utiles sont :

- arithmétique protégée contre les overflows ;
- préallocation avant bind ;
- plafonds explicites par scope partagé, client et process ;
- aucune croissance non bornée après `Ready`.

La chronologie des work packages ne constitue plus une contrainte
d'exécution. Elle décrit la construction passée plutôt que la propriété finale
à garantir.

**Alternative minimale**

Une fois les pools et fonctionnalités inutiles supprimés, additionner une seule
fois les octets effectivement possédés par chaque scope avec une arithmétique
vérifiée, puis comparer ces sommes aux plafonds avant bind.

Vérifier séparément l'absence de croissance après `Ready`, sans reconstruire
la chronologie WPxx.

**Réduction attendue**

Suppression des catégories différées, APIs WPxx, calculs parallèles et tests de
sous-totaux intermédiaires, tout en conservant les garanties mémoire.

### 4.4 Des fonctionnalités wire futures sont maintenues sans produit actuel

**Élément**

Le protocole contient encore :

- les messages Target Video ;
- `COMM_VIEW_STATE` et `COMM_VIEW_EVENT` ;
- les capabilities et extensions de négociation correspondantes ;
- codecs, reassembly vidéo, règles de fiabilité et de sécurité ;
- cycles de vie, caches, transitions, récupération et tests spécialisés.

La négociation Target Video est elle-même marquée réservée et non émettable.
Aucun producteur concret ni aucun des trois clients actifs ne consomme ces
fonctionnalités.

**Évidence**

Il n'existe ni release externe à préserver, ni besoin MVP actuel qui justifie
leur coût transversal. Talking Head ou vidéo cible pourront être réintroduits
si un besoin simpit concret est décidé ; ils exigeront alors une intégration
complète producteur-client, pas une réservation anticipée.

**Alternative minimale**

Conserver les idées dans la roadmap, mais retirer du produit actif les types,
capabilities, codecs, branches de sécurité, mécanismes de cycle de vie et tests
qui n'ont aucun émetteur ni consommateur.

Ne réserver aucun identifiant ou mécanisme de compatibilité avant une release.
Une future évolution choisira sa version et sa stratégie de compatibilité à
partir du contrat publié qui existera alors.

**Réduction attendue**

Suppression d'une large surface transversale sans comportement produit :
messages, états, quotas vidéo, stratégies de récupération et matrices de
capabilities.

### 4.5 Les invariants protocolaires restants ne sont pas encore DRY

**Élément**

La version courante et le masque de couverture connu ont été centralisés, mais
d'autres règles fermées restent recomposées :

- couvertures `CoreGate`, `CompleteShip` et `CockpitSensors` ;
- paires et masques de capabilities ;
- classifications de `MessageType` pour flags, autorité et fiabilité ;
- lecteurs de validation voisins dans plusieurs fichiers de records ;
- helper `publish_payload()` dupliqué entre états et événements.

**Évidence**

Ces règles doivent évoluer ensemble et peuvent diverger. Une centralisation
reste toutefois inutile pour les types qui seront supprimés avec les profils
ou fonctionnalités futures.

**Alternative minimale**

Après réduction du périmètre :

- conserver une seule constante de couverture cockpit ;
- centraliser les propriétés réellement communes des messages restants ;
- réutiliser un petit lecteur de validation interne ;
- extraire le helper commun de publication préfixe plus payload.

Ne pas introduire de générateur de code, moteur de schéma réflexif, registre
dynamique ou framework de sérialisation.

**Réduction attendue**

Moins de règles parallèles et un risque de dérive réduit, sans nouvelle
architecture générale.

### 4.6 Le Dashboard mélange produit actuel, indisponibilité et roadmap

**Élément**

Le catalogue et les cockpits exposent encore des panneaux `future` :

- `flight-future`, `energy-future`, `integrity-future` ;
- `weapon-future`, `support-future`, `tactical-future` ;
- Talking Head, événements de communication et vidéo cible.

Le mécanisme `future:...` peut promouvoir automatiquement un placeholder si
un record du même nom apparaît. En parallèle, `NAVIGATION_STATE` est encore
déclaré futur et non produit alors qu'il appartient déjà à la sortie
`CockpitSensors` actuelle.

Les composants répètent aussi les labels d'état, la recherche de définition,
la sélection du record joueur et le formatage de métriques simples.

**Évidence**

Deux sens différents de `ND` sont mélangés :

- une donnée du contrat courant peut être légitimement indisponible selon le
  contexte, par exemple absence de cible ou vaisseau sans bouclier ;
- une fonctionnalité non implémentée n'a pas à occuper un panneau du produit
  actif pour matérialiser la roadmap.

Le second usage entretient des composants et tests sans donnée actuelle. Le
mécanisme de promotion automatique anticipe en outre une intégration qui doit
rester explicite et testée.

**Alternative minimale**

- conserver l'état `ND` pour les données actuelles réellement indisponibles ou
  non applicables ;
- connecter `NAVIGATION_STATE` à sa source courante ;
- retirer les panneaux prospectifs et le mécanisme générique `future:...` ;
- conserver les besoins possibles dans la documentation, pas dans l'interface
  active ;
- mutualiser seulement les labels, la recherche stricte de définition, le
  sélecteur du record joueur, l'accès dérivé et le formatage simple.

Les panneaux visuellement différents restent des composants spécifiques ; un
composant universel paramétrable ne serait pas une simplification KISS.

**Réduction attendue**

Un Dashboard aligné sur ce que produit réellement FSTL 1.1, moins de panneaux
vides et une sémantique uniforme de `live`, `stale`, `waiting`, `invalid` et
`nd`.

### 4.7 Les artefacts historiques restent dans le périmètre actif

**Élément**

Le dépôt conserve encore :

- douze CSV de campagnes passées dans
  `test/telemetry/producer/frame-benchmark` ;
- deux missions d'observation `CoreGate` et `CompleteShip` ;
- de nombreux noms `WPxx`, `P8`, `P9.3`, `Phase1`, `Phase2` et `Phase3` dans
  le code et les tests actifs.

**Évidence**

Ces noms et sorties entretiennent l'impression que la chronologie des phases
ou un workflow de certification historique restent des responsabilités du
produit. Les CSV ne participent pas aux tests unitaires ordinaires et les deux
missions ne représentent plus une configuration producteur valide.

**Alternative minimale**

- supprimer ou déplacer les sorties de benchmark dans l'archive historique ;
- supprimer les scénarios qui ne protègent aucun contrat actuel ;
- renommer progressivement les composants conservés par responsabilité métier
  après la consolidation cockpit.

Ne pas effectuer un renommage massif avant les suppressions : cela créerait du
bruit et du travail jetable.

**Réduction attendue**

Une topologie alignée sur le produit courant et des tests qui décrivent des
comportements plutôt que l'ordre historique des travaux.

## 5. Éléments à conserver

### 5.1 Filtrage cockpit côté producteur

La visibilité doit continuer à être décidée avant attribution d'une identité
publique et avant sérialisation. Le passage par la projection radar du moteur
est une frontière de sécurité et de produit nécessaire.

Une simplification ne doit jamais réintroduire une vue omnisciente, une donnée
mission cachée ou un filtrage seulement côté client.

### 5.2 Sécurité et fiabilité UDP

Les éléments suivants répondent à des contraintes actuelles :

- datagrammes bornés ;
- fragmentation et réassemblage validés avant allocation ;
- CRC et validation de contexte ;
- allowlist et endpoints canoniques ;
- anti-amplification du handshake ;
- rate limiting ;
- ACK, NACK, retransmission et resynchronisation ;
- quotas de mémoire et cardinalités fixes.

Leur complexité ne doit pas être supprimée uniquement pour réduire le nombre
de lignes.

### 5.3 Codecs filaires explicites pour le contrat courant

Les codecs FSTL 1.1 par record peuvent rester explicites lorsque leurs layouts
et règles diffèrent réellement. Les golden vectors courants, le décodeur de
référence et les fuzzers restent utiles pour stabiliser le MVP avant sa
première release.

Ils ne constituent pas une promesse de compatibilité avec une version non
publiée. Les remplacer par un moteur réflexif ou un générateur complexe ne
serait pas une amélioration proportionnée.

### 5.4 Champs de version sans infrastructure de compatibilité

Les champs `version_major` et `version_minor` restent dans les datagrammes et
messages qui les portent. Ils identifient le contrat wire courant et
permettront un jour de reconnaître une évolution réelle.

Pour le moment, leur seule valeur valide est exactement 1.1. Il n'existe ni
plage, ni fallback, ni sélection partielle, ni version réservée.

### 5.5 Séparation cycle de vie et I/O native

La séparation entre le cycle de vie global `Runtime` et le serveur UDP
`NativeSessionRuntime` possède une responsabilité observable. Elle peut être
conservée tant que son interface reste limitée au produit courant et ne devient
pas un framework de services extensible sans seconde implémentation.

## 6. Ordre de simplification recommandé

1. **Terminé —** rendre `CockpitSensors` unique dans les interfaces produit.
2. **Terminé —** fixer émission et réception à FSTL 1.1 sans compatibilité.
3. **Terminé —** construire directement l'image finale `CockpitSensors` et retirer
   `Phase2Profile` du runtime et du contrôleur.
4. **Terminé —** supprimer les builders, pools, branches et tests de production
   devenus orphelins.
5. Retirer les fonctionnalités wire non publiées sans producteur ni client :
   COMM View, Target Video et capabilities associées.
6. Remplacer la chaîne de budgets WPxx par le calcul unique des capacités
   effectivement possédées après ces suppressions.
7. Centraliser les petits invariants DRY du protocole restant, sans framework
   générique.
8. Aligner le Dashboard sur les données réellement produites, retirer ses
   surfaces prospectives et mutualiser ses sélecteurs purs.
9. Archiver les benchmarks historiques et renommer les composants conservés
   par responsabilité métier.

Cet ordre traite les décisions racines avant leurs symptômes. Il évite :

- d'abstraire des builders ou pools destinés à disparaître ;
- de recalculer les budgets avant de connaître le stockage final ;
- de centraliser des types protocolaires futurs qui seront supprimés ;
- de renommer massivement du code voué à être retiré.

## 7. Validation attendue

Chaque lot de simplification doit préserver proportionnellement :

- les tests unitaires et d'intégration pertinents de `CockpitSensors` ;
- les vecteurs, le décodeur de référence et les fuzzers du contrat courant
  FSTL 1.1 ;
- les tests de sécurité, perte, désordre, duplication, retransmission et
  resynchronisation ;
- les limites de ressources avant bind et après `Ready` ;
- une vérification manuelle en jeu confirmant que Radar, AV Core et Dashboard
  montrent uniquement les informations autorisées visibles depuis le cockpit.

La validation est constituée des tests automatisés pertinents et d'un contrôle
manuel en jeu. Aucun tracker, score, registre de preuve, gate de phase,
certification, mécanisme d'éligibilité ou compatibilité spéculative ne doit être
ajouté pour conduire cette simplification.
