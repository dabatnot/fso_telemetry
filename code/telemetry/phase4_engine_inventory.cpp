#include "telemetry/phase4_engine_inventory.h"

#include "globalincs/systemvars.h"
#include "object/object.h"
#include "ship/ship.h"
#include "telemetry/engine_adapter.h"
#include "weapon/weapon.h"

#include <cmath>

namespace telemetry::detail {
namespace {

protocol::ObjectType object_type(const object& source) noexcept
{
	switch (source.type) {
	case OBJ_SHIP: return protocol::ObjectType::Ship;
	case OBJ_WEAPON: return protocol::ObjectType::Weapon;
	case OBJ_ASTEROID: return protocol::ObjectType::Asteroid;
	case OBJ_DEBRIS: return protocol::ObjectType::Debris;
	case OBJ_JUMP_NODE: return protocol::ObjectType::JumpNode;
	case OBJ_WAYPOINT: return protocol::ObjectType::Waypoint;
	case OBJ_FIREBALL: return protocol::ObjectType::Fireball;
	default: return protocol::ObjectType::Other;
	}
}

bool live_exportable_object(const object& source) noexcept
{
	return source.signature > 0 &&
		!source.flags[Object::Object_Flags::Should_be_dead];
}

bool static_marker(protocol::ObjectType type) noexcept
{
	return type == protocol::ObjectType::JumpNode ||
		type == protocol::ObjectType::Waypoint;
}

bool source_class_key(const object& source, protocol::ObjectType type,
	std::uint32_t& output) noexcept
{
	output = 0U;
	if (type == protocol::ObjectType::Ship) {
		if (source.instance < 0 || source.instance >= MAX_SHIPS ||
			Ships[source.instance].objnum != OBJ_INDEX(&source) ||
			Ships[source.instance].ship_info_index < 0)
			return false;
		output = static_cast<std::uint32_t>(
			Ships[source.instance].ship_info_index) + 1U;
		return true;
	}
	if (type == protocol::ObjectType::Weapon) {
		if (source.instance < 0 || source.instance >= MAX_WEAPONS ||
			Weapons[source.instance].objnum != OBJ_INDEX(&source) ||
			Weapons[source.instance].weapon_info_index < 0)
			return false;
		output = static_cast<std::uint32_t>(
			Weapons[source.instance].weapon_info_index) + 1U;
	}
	return true;
}

bool copy_pose(const object& source, bool marker,
	Phase4EngineInventoryEntry& output) noexcept
{
	output.position_world = {source.pos.xyz.x, source.pos.xyz.y, source.pos.xyz.z};
	output.radius = source.radius;
	if (!std::isfinite(output.position_world[0]) ||
		!std::isfinite(output.position_world[1]) ||
		!std::isfinite(output.position_world[2]) ||
		!std::isfinite(output.radius) || output.radius < 0.0F)
		return false;
	output.static_marker = marker;
	if (marker) {
		output.orientation_local_to_world = {0.0F, 0.0F, 0.0F, 1.0F};
		output.velocity_world = {};
		output.rotational_velocity_local = {};
		output.physics_mode_flags = 0U;
		return true;
	}
	const CaptureOrientationBasis basis{
		{source.orient.vec.rvec.xyz.x, source.orient.vec.rvec.xyz.y, source.orient.vec.rvec.xyz.z},
		{source.orient.vec.uvec.xyz.x, source.orient.vec.uvec.xyz.y, source.orient.vec.uvec.xyz.z},
		{source.orient.vec.fvec.xyz.x, source.orient.vec.fvec.xyz.y, source.orient.vec.fvec.xyz.z}};
	CaptureQuaternionf orientation;
	if (convert_fso_orientation_to_local_to_world(basis, orientation) !=
		QuaternionConversionStatus::Converted)
		return false;
	// Phase 4 FLIGHT_STATE stores the quaternion as x/y/z/w, unlike the
	// CaptureQuaternionf helper's w/x/y/z transport representation.
	output.orientation_local_to_world = {orientation.x, orientation.y, orientation.z, orientation.w};
	output.velocity_world = {source.phys_info.vel.xyz.x, source.phys_info.vel.xyz.y,
		source.phys_info.vel.xyz.z};
	output.rotational_velocity_local = {source.phys_info.rotvel.xyz.x,
		source.phys_info.rotvel.xyz.y, source.phys_info.rotvel.xyz.z};
	for (const auto value : output.velocity_world)
		if (!std::isfinite(value)) return false;
	for (const auto value : output.rotational_velocity_local)
		if (!std::isfinite(value)) return false;
	output.physics_mode_flags = map_player_physics_mode_flags({
		static_cast<std::uint32_t>(source.phys_info.flags),
		source.flags[Object::Object_Flags::Immobile],
		source.flags[Object::Object_Flags::Dont_change_position],
		source.flags[Object::Object_Flags::Dont_change_orientation]});
	return true;
}

} // namespace

Phase4EngineInventoryStatus collect_phase4_engine_inventory(
	Phase4EntityRegistry& identities,
	std::vector<Phase4EngineInventoryEntry>& output) noexcept
{
	if (!identities.ready()) return Phase4EngineInventoryStatus::NotReady;
	std::size_t count = 0U;
	for (auto* source = GET_FIRST(&obj_used_list);
		 source != END_OF_LIST(&obj_used_list);
		 source = GET_NEXT(source)) {
		if (!live_exportable_object(*source)) continue;
		if (++count > Phase4MaximumEntityIdentities)
			return Phase4EngineInventoryStatus::SourceLimitExceeded;
	}
	if (output.capacity() < count)
		return Phase4EngineInventoryStatus::InsufficientStorage;

	output.clear();
	for (auto* source = GET_FIRST(&obj_used_list);
		 source != END_OF_LIST(&obj_used_list);
		 source = GET_NEXT(source)) {
		if (!live_exportable_object(*source)) continue;
		Phase4EngineInventoryEntry entry;
		entry.identity = {static_cast<std::uint64_t>(source->signature), object_type(*source)};
		if (!source_class_key(*source, entry.identity.object_type, entry.source_class_key) ||
			!copy_pose(*source, static_marker(entry.identity.object_type), entry)) {
			output.clear();
			return Phase4EngineInventoryStatus::InvalidSource;
		}
		const auto resolved = identities.resolve(entry.identity);
		if (resolved.status != Phase4EntityRegistryStatus::Existing &&
			resolved.status != Phase4EntityRegistryStatus::Allocated) {
			output.clear();
			return Phase4EngineInventoryStatus::IdentityFailure;
		}
		entry.entity_id = resolved.entity_id;
		output.push_back(entry);
	}
	// Retire only after the complete source traversal and all identity resolves
	// succeeded. A transient invalid object therefore cannot erase an otherwise
	// coherent exported graph.
	if (identities.begin_reconciliation() != Phase4EntityRegistryStatus::Reconciled) {
		output.clear();
		return Phase4EngineInventoryStatus::IdentityFailure;
	}
	for (const auto& entry : output) {
		if (identities.mark_observed(entry.identity) !=
			Phase4EntityRegistryStatus::Existing) {
			output.clear();
			return Phase4EngineInventoryStatus::IdentityFailure;
		}
	}
	if (identities.commit_reconciliation() != Phase4EntityRegistryStatus::Reconciled) {
		output.clear();
		return Phase4EngineInventoryStatus::IdentityFailure;
	}
	return Phase4EngineInventoryStatus::Collected;
}

} // namespace telemetry::detail
