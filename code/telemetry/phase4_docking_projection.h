#pragma once

#include "telemetry/protocol/telemetry_replication.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace telemetry::detail {

struct Phase4DockingRelation {
	std::uint64_t local_entity_id = 0U;
	std::uint64_t remote_entity_id = 0U;
	std::uint16_t local_dockpoint = 0U;
	std::uint16_t remote_dockpoint = 0U;
	std::string_view local_dock_bay_name;
	std::string_view remote_dock_bay_name;
};

enum class Phase4DockingProjectionStatus : std::uint8_t {
	Created = 0,
	InvalidShipInventory,
	UnknownEndpoint,
	SelfRelation,
	InvalidDockpoint,
	MissingReciprocal,
	DuplicateRelation,
	TooManyRelations,
	EncodingFailure,
	AllocationFailure,
	Count,
};

// Relations are directed because FSTL carries a local and remote dockpoint in
// each subject record. Every input relation must have its exact reverse.
// Output is left unchanged on every failure.
Phase4DockingProjectionStatus project_phase4_docking_states(
	const std::vector<std::uint64_t>& ship_entity_ids,
	const std::vector<Phase4DockingRelation>& relations,
	std::uint64_t sample_time_us,
	std::vector<protocol::StateAtom>& output);

} // namespace telemetry::detail
