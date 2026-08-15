# Feuille de route d'implémentation

## 1. Définition de « terminé »

Le premier système complet est considéré opérationnel lorsqu'un client distant peut :

1. découvrir ou joindre un producteur configuré ;
2. négocier une version compatible ;
3. recevoir tous les manifestes nécessaires ;
4. construire un snapshot complet ;
5. maintenir cet état avec des deltas cumulatifs calculés contre une baseline appliquée et acquittée par `ACK APPLIED`, malgré perte, duplication et désordre UDP ;
6. demander et obtenir une resynchronisation ;
7. gérer mission, mort, respawn et changement de session ;
8. exposer l'état à des radars, jauges et adaptateurs ESP32 ;
9. afficher la vue de communication à partir d'un bundle local vérifié, sans streamer ses images ;
10. afficher la cible dans un flux 3D rendu nativement jusqu'en 1024, sans agrandir le viewport HUD ;
11. ne jamais influencer la simulation ;
12. ne pas introduire de ralentissement sensible lorsque la télémétrie est désactivée.

Cette roadmap est indicative. Elle ne fixe ni ordre obligatoire, ni gate, ni
état de complétude. Le propriétaire du projet peut réordonner, sauter ou
explorer une phase selon les besoins du simpit.

Les tests unitaires pertinents et une vérification manuelle en jeu suffisent.
Leurs résultats sont rapportés directement et ne sont enregistrés dans aucun
système de preuve. Un test rouge est un risque à signaler, pas une interdiction
automatique de poursuivre.

## 2. Phase 0 — Contrat de protocole

Livrables :

- schéma wire v1 exhaustif couvrant chaque message, record, enum, ID, champ optionnel, unité et limite décrits dans l'inventaire ;
- conventions wire figées : endianness, largeurs, IEEE-754, UTF-8, alignement, valeurs absentes, compatibilité major/minor et rejet des valeurs non finies ;
- conventions d'horloge figées : domaine monotone du producteur, unités, synchronisation, pause, compression temporelle et taux de lecture signé ;
- conventions de fiabilité figées : portée des séquences, sémantique `ACK`/`NACK`, fenêtre et backoff, délais, expiration et déclenchement d'un resync ;
- sémantique `DELTA` cumulative : chaque delta contient tous les changements depuis `baseline_snapshot_id` jusqu'à son `sample_time`, afin qu'un delta plus récent puisse remplacer un delta perdu de la même baseline ;
- constantes, types et limites du protocole ;
- `PacketWriter` et `PacketReader` bornés ;
- en-tête de datagramme incluant `message_id`, `fragment_index`, `fragment_count`, `message_size`, `fragment_offset` et les CRC nécessaires ;
- fragmentation/réassemblage validant `message_size`, les offsets et les limites avant toute allocation ;
- messages `HELLO`, `WELCOME`, `ACK`, `NACK`, `HEARTBEAT` et `RESYNC_REQUEST` ;
- vecteurs binaires de référence décodables indépendamment du moteur ;
- tests de valeurs invalides, tronquées, incohérentes entre fragments et surdimensionnées.
- amendement additif FSTL 1.1 pour le premier flux Phase 1 : `StateDomainCoverage.PLAYER_KINEMATICS` au bit 10, négociation `1..1`, snapshot minimal sans manifeste, rejets croisés et non-régression byte-identical de tous les artefacts FSTL 1.0.

Cette phase peut être réalisée sans toucher à la boucle FS2Open. Avant de publier un producteur ou un client interopérable, le contrat v1 et ses golden vectors doivent concorder dans une revue courte. Le travail d'intégration peut progresser derrière des interfaces proposées tant qu'il ne prétend pas figer ou publier un wire incompatible.

La préparation documentaire et contractuelle `WP01` de la Phase 1 peut produire l'amendement FSTL 1.1. La cohérence du schéma et des vecteurs est un prérequis produit à l'interopérabilité, pas une chaîne de certification : elle ne demande aucune campagne longue et ne justifie pas de bloquer les travaux indépendants qui ne publient pas encore ce format.

