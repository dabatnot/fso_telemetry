# 01 — Cadre normatif et périmètre

`TrustedFullState` est additif. FSTL 1.0, les layouts v1 à v6 et les profils Phase 1 à 3 restent byte-identiques. Le profil n’est sélectionnable que par une configuration locale explicite et un endpoint autorisé; un refus n’expose aucun snapshot partiel ni détail sur les entités présentes.

| ID | Exigence normative |
|---|---|
| `P4-REQ-001` | Les contrats Phase 0 à 3 et `CockpitSensors` restent inchangés. |
| `P4-REQ-002` | `TrustedFullState` négocie exactement FSTL 1.1, `SOLO + TRUSTED_FULL_STATE` et le masque `0x07DB`; ces faits sont immuables après `WELCOME`. Toute autorité multijoueur est refusée sans downgrade vers Cockpit. |
| `P4-REQ-003` | Le profil est refusé avant allocation de session si l’opt-in local ou l’allowlist d’endpoint manque. |
| `P4-REQ-004` | L’ensemble exporté contient exactement les objets exportables connus du producteur des types FSTL fermés `SHIP`, `WEAPON`, `ASTEROID`, `DEBRIS`, `JUMP_NODE`, `WAYPOINT`, `FIREBALL` et `OTHER`. |
| `P4-REQ-005` | Chaque entité exportée possède un `entity_id` non nul, monotone dans la session et jamais réutilisé après retrait. |
| `P4-REQ-006` | `ENTITY_LIFECYCLE` publie le type, la phase et seulement les présences autorisées par son layout; `UNKNOWN` et les bits réservés sont rejetés. |
| `P4-REQ-007` | Seuls `SHIP` et `WEAPON` ont une classe publique : le premier référence `CLASS_MANIFEST`, le second `WEAPON_MANIFEST`; les autres types omettent `class_id`. Une classe requise absente déclenche manifeste puis keyframe, jamais une référence opaque. |
| `P4-REQ-008` | Les relations `parent_entity_id`, cible et docking ne désignent que des entités présentes dans la même image; les cycles de parenté sont refusés. |
| `P4-REQ-009` | Le docking global est réciproque et sa suppression retire les liens dépendants sans retirer abusivement les entités encore actives. Plus de 64 relations refuse le profil avant `WELCOME` ou termine proprement la session si le dépassement survient ensuite. |
| `P4-REQ-010` | Les états vaisseau hérités sont publiés pour chaque `SHIP` matérialisé et ne sont jamais appliqués à un type différent. |
| `P4-REQ-011` | Chaque `WEAPON` exporté, projectile compris, porte `ENTITY_LIFECYCLE` et `FLIGHT_STATE`; créateur ou cible ne sont référencés que s’ils sont présents et stables. |
| `P4-REQ-012` | Chaque entité exportée positionnable porte `FLIGHT_STATE`; un repère statique utilise vitesse nulle et quaternion identité. Les types non-vaisseaux n’ont aucun état détaillé hors lifecycle, pose et champs explicitement présents, et n’exposent ni pointeur ni index moteur. |
| `P4-REQ-013` | Un join-in-progress installe manifestes puis une keyframe exhaustive et atomique avant tout delta. |
| `P4-REQ-014` | Les keyframes périodiques, resync et changement de mission reconstruisent exactement le même graphe exporté que la projection producteur. |
| `P4-REQ-015` | Les deltas restent cumulatifs contre la baseline `ACK APPLIED` et contiennent le remplacement net complet de chaque entité ou relation modifiée. |
| `P4-REQ-016` | La perte contrôlée d’un delta converge par delta cumulatif ou keyframe sans relation pendante ni réapparition d’ID retiré. |
| `P4-REQ-017` | Les limites de records, transaction, fragmentation, IDs et collections sont vérifiées avant allocation; un dépassement échoue fermé sans troncature. |
| `P4-REQ-018` | La sélection de priorités de capture reste déterministe et ne peut omettre une entité annoncée comme présente dans une keyframe complète. |
| `P4-REQ-019` | La capture moteur reste sur le thread principal; réseau et sérialisation restent non bloquants et ne modifient jamais la simulation. |
| `P4-REQ-020` | Logs et métriques n’exposent ni noms, adresses, IDs publics ni contenu caché; ils identifient seulement des catégories d’échec fermées. |
