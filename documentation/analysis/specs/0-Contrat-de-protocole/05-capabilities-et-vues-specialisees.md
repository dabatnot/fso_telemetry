# 05 — Capabilities et vues spécialisées

## 1. Objet et dépendances

Ce document fixe le contrat FSTL 1.0 des capabilities, de la vue de communication par assets locaux et du flux de cible H.264. Il est normatif.

Il complète :

- [02 — Format filaire et registres](02-format-filaire-et-registres.md), autorité pour les scalaires, l'en-tête de 68 octets, les CRC, les types de messages et de records ;
- [03 — Session, horloges et fiabilité](03-session-horloges-fiabilite.md), autorité pour HELLO, WELCOME, les horloges, ACK/NACK, le backoff et la fragmentation ;
- [04 — Modèle de données v1](04-modele-de-donnees-v1.md), autorité pour les IDs d'entité et les autres records ;
- [06 — Validation, sécurité et conformité](06-validation-securite-et-conformite.md), autorité pour l'ordre de validation, les erreurs et les golden vectors.

Les payloads décrits ici commencent immédiatement après l'en-tête FSTL. Ils sont little-endian, sans padding implicite. Tout octet réservé DOIT être émis à zéro et une valeur non nulle DOIT faire rejeter le message ou le record qui la contient. Les tailles de payload excluent toujours l'en-tête de datagramme de 68 octets. Les tailles de record excluent l'en-tête de record de 6 octets.

La Phase 0 définit et teste ces octets, mais n'impose ni renderer, ni encodeur, ni décodeur H.264 dans la bibliothèque de contrat.

## 2. Registres réservés

### 2.1 Types de messages spécialisés

Les valeurs suivantes sont figées :

| Valeur | MessageType | Direction | Livraison |
|---:|---|---|---|
| 14 | TARGET_VIDEO_SUBSCRIBE | client vers producteur | fiable et idempotent |
| 15 | TARGET_VIDEO_CONFIG | producteur vers client | fiable et acquitté |
| 16 | TARGET_VIDEO_FRAME | producteur vers client | inter non fiable ; IDR récupérable pendant 500 ms |
| 17 | TARGET_VIDEO_KEYFRAME_REQUEST | client vers producteur | fiable, idempotent et rate-limité |
| 18 | TARGET_VIDEO_STOP | bidirectionnel | fiable et idempotent |
| 19 | TARGET_VIDEO_STATS | client vers producteur | remplaçable, non fiable et périodique |
| 20 | CAPABILITY_UPDATE | bidirectionnel | fiable et acquitté |

TARGET_VIDEO_STOP est le seul mécanisme de désabonnement v1. Aucun TARGET_VIDEO_UNSUBSCRIBE n'est réservé.

### 2.2 Types de records de communication

| Valeur | RecordType | Version | Conteneurs autorisés |
|---:|---|---:|---|
| 25 | COMM_ASSET_MANIFEST | 1 | MANIFEST fiable |
| 26 | COMM_VIEW_STATE | 1 | FULL_SNAPSHOT, keyframe et DELTA |
| 27 | COMM_VIEW_EVENT | 1 | EVENT_BATCH fiable |

COMM_ASSET_MANIFEST est global. COMM_VIEW_STATE et COMM_VIEW_EVENT décrivent l'unique vue Talking Head du cockpit observé et n'ont pas d'entity_id de préfixe.

Pour ces trois records, record_version vaut 1. COMM_ASSET_MANIFEST et COMM_VIEW_STATE imposent record_flags égal à zéro ; CREATE, DELETE et PARTIAL y sont interdits. COMM_VIEW_EVENT impose le seul bit CREATE, soit record_flags égal à 0x01 ; DELETE et PARTIAL y sont interdits. Son event_id est append-only.

## 3. Registre et négociation des capabilities

### 3.1 Bitmap Capability

Capability est un u64 extensible :

| Bit | Masque | Nom | Propriétaire |
|---:|---:|---|---|
| 0 | 0x0000000000000001 | COMM_VIEW_LOCAL_ASSETS | consommateur/client |
| 1 | 0x0000000000000002 | COMM_VIEW_AUTHORITATIVE_SOURCE | producteur |
| 2 | 0x0000000000000004 | TARGET_VIDEO_H264 | consommateur/client |
| 3 | 0x0000000000000008 | TARGET_VIDEO_REMOTE_RENDER | producteur |
| 4 | 0x0000000000000010 | CAPABILITY_UPDATE | chaque pair |
| 5 à 63 | — | réservés | aucun |

Un client NE DOIT PAS annoncer les bits 1 ou 3. Un producteur NE DOIT PAS annoncer les bits 0 ou 2. Chaque pair PEUT annoncer le bit 4. Les bits inconnus sont ignorés à la lecture, exclus du masque actif et ne sont jamais réémis par un relais v1.

Le masque active_capabilities contient les deux bits d'une paire activée :

- communication active : bits 0 et 1 présents ;
- vidéo active : bits 2 et 3 présents ;
- retrait dynamique autorisé : bit 4 annoncé par les deux pairs et présent dans active_capabilities.

La présence d'un seul bit d'une paire n'active jamais la fonctionnalité. Une
capability visuelle n'élargit jamais le périmètre cockpit et son absence
n'empêche jamais la télémétrie canonique de devenir Live.

### 3.2 Offre de bundle dans HELLO

Les extensions utilisent l'enveloppe de 6 octets définie par [02](02-format-filaire-et-registres.md). Les IDs spécialisés sont :

| extension_type | Nom | Version | Direction | Taille du contenu |
|---:|---|---:|---|---:|
| 1 | COMM_VIEW_NEGOTIATION | 1 | HELLO ou WELCOME | 40 octets |
| 2 | TARGET_VIDEO_NEGOTIATION | 1 | réservé, non émis en v1 | 0 |

La taille indiquée exclut l'enveloppe d'extension. TARGET_VIDEO_NEGOTIATION réserve l'identité d'une future extension ; FSTL 1.0 négocie ses paramètres uniquement par TARGET_VIDEO_SUBSCRIBE et TARGET_VIDEO_CONFIG.

Lorsque le client annonce COMM_VIEW_LOCAL_ASSETS, le contenu CommBundleOffer de l'extension type 1 de HELLO mesure exactement 40 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | bundle_version | u16 | 1 pour le format défini ici ; 0 signifie bundle absent |
| 2 | reserved | u16 | zéro |
| 4 | supported_delivered_formats | u32 | bitmap DeliveredFormatBit |
| 8 | bundle_hash | bytes[32] | SHA-256 du manifeste canonique ; tout zéro si bundle absent |

Si le bit COMM_VIEW_LOCAL_ASSETS est absent, cette extension DOIT être absente. Une offre présente avec capability absente est rejetée comme négociation incohérente.

### 3.3 Sélection de bundle dans WELCOME

Le contenu CommBundleSelection de l'extension type 1 de WELCOME mesure exactement 40 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | result | u8 | CommNegotiationResult |
| 1 | reserved | u8 | zéro |
| 2 | bundle_version | u16 | 1 si la source et son manifeste sont valides ; zéro sinon |
| 4 | required_delivered_formats | u32 | union des DeliveredFormatBit utilisés par le manifeste |
| 8 | required_bundle_hash | bytes[32] | hash exigé ; zéro si aucune exigence valide ne peut être annoncée |

CommNegotiationResult est une enum fermée :

| Valeur | Nom |
|---:|---|
| 0 | NOT_REQUESTED |
| 1 | ACCEPTED |
| 2 | SOURCE_UNAVAILABLE |
| 3 | BUNDLE_ABSENT |
| 4 | BUNDLE_VERSION_MISMATCH |
| 5 | BUNDLE_HASH_MISMATCH |
| 6 | NO_COMMON_FORMAT |
| 7 | MANIFEST_INVALID |

ACCEPTED exige simultanément :

1. COMM_VIEW_LOCAL_ASSETS annoncé par le client ;
2. COMM_VIEW_AUTHORITATIVE_SOURCE annoncé par le producteur ;
3. bundle_version égal à 1 ;
4. égalité octet par octet des 32 octets de bundle_hash ;
5. tous les formats livrés utilisés par le manifeste sont inclus dans supported_delivered_formats, soit required_delivered_formats & ~supported_delivered_formats égal à zéro ;
6. manifeste producteur valide et sans collision d'asset_id.

En cas d'acceptation, les bits 0 et 1 figurent dans active_capabilities. Tout autre résultat les retire tous les deux. Un hash différent désactive seulement la vue de communication ; il ne ferme pas la session.

