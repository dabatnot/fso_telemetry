#pragma once

#include "telemetry/phase4_catalog_bindings.h"
#include "telemetry/phase4_docking_projection.h"
#include "telemetry/phase4_entity_image.h"

#include <vector>

namespace telemetry::detail {

enum class Phase4TrustedImageStatus : std::uint8_t {
	Created = 0,
	InvalidEntities,
	InvalidDocking,
	InvalidCompositeRecord,
	AllocationFailure,
	Count,
};

// Performs the production-side handoff from the ABI-free engine inventory to
// the public entity image.  Public class IDs are resolved exclusively through
// an already installed manifest; a missing definition therefore fails before
// any StateImage can be observed by the caller.
Phase4TrustedImageStatus build_phase4_trusted_image_from_inventory(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	const std::vector<Phase4DockingRelation>& docking_relations,
	std::uint64_t sample_time_us,
	protocol::StateImage& output);

// Allocation-free first runtime slice. It intentionally emits only the two
// universal records; docking and inherited ship records are added by later
// increments before Phase 4 is declared complete.
Phase4TrustedImageStatus build_phase4_trusted_image_from_inventory_preallocated(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	std::uint64_t sample_time_us,
	std::vector<Phase4EntityProjectionInput>& projections,
	Phase4StateImagePool& pool,
	protocol::StateImage& output) noexcept;

// Combines the entity graph and its ship docking topology into one immutable
// candidate. The caller observes output only once both projections succeed.
Phase4TrustedImageStatus build_phase4_trusted_image(
	const std::vector<Phase4EntityProjectionInput>& entities,
	const std::vector<Phase4DockingRelation>& docking_relations,
	std::uint64_t sample_time_us,
	protocol::StateImage& output);

} // namespace telemetry::detail
