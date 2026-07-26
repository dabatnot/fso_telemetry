#include "telemetry/engine_adapter.h"
#include "telemetry/phase2_observation.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "globalincs/globals.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {

std::atomic<std::size_t> allocation_count{0U};
bool count_allocations = false;

void* allocate(std::size_t size)
{
	if (count_allocations) {
		++allocation_count;
	}
	if (auto* memory = std::malloc(size == 0U ? 1U : size)) {
		return memory;
	}
	throw std::bad_alloc();
}

class AllocationWindow final {
  public:
	AllocationWindow() noexcept
	{
		allocation_count.store(0U);
		count_allocations = true;
	}

	~AllocationWindow()
	{
		count_allocations = false;
	}

	std::size_t count() const noexcept
	{
		return allocation_count.load();
	}
};

template <typename Left, typename Right>
void expect_raw_vec3_equal(const Left& left, const Right& right)
{
	EXPECT_FLOAT_EQ(left.x, right.x);
	EXPECT_FLOAT_EQ(left.y, right.y);
	EXPECT_FLOAT_EQ(left.z, right.z);
}

using namespace telemetry::detail;

static_assert(MaximumPhase2PhysicalPrimaryBanks == MAX_SHIP_PRIMARY_BANKS,
	"Phase 2 primary turret-bank storage must equal the engine physical bound.");
static_assert(MaximumPhase2PhysicalSecondaryBanks == MAX_SHIP_SECONDARY_BANKS,
	"Phase 2 secondary turret-bank storage must equal the engine physical bound.");

template <typename Source, typename = void>
struct has_phase2_s8_ship_blocks : std::false_type {};

template <typename Source>
struct has_phase2_s8_ship_blocks<Source,
	std::void_t<decltype(std::declval<Source&>().weapons),
		decltype(std::declval<Source&>().support),
		decltype(std::declval<Source&>().docking),
		decltype(std::declval<Source&>().subsystems)>> : std::true_type {};

template <typename Observation, typename = void>
struct has_wp02_owned_raw_static_catalog : std::false_type {};

template <typename Observation>
struct has_wp02_owned_raw_static_catalog<Observation,
	std::void_t<decltype(std::declval<Observation&>().raw_static_catalog)>>
	: std::true_type {};

#define DEFINE_WP02_CATALOG_TRAIT(name, ...)                                  \
	template <typename Observation, typename = void>                           \
	struct name : std::false_type {};                                          \
	template <typename Observation>                                            \
	struct name<Observation, std::void_t<__VA_ARGS__>> : std::true_type {}

DEFINE_WP02_CATALOG_TRAIT(has_wp02_catalog_shape,
	decltype(std::declval<Observation&>().raw_static_catalog.class_count),
	decltype(std::declval<Observation&>().raw_static_catalog.weapon_count),
	decltype(std::declval<Observation&>().raw_static_catalog.auxiliary_count),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.aggregate_subsystem_count),
	decltype(std::declval<Observation&>().raw_static_catalog.class_definitions),
	decltype(std::declval<Observation&>().raw_static_catalog.weapon_definitions),
	decltype(std::declval<Observation&>().raw_static_catalog.auxiliary_entries));

DEFINE_WP02_CATALOG_TRAIT(has_wp02_complete_class_facts,
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].class_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].internal_name.view()),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].model_mass),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].density),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].model_inertia),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].effective_mass),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].effective_inertia),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].center_of_mass),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].max_velocity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.afterburner_max_velocity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.booster_max_velocity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.max_rotational_velocity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].max_rear_velocity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].forward_accel_time),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.afterburner_forward_accel_time),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.booster_forward_accel_time),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].forward_decel_time),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].slide_accel_time),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].slide_decel_time),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].max_hull_strength),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].max_shield_strength),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].has_afterburner),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.afterburner_fuel_capacity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.afterburner_burn_rate),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.afterburner_recover_rate),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.afterburner_min_start_fuel),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.afterburner_cooldown_seconds),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].has_scan),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].scan_time_ms),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].scan_range_normal),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].scan_range_capital),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.scanning_range_multiplier),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.has_glide),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].glide_cap),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].has_autoaim),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].autoaim_fov_rad),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].species_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].ship_type_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].iff_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].wing_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].armor_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.damage_type_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.countermeasure_capacity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.countermeasure_cargo_size),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.countermeasure_uses_capacity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.countermeasure_weapon_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.countermeasure_firewait_ms));

DEFINE_WP02_CATALOG_TRAIT(has_wp02_complete_nested_class_facts,
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].subsystem_count),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].subsystem_offset),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].subsystem_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].internal_name.view()),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].alt_name.view()),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].hud_name.view()),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].local_position),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].radius),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].max_hits),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].subsystem_type_source),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].raw_static_flags),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage[0].armor_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].bank_count),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].bank_offset),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].primary_bank_count),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].secondary_bank_count),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].tertiary_bank_count),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].turret_bank_count),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].bank_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].owner_subsystem_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].family_source),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].source_family),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].bank_index),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].weapon_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].consumes_ammunition),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].has_capacity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].capacity),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].firing_pattern_source_code),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].fire_point_count),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.bank_storage[0].fire_points));

DEFINE_WP02_CATALOG_TRAIT(has_wp02_complete_weapon_facts,
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].weapon_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].internal_name.view()),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].title.view()),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].weapon_subtype_source),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].raw_class_flags),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].max_speed),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].mass),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].gravity_constant),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.velocity_inherit_amount),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].lifetime_seconds),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.acceleration_time_seconds),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].minimum_range),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].optimal_range),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].maximum_range),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].fire_wait_seconds),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].energy_consumed),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].damage),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.damage_type_capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.shockwave_outer_radius),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].raw_effect_flags),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.guidance_type_source),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.guidance_fov_source_cosine),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.lock_time_seconds),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.lock_fov_source_cosine),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].cargo_size),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].rearm_rate_seconds),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.reloaded_per_batch),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].burst_shots),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0]
			.burst_delay_seconds),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].swarm_count_source),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.weapon_definitions[0].shots_source));

DEFINE_WP02_CATALOG_TRAIT(has_wp02_capture_local_auxiliary_facts,
	decltype(std::declval<Observation&>()
			.raw_static_catalog.auxiliary_entries[0].registry),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.auxiliary_entries[0].capture_key),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.auxiliary_entries[0].name.view()),
	decltype(std::declval<Observation&>()
			.raw_static_catalog.auxiliary_entries[0]
			.firing_pattern_source_code));

DEFINE_WP02_CATALOG_TRAIT(has_wp02_per_ship_loadout_references,
	decltype(std::declval<Observation&>()
			.ships[0].raw_static_references.class_capture_key),
	decltype(std::declval<Observation&>()
			.ships[0].raw_static_references.weapon_count),
	decltype(std::declval<Observation&>()
			.ships[0].raw_static_references.weapon_capture_keys),
	decltype(std::declval<Observation&>()
			.ships[0].raw_static_references.auxiliary_count),
	decltype(std::declval<Observation&>()
			.ships[0].raw_static_references.auxiliary_capture_keys));

#undef DEFINE_WP02_CATALOG_TRAIT

template <typename Node, typename = void>
struct has_wp02_reciprocal_topology_authority : std::false_type {};

template <typename Node>
struct has_wp02_reciprocal_topology_authority<Node,
	std::void_t<decltype(std::declval<Node&>().support_capture_key),
		decltype(std::declval<Node&>().group_leader_capture_key),
		decltype(std::declval<Node&>().direct_docking_capture_keys),
		decltype(std::declval<Node&>().direct_docking_reciprocal)>>
	: std::true_type {};

template <typename Key, typename = void>
struct has_forbidden_object_index : std::false_type {};

template <typename Key>
struct has_forbidden_object_index<Key,
	std::void_t<decltype(std::declval<Key&>().object_index)>>
	: std::true_type {};

template <typename Node, typename = void>
struct has_wp02_forbidden_topology_engine_authorities : std::false_type {};

template <typename Node>
struct has_wp02_forbidden_topology_engine_authorities<Node,
	std::void_t<decltype(std::declval<Node&>().key),
		decltype(std::declval<Node&>().dock_leader_key),
		decltype(std::declval<Node&>().leader_object_index),
		decltype(std::declval<Node&>().leader_object_signature),
		decltype(std::declval<Node&>().support_ship_objnum),
		decltype(std::declval<Node&>().support_ship_signature),
		decltype(std::declval<Node&>().direct_docking_signatures)>>
	: std::true_type {};

template <typename Observation, typename = void>
struct has_wp02_player_object_index : std::false_type {};
template <typename Observation>
struct has_wp02_player_object_index<Observation,
	std::void_t<decltype(
		std::declval<Observation&>().player_key.object_index)>>
	: std::true_type {};

template <typename Observation, typename = void>
struct has_wp02_ship_object_index : std::false_type {};
template <typename Observation>
struct has_wp02_ship_object_index<Observation,
	std::void_t<decltype(
		std::declval<Observation&>().ships[0].key.object_index)>>
	: std::true_type {};

#define DEFINE_WP02_FORBIDDEN_NODE_FIELD(trait_name, field_name)              \
	template <typename Node, typename = void>                                  \
	struct trait_name : std::false_type {};                                    \
	template <typename Node>                                                   \
	struct trait_name<Node,                                                    \
		std::void_t<decltype(std::declval<Node&>().field_name)>>                \
		: std::true_type {}

DEFINE_WP02_FORBIDDEN_NODE_FIELD(has_wp02_node_engine_key, key);
DEFINE_WP02_FORBIDDEN_NODE_FIELD(has_wp02_node_dock_leader_key, dock_leader_key);
DEFINE_WP02_FORBIDDEN_NODE_FIELD(has_wp02_node_leader_index, leader_object_index);
DEFINE_WP02_FORBIDDEN_NODE_FIELD(
	has_wp02_node_leader_signature, leader_object_signature);
DEFINE_WP02_FORBIDDEN_NODE_FIELD(
	has_wp02_node_support_index, support_ship_objnum);
DEFINE_WP02_FORBIDDEN_NODE_FIELD(
	has_wp02_node_support_signature, support_ship_signature);
DEFINE_WP02_FORBIDDEN_NODE_FIELD(
	has_wp02_node_docking_signatures, direct_docking_signatures);

#undef DEFINE_WP02_FORBIDDEN_NODE_FIELD

template <typename Observation, typename = void>
struct has_wp02_subsystem_definition_pointer_view : std::false_type {};
template <typename Observation>
struct has_wp02_subsystem_definition_pointer_view<Observation,
	std::void_t<decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.subsystem_definitions.values)>>
	: std::bool_constant<std::is_pointer_v<std::remove_reference_t<decltype(
		  std::declval<Observation&>().raw_static_catalog.class_definitions[0]
			  .subsystem_definitions.values)>>> {};

template <typename Observation, typename = void>
struct has_wp02_bank_definition_pointer_view : std::false_type {};
template <typename Observation>
struct has_wp02_bank_definition_pointer_view<Observation,
	std::void_t<decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0]
			.bank_definitions.values)>>
	: std::bool_constant<std::is_pointer_v<std::remove_reference_t<decltype(
		  std::declval<Observation&>().raw_static_catalog.class_definitions[0]
			  .bank_definitions.values)>>> {};

template <typename Observation, typename = void>
struct has_wp02_flat_owned_nested_layout : std::false_type {};
template <typename Observation>
struct has_wp02_flat_owned_nested_layout<Observation,
	std::void_t<decltype(std::declval<Observation&>()
			.raw_static_catalog.subsystem_storage),
		decltype(std::declval<Observation&>().raw_static_catalog.bank_storage),
		decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].subsystem_offset),
		decltype(std::declval<Observation&>()
			.raw_static_catalog.class_definitions[0].bank_offset)>>
	: std::true_type {};

template <typename Value, typename = void>
struct has_wp02_raw_static_catalog_member : std::false_type {};

template <typename Value>
struct has_wp02_raw_static_catalog_member<Value,
	std::void_t<decltype(std::declval<Value&>().raw_static_catalog)>>
	: std::true_type {};

template <typename Value, typename = void>
struct has_wp02_raw_static_references_member : std::false_type {};

template <typename Value>
struct has_wp02_raw_static_references_member<Value,
	std::void_t<decltype(std::declval<Value&>().raw_static_references)>>
	: std::true_type {};

#define DEFINE_WP02_FORBIDDEN_CATALOG_INDEX(trait_name, expression)           \
	template <typename Observation, typename = void>                           \
	struct trait_name : std::false_type {};                                    \
	template <typename Observation>                                            \
	struct trait_name<Observation, std::void_t<decltype(expression)>>          \
		: std::true_type {}

DEFINE_WP02_FORBIDDEN_CATALOG_INDEX(has_wp02_species_source_index,
	std::declval<Observation&>().raw_static_catalog.class_definitions[0]
		.species_source_index);
DEFINE_WP02_FORBIDDEN_CATALOG_INDEX(has_wp02_ship_type_source_index,
	std::declval<Observation&>().raw_static_catalog.class_definitions[0]
		.ship_type_source_index);
DEFINE_WP02_FORBIDDEN_CATALOG_INDEX(has_wp02_damage_type_source_index,
	std::declval<Observation&>().raw_static_catalog.weapon_definitions[0]
		.damage_type_source_index);

#undef DEFINE_WP02_FORBIDDEN_CATALOG_INDEX

#define DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(trait_name, expression)           \
	template <typename Observation, typename = void>                           \
	struct trait_name : std::false_type {};                                    \
	template <typename Observation>                                            \
	struct trait_name<Observation, std::void_t<decltype(expression)>>          \
		: std::true_type {}

DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(has_wp02_support_object_signature,
	std::declval<Observation&>().ships[0].support.support_object_signature);
DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(has_wp02_support_ai_index,
	std::declval<Observation&>().ships[0].support.ai_index);
DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(has_wp02_support_ship_objnum,
	std::declval<Observation&>().ships[0].support.support_ship_objnum);
DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(has_wp02_support_ship_signature,
	std::declval<Observation&>().ships[0].support.support_ship_signature);
DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(has_wp02_docking_remote_signature,
	std::declval<Observation&>()
		.ships[0].docking.relations[0].remote_object_signature);
DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(has_wp02_cargo_target_signature,
	std::declval<Observation&>().player_cargo_scan.target_object_signature);
DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(has_wp02_ship_source_object_index,
	std::declval<Phase2ShipSource&>().object_index);
DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY(has_wp02_ship_source_object_signature,
	std::declval<Phase2ShipSource&>().object_signature);

#undef DEFINE_WP02_FORBIDDEN_DTO_AUTHORITY

template <typename Observation, typename = void>
struct has_wp02_forbidden_referenced_aggregates : std::false_type {};

template <typename Observation>
struct has_wp02_forbidden_referenced_aggregates<Observation,
	std::void_t<decltype(std::declval<Observation&>()
			.raw_static_catalog.referenced_class_source_keys),
		decltype(std::declval<Observation&>()
			.raw_static_catalog.referenced_weapon_source_keys)>>
	: std::true_type {};

template <typename View, typename = void>
struct has_wp02_fso_static_extractor_seam : std::false_type {};

template <typename View>
struct has_wp02_fso_static_extractor_seam<View,
	std::void_t<decltype(&View::extract_static_authorities_for_test)>>
	: std::true_type {};

template <typename Method>
struct wp02_member_function_arguments;

template <typename Class, typename Result, typename Input, typename Output>
struct wp02_member_function_arguments<
	Result (Class::*)(const Input&, Output&) const noexcept> {
	using input_type = Input;
	using output_type = Output;
};

template <typename Input, typename = void>
struct has_wp02_fso_extractor_fixture_authorities : std::false_type {};

template <typename Input>
struct has_wp02_fso_extractor_fixture_authorities<Input,
	std::void_t<decltype(std::declval<Input&>().guards_valid),
		decltype(std::declval<Input&>().ship_info.effective_mass),
		decltype(std::declval<Input&>().ship_info.max_rear_velocity),
		decltype(std::declval<Input&>().weapon_info.mass),
		decltype(std::declval<Input&>().weapon_info.damage),
		decltype(std::declval<Input&>().model.center_of_mass),
		decltype(std::declval<Input&>().subsystems[0].local_position),
		decltype(std::declval<Input&>().banks[0].fire_points[0]),
		decltype(std::declval<Input&>().registries.species.capture_key),
		decltype(std::declval<Input&>().registries.weapon_damage_type.capture_key)>>
	: std::true_type {};

template <typename Key, typename = void>
struct is_wp02_opaque_capture_local_key : std::false_type {};

template <typename Key>
struct is_wp02_opaque_capture_local_key<Key,
	std::void_t<decltype(std::declval<Key&>().value)>>
	: std::bool_constant<
		  !std::is_arithmetic_v<Key> &&
		  !std::is_pointer_v<Key> &&
		  !std::is_same_v<Key, EngineEntityKey> &&
		  std::is_trivially_copyable_v<Key> &&
		  !has_forbidden_object_index<Key>::value> {};

template <typename Input, typename = void>
struct has_wp02_exhaustive_hostile_mapper_authorities : std::false_type {};

template <typename Input>
struct has_wp02_exhaustive_hostile_mapper_authorities<Input,
	std::void_t<
		decltype(std::declval<Input&>().weapon_info.fire_wait_seconds),
		decltype(std::declval<Input&>().banks[0].num_slots)>>
	: std::true_type {};

template <typename Source, typename Destination>
void copy_phase2_s8_ship_blocks(const Source& source, Destination& output) noexcept
{
	if constexpr (has_phase2_s8_ship_blocks<Source>::value) {
		output.weapons = source.weapons;
		output.support = source.support;
		output.docking = source.docking;
		output.subsystems = source.subsystems;
	}
}

template <typename Source, typename Destination>
void copy_wp02_static_catalog(
	const Source& source, Destination& output) noexcept
{
	output.raw_static_catalog = source.raw_static_catalog;
	output.raw_static_references = source.raw_static_references;
}

bool wp02_fake_source_exceeds_limits(
	const Phase2RawStaticCatalog& catalog) noexcept
{
	if (catalog.class_count > MaximumPhase2StaticClasses ||
		catalog.weapon_count > MaximumPhase2StaticWeapons ||
		catalog.auxiliary_count > MaximumPhase2StaticAuxiliaryEntries ||
		catalog.aggregate_subsystem_count > MaximumPhase2StaticSubsystems) {
		return true;
	}
	for (std::uint32_t index = 0U; index < catalog.class_count; ++index) {
		const auto& ship_class = catalog.class_definitions[index];
		if (ship_class.subsystem_count > MaximumPhase2SubsystemsPerShip ||
			ship_class.bank_count > MaximumPhase2StaticBanksPerClass ||
			ship_class.primary_bank_count >
				MaximumPhase2WeaponBanksPerFamily ||
			ship_class.secondary_bank_count >
				MaximumPhase2WeaponBanksPerFamily ||
			ship_class.tertiary_bank_count >
				MaximumPhase2WeaponBanksPerFamily ||
			ship_class.turret_bank_count >
				MaximumPhase2WeaponBanksPerFamily ||
			ship_class.bank_offset > catalog.bank_storage.size() ||
			ship_class.bank_count >
				catalog.bank_storage.size() - ship_class.bank_offset) {
			return true;
		}
		for (std::uint32_t bank = 0U; bank < ship_class.bank_count; ++bank) {
			if (catalog.bank_storage[ship_class.bank_offset + bank]
					.fire_point_count > MaximumPhase2StaticFirePoints) {
				return true;
			}
		}
	}
	return false;
}

bool phase2_s8_fake_source_exceeds_limits(
	const Phase2ShipSource& source) noexcept
{
	return source.weapons.primary_bank_count >
			MaximumPhase2WeaponBanksPerFamily ||
		source.weapons.secondary_bank_count >
			MaximumPhase2WeaponBanksPerFamily ||
		source.docking.relation_count >
			MaximumPhase2DockRelationsPerShip ||
		source.subsystems.count > MaximumPhase2SubsystemsPerShip;
}

template <typename Ship>
void clear_phase2_s8_relations(Ship& ship) noexcept
{
	if constexpr (has_phase2_s8_ship_blocks<Ship>::value) {
		ship.support = {};
		ship.docking = {};
	}
}

class FakePhase2EngineReadView final : public Phase2EngineReadView {
  public:
	bool main_thread = true;
	bool mission = true;
	bool player = true;
	bool player_object = true;
	bool player_ship = true;
	bool consistent = true;
	std::size_t ship_count = 1U;
	// Deliberately invisible to Phase2EngineReadView: WP02 consumes only the
	// authorized closure and must not derive it from global mission population.
	std::size_t global_ship_count = 1U;
	mutable std::size_t read_calls = 0U;
	mutable std::size_t root_read_calls = 0U;
	mutable std::size_t discovery_read_calls = 0U;
	mutable std::size_t control_read_calls = 0U;
	mutable std::size_t cargo_read_calls = 0U;
	std::string internal_name = "owned-alpha";
	std::size_t failing_read_index = std::numeric_limits<std::size_t>::max();
	bool duplicate_signatures = false;
	bool controls_read_succeeds = true;
	bool cargo_read_succeeds = true;
	bool player_only_s8_relations = false;
	bool use_discovery_node_sources = false;
	Phase2SourceReadStatus discovery_status = Phase2SourceReadStatus::Valid;
	bool use_discovery_status_by_key = false;
	std::array<Phase2SourceReadStatus, MaximumPhase2ObservationShips>
		discovery_status_by_key{};
	std::array<Phase2DiscoveryNode, MaximumPhase2ObservationShips>
		discovery_node_sources{};
	std::unique_ptr<Phase2ShipSource> block_source =
		std::make_unique<Phase2ShipSource>();
	std::array<Phase2ShipSource*, MaximumPhase2ObservationShips>
		block_source_by_ship{};
	PlayerControlObservation control_source;
	PlayerCargoScanObservation cargo_source;