L'extension de sélection DOIT être présente lorsque HELLO contenait une offre et absente sinon. Pour ACCEPTED, bundle_version, required_delivered_formats et required_bundle_hash sont non nuls et reproduisent exactement le bundle accepté. Pour BUNDLE_ABSENT, BUNDLE_VERSION_MISMATCH, BUNDLE_HASH_MISMATCH ou NO_COMMON_FORMAT, ils décrivent le bundle exigé afin de permettre le diagnostic hors session. Pour SOURCE_UNAVAILABLE, NOT_REQUESTED ou MANIFEST_INVALID, ils sont tous nuls.

TARGET_VIDEO_H264 et TARGET_VIDEO_REMOTE_RENDER n'ont pas d'autre extension de HELLO/WELCOME. Leur paire devient active lorsque le client peut décoder le profil v1 et lorsque le producteur dispose effectivement d'un renderer, d'un readback asynchrone et d'un encodeur autorisé. La configuration concrète est ensuite négociée par TARGET_VIDEO_SUBSCRIBE.

### 3.4 Retrait dynamique : CAPABILITY_UPDATE

MessageType 20 est bidirectionnel, fiable et acquitté. Son préfixe v1 mesure exactement 36 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | capability_generation | u32 | strictement croissant par émetteur dans la session, commence à 1 |
| 4 | advertised_capabilities | u64 | masque désormais annoncé par l'émetteur |
| 12 | active_capabilities | u64 | masque de paires encore actives |
| 20 | effective_time_us | u64 | horloge monotone de l'émetteur |
| 28 | reason | u8 | CapabilityUpdateReason |
| 29 | reserved | bytes[3] | zéro |
| 32 | extensions_length | u16 | zéro en v1 |
| 34 | extension_count | u16 | zéro en v1 |

Le payload v1 mesure donc exactement 36 octets. Aucun octet ne suit tant que extensions_length vaut zéro.

CapabilityUpdateReason est le registre commun fixé par [02](02-format-filaire-et-registres.md) :

| Valeur | Nom |
|---:|---|
| 0 | INVALID |
| 1 | RUNTIME_AVAILABILITY |
| 2 | PEER_REQUEST |
| 3 | CONFIGURATION_CHANGE |
| 4 | ERROR_RECOVERY |

INVALID n'est jamais émis et provoque le rejet du message. La cause spécialisée est portée par CommNegotiationResult, TARGET_VIDEO_STOP ou le diagnostic local ; elle n'étend pas ce registre transversal.

FSTL 1.0 autorise uniquement le retrait de bits pendant une session. advertised_capabilities et active_capabilities DOIVENT être des sous-ensembles des valeurs précédemment acceptées. L'émetteur ne peut retirer dans advertised_capabilities que les bits dont il est propriétaire ; lorsqu'un rôle disparaît, il retire les deux bits de la paire dans active_capabilities. Ajouter un bit, réactiver une paire ou changer de bundle exige une nouvelle session. Un message qui retire CAPABILITY_UPDATE est la dernière mise à jour dynamique valide de cet émetteur.

Une capability_generation inférieure, syntaxiquement valide et issue de la même session/du même endpoint est acquittée `VALIDATED | APPLIED` mais n'est pas réappliquée ; une génération égale n'est réacquittée que si son contenu est identique, conformément à 03. Un saut vers une génération supérieure est accepté puisque l'état transmis est complet. Le retrait du bit producteur de communication rend immédiatement la vue indisponible. Le retrait d'un bit vidéo arrête chaque flux concerné, purge les frames et interdit un nouvel abonnement. Les STOP et CAPABILITY_UPDATE sont idempotents et peuvent arriver dans n'importe quel ordre.

Un pair qui n'a pas négocié CAPABILITY_UPDATE ne reçoit pas ce message : une perte locale de capability entraîne alors TARGET_VIDEO_STOP pour un flux vidéo et, si la cohérence globale l'exige, SESSION_END.

## 4. Bundle et manifeste de communication

### 4.1 Principe

Les pixels et l'audio Talking Head ne traversent jamais FSTL 1.0. Le bundle est préparé et installé hors session. Le réseau transporte uniquement son identité, ses métadonnées et l'état de lecture. COMM_ASSET_MANIFEST ne transporte aucun fichier.

Le packager DOIT utiliser la même pile de mods et les mêmes règles CFile que le producteur. Il inclut toutes les variantes effectivement résolubles et susceptibles d'être sélectionnées dans le périmètre de missions déclaré ; il exclut les fichiers masqués par la priorité CFile. Une représentation livrée différente, y compris une conversion différente, produit un autre content_hash, un autre asset_id et un autre bundle_hash.

### 4.2 Formats

SourceFormat est une enum fermée :

| Valeur | Nom |
|---:|---|
| 0 | INVALID |
| 1 | ANI |
| 2 | EFF |
| 3 | APNG |
| 4 | STATIC_IMAGE |

DeliveredFormat est une enum fermée :

| Valeur | Nom | Bit de support |
|---:|---|---:|
| 0 | INVALID | aucun |
| 1 | ANI | 0x00000001 |
| 2 | EFF | 0x00000002 |
| 3 | APNG | 0x00000004 |
| 4 | WEBM_NO_AUDIO | 0x00000008 |
| 5 | RGBA8_ATLAS | 0x00000010 |
| 6 | PNG | 0x00000020 |

Les bits 6 à 31 de DeliveredFormatBit sont réservés. Un format enum inconnu invalide l'entrée. Un bit de support inconnu est ignoré.

AlphaMode est une enum fermée :

| Valeur | Nom | Sémantique |
|---:|---|---|
| 0 | NONE | aucun canal alpha |
| 1 | STRAIGHT | composantes couleur non prémultipliées |
| 2 | PREMULTIPLIED | composantes couleur prémultipliées par alpha |

AssetTimingMode est une enum fermée :

| Valeur | Nom | Durées explicites |
|---:|---|---|
| 0 | FORMAT_INTRINSIC | aucune ; le conteneur livré porte les timings |
| 1 | CONSTANT | une durée commune à toutes les frames |
| 2 | PER_FRAME | une durée par frame |

ManifestFlags est un bitmap fermé : bit 0, masque 0x0001, FRAME_ASSET ; bit 1, masque 0x0002, PLACEHOLDER_ASSET. Les bits 2 à 15 sont réservés et DOIVENT être nuls.

Une conversion DOIT préserver l'ordre des images, la durée de chaque image, le canal alpha, les dimensions logiques, la durée totale et le seek à un offset arbitraire.

ANI, APNG, WEBM_NO_AUDIO, PNG et RGBA8_ATLAS désignent chacun un fichier ou conteneur autonome. Pour un DeliveredFormat EFF, content_hash et asset_id adressent un descripteur canonique ; chaque image référencée possède sa propre entrée PNG dans le même manifeste. Le bundle_hash couvre ainsi le descripteur et chaque frame.

Le descripteur EFF livré en v1 est strictement ASCII, sans BOM, commentaire, ligne vide, espace final ni CR. Il contient exactement les quatre lignes suivantes, dans cet ordre, chacune terminée par un unique LF, avec un unique espace ASCII après `:` :

~~~text
$Type: png
$Frames: N
$FPS: F
$Keyframe: K
~~~

N, F et K utilisent leur écriture décimale canonique sans signe ni zéro initial, sauf la valeur K égale à zéro. N est dans 1..65 535, F dans 1..1000 et K dans 0..N-1. Le chemin du descripteur se termine par l'extension minuscule `.eff`. Si `stem` désigne ce chemin sans `.eff`, la frame d'index i est `stem_IIII.png`, où IIII est i en décimal complété à gauche jusqu'à au moins quatre chiffres, sans troncature au-delà de 9999. Chaque chemin ainsi produit respecte la grammaire portable ci-dessous et correspond à exactement une entrée DeliveredFormat PNG dont le hash a été validé.

L'entrée EFF porte timingMode PER_FRAME. Pour i dans 0..N, le packager calcule en u64 la frontière `b_i = floor((2 × i × 1 000 000 + F) / (2 × F))`, soit l'arrondi demi-supérieur de `i × 1 000 000 / F`. La durée de frame i dans 0..N-1 est `b_(i+1) - b_i`, frameDurationsUs contient exactement ces N différences et durationUs vaut b_N. Aucune implémentation ne peut recalculer ces valeurs avec un float. Toute autre grammaire EFF, référence implicite ou valeur de timing invalide le bundle v1.

### 4.3 Identité et manifeste canonique

