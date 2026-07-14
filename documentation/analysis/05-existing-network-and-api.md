# Réseau et API existants

## 1. Conclusion de l'analyse

FS2Open sait déjà sérialiser une partie importante de l'état du vaisseau pour son multijoueur. Ce code confirme la faisabilité du projet et fournit des références pour l'autorité, les cadences, l'interpolation et le découpage des paquets.

Il ne convient cependant pas comme protocole public de télémétrie : il est lié à une partie multijoueur FS2Open, partiel, quantifié, versionné implicitement par le moteur et dépendant d'indices partagés entre participants.

## 2. Object updates

Les flags de mise à jour couvrent notamment :

- position et orientation ;
- physique complète ;
- coque ;
- boucliers ;
- sous-systèmes ;
- IA et cible ;
- afterburner ;
- banques, liens et gâchette ;
- état du support.

Référence : [`code/network/multi_obj.cpp`](../../code/network/multi_obj.cpp#L174-L185).

`OO_ANIMATION` est bien défini dans cette liste, mais n'apparaît nulle part ailleurs dans le fichier : ni le packer ni le dépaquetage ne le testent et aucun état d'animation n'est sérialisé. Il s'agit donc d'un flag réservé ou inachevé, pas d'une capacité active du réseau existant.

Le packer principal, dans [`multi_oo_pack_data()`](../../code/network/multi_obj.cpp#L1233-L1567), envoie selon le contexte :

- position compressée ;
- orientation compressée ;
- vitesse et vitesse angulaire ;
- vitesses désirées pour certains objets ;
- pourcentage de coque ;
- segments de bouclier dynamiques ;
- HP et rotations de sous-systèmes ;
- mode IA, sous-mode, cible et énergie d'arme ;
- état de réparation/support ;
- afterburner actif.

Le dépaquetage applique ces valeurs et protège certains domaines contre une écriture non autorisée depuis un client. Ce modèle d'autorité devra inspirer le module de télémétrie, sans réutiliser son format binaire.

## 3. Client update

Le paquet de mise à jour du vaisseau joueur transmet déjà une partie des informations nécessaires à un cockpit distant :

- pause ;
- coque ;
- boucliers ;
- santé des sous-systèmes ;
- menaces ;
- énergie d'arme ;
- munitions secondaires.

Références : [`code/network/multimsgs.cpp`](../../code/network/multimsgs.cpp#L7152-L7375).

L'état serveur associé conserve aussi des sélections de banques, liens, pose de l'œil, ETS, cible et entrées accumulées dans [`code/network/multi.h`](../../code/network/multi.h#L403-L449).

## 4. Cadences et priorités existantes

Le multijoueur adapte déjà la fréquence de ses flux selon le niveau d'update, la cible, la distance et l'angle de vue :

- `CLIENT_UPDATE` utilise des intervalles de 333, 166, 80 ou 30 ms, soit environ 3 à 33 Hz selon le niveau : [`code/network/multi.cpp`](../../code/network/multi.cpp#L94-L101) ;
- les object updates vont d'environ 0,4 Hz pour un objet lointain derrière le joueur au niveau Dialup à 50 Hz pour une cible, un joueur ou certains objets au niveau LAN : [`code/network/multi_obj.cpp`](../../code/network/multi_obj.cpp#L212-L282) ;
- le choix de l'intervalle d'un objet dépend explicitement de son statut de cible ou de joueur, puis de sa classe de distance et de sa présence dans le cône avant : [`multi_oo_reset_timestamp()`](../../code/network/multi_obj.cpp#L2261-L2303) ;
- les contrôles client sont plafonnés par `OO_CIRATE = 85 ms`, soit environ 11,8 Hz ; le commentaire « 15x a second » associé à cette constante n'est pas cohérent avec sa valeur : [`code/network/multi_obj.cpp`](../../code/network/multi_obj.cpp#L2909-L2912) et [`multi_oo_client_process()`](../../code/network/multi_obj.cpp#L3001-L3008) ;
- les mises à jour complètes de coque/boucliers et de sous-systèmes ont des périodes distinctes de 600 et 1000 ms : [`code/network/multi_obj.cpp`](../../code/network/multi_obj.cpp#L203-L205).

La télémétrie reprendra ce principe de canaux à cadences différentes, mais proposera une fréquence de vol configurable jusqu'à 60 Hz pour les instruments locaux/LAN.

## 5. Quantification actuelle

Le protocole multijoueur réduit la taille des données :

- coque object-update sur 8 bits ;
- segments de bouclier sur 8 bits ;
- santé des sous-systèmes sur 7 bits ;
- rotations de sous-systèmes sur 9 bits par tour ;
- plusieurs positions, orientations et vitesses compressées ou saturées ;
- certains client updates utilisent des pourcentages entiers.

Les fonctions de compression de pose et de vitesse se trouvent dans [`code/network/multiutil.cpp`](../../code/network/multiutil.cpp#L3382-L3648).

Cette précision est adaptée au gameplay multijoueur, mais ne devra pas être la source canonique d'une télémétrie complète. La première version du nouveau protocole transmettra des `float32` explicites ; une quantification pourra être ajoutée ensuite, champ par champ, après mesure.

## 6. Taille de paquet existante

Le réseau fixe :

- `MAX_TOP_LAYER_PACKET_SIZE = 1232` ;
- `MAX_PACKET_SIZE = 1221` après réservation interne PSNET.

Référence : [`code/network/psnet2.h`](../../code/network/psnet2.h#L34-L44).

Le code object-update sait répartir les objets entre plusieurs paquets lorsqu'il approche de la limite, dans [`code/network/multi_obj.cpp`](../../code/network/multi_obj.cpp#L2483-L2578). Le protocole de télémétrie adoptera la même contrainte générale, avec sa propre fragmentation applicative et une limite de 1200 octets par datagramme.

## 7. Lacunes pour la télémétrie

Les chemins multijoueur ne fournissent pas un miroir complet et régulier de tout l'état :

- pas de correction périodique exhaustive des munitions balistiques primaires ;
- carburant afterburner distant absent de plusieurs flux ;
- compteur de contre-mesures non maintenu comme état autoritaire complet ;
- index ETS non diffusés continuellement ;
- énergie d'arme distante seulement dans certains paquets ;
- translations de sous-systèmes préparées dans certains chemins, mais non sérialisées ;
- cargo envoyé comme événement de visibilité sans contenu autonome ;
- plusieurs tirs sont reproduits comme événements plutôt que confirmés par un snapshot complet ;
- radar, locks, navigation et jauges HUD ne forment pas une API cohérente ;
- les participants supposent posséder les mêmes tables et la même mission.

Une connexion tardive dispose de chemins de synchronisation spécifiques au multijoueur dans [`code/network/multi_ingame.cpp`](../../code/network/multi_ingame.cpp#L1469-L1549), mais ce mécanisme reste dépendant de la simulation FS2Open et ne peut pas servir directement à un client générique.

## 8. Pourquoi un socket séparé

`psnet_send()` :

- utilise le socket global du jeu ;
- ajoute un octet de type interne ;
- dépend d'une table fermée de catégories PSNET ;
- alimente les compteurs et buffers multijoueur.

Références : [`code/network/psnet2.cpp`](../../code/network/psnet2.cpp#L321-L342) et [`code/network/psnet2.cpp`](../../code/network/psnet2.cpp#L783-L839).

Réutiliser ce socket demanderait de modifier davantage le réseau upstream et pourrait mélanger les réponses `ACK`/`HELLO` de la télémétrie avec les paquets du jeu. Un socket autonome dans `code/telemetry` isolera totalement les deux usages.

## 9. API Lua existante

Une grande partie du vaisseau est déjà lisible via l'API de scripting :

- pose, coque et boucliers : [`code/scripting/api/objs/object.cpp`](../../code/scripting/api/objs/object.cpp#L150-L344) ;
- segments de bouclier : [`code/scripting/api/objs/shields.cpp`](../../code/scripting/api/objs/shields.cpp#L10-L134) ;
- physique : [`code/scripting/api/objs/physics_info.cpp`](../../code/scripting/api/objs/physics_info.cpp#L30-L466) ;
- énergie, afterburner, contre-mesures, banques et ETS : [`code/scripting/api/objs/ship.cpp`](../../code/scripting/api/objs/ship.cpp#L418-L1144) ;
- banques détaillées : [`code/scripting/api/objs/ship_bank.cpp`](../../code/scripting/api/objs/ship_bank.cpp#L84-L474) ;
- sous-systèmes : [`code/scripting/api/objs/subsystem.cpp`](../../code/scripting/api/objs/subsystem.cpp#L243-L509) ;
- docking : [`code/scripting/api/objs/ship.cpp`](../../code/scripting/api/objs/ship.cpp#L2724-L2936).

Lua permettrait un prototype rapide, mais ne couvre pas proprement tout le domaine requis : contenu cargo, contacts radar autoritairement filtrés, locks complets et plusieurs états internes. Le RPC Lua existant est également limité au réseau multijoueur interne et à la taille `MAX_PACKET_SIZE`.

Le module C++ sera donc retenu pour la version complète. L'API Lua restera utile comme référence de sémantique et comme oracle de test pour certaines valeurs.

## 10. Éléments à réutiliser conceptuellement

- règles d'autorité serveur/client ;
- IDs/signatures pour retrouver une entité ;
- interpolation de pose ;
- séparation entre données fréquentes et peu fréquentes ;
- limites de datagramme compatibles IPv6 ;
- découpage de listes entre plusieurs paquets ;
- snapshots de connexion tardive ;
- flags de présence pour ne transmettre que les blocs modifiés.

## 11. Éléments à ne pas réutiliser directement

- structures de paquets C++ du multijoueur ;
- macros `ADD_*`/`GET_*` comme API publique ;
- indices locaux partagés implicitement ;
- quantification sans documentation par champ ;
- socket global et types PSNET ;
- hypothèse que le client possède la mission et les tables ;
- autorité d'écriture du client de jeu.

## 12. Messages de mission et vue de communication

Le gauge de communication est `HudGaugeTalkingHead`. Il dessine un cadre, avance un `generic_anim` et affiche l'image courante dans [`code/hud/hudmessage.cpp`](../../code/hud/hudmessage.cpp#L1175-L1317).

La ressource déclarée dans une mission est séparée en deux parties :

- `+AVI Name`, malgré son nom historique, désigne une animation ;
- `+Wave Name` désigne une voix facultative.

Le parsing se trouve dans [`code/mission/missionmessage.cpp`](../../code/mission/missionmessage.cpp#L480-L560). La voix est chargée et jouée séparément dans [`message_play_wave()`](../../code/mission/missionmessage.cpp#L1315-L1356). La première version de la vue distante ne répliquera pas cette piste audio.

`message_play_anim()` retire l'extension, applique les règles de persona, peut ajouter un suffixe comme `a`, `b`, `-reg` ou `-death`, puis charge l'animation effectivement résolue : [`code/mission/missionmessage.cpp`](../../code/mission/missionmessage.cpp#L1409-L1562). Le gauge peut ensuite sélectionner une image initiale aléatoire avec le nouveau système de suffixes : [`code/hud/hudmessage.cpp`](../../code/hud/hudmessage.cpp#L1299-L1314).

Les animations génériques acceptent ANI, EFF et APNG dans [`code/graphics/generic.cpp`](../../code/graphics/generic.cpp#L157-L305). `generic_anim_stream()` appelle `cf_find_file_location_ext(..., CF_TYPE_ANY)` : la recherche n'est donc pas limitée à `data/maps` et conserve l'ordre des extensions d'animation : [`code/graphics/generic.cpp`](../../code/graphics/generic.cpp#L157-L172). Le résolveur CFile applique ensuite sa priorité complète entre types de chemins, racines actives de la pile de mods, fichiers libres et archives VP : [`code/cfile/cfilesystem.cpp`](../../code/cfile/cfilesystem.cpp#L1260-L1433) et [`code/cfile/cfilesystem.cpp`](../../code/cfile/cfilesystem.cpp#L1440-L1650).

Le paquet multijoueur de message existant transmet notamment l'identifiant du message, l'émetteur, la priorité et des paramètres de timing dans [`code/network/multimsgs.cpp`](../../code/network/multimsgs.cpp#L3532-L3597). Il ne transmet ni l'animation résolue ni son offset réel : les participants FS2Open supposent qu'ils possèdent les mêmes missions, tables et ressources.

Le protocole de télémétrie ne devra donc pas réutiliser ce paquet. Il transmettra un `head_asset_id` stable, l'offset autoritaire observé dans le gauge et un temps monotone. Le client affichera ensuite son asset local. Cette approche conservera la sémantique du moteur sans dépendre de ses indices, handles bitmap ou choix pseudo-aléatoires.

L'utilitaire [`code/cfileextractor/cfileextractor.cpp`](../../code/cfileextractor/cfileextractor.cpp#L405-L421) pourra servir à extraire une archive VP pendant les premiers essais. Le packager final devra cependant appeler le résolveur CFile, ou reproduire exactement toute sa priorité, notamment l'ordre des racines de mods, des types de chemins, des fichiers libres, des VP et des extensions. Il produira uniquement les assets effectivement choisis par le gauge, avec un manifeste et des hashes ; un simple scan de `data/maps` ne sera pas équivalent.

## 13. Rendu existant de la vue de cible

[`HudGaugeTargetBox::render()`](../../code/hud/hudtargetbox.cpp#L383-L483) récupère `Player_ai->target_objnum`, dessine le moniteur et choisit un chemin selon le type de l'objet ciblé.

Pour un vaisseau, [`renderTargetShip()`](../../code/hud/hudtargetbox.cpp#L618-L790) :

- construit la caméra depuis la position du joueur et celle de la cible ;
- utilise l'axe vertical de la caméra joueur ;
- applique `closeup_pos_targetbox` et `closeup_zoom_targetbox` ;
- configure couleurs d'équipe, wireframe, désaturation, LOD et glowmaps ;
- utilise le modèle HUD spécial s'il existe, sinon le modèle principal avec ses textures de remplacement ;
- appelle `model_render_immediate()` avec l'orientation réelle de la cible ;
- projette ensuite le sous-système sélectionné.

Le viewport par défaut ne mesure que 131 × 112 pixels dans [`code/hud/hudparse.cpp`](../../code/hud/hudparse.cpp#L3392-L3443). Cette taille appartient au gauge, pas au renderer de modèles. Un nouveau contexte hors écran pourra donc reprendre la même caméra et rendre le modèle directement en 1024 sans agrandir les pixels du HUD.

Les ressources et paramètres de classe sont définis par :

- `$POF target file` et `$POF target LOD` : [`code/ship/ship.cpp`](../../code/ship/ship.cpp#L3349-L3373) ;
- `$Closeup_pos_targetbox` et `$Closeup_zoom_targetbox` : [`code/ship/ship.cpp`](../../code/ship/ship.cpp#L4800-L4819) ;
- modèle principal, modèle HUD et LOD : [`code/ship/ship.h`](../../code/ship/ship.h#L1175-L1184).

Le POF HUD pouvant être simplifié pour la petite vue originale, le profil haute résolution `MfdHigh` utilisera le modèle principal et un LOD adapté. `HudExact` conservera le choix historique.

Le gauge sait aussi rendre :

- débris : [`code/hud/hudtargetbox.cpp`](../../code/hud/hudtargetbox.cpp#L808-L916) ;
- armes et missiles : [`code/hud/hudtargetbox.cpp`](../../code/hud/hudtargetbox.cpp#L921-L1140) ;
- astéroïdes : [`code/hud/hudtargetbox.cpp`](../../code/hud/hudtargetbox.cpp#L1184-L1285) ;
- jump nodes : [`code/hud/hudtargetbox.cpp`](../../code/hud/hudtargetbox.cpp#L1317-L1366).

FS2Open possède déjà des render targets dans [`code/bmpman/bmpman.h`](../../code/bmpman/bmpman.h#L628-L644). En revanche, la lecture de région OpenGL actuelle appelle directement `glReadPixels()` dans [`code/graphics/opengl/gropengl.cpp`](../../code/graphics/opengl/gropengl.cpp#L655-L674), et les fonctions correspondantes sont des stubs dans le backend Vulkan actuel : [`code/graphics/vulkan/vulkan_stubs.cpp`](../../code/graphics/vulkan/vulkan_stubs.cpp#L33-L36).

La réutilisation correcte consistera donc à extraire la partie modèle du target box et à ajouter une abstraction de readback asynchrone générique. Capturer puis agrandir la zone déjà dessinée, dupliquer toute la logique dans `code/telemetry` ou appeler directement `glReadPixels()` depuis le module créeraient respectivement une mauvaise qualité, une dette upstream ou un blocage potentiel de la frame.