	bool current_thread_is_main() const noexcept override
	{
		return main_thread;
	}
	bool in_mission() const noexcept override
	{
		return mission;
	}
	bool player_exists() const noexcept override
	{
		return player;
	}
	bool player_object_exists() const noexcept override
	{
		return player_object;
	}
	bool player_ship_exists() const noexcept override
	{
		return player_ship;
	}
	bool player_source_is_consistent() const noexcept override
	{
		return consistent;
	}
	SourceReadResult read_player_root_key(EngineEntityKey& output) const noexcept override
	{
		++root_read_calls;
		if (ship_count == 0U || failing_read_index == 0U) {
			return {Phase2SourceReadStatus::InvalidSource};
		}
		output.object_signature = 1U;
		return {Phase2SourceReadStatus::Valid};
	}
	SourceReadResult read_discovery_node(
		EngineEntityKey key, Phase2DiscoveryNode& output) const noexcept override
	{
		++discovery_read_calls;
		const auto keyed_status =
			use_discovery_status_by_key && key.object_signature > 0U &&
				key.object_signature <= discovery_status_by_key.size()
			? discovery_status_by_key[key.object_signature - 1U]
			: discovery_status;
		if (keyed_status != Phase2SourceReadStatus::Valid) {
			return {keyed_status};
		}
		if (key.object_signature == 0U ||
			key.object_signature > ship_count) {
			return {Phase2SourceReadStatus::InvalidSource};
		}
		if (use_discovery_node_sources) {
			output = discovery_node_sources[key.object_signature - 1U];
			return {Phase2SourceReadStatus::Valid};
		}
		output = {};
		output.capture_key.value =
			duplicate_signatures
			? 1U
			: static_cast<std::uint32_t>(key.object_signature);
		return {Phase2SourceReadStatus::Valid};
	}
	SourceReadResult read_ship(
		EngineEntityKey key, Phase2ShipSource& output) const noexcept override
	{
		++read_calls;
		if (key.object_signature == 0U) {
			return {Phase2SourceReadStatus::InvalidSource};
		}
		const auto index = static_cast<std::size_t>(key.object_signature - 1U);
		if (index >= ship_count || index == failing_read_index) {
			return {Phase2SourceReadStatus::InvalidSource};
		}
		const auto* selected_source = block_source_by_ship[index] != nullptr
			? block_source_by_ship[index]
			: block_source.get();
		if (phase2_s8_fake_source_exceeds_limits(*selected_source) ||
			wp02_fake_source_exceeds_limits(
				selected_source->raw_static_catalog)) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
		output.internal_name = internal_name;
		output.identity = selected_source->identity;
		output.lifecycle = selected_source->lifecycle;
		output.flight = selected_source->flight;
		output.damage = selected_source->damage;
		output.shields = selected_source->shields;
		output.energy = selected_source->energy;
		output.propulsion = selected_source->propulsion;
		copy_phase2_s8_ship_blocks(*selected_source, output);
		copy_wp02_static_catalog(*selected_source, output);
		if (player_only_s8_relations && index != 0U) {
			clear_phase2_s8_relations(output);
		}
		return {Phase2SourceReadStatus::Valid};
	}
	bool read_player_controls(PlayerControlObservation& output) const noexcept override
	{
		++control_read_calls;
		output = control_source;
		return controls_read_succeeds;
	}
	bool read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept override
	{
		++cargo_read_calls;
		output = cargo_source;
		return cargo_read_succeeds;
	}
};

Phase2CaptureResult collect_fake_phase2_observation(const FakePhase2EngineReadView& source,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationDto& output,
	Phase2ObservationProjection projection =
		Phase2ObservationProjection::DiscoveryExtension) noexcept
{
	Phase2ObservationSelection selection;
	if (source.ship_count > MaximumPhase2ObservationShips) {
		selection.count = source.ship_count;
	} else {
		selection.count = source.ship_count;
		for (std::size_t index = 0U; index < selection.count; ++index) {
			selection.ship_keys[index].value =
				static_cast<std::uint32_t>(index + 1U);
		}
	}
	return collect_phase2_observation(
		source, selection, producer_sample_time_us, output, projection);
}

void reset_phase2_ship_source(Phase2ShipSource& source) noexcept
{
	source.~Phase2ShipSource();
	new (&source) Phase2ShipSource;
}

class RecordingPhase2SeamDouble final : public Phase2SeamTestDouble {
  public:
	std::size_t cleanup_calls = 0U;
	std::size_t support_calls = 0U;
	std::size_t control_calls = 0U;
	std::size_t cargo_calls = 0U;
	std::size_t socket_calls = 0U;
	ShipCleanupFact last_cleanup;
	SupportTransitionFact last_support;
	ControlTargetAuthority last_control = ControlTargetAuthority::Ship;
	CargoAuthorityFact last_cargo;

	void on_ship_cleanup(const ShipCleanupFact& fact) noexcept override
	{
		++cleanup_calls;
		last_cleanup = fact;
	}
	void on_support_transition(const SupportTransitionFact& fact) noexcept override
	{
		++support_calls;
		last_support = fact;
	}
	void on_control_target(ControlTargetAuthority authority) noexcept override
	{
		++control_calls;
		last_control = authority;
	}
	void on_cargo_authority(const CargoAuthorityFact& fact) noexcept override
	{
		++cargo_calls;
		last_cargo = fact;
	}
};

static_assert(!std::is_pointer_v<decltype(Phase2ObservationDto{}.ships)>);
static_assert(!std::is_pointer_v<decltype(Phase2ObservationDto{}.player_key)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.identity.internal_name)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.lifecycle)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.flight)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.damage)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.shields)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.energy)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.propulsion)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.weapons)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.support)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.docking)>);
static_assert(!std::is_pointer_v<decltype(ShipObservationDto{}.subsystems)>);
static_assert(!std::is_pointer_v<decltype(Phase2ObservationDto{}.player_controls)>);
static_assert(!std::is_pointer_v<decltype(Phase2ObservationDto{}.player_cargo_scan)>);
static_assert(std::is_base_of_v<Phase2EngineReadView, FsoEngineReadView>);
static_assert(std::is_nothrow_constructible_v<FsoEngineReadView>);
static_assert(std::is_trivially_copyable_v<Phase2SeamHandoffSnapshot>);
static_assert(static_cast<std::uint8_t>(Phase2CaptureStatus::UnsupportedEngineState) == 3U);
static_assert(static_cast<std::uint8_t>(Phase2CaptureStatus::UnsupportedEngineState) <
	static_cast<std::uint8_t>(Phase2CaptureStatus::SourceLimitExceeded));
static_assert(static_cast<std::uint8_t>(Phase2CaptureStatus::Count) ==
	static_cast<std::uint8_t>(Phase2CaptureStatus::SourceLimitExceeded) + 1U);
static_assert(std::tuple_size_v<decltype(ShipWeaponsObservation{}.primary_banks)> ==
	MaximumPhase2WeaponBanksPerFamily);
static_assert(std::tuple_size_v<decltype(ShipWeaponsObservation{}.secondary_banks)> ==
	MaximumPhase2WeaponBanksPerFamily);
static_assert(std::tuple_size_v<decltype(ShipWeaponsObservation{}.primary_banks)> == 64U);
static_assert(std::tuple_size_v<decltype(ShipWeaponsObservation{}.secondary_banks)> == 64U);
static_assert(noexcept(telemetry::OnShipCleanup(1U, ShipCleanupMode::Destroyed)));
static_assert(noexcept(telemetry::OnSupportTransition(
	1U, 2U, 3U, SupportTransitionReason::Complete, 4U)));
static_assert(noexcept(telemetry::OnControlTarget(ControlTargetAuthority::Ship)));
static_assert(noexcept(telemetry::OnCargoAuthority(CargoAuthorityFact{})));

template <typename Source>
void initialize_s8_ship_blocks(Source& blocks, bool maximum)
{
	if constexpr (has_phase2_s8_ship_blocks<Source>::value) {
		blocks.weapons.presence = telemetry::protocol::WeaponStatePresenceFlagCountermeasure;
		blocks.weapons.countermeasure_count = 7U;
		blocks.weapons.countermeasure_maximum = 7U;
		blocks.weapons.countermeasure_class_source_key.value = 17U;
		blocks.weapons.primary_bank_count =
			static_cast<std::uint8_t>(maximum ? MaximumPhase2WeaponBanksPerFamily : 1U);
		blocks.weapons.secondary_bank_count =
			static_cast<std::uint8_t>(maximum ? MaximumPhase2WeaponBanksPerFamily : 1U);
		for (std::size_t index = 0U; index < blocks.weapons.primary_bank_count; ++index) {
			auto& bank = blocks.weapons.primary_banks[index];
			bank.source_bank_key = static_cast<std::uint32_t>(index);
			bank.weapon_class_source_key.value =
				static_cast<std::uint32_t>(100U + index);
			bank.family = ShipWeaponBankFamily::Primary;
			bank.ammunition_current = static_cast<std::uint16_t>(10U + index);
			bank.ammunition_initial = static_cast<std::uint16_t>(20U + index);
			bank.cooldown_remaining_us = 1'000U + index;
		}
		for (std::size_t index = 0U; index < blocks.weapons.secondary_bank_count; ++index) {
			auto& bank = blocks.weapons.secondary_banks[index];
			bank.source_bank_key = static_cast<std::uint32_t>(index);
			bank.weapon_class_source_key.value =
				static_cast<std::uint32_t>(200U + index);
			bank.family = ShipWeaponBankFamily::Secondary;
			bank.ammunition_current = static_cast<std::uint16_t>(30U + index);
			bank.ammunition_initial = static_cast<std::uint16_t>(40U + index);
			bank.cooldown_remaining_us = 2'000U + index;
		}

		blocks.support = {};
		blocks.support.raw_countermeasure_capacity = 7U;
		blocks.docking = {};
		blocks.subsystems.count =
			static_cast<std::uint16_t>(maximum ? MaximumPhase2SubsystemsPerShip : 1U);
		for (std::size_t index = 0U; index < blocks.subsystems.count; ++index) {
			auto& subsystem = blocks.subsystems.values[index];
			subsystem.presence = telemetry::protocol::SubsystemStatePresenceFlagNone;
			subsystem.source_key.value = static_cast<std::uint32_t>(index);
			subsystem.kind = index == 0U ? ShipSubsystemKind::Turret : ShipSubsystemKind::Generic;
			subsystem.hits_current = 25.0F;
			subsystem.hits_maximum = 100.0F;
			subsystem.position_local = {{static_cast<float>(index), 2.0F, 3.0F}};
			subsystem.orientation_local = {{1.0F, 0.0F, 0.0F, 0.0F}};
		}
	}
}

template <typename Source>
void run_s8_exact_copy_and_poison_contract()
{
	if constexpr (!has_phase2_s8_ship_blocks<Source>::value) {
		FAIL() << "WP02-S8 RED: Phase2ShipSource does not expose weapons/support/docking/subsystems";
	} else {
		FakePhase2EngineReadView source;
		initialize_s8_ship_blocks(*source.block_source, false);

		auto observation_storage = std::make_unique<Phase2ObservationDto>();

		auto& observation = *observation_storage;
		ASSERT_EQ(Phase2CaptureStatus::Valid,
			collect_fake_phase2_observation(source, 900U, observation).status);
		ASSERT_EQ(1U, observation.ships.size());
		const auto& ship = observation.ships.front();
		EXPECT_EQ(1U, ship.weapons.primary_bank_count);
		EXPECT_EQ(100U,
			ship.weapons.primary_banks[0].weapon_class_source_key.value);
		EXPECT_EQ(1U, ship.weapons.secondary_bank_count);
		EXPECT_EQ(200U,
			ship.weapons.secondary_banks[0].weapon_class_source_key.value);
		EXPECT_EQ(7U, ship.weapons.countermeasure_count);
		EXPECT_EQ(1U, ship.subsystems.count);
		EXPECT_EQ(ShipSubsystemKind::Turret, ship.subsystems.values[0].kind);
		EXPECT_EQ((std::array<float, 3U>{{0.0F, 2.0F, 3.0F}}),
			ship.subsystems.values[0].position_local);

		initialize_s8_ship_blocks(*source.block_source, false);
		source.block_source->weapons.primary_banks[0]
			.weapon_class_source_key.value = 999U;
		source.block_source->subsystems.values[0].hits_current = 99.0F;
		EXPECT_EQ(100U,
			ship.weapons.primary_banks[0].weapon_class_source_key.value);
		EXPECT_EQ(25.0F, ship.subsystems.values[0].hits_current);
	}
}

template <typename Source>
void run_s8_maxima_and_plus_one_contract()
{
	if constexpr (!has_phase2_s8_ship_blocks<Source>::value) {
		FAIL() << "WP02-S8 RED: maximum pre-ID ship blocks are absent from Phase2ShipSource";
	} else {
		FakePhase2EngineReadView source;
		initialize_s8_ship_blocks(*source.block_source, true);
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		ASSERT_EQ(Phase2CaptureStatus::Valid,
			collect_fake_phase2_observation(source, 901U, observation).status);
		ASSERT_EQ(1U, observation.ships.size());
		EXPECT_EQ(64U, observation.ships[0].weapons.primary_bank_count);
		EXPECT_EQ(64U, observation.ships[0].weapons.secondary_bank_count);
		EXPECT_EQ(1024U, observation.ships[0].subsystems.count);

		source.block_source->weapons.primary_bank_count = 65U;
		EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded,
			collect_fake_phase2_observation(source, 902U, observation).status);
		EXPECT_TRUE(observation.ships.empty());
		initialize_s8_ship_blocks(*source.block_source, true);
		source.block_source->weapons.secondary_bank_count = 65U;
		EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded,
			collect_fake_phase2_observation(source, 903U, observation).status);
		EXPECT_TRUE(observation.ships.empty());
		initialize_s8_ship_blocks(*source.block_source, true);
		source.block_source->subsystems.count = 1025U;
		EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded,
			collect_fake_phase2_observation(source, 904U, observation).status);
		EXPECT_TRUE(observation.ships.empty());
		initialize_s8_ship_blocks(*source.block_source, true);
		source.ship_count = MaximumPhase2ObservationShips;
		source.player_only_s8_relations = true;
		// This branch prices the docking closure maximum; keep the independent
		// countermeasure/support relation absent for non-player synthetic ships.
		source.block_source->weapons.presence =
			telemetry::protocol::WeaponStatePresenceFlagNone;
		source.block_source->weapons.countermeasure_count = 0U;
		source.block_source->weapons.countermeasure_maximum = 0U;
		source.block_source->support.raw_countermeasure_capacity = 0U;
		source.block_source->docking.relation_count = 64U;
		for (std::size_t index = 0U; index < 64U; ++index) {
			source.block_source->docking.relations[index] = {
				{static_cast<std::uint32_t>(2U + index % 63U)},
				static_cast<std::uint16_t>(index),
				static_cast<std::uint16_t>(index)};
		}
		EXPECT_EQ(Phase2CaptureStatus::Valid,
			collect_fake_phase2_observation(source, 905U, observation).status);
		source.block_source->docking.relation_count = 65U;
		EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded,
			collect_fake_phase2_observation(source, 906U, observation).status);
		EXPECT_TRUE(observation.ships.empty());
	}
}

template <typename Source>
void run_s8_fail_closed_contract()
{
	if constexpr (!has_phase2_s8_ship_blocks<Source>::value) {
		FAIL() << "WP02-S8 RED: invalid pre-ID block inputs cannot yet be exercised";
	} else {
		FakePhase2EngineReadView source;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		auto expect_atomic_rejection = [&](const Source& invalid) {
			*source.block_source = invalid;
			observation.ships.emplace_back();
			const auto result = collect_fake_phase2_observation(source, 907U, observation);
			EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
			EXPECT_EQ(Phase2CaptureReason::UnsupportedShipBlock, result.reason);
			EXPECT_TRUE(observation.ships.empty());
			EXPECT_EQ(0U, observation.player_key.value);
		};

		auto invalid = std::make_unique<Source>();
		initialize_s8_ship_blocks(*invalid, false);
		invalid->weapons.presence = std::numeric_limits<std::uint64_t>::max();
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->weapons.primary_banks[0].family = ShipWeaponBankFamily::Count;
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->weapons.primary_banks[0].ammunition_current = 21U;
		invalid->weapons.primary_banks[0].ammunition_initial = 20U;
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->weapons.primary_banks[0].cooldown_remaining_us = 86'400'000'001ULL;
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->support.phase = ShipSupportPhase::Count;
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->support.presence = std::numeric_limits<std::uint64_t>::max();
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->support.repair_progress = std::numeric_limits<float>::quiet_NaN();
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->docking.presence = 1U;
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->docking.relation_count = 1U;
		invalid->docking.relations[0] = {{0U}, 0U, 0U};
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->subsystems.values[0].kind = ShipSubsystemKind::Count;
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->subsystems.values[0].presence = std::numeric_limits<std::uint64_t>::max();
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->subsystems.values[0].source_key.value =
			static_cast<std::uint32_t>(std::numeric_limits<int>::max()) + 1U;
		expect_atomic_rejection(*invalid);
		initialize_s8_ship_blocks(*invalid, false);
		invalid->subsystems.values[0].hits_current = std::numeric_limits<float>::infinity();
		expect_atomic_rejection(*invalid);
	}
}

template <typename Source>
void run_s8_maximum_ready_allocation_contract()
{
	if constexpr (!has_phase2_s8_ship_blocks<Source>::value) {
		FAIL() << "WP02-S8 RED: maximal Ready capture cannot include absent pre-ID blocks";
	} else {
		FakePhase2EngineReadView source;
		initialize_s8_ship_blocks(*source.block_source, true);
		auto buffer_storage = std::make_unique<Phase2ObservationBuffer>();
		auto& buffer = *buffer_storage;
		ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
		ASSERT_TRUE(buffer.enter_ready());
		{
			AllocationWindow allocations;
			const auto result = buffer.capture(source, 911U);
			EXPECT_EQ(Phase2CaptureStatus::Valid, result.status);
			EXPECT_EQ(0U, allocations.count());
		}
		ASSERT_EQ(1U, buffer.observation().ships.size());
		for (const auto& ship : buffer.observation().ships) {
			EXPECT_EQ(64U, ship.weapons.primary_bank_count);
			EXPECT_EQ(64U, ship.weapons.secondary_bank_count);
			EXPECT_EQ(1024U, ship.subsystems.count);
		}
	}
}

TEST(TelemetryPhase2ObservationContract, S8CopiesOwnedPreIdBlocksAndSurvivesSourcePoison)
{
	run_s8_exact_copy_and_poison_contract<Phase2ShipSource>();
}

TEST(TelemetryPhase2ObservationContract, S8AcceptsExactBlockMaximaAndRejectsEveryPlusOneAtomically)
{
	run_s8_maxima_and_plus_one_contract<Phase2ShipSource>();
}

TEST(TelemetryPhase2ObservationContract, S8RejectsInvalidKeysEnumsPresenceRelationsAndFloatsAtomically)
{
	run_s8_fail_closed_contract<Phase2ShipSource>();
}

TEST(TelemetryPhase2ObservationContract, S8ReadyMaximumCaptureAllocatesNothingWithAllBlocksPopulated)
{
	run_s8_maximum_ready_allocation_contract<Phase2ShipSource>();
}

TEST(TelemetryPhase2ObservationContract, S8ReadsAuthorizedClosureThroughExact64AndRejectsClosure65)
{
	FakePhase2EngineReadView source;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	source.global_ship_count = MaximumPhase2ObservationShips + 1U;
	EXPECT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(source, 911U, observation).status);
	ASSERT_EQ(1U, observation.ships.size());
	source.read_calls = 0U;
	source.ship_count = MaximumPhase2ObservationShips;
	EXPECT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(source, 912U, observation).status);
	ASSERT_EQ(64U, observation.ships.size());
	EXPECT_EQ(64U, source.read_calls);
	EXPECT_EQ(64U, observation.ships.back().capture_key.value);
	source.ship_count = MaximumPhase2ObservationShips + 1U;
	EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded,
		collect_fake_phase2_observation(source, 913U, observation).status);
	EXPECT_TRUE(observation.ships.empty());
}

TEST(TelemetryPhase2ObservationContract, ReviewerS8NotScannableRejectsEveryOptionalGroup)
{
	auto capture = [](const PlayerCargoScanObservation& cargo) {
		FakePhase2EngineReadView source;
		source.cargo_source = cargo;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		return collect_fake_phase2_observation(source, 914U, observation).status;
	};

	PlayerCargoScanObservation cargo;
	cargo.phase = CargoScanPhaseObservation::NotScannable;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(cargo));

	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagTarget;
	cargo.target_capture_key.value = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo = {};
	cargo.phase = CargoScanPhaseObservation::NotScannable;
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagTarget |
		telemetry::protocol::CargoScanStatePresenceFlagSubsystem;
	cargo.target_capture_key.value = 1U;
	cargo.target_subsystem_source_key.value = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo = {};
	cargo.phase = CargoScanPhaseObservation::NotScannable;
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagValidity;
	cargo.validity_flags = telemetry::protocol::ScanValidityFlagInRange;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
}

TEST(TelemetryPhase2ObservationContract, ReviewerS8V3BufferGuardsPrecedeRootReadAndClassifyAbsence)
{
	SCOPED_TRACE("REVIEW-S8V3-02 REQ-009 REQ-012 AC-004 D2-009 D2-019");
	auto ready_buffer = [] {
		auto buffer = std::make_unique<Phase2ObservationBuffer>();
		EXPECT_TRUE(buffer->provision(Phase2ProvisioningMode::ValidEnabled));
		EXPECT_TRUE(buffer->enter_ready());
		return buffer;
	};

	{
		auto buffer = ready_buffer();
		FakePhase2EngineReadView source;
		source.global_ship_count = MaximumPhase2ObservationShips + 1U;
		const auto result = buffer->capture(source, 919U);
		EXPECT_EQ(Phase2CaptureStatus::Valid, result.status);
		ASSERT_EQ(1U, buffer->observation().ships.size());
		EXPECT_EQ(1U, source.root_read_calls);
	}
	{
		auto buffer = ready_buffer();
		FakePhase2EngineReadView source;
		source.main_thread = false;
		const auto result = buffer->capture(source, 920U);
		EXPECT_EQ(Phase2CaptureReason::WrongThread, result.reason);
		EXPECT_EQ(0U, source.root_read_calls);
		EXPECT_EQ(Phase2ObservationBufferState::FailedClosed, buffer->state());
	}
	{
		auto buffer = ready_buffer();
		FakePhase2EngineReadView source;
		source.mission = false;
		const auto result = buffer->capture(source, 921U);
		EXPECT_EQ(Phase2CaptureReason::NotInMission, result.reason);
		EXPECT_EQ(0U, source.root_read_calls);
		EXPECT_EQ(Phase2ObservationBufferState::Ready, buffer->state());
	}
	{
		auto buffer = ready_buffer();
		FakePhase2EngineReadView source;
		source.player = false;
		source.player_object = false;
		source.player_ship = false;
		const auto result = buffer->capture(source, 922U);
		EXPECT_EQ(Phase2CaptureStatus::NoPlayer, result.status);
		EXPECT_EQ(0U, source.root_read_calls);
		EXPECT_EQ(Phase2ObservationBufferState::Ready, buffer->state());
	}
	{
		auto buffer = ready_buffer();
		FakePhase2EngineReadView source;
		source.player = false;
		source.player_object = true;
		source.player_ship = false;
		const auto result = buffer->capture(source, 926U);
		EXPECT_EQ(Phase2CaptureReason::PartialPlayerSource, result.reason);
		EXPECT_EQ(0U, source.root_read_calls);
		EXPECT_EQ(Phase2ObservationBufferState::FailedClosed, buffer->state());
	}
}