## 3. Phase 1 — Squelette et premier flux

Modifications upstream minimales :

- groupe `Telemetry` dans `source_groups.cmake` ;
- include et appel `telemetry::initialize()` dans `freespace.cpp`.

Livrables producteur, dans cet ordre :

- configuration JSON ;
- socket UDP dédié, non bloquant et dual-stack ;
- abonnement aux événements moteur ;
- session et heartbeat ;
- premier snapshot : temps, identité joueur, position, quaternion, vitesse et rotation ;
- profil filaire FSTL 1.1 strict : `state_domain_coverage = PLAYER_KINEMATICS`, `required_manifest_id = 0`, capabilities et couvertures événementielles nulles ; `SESSION_STATE` et `MISSION_STATE`, puis, si le joueur existe, son `ENTITY_LIFECYCLE` et son `FLIGHT_STATE` à `presence = 0` ;
- négociation `min_minor = max_minor = 1` et rejet `UnsupportedVersion` d'un client limité à FSTL 1.0, sans rétrogradation mensongère vers `CORE_SHIP` ;
- deltas cumulatifs contre la dernière baseline appliquée et acquittée par `ACK APPLIED`, et renouvellement périodique de cette baseline ;
- métriques et tests d'intégration du producteur.

Une fois ce premier flux producteur stabilisé, un décodeur puis un client console minimal l'affichent et le testent. Ce client minimal ne préjuge pas de l'architecture du client distant d'une phase ultérieure.

Critère de sortie : une mission solo représentative peut être observée à distance, puis arrêtée et relancée proprement ; le client valide la complétude `PLAYER_KINEMATICS`, une nouvelle keyframe accompagne tout changement de mission ou de joueur observé, et le corpus FSTL 1.0 reste byte-identical. Une observation prolongée ou un soak peut rechercher des défauts rares après livraison, uniquement sur demande humaine explicite, sans bloquer ce critère.

## 4. Phase 2 — Vaisseau complet

Ajouter :

- coque et boucliers dynamiques ;
- énergie, ETS et propulsion ;
- afterburner ;
- `CONTROL_STATE`, incluant les commandes observées, les modes de vol actifs et les indicateurs de pilotage/autopilote définis par le schéma v1 ;
- armement, banques, munitions et contre-mesures ;
- sous-systèmes et tourelles ;
- `SUPPORT_STATE`, incluant le support assigné, l'état brut de réparation/réarmement et les transitions terminales ; les progressions restent dérivées côté client à partir des délais et quantités publiés ;
- catalogues de classes de vaisseaux et d'armes ;
- promotion de la couverture FSTL 1.1 vers `PLAYER_KINEMATICS | CORE_SHIP` seulement après installation du manifeste de classes et disponibilité de tous les records imposés par `CORE_SHIP` ;
- snapshots complets et deltas cumulatifs par bloc contre une baseline explicite ;
- apparition, mort et respawn du vaisseau joueur.

Critère de sortie : chaque valeur d'un tableau de bord peut être comparée à sa source moteur et retrouve sa valeur correcte après une perturbation réseau prise en charge.

## 5. Phase 3 — Ciblage et capteurs

Ajouter :

- cible et sous-système ciblé ;
- lead ;
- locks missile ;
- radar et contacts ;
- AWACS, furtivité et distorsion ;
- missiles entrants et niveaux de menace ;
- cargo et scan ;
- navigation et destination d'autopilote, sans dupliquer les modes de vol déjà portés par `CONTROL_STATE`.

Critère de sortie : le radar distant montre les mêmes contacts autorisés que le HUD du producteur, sans révéler un contact caché en mode `Cockpit`.

## 6. Phase 4 — Vue de communication et client distant utilisable

Les livrables producteur de la vue de communication sont terminés avant son intégration au client :