content_hash est SHA-256 appliqué aux octets exacts du fichier ou conteneur livré. asset_id est l'entier u64 formé par les huit premiers octets de content_hash interprétés big-endian ; ce u64 est ensuite sérialisé little-endian sur le fil. asset_id zéro est interdit. Deux contenus différents ayant le même asset_id dans un bundle rendent le bundle invalide.

bundle_hash est SHA-256 du manifeste JSON canonique :

- UTF-8 sans BOM, sans normalisation Unicode implicite ;
- objets sans clés dupliquées ;
- clés triées selon leurs octets UTF-8 ;
- aucune espace hors chaînes ;
- entiers uniquement, écrits en base 10 sans signe plus, zéro initial ni exposant ;
- chaînes avec l'échappement JSON minimal obligatoire ; caractères de contrôle échappés, autres scalaires Unicode conservés ;
- tableau assets trié par asset_id numérique puis logicalName selon ses octets UTF-8 ;
- chemins relatifs normalisés avec / ;
- dates, permissions, chemins absolus et métadonnées du système de fichiers exclus.

Un parseur de manifeste NE DOIT PAS convertir un entier JSON u64 via un binary64. La forme canonique n'utilise ni NaN, ni infini, ni nombre fractionnaire.

Le schéma JSON v1 n'autorise aucune autre clé :

| Objet | Clé | Type et contrainte |
|---|---|---|
| racine | bundleVersion | entier 1 |
| racine | sourceRevision | chaîne UTF-8 de 0 à 64 octets, diagnostic |
| racine | modSignature | chaîne UTF-8 de 1 à 255 octets |
| racine | converterId | chaîne UTF-8 de 1 à 63 octets ; `identity` si et seulement si chaque asset livré est octet pour octet identique à sa source, sinon identifiant stable du pipeline de conversion |
| racine | converterVersion | chaîne UTF-8 de 1 à 63 octets ; version reproductible du packager/convertisseur |
| racine | frameAssetId | entier décimal u64 ; zéro si aucun cadre n'est recommandé, sinon ID présent dans assets |
| racine | placeholderAssetId | entier décimal u64 ; zéro si aucun remplacement n'est recommandé, sinon ID présent dans assets |
| racine | assets | tableau de 1 à 4096 entrées |
| asset | id | entier décimal u64 non nul, égal à asset_id |
| asset | logicalName | chaîne UTF-8 de 1 à 255 octets |
| asset | sourceFormat | entier SourceFormat |
| asset | deliveredFormat | entier DeliveredFormat |
| asset | file | chemin relatif ASCII portable de 1 à 1024 octets |
| asset | sha256 | chaîne ASCII de 64 chiffres hexadécimaux minuscules |
| asset | alphaMode | entier AlphaMode |
| asset | timingMode | entier AssetTimingMode |
| asset | width | entier de 1 à 4096 |
| asset | height | entier de 1 à 4096 |
| asset | frames | entier de 1 à 65 535 |
| asset | durationUs | entier u64 de 1 à 86 400 000 000 |
| asset | frameDurationsUs | tableau d'entiers u32 strictement positifs, cardinalité dictée par timingMode |

Les clés sont toutes obligatoires. sourceRevision vide est autorisé pour une source sans identifiant de révision, mais la clé reste présente. Les champs frameAssetId et placeholderAssetId sont les seuls IDs racine qui acceptent zéro ; leur présence non nulle détermine respectivement les bits FRAME_ASSET et PLACEHOLDER_ASSET du record filaire. La valeur sha256 décodée DOIT être égale à content_hash et ses huit premiers octets DOIVENT produire id. Toute extension du schéma JSON exige un nouveau bundleVersion.

frameDurationsUs suit exactement ces règles :

- FORMAT_INTRINSIC : tableau vide ; le format livré contient les timings et leur somme décodée vaut durationUs ;
- CONSTANT : un seul élément d strictement positif et frames multiplié par d vaut exactement durationUs, avec calcul u64 sans overflow ;
- PER_FRAME : exactement frames éléments strictement positifs dont la somme u64 sans overflow vaut exactement durationUs.

Le JSON canonique du bundle décrit ici est distinct du JSON attendu des fixtures de [06](06-validation-securite-et-conformite.md). Ses u64 sont des nombres JSON décimaux selon la grammaire ci-dessus ; un consommateur qui ne sait pas les parser sans binary64 n'est pas un parseur conforme de manifeste.

Un chemin de bundle est une chaîne ASCII composée de segments séparés uniquement par `/`. Chaque segment mesure 1 à 255 octets et correspond à l'expression régulière `[A-Za-z0-9_-](?:[A-Za-z0-9._-]{0,253}[A-Za-z0-9_-])?`. La longueur totale, séparateurs compris, est de 1 à 1024 octets. Cette grammaire exclut par construction racine, préfixe de volume, segment vide, `.`/`..`, backslash, NUL, deux séparateurs consécutifs, point/espace initial ou final et caractères dépendants de la plateforme.

Après ASCII-casefold, la partie d'un segment précédant son premier point NE DOIT PAS être `CON`, `PRN`, `AUX`, `NUL`, `COM1` à `COM9` ni `LPT1` à `LPT9`. Deux chemins du même bundle ne peuvent être égaux après ASCII-casefold. Les logicalName peuvent rester UTF-8 et ne participent jamais à l'ouverture d'un fichier. La résolution finale d'un file validé reste sous la racine canonique du bundle ; aucune règle supplémentaire propre à l'OS ne modifie l'acceptation protocolaire.

### 4.4 Layout COMM_ASSET_MANIFEST

Le payload d'un record RecordType 25/version 1 commence par un préfixe fixe de 64 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | bundle_version | u16 | 1 |
| 2 | manifest_flags | u16 | ManifestFlags ; bits non définis nuls |
| 4 | bundle_hash | bytes[32] | hash négocié |
| 36 | converter_id_length | u8 | 1 à 63 octets |
| 37 | converter_version_length | u8 | 1 à 63 octets |
| 38 | frame_asset_id | u64 | non nul si et seulement si FRAME_ASSET est présent |
| 46 | placeholder_asset_id | u64 | non nul si et seulement si PLACEHOLDER_ASSET est présent |
| 54 | total_asset_count | u32 | 1 à 4096 |
| 58 | first_asset_index | u32 | index de la première entrée de ce record |
| 62 | entry_count | u16 | 1 à 4096, sans dépasser le total |
| 64 | converter_id | bytes[converter_id_length] | UTF-8 valide, sans NUL |
| variable | converter_version | bytes[converter_version_length] | UTF-8 valide, sans NUL |

Les entrées commencent immédiatement après converter_version. Tous les records d'une même transaction répètent byte-identiquement manifest_flags, bundle_hash, converter_id, converter_version, frame_asset_id, placeholder_asset_id et total_asset_count. Les deux IDs globaux non nuls DOIVENT référencer des entrées du manifeste complet. `converter_id` vaut `identity` si et seulement si chaque asset livré est octet pour octet identique à sa source ; dès qu'au moins un asset est transformé, il identifie de façon stable le pipeline appliqué. `converter_version` identifie dans tous les cas la version reproductible du packager/convertisseur qui a produit et ordonné le manifeste.

Chaque entrée suit immédiatement la précédente et possède un préfixe fixe de 72 octets :

| Offset relatif | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | entry_length | u16 | 72 + logical_name_length + file_path_length + 4 × frame_duration_count |
| 2 | asset_id | u64 | non nul |
| 10 | content_hash | bytes[32] | SHA-256 complet |
| 42 | source_format | u8 | SourceFormat |
| 43 | delivered_format | u8 | DeliveredFormat |
| 44 | alpha_mode | u8 | AlphaMode |
| 45 | timing_mode | u8 | AssetTimingMode |
| 46 | reserved | u16 | zéro |
| 48 | width | u16 | 1 à 4096 |
| 50 | height | u16 | 1 à 4096 |
| 52 | frame_count | u32 | 1 à 65 535 |
| 56 | duration_us | u64 | 1 à 86 400 000 000 |
| 64 | logical_name_length | u16 | 1 à 255 octets |
| 66 | file_path_length | u16 | 1 à 1024 octets |
| 68 | frame_duration_count | u32 | 0, 1 ou frame_count selon timing_mode |
| 72 | logical_name | bytes[logical_name_length] | UTF-8, sans NUL |
| variable | file_path | bytes | ASCII et chemin portable conforme à la grammaire ci-dessus |
| variable | frame_durations_us | u32[frame_duration_count] | little-endian, strictement positives |