TEST(TelemetryPhase2ObservationContract, S9RepeatedReadyMaximumTicksAllocateNothingAndFailAtomically)
{
	FakePhase2EngineReadView source;
	initialize_s8_ship_blocks(*source.block_source, true);
	auto buffer_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& buffer = *buffer_storage;
	ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
	ASSERT_TRUE(buffer.enter_ready());

	std::size_t allocations_after_ready = 0U;
	{
		AllocationWindow allocations;
		for (std::uint64_t tick = 0U; tick < 4U; ++tick) {
			const auto sample_time = 1'000U + tick;
			const auto result = buffer.capture(source, sample_time);
			ASSERT_EQ(Phase2CaptureStatus::Valid, result.status);
			ASSERT_EQ(1U, buffer.observation().ships.size());
			EXPECT_EQ(sample_time, buffer.observation().producer_sample_time_us);
			EXPECT_EQ(sample_time, buffer.observation().player_controls.sample_time_us);
			for (const auto& ship : buffer.observation().ships) {
				EXPECT_EQ(sample_time, ship.flight.sample_time_us);
				EXPECT_EQ(sample_time, ship.weapons.sample_time_us);
				EXPECT_EQ(sample_time, ship.support.sample_time_us);
				EXPECT_EQ(sample_time, ship.docking.sample_time_us);
				ASSERT_EQ(1024U, ship.subsystems.count);
				EXPECT_EQ(sample_time, ship.subsystems.values[1023U].sample_time_us);
			}
		}
		allocations_after_ready = allocations.count();
	}
	EXPECT_EQ(0U, allocations_after_ready);

	source.block_source->weapons.primary_bank_count =
		static_cast<std::uint8_t>(MaximumPhase2WeaponBanksPerFamily + 1U);
	std::size_t failure_allocations = 0U;
	Phase2CaptureResult failure;
	{
		AllocationWindow allocations;
		failure = buffer.capture(source, 2'000U);
		failure_allocations = allocations.count();
	}
	EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded, failure.status);
	EXPECT_EQ(0U, failure_allocations);
	EXPECT_TRUE(buffer.observation().ships.empty());
	EXPECT_EQ(0U, buffer.observation().producer_sample_time_us);
	EXPECT_EQ(Phase2ObservationBufferState::FailedClosed, buffer.state());
}

TEST(TelemetryPhase2ObservationContract, CargoIdleAndScanningRequireTargetTimingAndValidityTogether)
{
	FakePhase2EngineReadView source;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	auto capture = [&](const PlayerCargoScanObservation& cargo) {
		source.cargo_source = cargo;
		return collect_fake_phase2_observation(source, 914U, observation).status;
	};
	const auto required_presence = telemetry::protocol::CargoScanStatePresenceFlagTarget |
		telemetry::protocol::CargoScanStatePresenceFlagTiming |
		telemetry::protocol::CargoScanStatePresenceFlagValidity;

	PlayerCargoScanObservation cargo;
	cargo.phase = CargoScanPhaseObservation::Idle;
		cargo.target_capture_key.value = 1U;
	cargo.elapsed_us = 3U;
	cargo.required_us = 10U;
	for (const auto missing : {telemetry::protocol::CargoScanStatePresenceFlagTarget,
			 telemetry::protocol::CargoScanStatePresenceFlagTiming,
			 telemetry::protocol::CargoScanStatePresenceFlagValidity}) {
		cargo.presence = required_presence & ~missing;
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo)) << missing;
		EXPECT_TRUE(observation.ships.empty());
	}
	cargo.presence = required_presence;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(cargo));

	cargo.phase = CargoScanPhaseObservation::Scanning;
	for (const auto missing : {telemetry::protocol::CargoScanStatePresenceFlagTarget,
			 telemetry::protocol::CargoScanStatePresenceFlagTiming,
			 telemetry::protocol::CargoScanStatePresenceFlagValidity}) {
		cargo.presence = required_presence & ~missing;
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo)) << missing;
		EXPECT_TRUE(observation.ships.empty());
	}
	cargo.presence = required_presence;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(cargo));
}

TEST(TelemetryPhase2ObservationContract, CursorAuthoritiesAreFiniteAndUseClosedPhysicalBounds)
{
	FakePhase2EngineReadView source;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	auto capture = [&](const PlayerControlObservation& controls) {
		source.control_source = controls;
		return collect_fake_phase2_observation(source, 915U, observation).status;
	};
	PlayerControlObservation controls;
	controls.flight_cursor_pitch = 3.1415927F;
	controls.flight_cursor_heading = -3.1415927F;
	controls.flight_cursor_sensitivity = 1.0F;
	controls.effective_aim_extent = 0.000001F;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(controls));

	for (const auto invalid : {std::numeric_limits<float>::infinity(),
			 std::numeric_limits<float>::quiet_NaN()}) {
		controls.flight_cursor_pitch = invalid;
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
		controls = {};
		controls.flight_cursor_heading = invalid;
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
		controls = {};
		controls.flight_cursor_sensitivity = invalid;
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
		controls = {};
		controls.effective_aim_extent = invalid;
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
		controls = {};
	}
	controls.flight_cursor_pitch =
		std::nextafter(3.1415927F, std::numeric_limits<float>::infinity());
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
	controls = {};
	controls.flight_cursor_heading =
		std::nextafter(-3.1415927F, -std::numeric_limits<float>::infinity());
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
	controls = {};
	controls.flight_cursor_sensitivity = -0.000001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
	controls.flight_cursor_sensitivity = 1.000001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
	controls = {};
	controls.effective_aim_extent = 0.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
	controls.effective_aim_extent = -0.000001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
	controls.effective_aim_extent =
		std::nextafter(3.1415927F, std::numeric_limits<float>::infinity());
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
}

TEST(TelemetryPhase2ObservationContract, DtoOwnsCopiedStringsAndRejectsDeferredSourceAliasing)
{
	FakePhase2EngineReadView source;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(source, 100U, observation).status);
	ASSERT_EQ(1U, observation.ships.size());
	EXPECT_EQ("owned-alpha", observation.ships[0].identity.internal_name);

	source.internal_name.assign("mutated-after-capture");
	EXPECT_EQ("owned-alpha", observation.ships[0].identity.internal_name);
	EXPECT_EQ(1U, observation.ships[0].capture_key.value);
}

TEST(TelemetryPhase2ObservationContract, ShipSourceCopiesFlightDamageAndShieldsWithFloatCanonicalization)
{
	FakePhase2EngineReadView source;
	source.block_source->flight.presence = 0x11U;
	source.block_source->flight.position_world = {{-0.0F, 2.0F, 3.0F}};
	source.block_source->flight.orientation_local_to_world = {{1.0F, -0.0F, 0.0F, 0.0F}};
	source.block_source->flight.velocity_world = {{4.0F, 5.0F, -0.0F}};
	source.block_source->flight.rotational_velocity_local = {{6.0F, -0.0F, 7.0F}};
	source.block_source->flight.radius = 12.0F;
	source.block_source->flight.physics_mode_flags = 0x1234U;
	source.block_source->damage.presence = 0x22U;
	source.block_source->damage.hull_current = -0.0F;
	source.block_source->damage.hull_maximum = 100.0F;
	source.block_source->damage.guardian_threshold = 25.0F;
	source.block_source->damage.armor_source_key.value = 42U;
	source.block_source->shields.presence = 0x33U;
	source.block_source->shields.has_shields = true;
	source.block_source->shields.segment_count = 2U;
	source.block_source->shields.segment_current_hits[0] = -0.0F;
	source.block_source->shields.segment_current_hits[1] = 5.0F;
	source.block_source->shields.segment_maximum_hits[0] = 10.0F;
	source.block_source->shields.segment_maximum_hits[1] = 20.0F;
	source.block_source->shields.recharge_maximum = 30.0F;
	source.block_source->shields.regeneration_rate = -0.0F;
	source.block_source->shields.deferred_transfer = 1.5F;

	auto observation_storage = std::make_unique<Phase2ObservationDto>();

	auto& observation = *observation_storage;
	constexpr std::uint64_t SampleTime = 123456U;
	const auto result = collect_fake_phase2_observation(source, SampleTime, observation);
	ASSERT_EQ(Phase2CaptureStatus::Valid, result.status);
	ASSERT_EQ(1U, observation.ships.size());
	const auto& ship = observation.ships.front();

	EXPECT_EQ(SampleTime, ship.flight.sample_time_us);
	EXPECT_EQ(0x11U, ship.flight.presence);
	EXPECT_EQ((std::array<float, 3U>{{0.0F, 2.0F, 3.0F}}), ship.flight.position_world);
	EXPECT_FALSE(std::signbit(ship.flight.position_world[0]));
	EXPECT_EQ((std::array<float, 4U>{{1.0F, 0.0F, 0.0F, 0.0F}}),
		ship.flight.orientation_local_to_world);
	EXPECT_EQ((std::array<float, 3U>{{4.0F, 5.0F, 0.0F}}), ship.flight.velocity_world);
	EXPECT_EQ((std::array<float, 3U>{{6.0F, 0.0F, 7.0F}}),
		ship.flight.rotational_velocity_local);
	EXPECT_EQ(12.0F, ship.flight.radius);
	EXPECT_EQ(0x1234U, ship.flight.physics_mode_flags);

	EXPECT_EQ(SampleTime, ship.damage.sample_time_us);
	EXPECT_EQ(0x22U, ship.damage.presence);
	EXPECT_EQ(0.0F, ship.damage.hull_current);
	EXPECT_FALSE(std::signbit(ship.damage.hull_current));
	EXPECT_EQ(100.0F, ship.damage.hull_maximum);
	EXPECT_EQ(25.0F, ship.damage.guardian_threshold);
	EXPECT_EQ(42U, ship.damage.armor_source_key.value);

	EXPECT_EQ(SampleTime, ship.shields.sample_time_us);
	EXPECT_EQ(0x33U, ship.shields.presence);
	EXPECT_TRUE(ship.shields.has_shields);
	EXPECT_EQ(2U, ship.shields.segment_count);
	EXPECT_EQ(0.0F, ship.shields.segment_current_hits[0]);
	EXPECT_FALSE(std::signbit(ship.shields.segment_current_hits[0]));
	EXPECT_EQ(5.0F, ship.shields.segment_current_hits[1]);
	EXPECT_EQ(10.0F, ship.shields.segment_maximum_hits[0]);
	EXPECT_EQ(20.0F, ship.shields.segment_maximum_hits[1]);
	EXPECT_EQ(30.0F, ship.shields.recharge_maximum);
	EXPECT_EQ(0.0F, ship.shields.regeneration_rate);
	EXPECT_FALSE(std::signbit(ship.shields.regeneration_rate));
	EXPECT_EQ(1.5F, ship.shields.deferred_transfer);
}

TEST(TelemetryPhase2ObservationContract, ShipSourceCopiesIdentityLifecycleEnergyAndPropulsionWithoutDerivedRatios)
{
	FakePhase2EngineReadView source;
	source.block_source->identity.presence =
		telemetry::protocol::ShipIdentityPresenceFlagDisplayName |
		telemetry::protocol::ShipIdentityPresenceFlagWing;
	source.block_source->identity.class_source_key = 77U;
	source.block_source->lifecycle.presence =
		telemetry::protocol::EntityLifecyclePresenceFlagClassReference;
	source.block_source->lifecycle.state = ShipLifecycleState::Disabled;
	source.block_source->lifecycle.lifecycle_flags =
		telemetry::protocol::EntityLifecycleFlagDisabled;
	source.block_source->energy.presence =
		telemetry::protocol::EnergyStatePresenceFlagWeaponEnergy;
	source.block_source->energy.weapon_energy_current = -0.0F;
	source.block_source->energy.weapon_energy_maximum = 200.0F;
	source.block_source->energy.shield_recharge_index = 2U;
	source.block_source->energy.weapon_recharge_index = 3U;
	source.block_source->energy.engine_recharge_index = 4U;
	source.block_source->energy.ets_available = true;
	source.block_source->propulsion.presence =
		telemetry::protocol::PropulsionStatePresenceFlagFuel |
		telemetry::protocol::PropulsionStatePresenceFlagConsumption |
		telemetry::protocol::PropulsionStatePresenceFlagEngagement;
	source.block_source->propulsion.propulsion_flags =
		telemetry::protocol::PropulsionFlagAfterburnerAvailable;
	source.block_source->propulsion.afterburner_fuel = 25.0F;
	source.block_source->propulsion.afterburner_capacity = 100.0F;
	source.block_source->propulsion.burn_rate = -0.0F;
	source.block_source->propulsion.recovery_rate = 2.5F;
	source.block_source->propulsion.cooldown_remaining_us = 1234U;
	source.block_source->propulsion.time_since_last_stop_us = 5678U;

	auto observation_storage = std::make_unique<Phase2ObservationDto>();

	auto& observation = *observation_storage;
	constexpr std::uint64_t SampleTime = 654321U;
	const auto result = collect_fake_phase2_observation(source, SampleTime, observation);
	ASSERT_EQ(Phase2CaptureStatus::Valid, result.status);
	ASSERT_EQ(1U, observation.ships.size());
	const auto& ship = observation.ships.front();

	EXPECT_EQ(SampleTime, ship.identity.sample_time_us);
	EXPECT_EQ(telemetry::protocol::ShipIdentityPresenceFlagDisplayName |
			telemetry::protocol::ShipIdentityPresenceFlagWing,
		ship.identity.presence);
	EXPECT_EQ(77U, ship.identity.class_source_key);
	EXPECT_EQ("owned-alpha", ship.identity.internal_name);
	EXPECT_EQ(SampleTime, ship.lifecycle.sample_time_us);
	EXPECT_EQ(telemetry::protocol::EntityLifecyclePresenceFlagClassReference,
		ship.lifecycle.presence);
	EXPECT_EQ(ShipLifecycleState::Disabled, ship.lifecycle.state);
	EXPECT_EQ(telemetry::protocol::EntityLifecycleFlagDisabled,
		ship.lifecycle.lifecycle_flags);

	EXPECT_EQ(SampleTime, ship.energy.sample_time_us);
	EXPECT_EQ(telemetry::protocol::EnergyStatePresenceFlagWeaponEnergy,
		ship.energy.presence);
	EXPECT_EQ(0.0F, ship.energy.weapon_energy_current);
	EXPECT_FALSE(std::signbit(ship.energy.weapon_energy_current));
	EXPECT_EQ(200.0F, ship.energy.weapon_energy_maximum);
	EXPECT_EQ(2U, ship.energy.shield_recharge_index);
	EXPECT_EQ(3U, ship.energy.weapon_recharge_index);
	EXPECT_EQ(4U, ship.energy.engine_recharge_index);
	EXPECT_TRUE(ship.energy.ets_available);

	EXPECT_EQ(SampleTime, ship.propulsion.sample_time_us);
	EXPECT_EQ(telemetry::protocol::PropulsionStatePresenceFlagFuel |
			telemetry::protocol::PropulsionStatePresenceFlagConsumption |
			telemetry::protocol::PropulsionStatePresenceFlagEngagement,
		ship.propulsion.presence);
	EXPECT_EQ(telemetry::protocol::PropulsionFlagAfterburnerAvailable,
		ship.propulsion.propulsion_flags);
	EXPECT_EQ(25.0F, ship.propulsion.afterburner_fuel);
	EXPECT_EQ(100.0F, ship.propulsion.afterburner_capacity);
	EXPECT_EQ(0.0F, ship.propulsion.burn_rate);
	EXPECT_FALSE(std::signbit(ship.propulsion.burn_rate));
	EXPECT_EQ(2.5F, ship.propulsion.recovery_rate);
	EXPECT_EQ(1234U, ship.propulsion.cooldown_remaining_us);
	EXPECT_EQ(5678U, ship.propulsion.time_since_last_stop_us);
}

TEST(TelemetryPhase2ObservationContract, LifecycleIdentityAndKnownMasksFailClosed)
{
	auto capture = [](const Phase2ShipSource& blocks) {
		FakePhase2EngineReadView source;
		*source.block_source = blocks;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		return collect_fake_phase2_observation(source, 100U, observation).status;
	};

	auto blocks_ptr = std::make_unique<Phase2ShipSource>();
	blocks_ptr->lifecycle.state = ShipLifecycleState::Count;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));
	blocks_ptr->lifecycle.state = static_cast<ShipLifecycleState>(0xffU);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));

	reset_phase2_ship_source(*blocks_ptr);
	blocks_ptr->identity.class_source_key =
		static_cast<std::uint32_t>(std::numeric_limits<int>::max()) + 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));

	reset_phase2_ship_source(*blocks_ptr);
	blocks_ptr->identity.presence =
		telemetry::protocol::KnownShipIdentityPresenceFlags | (1ULL << 63U);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));
	reset_phase2_ship_source(*blocks_ptr);
	blocks_ptr->lifecycle.presence =
		telemetry::protocol::KnownEntityLifecyclePresenceFlags | (1ULL << 63U);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));
	reset_phase2_ship_source(*blocks_ptr);
	blocks_ptr->lifecycle.lifecycle_flags =
		telemetry::protocol::KnownEntityLifecycleFlags | 0x80000000U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));
	reset_phase2_ship_source(*blocks_ptr);
	blocks_ptr->energy.presence =
		telemetry::protocol::KnownEnergyStatePresenceFlags | (1ULL << 63U);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));
	reset_phase2_ship_source(*blocks_ptr);
	blocks_ptr->propulsion.presence =
		telemetry::protocol::KnownPropulsionStatePresenceFlags | (1ULL << 63U);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));
	reset_phase2_ship_source(*blocks_ptr);
	blocks_ptr->propulsion.propulsion_flags =
		telemetry::protocol::KnownPropulsionFlags | 0x80000000U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(*blocks_ptr));
}

TEST(TelemetryPhase2ObservationContract, EnergyEtsAndFiniteBoundariesAreClosed)
{
	auto capture = [](const ShipEnergyObservation& energy) {
		FakePhase2EngineReadView source;
		source.block_source->energy = energy;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		return collect_fake_phase2_observation(source, 100U, observation).status;
	};

	ShipEnergyObservation energy;
	energy.ets_available = true;
	for (const auto index : {0U, 12U}) {
		energy.shield_recharge_index = static_cast<std::uint8_t>(index);
		energy.weapon_recharge_index = static_cast<std::uint8_t>(index);
		energy.engine_recharge_index = static_cast<std::uint8_t>(index);
		SCOPED_TRACE(index);
		EXPECT_EQ(Phase2CaptureStatus::Valid, capture(energy));
	}
	energy.shield_recharge_index = 13U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(energy));

	energy = {};
	energy.ets_available = false;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(energy));
	energy.weapon_recharge_index = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(energy));

	energy = {};
	energy.presence = telemetry::protocol::EnergyStatePresenceFlagWeaponEnergy;
	energy.weapon_energy_maximum = 100.0F;
	energy.weapon_energy_current = 100.001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(energy));
	energy.weapon_energy_current = std::numeric_limits<float>::quiet_NaN();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(energy));
	energy.weapon_energy_current = 50.0F;
	energy.weapon_energy_maximum = std::numeric_limits<float>::infinity();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(energy));
}

TEST(TelemetryPhase2ObservationContract, AfterburnerPresenceFlagsValuesAndTimersAreClosed)
{
	auto capture = [](const ShipPropulsionObservation& propulsion) {
		FakePhase2EngineReadView source;
		source.block_source->propulsion = propulsion;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		return collect_fake_phase2_observation(source, 100U, observation).status;
	};

	constexpr auto FuelAndConsumption =
		telemetry::protocol::PropulsionStatePresenceFlagFuel |
		telemetry::protocol::PropulsionStatePresenceFlagConsumption;
	ShipPropulsionObservation propulsion;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(propulsion));

	propulsion.presence = FuelAndConsumption;
	propulsion.propulsion_flags =
		telemetry::protocol::PropulsionFlagAfterburnerAvailable;
	propulsion.afterburner_capacity = 100.0F;
	propulsion.afterburner_fuel = 25.0F;
	propulsion.burn_rate = 5.0F;
	propulsion.recovery_rate = 2.0F;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(propulsion));

	propulsion.afterburner_capacity = 0.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.afterburner_capacity = -1.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.afterburner_capacity = std::numeric_limits<float>::infinity();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));

	propulsion.afterburner_capacity = 100.0F;
	propulsion.afterburner_fuel = -1.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.afterburner_fuel = 100.001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.afterburner_fuel = std::numeric_limits<float>::quiet_NaN();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.afterburner_fuel = 25.0F;
	propulsion.burn_rate = -1.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.burn_rate = std::numeric_limits<float>::infinity();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.burn_rate = 5.0F;
	propulsion.recovery_rate = -1.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));

	propulsion.recovery_rate = 2.0F;
	propulsion.presence |= telemetry::protocol::PropulsionStatePresenceFlagEngagement;
	propulsion.cooldown_remaining_us = 86'400'000'000ULL;
	propulsion.time_since_last_stop_us = 86'400'000'000ULL;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(propulsion));
	propulsion.cooldown_remaining_us = 86'400'000'001ULL;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.cooldown_remaining_us = 0U;
	propulsion.time_since_last_stop_us = std::numeric_limits<std::uint64_t>::max();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));

	propulsion = {};
	propulsion.propulsion_flags = telemetry::protocol::PropulsionFlagAfterburnerActive;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
	propulsion.propulsion_flags = telemetry::protocol::PropulsionFlagAfterburnerAvailable |
		telemetry::protocol::PropulsionFlagAfterburnerLocked |
		telemetry::protocol::PropulsionFlagAfterburnerActive;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(propulsion));
}

