#pragma once

#include "telemetry/protocol/telemetry_replication.h"

#include <cstdint>
#include <vector>

namespace telemetry::detail {

struct Phase4EntityTypeBinding {
	std::uint64_t entity_id = 0U;
	protocol::ObjectType object_type = protocol::ObjectType::Unknown;
};

enum class Phase4ShipStateScopeStatus : std::uint8_t {
	Valid = 0,
	InvalidBinding,
	MalformedIdentity,
	UnknownEntity,
	NonShipDetailedState,
	Count,
};

// Validates the inherited detailed-record matrix. FLIGHT_STATE is deliberately
// absent: Phase 4 applies it to every positionable exported entity.
Phase4ShipStateScopeStatus validate_phase4_ship_state_scope(
	const std::vector<protocol::StateAtom>& records,
	const std::vector<Phase4EntityTypeBinding>& entities) noexcept;

} // namespace telemetry::detail