entry_length inclut son propre champ. FORMAT_INTRINSIC impose frame_duration_count égal à 0 ; CONSTANT l'impose à 1 ; PER_FRAME l'impose égal à frame_count. Les mêmes égalités de somme que le manifeste JSON s'appliquent. Les additions et la multiplication par 4 sont validées sans débordement avant lecture.

entry_length et le record_length de l'enveloppe Record sont des u16 et valent donc au plus 65 535. Le payload complet du record vaut exactement 64 + converter_id_length + converter_version_length + la somme des entry_length et NE DOIT PAS dépasser 65 535. Une entrée PER_FRAME dont les durées ne tiennent pas dans cette borne est non représentable en v1 et le packager DOIT choisir un conteneur à timing intrinsèque, une cadence constante, ou refuser le bundle ; il ne tronque ni ne répartit une entrée entre deux records.

Un manifeste peut utiliser plusieurs records consécutifs dans une ou plusieurs parts de la même transaction MANIFEST fiable définie par [03](03-session-horloges-fiabilite.md). Un record et une entrée ne traversent jamais une part. Les intervalles first_asset_index plus entry_count sont triés, contigus, sans recouvrement et couvrent exactement 0 à total_asset_count - 1. Le manifeste n'est installé qu'après validation atomique de toutes les parts, de tous les records et de leurs CRC. Chaque message logique de la transaction reste soumis au maximum fiable de 1 048 576 octets.

Les entrées sont ordonnées comme le tableau canonique et chaque champ filaire reproduit exactement la valeur JSON correspondante. Le content_hash complet est vérifié avant le premier affichage. Une collision d'asset_id, un doublon d'index, un ordre incorrect, un ID global absent du catalogue, une durée incohérente ou un mode alpha/timing invalide annule atomiquement tout le manifeste.

## 5. Vue de communication

### 5.1 Enums

| Enum | Valeur | Nom |
|---|---:|---|
| CommEventKind | 1 | START |
| CommEventKind | 2 | STOP |
| CommPlaybackMode | 0 | ONCE |
| CommPlaybackMode | 1 | LOOP |
| CommColorMode | 0 | HUD_TINT |
| CommColorMode | 1 | FULL_COLOR |
| CommStopReason | 0 | NONE |
| CommStopReason | 1 | COMPLETED |
| CommStopReason | 2 | INTERRUPTED |
| CommStopReason | 3 | REPLACED |
| CommStopReason | 4 | HUD_DISABLED |
| CommStopReason | 5 | MISSION_CHANGED |
| CommStopReason | 6 | SESSION_STOPPED |

Ces enums sont fermées. Aucune valeur implicite ne représente la lecture inverse : seul playback_rate porte le signe.

### 5.2 Layout COMM_VIEW_STATE

Le payload RecordType 26/version 1 mesure exactement 60 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | active | u8 | 0 ou 1 |
| 1 | playback_mode | u8 | CommPlaybackMode |
| 2 | color_mode | u8 | CommColorMode |
| 3 | stop_reason | u8 | CommStopReason |
| 4 | playback_id | u64 | non nul si active ; monotone dans la session |
| 12 | engine_message_id | u32 | corrélation moteur ; 0 si absent |
| 16 | sender_entity_id | u64 | 0 si absent |
| 24 | head_asset_id | u64 | asset_id non nul si active |
| 32 | producer_sample_time_us | u64 | horloge monotone producteur |
| 40 | animation_time_us | u64 | offset autoritaire |
| 48 | duration_us | u64 | durée attendue |
| 56 | playback_rate | f32 | fini, intervalle fermé -64 à +64 |

Si active vaut 1, stop_reason DOIT être NONE, duration_us DOIT être non nul et égal à la durée du manifeste, et head_asset_id DOIT exister dans le bundle validé.

Si active vaut 0, tous les champs de lecture DOIVENT être zéro : playback_id, engine_message_id, sender_entity_id, head_asset_id, producer_sample_time_us, animation_time_us, duration_us et playback_rate. playback_mode et color_mode conservent leurs valeurs enum canoniques zéro. stop_reason vaut NONE avant toute lecture, ou explique l'arrêt le plus récent.

Une valeur plus récente du même playback_id remplace intégralement l'ancienne. Un playback_id inférieur est ignoré. Zéro n'est jamais attribué à une lecture ; l'épuisement de u64 impose une nouvelle session.

### 5.3 Layout COMM_VIEW_EVENT

Le payload RecordType 27/version 1 mesure exactement 68 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | event_id | u64 | non nul, strictement croissant dans la session |
| 8 | event_kind | u8 | CommEventKind |
| 9 | playback_mode | u8 | CommPlaybackMode |
| 10 | color_mode | u8 | CommColorMode |
| 11 | stop_reason | u8 | CommStopReason |
| 12 | playback_id | u64 | lecture concernée, non nulle |
| 20 | engine_message_id | u32 | corrélation moteur |
| 24 | sender_entity_id | u64 | 0 si absent |
| 32 | head_asset_id | u64 | asset concerné |
| 40 | producer_sample_time_us | u64 | instant autoritaire |
| 48 | animation_time_us | u64 | offset autoritaire |
| 56 | duration_us | u64 | durée attendue |
| 64 | playback_rate | f32 | fini, -64 à +64 |

START transporte un état actif complet : stop_reason vaut NONE, head_asset_id et duration_us sont non nuls.

STOP transporte event_id, event_kind, stop_reason, playback_id et producer_sample_time_us. Tous les autres champs sont zéro. Un STOP ne s'applique que si son playback_id correspond encore à la lecture visible. Un événement dupliqué est acquitté de nouveau sans republier la transition.

Un remplacement est ordonné comme STOP avec REPLACED, puis START avec un playback_id supérieur. Le producteur NE DOIT PAS réutiliser un playback_id après changement de mission.

Un saut d'offset ou un changement de playback_rate, playback_mode, color_mode ou head_asset_id qui conserve la lecture active n'est pas un événement. Le producteur émet immédiatement un COMM_VIEW_STATE remplaçable portant l'état complet mis à jour.

### 5.4 Horloge et restitution

Le client utilise l'estimation d'horloge définie par HELLO/WELCOME et HEARTBEAT :

    elapsed_us = max(0, estimated_producer_now_us - producer_sample_time_us)
    raw_time_us = animation_time_us + playback_rate * elapsed_us

Le calcul intermédiaire est signé et effectué avec au moins la précision binary64. Pour LOOP, le client applique un modulo euclidien dans [0, duration_us). Pour ONCE, il borne dans [0, duration_us]. playback_rate égal à zéro conserve exactement animation_time_us ; une valeur négative lit en sens inverse.

Le producteur émet START et STOP immédiatement. Il émet aussi COMM_VIEW_STATE immédiatement après chaque changement d'offset, vitesse, boucle, couleur ou asset. Pendant une lecture active, il tente en plus un COMM_VIEW_STATE toutes les 100 000 microsecondes, sans jamais dépasser dix émissions périodiques par seconde. Cet état est remplaçable. Les keyframes restent l'autorité de reconnexion ou de resynchronisation.

Une durée différente entre état et manifeste, un f32 non fini, un asset inconnu ou un résultat arithmétique hors domaine produit un placeholder et un diagnostic ; il ne ferme pas la session. L'asset local n'est jamais recherché par engine_message_id ou par un chemin reçu hors manifeste.

### 5.5 Cycle de vie et fallback

Le sous-état client est l'un des suivants :

| État | Condition |
|---|---|
| Unsupported | capability client absente |
| SourceUnavailable | capability producteur absente ou retirée |
| BundleMismatch | offre refusée |
| Ready | bundle et manifeste validés, aucune lecture active |
| Active | lecture active et asset valide |
| Placeholder | lecture active mais asset ponctuellement absent ou corrompu |

BundleMismatch n'utilise jamais de placeholder : la vue entière est indisponible. Placeholder est réservé à une anomalie locale ponctuelle dans un bundle annoncé compatible. La télémétrie, le radar et la vidéo cible restent indépendants.

À la connexion tardive, COMM_VIEW_STATE actif suffit pour effectuer le seek. Au changement de mission, le producteur publie STOP/MISSION_CHANGED ou un état inactif dans la keyframe, invalide les anciens playback_id et NE DOIT PAS laisser une correction de l'ancienne mission réactiver la vue.

## 6. Registres vidéo H.264

### 6.1 Enums et bitmaps

VideoConfigResult :