TEST(TelemetryPhase2ObservationContract, PlayerControlsAndCargoCopyExactlyOwnTextAndCanonicalizeZeros)
{
	FakePhase2EngineReadView source;
	source.control_source.presence =
		telemetry::protocol::ControlStatePresenceFlagCruise |
		telemetry::protocol::ControlStatePresenceFlagRequestCounters;
	source.control_source.mode = PlayerControlModeObservation::Camera;
	source.control_source.pitch = -0.0F;
	source.control_source.heading = 0.25F;
	source.control_source.bank = -0.5F;
	source.control_source.forward = 1.0F;
	source.control_source.sideways = -0.75F;
	source.control_source.vertical = -0.0F;
	source.control_source.action_flags =
		telemetry::protocol::ControlFlagAutoTarget |
		telemetry::protocol::ControlFlagPrimaryLinked;
	source.control_source.forward_cruise_percent = 42.0F;
	source.control_source.fire_primary_count = 1U;
	source.control_source.fire_secondary_count = 2U;
	source.control_source.fire_countermeasure_count = 3U;

	source.cargo_source.presence =
		telemetry::protocol::CargoScanStatePresenceFlagTarget |
		telemetry::protocol::CargoScanStatePresenceFlagSubsystem |
		telemetry::protocol::CargoScanStatePresenceFlagTiming |
		telemetry::protocol::CargoScanStatePresenceFlagValidity |
		telemetry::protocol::CargoScanStatePresenceFlagCargoText;
	source.cargo_source.phase = CargoScanPhaseObservation::Completed;
	source.cargo_source.target_capture_key.value = 1U;
	source.cargo_source.target_subsystem_source_key.value = 92U;
	source.cargo_source.elapsed_us = 93U;
	source.cargo_source.required_us = 94U;
	source.cargo_source.validity_flags =
		telemetry::protocol::ScanValidityFlagInRange |
		telemetry::protocol::ScanValidityFlagInAngle |
		telemetry::protocol::ScanValidityFlagLineOfSight;
	ASSERT_TRUE(source.cargo_source.cargo_text.assign("medical supplies"));

	auto observation_storage = std::make_unique<Phase2ObservationDto>();

	auto& observation = *observation_storage;
	constexpr std::uint64_t SampleTime = 777U;
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(source, SampleTime, observation).status);
	EXPECT_EQ(SampleTime, observation.player_controls.sample_time_us);
	EXPECT_EQ(source.control_source.presence, observation.player_controls.presence);
	EXPECT_EQ(PlayerControlModeObservation::Camera, observation.player_controls.mode);
	EXPECT_EQ(0.0F, observation.player_controls.pitch);
	EXPECT_FALSE(std::signbit(observation.player_controls.pitch));
	EXPECT_EQ(0.25F, observation.player_controls.heading);
	EXPECT_EQ(-0.5F, observation.player_controls.bank);
	EXPECT_EQ(1.0F, observation.player_controls.forward);
	EXPECT_EQ(-0.75F, observation.player_controls.sideways);
	EXPECT_EQ(0.0F, observation.player_controls.vertical);
	EXPECT_FALSE(std::signbit(observation.player_controls.vertical));
	EXPECT_EQ(source.control_source.action_flags, observation.player_controls.action_flags);
	EXPECT_EQ(42.0F, observation.player_controls.forward_cruise_percent);
	EXPECT_EQ(1U, observation.player_controls.fire_primary_count);
	EXPECT_EQ(2U, observation.player_controls.fire_secondary_count);
	EXPECT_EQ(3U, observation.player_controls.fire_countermeasure_count);

	EXPECT_EQ(SampleTime, observation.player_cargo_scan.sample_time_us);
	EXPECT_EQ(source.cargo_source.presence, observation.player_cargo_scan.presence);
	EXPECT_EQ(CargoScanPhaseObservation::Completed, observation.player_cargo_scan.phase);
	EXPECT_EQ(1U, observation.player_cargo_scan.target_capture_key.value);
	EXPECT_EQ(
		92U, observation.player_cargo_scan.target_subsystem_source_key.value);
	EXPECT_EQ(93U, observation.player_cargo_scan.elapsed_us);
	EXPECT_EQ(94U, observation.player_cargo_scan.required_us);
	EXPECT_EQ(source.cargo_source.validity_flags,
		observation.player_cargo_scan.validity_flags);
	EXPECT_EQ("medical supplies", observation.player_cargo_scan.cargo_text);
	EXPECT_EQ(1U, source.control_read_calls);
	EXPECT_EQ(1U, source.cargo_read_calls);
	ASSERT_TRUE(source.cargo_source.cargo_text.assign("mutated after capture"));
	EXPECT_EQ("medical supplies", observation.player_cargo_scan.cargo_text);
}

TEST(TelemetryPhase2ObservationContract, PlayerControlsAndCargoEnumsMasksAndValuesFailClosed)
{
	auto capture = [](const PlayerControlObservation& controls,
					   const PlayerCargoScanObservation& cargo) {
		FakePhase2EngineReadView source;
		source.control_source = controls;
		source.cargo_source = cargo;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		return collect_fake_phase2_observation(source, 100U, observation).status;
	};

	PlayerControlObservation controls;
	PlayerCargoScanObservation cargo;
	controls.mode = PlayerControlModeObservation::Count;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls, cargo));
	controls.mode = PlayerControlModeObservation::Ship;
	controls.presence = telemetry::protocol::KnownControlStatePresenceFlags | (1ULL << 63U);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls, cargo));
	controls.presence = 0U;
	controls.action_flags = telemetry::protocol::KnownControlFlags | 0x80000000U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls, cargo));
	controls.action_flags = 0U;
	controls.pitch = 1.001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls, cargo));
	controls.pitch = std::numeric_limits<float>::quiet_NaN();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls, cargo));

	controls = {};
	cargo.phase = CargoScanPhaseObservation::Count;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls, cargo));
	cargo.phase = CargoScanPhaseObservation::Idle;
	cargo.presence =
		telemetry::protocol::KnownCargoScanStatePresenceFlags | (1ULL << 63U);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls, cargo));
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagValidity;
	cargo.validity_flags = telemetry::protocol::KnownScanValidityFlags | 0x80U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls, cargo));
}

TEST(TelemetryPhase2ObservationContract, EveryControlAxisModeAndOptionalGroupBoundaryIsClosed)
{
	auto capture = [](const PlayerControlObservation& controls) {
		FakePhase2EngineReadView source;
		source.control_source = controls;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		return collect_fake_phase2_observation(source, 100U, observation).status;
	};
	constexpr std::array<float PlayerControlObservation::*, 6U> axes{{
		&PlayerControlObservation::pitch,
		&PlayerControlObservation::heading,
		&PlayerControlObservation::bank,
		&PlayerControlObservation::forward,
		&PlayerControlObservation::sideways,
		&PlayerControlObservation::vertical,
	}};
	for (const auto axis : axes) {
		for (const auto value : {-1.0F, 1.0F}) {
			PlayerControlObservation controls;
			controls.*axis = value;
			EXPECT_EQ(Phase2CaptureStatus::Valid, capture(controls));
		}
		for (const auto value : {-1.001F, 1.001F,
				 std::numeric_limits<float>::quiet_NaN(),
				 std::numeric_limits<float>::infinity()}) {
			PlayerControlObservation controls;
			controls.*axis = value;
			EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
		}
	}
	for (std::uint8_t value = 0U;
		 value < static_cast<std::uint8_t>(PlayerControlModeObservation::Count);
		 ++value) {
		PlayerControlObservation controls;
		controls.mode = static_cast<PlayerControlModeObservation>(value);
		EXPECT_EQ(Phase2CaptureStatus::Valid, capture(controls));
	}
	PlayerControlObservation controls;
	controls.mode = static_cast<PlayerControlModeObservation>(0xffU);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));

	controls = {};
	controls.forward_cruise_percent = 1.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
	controls.presence = telemetry::protocol::ControlStatePresenceFlagCruise;
	for (const auto value : {-100.0F, 100.0F}) {
		controls.forward_cruise_percent = value;
		EXPECT_EQ(Phase2CaptureStatus::Valid, capture(controls));
	}
	controls.forward_cruise_percent = 100.001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));

	controls = {};
	controls.fire_primary_count = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(controls));
	controls.presence = telemetry::protocol::ControlStatePresenceFlagRequestCounters;
	controls.fire_primary_count = std::numeric_limits<std::uint16_t>::max();
	controls.fire_secondary_count = std::numeric_limits<std::uint16_t>::max();
	controls.fire_countermeasure_count = std::numeric_limits<std::uint16_t>::max();
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(controls));
}

TEST(TelemetryPhase2ObservationContract, CargoPresenceAndPhaseRelationsRejectEveryMismatch)
{
	auto capture = [](const PlayerCargoScanObservation& cargo) {
		FakePhase2EngineReadView source;
		source.cargo_source = cargo;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		return collect_fake_phase2_observation(source, 100U, observation).status;
	};
	PlayerCargoScanObservation cargo;
	cargo.target_capture_key.value = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));

	cargo = {};
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagTarget;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));

	cargo = {};
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagSubsystem;
	cargo.target_subsystem_source_key.value = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));

	cargo = {};
	cargo.target_subsystem_source_key.value = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));

	cargo = {};
	cargo.elapsed_us = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo = {};
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagTiming;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo.required_us = 10U;
	cargo.elapsed_us = 11U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));

	cargo = {};
	cargo.validity_flags = telemetry::protocol::ScanValidityFlagInRange;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo = {};
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagCargoText;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo = {};
	ASSERT_TRUE(cargo.cargo_text.assign("undeclared"));
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));

	cargo = {};
	cargo.phase = CargoScanPhaseObservation::Scanning;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagTarget |
		telemetry::protocol::CargoScanStatePresenceFlagTiming |
		telemetry::protocol::CargoScanStatePresenceFlagValidity;
	cargo.target_capture_key.value = 1U;
	cargo.required_us = 10U;
	cargo.elapsed_us = 9U;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(cargo));
	cargo.elapsed_us = 10U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));

	cargo = {};
	cargo.phase = CargoScanPhaseObservation::Completed;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagTarget;
	cargo.target_capture_key.value = 1U;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(cargo));

	cargo = {};
	cargo.phase = CargoScanPhaseObservation::NotScannable;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(cargo));
	cargo.presence = telemetry::protocol::CargoScanStatePresenceFlagTarget;
	cargo.target_capture_key.value = 1U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
	cargo.presence |= telemetry::protocol::CargoScanStatePresenceFlagTiming;
	cargo.required_us = 10U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(cargo));
}

TEST(TelemetryPhase2ObservationContract, CargoTextRejectsInvalidUtf8BeforeDisclosure)
{
	FakePhase2EngineReadView source;
	source.cargo_source.phase = CargoScanPhaseObservation::Completed;
	source.cargo_source.presence =
		telemetry::protocol::CargoScanStatePresenceFlagTarget |
		telemetry::protocol::CargoScanStatePresenceFlagCargoText;
	source.cargo_source.target_capture_key.value = 1U;
	const char invalid_utf8[] = {static_cast<char>(0xc3), static_cast<char>(0x28), '\0'};
	ASSERT_TRUE(source.cargo_source.cargo_text.assign(
		std::string_view{invalid_utf8, sizeof(invalid_utf8) - 1U}));

	auto observation_storage = std::make_unique<Phase2ObservationDto>();

	auto& observation = *observation_storage;
	const auto result = collect_fake_phase2_observation(source, 100U, observation);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_TRUE(observation.player_cargo_scan.cargo_text.empty());
}

TEST(TelemetryPhase2ObservationContract, RootReadsOccurExactlyOnceAndFailuresAreAtomic)
{
	FakePhase2EngineReadView source;
	source.ship_count = 3U;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(source, 100U, observation).status);
	EXPECT_EQ(3U, source.read_calls);
	EXPECT_EQ(1U, source.control_read_calls);
	EXPECT_EQ(1U, source.cargo_read_calls);

	source = {};
	source.controls_read_succeeds = false;
	auto result = collect_fake_phase2_observation(source, 101U, observation);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(1U, source.read_calls);
	EXPECT_EQ(1U, source.control_read_calls);
	EXPECT_EQ(0U, source.cargo_read_calls);

	source = {};
	source.cargo_read_succeeds = false;
	result = collect_fake_phase2_observation(source, 102U, observation);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(1U, source.read_calls);
	EXPECT_EQ(1U, source.control_read_calls);
	EXPECT_EQ(1U, source.cargo_read_calls);

	source = {};
	source.failing_read_index = 0U;
	result = collect_fake_phase2_observation(source, 103U, observation);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource, result.status);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(0U, source.read_calls);
	EXPECT_EQ(0U, source.control_read_calls);
	EXPECT_EQ(0U, source.cargo_read_calls);
}

TEST(TelemetryPhase2ObservationContract, CargoHandoffIsIdenticalWithTestSeamOffAndOnAndResetIsMissionScoped)
{
	CargoAuthorityFact fact;
	fact.player_signature = 11U;
	fact.target_signature = 12U;
	fact.target_subsystem_source_key = 13U;
	fact.presence = telemetry::protocol::CargoScanStatePresenceFlagTarget |
		telemetry::protocol::CargoScanStatePresenceFlagTiming;
	fact.phase = CargoScanPhaseObservation::Scanning;
	fact.elapsed_us = 14U;
	fact.required_us = 15U;
	fact.validity_flags = telemetry::protocol::ScanValidityFlagInRange;
	ASSERT_TRUE(fact.cargo_text.assign("hidden cargo"));

	reset_phase2_mission_observation_state();
	telemetry::test_seam::set_phase2_seam_test_double(nullptr);
	telemetry::OnCargoAuthority(fact);
	const auto detached = phase2_seam_handoff_snapshot();

	RecordingPhase2SeamDouble recorder;
	reset_phase2_mission_observation_state();
	telemetry::test_seam::set_phase2_seam_test_double(&recorder);
	telemetry::OnCargoAuthority(fact);
	telemetry::OnCargoAuthority(fact);
	const auto attached = phase2_seam_handoff_snapshot();
	telemetry::test_seam::set_phase2_seam_test_double(nullptr);

	ASSERT_TRUE(detached.has_cargo_authority);
	ASSERT_TRUE(attached.has_cargo_authority);
	EXPECT_EQ(2U, recorder.cargo_calls);
	EXPECT_EQ(detached.cargo_authority.player_signature,
		attached.cargo_authority.player_signature);
	EXPECT_EQ(detached.cargo_authority.target_signature,
		attached.cargo_authority.target_signature);
	EXPECT_EQ(detached.cargo_authority.elapsed_us, attached.cargo_authority.elapsed_us);
	EXPECT_EQ(14U, attached.cargo_authority.elapsed_us);
	reset_phase2_mission_observation_state();
	const auto reset = phase2_seam_handoff_snapshot();
	EXPECT_FALSE(reset.has_ship_cleanup);
	EXPECT_FALSE(reset.has_support_transition);
	EXPECT_TRUE(reset.has_control_target);
	EXPECT_EQ(ControlTargetAuthority::Ship, reset.control_target);
	EXPECT_FALSE(reset.has_cargo_authority);
	reset_phase2_mission_observation_state();
	EXPECT_FALSE(phase2_seam_handoff_snapshot().has_cargo_authority);
}

TEST(TelemetryPhase2ObservationContract, InvalidShipBlockFloatFailsAtomically)
{
	FakePhase2EngineReadView source;
	source.block_source->flight.radius = std::numeric_limits<float>::quiet_NaN();
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	const auto result = collect_fake_phase2_observation(source, 100U, observation);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(0U, observation.player_key.value);
	EXPECT_EQ(0U, observation.producer_sample_time_us);
}

TEST(TelemetryPhase2ObservationContract, HullAndGuardianBoundariesRejectWithoutPartialObservation)
{
	auto capture = [](const ShipDamageObservation& damage) {
		FakePhase2EngineReadView source;
		source.block_source->damage = damage;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		const auto result = collect_fake_phase2_observation(source, 100U, observation);
		EXPECT_TRUE(result.status == Phase2CaptureStatus::Valid ||
			result.status == Phase2CaptureStatus::UnsupportedEngineState);
		if (result.status != Phase2CaptureStatus::Valid) {
			EXPECT_TRUE(observation.ships.empty());
			EXPECT_EQ(0U, observation.player_key.value);
			EXPECT_EQ(0U, observation.producer_sample_time_us);
		}
		return result.status;
	};

	ShipDamageObservation damage;
	damage.hull_maximum = 0.0F;
	damage.hull_current = 0.0F;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(damage));

	damage.hull_maximum = -1.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(damage));
	damage.hull_maximum = std::numeric_limits<float>::infinity();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(damage));
	damage.hull_maximum = std::numeric_limits<float>::quiet_NaN();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(damage));

	damage = {};
	damage.hull_maximum = 100.0F;
	damage.hull_current = 100.001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(damage));
	damage.hull_current = 50.0F;
	damage.guardian_threshold = 100.001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(damage));
	damage.guardian_threshold = std::numeric_limits<float>::infinity();
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, capture(damage));
}

TEST(TelemetryPhase2ObservationContract, ShieldCardinalityAndPhysicalMaximumBoundariesAreClosed)
{
	auto capture = [](const ShipShieldObservation& shields, Phase2ObservationDto& observation) {
		FakePhase2EngineReadView source;
		source.block_source->shields = shields;
		return collect_fake_phase2_observation(source, 100U, observation);
	};

	auto observation_storage = std::make_unique<Phase2ObservationDto>();

	auto& observation = *observation_storage;
	ShipShieldObservation shields;
	EXPECT_EQ(Phase2CaptureStatus::Valid, capture(shields, observation).status);
	ASSERT_EQ(1U, observation.ships.size());
	EXPECT_FALSE(observation.ships.front().shields.has_shields);
	EXPECT_EQ(0U, observation.ships.front().shields.segment_count);

	shields.has_shields = true;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture(shields, observation).status);
	EXPECT_TRUE(observation.ships.empty());

	for (const auto count : {1U, 64U}) {
		shields = {};
		shields.has_shields = true;
		shields.segment_count = static_cast<std::uint8_t>(count);
		for (std::size_t index = 0U; index < count; ++index) {
			shields.segment_current_hits[index] = 1.0F;
			shields.segment_maximum_hits[index] = 2.0F;
		}
		shields.recharge_maximum = static_cast<float>(count * 2U);
		SCOPED_TRACE(count);
		EXPECT_EQ(Phase2CaptureStatus::Valid, capture(shields, observation).status);
		ASSERT_EQ(1U, observation.ships.size());
		EXPECT_EQ(count, observation.ships.front().shields.segment_count);
	}

	shields.segment_count = 65U;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture(shields, observation).status);
	EXPECT_TRUE(observation.ships.empty());

	shields = {};
	shields.has_shields = true;
	shields.segment_count = 1U;
	shields.segment_current_hits[0] = 10.001F;
	shields.segment_maximum_hits[0] = 10.0F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture(shields, observation).status);

	shields.segment_current_hits[0] = 10.0F;
	shields.recharge_maximum = 10.001F;
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture(shields, observation).status);
	EXPECT_TRUE(observation.ships.empty());
}

TEST(TelemetryPhase2ObservationContract, QuaternionMustBeFiniteUnitAndCanonicalSign)
{
	auto capture = [](const std::array<float, 4U>& orientation) {
		FakePhase2EngineReadView source;
		source.block_source->flight.orientation_local_to_world = orientation;
		auto observation_storage = std::make_unique<Phase2ObservationDto>();
		auto& observation = *observation_storage;
		return collect_fake_phase2_observation(source, 100U, observation).status;
	};

	EXPECT_EQ(Phase2CaptureStatus::Valid,
		capture({{1.0F, 0.0F, 0.0F, 0.0F}}));
	EXPECT_EQ(Phase2CaptureStatus::Valid,
		capture({{0.0F, 1.0F, 0.0F, 0.0F}}));
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture({{-1.0F, 0.0F, 0.0F, 0.0F}}));
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture({{0.0F, -1.0F, 0.0F, 0.0F}}));
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture({{0.5F, 0.0F, 0.0F, 0.0F}}));
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture({{1.0F, std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F}}));
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		capture({{1.0F, std::numeric_limits<float>::infinity(), 0.0F, 0.0F}}));
}

TEST(TelemetryPhase2ObservationContract, BufferTransitionsToFailedClosedOnUnsupportedEngineState)
{
	auto buffer_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& buffer = *buffer_storage;
	ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
	ASSERT_TRUE(buffer.enter_ready());
	FakePhase2EngineReadView source;
	source.block_source->flight.radius = std::numeric_limits<float>::quiet_NaN();
	const auto result = buffer.capture(source, 100U);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
	EXPECT_EQ(Phase2ObservationBufferState::FailedClosed, buffer.state());
	EXPECT_TRUE(buffer.observation().ships.empty());
	EXPECT_EQ(0U, buffer.observation().producer_sample_time_us);
}

TEST(TelemetryPhase2ObservationContract, MainThreadAndPlayerPresenceGuardsFailBeforeReadingShips)
{
	FakePhase2EngineReadView source;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	source.main_thread = false;
	auto result = collect_fake_phase2_observation(source, 100U, observation);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource, result.status);
	EXPECT_EQ(Phase2CaptureReason::WrongThread, result.reason);
	EXPECT_EQ(0U, source.read_calls);
	EXPECT_TRUE(observation.ships.empty());

	source.main_thread = true;
	source.player = false;
	source.player_object = false;
	source.player_ship = false;
	result = collect_fake_phase2_observation(source, 100U, observation);
	EXPECT_EQ(Phase2CaptureStatus::NoPlayer, result.status);
	EXPECT_EQ(0U, source.read_calls);

	source.player_object = true;
	result = collect_fake_phase2_observation(source, 100U, observation);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource, result.status);
	EXPECT_EQ(Phase2CaptureReason::PartialPlayerSource, result.reason);
	EXPECT_EQ(0U, source.read_calls);
}

TEST(TelemetryPhase2ObservationContract, InvalidSourceAndClosureCapacityAreFailClosed)
{
	FakePhase2EngineReadView source;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	source.consistent = false;
	auto result = collect_fake_phase2_observation(source, 100U, observation);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource, result.status);
	EXPECT_EQ(0U, source.read_calls);
	EXPECT_TRUE(observation.ships.empty());

	source.consistent = true;
	source.ship_count = 64U;
	result = collect_fake_phase2_observation(source, 100U, observation);
	ASSERT_EQ(Phase2CaptureStatus::Valid, result.status);
	EXPECT_EQ(64U, observation.ships.size());

	source.ship_count = 65U;
	result = collect_fake_phase2_observation(source, 100U, observation);
	EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded, result.status);
	EXPECT_TRUE(observation.ships.empty());
}

