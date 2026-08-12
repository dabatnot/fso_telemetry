#pragma once

#include "telemetry/phase4_entity_projection.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <cstddef>
#include <vector>

namespace telemetry::detail {

enum class Phase4EntityImageStatus : std::uint8_t {
	Created = 0,
	EmptyInventory,
	TooManyEntities,
	DuplicateEntityId,
	UnknownParent,
	ParentCycle,
	InvalidEntity,
	AllocationFailure,
	Count,
};

// Builds an unpublished, complete entity image. On failure, output is left
// unchanged; callers can therefore retain the last ACKed baseline.
Phase4EntityImageStatus build_phase4_entity_image(
	const std::vector<Phase4EntityProjectionInput>& entities,
	protocol::StateImage& output);

} // namespace telemetry::detail
