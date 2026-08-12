# 03 — Flux, cycle de vie et concurrence

Une session autorisée suit : `HELLO` → vérification opt-in/allowlist → `WELCOME(0x07DB)` → manifestes appliqués → `FULL_SNAPSHOT` → `ACK APPLIED` → deltas cumulatifs. Avant l’ACK du snapshot, aucun delta Phase 4 n’est émis.

Un join-in-progress utilise exactement la même séquence. La keyframe contient l’ensemble exhaustif de l’image capturée; les entités créées ou retirées pendant sa préparation restent dans le dirty set suivant et sont reflétées par le delta cumulatif ou la keyframe suivante.

`CREATE` publie d’abord le lifecycle et les catalogues requis. `DELETE` retire l’entité et cascade ses relations, records spécialisés et références d’enfant. Une transition `SPAWNING`, `ACTIVE`, `DEPARTING`, `DYING`, `DESTROYED` ou `REMOVED` reste une valeur FSTL fermée, jamais une heuristique client.

La lecture moteur et la projection sont main-thread. Le socket reste non bloquant; aucun worker n’accède aux tables moteur. Une priorité peut choisir l’ordre des lots de préparation, mais jamais produire une keyframe partielle.
