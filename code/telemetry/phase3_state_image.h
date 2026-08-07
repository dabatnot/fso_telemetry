#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry {

constexpr std::uint64_t Phase3CockpitSensorsCoverage =
	protocol::StateDomainCoverageBitPlayerKinematics |
	protocol::StateDomainCoverageBitCoreShip |
	protocol::StateDomainCoverageBitControlInputs |
	protocol::StateDomainCoverageBitRadarSensors |
	protocol::StateDomainCoverageBitTargeting |
	protocol::StateDomainCoverageBitWeapons |
	protocol::StateDomainCoverageBitCargoDockSupport |
	protocol::StateDomainCoverageBitNavigation;
constexpr std::uint64_t Phase3StateDerivedEventCoverage =
	protocol::EventFamilyBitEntity | protocol::EventFamilyBitDamage |
	protocol::EventFamilyBitTarget | protocol::EventFamilyBitCargoScan;

constexpr std::size_t MaximumPhase3Contacts = 4096U;
constexpr std::size_t MaximumPhase3Locks = 64U;
constexpr std::size_t MaximumPhase3IncomingMissiles = 256U;
constexpr std::size_t MaximumPhase3Navpoints = 1024U;
constexpr std::size_t MaximumPhase3RouteWaypoints = 2048U;

template <std::size_t Capacity>
struct Phase3OwnedString {
	std::array<char, Capacity + 1U> bytes{};
	std::size_t size = 0U;

	bool assign(const char* source, std::size_t length) noexcept
	{
		if (source == nullptr || length > Capacity) return false;
		for (std::size_t index = 0U; index < length; ++index)
			bytes[index] = source[index];
		bytes[length] = '\0';
		size = length;
		return true;
	}
};

struct Phase3LockItem {
	std::uint16_t presence = 0U;
	bool locked = false;
	bool target_in_lock_cone = false;
	std::uint64_t target_entity_id = 0U;
	std::uint32_t subsystem_id = 0U;
	std::array<float, 3U> world_position{};
	std::uint64_t time_to_lock_remaining_us = 0U;
};

struct Phase3TargetState {
	std::uint64_t presence = 0U;
	std::uint64_t producer_sample_time_us = 0U;
	std::uint64_t current_target_entity_id = 0U;
	std::uint64_t previous_target_entity_id = 0U;
	protocol::ObjectType revealed_object_type = protocol::ObjectType::Unknown;
	Phase3OwnedString<255U> revealed_name;
	std::uint32_t revealed_class_id = 0U;
	std::uint32_t revealed_team_id = 0U;
	std::uint32_t revealed_iff_id = 0U;
	std::uint64_t time_on_target_us = 0U;
	std::uint32_t target_subsystem_id = 0U;
	std::uint32_t lock_subsystem_id = 0U;
	std::array<float, 3U> last_stealth_position{};
	std::array<float, 3U> last_stealth_velocity{};
	std::uint8_t distance_trend = 0U;
	std::uint8_t speed_trend = 0U;
	bool in_cone = false;
	std::array<float, 3U> lead_world{};
	std::uint32_t lead_bank_id = 0U;
	std::uint64_t attacker_entity_id = 0U;
	std::uint64_t dangerous_weapon_entity_id = 0U;
	std::uint64_t nearest_locked_entity_id = 0U;
	float exact_hud_distance = 0.0F;
	float exact_hud_speed = 0.0F;
};

struct Phase3RadarState {
	std::uint64_t presence = 0U;
	std::uint64_t producer_sample_time_us = 0U;
	protocol::RadarMode mode = protocol::RadarMode::Short;
	float selected_range = 0.0F;
	protocol::SensorState sensor_state = protocol::SensorState::Offline;
	float sensor_current_hits = 0.0F;
	float sensor_max_hits = 1.0F;
	float bright_range = 0.0F;
	float primitive_range = 0.0F;
	float awacs_intensity = 0.0F;
	float awacs_range = 0.0F;
	float emp_intensity = 0.0F;
	std::uint64_t emp_remaining_us = 0U;
	float jamming_intensity = 0.0F;
	float distortion_intensity = 0.0F;
	std::uint64_t first_visible_time_us = 0U;
	std::uint64_t last_contact_time_us = 0U;
};

struct Phase3RadarContact {
	std::uint64_t presence = 0U;
	std::uint64_t producer_sample_time_us = 0U;
	std::uint64_t entity_id = 0U;
	protocol::ObjectType object_type = protocol::ObjectType::Unknown;
	std::uint8_t category = 0U;
	protocol::RadarVisibility visibility = protocol::RadarVisibility::NotVisible;
	std::array<float, 3U> position_world{};
	std::array<float, 3U> velocity_world{};
	// Captured from the same radar projection tick as position_world. This is
	// expressed in the cockpit eye frame used by the standard FSO radar, not
	// reconstructed later from an independently sampled FLIGHT_STATE pose.
	std::array<float, 3U> radar_local_position{};
	float radar_projection_distance = 0.0F;
	float radius = 0.0F;
	std::uint32_t flags = protocol::ContactFlagNone;
	float icon_size = 0.0F;
	Phase3OwnedString<255U> revealed_name;
	std::uint32_t revealed_class_id = 0U;
	std::uint32_t revealed_team_id = 0U;
	std::uint32_t revealed_iff_id = 0U;
	std::uint64_t first_detection_time_us = 0U;
	std::uint64_t last_detection_time_us = 0U;
	float confidence = 0.0F;
};

