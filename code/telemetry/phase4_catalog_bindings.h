#pragma once

#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase4_engine_inventory.h"
#include "telemetry/phase4_entity_projection.h"

#include <cstdint>
#include <vector>

namespace telemetry::detail {

enum class Phase4CatalogBindingStatus : std::uint8_t {
	Created = 0,
	MissingManifest,
	InvalidInventory,
	DuplicateDefinition,
	MissingClass,
	AllocationFailure,
	Count,
};

// Resolves the collector-local source class keys against the public IDs of an
// already built manifest.  It performs no fallback to an engine index: a
// TrustedFullState image can only reference a class that the client has been
// given in its manifest.
Phase4CatalogBindingStatus bind_phase4_catalogs(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	std::uint64_t sample_time_us,
	std::vector<Phase4EntityProjectionInput>& output) noexcept;

} // namespace telemetry::detail
