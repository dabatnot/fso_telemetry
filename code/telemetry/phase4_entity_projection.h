#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <array>
#include <cstdint>

namespace telemetry::detail {

struct Phase4EntityProjectionInput {
	std::uint64_t entity_id = 0U;
	protocol::ObjectType object_type = protocol::ObjectType::Unknown;
	protocol::LifecyclePhase lifecycle_phase = protocol::LifecyclePhase::Active;
	std::uint64_t sample_time_us = 0U;
	std::uint32_t lifecycle_flags = protocol::EntityLifecycleFlagNone;
	std::uint32_t class_id = 0U;
	std::uint64_t parent_entity_id = 0U;
	std::array<float, 3U> position_world{};
	std::array<float, 4U> orientation_local_to_world{{0.0F, 0.0F, 0.0F, 1.0F}};
	std::array<float, 3U> velocity_world{};
	std::array<float, 3U> rotational_velocity_local{};
	float radius = 0.0F;
	std::uint32_t physics_mode_flags = 0U;
	bool static_marker = false;
};

enum class Phase4EntityProjectionStatus : std::uint8_t {
	Created = 0,
	InvalidInput,
	InvalidLifecycle,
	InvalidPose,
	EncodingFailure,
	Count,
};

// Projects the universally required records for one TrustedFullState entity.
// SHIP-specific records remain owned by the inherited Phase 2 image path.
Phase4EntityProjectionStatus project_phase4_entity(
	const Phase4EntityProjectionInput& input,
	protocol::StateAtom& lifecycle,
	protocol::StateAtom& flight);

} // namespace telemetry::detail