struct Phase3IncomingMissile {
	std::uint16_t presence = 0U;
	std::uint8_t guidance_type = 0U;
	protocol::RadarVisibility radar_visibility = protocol::RadarVisibility::NotVisible;
	std::uint64_t entity_id = 0U;
	std::uint32_t weapon_class_id = 0U;
	std::uint32_t homing_subsystem_id = 0U;
	std::array<float, 3U> position_world{};
	std::array<float, 4U> orientation_local_to_world{{1.0F, 0.0F, 0.0F, 0.0F}};
	std::array<float, 3U> velocity_world{};
};

struct Phase3ThreatState {
	std::uint64_t presence = 0U;
	std::uint64_t producer_sample_time_us = 0U;
	protocol::ThreatLevel threat_level = protocol::ThreatLevel::None;
	std::uint64_t nearest_attacker_entity_id = 0U;
	std::uint64_t dangerous_weapon_entity_id = 0U;
	std::uint64_t nearest_homing_entity_id = 0U;
	std::size_t incoming_missile_count = 0U;
	std::array<Phase3IncomingMissile, MaximumPhase3IncomingMissiles> incoming_missiles{};
};

struct Phase3CargoState {
	std::uint64_t presence = 0U;
	std::uint64_t producer_sample_time_us = 0U;
	std::uint8_t scan_phase = 0U;
	std::uint8_t disclosure = 0U;
	std::uint64_t target_entity_id = 0U;
	std::uint32_t target_subsystem_id = 0U;
	std::uint64_t elapsed_us = 0U;
	std::uint64_t required_us = 0U;
	std::uint8_t validity_flags = 0U;
	Phase3OwnedString<511U> cargo_text;
};

struct Phase3Navpoint {
	std::uint16_t presence = 0U;
	std::uint8_t type = 0U;
	std::uint8_t flags = 0U;
	std::uint32_t navpoint_id = 0U;
	Phase3OwnedString<255U> name;
	std::array<float, 3U> position_world{};
	std::uint64_t linked_entity_id = 0U;
	std::uint32_t waypoint_list_id = 0U;
	std::uint16_t waypoint_index = 0U;
};

struct Phase3RouteWaypoint {
	std::uint32_t waypoint_list_id = 0U;
	std::uint16_t waypoint_index = 0U;
	std::array<float, 3U> position_world{};
};

struct Phase3NavigationState {
	std::uint64_t presence = 0U;
	std::uint64_t producer_sample_time_us = 0U;
	protocol::AutopilotState autopilot_state = protocol::AutopilotState::Disengaged;
	std::size_t navpoint_count = 0U;
	std::array<Phase3Navpoint, MaximumPhase3Navpoints> navpoints{};
	std::uint32_t current_navpoint_id = 0U;
	protocol::AutopilotRefusal autopilot_refusal = protocol::AutopilotRefusal::NoValidNav;
	std::size_t route_waypoint_count = 0U;
	std::array<Phase3RouteWaypoint, MaximumPhase3RouteWaypoints> route_waypoints{};
	std::uint16_t current_route_index = 0U;
	float route_speed_limit = 0.0F;
};

struct Phase3NavIdentityEntry {
	Phase3OwnedString<255U> name;
	std::uint8_t type = 0U;
	std::uint32_t navpoint_id = 0U;
	bool occupied = false;
};

struct Phase3Projection {
	std::uint64_t player_entity_id = 0U;
	std::size_t lock_count = 0U;
	std::array<Phase3LockItem, MaximumPhase3Locks> locks{};
	Phase3TargetState target;
	Phase3RadarState radar;
	std::size_t contact_count = 0U;
	std::array<Phase3RadarContact, MaximumPhase3Contacts> contacts{};
	Phase3ThreatState threat;
	Phase3CargoState cargo;
	Phase3NavigationState navigation;
	std::size_t nav_identity_count = 0U;
	std::uint32_t next_navpoint_id = 1U;
	std::array<Phase3NavIdentityEntry, MaximumPhase3Navpoints>
		nav_identities{};
};

enum class Phase3StateImageBuildStatus : std::uint8_t {
	Created = 0,
	InvalidInput,
	CapacityExceeded,
	EncodingFailed,
	AllocationFailed,
	Count,
};

Phase3StateImageBuildStatus build_phase3_cockpit_sensor_state_image(
	const protocol::StateImage& complete_ship,
	const Phase3Projection& projection,
	protocol::StateImage& output) noexcept;

} // namespace telemetry