- hook moteur minimal au point d'état autoritaire du `Talking Head`, couvrant début, remplacement, arrêt, `playback_rate` signé et offset initial choisi par le gauge ;
- production de `COMM_VIEW_EVENT` et de l'état correctif `COMM_VIEW_STATE`, y compris dans les snapshots complets ;
- packager qui utilise le résolveur CFile, ou reproduit exactement sa priorité entre pile de mods, types de chemins, fichiers libres, archives VP et extensions ANI/EFF/APNG ;
- bundle local versionné contenant uniquement les assets effectivement résolus ;
- `manifest.json`, hashes par asset, hash global, `bundle-info.json` et golden manifest ;
- publication fiable et acquittée de `COMM_ASSET_MANIFEST`, sans transfert des images pendant la session ;
- tests du hook et du packager avec assets masqués par plusieurs mods et VP.

Le client maintient ensuite une base d'état indépendante du rendu :

```text
UDP receiver
  -> validation
  -> réassemblage
  -> session et ACK
  -> ReplicaStore
      -> interpolation
      -> radar
      -> jauges
      -> vue de communication
      -> export ESP32
      -> enregistrement/replay
```

Fonctions :

- horloge locale alignée au producteur ;
- buffer d'interpolation ;
- état `Synchronizing`, `Live`, `Stale` ;
- API de lecture thread-safe ;
- métriques de perte, jitter, latence et âge des données ;
- résolution d'un `head_asset_id` dans un bundle visuel versionné ;
- lecture locale de la vue de communication avec seek et correction d'offset ;
- prise en compte d'un `playback_rate` signé, nul pendant la pause et dérivé de la compression temporelle du moteur ;
- placeholder non bloquant lorsqu'un asset manque ou est corrompu dans un bundle déclaré compatible ; capability de communication refusée si le hash de bundle diffère ;
- adaptateur ESP32 utilisant le même transport UDP avec des messages de jauges compacts.

Le client doit conserver la sémantique brute et calculer les valeurs d'affichage sans altérer l'état reçu.

## 7. Phase 5 — Événements exacts et optimisation

Comparer les événements détectés par diff aux besoins réels. Ajouter un hook moteur explicite uniquement lorsqu'un événement peut être manqué entre deux captures :

- tir très bref ;
- impact avec position exacte ;
- événement de script non représenté dans l'état final ;
- transition apparaissant et disparaissant dans une seule frame.

Le hook `Talking Head`, requis fonctionnellement et déjà livré en phase 4, sert de modèle pour ces hooks additionnels.

Chaque hook doit :

- appeler une fonction du module avec des valeurs copiées ;
- ne contenir aucune logique de protocole ;
- rester indépendant de l'activation du réseau ;
- être documenté dans une liste centrale des points d'intégration.

Optimisations possibles après mesure :

- quantification contrôlée de certains vecteurs ;
- dictionnaires de chaînes ;
- compression des manifestes, jamais des petits deltas ;
- thread réseau et file SPSC ;
- adaptation dynamique de fréquence.

## 8. Phase 6 — Vue de cible 3D haute résolution

Livrables producteur :

- extraction d'une fonction de rendu modèle réutilisable depuis le target box ;
- render target persistante jusqu'à 1024 × 1024 ;
- profils `MfdHigh` et `HudExact` ;
- abstraction de readback GPU asynchrone avec une implémentation OpenGL concrète ;
- anneau de deux ou trois PBO persistants, chacun protégé par une fence `GLsync` ; seul un slot dont la fence est signalée peut être mappé ou copié, et aucune attente GPU n'est autorisée sur la frame du jeu ;
- file CPU strictement bornée ;
- interface `TargetVideoEncoder` basse latence et backend concret `FfmpegTargetVideoEncoder` ;
- découverte à l'exécution des encodeurs H.264 compilés dans FFmpeg, puis sélection dans une allowlist autorisée par les options de build et de licence du binaire ;
- backend `NullTargetVideoEncoder`, toujours disponible, qui refuse proprement la capability vidéo sans affecter la télémétrie d'état ;
- abonnement, configuration, arrêt et demande d'IDR ;
- schémas alignés : `codec_profile` et `codec_level` dans `TARGET_VIDEO_CONFIG`, puis `max_bitrate_kbps`, `preferred_bitrate_kbps`, `supported_h264_profiles`, `supported_h264_levels` et `overlay_capabilities` dans `TARGET_VIDEO_SUBSCRIBE` ;
- interframes fragmentées non fiables et jamais retransmises ;
- IDR « deadline-reliable » : fragments conservés au plus 500 ms dans un cache borné, `NACK` sélectif des fragments manquants et retransmission uniquement tant que la deadline n'est pas expirée ; après expiration, le client demande une nouvelle IDR ;
- intégration CMake des backends et options, inventaire des licences FFmpeg/encodeurs, règles de packaging des bibliothèques et test de chaque variante de build supportée ;
- arrêt immédiat du second rendu lorsqu'aucun client n'est abonné.