TEST(TelemetryPhase2ObservationContract, FloatAndTimerHelpersAreFiniteBoundedAndCanonical)
{
	float canonical = 1.0F;
	ASSERT_TRUE(canonicalize_phase2_float(-0.0F, 100.0F, canonical));
	EXPECT_EQ(0.0F, canonical);
	EXPECT_FALSE(std::signbit(canonical));
	EXPECT_FALSE(canonicalize_phase2_float(
		std::numeric_limits<float>::infinity(), 100.0F, canonical));
	EXPECT_FALSE(canonicalize_phase2_float(
		std::numeric_limits<float>::quiet_NaN(), 100.0F, canonical));
	EXPECT_FALSE(canonicalize_phase2_float(101.0F, 100.0F, canonical));

	std::uint64_t remaining_us = 99U;
	ASSERT_TRUE(phase2_timestamp_remaining_us(100, 90, 60'000'000U, remaining_us));
	EXPECT_EQ(0U, remaining_us);
	ASSERT_TRUE(phase2_timestamp_remaining_us(100, 110, 60'000'000U, remaining_us));
	EXPECT_EQ(10'000U, remaining_us);
	EXPECT_FALSE(phase2_timestamp_remaining_us(100, 60'101, 60'000'000U, remaining_us));
}

TEST(TelemetryPhase2ObservationContract, TimerHandlesNegativeDomainsAndOverflowWithoutWrapping)
{
	std::uint64_t remaining_us = 99U;
	ASSERT_TRUE(phase2_timestamp_remaining_us(-10, -20, 60'000'000U, remaining_us));
	EXPECT_EQ(0U, remaining_us);
	ASSERT_TRUE(phase2_timestamp_remaining_us(-10, 10, 60'000'000U, remaining_us));
	EXPECT_EQ(20'000U, remaining_us);
	ASSERT_TRUE(phase2_timestamp_remaining_us(
		-60'000, 0, 60'000'000U, remaining_us));
	EXPECT_EQ(60'000'000U, remaining_us);

	remaining_us = 99U;
	EXPECT_FALSE(phase2_timestamp_remaining_us(std::numeric_limits<std::int64_t>::min(),
		std::numeric_limits<std::int64_t>::max(),
		std::numeric_limits<std::uint64_t>::max(),
		remaining_us));
	EXPECT_EQ(0U, remaining_us);
}

TEST(TelemetryPhase2ObservationContract, DuplicateSignaturesStringLimitAndReadFailureFailAtomically)
{
	FakePhase2EngineReadView source;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	source.ship_count = 2U;
	source.duplicate_signatures = true;
	auto result = collect_fake_phase2_observation(source, 100U, observation);
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(0U, observation.player_key.value);
	EXPECT_EQ(0U, observation.producer_sample_time_us);

	source.duplicate_signatures = false;
	source.internal_name.assign(MaximumPhase2InternalNameBytes, 'a');
	result = collect_fake_phase2_observation(source, 101U, observation);
	ASSERT_EQ(Phase2CaptureStatus::Valid, result.status);
	ASSERT_EQ(2U, observation.ships.size());
	EXPECT_EQ(MaximumPhase2InternalNameBytes, observation.ships.front().identity.internal_name.size());

	source.internal_name.push_back('b');
	result = collect_fake_phase2_observation(source, 102U, observation);
	EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded, result.status);
	EXPECT_EQ(Phase2CaptureReason::StringLimitExceeded, result.reason);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(0U, observation.player_key.value);
	EXPECT_EQ(0U, observation.producer_sample_time_us);

	source.internal_name = "valid";
	source.failing_read_index = 1U;
	result = collect_fake_phase2_observation(source, 103U, observation);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource, result.status);
	EXPECT_EQ(Phase2CaptureReason::ShipReadFailure, result.reason);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(0U, observation.player_key.value);
	EXPECT_EQ(0U, observation.producer_sample_time_us);
}

TEST(TelemetryPhase2ObservationContract, ReadyBufferCaptureDoesNotAllocateAfterProvisioning)
{
	FakePhase2EngineReadView source;
	initialize_s8_ship_blocks(*source.block_source, true);
	source.internal_name.assign(128U, 'a');
	auto buffer_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& buffer = *buffer_storage;
	ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
	ASSERT_TRUE(buffer.enter_ready());
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		buffer.capture(source, 100U).status);

	std::size_t steady_state_allocations = 0U;
	{
		AllocationWindow allocations;
		const auto result = buffer.capture(source, 101U);
		steady_state_allocations = allocations.count();
		if (result.status != Phase2CaptureStatus::Valid) {
			steady_state_allocations = std::numeric_limits<std::size_t>::max();
		}
	}
	EXPECT_EQ(0U, steady_state_allocations);
	EXPECT_EQ(1U, buffer.observation().ships.size());
}

TEST(TelemetryPhase2ObservationContract,
	CleanupSupportControlAndCargoSeamsAreOffOnOffAndPreserveFacts)
{
	RecordingPhase2SeamDouble recorder;
	telemetry::test_seam::set_phase2_seam_test_double(nullptr);
	std::size_t detached_allocations = 0U;
	{
		AllocationWindow allocations;
		telemetry::OnShipCleanup(90U, ShipCleanupMode::Departed);
		telemetry::OnSupportTransition(91U, 92U, 93U, SupportTransitionReason::Abort, 94U);
		telemetry::OnControlTarget(ControlTargetAuthority::Ship);
		telemetry::OnCargoAuthority(CargoAuthorityFact{95U, 96U, 97U});
		detached_allocations = allocations.count();
	}
	EXPECT_EQ(0U, detached_allocations);
	EXPECT_EQ(0U, recorder.cleanup_calls);
	EXPECT_EQ(0U, recorder.support_calls);
	EXPECT_EQ(0U, recorder.control_calls);
	EXPECT_EQ(0U, recorder.cargo_calls);

	telemetry::test_seam::set_phase2_seam_test_double(&recorder);
	std::size_t attached_allocations = 0U;
	{
		AllocationWindow allocations;
		telemetry::OnShipCleanup(1U, ShipCleanupMode::Destroyed);
		telemetry::OnSupportTransition(1U, 2U, 3U, SupportTransitionReason::Complete, 4U);
		telemetry::OnControlTarget(ControlTargetAuthority::Camera);
		telemetry::OnCargoAuthority(CargoAuthorityFact{5U, 6U, 7U});
		attached_allocations = allocations.count();
	}
	telemetry::test_seam::set_phase2_seam_test_double(nullptr);
	telemetry::OnShipCleanup(100U, ShipCleanupMode::Vanished);

	EXPECT_EQ(0U, attached_allocations);
	EXPECT_EQ(1U, recorder.cleanup_calls);
	EXPECT_EQ(1U, recorder.support_calls);
	EXPECT_EQ(1U, recorder.control_calls);
	EXPECT_EQ(1U, recorder.cargo_calls);
	EXPECT_EQ(0U, recorder.socket_calls);
	EXPECT_EQ(1U, recorder.last_cleanup.object_signature);
	EXPECT_EQ(ShipCleanupMode::Destroyed, recorder.last_cleanup.mode);
	EXPECT_EQ(1U, recorder.last_support.assisted_signature);
	EXPECT_EQ(2U, recorder.last_support.support_signature);
	EXPECT_EQ(3U, recorder.last_support.episode_sequence);
	EXPECT_EQ(SupportTransitionReason::Complete, recorder.last_support.reason);
	EXPECT_EQ(4U, recorder.last_support.sample_time);
	EXPECT_EQ(ControlTargetAuthority::Camera, recorder.last_control);
	EXPECT_EQ(5U, recorder.last_cargo.player_signature);
	EXPECT_EQ(6U, recorder.last_cargo.target_signature);
	EXPECT_EQ(7U, recorder.last_cargo.elapsed_us);
}

TEST(TelemetryPhase2ObservationContract, AbsentAndDisabledConfigurationOwnNoPhase2Buffers)
{
	for (const auto mode :
		{Phase2ProvisioningMode::ConfigurationAbsent, Phase2ProvisioningMode::ValidDisabled}) {
		auto buffer_storage = std::make_unique<Phase2ObservationBuffer>();
		auto& buffer = *buffer_storage;
		std::size_t allocations_made = 0U;
		bool configured = false;
		{
			AllocationWindow allocations;
			configured = buffer.provision(mode);
			allocations_made = allocations.count();
		}

		EXPECT_TRUE(configured);
		EXPECT_EQ(Phase2ObservationBufferState::Disabled, buffer.state());
		EXPECT_EQ(0U, buffer.ship_capacity());
		EXPECT_EQ(0U, buffer.owned_bytes());
		EXPECT_FALSE(buffer.enter_ready());
		EXPECT_EQ(0U, allocations_made);
	}
}

TEST(TelemetryPhase2ObservationContract, ReviewerS8V4ActualOwnedBudgetFitsShared64MiB)
{
	constexpr std::size_t SharedStartupBudgetBytes = 64U * 1024U * 1024U;

	for (const auto mode : {Phase2ProvisioningMode::ConfigurationAbsent,
			 Phase2ProvisioningMode::ValidDisabled}) {
		auto inactive_storage = std::make_unique<Phase2ObservationBuffer>();
		auto& inactive = *inactive_storage;
		ASSERT_TRUE(inactive.provision(mode));
		EXPECT_EQ(0U, inactive.owned_bytes());
		EXPECT_EQ(0U, inactive.ship_capacity());
	}

	auto enabled_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& enabled = *enabled_storage;
	ASSERT_TRUE(enabled.provision(Phase2ProvisioningMode::ValidEnabled));
	const auto measured_owned_bytes = enabled.owned_bytes();
	std::cout << "[ PHASE2 OWNED BYTES ] " << measured_owned_bytes << '\n';
	EXPECT_EQ(sizeof(Phase2ObservationBuffer) +
			enabled.ship_capacity() * sizeof(ShipObservationDto) +
			sizeof(Phase2ShipSource),
		measured_owned_bytes);
	EXPECT_LE(measured_owned_bytes, SharedStartupBudgetBytes) <<
		"Phase 2 maximum owned storage must share the frozen 64 MiB startup cap.";
}

TEST(TelemetryPhase2ObservationContract, ReviewerS8V4DiscoveryFailureFailsEveryProjectionAtomically)
{
	FakePhase2EngineReadView source;
	source.discovery_status = Phase2SourceReadStatus::UnsupportedEngineState;

	auto core_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& core = *core_storage;
	ASSERT_TRUE(core.provision(Phase2ProvisioningMode::ValidEnabled));
	ASSERT_TRUE(core.enter_ready());
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		core.capture(source, 100U, Phase2ObservationProjection::CoreGate).status);
	EXPECT_EQ(1U, source.discovery_read_calls);
	EXPECT_EQ(Phase2ObservationBufferState::FailedClosed, core.state());

	auto extension_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& extension = *extension_storage;
	ASSERT_TRUE(extension.provision(Phase2ProvisioningMode::ValidEnabled));
	ASSERT_TRUE(extension.enter_ready());
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		extension.capture(
			source, 101U, Phase2ObservationProjection::DiscoveryExtension).status);
	EXPECT_EQ(Phase2SourceReadStatus::Valid,
		extension.observation().discovery_status);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource,
		extension.observation().discovery_capture.status);
	EXPECT_EQ(Phase2ObservationBufferState::FailedClosed, extension.state());

	auto complete_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& complete = *complete_storage;
	ASSERT_TRUE(complete.provision(Phase2ProvisioningMode::ValidEnabled));
	ASSERT_TRUE(complete.enter_ready());
	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		complete.capture(
			source, 102U, Phase2ObservationProjection::CompleteShip).status);
	EXPECT_EQ(Phase2ObservationBufferState::FailedClosed, complete.state());
}

TEST(TelemetryPhase2ObservationContract, EnabledProvisioningReservesExactMaximumBeforeReady)
{
	auto buffer_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& buffer = *buffer_storage;
	ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
	EXPECT_EQ(Phase2ObservationBufferState::Provisioned, buffer.state());
	EXPECT_EQ(MaximumPhase2ObservationShips, buffer.ship_capacity());
	EXPECT_GT(buffer.owned_bytes(), 0U);
	ASSERT_TRUE(buffer.enter_ready());
	EXPECT_EQ(Phase2ObservationBufferState::Ready, buffer.state());
	EXPECT_FALSE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
}

TEST(TelemetryPhase2ObservationContract, ReadyBufferIgnoresGlobalPopulationAndRejectsSelectedBlockOverflowWithoutGrowth)
{
	auto buffer_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& buffer = *buffer_storage;
	ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
	ASSERT_TRUE(buffer.enter_ready());
	const auto provisioned_capacity = buffer.ship_capacity();
	const auto provisioned_bytes = buffer.owned_bytes();

	FakePhase2EngineReadView source;
	source.global_ship_count = MaximumPhase2ObservationShips + 1U;
	std::size_t exact_cap_allocations = 0U;
	Phase2CaptureResult result;
	{
		AllocationWindow allocations;
		result = buffer.capture(source, 200U);
		exact_cap_allocations = allocations.count();
	}
	ASSERT_EQ(Phase2CaptureStatus::Valid, result.status);
	EXPECT_EQ(1U, buffer.observation().ships.size());
	EXPECT_EQ(0U, exact_cap_allocations);

	source.block_source->weapons.primary_bank_count =
		static_cast<std::uint8_t>(MaximumPhase2WeaponBanksPerFamily + 1U);
	std::size_t plus_one_allocations = 0U;
	{
		AllocationWindow allocations;
		result = buffer.capture(source, 201U);
		plus_one_allocations = allocations.count();
	}
	EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded, result.status);
	EXPECT_TRUE(buffer.observation().ships.empty());
	EXPECT_EQ(Phase2ObservationBufferState::FailedClosed, buffer.state());
	EXPECT_EQ(provisioned_capacity, buffer.ship_capacity());
	EXPECT_EQ(provisioned_bytes, buffer.owned_bytes());
	EXPECT_EQ(0U, plus_one_allocations);
}

TEST(TelemetryPhase2ObservationContract, InvalidSeamEnumsAreIgnoredFailClosed)
{
	RecordingPhase2SeamDouble recorder;
	telemetry::test_seam::set_phase2_seam_test_double(&recorder);

	telemetry::OnShipCleanup(1U, ShipCleanupMode::Count);
	telemetry::OnShipCleanup(1U, static_cast<ShipCleanupMode>(0xffU));
	telemetry::OnSupportTransition(1U, 2U, 3U, SupportTransitionReason::Count, 4U);
	telemetry::OnSupportTransition(
		1U, 2U, 3U, static_cast<SupportTransitionReason>(0xffU), 4U);
	telemetry::OnControlTarget(ControlTargetAuthority::Count);
	telemetry::OnControlTarget(static_cast<ControlTargetAuthority>(0xffU));

	telemetry::test_seam::set_phase2_seam_test_double(nullptr);
	EXPECT_EQ(0U, recorder.cleanup_calls);
	EXPECT_EQ(0U, recorder.support_calls);
	EXPECT_EQ(0U, recorder.control_calls);
	EXPECT_EQ(0U, recorder.cargo_calls);
}

TEST(TelemetryPhase2ObservationContract, EveryClosedSeamEnumCopiesExactFactsWithoutAllocation)
{
	RecordingPhase2SeamDouble recorder;
	telemetry::test_seam::set_phase2_seam_test_double(&recorder);

	std::size_t allocations_made = 0U;
	{
		AllocationWindow allocations;
		for (std::uint8_t value = 0U; value < static_cast<std::uint8_t>(ShipCleanupMode::Count);
			 ++value) {
			telemetry::OnShipCleanup(10U + value, static_cast<ShipCleanupMode>(value));
		}
		for (std::uint8_t value = 0U;
			 value < static_cast<std::uint8_t>(SupportTransitionReason::Count);
			 ++value) {
			telemetry::OnSupportTransition(
				20U + value, 30U + value, 40U + value, static_cast<SupportTransitionReason>(value), 50U + value);
		}
		for (std::uint8_t value = 0U;
			 value < static_cast<std::uint8_t>(ControlTargetAuthority::Count);
			 ++value) {
			telemetry::OnControlTarget(static_cast<ControlTargetAuthority>(value));
		}
		telemetry::OnCargoAuthority(CargoAuthorityFact{
			std::numeric_limits<std::uint32_t>::max(),
			std::numeric_limits<std::uint32_t>::max() - 1U,
			std::numeric_limits<std::uint64_t>::max()});
		allocations_made = allocations.count();
	}
	telemetry::test_seam::set_phase2_seam_test_double(nullptr);

	EXPECT_EQ(0U, allocations_made);
	EXPECT_EQ(static_cast<std::size_t>(ShipCleanupMode::Count), recorder.cleanup_calls);
	EXPECT_EQ(static_cast<std::size_t>(SupportTransitionReason::Count), recorder.support_calls);
	EXPECT_EQ(static_cast<std::size_t>(ControlTargetAuthority::Count), recorder.control_calls);
	EXPECT_EQ(1U, recorder.cargo_calls);
	EXPECT_EQ(12U, recorder.last_cleanup.object_signature);
	EXPECT_EQ(ShipCleanupMode::Vanished, recorder.last_cleanup.mode);
	EXPECT_EQ(27U, recorder.last_support.assisted_signature);
	EXPECT_EQ(37U, recorder.last_support.support_signature);
	EXPECT_EQ(47U, recorder.last_support.episode_sequence);
	EXPECT_EQ(SupportTransitionReason::Complete, recorder.last_support.reason);
	EXPECT_EQ(57U, recorder.last_support.sample_time);
	EXPECT_EQ(ControlTargetAuthority::Camera, recorder.last_control);
	EXPECT_EQ(std::numeric_limits<std::uint32_t>::max(), recorder.last_cargo.player_signature);
	EXPECT_EQ(std::numeric_limits<std::uint32_t>::max() - 1U, recorder.last_cargo.target_signature);
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), recorder.last_cargo.elapsed_us);
}

TEST(TelemetryPhase2ObservationContract, KeyframeCaptureHasOneRootSampleTimeForEveryOwnedShipBlock)
{
	FakePhase2EngineReadView source;
	source.ship_count = MaximumPhase2ObservationShips;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	constexpr std::uint64_t KeyframeSampleTime = 0x0102030405060708ULL;
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(source, KeyframeSampleTime, observation).status);
	EXPECT_EQ(KeyframeSampleTime, observation.producer_sample_time_us);
	EXPECT_EQ(MaximumPhase2ObservationShips, observation.ships.size());
	EXPECT_EQ(MaximumPhase2ObservationShips, source.read_calls);
	EXPECT_EQ(KeyframeSampleTime, observation.player_controls.sample_time_us);
	EXPECT_EQ(KeyframeSampleTime, observation.player_cargo_scan.sample_time_us);
	for (const auto& ship : observation.ships) {
		EXPECT_EQ(KeyframeSampleTime, ship.identity.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.lifecycle.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.flight.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.damage.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.shields.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.energy.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.propulsion.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.weapons.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.support.sample_time_us);
		EXPECT_EQ(KeyframeSampleTime, ship.docking.sample_time_us);
		for (std::size_t index = 0U; index < ship.subsystems.count; ++index) {
			EXPECT_EQ(KeyframeSampleTime, ship.subsystems.values[index].sample_time_us);
		}
	}
}

TEST(TelemetryPhase2ObservationContract, FailedCaptureDeterministicallyResetsEveryOwnedRootAndShipBlock)
{
	FakePhase2EngineReadView source;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(source, 100U, observation).status);
	ASSERT_EQ(1U, observation.ships.size());
	observation.ships[0].flight.radius = 77.0F;
	observation.ships[0].damage.hull_current = 66.0F;
	observation.ships[0].shields.has_shields = true;
	observation.ships[0].subsystems.count = 1U;
	observation.ships[0].subsystems.values[0].sample_time_us = 55U;
	observation.player_controls.pitch = 44.0F;
	observation.player_cargo_scan.elapsed_us = 33U;

	source.main_thread = false;
	const auto result = collect_fake_phase2_observation(source, 200U, observation);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource, result.status);
	EXPECT_EQ(Phase2CaptureReason::WrongThread, result.reason);
	EXPECT_EQ(0U, observation.player_key.value);
	EXPECT_EQ(0U, observation.producer_sample_time_us);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(0U, observation.player_controls.sample_time_us);
	EXPECT_EQ(0.0F, observation.player_controls.pitch);
	EXPECT_EQ(0U, observation.player_cargo_scan.sample_time_us);
	EXPECT_EQ(0U, observation.player_cargo_scan.elapsed_us);
}

TEST(TelemetryPhase2ObservationContract, ProductionOwnedSeamHandoffCopiesLatestScalarsAndResetsDeterministically)
{
	telemetry::test_seam::set_phase2_seam_test_double(nullptr);
	reset_phase2_seam_handoff();

	std::size_t allocations_made = 0U;
	Phase2SeamHandoffSnapshot snapshot;
	{
		AllocationWindow allocations;
		telemetry::OnShipCleanup(11U, ShipCleanupMode::Departed);
		telemetry::OnShipCleanup(12U, ShipCleanupMode::Destroyed);
		telemetry::OnSupportTransition(
			21U, 22U, 23U, SupportTransitionReason::Broken, 24U);
		telemetry::OnControlTarget(ControlTargetAuthority::Camera);
		telemetry::OnCargoAuthority(CargoAuthorityFact{31U, 32U, 33U});
		snapshot = phase2_seam_handoff_snapshot();
		allocations_made = allocations.count();
	}

	EXPECT_EQ(0U, allocations_made);
	ASSERT_TRUE(snapshot.has_ship_cleanup);
	EXPECT_EQ(12U, snapshot.ship_cleanup.object_signature);
	EXPECT_EQ(ShipCleanupMode::Destroyed, snapshot.ship_cleanup.mode);
	ASSERT_TRUE(snapshot.has_support_transition);
	EXPECT_EQ(21U, snapshot.support_transition.assisted_signature);
	EXPECT_EQ(22U, snapshot.support_transition.support_signature);
	EXPECT_EQ(23U, snapshot.support_transition.episode_sequence);
	EXPECT_EQ(SupportTransitionReason::Broken, snapshot.support_transition.reason);
	EXPECT_EQ(24U, snapshot.support_transition.sample_time);
	ASSERT_TRUE(snapshot.has_control_target);
	EXPECT_EQ(ControlTargetAuthority::Camera, snapshot.control_target);
	ASSERT_TRUE(snapshot.has_cargo_authority);
	EXPECT_EQ(31U, snapshot.cargo_authority.player_signature);
	EXPECT_EQ(32U, snapshot.cargo_authority.target_signature);
	EXPECT_EQ(33U, snapshot.cargo_authority.elapsed_us);

	reset_phase2_seam_handoff();
	snapshot = phase2_seam_handoff_snapshot();
	EXPECT_FALSE(snapshot.has_ship_cleanup);
	EXPECT_FALSE(snapshot.has_support_transition);
	EXPECT_FALSE(snapshot.has_control_target);
	EXPECT_FALSE(snapshot.has_cargo_authority);
}

