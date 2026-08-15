# Contrat v1.0 — Format filaire, registres et enveloppes

## 1. Statut et portée

Ce document est normatif. Il fixe le format binaire commun à tous les datagrammes de télémétrie v1.0, les registres numériques, les enveloppes de payload, la fragmentation, les contrôles d'intégrité et les règles de compatibilité.

Les mots **DOIT**, **NE DOIT PAS**, **DEVRAIT**, **NE DEVRAIT PAS** et **PEUT** expriment respectivement une obligation, une interdiction, une recommandation forte, une pratique déconseillée et une possibilité.

Ce document complète :

- [la proposition de protocole UDP](../../03-udp-protocol.md) ;
- [la feuille de route, Phase 0](../../04-implementation-roadmap.md#2-phase-0--contrat-de-protocole) ;
- [le modèle de données v1](04-modele-de-donnees-v1.md), qui définit le payload métier de chaque record ;
- [les capabilities et vues spécialisées](05-capabilities-et-vues-specialisees.md), qui définissent les extensions de négociation et les payloads vidéo.

Les règles de session, d'horloge, de fiabilité, de baseline et de QoS sont normatives dans [03 — Session, horloges et fiabilité](03-session-horloges-fiabilite.md).

## 2. Conventions binaires

### 2.1 Types primitifs

| Notation | Taille | Encodage |
|---|---:|---|
| `u8` | 1 octet | entier non signé |
| `i8` | 1 octet | entier signé en complément à deux |
| `u16` | 2 octets | entier non signé little-endian |
| `i16` | 2 octets | entier signé little-endian, complément à deux |
| `u32` | 4 octets | entier non signé little-endian |
| `i32` | 4 octets | entier signé little-endian, complément à deux |
| `u64` | 8 octets | entier non signé little-endian |
| `i64` | 8 octets | entier signé little-endian, complément à deux |
| `float32` | 4 octets | IEEE-754 binary32 little-endian |
| `bytes[N]` | N octets | octets opaques, dans l'ordre indiqué |

Tous les champs multi-octets, y compris les enums et les flottants, **DOIVENT** être little-endian. Il n'existe aucun alignement implicite : les champs sont concaténés exactement dans l'ordre des tableaux de ce document, sans padding, même lorsqu'un entier commence à un offset non naturellement aligné.

Un booléen filaire est un `u8` fermé : `0` signifie faux et `1` signifie vrai. Toute autre valeur **DOIT** être rejetée.

Une enum est fermée par défaut : une valeur non listée invalide le message logique qui la contient. Seuls les champs explicitement déclarés « extensibles » — notamment les bitmaps de capabilities et certains bitmaps spécialisés — ignorent leurs bits inconnus. Un émetteur v1.0 met toujours les bits réservés à zéro.

Un `float32` non fini, NaN ou infini, **DOIT** être rejeté sauf autorisation explicite du champ par le modèle de données. Aucun champ v1.0 ne donne de sémantique au signe de zéro : un émetteur **DOIT** sérialiser tout zéro sous la forme `+0.0` (`0x00000000`) ; un lecteur accepte les deux motifs de zéro puis canonicalise immédiatement `-0.0` en `+0.0` dans la valeur publique.

### 2.2 Chaînes et tableaux

Une chaîne utilise :

| Champ | Type | Règle |
|---|---:|---|
| `byte_length` | `u16` | nombre exact d'octets qui suivent |
| `utf8_bytes` | `bytes[byte_length]` | UTF-8 valide, sans NUL terminal ajouté |

La longueur est une longueur en octets, pas en points de code. Le décodeur **DOIT** refuser l'UTF-8 invalide, un buffer tronqué et toute longueur supérieure à la borne métier du champ. Un NUL encodé à l'intérieur de la chaîne n'est permis que si le schéma du champ l'autorise explicitement ; par défaut il est interdit.

Une liste homogène de taille fixe, notée `list<T,N>`, utilise :

| Champ | Type | Règle |
|---|---:|---|
| `count` | `u16` | nombre d'éléments |
| `item_version` | `u8` | version commune aux éléments |
| `item_size` | `u16` | taille filaire exacte de chaque élément |
| `items` | `bytes[count * item_size]` | éléments contigus |

Le lecteur **DOIT** vérifier l'overflow de `count * item_size` avant toute allocation. La région `items` ne contient ni padding ni octet final.

Une liste d'éléments de tailles variables, notée `vlist<T,N>`, utilise :

| Champ | Type | Règle |
|---|---:|---|
| `count` | `u16` | nombre d'éléments |
| pour chaque élément : `item_version` | `u8` | version de cet élément |
| pour chaque élément : `item_size` | `u16` | taille de son payload |
| pour chaque élément : `item_payload` | `bytes[item_size]` | payload exact, sans padding |

Le lecteur vérifie chaque borne avant d'avancer et la somme totale avec arithmétique sans overflow. `N` est la borne métier maximale de `count` fixée par le document 04. Un schéma doit déclarer explicitement `list` ou `vlist` ; les deux encodages ne sont jamais autodétectés.

### 2.3 Valeurs absentes

Il n'existe aucune notion implicite de `null` sur le fil.

- L'ID d'entité `0` signifie toujours « aucune entité » et n'est jamais attribué.
- Un champ optionnel **DOIT** utiliser soit une sentinelle explicitement définie, soit un bitmap de présence défini par le schéma de son record.
- Une chaîne vide, une liste vide, zéro et un champ absent sont des états distincts, sauf règle métier explicite.
- Un champ réservé **DOIT** être émis à zéro et **DOIT** être rejeté s'il est non nul en v1.0.

### 2.4 Unités et repère

- Les temps réseau sont des microsecondes entières.
- `sent_time_us` et `producer_sample_time_us` utilisent une horloge monotone de processus.
- `mission_time_us` utilise le temps de simulation et peut se figer ou évoluer à un autre taux.
- Les positions et distances utilisent la world-unit FSO ; les vitesses utilisent la world-unit par seconde.
- Les angles sont en radians et les vitesses angulaires en radians par seconde.
- Le repère est direct : `+X` droite, `+Y` haut, `+Z` avant.
- Un quaternion est `float32[4]` dans l'ordre `(w, x, y, z)`, normalisé avant émission et canonicalisé avec `w >= 0` ; si `w == 0`, sa première composante non nulle parmi `x`, `y`, `z` est positive.

Les tolérances métier et conversions depuis le repère moteur sont définies dans [04 — Modèle de données v1](04-modele-de-donnees-v1.md).

## 3. Version du protocole

La présente spécification définit exclusivement la version **1.0** :

| Élément | Valeur |
|---|---:|
| `version_major` | `1` |
| `version_minor` | `0` |
| `header_size` | `68` |

Un changement est compatible dans une même version majeure seulement s'il :

- ajoute une valeur de message ou de record sans changer une valeur existante ;
- ajoute un bit de capability ou un type d'extension ignorable ;
- ajoute des champs à la fin d'un record dimensionné ;
- permet à une nouvelle implémentation d'émettre le format d'une version mineure négociée plus ancienne.

Changer la taille ou la signification d'un type primitif, un offset existant de l'en-tête, le CRC, la fragmentation, la sémantique d'une valeur numérique existante ou la représentation d'un champ existant exige une nouvelle version majeure.

La négociation choisit une version commune avant la session. Une fois la session acceptée, chaque datagramme **DOIT** porter exactement la version négociée. Un pair v1.0 **DOIT** rejeter un datagramme dont la majeure n'est pas `1`, dont la mineure n'est pas `0` ou dont `header_size != 68`. Il ne doit donc jamais recevoir « par surprise » un en-tête d'une mineure future.

Une future mineure de la majeure 1 pourra agrandir l'en-tête en ajoutant des octets après l'offset 67. Le champ `crc32` restera aux offsets 64 à 67 et couvrira alors tout `header_size`. Cette possibilité ne modifie pas le comportement strict d'un pair ayant négocié v1.0.

## 4. En-tête de datagramme v1.0

### 4.1 Magic et taille

Le magic est la séquence ASCII `FSTL` :

| Représentation | Valeur |
|---|---|
| octets sur le fil | `46 53 54 4c` |
| `u32` little-endian | `0x4c545346` |

La taille maximale d'un datagramme de télémétrie est de **1200 octets**, en-tête compris. Un datagramme v1.0 ne dépend jamais de la fragmentation IP.

~~~cpp
constexpr std::size_t TELEMETRY_MAX_DATAGRAM_SIZE = 1200;
constexpr std::size_t TELEMETRY_HEADER_SIZE_V1 = 68;
constexpr std::size_t TELEMETRY_MAX_FRAGMENT_PAYLOAD = 1132;
~~~

### 4.2 Layout exact

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | `magic` | `u32` | `0x4c545346` |
| 4 | `version_major` | `u8` | `1` |
| 5 | `version_minor` | `u8` | `0` |
| 6 | `message_type` | `u8` | registre `MessageType` |
| 7 | `flags` | `u8` | registre `MessageFlags` |
| 8 | `header_size` | `u16` | `68` |
| 10 | `payload_size` | `u16` | `0..1132` |
| 12 | `session_id` | `u64` | `0` seulement avant session ou rejet de négociation |
| 20 | `packet_sequence` | `u32` | séquence de datagramme par direction et session |
| 24 | `frame_id` | `u32` | capture logique d'état ; `0` si non applicable |
| 28 | `mission_time_us` | `i64` | temps de simulation ; `0` si aucune mission |
| 36 | `sent_time_us` | `u64` | heure monotone de l'émetteur |
| 44 | `message_id` | `u32` | ID du message logique ; non nul |
| 48 | `fragment_index` | `u16` | index base zéro |
| 50 | `fragment_count` | `u16` | nombre total, au moins `1` |
| 52 | `message_size` | `u32` | taille du payload logique complet |
| 56 | `fragment_offset` | `u32` | offset canonique du fragment |
| 60 | `message_crc32` | `u32` | CRC du payload logique complet |
| 64 | `crc32` | `u32` | CRC de l'en-tête et du fragment |

La taille UDP reçue **DOIT** être exactement `header_size + payload_size`. Aucun octet supplémentaire n'est toléré.

### 4.3 MessageFlags

| Bit | Valeur | Nom | Règle v1.0 |
|---:|---:|---|---|
| 0 | `0x01` | `FRAGMENTED` | présent si et seulement si `fragment_count > 1` |
| 1 | `0x02` | `ACK_REQUIRED` | le message exige l'ACK défini pour sa classe |
| 2 | `0x04` | `KEYFRAME` | seulement sur `FullSnapshot` ; obligatoire sur chaque part |
| 3 | `0x08` | `VIDEO_IDR` | seulement sur une frame vidéo IDR |
| 4 | `0x10` | `RETRANSMISSION` | ce datagramme retransmet une tranche déjà envoyée |
| 5–7 | `0xe0` | réservé | zéro |

Les bits inconnus sont une erreur de datagramme en v1.0. `KEYFRAME` et `VIDEO_IDR` sont mutuellement exclusifs. `RETRANSMISSION` ne change pas l'identité du message logique.

### 4.4 Matrice des champs contextuels

`F` signifie que `FRAGMENTED` est ajouté si et seulement si le message possède plusieurs fragments ; `R` signifie que `RETRANSMISSION` est ajouté seulement sur une retransmission.

| Message/classe | `session_id` | `frame_id` | `mission_time_us` | Flags de base |
|---|---:|---:|---:|---|
| `Discovery` | 0 | 0 | 0 | aucun |
| `Hello`, `Welcome` rejeté | 0 | 0 | 0 | R |
| `Welcome` accepté | non nul | 0 | 0 | `ACK_REQUIRED` + R |
| `SessionBegin`, `SessionEnd` | non nul | 0 | 0 | `ACK_REQUIRED` + R |
| part `Manifest` | non nul | 0 | 0 | `ACK_REQUIRED` + F + R |
| part `FullSnapshot` | non nul | capture non nulle, identique pour toutes les parts | valeur de la capture | `ACK_REQUIRED + KEYFRAME` + F + R |
| `Delta` | non nul | capture non nulle | valeur de la capture | F |
| `EventBatch Replaceable` | non nul | capture non nulle | valeur de la capture | F |
| `EventBatch Reliable` | non nul | capture non nulle | valeur de la capture | `ACK_REQUIRED` + F + R |
| `Heartbeat`, `Ack`, `Nack` | non nul | 0 | 0 | aucun |
| `ResyncRequest`, `CapabilityUpdate` | non nul | 0 | 0 | `ACK_REQUIRED` + R |
| contrôles vidéo fiables 14, 15, 17, 18 | non nul | 0 | 0 | `ACK_REQUIRED` + R |
| `TargetVideoFrame` interframe | non nul | 0 | 0 | F |
| `TargetVideoFrame` IDR | non nul | 0 | 0 | `VIDEO_IDR` + F ; ajouter R seulement aux fragments retransmis |
| `TargetVideoStats` | non nul | 0 | 0 | aucun |

La valeur de `mission_time_us` est répétée à l'identique dans tous les fragments et toutes les parts d'une même capture. Elle vaut zéro hors mission. `VIDEO_IDR` est présent sur chaque fragment, initial ou retransmis, si et seulement si `TargetVideoFrame.video_flags.IDR` est présent ; `RETRANSMISSION` s'ajoute seulement aux fragments effectivement renvoyés. `sent_time_us` reste toujours l'heure monotone effective de chaque émission, y compris lorsque `mission_time_us == 0`.

## 5. Contrôles d'intégrité

Les deux champs CRC utilisent **CRC-32/ISO-HDLC** :

| Paramètre | Valeur |
|---|---|
| polynôme normal | `0x04c11db7` |
| polynôme réfléchi | `0xedb88320` |
| init | `0xffffffff` |
| refin / refout | vrai / vrai |
| xorout | `0xffffffff` |
| check de `123456789` | `0xcbf43926` |

`message_crc32` est calculé sur exactement les `message_size` octets du payload logique avant fragmentation. Le CRC d'un payload logique vide est `0x00000000`.

`crc32` est calculé sur :

1. les `header_size` octets de l'en-tête, avec les octets 64 à 67 remplacés par zéro ;
2. les `payload_size` octets du fragment.

Le récepteur **DOIT** valider `crc32` avant de conserver un fragment. Il **DOIT** valider `message_crc32` après réassemblage et avant tout parsing du payload logique.

Un CRC protège contre une corruption accidentelle ; il ne fournit ni authenticité ni confidentialité.

## 6. Registre MessageType

`MessageType` est un `u8` fermé pour les valeurs connues. Les valeurs suivantes sont définitives en v1.0 :

| Valeur | Nom filaire | Direction principale | Payload |
|---:|---|---|---|
| 0 | `Invalid` | aucune | interdit |
| 1 | `Discovery` | producteur → destinations configurées | `DiscoveryPayload` |
| 2 | `Hello` | client → producteur | `HelloPayload` |
| 3 | `Welcome` | producteur → client | `WelcomePayload` |
| 4 | `SessionBegin` | producteur → client | `SessionBeginPayload` |
| 5 | `Manifest` | producteur → client | `ManifestPartPayload` |
| 6 | `FullSnapshot` | producteur → client | `FullSnapshotPartPayload` |
| 7 | `Delta` | producteur → client | `DeltaPayload` |
| 8 | `EventBatch` | producteur → client | `EventBatchPayload` |
| 9 | `Heartbeat` | bidirectionnel | `HeartbeatPayload` |
| 10 | `Ack` | bidirectionnel | `AckPayload` |
| 11 | `Nack` | bidirectionnel | `NackPayload` |
| 12 | `ResyncRequest` | client → producteur | `ResyncRequestPayload` |
| 13 | `SessionEnd` | producteur → client | `SessionEndPayload` |
| 14 | `TargetVideoSubscribe` | client → producteur | document 05 |
| 15 | `TargetVideoConfig` | producteur → client | document 05 |
| 16 | `TargetVideoFrame` | producteur → client | document 05 |
| 17 | `TargetVideoKeyframeRequest` | client → producteur | document 05 |
| 18 | `TargetVideoStop` | bidirectionnel | document 05 |
| 19 | `TargetVideoStats` | client → producteur | document 05 |
| 20 | `CapabilityUpdate` | bidirectionnel | `CapabilityUpdatePayload` |

Une valeur supérieure à 20 est inconnue en v1.0. Après validation de l'en-tête, de `crc32`, de la session et de l'endpoint, elle **DOIT** être abandonnée sans réassemblage ni allocation proportionnelle à `message_size`. Si `ACK_REQUIRED` est présent, le récepteur envoie au plus un `Nack UnsupportedMessage` rate-limité pour le tuple `(session_id, endpoint, message_type, message_id, message_crc32)` ; sinon il reste silencieux. `Invalid` est toujours rejeté.

## 7. Registre RecordType et enveloppe

### 7.1 RecordType

`RecordType` est un `u16`. Les valeurs sont assignées dans l'ordre de [l'inventaire du protocole](../../03-udp-protocol.md#6-catégories-de-records) :

| Valeur | Nom |
|---:|---|
| 0 | `Invalid` |
| 1 | `SessionState` |
| 2 | `MissionState` |
| 3 | `ClassManifest` |
| 4 | `WeaponManifest` |
| 5 | `EntityLifecycle` |
| 6 | `ShipIdentity` |
| 7 | `FlightState` |
| 8 | `ControlState` |
| 9 | `DamageState` |
| 10 | `ShieldState` |
| 11 | `SubsystemState` |
| 12 | `EnergyState` |
| 13 | `PropulsionState` |
| 14 | `WeaponState` |
| 15 | `LockState` |
| 16 | `TargetState` |
| 17 | `RadarState` |
| 18 | `RadarContacts` |
| 19 | `ThreatState` |
| 20 | `CargoScanState` |
| 21 | `DockingState` |
| 22 | `SupportState` |
| 23 | `NavigationState` |
| 24 | `EffectState` |
| 25 | `CommAssetManifest` |
| 26 | `CommViewState` |
| 27 | `CommViewEvent` |
| 28 | `Events` |

Les valeurs 29 à 65535 sont réservées à des extensions compatibles. Un type inconnu dont l'enveloppe est valide **DOIT** être sauté grâce à `record_length`. La validation sémantique du conteneur échoue toutefois si ce type est déclaré requis pour le mode/capability négocié. `Invalid` rend le message logique invalide.

### 7.2 Enveloppe Record

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | `record_type` | `u16` | registre `RecordType` |
| 2 | `record_version` | `u8` | `1` pour les records v1.0 |
| 3 | `record_flags` | `u8` | registre `RecordFlags` |
| 4 | `record_length` | `u16` | taille du seul `record_payload` |
| 6 | `record_payload` | `bytes[record_length]` | payload défini par le document 04 |

`record_length` n'inclut pas les six octets de l'enveloppe. Un record ne traverse jamais une part de transaction `Manifest` ou `FullSnapshot`.

Un record lié à une entité commence par `entity_id: u64`. Les records globaux sont déclarés explicitement comme tels par le document 04. `entity_id == 0` n'est jamais une clé d'entité valide.

### 7.3 RecordFlags et présence

| Bit | Valeur | Nom | Sémantique |
|---:|---:|---|---|
| 0 | `0x01` | `CREATE` | création explicite de la clé du record |
| 1 | `0x02` | `DELETE` | suppression explicite de la clé du record |
| 2 | `0x04` | `PARTIAL` | seuls les champs déclarés présents sont remplacés |
| 3–7 | `0xf8` | réservé | zéro |

`CREATE` et `DELETE` sont mutuellement exclusifs. `CREATE | PARTIAL` est interdit : une création doit fournir l'état complet requis. `DELETE` ne peut être combiné à `PARTIAL` et son payload doit être exactement la clé de suppression définie par le document 04.

`record_flags == 0` signifie un upsert complet à la granularité du record : la valeur courante de ce record remplace sa valeur de baseline. Le bit `PARTIAL` est réservé à une évolution compatible mais **interdit pour les 28 records v1.0** ; sa réception provoque le rejet du record. Les masques de présence du document 04 décrivent une valeur complète, pas un patch.

Dans un `FullSnapshot`, tous les records portent `record_flags == 0`. Un `Delta` peut utiliser `CREATE` ou `DELETE` seulement pour les types explicitement autorisés par le document 04.

### 7.4 Évolution d'un record

Dans une session négociée 1.0, tout `RecordType` connu **DOIT** porter `record_version == 1` et le layout v1.0 exact défini dans le document 04. Une extension compatible dans une future mineure peut uniquement ajouter des champs à la fin du layout de version 1 et n'est émise que si cette mineure a été négociée. Une implémentation future qui négocie 1.0 **DOIT** réémettre strictement le format 1.0, sans tail futur. Un lecteur de la future mineure vérifie le préfixe connu puis saute exactement la queue que cette mineure autorise.

Une valeur `record_version` inconnue est traitée comme un record inconnu : son enveloppe est sautée, puis la validation sémantique échoue seulement si le record est requis. Elle n'est jamais interprétée comme automatiquement préfixe-compatible. Une modification incompatible crée une nouvelle `record_version`, un nouveau `RecordType` ou une nouvelle majeure selon sa portée.

## 8. Enveloppes de records

Une région de records contient exactement `record_count` enveloppes concaténées. Le parseur **DOIT** consommer exactement la région jusqu'à la fin du payload logique. Il rejette :

- moins ou plus de records que `record_count` ;
- un record tronqué ;
- une somme de longueurs en overflow ;
- un octet résiduel après le dernier record ;
- un record dont la borne métier est dépassée.

Les records peuvent apparaître dans n'importe quel ordre à l'intérieur d'un message complet. Les contraintes d'unicité de clés et de dépendances sont validées avant publication atomique.

## 9. Payloads de négociation et de contrôle

Les payloads fixes explicitement détaillés aux sections 9.2 à 9.11 sont sans enveloppe Record et **DOIVENT** tenir dans un unique fragment. `Manifest`, `FullSnapshot`, `Delta` et `EventBatch` suivent la section 10. Les types spécialisés 14 à 19 suivent le document 05 ; en particulier, `TargetVideoFrame` peut être fragmenté jusqu'aux limites vidéo de la section 11.4.

### 9.1 Capabilities communes

Une capability est un bit d'un `u64` extensible :

| Bit | Valeur | Nom | Rôle |
|---:|---:|---|---|
| 0 | `0x0000000000000001` | `COMM_VIEW_LOCAL_ASSETS` | consommateur de vue de communication |
| 1 | `0x0000000000000002` | `COMM_VIEW_AUTHORITATIVE_SOURCE` | producteur de vue de communication |
| 2 | `0x0000000000000004` | `TARGET_VIDEO_H264` | consommateur vidéo H.264 |
| 3 | `0x0000000000000008` | `TARGET_VIDEO_REMOTE_RENDER` | producteur de rendu vidéo |
| 4 | `0x0000000000000010` | `CAPABILITY_UPDATE` | mises à jour dynamiques |
| 5–63 | — | réservé | émis à zéro en v1.0 |

Une fonction spécialisée n'est active que si les deux rôles complémentaires sont annoncés. `active_capabilities` contient alors les deux bits de la paire. `CAPABILITY_UPDATE` n'est actif que si les deux pairs l'annoncent.

Un récepteur ignore les bits de capability inconnus. Il ne les recopie jamais dans `active_capabilities`.

Les paramètres de capability utilisent une suite bornée d'extensions :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `extension_type` | `u16` |
| 2 | `extension_version` | `u8` |
| 3 | `extension_flags` | `u8`, zéro en v1.0 |
| 4 | `extension_length` | `u16` |
| 6 | `extension_payload` | `bytes[extension_length]` |

`extension_type == 0` est invalide ; `1` est réservé aux paramètres de communication et `2` aux paramètres vidéo. Le document 05 en fixe le contenu. Une extension inconnue est sautée. La somme des extensions d'un message de contrôle est limitée à 768 octets.

Dans un même payload, `extension_type` est unique ; un doublon, même avec une autre version, invalide le payload. `extension_count` est au plus `128`, car chaque enveloppe occupe au moins six octets, et doit conduire exactement à `extensions_length` sans octet résiduel.

### 9.2 DiscoveryPayload

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | `producer_id` | `u64` | non nul, persistant dans le profil producteur |
| 8 | `listen_port` | `u16` | `1..65535`, port annoncé |
| 10 | `min_major` | `u8` | `1` |
| 11 | `max_major` | `u8` | `1` |
| 12 | `min_minor` | `u8` | `0` |
| 13 | `max_minor` | `u8` | `0` |
| 14 | `producer_capabilities` | `u64` | offre producteur |
| 22 | `advert_sequence` | `u32` | séquence d'annonce |
| 26 | `producer_name_length` | `u16` | `0..64` |
| 28 | `producer_name` | UTF-8 | nom de diagnostic |

Le préfixe fixe mesure 28 octets et le payload complet mesure au plus 92 octets. `producer_name` contient exactement `producer_name_length` octets UTF-8 valides, sans NUL. Le message n'est jamais fragmenté. L'en-tête porte `session_id = 0`, `frame_id = 0` et `mission_time_us = 0`. La découverte ne vaut jamais autorisation ni ouverture de session.

### 9.3 HelloPayload

Préfixe fixe de 38 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | `client_nonce` | `u64` | aléatoire non nul |
| 8 | `client_send_t0_us` | `u64` | heure monotone client |
| 16 | `min_major` | `u8` | `1` |
| 17 | `max_major` | `u8` | `1` |
| 18 | `min_minor` | `u8` | `0` |
| 19 | `max_minor` | `u8` | `0` |
| 20 | `requested_visibility_mode` | `u8` | `VisibilityMode` demandé |
| 21 | `reserved` | `bytes[3]` | zéro |
| 24 | `advertised_capabilities` | `u64` | capabilities du client |
| 32 | `requested_heartbeat_ms` | `u16` | `200..5000` |
| 34 | `extensions_length` | `u16` | somme exacte des extensions |
| 36 | `extension_count` | `u16` | nombre exact |
| 38 | `extensions` | bytes | enveloppes de capability |

`VisibilityMode` est fermé à l'unique valeur `0 COCKPIT`; les valeurs `1..255`
sont réservées et rejetées. Le payload total est limité à 806 octets. Le
`Hello` initial porte `session_id = 0` et n'est pas fragmenté.

### 9.4 WelcomePayload

Préfixe fixe de 68 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | `client_nonce` | `u64` | recopie du `Hello` |
| 8 | `client_send_t0_us` | `u64` | recopie du `Hello` |
| 16 | `producer_receive_t1_us` | `u64` | réception producteur |
| 24 | `producer_send_t2_us` | `u64` | émission producteur |
| 32 | `status` | `u8` | `WelcomeStatus` |
| 33 | `selected_major` | `u8` | `1` si accepté |
| 34 | `selected_minor` | `u8` | `0` si accepté |
| 35 | `selected_visibility_mode` | `u8` | `VisibilityMode` effectivement accordé |
| 36 | `producer_capabilities` | `u64` | capabilities annoncées |
| 44 | `active_capabilities` | `u64` | sous-ensemble négocié |
| 52 | `heartbeat_interval_ms` | `u16` | `200..5000` si accepté |
| 54 | `reliable_reassembly_timeout_ms` | `u16` | `2000` en v1.0 |
| 56 | `extensions_length` | `u16` | somme exacte |
| 58 | `extension_count` | `u16` | nombre exact |
| 60 | `producer_id` | `u64` | identité persistante du profil producteur |
| 68 | `extensions` | bytes | paramètres sélectionnés |

`WelcomeStatus` :

| Valeur | Nom |
|---:|---|
| 0 | `Accepted` |
| 1 | `UnsupportedVersion` |
| 2 | `Unauthorized` |
| 3 | `Busy` |
| 4 | `InvalidCapabilities` |

Un `Welcome Accepted` porte un `session_id` aléatoire non nul. `selected_visibility_mode` est au plus aussi permissif que la configuration autorisée par le producteur. Un rejet porte `session_id = 0`, `selected_major = selected_minor = 0`, `selected_visibility_mode = COCKPIT`, `producer_capabilities = active_capabilities = 0`, `heartbeat_interval_ms = reliable_reassembly_timeout_ms = 0`, aucune extension et aucune obligation d'ACK. `producer_id` reste renseigné.

### 9.5 SessionBeginPayload

Payload fixe de 28 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `session_flags` | `u32` |
| 4 | `producer_session_start_us` | `u64` |
| 12 | `mission_instance_id` | `u64` |
| 20 | `initial_snapshot_id` | `u32` |
| 24 | `required_manifest_id` | `u32` |

`SessionBeginFlags` :

| Bit | Valeur | Nom |
|---:|---:|---|
| 0 | `0x00000001` | `READ_ONLY`, obligatoire |
| 1 | `0x00000002` | `MISSION_ACTIVE` |
| 2 | `0x00000004` | `MANIFEST_REQUIRED` |
| 3–31 | — | réservé, zéro |

`mission_instance_id == 0` signifie qu'aucune mission n'est active. `initial_snapshot_id` vaut zéro si aucun snapshot n'est annoncé immédiatement ; sinon il identifie la transaction initiale attendue. `required_manifest_id` vaut zéro lorsqu'aucun manifeste n'est requis ; sinon le client doit installer cette transaction de manifeste avant le snapshot annoncé.

`READ_ONLY` est toujours présent. `MISSION_ACTIVE` est présent si et seulement si `mission_instance_id != 0`. `MANIFEST_REQUIRED` est présent si et seulement si `required_manifest_id != 0`. Si `initial_snapshot_id == 0`, le client reste `Synchronizing` jusqu'à l'annonce ultérieure d'un `FullSnapshot`; une session ne devient jamais `Live` sans snapshot.

### 9.6 HeartbeatPayload

Payload fixe de 32 octets :

| Offset | Champ | Type | Règle |
|---:|---|---:|---|
| 0 | `probe_id` | `u32` | compteur non nul de l'initiateur |
| 4 | `kind` | `u8` | `1 Request`, `2 Response` |
| 5 | `reserved` | `bytes[3]` | zéro |
| 8 | `origin_t0_us` | `u64` | émission de la requête |
| 16 | `receive_t1_us` | `u64` | zéro en requête |
| 24 | `transmit_t2_us` | `u64` | zéro en requête |

Une réponse recopie `probe_id` et `origin_t0_us`, impose `transmit_t2_us >= receive_t1_us` et renseigne les deux temps producteur. Dans une requête, les deux temps de réponse valent zéro. Les règles de calcul sont dans le document 03.

### 9.7 AckPayload

Payload fixe de 12 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `target_message_id` | `u32` |
| 4 | `target_message_type` | `u8` |
| 5 | `ack_flags` | `u8` |
| 6 | `target_fragment_count` | `u16` |
| 8 | `target_message_crc32` | `u32` |

`AckFlags` :

| Bit | Valeur | Nom |
|---:|---:|---|
| 0 | `0x01` | `VALIDATED` |
| 1 | `0x02` | `APPLIED` |
| 2–7 | — | réservé |

`APPLIED` implique `VALIDATED` : un ACK appliqué porte donc `ack_flags == 0x03`. `ack_flags == 0`, `0x02` seul et tout bit inconnu sont invalides.

### 9.8 NackPayload

Préfixe fixe de 24 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `target_message_id` | `u32` |
| 4 | `target_message_type` | `u8` |
| 5 | `reason` | `u8` |
| 6 | `target_fragment_count` | `u16` |
| 8 | `target_message_crc32` | `u32` |
| 12 | `needed_before_producer_time_us` | `u64` |
| 20 | `bitmap_bytes` | `u16` |
| 22 | `reserved` | `u16`, zéro |
| 24 | `missing_bitmap` | `bytes[bitmap_bytes]` |

`NackReason` :

| Valeur | Nom |
|---:|---|
| 0 | `Invalid` |
| 1 | `MissingFragments` |
| 2 | `BadMessageCrc` |
| 3 | `StaleBaseline` |
| 4 | `BadFragmentLayout` |
| 5 | `ResourceLimit` |
| 6 | `UnsupportedMessage` |
| 7 | `SemanticValidationFailed` |
| 8 | `DeadlineExpired` |

Pour `MissingFragments`, `bitmap_bytes == ceil(target_fragment_count / 8)`, avec un maximum de 256. Le bit `i`, poids faible d'abord dans son octet, vaut 1 si le fragment `i` manque. Les bits au-delà de `target_fragment_count` sont zéro. Pour toute autre raison, `bitmap_bytes == 0`.

`needed_before_producer_time_us` vaut zéro sauf pour une donnée à deadline, notamment une IDR vidéo.

### 9.9 ResyncRequestPayload

Payload fixe de 24 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `request_id` | `u32`, non nul |
| 4 | `reason` | `u8` |
| 5 | `request_flags` | `u8` |
| 6 | `reserved` | `u16`, zéro |
| 8 | `last_applied_snapshot_id` | `u32` |
| 12 | `last_applied_delta_sequence` | `u32` |
| 16 | `client_send_time_us` | `u64` |

`ResyncReason` :

| Valeur | Nom |
|---:|---|
| 0 | `Invalid` |
| 1 | `UnknownBaseline` |
| 2 | `ValidationFailed` |
| 3 | `ReassemblyTimeout` |
| 4 | `SessionStale` |
| 5 | `Manual` |

`ResyncRequestFlags` : bit 0 `REQUIRE_MANIFEST`, bit 1 `REQUIRE_FULL_SNAPSHOT` ; les autres bits sont zéro. `REQUIRE_FULL_SNAPSHOT` est obligatoire dans toute requête v1.0. Les deux IDs de dernier état peuvent valoir zéro ; si `last_applied_snapshot_id == 0`, `last_applied_delta_sequence` vaut également zéro.

### 9.10 SessionEndPayload

Payload fixe de 16 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `reason` | `u8` |
| 1 | `end_flags` | `u8` |
| 2 | `reserved` | `u16`, zéro |
| 4 | `last_snapshot_id` | `u32` |
| 8 | `producer_sample_time_us` | `u64` |

`SessionEndReason` : `1 Normal`, `2 ProducerShutdown`, `3 MissionEnded`, `4 Restart`, `5 ProtocolError`, `6 Timeout`. Zéro est invalide. Le bit 0 de `end_flags`, `RECONNECT_ALLOWED`, autorise un nouveau `Hello` ; les autres bits sont zéro.

### 9.11 CapabilityUpdatePayload

Préfixe fixe de 36 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `capability_generation` | `u32` |
| 4 | `advertised_capabilities` | `u64` |
| 12 | `active_capabilities` | `u64` |
| 20 | `effective_time_us` | `u64` |
| 28 | `reason` | `u8` |
| 29 | `reserved` | `bytes[3]`, zéro |
| 32 | `extensions_length` | `u16` |
| 34 | `extension_count` | `u16` |
| 36 | `extensions` | bytes |

`CapabilityUpdateReason` : `1 RuntimeAvailability`, `2 PeerRequest`, `3 ConfigurationChange`, `4 ErrorRecovery` ; zéro est invalide.

En v1.0, `extensions_length` et `extension_count` **DOIVENT** être zéro et aucun octet ne suit le préfixe. Une future extension exige une version mineure qui la définit et qui a été négociée.

Une mise à jour v1.0 ne peut que retirer des bits : `advertised_capabilities` et `active_capabilities` sont des sous-ensembles des valeurs précédemment acceptées. Ajouter/réactiver une capability ou changer un bundle exige une nouvelle session. Les règles de propriété et de convergence sont fixées dans le document 03.

## 10. Messages à records

### 10.1 Transaction paginée

Un manifeste ou snapshot exhaustif peut dépasser la limite d'un message logique. v1.0 utilise donc des transactions paginées :

| Constante | Valeur |
|---|---:|
| `TELEMETRY_MAX_STATE_PART_SIZE` | 1 048 576 octets, en-tête de part compris |
| `TELEMETRY_MAX_TRANSACTION_SIZE` | 16 777 216 octets de régions de records |
| `TELEMETRY_MAX_TRANSACTION_PARTS` | 64 |
| `TELEMETRY_MAX_CANDIDATE_TRANSACTIONS_PER_CLIENT` | 2 : un manifeste et un snapshot |
| `TELEMETRY_MAX_CANDIDATE_TRANSACTION_BYTES_PER_CLIENT` | 33 554 432 octets |
| `TELEMETRY_TRANSACTION_ASSEMBLY_TIMEOUT_MS` | 10 000 |

Chaque part est un message logique indépendant, avec son propre `message_id`, sa propre fragmentation et son propre `message_crc32`. Une part reste inférieure ou égale à 1 Mio. Aucun record ne traverse deux parts.

Une transaction v1.0 n'est jamais vide : `transaction_size` est dans `1..TELEMETRY_MAX_TRANSACTION_SIZE`, chaque part contient au moins un record et une région de records non vide. Le cas générique `transaction_size == 0` est invalide ; aucun `MessageType` v1.0 ne l'emploie.

`transaction_size` est la somme des seules régions de records, dans l'ordre des `part_index`. `transaction_sha256` est le SHA-256 de leur concaténation exacte, sans les préfixes de part ni padding. Toutes les parts répètent les mêmes identifiants, taille, hash, heure d'échantillon et flags transactionnels.

Le récepteur valide chaque part isolément, puis la transaction complète. Il n'expose aucun record avant commit atomique. Les ACK transactionnels sont définis dans le document 03.

### 10.2 ManifestPartPayload

Préfixe fixe de 56 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `manifest_id` | `u32` |
| 4 | `part_index` | `u16` |
| 6 | `part_count` | `u16` |
| 8 | `transaction_size` | `u32` |
| 12 | `transaction_sha256` | `bytes[32]` |
| 44 | `producer_sample_time_us` | `u64` |
| 52 | `manifest_kind` | `u16` |
| 54 | `record_count` | `u16` |
| 56 | `records` | bytes |

`ManifestKind` n'admet que `1 FullRequired` en v1.0. Les valeurs 0 et 2 à 65535 sont invalides/réservées. Chaque nouvel `manifest_id` transporte un manifeste exhaustif autonome ; il ne dépend d'aucun manifeste de base.

`manifest_id` est strictement croissant dans la session. `part_count` est dans `1..64`, `part_index < part_count`, `record_count` est dans `1..65535` et aucune part vide n'est émise.

### 10.3 FullSnapshotPartPayload

Préfixe fixe de 60 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `snapshot_id` | `u32` |
| 4 | `part_index` | `u16` |
| 6 | `part_count` | `u16` |
| 8 | `transaction_size` | `u32` |
| 12 | `transaction_sha256` | `bytes[32]` |
| 44 | `producer_sample_time_us` | `u64` |
| 52 | `required_manifest_id` | `u32` |
| 56 | `snapshot_flags` | `u16` |
| 58 | `record_count` | `u16` |
| 60 | `records` | bytes |

`SnapshotFlags` : bit 0 `INITIAL`, bit 1 `PERIODIC_KEYFRAME`, bit 2 `RESYNC` ; les autres bits sont zéro. Une transaction doit avoir exactement un de ces trois bits. `snapshot_id` est strictement croissant dans la session. `record_count` est dans `1..65535` et aucune part vide n'est émise. Toutes les parts répètent le même `required_manifest_id`. Le client ne peut valider puis committer le snapshot que si ce manifeste est déjà installé ; un changement de manifeste exige une nouvelle keyframe qui référence le nouvel ID.

### 10.4 DeltaPayload

Préfixe fixe de 20 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `baseline_snapshot_id` | `u32` |
| 4 | `delta_sequence` | `u32` |
| 8 | `producer_sample_time_us` | `u64` |
| 16 | `record_count` | `u16` |
| 18 | `reserved` | `u16`, zéro |
| 20 | `records` | bytes |

`baseline_snapshot_id`, `delta_sequence` et `record_count` sont non nuls. Un delta est cumulatif depuis `baseline_snapshot_id`. Sa taille logique maximale est 1 Mio. Il n'est jamais paginé : si l'état cumulatif ne tient pas, le producteur déclenche une nouvelle transaction `FullSnapshot`.

### 10.5 EventBatchPayload

Préfixe fixe de 24 octets :

| Offset | Champ | Type |
|---:|---|---:|
| 0 | `batch_id` | `u32` |
| 4 | `first_event_id` | `u64` |
| 12 | `producer_sample_time_us` | `u64` |
| 20 | `delivery_class` | `u8` |
| 21 | `reserved` | `u8`, zéro |
| 22 | `record_count` | `u16` |
| 24 | `records` | bytes |

`batch_id`, `first_event_id` et `record_count` sont non nuls. `delivery_class == 1` signifie `Replaceable` et interdit `ACK_REQUIRED`. `delivery_class == 2` signifie `Reliable` et exige `ACK_REQUIRED`. Les autres valeurs sont invalides. `first_event_id` est le plus petit `event_id` présent dans les records du batch.

## 11. Fragmentation applicative

### 11.1 Construction canonique

Soit `L = 1132` en v1.0.

Pour `message_size > 0` :

~~~text
fragment_count = ceil(message_size / L)
fragment_offset(i) = i * L
payload_size(i) = min(L, message_size - fragment_offset(i))
~~~

Pour le seul cas `message_size == 0` :

~~~text
fragment_count = 1
fragment_index = 0
fragment_offset = 0
payload_size = 0
message_crc32 = 0
~~~

Tous les fragments sauf le dernier ont exactement 1132 octets. Le fragment final ne peut pas être vide pour un message non vide.

### 11.2 Cohérence inter-fragments

Pour un même message logique, tous les fragments **DOIVENT** avoir les mêmes :

- version, `message_type` et flags hors `RETRANSMISSION` ;
- `session_id`, `frame_id` et `mission_time_us` ;
- `message_id`, `fragment_count`, `message_size` et `message_crc32`.

`packet_sequence`, `sent_time_us`, `fragment_index`, `fragment_offset`, `payload_size`, `crc32` et le bit `RETRANSMISSION` sont propres au datagramme.

Un doublon du même `fragment_index` est idempotent seulement si sa tranche est octet pour octet identique. Un doublon contradictoire, un chevauchement, un offset non canonique ou une incohérence d'un champ commun invalide et libère tout le réassemblage. Un pair de session peut alors envoyer `Nack BadFragmentLayout` pour un message fiable.

### 11.3 Ordre de validation avant allocation

Le lecteur **DOIT**, dans cet ordre :

1. vérifier que le datagramme contient au moins 68 octets ;
2. vérifier magic, version, `header_size` et taille UDP exacte ;
3. vérifier `payload_size <= 1132` et `crc32` ;
4. vérifier le type, la classe de taille et les flags ;
5. calculer le `fragment_count` attendu et vérifier index, offset et taille canonique avec arithmétique sans overflow ;
6. vérifier les quotas de l'endpoint et réserver une entrée ;
7. seulement alors allouer ou copier la tranche ;
8. après complétude, vérifier `message_crc32` ;
9. parser et valider le payload logique ;
10. publier atomiquement selon sa classe.

### 11.4 Limites et quotas de réassemblage

| Classe | Taille logique max | Fragments max | Réassemblages simultanés/client | Octets réservés/client |
|---|---:|---:|---:|---:|
| contrôle, état et part fiable | 1 048 576 | 1024 | 4 | 4 194 304 |
| `TargetVideoFrame` | 2 097 152 | 2048 | 3 | 6 291 456 |

Les quotas de transaction paginée de la section 10.1 sont distincts des buffers de réassemblage. Une réservation dépassant un quota est rejetée sans évincer un message fiable plus ancien au profit d'une vidéo.

Un message de contrôle non autorisé à fragmenter est rejeté si `fragment_count != 1`.

## 12. Règles de validation communes

Un `PacketReader` borné **DOIT** :

- vérifier chaque addition, multiplication et conversion de taille ;
- ne jamais exposer un pointeur au-delà du buffer reçu ;
- distinguer `Truncated`, `Malformed`, `Unsupported`, `IntegrityFailure`, `ResourceLimit` et `SemanticFailure` ;
- ne jamais allouer selon une longueur non validée ;
- rejeter tout champ réservé non nul ;
- consommer exactement tout payload fixe ou toute région dimensionnée ;
- ne jamais appliquer un message partiel.

Un `PacketWriter` borné **DOIT** :

- refuser une écriture excédant sa capacité ;
- écrire explicitement en little-endian, sans copie brute d'une structure C/C++ ;
- canonicaliser les valeurs requises avant CRC ;
- produire un résultat déterministe, octet pour octet, pour des entrées identiques.

Les fichiers golden v1.0 **DOIVENT** inclure au minimum :

- un exemplaire de chaque payload de contrôle ;
- un header vide et un message à un, deux et trois fragments ;
- les deux CRC et le vecteur `123456789` ;
- un manifeste et un snapshot à une et plusieurs parts ;
- un delta cumulatif ;
- un ACK `VALIDATED` et un ACK `VALIDATED | APPLIED` ;
- un NACK avec bitmap dont le dernier octet contient des bits inutilisés à zéro ;
- les records et messages spécialisés exigés par les [spécifications actives](../README.md).

Chaque fixture binaire est accompagnée d'une description textuelle des champs et de sa longueur totale. Les mêmes fichiers sont consommés par le producteur et au moins un décodeur indépendant du moteur.