Livrables client :

- réassemblage vidéo borné avec timeout court ;
- décodeur H.264 ;
- validation de `stream_id`, génération et `target_entity_id` ;
- texture de dernière frame valide ;
- état `Starting`, `Live`, `Stale`, `Unsupported` et `Stopped` ;
- composition avec les textes, brackets et jauges produits localement ;
- demande d'IDR limitée en fréquence ;
- émission de `NACK` sélectifs pour une IDR encore présente dans la fenêtre fiable de 500 ms ;
- négociation de résolution, cadence et bitrate, avec `codec_profile`, `codec_level` et `overlay_mode` renvoyés par `TARGET_VIDEO_CONFIG`.

Ordre de couverture :

1. vaisseaux ;
2. armes et missiles ;
3. astéroïdes ;
4. débris ;
5. jump nodes.

Critère nominal de sortie : un MFD distant affiche un vaisseau ciblé en 1024 × 1024 à 15 FPS, avec une latence bornée, sans présenter une frame de l'ancienne cible et sans blocage mesurable de la frame du jeu.

Sous perte UDP, la vidéo ne doit jamais retarder la télémétrie numérique :

| Perte UDP injectée | Critère de sortie vidéo et télémétrie |
|---:|---|
| 1 % | session et télémétrie restent `Live`, aucune file vidéo ne dépasse 500 ms et une demande d'IDR retrouve une frame décodable en 500 ms au plus |
| 5 % | session et télémétrie restent `Live`, le trafic d'état n'est jamais privé de bande passante et la vidéo récupère sur une IDR complète en 1 s au plus |
| 20 % | session et télémétrie restent `Live`, mémoire et files restent bornées, et le flux retrouve une frame décodable en 2 s au plus ; aucune qualité, cadence ou continuité nominale n'est exigée à ce niveau de perte |

## 9. Validation définie avec chaque phase

Les specs racines ne définissent aucun harness, aucune matrice de tests et aucun seuil de campagne. Lors de la spécification d'une phase, ses livrables et critères produit sont établis d'abord. Un plan de validation séparé peut ensuite définir les quelques tests et outils nécessaires à ces seuls livrables.

Ces idées de tests restent facultatives et remplaçables. Elles ne peuvent pas étendre le périmètre produit, imposer une architecture destinée uniquement aux tests ni bloquer un autre travail. Les campagnes longues restent soumises à une demande humaine explicite.

## 10. Observabilité du module

Prévoir des compteurs consultables dans les logs :

- état enabled/disabled et mode ;
- clients actifs ;
- séquences envoyées/reçues ;
- baseline courante par client, âge et nombre de deltas cumulatifs produits ;
- datagrammes, octets et fragments ;
- pertes estimées ;
- ACK, retransmissions et resync ;
- messages abandonnés par priorité ;
- erreurs de validation ;
- durée de collecte/encodage ;
- communications démarrées, interrompues et remplacées ;
- assets de communication inconnus et incompatibilités de bundle ;
- abonnements et configurations vidéo actifs ;
- frames rendues, lues, encodées, envoyées et abandonnées ;
- IDR produites et demandées ;
- fragments d'IDR mis en cache, retransmis par `NACK` et expirés après leur deadline ;
- résolution, FPS, bitrate et latence vidéo courants ;
- backend/encodeur FFmpeg sélectionné ou fallback `Null` et raison du refus de capability ;
- erreurs de render target, readback, encodeur et décodeur ;
- âge de la dernière keyframe acquittée.