TEST(TelemetryPhase2ObservationContract, InvalidSeamEnumsLeaveProductionHandoffBitForBitUnchanged)
{
	telemetry::test_seam::set_phase2_seam_test_double(nullptr);
	reset_phase2_seam_handoff();
	telemetry::OnShipCleanup(11U, ShipCleanupMode::Departed);
	telemetry::OnSupportTransition(
		21U, 22U, 23U, SupportTransitionReason::Broken, 24U);
	telemetry::OnControlTarget(ControlTargetAuthority::Camera);
	telemetry::OnCargoAuthority(CargoAuthorityFact{31U, 32U, 33U});
	const auto before = phase2_seam_handoff_snapshot();

	telemetry::OnShipCleanup(101U, ShipCleanupMode::Count);
	telemetry::OnShipCleanup(102U, static_cast<ShipCleanupMode>(0xffU));
	telemetry::OnSupportTransition(
		103U, 104U, 105U, SupportTransitionReason::Count, 106U);
	telemetry::OnSupportTransition(
		107U, 108U, 109U, static_cast<SupportTransitionReason>(0xffU), 110U);
	telemetry::OnControlTarget(ControlTargetAuthority::Count);
	telemetry::OnControlTarget(static_cast<ControlTargetAuthority>(0xffU));
	const auto after = phase2_seam_handoff_snapshot();

	EXPECT_EQ(before.has_ship_cleanup, after.has_ship_cleanup);
	EXPECT_EQ(before.ship_cleanup.object_signature, after.ship_cleanup.object_signature);
	EXPECT_EQ(before.ship_cleanup.mode, after.ship_cleanup.mode);
	EXPECT_EQ(before.has_support_transition, after.has_support_transition);
	EXPECT_EQ(before.support_transition.assisted_signature,
		after.support_transition.assisted_signature);
	EXPECT_EQ(before.support_transition.support_signature,
		after.support_transition.support_signature);
	EXPECT_EQ(before.support_transition.episode_sequence,
		after.support_transition.episode_sequence);
	EXPECT_EQ(before.support_transition.reason, after.support_transition.reason);
	EXPECT_EQ(before.support_transition.sample_time, after.support_transition.sample_time);
	EXPECT_EQ(before.has_control_target, after.has_control_target);
	EXPECT_EQ(before.control_target, after.control_target);
	EXPECT_EQ(before.has_cargo_authority, after.has_cargo_authority);
	EXPECT_EQ(before.cargo_authority.player_signature,
		after.cargo_authority.player_signature);
	EXPECT_EQ(before.cargo_authority.target_signature,
		after.cargo_authority.target_signature);
	EXPECT_EQ(before.cargo_authority.elapsed_us, after.cargo_authority.elapsed_us);
}

TEST(TelemetryPhase2ObservationContract,
	ReviewerS8V5DiscoveryMultiKeyFailureIsAtomicBeforeShipReads)
{
	FakePhase2EngineReadView source;
	source.ship_count = 2U;
	source.use_discovery_status_by_key = true;
	source.discovery_status_by_key.fill(Phase2SourceReadStatus::Valid);
	source.discovery_status_by_key[0] = Phase2SourceReadStatus::InvalidSource;

	Phase2ObservationSelection selection;
	selection.count = 2U;
	selection.ship_keys[0].value = 1U;
	selection.ship_keys[1].value = 2U;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;

	ASSERT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		collect_phase2_observation(source, selection, 501U, observation).status);
	EXPECT_EQ(1U, source.discovery_read_calls)
		<< "Discovery fails fast before resolving or reading later selections.";
	EXPECT_EQ(0U, observation.discovery_count);
	EXPECT_TRUE(observation.ships.empty());
	EXPECT_EQ(Phase2SourceReadStatus::Valid, observation.discovery_status);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource,
		observation.discovery_capture.status);
}

TEST(TelemetryPhase2ObservationContract,
	ReviewerS8V5CompleteShipMultiKeyFailsFromOwnedDiscoveryAggregate)
{
	FakePhase2EngineReadView source;
	source.ship_count = 2U;
	source.use_discovery_status_by_key = true;
	source.discovery_status_by_key.fill(Phase2SourceReadStatus::Valid);
	source.discovery_status_by_key[0] = Phase2SourceReadStatus::InvalidSource;
	Phase2ObservationSelection selection;
	selection.count = 2U;
	selection.ship_keys[0].value = 1U;
	selection.ship_keys[1].value = 2U;
	auto observation_storage = std::make_unique<Phase2ObservationDto>();
	auto& observation = *observation_storage;

	EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState,
		collect_phase2_observation(source,
			selection,
			502U,
			observation,
			Phase2ObservationProjection::CompleteShip).status);
	EXPECT_EQ(Phase2SourceReadStatus::Valid, observation.discovery_status);
	EXPECT_EQ(Phase2CaptureStatus::InvalidSource,
		observation.discovery_capture.status);
}

TEST(TelemetryPhase2ObservationContract,
	ReviewerS8V5OwnedBytesIncludesInlineObjectAndEachOwnedPayloadExactlyOnce)
{
	auto buffer_storage = std::make_unique<Phase2ObservationBuffer>();
	auto& buffer = *buffer_storage;
	ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
	const auto expected =
		sizeof(Phase2ObservationBuffer) +
		buffer.ship_capacity() * sizeof(ShipObservationDto) +
		sizeof(Phase2ShipSource);
	EXPECT_EQ(expected, buffer.owned_bytes())
		<< "owned_bytes is inclusive: inline buffer/DTO plus vector payload and scratch exactly once.";
	EXPECT_LE(buffer.owned_bytes(), MaximumPhase2OwnedBytes);
}

