#pragma once

#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase4_engine_inventory.h"

#include <array>
#include <cstdint>

namespace telemetry::detail {

// Deterministic, pre-ID dependency list for the manifest. Source keys remain
// collector-private and are replaced by manifest IDs before any StateAtom is
// created.
struct Phase4CatalogDependencies {
	std::array<std::uint32_t, Phase2ManifestLimits::MaxClasses> ship_class_source_keys{};
	std::uint32_t ship_class_count = 0U;
	std::array<std::uint32_t, Phase2ManifestLimits::MaxWeapons> weapon_source_keys{};
	std::uint32_t weapon_count = 0U;
};

enum class Phase4CatalogDependencyStatus : std::uint8_t {
	Collected = 0,
	InvalidInventory,
	SourceLimitExceeded,
	Count,
};

Phase4CatalogDependencyStatus discover_phase4_catalog_dependencies(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase4CatalogDependencies& output) noexcept;

} // namespace telemetry::detail
