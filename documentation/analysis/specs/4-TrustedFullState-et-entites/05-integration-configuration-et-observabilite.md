# 05 — Intégration, configuration et observabilité

La configuration Phase 4 exige un opt-in explicite `trustedFullState` et une allowlist d’endpoints. Par défaut, elle est désactivée. Une configuration invalide ou un endpoint non autorisé refuse seulement `TrustedFullState` et ne dégrade pas les profils Cockpit existants.

Les budgets de Phase 4 sont définis avant implémentation par les limites FSTL et les allocations réellement nécessaires. Toute collection est préallouée ou refuse avant publication; aucune valeur de budget, seuil de performance ou campagne longue n’est une condition automatique de livraison.

Les métriques utilisent des compteurs fermés pour inventaire, lifecycle, manifestes, keyframes, deltas, resync, refus d’autorisation et dépassements. Les logs n’émettent que catégorie, code fermé et compteur : ni nom d’objet, ni adresse, ni ID public, ni contenu mission ne sont journalisés.
