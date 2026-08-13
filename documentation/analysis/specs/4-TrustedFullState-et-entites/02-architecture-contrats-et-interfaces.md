# 02 — Architecture, contrats et interfaces

Le collecteur Phase 4 lit les objets moteur sur le thread principal et produit une `TrustedStateImage` possédée. La projection attribue les IDs publics avant de construire catalogues, lifecycle, relations et états détaillés. Aucun DTO ne conserve un pointeur, handle ou index moteur.

```text
objets moteur -> projection TrustedFullState -> catalogues -> StateImage
           -> snapshot/delta FSTL -> décodeur indépendant
```

Le registre d’identités est partagé par tous les records d’une session. Il associe une identité moteur stable au même `entity_id` jusqu’au retrait, puis ne réutilise pas cet ID. Les références vers une entité non retenue sont omises si elles sont explicitement optionnelles, sinon l’image est rejetée avant publication.

Les interfaces Phase 4 séparent inventaire d’objets, projection de lifecycle, construction de relations, catalogues, image atomique et ordonnancement de priorités. Elles n’exposent pas l’ABI de `object`, `ship` ou `weapon`.

Une entrée `CLASS_MANIFEST` est une **configuration statique effective** de
`SHIP`, et non seulement un `ship_info_index`. Sa clé de déduplication couvre
le modèle, ses sous-systèmes et ses banques statiques, y compris les armes qui
y sont référencées. Deux vaisseaux de même modèle mais avec une configuration
différente reçoivent donc deux définitions publiques distinctes. `WEAPON_MANIFEST`
reste dédupliqué par définition d’arme. Les clés de déduplication sont internes
au collecteur ; seuls les IDs de manifeste FSTL atteignent l’image.

La collecte construit d’abord l’inventaire exact, puis les configurations de
classes et d’armes requises, puis le manifeste. Si ce manifeste diffère de
celui installé, il est appliqué avant toute keyframe dépendante. Un dépassement
des limites FSTL de classes, armes ou transactions échoue fermé : il n’y a ni
fusion approximative de configurations, ni troncature.
