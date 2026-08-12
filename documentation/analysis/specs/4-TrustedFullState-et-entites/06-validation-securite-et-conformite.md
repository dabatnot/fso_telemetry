# 06 — Validation, sécurité et conformité

Les contrôles courts observent le vrai chemin projection → manifeste → snapshot/delta → décodeur indépendant. Ils vérifient le masque `0x07DB`, le gel des artefacts antérieurs, les IDs, le graphe, les transitions et les limites exactes.

Un loopback déterministe couvre un join-in-progress, une création et un retrait, une relation de docking, une perte unique de delta puis convergence. Il reste sous cinq minutes après compilation. Les tests max/max+1 couvrent les limites FSTL pertinentes, sans fuzz, soak ni score.

La sécurité vérifie allowlist avant allocation, absence de commandes gameplay, validation avant allocation, rejet des valeurs fermées invalides et absence de fuite vers Cockpit. Une observation Release facultative compare brièvement le graphe affiché par un client de confiance avec la mission; elle consigne `attendu`, `observé`, `écart` et `impact`.