| Valeur | Nom |
|---:|---|
| 0 | ACCEPTED |
| 1 | UNSUPPORTED_CAPABILITY |
| 2 | INVALID_REQUEST |
| 3 | NO_COMMON_PROFILE |
| 4 | NO_COMMON_LEVEL |
| 5 | NO_COMMON_RENDER_PROFILE |
| 6 | REQUIRED_OVERLAYS_MISSING |
| 7 | RESOURCE_LIMIT |
| 8 | TEMPORARILY_UNAVAILABLE |

Codec :

| Valeur | Nom |
|---:|---|
| 0 | INVALID |
| 1 | H264_ANNEX_B |

H264Profile utilise les profile_idc normalisés :

| Valeur config | Nom | Bit de supported_h264_profiles |
|---:|---|---:|
| 66 | CONSTRAINED_BASELINE | 0x00000001 |
| 77 | MAIN | 0x00000002 |
| 100 | HIGH | 0x00000004 |

H264Level utilise level_idc :

| Valeur config | Nom | Bit de supported_h264_levels |
|---:|---|---:|
| 31 | LEVEL_3_1 | 0x00000001 |
| 32 | LEVEL_3_2 | 0x00000002 |
| 40 | LEVEL_4_0 | 0x00000004 |
| 41 | LEVEL_4_1 | 0x00000008 |
| 42 | LEVEL_4_2 | 0x00000010 |
| 50 | LEVEL_5_0 | 0x00000020 |
| 51 | LEVEL_5_1 | 0x00000040 |
| 52 | LEVEL_5_2 | 0x00000080 |

Les bits supérieurs des bitmaps profil et niveau sont réservés et ignorés à la lecture.

| Enum | Valeur | Nom |
|---|---:|---|
| PixelFormat | 0 | INVALID |
| PixelFormat | 1 | YUV420P8 |
| RenderProfile | 0 | INVALID |
| RenderProfile | 1 | MFD_HIGH |
| RenderProfile | 2 | HUD_EXACT |
| OverlayMode | 0 | INVALID |
| OverlayMode | 1 | CLIENT |
| RecoveryMode | 0 | INVALID |
| RecoveryMode | 1 | IDR_SELECTIVE_RETRANSMIT |

RenderProfileBit :

| Bit | Masque | Profil |
|---:|---:|---|
| 0 | 0x00000001 | MFD_HIGH |
| 1 | 0x00000002 | HUD_EXACT |

MFD_HIGH demande le modèle principal et un LOD haute qualité ou automatique. HUD_EXACT demande le POF/LOD historique du target box lorsqu'il existe. Le producteur reste maître de ses limites de coût ; il ne sélectionne toutefois jamais un profil absent de acceptable_render_profiles.

OverlayCapabilityBit :

| Bit | Masque | Overlay local |
|---:|---:|---|
| 0 | 0x00000001 | TARGET_IDENTITY |
| 1 | 0x00000002 | DISTANCE_AND_SPEED |
| 2 | 0x00000004 | HULL_AND_SUBSYSTEM |
| 3 | 0x00000008 | IFF_AND_BRACKETS |
| 4 | 0x00000010 | AUXILIARY_GAUGES |

Les quatre premiers bits sont obligatoires pour OverlayMode CLIENT v1. Le bit 4 est facultatif.

VideoFrameFlag :

| Bit | Masque | Nom |
|---:|---:|---|
| 0 | 0x0001 | IDR |
| 1 | 0x0002 | DISCONTINUITY |
| 2 | 0x0004 | TARGET_CHANGED |

VideoKeyframeReason :

| Valeur | Nom |
|---:|---|
| 1 | PACKET_LOSS |
| 2 | DECODER_ERROR |
| 3 | LATE_JOIN |
| 4 | CONFIG_CHANGED |
| 5 | IDR_RECOVERY_EXPIRED |

VideoStopReason :

| Valeur | Nom | Émetteur autorisé |
|---:|---|---|
| 1 | CLIENT_UNSUBSCRIBE | client ou confirmation producteur |
| 2 | NO_TARGET | producteur |
| 3 | UNSUPPORTED_TARGET | producteur |
| 4 | MISSION_CHANGED | producteur |
| 5 | SESSION_STOPPED | producteur |
| 6 | CAPABILITY_WITHDRAWN | producteur |
| 7 | ENCODER_FAILED | producteur |
| 8 | RENDERER_UNAVAILABLE | producteur |
| 9 | RESOURCE_LIMIT | producteur |

VideoStopFlag bit 0, masque 0x01, signifie CONFIRMATION. Les bits 1 à 7 sont réservés.

VideoStatsFlag :

| Bit | Masque | Nom |
|---:|---:|---|
| 0 | 0x0001 | DECODER_STALLED |
| 1 | 0x0002 | DISPLAY_STALE |
| 2 | 0x0004 | CLIENT_OVERLOADED |

Tous les enums ci-dessus sont fermés. Les bitmaps explicitement nommés sont extensibles : bits inconnus ignorés ; flags réservés dans un message concret non nuls entraînent son rejet.

### 6.2 Plafonds de niveau AVC

Une CONFIG ACCEPTED respecte les plafonds suivants du level_idc sélectionné. MaxFS et MaxMBPS sont exprimés en macroblocs de luminance 16 × 16 ; MaxBR est la limite v1 conservatrice en kbit/s, suffisante pour CONSTRAINED_BASELINE, MAIN et HIGH puisque le protocole borne déjà le débit à 8000 kbit/s.

| level_idc | MaxFS | MaxMBPS | MaxBR_kbps |
|---:|---:|---:|---:|
| 31 | 3 600 | 108 000 | 14 000 |
| 32 | 5 120 | 216 000 | 20 000 |
| 40 | 8 192 | 245 760 | 20 000 |
| 41 | 8 192 | 245 760 | 50 000 |
| 42 | 8 704 | 522 240 | 50 000 |
| 50 | 22 080 | 589 824 | 135 000 |
| 51 | 36 864 | 983 040 | 240 000 |
| 52 | 36 864 | 2 073 600 | 240 000 |

Pour la vidéo progressive v1 :

    width_in_mbs = ceil(width / 16)
    height_in_mbs = ceil(height / 16)
    pic_size_in_mbs = width_in_mbs * height_in_mbs

Le producteur calcule ces valeurs en u64 sans overflow et vérifie simultanément : pic_size_in_mbs inférieur ou égal à MaxFS ; width_in_mbs et height_in_mbs inférieurs ou égaux à floor(sqrt(MaxFS × 8)) ; pic_size_in_mbs × fps_num inférieur ou égal à MaxMBPS × fps_den ; bitrate_kbps inférieur ou égal à MaxBR_kbps. fps_num/fps_den est réduit à sa forme irréductible avant émission. Une requête structurellement incohérente produit INVALID_REQUEST ; l'absence de niveau commun capable de porter une configuration autorisée produit NO_COMMON_LEVEL. ACCEPTED ne peut jamais annoncer une configuration qui dépasse un de ces plafonds.

Le profile_idc, le level_idc et les contraintes du bitstream effectif DOIVENT être conformes à CONFIG. Recopier seulement les octets profile_idc/level_idc dans le SPS ne suffit pas : dimensions codées/cropping, débit de macroblocs, débit binaire et toutes les autres restrictions normatives du profil/niveau sélectionné restent applicables à chaque access unit.

## 7. Layouts des messages vidéo

### 7.1 TARGET_VIDEO_SUBSCRIBE — 44 octets

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | request_id | u32 | non nul, strictement croissant dans la famille SUBSCRIBE ; aucun wrap |
| 4 | max_width | u16 | pair, 64 à 1024 |
| 6 | max_height | u16 | pair, 64 à 1024 |
| 8 | preferred_width | u16 | pair, 64 à max_width |
| 10 | preferred_height | u16 | pair, 64 à max_height |
| 12 | max_fps | u16 | 1 à 20 |
| 14 | preferred_fps | u16 | 1 à max_fps |
| 16 | max_bitrate_kbps | u32 | 128 à 8000 |
| 20 | preferred_bitrate_kbps | u32 | 128 à max_bitrate_kbps |
| 24 | supported_h264_profiles | u32 | au moins un bit connu |
| 28 | supported_h264_levels | u32 | au moins un bit connu |
| 32 | overlay_capabilities | u32 | bits obligatoires 0 à 3 |
| 36 | acceptable_render_profiles | u32 | au moins MFD_HIGH ou HUD_EXACT |
| 40 | preferred_render_profile | u8 | RenderProfile présent dans le bitmap précédent |
| 41 | reserved | bytes[3] | zéro |

La taille est exactement 44 octets. Le producteur rejette la demande entière si une préférence dépasse son maximum, si une dimension est impaire ou si un bitmap obligatoire est vide. Il PEUT choisir une valeur inférieure à la préférence, mais jamais supérieure au maximum client ni à sa propre configuration.

