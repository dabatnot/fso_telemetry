#pragma once

#include "telemetry/phase2_profile_gate.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <array>
#include <cstdint>

namespace telemetry {

struct Phase2ClosureLimits {
	static constexpr std::uint32_t MaxShips = 64;
	static constexpr std::uint32_t MaxSubsystemsPerShip = 1024;
	static constexpr std::uint32_t MaxAggregateSubsystems = 4096;
};

enum class Phase2ClosureError : std::uint8_t {
	None,
	InvalidSource,
	InvalidAuthorizedEdge,
	UnauthorizedReference,
	TooManyShips,
	TooManySubsystemsPerShip,
	TooManyAggregateSubsystems,
};

struct Phase2ClosureNode {
	std::uint64_t source_key = 0;
	std::uint32_t instance_signature = 0;
	std::uint32_t ship_class_key = 0;
	bool valid = false;
	std::uint64_t support_source_key = 0;
	bool support_signature_valid = false;
	std::array<std::uint64_t, Phase2ClosureLimits::MaxShips> dock_source_keys{};
	std::uint32_t dock_source_count = 0;
	std::uint64_t group_leader_source_key = 0;
	bool group_leader_component_valid = false;
	std::array<std::uint32_t, Phase2ClosureLimits::MaxSubsystemsPerShip> subsystem_keys{};
	std::uint32_t subsystem_key_count = 0;
	float effective_mass = 0.0F;
	std::int32_t engine_index = 0;
	std::uint64_t team_source_key = 0;
	std::uint64_t proximity_source_key = 0;
	std::uint64_t target_source_key = 0;
	std::uint64_t sensor_source_key = 0;
	std::uint64_t parent_source_key = 0;
	std::uint64_t event_source_key = 0;
	std::uint64_t cargo_target_source_key = 0;
};

struct Phase2ClosureInput {
	std::uint64_t player_source_key = 0;
	std::array<Phase2ClosureNode, Phase2ClosureLimits::MaxShips> nodes{};
	std::uint32_t node_count = 0;
	std::uint32_t manifest_generation = 0;
};

struct Phase2ClassDescriptor {
	std::uint32_t instance_signature = 0;
	std::uint32_t ship_class_key = 0;
	float effective_mass = 0.0F;
	bool operator==(const Phase2ClassDescriptor& other) const noexcept
	{
		return instance_signature == other.instance_signature &&
			ship_class_key == other.ship_class_key &&
			effective_mass == other.effective_mass;
	}
};

struct Phase2Closure {
	std::array<std::uint64_t, Phase2ClosureLimits::MaxShips> ship_entities{};
	std::uint32_t ship_entity_count = 0;
	std::array<Phase2ClassDescriptor, Phase2ClosureLimits::MaxShips> class_descriptors{};
	std::uint32_t class_descriptor_count = 0;
	std::array<std::uint32_t, Phase2ClosureLimits::MaxAggregateSubsystems> subsystem_keys{};
	std::uint32_t subsystem_count = 0;
	protocol::Sha256Digest topology_fingerprint{};
	protocol::Sha256Digest catalog_fingerprint{};
	std::array<std::array<std::uint64_t, 2>, Phase2ClosureLimits::MaxShips> cargo_references{};
	std::uint32_t cargo_reference_count = 0;

	bool references_cargo_target(std::uint64_t owner, std::uint64_t target) const noexcept;
};

Phase2ClosureError build_phase2_closure(
	const Phase2ClosureInput& input, Phase2Profile profile, Phase2Closure& output) noexcept;

} // namespace telemetry
