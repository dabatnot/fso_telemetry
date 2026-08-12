#pragma once

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

// Combines the entity graph and its ship docking topology into one immutable
// candidate. The caller observes output only once both projections succeed.
Phase4TrustedImageStatus build_phase4_trusted_image(
	const std::vector<Phase4EntityProjectionInput>& entities,
	const std::vector<Phase4DockingRelation>& docking_relations,
	std::uint64_t sample_time_us,
	protocol::StateImage& output);

} // namespace telemetry::detail