Les logs ne doivent pas imprimer les données complètes à chaque frame.

## 11. Risques principaux

| Risque | Réponse prévue |
|---|---|
| Évolution upstream des structures | collecteurs centralisés et schéma public indépendant |
| Perte d'un delta UDP | deltas cumulatifs depuis une baseline acquittée, séquences, keyframes et resync si la baseline est inconnue |
| Fragmentation IP | datagrammes limités à 1200 octets |
| Message fragmenté malveillant ou incohérent | `message_size`, offsets, nombre de fragments, CRC et budgets validés avant allocation |
| Snapshot trop volumineux | fragmentation applicative bornée et priorités |
| Blocage de la frame | socket non bloquant et files bornées |
| Data race | lecture moteur exclusivement sur le thread principal |
| Fuite d'informations radar | filtrage cockpit avant sérialisation et tests de non-divulgation |
| Réutilisation d'un ID | combinaison session + ID monotone |
| Client incompatible | négociation de versions et capabilities |
| Bundle visuel absent ou différent | capability de communication refusée si le hash négocié diffère ; placeholder réservé aux assets manquants ou corrompus dans un bundle compatible ; réplication générale maintenue |
| Asset de mod non résolu comme dans le jeu | packager fondé sur le résolveur CFile complet, même pile de mods, même priorité loose/VP et nom final observé dans le gauge |
| Readback GPU bloquant | PBO en anneau, fences testées sans attente et abandon de frame si aucun slot n'est prêt |
| Encodeur H.264 indisponible | backend `NullTargetVideoEncoder`, capability refusée sans affecter la télémétrie d'état |
| Encodeur FFmpeg présent mais interdit par le build ou sa licence | allowlist de build, inventaire de licences, tests CMake et packaging par variante |
| Débit vidéo trop élevé | H.264, limites négociées et adaptation résolution/FPS/bitrate |
| Vidéo affamant la télémétrie | priorité minimale, token bucket et abandon des frames en premier |
| Perte d'une interframe | abandon sans retransmission et attente de la prochaine frame décodable |
| Perte d'une IDR | cache borné 500 ms, `NACK` sélectif jusqu'à la deadline, puis nouvelle demande d'IDR limitée |
| Ancienne cible affichée | `target_entity_id` et génération vérifiés avant présentation |
| POF HUD insuffisant en 1024 | profil `MfdHigh` utilisant le modèle principal |
| Dette de fork | seams isolés : socle, Talking Head, helper target box et readback générique |

## 12. Ordre de développement recommandé

1. schéma wire v1 exhaustif, golden vectors et tests sans moteur ;
2. squelette producteur `code/telemetry`, transport, session et observabilité ;
3. heartbeat, premier snapshot producteur, baseline et deltas cumulatifs ;
4. décodeur puis client console minimal pour valider ce premier flux ;
5. collecteurs producteur du vaisseau et des systèmes, dont `CONTROL_STATE` et `SUPPORT_STATE` ;
6. collecteurs producteur radar/ciblage, puis vues de communication ;
7. hook producteur `Talking Head`, packager CFile, bundle et manifeste ;
8. `ReplicaStore` et client distant, puis vue de communication locale ;
9. vue cible côté producteur : rendu hors écran et readback OpenGL PBO/fences ;
10. backend producteur `FfmpegTargetVideoEncoder`, sélection d'encodeur, politique IDR fiable jusqu'à deadline et packaging ;
11. décodeur vidéo client, interfaces graphiques et overlays MFD ;
12. flux compact pour ESP32 ;
13. optimisation uniquement après profilage.

À chaque étape, le producteur et ses tests de contrat précèdent le consommateur applicatif correspondant ; les outils de décodage minimaux ne servent qu'à valider le wire format.