TARGET_VIDEO_SUBSCRIBE ne contient volontairement aucun target_entity_id. Le flux suit exclusivement la cible courante autorisée du joueur observé telle que publiée par TARGET_STATE ; le client ne peut ni choisir une entité, ni arbitrer entre plusieurs cibles, ni demander le rendu d'une entité absente de cet état canonique. Un changement de cible suit la séquence nouvelle CONFIG, ACK, puis IDR définie ci-dessous. L'absence de cible ou une cible non supportée produit respectivement TARGET_VIDEO_STOP/NO_TARGET ou TARGET_VIDEO_STOP/UNSUPPORTED_TARGET.

Un seul flux est actif par session client en v1. Le producteur alloue les stream_id non nuls dans un ordre strictement croissant et ne réutilise jamais un stream_id pendant la session, même après STOP. Sans flux actif, un nouveau request_id accepté crée le stream_id suivant. Avec un flux actif, il reconfigure le même stream_id, incrémente config_generation et remplace la demande précédente après acquittement de la nouvelle configuration ; aucun STOP intermédiaire n'est émis. Dans la fenêtre de déduplication, une retransmission byte-identique du même request_id renvoie le même résultat et ne crée aucune ressource supplémentaire. Réutiliser un request_id avec des octets différents est INVALID_REQUEST.

Tout SUBSCRIBE dont le payload et le contexte de session sont valides produit exactement un TARGET_VIDEO_CONFIG corrélé, que le résultat soit ACCEPTED ou un refus VideoConfigResult. Le producteur inscrit d'abord la décision et les octets exacts de CONFIG dans son état de déduplication et sa fenêtre fiable ; en cas d'acceptation, il réserve et committe aussi la configuration du flux, sans encore émettre de frame. Il émet alors pour SUBSCRIBE un ACK avec VALIDATED et APPLIED, soit ack_flags égal à 0x03. Un payload structurellement malformé reçoit le rejet protocolaire de 02/03 et ne produit pas de CONFIG.

Pendant la fenêtre fiable plus deux secondes définie par 03, une répétition byte-identique du même request_id renvoie le même ACK 0x03 et le même CONFIG octet pour octet, en réutilisant la décision et la fenêtre fiable existantes, sans nouvelle allocation, nouveau stream_id ni nouvelle config_generation. Ce cache de décisions partage la borne de 4096 entrées par client de la déduplication fiable. Après expiration, un request_id inférieur ou égal au plus grand ID déjà accepté dans sa famille est obsolète et ne peut jamais créer ou reconfigurer un flux. Le client ACKe CONFIG avec VALIDATED et APPLIED seulement après avoir validé et installé la configuration acceptée, ou enregistré atomiquement le refus ; le producteur n'envoie la première IDR d'une configuration acceptée qu'après cet ACK APPLIED. ACK et CONFIG peuvent arriver dans l'un ou l'autre ordre sur UDP sans changer ce résultat.

### 7.2 TARGET_VIDEO_CONFIG — 36 octets

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | request_id | u32 | echo du subscribe ; zéro pour reconfiguration spontanée |
| 4 | result | u8 | VideoConfigResult |
| 5 | codec | u8 | Codec |
| 6 | codec_profile | u8 | H264Profile |
| 7 | codec_level | u8 | H264Level |
| 8 | stream_id | u32 | non nul si accepté |
| 12 | config_generation | u32 | non nulle, strictement croissante par stream ; aucun wrap |
| 16 | pixel_format | u8 | YUV420P8 |
| 17 | render_profile | u8 | MFD_HIGH ou HUD_EXACT |
| 18 | overlay_mode | u8 | CLIENT |
| 19 | recovery_mode | u8 | IDR_SELECTIVE_RETRANSMIT |
| 20 | width | u16 | pair, 64 à maximum négocié |
| 22 | height | u16 | pair, 64 à maximum négocié |
| 24 | fps_num | u16 | supérieur à zéro |
| 26 | fps_den | u16 | supérieur à zéro |
| 28 | bitrate_kbps | u32 | 128 à maximum négocié |
| 32 | gop_duration_ms | u16 | 1 à 1000 |
| 34 | idr_recovery_window_ms | u16 | 1 à 500 ; 500 par défaut |

La taille est exactement 36 octets. Si result diffère de ACCEPTED, tous les champs après result DOIVENT être zéro. Une absence d'intersection produit un résultat précis, pas un fallback silencieux. Pour ACCEPTED, fps_num/fps_den est une fraction irréductible comprise entre 1 et max_fps inclus ; le profil et le niveau appartiennent aux bitmaps annoncés, le render_profile appartient à acceptable_render_profiles et chaque autre valeur reste dans les maxima du SUBSCRIBE corrélé.

Toute modification de cible, codec, profil, niveau, pixel format, résolution, cadence, GOP, render profile, overlay mode ou recovery mode incrémente config_generation. Une modification de bitrate seule incrémente également la génération en v1. Le producteur envoie CONFIG de façon fiable et attend son ACK avant la première IDR de la nouvelle génération.

Le client abandonne sans décoder toute frame d'une génération inconnue ou antérieure. Un stream_id n'est jamais partagé sur le fil entre clients, même si le producteur mutualise en interne rendu et encodage. Avant épuisement de config_generation, le producteur arrête le flux et exige un nouvel abonnement, donc un nouveau stream_id ; avant épuisement de stream_id, il ouvre une nouvelle session. Aucun de ces compteurs ne revient à zéro ni ne réutilise une valeur dans sa portée.

Le producteur DEVRAIT sélectionner preferred_render_profile lorsqu'il est localement disponible. À défaut, le profil producteur par défaut, lorsque l'offre client l'autorise, est MFD_HIGH, 1024 × 1024, 15/1 FPS, 4000 kbit/s, GOP 1000 ms, YUV420P8 et fenêtre IDR 500 ms. Ce sont des préférences producteur, jamais une garantie supérieure aux maxima du client.

### 7.3 TARGET_VIDEO_FRAME — préfixe de 36 octets

Le message logique commence par :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | stream_id | u32 | flux actif |
| 4 | config_generation | u32 | génération acquittée |
| 8 | video_frame_id | u32 | non nul, strictement croissant dans le stream ; aucun wrap |
| 12 | target_entity_id | u64 | entité stable non nulle |
| 20 | presentation_time_us | u64 | horloge monotone producteur |
| 28 | encoded_frame_size | u32 | nombre exact d'octets Annex B |
| 32 | video_flags | u16 | VideoFrameFlag |
| 34 | reserved | u16 | zéro |
| 36 | annex_b_access_unit | bytes | exactement encoded_frame_size octets |

message_size vaut exactement 36 + encoded_frame_size. La taille logique maximale est 2 097 152 octets ; encoded_frame_size est donc au plus 2 097 116. Les additions sont validées avant allocation.

video_frame_id est monotone dans le stream et ne repart pas à zéro lors d'un changement de génération. Avant épuisement, le producteur arrête le flux et exige un nouvel abonnement ; il ne wrappe jamais. Le client ne présente une frame que si stream_id, config_generation et target_entity_id correspondent encore à l'état courant. Une frame retardée de l'ancienne cible est abandonnée.

### 7.4 TARGET_VIDEO_KEYFRAME_REQUEST — 32 octets

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | request_id | u32 | non nul, strictement croissant dans la famille KEYFRAME_REQUEST ; aucun wrap |
| 4 | stream_id | u32 | flux actif |
| 8 | config_generation | u32 | génération attendue |
| 12 | last_decodable_frame_id | u32 | zéro si aucune frame décodable |
| 16 | needed_before_producer_time_us | u64 | deadline dans l'horloge producteur estimée ; zéro si le filtre d'horloge est invalide |
| 24 | reason | u8 | VideoKeyframeReason |
| 25 | reserved | bytes[7] | zéro |

La taille est exactement 32 octets. Si le filtre d'horloge est valide, la deadline est non nulle, DOIT être postérieure à l'estimation courante et au plus 500 000 microsecondes dans le futur. Si le filtre est invalide, le client DOIT écrire zéro ; le producteur ne compare alors aucune horloge distante et applique sa fenêtre locale de rétention de la classe fiable. Une valeur zéro avec un filtre valide ou une valeur non nulle avec un filtre invalide est une erreur sémantique. Une requête à deadline non nulle déjà expirée est acquittée mais n'impose pas d'IDR.

Le producteur déduplique request_id, peut agréger plusieurs demandes en une IDR et ne produit jamais plusieurs IDR pour les retransmissions du même request_id.