TEST(TelemetryPhase2ObservationContract,
	ReviewerUltimateSupportRawHostileRelationsFailAtomically)
{
	enum class Hostile {
		CountermeasureCurrentAboveMaximum,
		CountermeasurePoolBelowMinusOne,
		MissionDisallowButEnergyApplicable,
		WeaponDisallowButAmmoApplicable,
		CountermeasureApplicableWithZeroWork,
		HullApplicableWithoutAuthorization,
		AmmunitionApplicableWithZeroWork,
		CountermeasureCapacityMismatch,
		InvalidRawFraction,
		InvalidRawRate,
	};
	for (const auto hostile : {
			 Hostile::CountermeasureCurrentAboveMaximum,
			 Hostile::CountermeasurePoolBelowMinusOne,
			 Hostile::MissionDisallowButEnergyApplicable,
			 Hostile::WeaponDisallowButAmmoApplicable,
			 Hostile::CountermeasureApplicableWithZeroWork,
			 Hostile::HullApplicableWithoutAuthorization,
			 Hostile::AmmunitionApplicableWithZeroWork,
			 Hostile::CountermeasureCapacityMismatch,
			 Hostile::InvalidRawFraction,
			 Hostile::InvalidRawRate}) {
		FakePhase2EngineReadView source;
		auto& weapons = source.block_source->weapons;
		auto& support = source.block_source->support;
		switch (hostile) {
		case Hostile::CountermeasureCurrentAboveMaximum:
			weapons.countermeasure_count = 5U;
			weapons.countermeasure_maximum = 4U;
			support.raw_countermeasure_capacity = 4U;
			break;
		case Hostile::CountermeasurePoolBelowMinusOne:
			support.raw_countermeasure_rearm_pool = -2;
			break;
		case Hostile::MissionDisallowButEnergyApplicable:
			support.raw_mission_rearm_disallowed = true;
			support.raw_weapon_energy_rearm_applicable = true;
			support.raw_weapon_energy_rearm_work = 1.0F;
			break;
		case Hostile::WeaponDisallowButAmmoApplicable:
			support.raw_weapon_rearm_disallowed = true;
			support.raw_ammunition_rearm_applicable = true;
			support.raw_ammunition_rearm_work = 1U;
			break;
		case Hostile::CountermeasureApplicableWithZeroWork:
			support.raw_countermeasure_rearm_applicable = true;
			support.raw_countermeasure_capacity = 4U;
			break;
		case Hostile::HullApplicableWithoutAuthorization:
			support.raw_hull_repair_applicable = true;
			support.raw_hull_repair_work = 1.0F;
			support.raw_hull_repair_rate = 1.0F;
			break;
		case Hostile::AmmunitionApplicableWithZeroWork:
			support.raw_ammunition_rearm_applicable = true;
			break;
		case Hostile::CountermeasureCapacityMismatch:
			weapons.countermeasure_maximum = 4U;
			support.raw_countermeasure_capacity = 5U;
			break;
		case Hostile::InvalidRawFraction:
			support.raw_max_hull_repair_fraction = 1.01F;
			break;
		case Hostile::InvalidRawRate:
			support.raw_hull_repair_rate =
				std::numeric_limits<float>::quiet_NaN();
			break;
		}

		auto observation_storage = std::make_unique<Phase2ObservationDto>();

		auto& observation = *observation_storage;
		observation.producer_sample_time_us = 999U;
		observation.ships.emplace_back();
		const auto result = collect_phase2_observation(source, 601U, observation);
		SCOPED_TRACE(static_cast<int>(hostile));
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
		EXPECT_TRUE(observation.ships.empty());
		EXPECT_EQ(0U, observation.producer_sample_time_us);
		EXPECT_EQ(0U, observation.discovery_count);
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedTypedRawCatalogContractIsExhaustiveAndPreId)
{
	EXPECT_TRUE(has_wp02_catalog_shape<Phase2ObservationDto>::value)
		<< "Missing owned raw_static_catalog and bounded class/weapon/auxiliary "
		   "collections.";
	EXPECT_TRUE(has_wp02_complete_class_facts<Phase2ObservationDto>::value)
		<< "Missing complete raw class motion, physics, afterburner, scan, "
		   "glide, autoaim, or countermeasure authorities.";
	EXPECT_TRUE(
		has_wp02_complete_nested_class_facts<Phase2ObservationDto>::value)
		<< "Missing complete raw subsystem/bank geometry, ownership, capacity, "
		   "pattern, armor, type, or static-flag authorities.";
	EXPECT_TRUE(has_wp02_complete_weapon_facts<Phase2ObservationDto>::value)
		<< "Missing complete raw weapon mass, motion, damage/effect, guidance, "
		   "lock, cargo/rearm, burst, swarm, or shots authorities.";
	EXPECT_TRUE(
		has_wp02_capture_local_auxiliary_facts<Phase2ObservationDto>::value)
		<< "Missing owned capture-local auxiliary keys/names/pattern values.";
	EXPECT_TRUE(
		has_wp02_per_ship_loadout_references<Phase2ObservationDto>::value)
		<< "Missing raw per-ship/loadout class, weapon, and auxiliary capture "
		   "references.";
	EXPECT_FALSE(has_wp02_species_source_index<Phase2ObservationDto>::value);
	EXPECT_FALSE(has_wp02_ship_type_source_index<Phase2ObservationDto>::value);
	EXPECT_FALSE(has_wp02_damage_type_source_index<Phase2ObservationDto>::value)
		<< "Every engine registry index must be resolved independently.";
	EXPECT_FALSE(
		has_wp02_support_object_signature<Phase2ObservationDto>::value);
	EXPECT_FALSE(has_wp02_support_ai_index<Phase2ObservationDto>::value);
	EXPECT_FALSE(has_wp02_support_ship_objnum<Phase2ObservationDto>::value);
	EXPECT_FALSE(
		has_wp02_support_ship_signature<Phase2ObservationDto>::value);
	EXPECT_FALSE(
		has_wp02_docking_remote_signature<Phase2ObservationDto>::value);
	EXPECT_FALSE(
		has_wp02_cargo_target_signature<Phase2ObservationDto>::value);
	EXPECT_FALSE(
		has_wp02_ship_source_object_index<Phase2ObservationDto>::value);
	EXPECT_FALSE(
		has_wp02_ship_source_object_signature<Phase2ObservationDto>::value)
		<< "Nested/public capture authorities must use opaque capture-local "
		   "keys, never engine signatures, indices, ai_index, or objnum.";
	EXPECT_FALSE(
		has_wp02_forbidden_referenced_aggregates<Phase2ObservationDto>::value)
		<< "WP02 keeps raw references in each ship/loadout; it must not build "
		   "WP03 referenced_* aggregate lists.";
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReviewB1EveryPublishedSourceKeyIsOpaqueAndCaptureLocal)
{
	using CargoSubsystemKey = std::remove_reference_t<decltype(
		std::declval<PlayerCargoScanObservation&>()
			.target_subsystem_source_key)>;
	using ClassKey = std::remove_reference_t<decltype(
		std::declval<ShipIdentityObservation&>().class_source_key)>;
	using DamageArmorKey = std::remove_reference_t<decltype(
		std::declval<ShipDamageObservation&>().armor_source_key)>;
	using SubsystemKey = std::remove_reference_t<decltype(
		std::declval<ShipSubsystemObservation&>().source_key)>;
	using ArmorKey = std::remove_reference_t<decltype(
		std::declval<ShipSubsystemObservation&>().armor_source_key)>;
	using WeaponKey = std::remove_reference_t<decltype(
		std::declval<ShipWeaponBankObservation&>()
			.weapon_class_source_key)>;
	using CountermeasureKey = std::remove_reference_t<decltype(
		std::declval<ShipWeaponsObservation&>()
			.countermeasure_class_source_key)>;
	using TurretPrimaryKey = std::remove_reference_t<decltype(
		std::declval<ShipTurretObservation&>()
			.turret_primary_bank_weapon_source_keys[0])>;
	using TurretSecondaryKey = std::remove_reference_t<decltype(
		std::declval<ShipTurretObservation&>()
			.turret_secondary_bank_weapon_source_keys[0])>;

	EXPECT_TRUE(is_wp02_opaque_capture_local_key<CargoSubsystemKey>::value);
	EXPECT_TRUE(is_wp02_opaque_capture_local_key<ClassKey>::value);
	EXPECT_TRUE(is_wp02_opaque_capture_local_key<DamageArmorKey>::value);
	EXPECT_TRUE(is_wp02_opaque_capture_local_key<SubsystemKey>::value);
	EXPECT_TRUE(is_wp02_opaque_capture_local_key<ArmorKey>::value);
	EXPECT_TRUE(is_wp02_opaque_capture_local_key<WeaponKey>::value);
	EXPECT_TRUE(is_wp02_opaque_capture_local_key<CountermeasureKey>::value);
	EXPECT_TRUE(is_wp02_opaque_capture_local_key<TurretPrimaryKey>::value);
	EXPECT_TRUE(is_wp02_opaque_capture_local_key<TurretSecondaryKey>::value)
		<< "Cargo subsystem, subsystem/armor, loadout and turret references "
		   "must be canonical opaque capture-local keys, never raw engine "
		   "indices or signatures.";
}

template <typename Observation>
void exercise_wp02_owned_bounded_storage_types()
{
	if constexpr (
		has_wp02_catalog_shape<Observation>::value &&
		has_wp02_complete_class_facts<Observation>::value &&
		has_wp02_complete_nested_class_facts<Observation>::value &&
		has_wp02_complete_weapon_facts<Observation>::value &&
		has_wp02_capture_local_auxiliary_facts<Observation>::value &&
		has_wp02_per_ship_loadout_references<Observation>::value) {
		using Catalog = std::remove_reference_t<decltype(
			std::declval<Observation&>().raw_static_catalog)>;
		using ClassDefinitions = std::remove_reference_t<decltype(
			std::declval<Catalog&>().class_definitions)>;
		using WeaponDefinitions = std::remove_reference_t<decltype(
			std::declval<Catalog&>().weapon_definitions)>;
		using AuxiliaryEntries = std::remove_reference_t<decltype(
			std::declval<Catalog&>().auxiliary_entries)>;
		using ClassDefinition = std::remove_reference_t<decltype(
			std::declval<ClassDefinitions&>()[0])>;
		using ClassName = std::remove_reference_t<decltype(
			std::declval<ClassDefinition&>().internal_name)>;
		using SubsystemName = std::remove_reference_t<decltype(
			std::declval<Catalog&>().subsystem_storage[0].internal_name)>;
		using WeaponName = std::remove_reference_t<decltype(
			std::declval<WeaponDefinitions&>()[0].internal_name)>;
		using AuxiliaryName = std::remove_reference_t<decltype(
			std::declval<AuxiliaryEntries&>()[0].name)>;
		using ClassCaptureKey = std::remove_reference_t<decltype(
			std::declval<ClassDefinition&>().class_capture_key)>;

		static_assert(!std::is_pointer_v<ClassDefinitions>);
		static_assert(!std::is_pointer_v<WeaponDefinitions>);
		static_assert(!std::is_pointer_v<AuxiliaryEntries>);
		static_assert(!std::is_pointer_v<ClassDefinition>);
		static_assert(!std::is_pointer_v<ClassName>);
		static_assert(!std::is_pointer_v<SubsystemName>);
		static_assert(!std::is_pointer_v<WeaponName>);
		static_assert(!std::is_pointer_v<AuxiliaryName>);
		static_assert(!std::is_same_v<ClassName, std::string_view>);
		static_assert(!std::is_same_v<SubsystemName, std::string_view>);
		static_assert(!std::is_same_v<WeaponName, std::string_view>);
		static_assert(!std::is_same_v<AuxiliaryName, std::string_view>);
		static_assert(!has_forbidden_object_index<ClassCaptureKey>::value);

		auto observation = std::make_unique<Observation>();
		EXPECT_TRUE(observation->raw_static_catalog.class_definitions[0]
				.internal_name.view()
				.empty());
		EXPECT_TRUE(observation->raw_static_catalog.weapon_definitions[0]
				.internal_name.view()
				.empty());
		EXPECT_TRUE(observation->raw_static_catalog.auxiliary_entries[0]
				.name.view()
				.empty());
	} else {
		ADD_FAILURE()
			<< "Owned bounded DTO types cannot be checked until the complete "
			   "raw catalog and per-ship reference API exists.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedCatalogUsesOwnedBoundedTypesWithoutPointersOrViews)
{
	exercise_wp02_owned_bounded_storage_types<Phase2ObservationDto>();
	EXPECT_FALSE(has_wp02_subsystem_definition_pointer_view<
		Phase2ObservationDto>::value)
		<< "Subsystem definitions must never contain a pointer-backed view.";
	EXPECT_FALSE(has_wp02_bank_definition_pointer_view<
		Phase2ObservationDto>::value)
		<< "Bank definitions must never contain a pointer-backed view.";
	EXPECT_TRUE(
		has_wp02_flat_owned_nested_layout<Phase2ObservationDto>::value)
		<< "Catalog must own flat subsystem/bank stores and classes must expose "
		   "bounded offsets/counts or an equivalent typed offset accessor.";
	EXPECT_FALSE(has_wp02_node_engine_key<Phase2DiscoveryNode>::value);
	EXPECT_FALSE(has_wp02_node_dock_leader_key<Phase2DiscoveryNode>::value);
	EXPECT_FALSE(has_wp02_node_leader_index<Phase2DiscoveryNode>::value);
	EXPECT_FALSE(has_wp02_node_leader_signature<Phase2DiscoveryNode>::value);
	EXPECT_FALSE(has_wp02_node_support_index<Phase2DiscoveryNode>::value);
	EXPECT_FALSE(has_wp02_node_support_signature<Phase2DiscoveryNode>::value);
	EXPECT_FALSE(has_wp02_node_docking_signatures<Phase2DiscoveryNode>::value);
	EXPECT_FALSE(has_wp02_player_object_index<Phase2ObservationDto>::value)
		<< "Observation root must use an opaque capture-local key.";
	EXPECT_FALSE(has_wp02_ship_object_index<Phase2ObservationDto>::value)
		<< "Published ships must use opaque capture-local keys.";
}

template <typename View>
void exercise_wp02_real_fso_static_extractor()
{
	if constexpr (has_wp02_fso_static_extractor_seam<View>::value) {
		using Arguments = wp02_member_function_arguments<
			decltype(&View::extract_static_authorities_for_test)>;
		using Input = typename Arguments::input_type;
		using Output = typename Arguments::output_type;
		if constexpr (
			has_wp02_fso_extractor_fixture_authorities<Input>::value &&
			has_wp02_catalog_shape<Output>::value &&
			has_wp02_complete_class_facts<Output>::value &&
			has_wp02_complete_nested_class_facts<Output>::value &&
			has_wp02_complete_weapon_facts<Output>::value &&
			has_wp02_capture_local_auxiliary_facts<Output>::value) {
			auto input = std::make_unique<Input>();
			input->guards_valid = true;
			input->ship_info.effective_mass = 101.0F;
			input->ship_info.max_rear_velocity = 102.0F;
			input->weapon_info.mass = 103.0F;
			input->weapon_info.damage = 104.0F;
			input->model.center_of_mass = {105.0F, 106.0F, 107.0F};
			input->subsystem_count = 1U;
			input->subsystems[0].subsystem_capture_key = 121U;
			ASSERT_TRUE(input->subsystems[0].internal_name.assign("reactor"));
			ASSERT_TRUE(input->subsystems[0].alt_name.assign("core"));
			ASSERT_TRUE(input->subsystems[0].hud_name.assign("Reactor"));
			input->subsystems[0].local_position =
				{108.0F, 109.0F, 110.0F};
			input->subsystems[0].radius = 116.0F;
			input->subsystems[0].max_hits = 117.0F;
			input->subsystems[0].subsystem_type_source = 3U;
			input->subsystems[0].raw_static_flags = 0x1234U;
			input->subsystems[0].armor_capture_key = 118U;
			input->bank_count = 1U;
			input->banks[0].bank_capture_key = 131U;
			input->banks[0].owner_subsystem_capture_key = 121U;
			input->banks[0].family_source = 4U;
			input->banks[0].source_family = 2U;
			input->banks[0].bank_index = 7U;
			input->banks[0].weapon_capture_key = 1U;
			input->banks[0].consumes_ammunition = true;
			input->banks[0].has_capacity = true;
			input->banks[0].capacity = 132.0F;
			input->banks[0].firing_pattern_source_code = 5U;
			input->banks[0].fire_point_count = 1U;
			input->banks[0].fire_points[0] =
				{111.0F, 112.0F, 113.0F};
			input->registries.species.capture_key = 114U;
			input->registries.species.firing_pattern_source_code = 6U;
			ASSERT_TRUE(
				input->registries.species.name.assign("oracle-species"));
			input->registries.weapon_damage_type.capture_key = 115U;
			input->registries.weapon_damage_type.firing_pattern_source_code =
				7U;
			ASSERT_TRUE(input->registries.weapon_damage_type.name.assign(
				"oracle-damage"));

			View view;
			auto output = std::make_unique<Output>();
			const auto result =
				view.extract_static_authorities_for_test(*input, *output);
			ASSERT_EQ(Phase2SourceReadStatus::Valid, result.status);
			ASSERT_EQ(1U, output->raw_static_catalog.class_count);
			ASSERT_EQ(1U, output->raw_static_catalog.weapon_count);
			const auto& ship_class =
				output->raw_static_catalog.class_definitions[0];
			EXPECT_FLOAT_EQ(101.0F, ship_class.effective_mass);
			EXPECT_FLOAT_EQ(102.0F, ship_class.max_rear_velocity);
			expect_raw_vec3_equal(
				input->model.center_of_mass, ship_class.center_of_mass);
			ASSERT_EQ(1U, ship_class.subsystem_count);
			const auto& subsystem =
				output->raw_static_catalog.subsystem_storage[
					ship_class.subsystem_offset];
			EXPECT_EQ(input->subsystems[0].subsystem_capture_key,
				subsystem.subsystem_capture_key);
			EXPECT_EQ(input->subsystems[0].internal_name.view(),
				subsystem.internal_name.view());
			EXPECT_EQ(input->subsystems[0].alt_name.view(),
				subsystem.alt_name.view());
			EXPECT_EQ(input->subsystems[0].hud_name.view(),
				subsystem.hud_name.view());
			expect_raw_vec3_equal(
				input->subsystems[0].local_position, subsystem.local_position);
			EXPECT_FLOAT_EQ(input->subsystems[0].radius, subsystem.radius);
			EXPECT_FLOAT_EQ(input->subsystems[0].max_hits, subsystem.max_hits);
			EXPECT_EQ(input->subsystems[0].subsystem_type_source,
				subsystem.subsystem_type_source);
			EXPECT_EQ(input->subsystems[0].raw_static_flags,
				subsystem.raw_static_flags);
			EXPECT_EQ(input->subsystems[0].armor_capture_key,
				subsystem.armor_capture_key);
			ASSERT_EQ(1U, ship_class.bank_count);
			const auto& bank = output->raw_static_catalog.bank_storage[
				ship_class.bank_offset];
			EXPECT_EQ(input->banks[0].bank_capture_key,
				bank.bank_capture_key);
			EXPECT_EQ(input->banks[0].owner_subsystem_capture_key,
				bank.owner_subsystem_capture_key);
			EXPECT_EQ(input->banks[0].family_source, bank.family_source);
			EXPECT_EQ(input->banks[0].source_family, bank.source_family);
			EXPECT_EQ(input->banks[0].bank_index, bank.bank_index);
			EXPECT_EQ(input->banks[0].weapon_capture_key,
				bank.weapon_capture_key);
			EXPECT_EQ(input->banks[0].consumes_ammunition,
				bank.consumes_ammunition);
			EXPECT_EQ(input->banks[0].has_capacity, bank.has_capacity);
			EXPECT_FLOAT_EQ(input->banks[0].capacity, bank.capacity);
			EXPECT_EQ(input->banks[0].firing_pattern_source_code,
				bank.firing_pattern_source_code);
			EXPECT_EQ(input->banks[0].fire_point_count,
				bank.fire_point_count);
			expect_raw_vec3_equal(
				input->banks[0].fire_points[0], bank.fire_points[0]);
			EXPECT_FLOAT_EQ(103.0F,
				output->raw_static_catalog.weapon_definitions[0].mass);
			EXPECT_FLOAT_EQ(104.0F,
				output->raw_static_catalog.weapon_definitions[0].damage);
			EXPECT_EQ(114U, ship_class.species_capture_key);
			EXPECT_EQ(115U,
				output->raw_static_catalog.weapon_definitions[0]
					.damage_type_capture_key);
			ASSERT_EQ(2U, output->raw_static_catalog.auxiliary_count);
			EXPECT_EQ(Phase2RawAuxiliaryRegistry::Species,
				output->raw_static_catalog.auxiliary_entries[0].registry);
			EXPECT_EQ(input->registries.species.name.view(),
				output->raw_static_catalog.auxiliary_entries[0].name.view());
			EXPECT_EQ(input->registries.species.firing_pattern_source_code,
				output->raw_static_catalog.auxiliary_entries[0]
					.firing_pattern_source_code);
			EXPECT_EQ(Phase2RawAuxiliaryRegistry::DamageType,
				output->raw_static_catalog.auxiliary_entries[1].registry);
			EXPECT_EQ(input->registries.weapon_damage_type.name.view(),
				output->raw_static_catalog.auxiliary_entries[1].name.view());
			EXPECT_EQ(
				input->registries.weapon_damage_type
					.firing_pattern_source_code,
				output->raw_static_catalog.auxiliary_entries[1]
					.firing_pattern_source_code);

			input->guards_valid = false;
			output->raw_static_catalog.class_count = 9U;
			const auto refused =
				view.extract_static_authorities_for_test(*input, *output);
			EXPECT_NE(Phase2SourceReadStatus::Valid, refused.status);
			EXPECT_EQ(0U, output->raw_static_catalog.class_count);
		} else {
			ADD_FAILURE()
				<< "FsoEngineReadView test extractor exists but its typed "
				   "fixture/output omits guarded Ship_info, Weapon_info, model, "
				   "subsystem, bank, or registry authorities.";
		}
	} else {
		ADD_FAILURE()
			<< "Missing guarded typed "
			   "FsoEngineReadView::extract_static_authorities_for_test seam.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedRealFsoExtractorMapsEveryGuardedAuthority)
{
	exercise_wp02_real_fso_static_extractor<FsoEngineReadView>();
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedRequiresExplicitReciprocalTopologyFacts)
{
	EXPECT_TRUE(
		has_wp02_reciprocal_topology_authority<Phase2DiscoveryNode>::value)
		<< "Missing owned support_capture_key, group_leader_capture_key, "
		   "direct_docking_capture_keys, or reciprocal facts.";
}

template <typename Source,
	typename Observation = Phase2ObservationDto,
	typename Node = Phase2DiscoveryNode>
void exercise_wp02_reciprocal_topology(Source& source)
{
	if constexpr (
		has_wp02_reciprocal_topology_authority<Node>::value) {
		using CaptureKey = std::remove_reference_t<decltype(
			std::declval<Node&>().support_capture_key)>;
		static_assert(!std::is_pointer_v<CaptureKey>);
		static_assert(!std::is_arithmetic_v<CaptureKey>,
			"Published topology requires an opaque capture-local key.");
		static_assert(!std::is_same_v<CaptureKey, EngineEntityKey>,
			"Published topology must never retain EngineEntityKey.");
		static_assert(std::is_trivially_copyable_v<CaptureKey>);
		static_assert(!has_forbidden_object_index<CaptureKey>::value,
			"Published topology keys must not expose object_index.");
		source.ship_count = 2U;
		source.use_discovery_node_sources = true;
		auto& player = source.discovery_node_sources[0];
		player.capture_key = {1U};
		player.support_capture_key = {2U};
		player.group_leader_capture_key = {1U};
		player.direct_docking_count = 1U;
		player.direct_docking_capture_keys[0] = {2U};
		player.direct_docking_reciprocal[0] = true;
		player.direct_local_dockpoints[0] = 3U;
		player.direct_remote_dockpoints[0] = 7U;
		auto& support = source.discovery_node_sources[1];
		support.capture_key = {2U};
		support.group_leader_capture_key = {1U};
		support.direct_docking_count = 1U;
		support.direct_docking_capture_keys[0] = {1U};
		support.direct_docking_reciprocal[0] = true;
		support.direct_local_dockpoints[0] = 7U;
		support.direct_remote_dockpoints[0] = 3U;

		auto observation = std::make_unique<Observation>();
		const auto result = collect_phase2_observation(
			source, 699U, *observation,
			Phase2ObservationProjection::CompleteShip);
		ASSERT_EQ(Phase2CaptureStatus::Valid, result.status);
		ASSERT_EQ(2U, observation->discovery_count);
		EXPECT_EQ(2U,
			observation->discovery_nodes[0]
				.direct_docking_capture_keys[0]
				.value);
		EXPECT_TRUE(observation->discovery_nodes[0]
				.direct_docking_reciprocal[0]);

		source.discovery_node_sources[1].direct_remote_dockpoints[0] = 99U;
		const auto refused = collect_phase2_observation(
			source, 700U, *observation,
			Phase2ObservationProjection::CompleteShip);
		EXPECT_EQ(
			Phase2CaptureStatus::UnsupportedEngineState, refused.status);
		EXPECT_EQ(0U, observation->discovery_count);
		EXPECT_TRUE(observation->ships.empty());

		source.discovery_node_sources[1].direct_remote_dockpoints[0] = 3U;
		source.discovery_node_sources[0].capture_key = {};
		const auto missing_key = collect_phase2_observation(
			source, 701U, *observation,
			Phase2ObservationProjection::CompleteShip);
		EXPECT_EQ(
			Phase2CaptureStatus::UnsupportedEngineState, missing_key.status)
			<< "Capture must never invent fallback index+1 topology keys.";
		EXPECT_EQ(0U, observation->discovery_count);
	} else {
		ADD_FAILURE()
			<< "Typed reciprocal topology fields are absent; fake-source "
			   "runtime reciprocity contract cannot execute.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedReciprocityIsRuntimeValidatedAndAtomic)
{
	auto source = std::make_unique<FakePhase2EngineReadView>();
	exercise_wp02_reciprocal_topology(*source);
}

template <typename Source, typename Buffer>
void exercise_wp02_maximum_catalog_noalloc(Source& source, Buffer& buffer)
{
	using Observation = std::remove_reference_t<
		decltype(std::declval<Buffer&>().observation())>;
	if constexpr (
		has_wp02_catalog_shape<Observation>::value &&
		has_wp02_complete_class_facts<Observation>::value &&
		has_wp02_complete_nested_class_facts<Observation>::value &&
		has_wp02_complete_weapon_facts<Observation>::value &&
		has_wp02_capture_local_auxiliary_facts<Observation>::value &&
		has_wp02_per_ship_loadout_references<Observation>::value) {
		auto& catalog = source.block_source->raw_static_catalog;
		auto& references = source.block_source->raw_static_references;
		catalog.class_count = 64U;
		catalog.weapon_count = 4096U;
		catalog.auxiliary_count = 256U;
		catalog.aggregate_subsystem_count = 4096U;
		for (std::size_t class_index = 0U; class_index < 64U; ++class_index) {
			auto& ship_class = catalog.class_definitions[class_index];
			ship_class.class_capture_key =
				static_cast<std::uint32_t>(class_index + 1U);
			ASSERT_TRUE(ship_class.internal_name.assign(
				"owned-class-" + std::to_string(class_index)));
			const std::size_t subsystem_count = class_index == 0U ? 1024U :
				class_index == 1U ? 96U : 48U;
			const auto subsystem_base = class_index == 0U ? 0U :
				class_index == 1U ? 1024U :
				1120U + (class_index - 2U) * 48U;
			ship_class.subsystem_count =
				static_cast<std::uint32_t>(subsystem_count);
			ship_class.subsystem_offset =
				static_cast<std::uint32_t>(subsystem_base);
			ship_class.bank_count = class_index == 0U ? 192U : 0U;
			ship_class.bank_offset =
				static_cast<std::uint32_t>(class_index * 192U);
			ship_class.primary_bank_count = class_index == 0U ? 64U : 0U;
			ship_class.secondary_bank_count = class_index == 0U ? 64U : 0U;
			ship_class.tertiary_bank_count = class_index == 0U ? 64U : 0U;
			ship_class.turret_bank_count = 0U;
			for (std::size_t subsystem = 0U; subsystem < subsystem_count;
				 ++subsystem) {
				auto& definition = catalog.subsystem_storage[
					ship_class.subsystem_offset + subsystem];
				definition.subsystem_capture_key =
					static_cast<std::uint32_t>(
						subsystem_base + subsystem + 1U);
				ASSERT_TRUE(definition.internal_name.assign("owned-subsystem"));
			}
			for (std::size_t bank = 0U; bank < ship_class.bank_count; ++bank) {
				auto& definition =
					catalog.bank_storage[ship_class.bank_offset + bank];
				definition.bank_capture_key =
					static_cast<std::uint32_t>(
						class_index * 192U + bank + 1U);
				definition.weapon_capture_key =
					static_cast<std::uint32_t>(bank + 1U);
				definition.fire_point_count = 64U;
			}
		}
		for (std::size_t weapon = 0U; weapon < 4096U; ++weapon) {
			auto& definition = catalog.weapon_definitions[weapon];
			definition.weapon_capture_key =
				static_cast<std::uint32_t>(weapon + 1U);
			definition.reloaded_per_batch = 1U;
			ASSERT_TRUE(definition.internal_name.assign(
				"owned-weapon-" + std::to_string(weapon)));
			references.weapon_capture_keys[weapon] =
				static_cast<std::uint32_t>(weapon + 1U);
		}
		references.class_capture_key = 1U;
		references.weapon_count = 4096U;
		references.auxiliary_count = 256U;
		for (std::size_t auxiliary = 0U; auxiliary < 256U; ++auxiliary) {
			auto& entry = catalog.auxiliary_entries[auxiliary];
			entry.capture_key =
				static_cast<std::uint32_t>(auxiliary + 1U);
			ASSERT_TRUE(entry.name.assign(
				"owned-auxiliary-" + std::to_string(auxiliary)));
			references.auxiliary_capture_keys[auxiliary] =
				static_cast<std::uint32_t>(auxiliary + 1U);
		}

		ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
		ASSERT_TRUE(buffer.enter_ready());
		AllocationWindow allocation_window;
		const auto result = collect_phase2_observation(buffer, source, 700U,
			Phase2ObservationProjection::CompleteShip);
		EXPECT_EQ(Phase2CaptureStatus::Valid, result.status);
		EXPECT_EQ(0U, allocation_window.count());
		const auto& captured = buffer.observation().raw_static_catalog;
		EXPECT_EQ(64U, captured.class_count);
		EXPECT_EQ(4096U, captured.weapon_count);
		EXPECT_EQ(256U, captured.auxiliary_count);
		EXPECT_EQ(4096U, captured.aggregate_subsystem_count);
		EXPECT_EQ(1024U,
			captured.class_definitions[0].subsystem_count);
		ASSERT_EQ(1U, buffer.observation().ships.size());
		EXPECT_EQ(4096U,
			buffer.observation()
				.ships[0]
				.raw_static_references.weapon_count);

		// Exercise the fourth family independently while retaining the exact
		// aggregate limit of 192 banks for this class.
		auto& first_class = catalog.class_definitions[0];
		first_class.tertiary_bank_count = 0U;
		first_class.turret_bank_count = 64U;
		const auto turret_family_result =
			collect_phase2_observation(buffer, source, 701U,
				Phase2ObservationProjection::CompleteShip);
		EXPECT_EQ(Phase2CaptureStatus::Valid, turret_family_result.status);
		EXPECT_EQ(64U,
			buffer.observation()
				.raw_static_catalog.class_definitions[0]
				.turret_bank_count);
	} else {
		ADD_FAILURE()
			<< "Typed raw static catalog API is absent; maximum/noalloc "
			   "runtime contract cannot execute.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedMaximumCatalogIsBoundedOwnedAndAllocationFreeAfterReady)
{
	auto source = std::make_unique<FakePhase2EngineReadView>();
	auto buffer = std::make_unique<Phase2ObservationBuffer>();
	exercise_wp02_maximum_catalog_noalloc(*source, *buffer);
}

template <typename Source, typename Buffer>
void exercise_wp02_deep_copy_and_atomic_plus_one(Source& source, Buffer& buffer)
{
	using Observation = std::remove_reference_t<
		decltype(std::declval<Buffer&>().observation())>;
	if constexpr (
		has_wp02_catalog_shape<Observation>::value &&
		has_wp02_complete_class_facts<Observation>::value &&
		has_wp02_complete_nested_class_facts<Observation>::value &&
		has_wp02_complete_weapon_facts<Observation>::value &&
		has_wp02_capture_local_auxiliary_facts<Observation>::value &&
		has_wp02_per_ship_loadout_references<Observation>::value) {
		auto& source_catalog = source.block_source->raw_static_catalog;
		auto& references = source.block_source->raw_static_references;
		source_catalog.class_count = 1U;
		source_catalog.weapon_count = 1U;
		source_catalog.auxiliary_count = 1U;
		source_catalog.aggregate_subsystem_count = 1U;
		source_catalog.class_definitions[0].class_capture_key = 11U;
		ASSERT_TRUE(source_catalog.class_definitions[0].internal_name.assign(
			"deep-class"));
		auto& subsystem = source_catalog.subsystem_storage[0];
		source_catalog.class_definitions[0].subsystem_count = 1U;
		source_catalog.class_definitions[0].subsystem_offset = 0U;
		subsystem.subsystem_capture_key = 12U;
		ASSERT_TRUE(subsystem.internal_name.assign("deep-subsystem"));
		ASSERT_TRUE(subsystem.alt_name.assign("deep-alt"));
		ASSERT_TRUE(subsystem.hud_name.assign("deep-hud"));
		auto& bank = source_catalog.bank_storage[0];
		source_catalog.class_definitions[0].bank_count = 1U;
		source_catalog.class_definitions[0].bank_offset = 0U;
		source_catalog.class_definitions[0].primary_bank_count = 1U;
		bank.bank_capture_key = 13U;
		bank.weapon_capture_key = 21U;
		bank.fire_point_count = 1U;
		bank.fire_points[0] = {1.25F, 2.5F, 3.75F};
		source_catalog.weapon_definitions[0].weapon_capture_key = 21U;
		source_catalog.weapon_definitions[0].reloaded_per_batch = 1U;
		ASSERT_TRUE(source_catalog.weapon_definitions[0].internal_name.assign(
			"deep-weapon"));
		ASSERT_TRUE(
			source_catalog.weapon_definitions[0].title.assign("deep-title"));
		source_catalog.auxiliary_entries[0].capture_key = 31U;
		ASSERT_TRUE(
			source_catalog.auxiliary_entries[0].name.assign("deep-aux"));
		references.class_capture_key = 11U;
		references.weapon_count = 1U;
		references.weapon_capture_keys[0] = 21U;
		references.auxiliary_count = 1U;
		references.auxiliary_capture_keys[0] = 31U;

		ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
		ASSERT_TRUE(buffer.enter_ready());
		ASSERT_EQ(Phase2CaptureStatus::Valid,
			collect_phase2_observation(buffer, source, 701U,
				Phase2ObservationProjection::CompleteShip).status);
		ASSERT_TRUE(source_catalog.class_definitions[0].internal_name.assign(
			"mutated-after-capture"));
		ASSERT_TRUE(subsystem.internal_name.assign("mutated-subsystem"));
		ASSERT_TRUE(subsystem.alt_name.assign("mutated-alt"));
		ASSERT_TRUE(subsystem.hud_name.assign("mutated-hud"));
		bank.fire_points[0] = {9.0F, 8.0F, 7.0F};
		ASSERT_TRUE(source_catalog.weapon_definitions[0].internal_name.assign(
			"mutated-weapon"));
		ASSERT_TRUE(
			source_catalog.weapon_definitions[0].title.assign("mutated-title"));
		ASSERT_TRUE(source_catalog.auxiliary_entries[0].name.assign(
			"mutated-aux"));
		references.weapon_capture_keys[0] = 999U;
		references.auxiliary_capture_keys[0] = 998U;
		const auto& captured = buffer.observation();
		EXPECT_EQ("deep-class",
			captured.raw_static_catalog.class_definitions[0]
				.internal_name.view());
		EXPECT_EQ("deep-subsystem",
			captured.raw_static_catalog.subsystem_storage[0]
				.internal_name.view());
		EXPECT_EQ("deep-alt",
			captured.raw_static_catalog.subsystem_storage[0].alt_name.view());
		EXPECT_EQ("deep-hud",
			captured.raw_static_catalog.subsystem_storage[0].hud_name.view());
		EXPECT_FLOAT_EQ(1.25F,
			captured.raw_static_catalog.bank_storage[0].fire_points[0].x);
		EXPECT_FLOAT_EQ(2.5F,
			captured.raw_static_catalog.bank_storage[0].fire_points[0].y);
		EXPECT_FLOAT_EQ(3.75F,
			captured.raw_static_catalog.bank_storage[0].fire_points[0].z);
		EXPECT_EQ("deep-weapon",
			captured.raw_static_catalog.weapon_definitions[0]
				.internal_name.view());
		EXPECT_EQ("deep-title",
			captured.raw_static_catalog.weapon_definitions[0].title.view());
		EXPECT_EQ("deep-aux",
			captured.raw_static_catalog.auxiliary_entries[0].name.view());
		EXPECT_EQ(1U,
			captured.ships[0].raw_static_references.weapon_capture_keys[0]);
		EXPECT_EQ(1U,
			captured.ships[0].raw_static_references.auxiliary_capture_keys[0]);
		EXPECT_EQ("deep-class",
			buffer.observation()
				.raw_static_catalog.class_definitions[0]
				.internal_name.view());

		source_catalog.weapon_count = 4097U;
		const auto refused = collect_phase2_observation(buffer, source, 702U,
			Phase2ObservationProjection::CompleteShip);
		EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded, refused.status);
		EXPECT_EQ(0U,
			buffer.observation().raw_static_catalog.class_count);
		EXPECT_EQ(0U,
			buffer.observation().raw_static_catalog.weapon_count);
		EXPECT_EQ(0U,
			buffer.observation().raw_static_catalog.auxiliary_count);
	} else {
		ADD_FAILURE()
			<< "Typed raw static catalog API is absent; deep-copy and exact/+1 "
			   "atomicity contracts cannot execute.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedCatalogDeepCopiesAndEveryPlusOneRollsBackAtomically)
{
	auto source = std::make_unique<FakePhase2EngineReadView>();
	auto buffer = std::make_unique<Phase2ObservationBuffer>();
	exercise_wp02_deep_copy_and_atomic_plus_one(*source, *buffer);
}

void initialize_wp02_merge_source(Phase2ShipSource& source,
	float class_mass,
	float weapon_damage)
{
	auto& catalog = source.raw_static_catalog;
	catalog.class_count = 1U;
	catalog.weapon_count = 1U;
	auto& ship_class = catalog.class_definitions[0];
	ship_class.class_capture_key = 11U;
	ASSERT_TRUE(ship_class.internal_name.assign("same-class-name"));
	ship_class.model_mass = class_mass;
	auto& weapon = catalog.weapon_definitions[0];
	weapon.weapon_capture_key = 21U;
	weapon.reloaded_per_batch = 1U;
	ASSERT_TRUE(weapon.internal_name.assign("same-weapon-name"));
	weapon.damage = weapon_damage;
	source.raw_static_references.class_capture_key = 11U;
	source.raw_static_references.weapon_count = 1U;
	source.raw_static_references.weapon_capture_keys[0] = 21U;
}

template <typename Observation, typename = void>
struct has_wp02_typed_variant_diagnostic : std::false_type {};

template <typename Observation>
struct has_wp02_typed_variant_diagnostic<Observation,
	std::void_t<decltype(
		std::declval<Observation&>().raw_static_diagnostic.reason)>>
	: std::true_type {};

template <typename Observation>
void expect_wp02_variant_diagnostic(const Observation& observation)
{
	if constexpr (has_wp02_typed_variant_diagnostic<Observation>::value) {
		using Reason = std::remove_reference_t<decltype(
			observation.raw_static_diagnostic.reason)>;
		static_assert(std::is_enum_v<Reason>);
		EXPECT_NE(Reason{}, observation.raw_static_diagnostic.reason);
	} else {
		ADD_FAILURE()
			<< "Distinct same-name variants require an owned typed enum/reason "
			   "diagnostic.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedMergeDeduplicatesOnlyByteIdenticalDescriptors)
{
	auto source = std::make_unique<FakePhase2EngineReadView>();
	auto first = std::make_unique<Phase2ShipSource>();
	auto second = std::make_unique<Phase2ShipSource>();
	source->ship_count = 2U;
	source->block_source_by_ship[0] = first.get();
	source->block_source_by_ship[1] = second.get();
	initialize_wp02_merge_source(*first, 10.0F, 20.0F);
	initialize_wp02_merge_source(*second, 10.0F, 20.0F);
	ASSERT_TRUE(second->raw_static_catalog.class_definitions[0]
			.internal_name.assign("trailing-class-bytes"));
	ASSERT_TRUE(second->raw_static_catalog.class_definitions[0]
			.internal_name.assign("same-class-name"));
	ASSERT_TRUE(second->raw_static_catalog.weapon_definitions[0]
			.internal_name.assign("trailing-weapon-bytes"));
	ASSERT_TRUE(second->raw_static_catalog.weapon_definitions[0]
			.internal_name.assign("same-weapon-name"));
	second->raw_static_catalog.class_definitions[0].effective_mass = -0.0F;
	second->raw_static_catalog.class_definitions[0].class_capture_key = 901U;
	second->raw_static_catalog.weapon_definitions[0].weapon_capture_key = 701U;
	second->raw_static_references.class_capture_key = 901U;
	second->raw_static_references.weapon_capture_keys[0] = 701U;
	auto observation = std::make_unique<Phase2ObservationDto>();
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(*source, 710U, *observation,
			Phase2ObservationProjection::CompleteShip).status);
	EXPECT_EQ(1U, observation->raw_static_catalog.class_count);
	EXPECT_EQ(1U, observation->raw_static_catalog.weapon_count);
	ASSERT_EQ(2U, observation->ships.size());
	EXPECT_EQ(1U,
		observation->ships[0].raw_static_references.class_capture_key);
	EXPECT_EQ(1U,
		observation->ships[1].raw_static_references.class_capture_key);
	EXPECT_EQ(1U,
		observation->ships[1].raw_static_references.weapon_capture_keys[0])
		<< "Arbitrary source keys must remap deterministically without "
		   "collisions; trailing bytes and signed zero are non-semantic.";

	initialize_wp02_merge_source(*second, 11.0F, 21.0F);
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(*source, 711U, *observation,
			Phase2ObservationProjection::CompleteShip).status);
	EXPECT_EQ(2U, observation->raw_static_catalog.class_count)
		<< "Same name with a distinct class descriptor is a variant.";
	EXPECT_EQ(2U, observation->raw_static_catalog.weapon_count)
		<< "Same name with a distinct weapon descriptor is a variant.";
	expect_wp02_variant_diagnostic(*observation);
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedRegistryHomonymsHaveTypedAmbiguityPolicy)
{
	auto source = std::make_unique<FakePhase2EngineReadView>();
	auto first = std::make_unique<Phase2ShipSource>();
	auto second = std::make_unique<Phase2ShipSource>();
	source->ship_count = 2U;
	source->block_source_by_ship[0] = first.get();
	source->block_source_by_ship[1] = second.get();
	initialize_wp02_merge_source(*first, 10.0F, 20.0F);
	initialize_wp02_merge_source(*second, 10.0F, 20.0F);
	for (auto* ship : {first.get(), second.get()}) {
		auto& catalog = ship->raw_static_catalog;
		catalog.auxiliary_count = 1U;
		catalog.auxiliary_entries[0].registry =
			Phase2RawAuxiliaryRegistry::Pattern;
		catalog.auxiliary_entries[0].capture_key = 301U;
		ASSERT_TRUE(
			catalog.auxiliary_entries[0].name.assign("same-registry-name"));
		ship->raw_static_references.auxiliary_count = 1U;
		ship->raw_static_references.auxiliary_capture_keys[0] = 301U;
	}
	second->raw_static_catalog.auxiliary_entries[0]
		.firing_pattern_source_code = 9U;

	auto observation = std::make_unique<Phase2ObservationDto>();
	const auto result = collect_fake_phase2_observation(*source, 712U,
		*observation, Phase2ObservationProjection::CompleteShip);
	if (result.status == Phase2CaptureStatus::Valid) {
		EXPECT_EQ(2U, observation->raw_static_catalog.auxiliary_count)
			<< "A same-name descriptor conflict must not silently alias.";
		expect_wp02_variant_diagnostic(*observation);
	} else {
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
		EXPECT_EQ(0U, observation->raw_static_catalog.auxiliary_count);
		EXPECT_TRUE(observation->ships.empty());
		expect_wp02_variant_diagnostic(*observation);
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReviewB2NamedRegistryHomonymsAreAmbiguousButEmptyPatternsAreNot)
{
	for (const auto registry : {
			 Phase2RawAuxiliaryRegistry::Species,
			 Phase2RawAuxiliaryRegistry::ShipType,
			 Phase2RawAuxiliaryRegistry::Iff,
			 Phase2RawAuxiliaryRegistry::Wing,
			 Phase2RawAuxiliaryRegistry::Armor,
			 Phase2RawAuxiliaryRegistry::DamageType}) {
		SCOPED_TRACE(static_cast<int>(registry));
		auto source = std::make_unique<FakePhase2EngineReadView>();
		auto first = std::make_unique<Phase2ShipSource>();
		auto second = std::make_unique<Phase2ShipSource>();
		source->ship_count = 2U;
		source->block_source_by_ship[0] = first.get();
		source->block_source_by_ship[1] = second.get();
		initialize_wp02_merge_source(*first, 10.0F, 20.0F);
		initialize_wp02_merge_source(*second, 10.0F, 20.0F);
		for (auto* ship : {first.get(), second.get()}) {
			auto& entry = ship->raw_static_catalog.auxiliary_entries[0];
			ship->raw_static_catalog.auxiliary_count = 1U;
			entry.registry = registry;
			entry.capture_key = 401U;
			ASSERT_TRUE(entry.name.assign("same-named-authority"));
			ship->raw_static_references.auxiliary_count = 1U;
			ship->raw_static_references.auxiliary_capture_keys[0] = 401U;
		}
		second->raw_static_catalog.auxiliary_entries[0]
			.firing_pattern_source_code = 1U;

		auto observation = std::make_unique<Phase2ObservationDto>();
		ASSERT_EQ(Phase2CaptureStatus::Valid,
			collect_fake_phase2_observation(*source, 713U, *observation,
				Phase2ObservationProjection::CompleteShip).status);
		EXPECT_EQ(
			Phase2ObservationDto::RawStaticDiagnostic::Reason::
				AmbiguousAuxiliaryName,
			observation->raw_static_diagnostic.reason);
	}

	auto source = std::make_unique<FakePhase2EngineReadView>();
	auto first = std::make_unique<Phase2ShipSource>();
	auto second = std::make_unique<Phase2ShipSource>();
	source->ship_count = 2U;
	source->block_source_by_ship[0] = first.get();
	source->block_source_by_ship[1] = second.get();
	initialize_wp02_merge_source(*first, 10.0F, 20.0F);
	initialize_wp02_merge_source(*second, 10.0F, 20.0F);
	for (auto* ship : {first.get(), second.get()}) {
		auto& entry = ship->raw_static_catalog.auxiliary_entries[0];
		ship->raw_static_catalog.auxiliary_count = 1U;
		entry.registry = Phase2RawAuxiliaryRegistry::Pattern;
		entry.capture_key = 501U;
		ship->raw_static_references.auxiliary_count = 1U;
		ship->raw_static_references.auxiliary_capture_keys[0] = 501U;
	}
	first->raw_static_catalog.auxiliary_entries[0]
		.firing_pattern_source_code = 1U;
	second->raw_static_catalog.auxiliary_entries[0]
		.firing_pattern_source_code = 2U;
	auto observation = std::make_unique<Phase2ObservationDto>();
	ASSERT_EQ(Phase2CaptureStatus::Valid,
		collect_fake_phase2_observation(*source, 714U, *observation,
			Phase2ObservationProjection::CompleteShip).status);
	EXPECT_EQ(Phase2ObservationDto::RawStaticDiagnostic::Reason::None,
		observation->raw_static_diagnostic.reason);
	EXPECT_EQ(2U, observation->raw_static_catalog.auxiliary_count);
}

template <typename Input>
void exercise_wp02_review_b3_same_mapper_hostiles()
{
	if constexpr (has_wp02_exhaustive_hostile_mapper_authorities<Input>::value) {
		auto input = std::make_unique<Input>();
		auto catalog = std::make_unique<Phase2RawStaticCatalog>();
		input->guards_valid = true;
		input->weapon_info.fire_wait_seconds = -1.0F;
		auto result = map_phase2_static_authorities(*input, *catalog);
		EXPECT_EQ(Phase2SourceReadStatus::UnsupportedEngineState, result.status);
		EXPECT_EQ(0U, catalog->class_count);
		EXPECT_EQ(0U, catalog->weapon_count);

		*input = {};
		input->guards_valid = true;
		input->bank_count = 1U;
		input->banks[0].num_slots = 0U;
		result = map_phase2_static_authorities(*input, *catalog);
		EXPECT_EQ(Phase2SourceReadStatus::UnsupportedEngineState, result.status);
		EXPECT_EQ(0U, catalog->class_count);
		EXPECT_EQ(0U, catalog->weapon_count);
	} else {
		ADD_FAILURE()
			<< "B3 RED: the one exhaustive mapper input does not expose "
			   "weapon fire_wait_seconds and bank num_slots, so production "
			   "and seam cannot execute identical hostile validation.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReviewB3ProductionAndSeamMapperRejectSameHostilesFailEmpty)
{
	exercise_wp02_review_b3_same_mapper_hostiles<Phase2StaticAuthorityInput>();
}

void initialize_wp02_hostile_catalog(Phase2RawStaticCatalog& catalog)
{
	catalog.clear();
	catalog.class_count = 1U;
	catalog.weapon_count = 1U;
	catalog.aggregate_subsystem_count = 1U;
	auto& ship_class = catalog.class_definitions[0];
	ship_class.class_capture_key = 1U;
	ASSERT_TRUE(ship_class.internal_name.assign("hostile-class"));
	ship_class.subsystem_count = 1U;
	ship_class.subsystem_offset = 0U;
	auto& subsystem = catalog.subsystem_storage[0];
	subsystem.subsystem_capture_key = 1U;
	ASSERT_TRUE(subsystem.internal_name.assign("hostile-subsystem"));
	auto& weapon = catalog.weapon_definitions[0];
	weapon.weapon_capture_key = 1U;
	ASSERT_TRUE(weapon.internal_name.assign("hostile-weapon"));
	weapon.maximum_range = 100.0F;
	weapon.optimal_range = 50.0F;
	weapon.reloaded_per_batch = 1U;
	weapon.burst_shots = 1;
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedHostileStaticScalarMatrixFailsWithoutCoercion)
{
	using Mutation = void (*)(Phase2RawStaticCatalog&);
	const std::array<std::pair<const char*, Mutation>, 12U> hostile{{
		{"negative-reloaded-per-batch", [](auto& value) {
			 value.weapon_definitions[0].reloaded_per_batch =
				 static_cast<std::uint32_t>(-1);
		 }},
		{"zero-reload-slots", [](auto& value) {
			 value.weapon_definitions[0].reloaded_per_batch = 0U;
		 }},
		{"negative-firewait", [](auto& value) {
			 value.weapon_definitions[0].fire_wait_seconds = -1.0F;
		 }},
		{"negative-hitpoints", [](auto& value) {
			 value.class_definitions[0].max_hull_strength = -1.0F;
		 }},
		{"negative-radius", [](auto& value) {
			 value.subsystem_storage[0].radius = -1.0F;
		 }},
		{"negative-minimum-range", [](auto& value) {
			 value.weapon_definitions[0].minimum_range = -1.0F;
		 }},
		{"inverted-ranges", [](auto& value) {
			 value.weapon_definitions[0].optimal_range = 101.0F;
		 }},
		{"negative-lifetime", [](auto& value) {
			 value.weapon_definitions[0].lifetime_seconds = -1.0F;
		 }},
		{"negative-lock-duration", [](auto& value) {
			 value.weapon_definitions[0].lock_time_seconds = -1.0F;
		 }},
		{"invalid-subtype-enum", [](auto& value) {
			 value.weapon_definitions[0].weapon_subtype_source = 0xffU;
		 }},
		{"invalid-guidance-enum", [](auto& value) {
			 value.weapon_definitions[0].guidance_type_source = 0xffU;
		 }},
		{"invalid-capacity-presence", [](auto& value) {
			 value.class_definitions[0].countermeasure_uses_capacity = true;
			 value.class_definitions[0].countermeasure_capacity = 0.0F;
		 }},
	}};

	for (const auto& [label, mutate] : hostile) {
		SCOPED_TRACE(label);
		auto input = std::make_unique<Phase2StaticAuthorityInput>();
		auto catalog = std::make_unique<Phase2RawStaticCatalog>();
		input->guards_valid = true;
		initialize_wp02_hostile_catalog(*catalog);
		mutate(*catalog);
		const auto result = map_phase2_static_authorities(*input, *catalog);
		EXPECT_EQ(Phase2SourceReadStatus::UnsupportedEngineState, result.status);
		EXPECT_EQ(0U, catalog->class_count);
		EXPECT_EQ(0U, catalog->weapon_count);
		EXPECT_EQ(0U, catalog->aggregate_subsystem_count);
	}
}

template <typename Observation>
void exercise_wp02_missing_remaps_and_owners()
{
	if constexpr (has_wp02_flat_owned_nested_layout<Observation>::value) {
	  for (const bool missing_owner : {false, true}) {
		SCOPED_TRACE(missing_owner);
		auto source = std::make_unique<FakePhase2EngineReadView>();
		initialize_wp02_merge_source(*source->block_source, 10.0F, 20.0F);
		auto& catalog = source->block_source->raw_static_catalog;
		if (missing_owner) {
			auto& ship_class = catalog.class_definitions[0];
			ship_class.bank_count = 1U;
			ship_class.bank_offset = 0U;
			ship_class.turret_bank_count = 1U;
			auto& bank = catalog.bank_storage[0];
			bank.bank_capture_key = 31U;
			bank.owner_subsystem_capture_key = 999U;
			bank.family_source = 4U;
			bank.source_family = 1U;
			bank.weapon_capture_key = 21U;
			bank.fire_point_count = 1U;
			bank.fire_points[0] = {1.0F, 0.0F, 0.0F};
		} else {
			catalog.weapon_definitions[0].damage_type_capture_key = 999U;
		}
		auto observation = std::make_unique<Phase2ObservationDto>();
		const auto result =
			collect_fake_phase2_observation(*source, 712U, *observation,
				Phase2ObservationProjection::CompleteShip);
		EXPECT_EQ(Phase2CaptureStatus::UnsupportedEngineState, result.status);
		EXPECT_EQ(0U, observation->raw_static_catalog.class_count);
		EXPECT_EQ(0U, observation->raw_static_catalog.weapon_count);
		EXPECT_TRUE(observation->ships.empty());
	  }
	} else {
		ADD_FAILURE()
			<< "Missing-remap/owner oracle requires the flat owned catalog "
			   "offset API.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedMissingRemapsAndOwnersFailEmptyWithoutRawOrZeroKeys)
{
	exercise_wp02_missing_remaps_and_owners<Phase2ObservationDto>();
}

enum class Wp02StaticBoundary {
	Classes65,
	Weapons4097,
	AggregateSubsystems4097,
	SubsystemsPerClass1025,
	BanksPerClass193,
	PrimaryBanks65,
	SecondaryBanks65,
	TertiaryBanks65,
	TurretBanks65,
	FirePointsPerBank65,
	AuxiliaryEntries257,
};

template <typename Source, typename Buffer>
void exercise_wp02_static_plus_one(
	Source& source, Buffer& buffer, Wp02StaticBoundary boundary)
{
	using Observation = std::remove_reference_t<
		decltype(std::declval<Buffer&>().observation())>;
	if constexpr (
		has_wp02_catalog_shape<Observation>::value &&
		has_wp02_complete_class_facts<Observation>::value &&
		has_wp02_complete_nested_class_facts<Observation>::value &&
		has_wp02_complete_weapon_facts<Observation>::value &&
		has_wp02_capture_local_auxiliary_facts<Observation>::value) {
		auto& catalog = source.block_source->raw_static_catalog;
		catalog.class_count = 1U;
		catalog.weapon_count = 1U;
		catalog.auxiliary_count = 1U;
		catalog.aggregate_subsystem_count = 1U;
		catalog.class_definitions[0].class_capture_key = 1U;
		ASSERT_TRUE(
			catalog.class_definitions[0].internal_name.assign("boundary"));
		catalog.class_definitions[0].subsystem_count = 1U;
		catalog.class_definitions[0].subsystem_offset = 0U;
		catalog.subsystem_storage[0].subsystem_capture_key = 1U;
		ASSERT_TRUE(catalog.subsystem_storage[0].internal_name.assign(
			"boundary-subsystem"));
		catalog.class_definitions[0].bank_count = 1U;
		catalog.class_definitions[0].bank_offset = 0U;
		catalog.class_definitions[0].primary_bank_count = 1U;
		catalog.bank_storage[0].bank_capture_key = 1U;
		catalog.bank_storage[0].weapon_capture_key = 1U;
		catalog.bank_storage[0].fire_point_count = 1U;
		catalog.weapon_definitions[0].weapon_capture_key = 1U;
		catalog.weapon_definitions[0].reloaded_per_batch = 1U;
		ASSERT_TRUE(
			catalog.weapon_definitions[0].internal_name.assign("boundary-weapon"));
		catalog.auxiliary_entries[0].capture_key = 1U;
		ASSERT_TRUE(
			catalog.auxiliary_entries[0].name.assign("boundary-aux"));

		switch (boundary) {
		case Wp02StaticBoundary::Classes65:
			catalog.class_count = 65U;
			break;
		case Wp02StaticBoundary::Weapons4097:
			catalog.weapon_count = 4097U;
			break;
		case Wp02StaticBoundary::AggregateSubsystems4097:
			catalog.aggregate_subsystem_count = 4097U;
			break;
		case Wp02StaticBoundary::SubsystemsPerClass1025:
			catalog.class_definitions[0].subsystem_count = 1025U;
			break;
		case Wp02StaticBoundary::BanksPerClass193:
			catalog.class_definitions[0].bank_count = 193U;
			break;
		case Wp02StaticBoundary::PrimaryBanks65:
			catalog.class_definitions[0].bank_count = 65U;
			catalog.class_definitions[0].primary_bank_count = 65U;
			break;
		case Wp02StaticBoundary::SecondaryBanks65:
			catalog.class_definitions[0].bank_count = 65U;
			catalog.class_definitions[0].primary_bank_count = 0U;
			catalog.class_definitions[0].secondary_bank_count = 65U;
			break;
		case Wp02StaticBoundary::TertiaryBanks65:
			catalog.class_definitions[0].bank_count = 65U;
			catalog.class_definitions[0].primary_bank_count = 0U;
			catalog.class_definitions[0].tertiary_bank_count = 65U;
			break;
		case Wp02StaticBoundary::TurretBanks65:
			catalog.class_definitions[0].bank_count = 65U;
			catalog.class_definitions[0].primary_bank_count = 0U;
			catalog.class_definitions[0].turret_bank_count = 65U;
			break;
		case Wp02StaticBoundary::FirePointsPerBank65:
			catalog.bank_storage[0].fire_point_count = 65U;
			break;
		case Wp02StaticBoundary::AuxiliaryEntries257:
			catalog.auxiliary_count = 257U;
			break;
		}

		ASSERT_TRUE(buffer.provision(Phase2ProvisioningMode::ValidEnabled));
		ASSERT_TRUE(buffer.enter_ready());
		const auto result = collect_phase2_observation(buffer, source, 703U,
			Phase2ObservationProjection::CompleteShip);
		EXPECT_EQ(Phase2CaptureStatus::SourceLimitExceeded, result.status);
		EXPECT_EQ(0U,
			buffer.observation().raw_static_catalog.class_count);
		EXPECT_EQ(0U,
			buffer.observation().raw_static_catalog.weapon_count);
		EXPECT_EQ(0U,
			buffer.observation().raw_static_catalog.auxiliary_count);
	} else {
		ADD_FAILURE()
			<< "Typed raw static catalog API is absent; exact/+1 boundary "
			   "contract cannot execute.";
	}
}

TEST(TelemetryPhase2ObservationContract,
	Wp02ReopenedEveryStaticPlusOneFailsBeforeCopyWithoutPartialDto)
{
	for (const auto boundary : {
			 Wp02StaticBoundary::Classes65,
			 Wp02StaticBoundary::Weapons4097,
			 Wp02StaticBoundary::AggregateSubsystems4097,
			 Wp02StaticBoundary::SubsystemsPerClass1025,
			 Wp02StaticBoundary::BanksPerClass193,
			 Wp02StaticBoundary::PrimaryBanks65,
			 Wp02StaticBoundary::SecondaryBanks65,
			 Wp02StaticBoundary::TertiaryBanks65,
			 Wp02StaticBoundary::TurretBanks65,
			 Wp02StaticBoundary::FirePointsPerBank65,
			 Wp02StaticBoundary::AuxiliaryEntries257}) {
		SCOPED_TRACE(static_cast<int>(boundary));
		auto source = std::make_unique<FakePhase2EngineReadView>();
		auto buffer = std::make_unique<Phase2ObservationBuffer>();
		exercise_wp02_static_plus_one(*source, *buffer, boundary);
	}
}

} // namespace

void* operator new(std::size_t size)
{
	return allocate(size);
}

void* operator new[](std::size_t size)
{
	return allocate(size);
}

void operator delete(void* memory) noexcept
{
	std::free(memory);
}

void operator delete[](void* memory) noexcept
{
	std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
	std::free(memory);
}