### 7.5 TARGET_VIDEO_STOP — 24 octets, bidirectionnel

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | request_id | u32 | non nul, appartenant à l'initiateur de la transaction STOP |
| 4 | stream_id | u32 | flux concerné |
| 8 | config_generation | u32 | dernière génération connue |
| 12 | reason | u8 | VideoStopReason |
| 13 | stop_flags | u8 | VideoStopFlag |
| 14 | reserved | u16 | zéro |
| 16 | producer_time_us | u64 | zéro dans requête client ; temps producteur sinon |

La taille est exactement 24 octets.

Pour se désabonner, le client alloue request_id dans sa famille STOP, sans wrap, puis envoie reason CLIENT_UNSUBSCRIBE, stop_flags égal à zéro et producer_time_us égal à zéro. Le producteur :

1. libère l'abonnement au plus une fois ;
2. ACKe le message fiable ;
3. renvoie TARGET_VIDEO_STOP avec le même request_id initié par le client, CLIENT_UNSUBSCRIBE, le bit CONFIRMATION et son temps monotone.

Le client ACKe cette confirmation mais NE DOIT PAS répondre par un autre STOP. Une répétition de la demande produit la même confirmation.

L'identité d'une transaction STOP est `(session_id, rôle_initiateur, request_id)` ; la confirmation n'alloue pas un ID producteur et ne collisionne donc jamais avec sa famille STOP. Un STOP initié par le producteur utilise un nouveau request_id strictement croissant de sa propre famille, stop_flags égal à zéro et une raison autre que CLIENT_UNSUBSCRIBE. Le client purge immédiatement frames, décodeur et réassemblages de ce stream, puis ACKe. Le STOP peut précéder ou suivre CAPABILITY_UPDATE sans changer le résultat final. Chaque famille request_id force une nouvelle session avant wrap.

### 7.6 TARGET_VIDEO_STATS — 72 octets

| Offset | Champ | Type |
|---:|---|---:|
| 0 | stream_id | u32 |
| 4 | config_generation | u32 |
| 8 | report_id | u32 |
| 12 | highest_received_frame_id | u32 |
| 16 | highest_decoded_frame_id | u32 |
| 20 | highest_presented_frame_id | u32 |
| 24 | received_frames | u32 |
| 28 | decoded_frames | u32 |
| 32 | presented_frames | u32 |
| 36 | dropped_frames | u32 |
| 40 | missing_fragments | u32 |
| 44 | decoder_errors | u32 |
| 48 | estimated_loss_ppm | u32 |
| 52 | jitter_us | u32 |
| 56 | decode_latency_us | u32 |
| 60 | presentation_latency_us | u32 |
| 64 | buffered_duration_us | u32 |
| 68 | report_interval_ms | u16 |
| 70 | stats_flags | u16 |

La taille est exactement 72 octets. Les compteurs couvrent uniquement report_interval_ms depuis le rapport précédent et saturent à UINT32_MAX. estimated_loss_ppm est borné à 0..1 000 000. report_interval_ms vaut 500 à 5000 ; la valeur recommandée est 1000. report_id commence à 1, est strictement croissant par stream et ne wrappe pas ; le client cesse STATS jusqu'au flux suivant avant épuisement.

STATS est remplaçable, non fragmenté, non fiable et facultatif. Il ne modifie jamais la simulation. Le producteur PEUT réduire bitrate, cadence ou résolution à partir de ces mesures, mais toute modification passe par une nouvelle CONFIG fiable.

## 8. Profil H.264 Annex B v1

Chaque TARGET_VIDEO_FRAME contient exactement une access unit. Chaque NAL unit commence par le start code de quatre octets 00 00 00 01 ; la forme à trois octets n'est pas canonique en émission v1. Un Access Unit Delimiter est facultatif parce que la frontière du message logique est autoritaire.

Contraintes obligatoires :

- H.264/AVC progressif, chroma_format_idc égal à 1, 8 bits, 4:2:0 ;
- lorsque codec_profile vaut 66, constraint_set1_flag DOIT valoir 1 afin que la valeur protocolaire signifie bien Constrained Baseline ;
- aucune B-frame ; ordre de décodage identique à l'ordre de présentation ;
- aucune interlaced field picture ;
- profil et niveau exactement égaux à CONFIG ;
- dimensions affichées exactement égales à width et height de CONFIG, avec cropping SPS si le codage par macroblocs l'exige ;
- VUI BT.709 : colour_primaries, transfer_characteristics et matrix_coefficients égaux à 1 ; video_full_range_flag égal à 0 ;
- intervalle entre IDR inférieur ou égal à gop_duration_ms ;
- chaque frame marquée IDR contient au moins une slice NAL type 5 ;
- SPS puis PPS complets apparaissent avant la première slice de chaque IDR ;
- une interframe non marquée IDR ne contient aucune slice type 5 ;
- TARGET_CHANGED et DISCONTINUITY impliquent IDR ;
- changement de cible, résolution, profil ou génération force une IDR.

Le payload H.264 ne contient ni texte, ni bracket, ni jauge. Les overlays sont reconstruits depuis TARGET_STATE et les records canoniques. MJPEG n'est pas un Codec v1 ; un diagnostic MJPEG nécessite une version ou capability ultérieure.

Le parseur Phase 0 valide les start codes, les limites, les flags et la présence SPS/PPS/IDR requise sans lancer de décodeur multimédia. Une implémentation applicative DOIT ensuite traiter le bitstream comme une entrée non fiable et borner toutes les ressources du décodeur.

presentation_time_us est l'instant monotone de capture, pas une horloge civile ni une consigne autorisant une attente non bornée. Le client l'interprète avec l'estimation d'horloge de [03](03-session-horloges-fiabilite.md), présente dès décodage dans sa politique de faible latence et calcule ses métriques à partir de cet instant. Il compose uniquement avec un TARGET_STATE courant portant le même target_entity_id. Si cet état n'existe plus, la frame est abandonnée ; les données d'une autre cible ne sont jamais superposées. Un client qui conserve un historique PEUT choisir l'échantillon canonique le plus proche de presentation_time_us, mais l'état numérique reste autoritaire.

## 9. Fragmentation et récupération vidéo

Le fragment canonique transporte au plus 1132 octets après l'en-tête FSTL de 68 octets. TARGET_VIDEO_FRAME respecte :

| Limite | Valeur v1 |
|---|---:|
| message_size maximal | 2 097 152 octets |
| fragment_count maximal | 2048 |
| réassemblages vidéo simultanés par client | 3 |
| mémoire vidéo de réassemblage par client | 6 Mio |
| timeout interframe | 200 ms |
| timeout/rétention IDR | 500 ms |

La fragmentation, les CRC et les NACK MISSING_FRAGMENTS suivent [02](02-format-filaire-et-registres.md) et [03](03-session-horloges-fiabilite.md). fragment_count, message_size et les offsets sont validés avant réservation. Une access unit partielle n'est jamais envoyée au décodeur.

Pour une interframe :

- aucun ACK de réception métier ;
- aucune retransmission ;
- expiration exactement 200 ms après le premier fragment accepté ;
- au premier fragment manquant à expiration, abandon intégral.

Pour une IDR :

- le producteur conserve uniquement les fragments de la dernière IDR pendant 500 ms après son premier envoi ;
- le client peut NACKer seulement les fragments manquants de cette IDR ;
- aucune retransmission n'est envoyée après needed_before_producer_time_us ni après la fenêtre de 500 ms ;
- une nouvelle IDR invalide la récupération de la précédente ;
- après expiration, le client abandonne le réassemblage et PEUT envoyer TARGET_VIDEO_KEYFRAME_REQUEST.

Une frame dupliquée ou ancienne est éliminée par le tuple session_id, stream_id, config_generation, video_frame_id. Un fragment dupliqué n'est accepté que s'il est byte-identique.

## 10. QoS, quotas et rate limits

### 10.1 Ordre strict de priorité

Le scheduler suit cet ordre, du plus haut au plus bas :

1. session, horloge, ACK/NACK, RESYNC_REQUEST, CAPABILITY_UPDATE, TARGET_VIDEO_SUBSCRIBE, TARGET_VIDEO_CONFIG, TARGET_VIDEO_STOP et TARGET_VIDEO_KEYFRAME_REQUEST ;
2. manifestes fiables, COMM_VIEW_EVENT, autres événements fiables et FULL_SNAPSHOT ;
3. DELTA et COMM_VIEW_STATE remplaçables ;
4. retransmissions de fragments de la dernière IDR ;
5. nouvelle IDR ;
6. interframes ;
7. TARGET_VIDEO_STATS.

La vidéo NE DOIT jamais retarder une classe 1 à 3. Une réservation ou queue vidéo ne peut évincer un message fiable ou un état. Si sendto retourne WOULD_BLOCK, le producteur abandonne d'abord STATS et interframes, puis les fragments non envoyés d'une nouvelle IDR ; il ne bloque jamais le thread de jeu.

### 10.2 Budgets vidéo

Chaque client possède un token bucket plafonné au bitrate_kbps négocié. Toutes les opérations suivantes sont effectuées en u64 ; chaque division entière arrondit vers le bas. La capacité, en octets, est :

    rate_bits_per_s = u64(bitrate_kbps) * 1000
    raw_burst_bytes = (rate_bits_per_s * 500) / (8 * 1000)
    capacity_bytes = min(2 097 152, max(262 144, raw_burst_bytes))

Le bucket est rempli avec des octets entiers à partir de l'horloge monotone. Pour un intervalle elapsed_us, l'implémentation conserve le reste de la division afin que le résultat ne dépende pas de la fréquence du scheduler :

    refill_numerator = rate_bits_per_s * elapsed_us + refill_remainder
    added_bytes = refill_numerator / 8 000 000
    next_remainder = refill_numerator % 8 000 000

Si `tokens_bytes + added_bytes` atteint ou dépasse capacity_bytes, tokens_bytes prend exactement capacity_bytes et refill_remainder redevient zéro. Sinon, tokens_bytes augmente de added_bytes et refill_remainder prend next_remainder.

refill_remainder est initialisé à zéro et reste strictement inférieur à 8 000 000. Avant la multiplication, elapsed_us est plafonné à `max_elapsed_us = (capacity_bytes * 8 000 000 + rate_bits_per_s - 1) / rate_bits_per_s` ; cette division arrondit elle aussi vers le bas et l'expression entière équivaut au plafond mathématique. Si ce plafond est atteint, le bucket est rempli à capacity_bytes et le reste devient zéro. Un nouveau flux démarre avec capacity_bytes jetons. Une reconfiguration conserve `min(tokens_bytes, nouvelle_capacity_bytes)` et le reste courant ; si cette valeur atteint la nouvelle capacité, le reste redevient zéro. Elle ne recrédite pas le burst initial. L'émission d'un datagramme vidéo de N octets n'est autorisée que si tokens_bytes est supérieur ou égal à N, puis soustrait exactement N.

Le producteur possède aussi un budget vidéo global configuré. Le budget global DOIT réserver en premier la bande passante maximale configurée pour les classes 1 à 3. Une IDR peut être partiellement abandonnée si les budgets sont épuisés ; sa présence dans le cache ne justifie jamais une attente bloquante.

Plusieurs clients demandant une configuration identique PEUVENT partager rendu et encodage, mais conservent sessions, stream_id, token buckets, NACK et quotas de réassemblage indépendants.

### 10.3 Rate limits minimaux

Une implémentation conforme applique au moins :

| Entrée | Limite soutenue | Burst |
|---|---:|---:|
| TARGET_VIDEO_SUBSCRIBE | 2 par seconde et par session | 2 |
| TARGET_VIDEO_STOP client | 4 par seconde et par session | 4 |
| TARGET_VIDEO_KEYFRAME_REQUEST | 2 par seconde et par stream | 1 |
| TARGET_VIDEO_STATS | 2 par seconde et par stream | 2 |
| CAPABILITY_UPDATE | 2 par seconde et par session | 2 |
| COMM_VIEW_EVENT producteur | 20 par seconde | 20 |
| COMM_VIEW_STATE périodique | 10 par seconde | 2 |

Les retransmissions fiables byte-identiques ne consomment pas une nouvelle opération métier, mais restent soumises aux limites de paquets de [03](03-session-horloges-fiabilite.md). Une entrée rate-limitée est ignorée ou reçoit une réponse compacte idempotente ; elle ne réserve aucune ressource coûteuse.

## 11. Cycle de vie du flux cible

Le client maintient un sous-état indépendant de ReplicaStore :

| État | Entrée | Sortie principale |
|---|---|---|
| Unsupported | paire de capabilities inactive | nouvelle session seulement |
| Stopped | paire active, aucun stream | SUBSCRIBE |
| Subscribing | SUBSCRIBE fiable en cours | CONFIG accepté ou refusé |
| Configured | CONFIG acceptée, ACK en cours | ACK puis première IDR |
| Live | au moins une frame décodable | perte, reconfiguration ou STOP |
| Stale | aucune progression décodable | IDR NACK/request ou STOP |

Règles :

1. aucun rendu ou encodage n'est créé avant ACK valide de WELCOME et SUBSCRIBE accepté ;
2. aucune frame d'une génération n'est envoyée avant ACK de sa CONFIG ;
3. la première frame d'un stream ou d'une génération est une IDR ;
4. une connexion tardive suit SUBSCRIBE, CONFIG, ACK, IDR ;
5. un changement de cible incrémente config_generation, purge l'ancienne cible et force CONFIG puis IDR ;
6. aucune cible produit STOP/NO_TARGET ;
7. un type non supporté produit STOP/UNSUPPORTED_TARGET et conserve les données numériques ;
8. perte renderer/readback/encodeur produit STOP, puis CAPABILITY_UPDATE si négocié ;
9. un client lent reçoit une CONFIG réduite ou perd des frames ; il ne bloque jamais le producteur ;
10. changement de mission ou session purge tous les réassemblages et décodeurs.

Le client peut conserver brièvement la dernière texture valide, mais la marque Stale au plus tard 500 ms après l'absence de progression décodable et affiche un placeholder au plus tard après 1000 ms. Les données numériques restent affichables. Une texture n'est jamais présentée après divergence de target_entity_id.

## 12. Validations obligatoires et fallback

### 12.1 Rejets locaux sans fermeture de session

Les situations suivantes désactivent seulement la vue concernée :

- bundle absent, version/hash/format incompatible ;
- asset ponctuellement absent ou hash local incorrect ;
- renderer, readback, encodeur ou décodeur absent ;
- absence de profil, niveau, render profile ou overlays communs ;
- résolution, cadence ou bitrate refusés ;
- type de cible non supporté ;
- CONFIG ou frame appartenant à une ancienne génération ;
- bitstream H.264 invalide.

Un message structurellement malformé suit la taxonomie de [06](06-validation-securite-et-conformite.md). Aucun échec visuel ne fait régresser l'état canonique Live.

### 12.2 Sécurité

Le bundle n'est jamais transféré automatiquement par UDP. Aucun chemin absolu reçu, aucun engine_message_id et aucun logical_name ne peut servir directement à ouvrir un fichier. Le chemin validé du manifeste est résolu sous une racine de bundle configurée.

SUBSCRIBE, NACK, KEYFRAME_REQUEST, STOP, STATS et CAPABILITY_UPDATE sont liés à la session et à son endpoint validé. Ils ne modifient aucune structure de simulation. Avant réception de `ACK APPLIED` pour `WELCOME`, aucun manifeste ni flux vidéo n'est envoyé et aucune ressource vidéo lourde n'est réservée.

Les logs NE DOIVENT PAS contenir une access unit, un chemin absolu ou un dump par frame. Ils PEUVENT contenir stream_id, génération, tailles, profils, compteurs de pertes, raison de fallback et hash tronqué pour diagnostic.

## 13. Critères de qualité des vues spécialisées

- les capabilities réservées, offertes, sélectionnées et retirées possèdent un résultat déterministe ;
- les offres de bundle distinguent version, hash, formats et absence de ressource ;
- chemins, UTF-8, IDs, hashes, timings, alpha et cardinalités sont validés avant installation ;
- `COMM_VIEW_STATE` représente activité, pause, vitesse, sens, offset et asset sans valeur non finie ;
- les tailles des messages spécialisés correspondent exactement aux layouts de ce document ;
- une configuration vidéo est acquittée avant la première IDR ;
- une frame H.264 respecte Annex B, son profil, son niveau et la limite de 2 097 152 octets ;
- une interframe incomplète expire à 200 ms et une IDR récupérable reste disponible au plus 500 ms ;
- une frame de l'ancienne cible ou génération n'est jamais présentée ;
- la télémétrie d'état conserve la priorité lorsque le budget vidéo est saturé ;
- une connexion tardive reçoit la configuration puis une IDR.

Les offsets, tailles et valeurs numériques concordent avec le schéma machine-readable. Deux décodeurs indépendants produisent le même résultat pour les golden vectors canoniques.
