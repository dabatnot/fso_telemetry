#include "telemetry/phase3_engine_collector.h"

#include "ai/ai.h"
#include "asteroid/asteroid.h"
#include "autopilot/autopilot.h"
#include "debris/debris.h"
#include "globalincs/systemvars.h"
#include "hud/hudconfig.h"
#include "hud/hudparse.h"
#include "hud/hudtarget.h"
#include "iff_defs/iff_defs.h"
#include "jumpnode/jumpnode.h"
#include "mod_table/mod_table.h"
#include "object/object.h"
#include "object/objectdock.h"
#include "object/waypoint.h"
#include "playerman/player.h"
#include "radar/radarsetup.h"
#include "ship/ship.h"
#include "ship/awacs.h"
#include "ship/subsysdamage.h"
#include "telemetry/engine_adapter.h"
#include "weapon/emp.h"
#include "weapon/weapon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

namespace telemetry::detail {
namespace {


template <typename T>
void reconstruct_in_place(T& value) noexcept
{
	// Phase3Projection and its navigation/contact members own large fixed
	// workspaces.  Aggregate assignment from {} makes MSVC materialize a
	// value-initialized temporary on the engine thread stack, even when the
	// branch is not taken.  Reconstruct directly in the existing heap-backed
	// storage instead.
	value.~T();
	new (&value) T();
}

protocol::ObjectType object_type(int type) noexcept
{
	switch (type) {
	case OBJ_SHIP: return protocol::ObjectType::Ship;
	case OBJ_WEAPON: return protocol::ObjectType::Weapon;
	case OBJ_ASTEROID: return protocol::ObjectType::Asteroid;
	case OBJ_DEBRIS: return protocol::ObjectType::Debris;
	case OBJ_JUMP_NODE: return protocol::ObjectType::JumpNode;
	case OBJ_WAYPOINT: return protocol::ObjectType::Waypoint;
	case OBJ_FIREBALL: return protocol::ObjectType::Fireball;
	default: return protocol::ObjectType::Unknown;
	}
}

std::uint8_t radar_category(protocol::ObjectType type) noexcept
{
	switch (type) {
	case protocol::ObjectType::Ship:
		return static_cast<std::uint8_t>(protocol::RadarCategory::Ship);
	case protocol::ObjectType::Weapon:
		return static_cast<std::uint8_t>(protocol::RadarCategory::Weapon);
	case protocol::ObjectType::Waypoint:
		return static_cast<std::uint8_t>(protocol::RadarCategory::Navigation);
	case protocol::ObjectType::JumpNode:
		return static_cast<std::uint8_t>(protocol::RadarCategory::JumpNode);
	case protocol::ObjectType::Asteroid:
		return static_cast<std::uint8_t>(protocol::RadarCategory::Asteroid);
	case protocol::ObjectType::Debris:
		return static_cast<std::uint8_t>(protocol::RadarCategory::Debris);
	default:
		return static_cast<std::uint8_t>(protocol::RadarCategory::Other);
	}
}

protocol::RadarVisibility radar_visibility(RadarVisibility visibility) noexcept
{
	switch (visibility) {
	case VISIBLE: return protocol::RadarVisibility::Visible;
	case DISTORTED: return protocol::RadarVisibility::Distorted;
	case NOT_VISIBLE:
	default: return protocol::RadarVisibility::NotVisible;
	}
}

void copy_color(const color& source,
	std::array<std::uint8_t, 4U>& destination) noexcept
{
	destination = {{source.red, source.green, source.blue, source.alpha}};
}

bool valid_iff_team(int team) noexcept
{
	return team >= 0 && static_cast<std::size_t>(team) < Iff_info.size();
}

bool capture_hud_target_color(object& target,
	std::array<std::uint8_t, 4U>& destination) noexcept
{
	if (Player_ship == nullptr || !valid_iff_team(Player_ship->team)) {
		return false;
	}

	switch (target.type) {
	case OBJ_SHIP:
		if (target.instance < 0 || target.instance >= MAX_SHIPS) return false;
		{
			const auto& source = Ships[target.instance];
			if (!valid_iff_team(source.team) || source.ship_info_index < 0 ||
				static_cast<std::size_t>(source.ship_info_index) >= Ship_info.size()) {
				return false;
			}
		}
		break;
	case OBJ_WEAPON:
		if (target.instance < 0 || target.instance >= MAX_WEAPONS ||
			!valid_iff_team(Weapons[target.instance].team)) {
			return false;
		}
		break;
	case OBJ_DEBRIS:
		if (target.instance < 0 ||
			static_cast<std::size_t>(target.instance) >= Debris.size() ||
			!valid_iff_team(Debris[target.instance].team)) {
			return false;
		}
		break;
	case OBJ_ASTEROID:
		if (target.instance < 0 || target.instance >= MAX_ASTEROIDS ||
			!valid_iff_team(Iff_traitor)) {
			return false;
		}
		break;
	case OBJ_JUMP_NODE:
		break;
	default:
		return false;
	}

	if (auto* target_color = hud_get_iff_color(&target, 1)) {
		copy_color(*target_color, destination);
		return true;
	}
	return false;
}

bool valid_live_object(const object* candidate) noexcept
{
	if (candidate == nullptr || candidate->signature <= 0 ||
		candidate->flags[Object::Object_Flags::Should_be_dead]) {
		return false;
	}
	for (auto* current = GET_FIRST(&obj_used_list);
		 current != END_OF_LIST(&obj_used_list);
		 current = GET_NEXT(current)) {
		if (current == candidate) return true;
	}
	return false;
}

Phase3IdentityResolveResult resolve_object(
	Phase3IdentityRegistry& identities, const object& source) noexcept
{
	const auto type = object_type(source.type);
	if (type == protocol::ObjectType::Unknown || source.signature <= 0) {
		return {};
	}
	return identities.resolve({static_cast<std::uint32_t>(source.signature),
		static_cast<std::uint8_t>(type)});
}

bool resolved(const Phase3IdentityResolveResult& result) noexcept
{
	return result.status == Phase3IdentityResolveStatus::Existing ||
		result.status == Phase3IdentityResolveStatus::Allocated;
}

bool cockpit_track_visible(object& source,
	RadarContactProjection& projection) noexcept
{
	// Radar projection is the engine-owned cockpit authorization surface.  It
	// must run before an identity can be allocated or a dynamic reference can
	// reach the per-client projection; a target or lock alone is not a grant of
	// visibility.
	return radar_project_contact(&source, projection);
}

const char* target_box_type_label(int type) noexcept
{
	switch (type) {
	case OBJ_SHIP: return "SHIP";
	case OBJ_WEAPON: return "WEAPON";
	case OBJ_DEBRIS: return "DEBRIS";
	case OBJ_ASTEROID: return "ASTEROID";
	case OBJ_JUMP_NODE: return "JUMP NODE";
	default: return nullptr;
	}
}

object* current_player_target() noexcept
{
	if (Player_ai == nullptr || Player_ai->target_objnum < 0 ||
		Player_ai->target_objnum >= MAX_OBJECTS)
		return nullptr;
	auto& target = Objects[Player_ai->target_objnum];
	// Target selection is mutable gameplay state.  A stale index/signature pair
	// is not an invalid telemetry session: it merely means that TARGET_STATE has
	// no coherent current target at this sample time.
	if (target.signature != Player_ai->target_signature ||
		!valid_live_object(&target)) {
		return nullptr;
	}
	const auto object_index = Player_ai->target_objnum;
	switch (target.type) {
	case OBJ_SHIP:
		return target.instance >= 0 && target.instance < MAX_SHIPS &&
			Ships[target.instance].objnum == object_index ? &target : nullptr;
	case OBJ_WEAPON:
		return target.instance >= 0 && target.instance < MAX_WEAPONS &&
			Weapons[target.instance].objnum == object_index ? &target : nullptr;
	case OBJ_DEBRIS:
		return target.instance >= 0 &&
			static_cast<std::size_t>(target.instance) < Debris.size() &&
			Debris[target.instance].objnum == object_index ? &target : nullptr;
	case OBJ_ASTEROID:
		return target.instance >= 0 && target.instance < MAX_ASTEROIDS &&
			Asteroids[target.instance].objnum == object_index ? &target : nullptr;
	case OBJ_JUMP_NODE:
		return jumpnode_get_by_objnum(object_index) != nullptr ? &target : nullptr;
	default:
		return nullptr;
	}
}

std::uint32_t installed_weapon_class_id(
	const Phase2ManifestCandidate* manifest, int engine_index) noexcept
{
	if (manifest == nullptr || engine_index < 0) return 0U;
	const auto source_key =
		static_cast<std::uint32_t>(engine_index) + 1U;
	for (std::uint32_t index = 0U;
		 index < manifest->weapon_record_count; ++index) {
		if (manifest->weapon_records[index].source_key == source_key)
			return manifest->weapon_records[index].weapon_class_id;
	}
	return 0U;
}

const Phase2ClassRecord* installed_ship_class(
	const Phase2ManifestCandidate* manifest, int engine_index) noexcept
{
	if (manifest == nullptr || engine_index < 0) return nullptr;
	const auto source_key =
		static_cast<std::uint32_t>(engine_index) + 1U;
	for (std::uint32_t index = 0U;
		 index < manifest->class_record_count; ++index) {
		if (manifest->class_records[index].source_key == source_key)
			return &manifest->class_records[index];
	}
	// Phase 2 uses capture-local class keys.  The player class therefore does
	// not necessarily share the engine Ship_info index used above.  A selected
	// ship of that already installed class must still resolve, but only through
	// one unambiguous internal-name match.
	if (engine_index >= static_cast<int>(Ship_info.size())) return nullptr;
	const auto& engine_class = Ship_info[engine_index];
	std::size_t name_size = 0U;
	while (name_size < sizeof(engine_class.name) &&
		engine_class.name[name_size] != '\0')
		++name_size;
	if (name_size == 0U || name_size == sizeof(engine_class.name))
		return nullptr;
	const std::string_view engine_name{engine_class.name, name_size};
	const Phase2ClassRecord* matched = nullptr;
	for (std::uint32_t index = 0U;
		 index < manifest->class_record_count; ++index) {
		const auto& candidate = manifest->class_records[index];
		if (static_cast<std::string_view>(candidate.name) != engine_name)
			continue;
		if (matched != nullptr) return nullptr;
		matched = &candidate;
	}
	return matched;
}

bool capture_hud_ship_text(const ship& source,
	Phase3OwnedString<255U>& display_name,
	Phase3OwnedString<255U>& type_label) noexcept
{
	// The object list may expose a ship for one frame while its dependent HUD
	// tables are still being installed (spawn/despawn transitions). The native
	// HUD only reaches these helpers once those indices are valid; telemetry
	// observes the list independently and must omit decoration instead of
	// dereferencing an incomplete reference and terminating the capture.
	if (source.ship_info_index < 0 ||
		static_cast<std::size_t>(source.ship_info_index) >= Ship_info.size() ||
		source.team < 0 ||
		static_cast<std::size_t>(source.team) >= Iff_info.size() ||
		source.objnum < 0 || source.objnum >= MAX_OBJECTS) {
		return false;
	}
	char hud_name[NAME_LENGTH * 2 + 3]{};
	char hud_class[NAME_LENGTH]{};
	char hud_callsign[NAME_LENGTH]{};
	hud_stuff_ship_name(hud_name, &source);
	hud_stuff_ship_class(hud_class, &source);
	hud_stuff_ship_callsign(hud_callsign, &source);
	auto name_size = std::strlen(hud_name);
	const auto callsign_size = std::strlen(hud_callsign);
	if (callsign_size != 0U) {
		if (name_size == 0U) {
			std::memcpy(hud_name, hud_callsign, callsign_size + 1U);
			name_size = callsign_size;
		} else if (name_size + callsign_size + 3U < sizeof(hud_name)) {
			hud_name[name_size++] = ' ';
			hud_name[name_size++] = '(';
			std::memcpy(hud_name + name_size, hud_callsign, callsign_size);
			name_size += callsign_size;
			hud_name[name_size++] = ')';
			hud_name[name_size] = '\0';
		}
	}
	const auto class_size = std::strlen(hud_class);
	return (name_size == 0U || display_name.assign(hud_name, name_size)) &&
		(class_size == 0U || type_label.assign(hud_class, class_size));
}

std::uint32_t installed_hull_primary_bank_id(
	const Phase2ManifestCandidate* manifest,
	const ship& source,
	int bank_index) noexcept
{
	if (bank_index < 0) return 0U;
	const auto* installed =
		installed_ship_class(manifest, source.ship_info_index);
	if (installed == nullptr) return 0U;
	for (std::uint32_t index = 0U; index < installed->bank_count;
		 ++index) {
		const auto& candidate = installed->banks[index];
		if (candidate.family == WeaponFamily::Primary &&
			candidate.owner_subsystem_id == 0U &&
			candidate.canonical_index ==
				static_cast<std::uint16_t>(bank_index)) {
			return candidate.bank_id;
		}
	}
	return 0U;
}

bool collect_default_primary_lead(
	object& target,
	const Phase2ManifestCandidate* installed_manifest,
	Phase3TargetState& output) noexcept
{
	// TARGET_STATE has one (position, bank) lead pair.  The HUD's linked
	// MULTIPLE and AVERAGE modes deliberately render several or aggregate
	// positions, which cannot be truthfully represented by that pair.  Publish
	// only the default HUD choice; it uses the engine's own lead calculation,
	// not a telemetry approximation.
	if (Player_obj == nullptr || Player_ship == nullptr ||
		Lead_indicator_behavior != leadIndicatorBehavior::DEFAULT) {
		return false;
	}
	const auto& weapons = Player_ship->weapons;
	if (weapons.num_primary_banks <= 0 ||
		weapons.current_primary_bank < 0 ||
		weapons.current_primary_bank >= weapons.num_primary_banks) {
		return false;
	}
	float primary_range = 0.0F;
	const auto bank_index = hud_get_best_primary_bank(&primary_range);
	if (bank_index < 0 || bank_index >= weapons.num_primary_banks ||
		weapons.primary_bank_weapons[bank_index] < 0 ||
		weapons.primary_bank_weapons[bank_index] >= weapon_info_size()) {
		return false;
	}
	const auto bank_id = installed_hull_primary_bank_id(
		installed_manifest, *Player_ship, bank_index);
	if (bank_id == 0U) return false;

	vec3d target_position = target.pos;
	float distance = hud_find_target_distance(&target, Player_obj);
	if (Player_ai->targeted_subsys != nullptr) {
		get_subsystem_world_pos(&target, Player_ai->targeted_subsys,
			&target_position);
		distance = vm_vec_dist(&target_position, &Player_obj->pos);
	}
	if (!std::isfinite(distance) || distance < 0.0F) return false;
	vec3d lead{};
	if (!hud_calculate_lead_pos(&Player_obj->pos, &lead, &target_position,
			&target, &Weapon_info[weapons.primary_bank_weapons[bank_index]],
			distance) ||
		!std::isfinite(lead.xyz.x) || !std::isfinite(lead.xyz.y) ||
		!std::isfinite(lead.xyz.z)) {
		return false;
	}
	output.lead_world = {lead.xyz.x, lead.xyz.y, lead.xyz.z};
	output.lead_bank_id = bank_id;
	output.presence |= protocol::TargetStatePresenceFlagLead;
	return true;
}

std::uint32_t installed_subsystem_id(
	const Phase2ManifestCandidate* manifest,
	const object& owner,
	const ship_subsys* subsystem) noexcept
{
	if (subsystem == nullptr || owner.type != OBJ_SHIP ||
		owner.instance < 0 || owner.instance >= MAX_SHIPS) {
		return 0U;
	}
	const auto& source_ship = Ships[owner.instance];
	if (source_ship.ship_info_index < 0 ||
		source_ship.ship_info_index >= static_cast<int>(Ship_info.size())) {
		return 0U;
	}
	const auto& source_class = Ship_info[source_ship.ship_info_index];
	std::uint32_t source_key = 0U;
	if (source_class.n_subsystems < 0 ||
		(source_class.n_subsystems != 0 &&
		 source_class.subsystems == nullptr)) {
		return 0U;
	}
	for (std::size_t index = 0U;
		 index < static_cast<std::size_t>(
			 source_class.n_subsystems); ++index) {
		if (&source_class.subsystems[index] == subsystem->system_info) {
			source_key = static_cast<std::uint32_t>(index) + 1U;
			break;
		}
	}
	const auto* installed =
		installed_ship_class(manifest, source_ship.ship_info_index);
	if (source_key == 0U || installed == nullptr) return 0U;
	for (std::uint32_t index = 0U;
		 index < installed->subsystem_count; ++index) {
		if (installed->subsystems[index].source_key == source_key)
			return installed->subsystems[index].subsystem_id;
	}
	return 0U;
}

std::uint32_t installed_subsystem_id_from_source_key(
	const Phase2ManifestCandidate* manifest,
	const object& owner,
	Phase2CaptureLocalKey source_key) noexcept
{
	if (source_key.value == 0U || owner.type != OBJ_SHIP ||
		owner.instance < 0 || owner.instance >= MAX_SHIPS) {
		return 0U;
	}
	const auto& source_ship = Ships[owner.instance];
	const auto* installed =
		installed_ship_class(manifest, source_ship.ship_info_index);
	if (installed == nullptr) return 0U;
	for (std::uint32_t index = 0U;
		 index < installed->subsystem_count; ++index) {
		if (installed->subsystems[index].source_key == source_key.value)
			return installed->subsystems[index].subsystem_id;
	}
	return 0U;
}

bool capture_hud_subsystem_label(const object& owner,
	const ship_subsys* candidate,
	Phase3OwnedString<255U>& destination) noexcept
{
	if (candidate == nullptr || owner.type != OBJ_SHIP ||
		owner.instance < 0 || owner.instance >= MAX_SHIPS) {
		return false;
	}
	const auto& owner_ship = Ships[owner.instance];
	const ship_subsys* matched = nullptr;
	for (auto* subsystem = GET_FIRST(&owner_ship.subsys_list);
		 subsystem != END_OF_LIST(&owner_ship.subsys_list);
		 subsystem = GET_NEXT(subsystem)) {
		if (subsystem == candidate) {
			matched = subsystem;
			break;
		}
	}
	// Dynamic target changes can leave a pointer from the previous ship visible
	// for part of a tick.  Never dereference it unless it belongs to the current
	// target's live subsystem list.
	if (matched == nullptr || matched->system_info == nullptr) return false;
	const auto* label = ship_subsys_get_name_on_hud(matched);
	if (label == nullptr) return false;
	std::size_t length = 0U;
	while (length <= 255U && label[length] != '\0') ++length;
	return length != 0U && length <= 255U &&
		destination.assign(label, length);
}

std::uint8_t guidance_type(const weapon_info& info) noexcept
{
	if (info.wi_flags[Weapon::Info_Flags::Swarm])
		return static_cast<std::uint8_t>(protocol::GuidanceType::Swarm);
	if (info.wi_flags[Weapon::Info_Flags::Homing_heat])
		return static_cast<std::uint8_t>(protocol::GuidanceType::Heat);
	if (info.wi_flags[Weapon::Info_Flags::Homing_aspect])
		return static_cast<std::uint8_t>(protocol::GuidanceType::Aspect);
	if (info.wi_flags[Weapon::Info_Flags::Homing_javelin])
		return static_cast<std::uint8_t>(protocol::GuidanceType::Homing);
	return static_cast<std::uint8_t>(protocol::GuidanceType::None);
}

void copy_position(const vec3d& value, std::array<float, 3U>& output) noexcept
{
	output = {value.xyz.x, value.xyz.y, value.xyz.z};
}

std::uint8_t value_trend(int trend) noexcept
{
	switch (trend) {
	case DECREASING:
		return static_cast<std::uint8_t>(protocol::ValueTrend::Decreasing);
	case NO_CHANGE:
		return static_cast<std::uint8_t>(protocol::ValueTrend::Stable);
	case INCREASING:
		return static_cast<std::uint8_t>(protocol::ValueTrend::Increasing);
	default:
		return static_cast<std::uint8_t>(protocol::ValueTrend::Unknown);
	}
}

std::uint8_t scan_phase(CargoScanPhaseObservation phase) noexcept
{
	switch (phase) {
	case CargoScanPhaseObservation::Idle:
		return static_cast<std::uint8_t>(protocol::ScanPhase::Idle);
	case CargoScanPhaseObservation::Scanning:
		return static_cast<std::uint8_t>(protocol::ScanPhase::Scanning);
	case CargoScanPhaseObservation::Completed:
		return static_cast<std::uint8_t>(protocol::ScanPhase::Completed);
	case CargoScanPhaseObservation::NotScannable:
	case CargoScanPhaseObservation::Count:
	default:
		return static_cast<std::uint8_t>(protocol::ScanPhase::NotScannable);
	}
}

bool same_owned_string(const Phase3OwnedString<255U>& value,
	const char* bytes, std::size_t size) noexcept
{
	return value.size == size &&
		(size == 0U ||
		 std::memcmp(value.bytes.data(), bytes, size) == 0);
}

std::uint32_t resolve_navpoint_id(Phase3Projection& projection,
	const char* name, std::size_t name_size, std::uint8_t type) noexcept
{
	for (std::size_t index = 0U;
		 index < projection.nav_identity_count; ++index) {
		auto& entry = projection.nav_identities[index];
		if (entry.occupied &&
			same_owned_string(entry.name, name, name_size)) {
			return entry.type == type ? entry.navpoint_id : 0U;
		}
	}
	if (projection.nav_identity_count ==
			projection.nav_identities.size() ||
		projection.next_navpoint_id == 0U) {
		return 0U;
	}
	auto& entry = projection.nav_identities[
		projection.nav_identity_count++];
	if (!entry.name.assign(name, name_size)) return 0U;
	entry.type = type;
	entry.navpoint_id = projection.next_navpoint_id++;
	entry.occupied = true;
	return entry.navpoint_id;
}

protocol::AutopilotRefusal autopilot_refusal(
	AutopilotAvailability availability) noexcept
{
	switch (availability) {
	case AutopilotAvailability::NoSelection:
		return protocol::AutopilotRefusal::NoValidNav;
	case AutopilotAvailability::TooClose:
		return protocol::AutopilotRefusal::TooClose;
	case AutopilotAvailability::Hostiles:
		return protocol::AutopilotRefusal::Hostiles;
	case AutopilotAvailability::Gliding:
	case AutopilotAvailability::Hazard:
	case AutopilotAvailability::SupportPresent:
		return protocol::AutopilotRefusal::Unknown;
	case AutopilotAvailability::Available:
	default:
		return protocol::AutopilotRefusal::None;
	}
}

Phase3EngineCollectStatus reconcile_phase2(
	const Phase3EngineCollectInput& input,
	Phase3IdentityRegistry& identities) noexcept
{
	if (input.phase2_observation == nullptr ||
		input.phase2_binding_count != input.phase2_observation->ships.size() ||
		(input.phase2_source_signatures != nullptr &&
		 input.phase2_source_signature_count != input.phase2_binding_count) ||
		(input.phase2_binding_count != 0U && input.phase2_bindings == nullptr)) {
		return Phase3EngineCollectStatus::InvalidSource;
	}
	for (std::size_t index = 0U; index < input.phase2_binding_count; ++index) {
		const auto& source = input.phase2_observation->ships[index];
		const auto& binding = input.phase2_bindings[index];
		const auto source_signature = input.phase2_source_signatures != nullptr
			? input.phase2_source_signatures[index]
			: source.capture_key;
		if (source.capture_key.value == 0U || source_signature.value == 0U ||
			binding.capture_key.value != source.capture_key.value ||
			binding.entity_id == 0U) {
			return Phase3EngineCollectStatus::InvalidSource;
		}
		const auto status = identities.reconcile_phase2_binding(
			{source_signature.value,
			 static_cast<std::uint8_t>(protocol::ObjectType::Ship)},
			binding.entity_id);
		if (status != Phase3IdentityReconcileStatus::Existing &&
			status != Phase3IdentityReconcileStatus::Bound) {
			return Phase3EngineCollectStatus::IdentityFailure;
		}
	}
	return Phase3EngineCollectStatus::Collected;
}

Phase3EngineCollectStatus collect_target_and_locks(
	std::uint64_t sample_time,
	const Phase2ManifestCandidate* installed_manifest,
	Phase3IdentityRegistry& identities,
	object* current_target,
	Phase3Projection& output) noexcept
{
	output.target.producer_sample_time_us = sample_time;
	if (Player_ai == nullptr || Player_ship == nullptr) {
		return Phase3EngineCollectStatus::InvalidSource;
	}
	if (current_target != nullptr) {
		auto& target = *current_target;
		// TARGET_STATE mirrors the Target Box, not the radar.  A selected HUD
		// object may legitimately have no radar blip.
		if (target_box_type_label(target.type) != nullptr) {
			const auto identity = resolve_object(identities, target);
			if (!resolved(identity)) return Phase3EngineCollectStatus::IdentityFailure;
			output.target.current_target_entity_id = identity.entity_id;
		// Brackets and the target box use the bright HUD IFF color for the
		// selected object. Copy the already-resolved RGBA value so a remote
		// client never has to reproduce team maps, accessibility palettes or
		// per-object/per-class overrides.
		if (capture_hud_target_color(
				target, output.target.hud_target_color)) {
			output.target.presence |=
				protocol::TargetStatePresenceFlagHudTargetColor;
		}
		output.target.presence |=
			protocol::TargetStatePresenceFlagDistanceTrend |
			protocol::TargetStatePresenceFlagSpeedTrend;
		output.target.distance_trend =
			value_trend(Player_ai->current_target_dist_trend);
		output.target.speed_trend =
			value_trend(Player_ai->current_target_speed_trend);
		// Mirror the standard target box values after its HUD unit multipliers.
		// These are display-authoritative values: distance can target a subsystem
		// and speed has a docked-ship fallback, neither of which a client can
		// reproduce faithfully from the public track alone.
		const auto hud_distance = Player_ai->current_target_distance;
		if (std::isfinite(hud_distance) && hud_distance >= 0.0F) {
			output.target.presence |=
				protocol::TargetStatePresenceFlagExactHudDistance;
			output.target.exact_hud_distance = Hud_unit_multiplier > 0.0F
				? hud_distance * Hud_unit_multiplier
				: hud_distance;
		}
		float hud_speed = vm_vec_mag(&target.phys_info.vel);
		if (hud_speed < 0.1F) hud_speed = 0.0F;
		if (hud_speed == 0.0F && target.type == OBJ_SHIP) {
			hud_speed = dock_calc_docked_fspeed(&target);
			if (hud_speed < 0.1F) hud_speed = 0.0F;
		}
		if (std::isfinite(hud_speed) && hud_speed >= 0.0F) {
			output.target.presence |=
				protocol::TargetStatePresenceFlagExactHudSpeed;
			output.target.exact_hud_speed = Hud_speed_multiplier > 0.0F
				? hud_speed * Hud_speed_multiplier
				: hud_speed;
		}
		if (std::isfinite(Player_ai->target_time) &&
			Player_ai->target_time >= 0.0F) {
			output.target.presence |=
				protocol::TargetStatePresenceFlagTimeOnTarget;
			output.target.time_on_target_us =
				static_cast<std::uint64_t>(
					static_cast<double>(Player_ai->target_time) *
					1'000'000.0);
		}
		output.target.presence |=
			protocol::TargetStatePresenceFlagInCone;
		output.target.in_cone = Player->target_in_lock_cone != 0;
		// The AI target state is the cockpit's retained last stealth observation.
		// It is optional and may only cross the capture boundary when the engine
		// still has a real observation stamp and both cached vectors are usable.
		// Do not extrapolate it here: the telemetry projection publishes exactly
		// the retained engine observation or nothing.
		const auto& stealth_position = Player_ai->stealth_last_pos.xyz;
		const auto& stealth_velocity = Player_ai->stealth_velocity.xyz;
		if (Player_ai->stealth_last_visible_stamp > 0 &&
			std::isfinite(stealth_position.x) && std::isfinite(stealth_position.y) &&
			std::isfinite(stealth_position.z) &&
			std::isfinite(stealth_velocity.x) && std::isfinite(stealth_velocity.y) &&
			std::isfinite(stealth_velocity.z)) {
			copy_position(Player_ai->stealth_last_pos,
				output.target.last_stealth_position);
			copy_position(Player_ai->stealth_velocity,
				output.target.last_stealth_velocity);
			output.target.presence |=
				protocol::TargetStatePresenceFlagLastStealthObservation;
		}
		// The optional lead is emitted only when the engine can associate the
		// exact HUD-authoritative result with an installed primary-bank ID.
		(void)collect_default_primary_lead(target, installed_manifest,
			output.target);
			if (target.type == OBJ_SHIP &&
				target.instance >= 0 &&
				target.instance < MAX_SHIPS) {
				const auto& target_ship = Ships[target.instance];
				const auto* installed = installed_ship_class(
					installed_manifest,
					target_ship.ship_info_index);
				// Capture exactly the two strings rendered by
				// HudGaugeTargetBox::renderTargetShipInfo().  Target selection is
				// its own HUD disclosure and does not require a radar contact or a
				// CLASS_MANIFEST entry merely to reproduce visible text.
				if (capture_hud_ship_text(target_ship,
						output.target.revealed_name,
						output.target.hud_type_label) &&
					output.target.hud_type_label.size != 0U) {
					output.target.presence |=
						protocol::TargetStatePresenceFlagRevealedIdentity |
						protocol::TargetStatePresenceFlagHudTypeLabel;
					output.target.revealed_object_type = object_type(target.type);
					if (installed != nullptr) {
						output.target.revealed_class_id =
							installed->class_id;
						output.target.revealed_iff_id = installed->iff_id;
					}
					output.target.revealed_team_id = 0U;
				}
			} else {
				const auto* label = target_box_type_label(target.type);
				if (label != nullptr &&
					output.target.revealed_name.assign(label, std::strlen(label)) &&
					output.target.hud_type_label.assign(label, std::strlen(label))) {
					output.target.presence |=
						protocol::TargetStatePresenceFlagRevealedIdentity |
						protocol::TargetStatePresenceFlagHudTypeLabel;
					output.target.revealed_object_type = object_type(target.type);
				}
			}
		// The Target Box renders the instance-aware, localized HUD subsystem name.
		// Capture it independently from CLASS_MANIFEST so newly spawned ship
		// classes remain reproducible without rebuilding the installed manifest.
		// Stable subsystem IDs remain optional catalog references for consumers of
		// older record versions.
		const auto identity_revealed =
			(output.target.presence &
				protocol::TargetStatePresenceFlagRevealedIdentity) != 0U;
		if (identity_revealed && target.type == OBJ_SHIP &&
			Player_ai->targeted_subsys != nullptr &&
			Player_ai->targeted_subsys_parent == OBJ_INDEX(&target)) {
			if (capture_hud_subsystem_label(target,
					Player_ai->targeted_subsys,
					output.target.hud_target_subsystem_label)) {
				output.target.presence |=
					protocol::TargetStatePresenceFlagHudTargetSubsystemLabel;
			}
			const auto subsystem_id = installed_subsystem_id(
				installed_manifest, target,
				Player_ai->targeted_subsys);
			if (subsystem_id != 0U) {
				output.target.presence |=
					protocol::TargetStatePresenceFlagTargetSubsystem;
				output.target.target_subsystem_id = subsystem_id;
			}
		}
		if (identity_revealed && target.type == OBJ_SHIP &&
			Player->locking_subsys != nullptr &&
			Player->locking_subsys_parent ==
				OBJ_INDEX(&target)) {
			if (capture_hud_subsystem_label(target,
					Player->locking_subsys,
					output.target.hud_lock_subsystem_label)) {
				output.target.presence |=
					protocol::TargetStatePresenceFlagHudLockSubsystemLabel;
			}
			const auto subsystem_id = installed_subsystem_id(
				installed_manifest, target,
				Player->locking_subsys);
			if (subsystem_id != 0U) {
				output.target.presence |=
					protocol::TargetStatePresenceFlagLockSubsystem;
				output.target.lock_subsystem_id = subsystem_id;
			}
		}
		}
	}

	if (Player_ai->previous_target_objnum >= 0 &&
		Player_ai->previous_target_objnum < MAX_OBJECTS) {
		auto& previous = Objects[Player_ai->previous_target_objnum];
		if (valid_live_object(&previous)) {
			RadarContactProjection public_previous;
			if (cockpit_track_visible(previous, public_previous)) {
			const auto identity = resolve_object(identities, previous);
			if (!resolved(identity)) return Phase3EngineCollectStatus::IdentityFailure;
			output.target.presence |=
				protocol::TargetStatePresenceFlagPreviousTarget;
			output.target.previous_target_entity_id = identity.entity_id;
			}
		}
	}

	if (Player_ship->missile_locks.size() > MaximumPhase3Locks) {
		return Phase3EngineCollectStatus::SourceLimitExceeded;
	}
	for (const auto& source : Player_ship->missile_locks) {
		if (!valid_live_object(source.obj)) {
			// Lock records are an optional HUD view.  The engine may retain an
			// object pointer for one tick while a contact despawns.
			continue;
		}
		if (!std::isfinite(source.world_pos.xyz.x) ||
			!std::isfinite(source.world_pos.xyz.y) ||
			!std::isfinite(source.world_pos.xyz.z) ||
			(!source.locked &&
			 (!std::isfinite(source.time_to_lock) ||
			  source.time_to_lock < 0.0F))) {
			continue;
		}
		RadarContactProjection public_lock;
		if (!cockpit_track_visible(*source.obj, public_lock)) {
			continue;
		}
		const auto identity = resolve_object(identities, *source.obj);
		if (!resolved(identity)) return Phase3EngineCollectStatus::IdentityFailure;
		auto& destination = output.locks[output.lock_count++];
		destination.locked = source.locked;
		destination.target_in_lock_cone = source.target_in_lock_cone;
		destination.target_entity_id = identity.entity_id;
		copy_position(source.world_pos, destination.world_position);
		if (source.subsys != nullptr) {
			const auto subsystem_id = installed_subsystem_id(
				installed_manifest, *source.obj, source.subsys);
			// A visible lock does not grant a subsystem identity when the
			// target class is not in the installed manifest.
			if (subsystem_id != 0U) {
				destination.presence |=
					protocol::LockItemPresenceFlagSubsystem;
				destination.subsystem_id = subsystem_id;
			}
		}
		if (!source.locked) {
			destination.presence |= protocol::LockItemPresenceFlagLockAttempt;
			destination.time_to_lock_remaining_us =
				source.time_to_lock > 0.0F && std::isfinite(source.time_to_lock)
				? static_cast<std::uint64_t>(
					static_cast<double>(source.time_to_lock) * 1'000'000.0)
				: 0U;
		}
	}
	std::sort(output.locks.begin(),
		output.locks.begin() + output.lock_count,
		[](const Phase3LockItem& left,
		   const Phase3LockItem& right) {
			if (left.target_entity_id != right.target_entity_id)
				return left.target_entity_id < right.target_entity_id;
			if (left.subsystem_id != right.subsystem_id)
				return left.subsystem_id < right.subsystem_id;
			return left.world_position < right.world_position;
		});
	for (std::size_t index = 1U; index < output.lock_count; ++index) {
		const auto& previous = output.locks[index - 1U];
		const auto& current = output.locks[index];
		if (previous.target_entity_id == current.target_entity_id &&
			previous.subsystem_id == current.subsystem_id &&
			previous.world_position == current.world_position) {
			return Phase3EngineCollectStatus::InvalidSource;
		}
	}
	return Phase3EngineCollectStatus::Collected;
}

Phase3EngineCollectStatus collect_radar(
	std::uint64_t sample_time,
	const Phase2ManifestCandidate* installed_manifest,
	Phase3IdentityRegistry& identities,
	object* current_target,
	int raw_target_objnum,
	Phase3Projection& output) noexcept
{
	output.radar.producer_sample_time_us = sample_time;
	output.radar.selected_range =
		HUD_config.rp_dist >= 0 && HUD_config.rp_dist < RR_MAX_RANGES
		? Radar_ranges[HUD_config.rp_dist]
		: 0.0F;
	output.radar.mode =
		HUD_config.rp_dist == RR_SHORT ? protocol::RadarMode::Short :
		HUD_config.rp_dist == RR_LONG ? protocol::RadarMode::Long :
		HUD_config.rp_dist == RR_INFINITY ? protocol::RadarMode::Infinite :
		protocol::RadarMode::Custom;
	const auto sensor_strength =
		std::clamp(ship_get_subsystem_strength(Player_ship, SUBSYSTEM_SENSORS),
			0.0F, 1.0F);
	output.radar.sensor_current_hits = sensor_strength;
	output.radar.sensor_max_hits = 1.0F;
	output.radar.sensor_state =
		sensor_strength < MIN_SENSOR_STR_TO_RADAR
		? protocol::SensorState::Offline
		: sensor_strength < SENSOR_STR_RADAR_NO_EFFECTS
		? protocol::SensorState::Degraded
		: protocol::SensorState::Online;
	const auto awacs = awacs_observer_telemetry(Player_ship);
	if (awacs.intensity > 0.0F || awacs.range > 0.0F) {
		output.radar.presence |= protocol::RadarStatePresenceFlagAwacs;
		output.radar.awacs_intensity = awacs.intensity;
		output.radar.awacs_range = awacs.range;
	}
	if (Player_ship->flags[Ship::Ship_Flags::Primitive_sensors]) {
		if (Player_ship->primitive_sensor_range < 0) {
			return Phase3EngineCollectStatus::InvalidSource;
		}
		output.radar.presence |=
			protocol::RadarStatePresenceFlagPrimitiveRange;
		output.radar.primitive_range =
			static_cast<float>(Player_ship->primitive_sensor_range);
	}
	if (Player_ship->emp_intensity > 0.0F) {
		if (!std::isfinite(Player_ship->emp_intensity) ||
			!std::isfinite(Player_ship->emp_decr) ||
			Player_ship->emp_decr <= 0.0F) {
			return Phase3EngineCollectStatus::InvalidSource;
		}
		const auto remaining_us = static_cast<double>(Player_ship->emp_intensity) /
			static_cast<double>(Player_ship->emp_decr) * 1'000'000.0;
		if (!std::isfinite(remaining_us) || remaining_us < 0.0 ||
			remaining_us > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
			return Phase3EngineCollectStatus::InvalidSource;
		}
		output.radar.presence |= protocol::RadarStatePresenceFlagEmp;
		output.radar.emp_intensity = Player_ship->emp_intensity;
		output.radar.emp_remaining_us =
			static_cast<std::uint64_t>(remaining_us);
	}
	// The standard radar plots in the cockpit eye frame. Capture that frame
	// alongside the contact projection rather than asking a remote client to
	// combine this contact with a potentially newer FLIGHT_STATE pose.
	vec3d unused_eye_position{};
	matrix eye_orientation{};
	object_get_eye(
		&unused_eye_position, &eye_orientation, Player_obj, false);
	for (auto* source = GET_FIRST(&obj_used_list);
		 source != END_OF_LIST(&obj_used_list);
		 source = GET_NEXT(source)) {
		// The used-object list can retain an object during its spawn/despawn
		// transition.  It has no stable public signature at that point, so it
		// cannot receive a Phase 3 entity ID.  Skip that transient entry rather
		// than failing the entire radar snapshot and stopping telemetry.
		if (!valid_live_object(source)) {
			continue;
		}
		const auto type = object_type(source->type);
		if (type != protocol::ObjectType::Ship &&
			type != protocol::ObjectType::Weapon &&
			type != protocol::ObjectType::JumpNode) {
			continue;
		}
		// The AI may retain an object index for a fraction of a tick after the
		// selected signature has disappeared. If that slot has already been
		// reused, treating the replacement as the current target would disagree
		// with TARGET_STATE and disclose a selection the Target Box no longer
		// owns. Omit only that ambiguous track until the AI reference is coherent.
		if (raw_target_objnum >= 0 && OBJ_INDEX(source) == raw_target_objnum &&
			source != current_target) {
			continue;
		}
		RadarContactProjection projected;
		if (!radar_project_contact(source, projected)) {
			continue;
		}
		// Match radar_plot_object() exactly: the one-second bright-range cache is
		// refreshed only after at least one contact has passed projection. The
		// timer makes subsequent contacts in this tick no-ops.
		radar_refresh_bright_range();
		const auto is_current_target = source == current_target;
		RadarContactVisual visual;
		if (!radar_resolve_contact_visual(
				source, projected, is_current_target, visual)) {
			// A legal spawn/despawn transition may invalidate an instance after
			// projection. Withdraw only that contact and retry next systems tick.
			continue;
		}
		// Telemetry owns a separate bounded contact model.  The HUD display pool
		// is not an authorization or transaction boundary: stopping at MAX_BLIPS
		// would silently publish a partial state image.  Continue through the
		// authoritative object list and fail atomically at the Phase 3 contract
		// limit below instead.
		if (output.contact_count == MaximumPhase3Contacts) {
			return Phase3EngineCollectStatus::SourceLimitExceeded;
		}
		const auto identity = resolve_object(identities, *source);
		if (!resolved(identity)) return Phase3EngineCollectStatus::IdentityFailure;
		auto& contact = output.contacts[output.contact_count++];
		// Contacts live in a fixed preallocated workspace reused between systems
		// captures. Reset the whole DTO before applying this sample: flags such
		// as CurrentTarget are sample state, not a retained track property.
		reconstruct_in_place(contact);
		contact.producer_sample_time_us = sample_time;
		contact.entity_id = identity.entity_id;
		contact.object_type = type;
		contact.category = radar_category(type);
		contact.visibility = radar_visibility(projected.visibility);
		copy_position(projected.world_position, contact.position_world);
		copy_position(projected.world_velocity, contact.velocity_world);
		vec3d radar_relative{};
		vec3d radar_local{};
		// Keep the exact origin used by radar_plot_object(): the standard
		// gauge asks object_get_eye for its orientation, but subtracts the
		// player object origin rather than the returned eye position.
		vm_vec_sub(
			&radar_relative, &projected.world_position, &Player_obj->pos);
		vm_vec_rotate(&radar_local, &radar_relative, &eye_orientation);
		if (!std::isfinite(radar_local.xyz.x) ||
			!std::isfinite(radar_local.xyz.y) ||
			!std::isfinite(radar_local.xyz.z) ||
			!std::isfinite(projected.distance) || projected.distance < 0.0F) {
			// radar_project_contact() may observe a newly spawned object during
			// the tick in which its pose/distance is still being initialized.
			// That is not a permanent capture invariant: withdraw this contact
			// atomically and retry it on the next systems sample.
			--output.contact_count;
			reconstruct_in_place(contact);
			continue;
		}
		copy_position(radar_local, contact.radar_local_position);
		contact.radar_projection_distance = projected.distance;
		contact.radius = std::max(0.0F, source->radius);
		copy_color(*visual.blip_color, contact.radar_blip_color);
		contact.radar_blip_type =
			static_cast<std::uint8_t>(visual.blip_type);
		contact.presence |=
			protocol::RadarContactsPresenceFlagRadarVisual;
		if (visual.bright) {
			contact.flags |= protocol::ContactFlagBright;
		}
		if (is_current_target) {
			contact.flags |= protocol::ContactFlagCurrentTarget;
		}
		switch (visual.blip_type) {
		case BLIP_TYPE_TAGGED_SHIP:
			contact.flags |= protocol::ContactFlagTagged;
			break;
		case BLIP_TYPE_WARPING_SHIP:
			contact.flags |= protocol::ContactFlagWarp;
			break;
		case BLIP_TYPE_BOMB:
			contact.flags |= protocol::ContactFlagBomb;
			break;
		default:
			break;
		}
		// A visible ship is targetable by the same cockpit and therefore exposes
		// exactly the two Target Box strings.  Distorted/remembered tracks and
		// non-ships stay anonymous.  The text is self-contained display data: it
		// never expands CLASS_MANIFEST.  A class ID is added only when that class
		// is already installed for an independent reason.
		if (contact.object_type == protocol::ObjectType::Ship &&
			contact.visibility == protocol::RadarVisibility::Visible &&
			source->instance >= 0 && source->instance < MAX_SHIPS) {
			Phase3OwnedString<255U> hud_name;
			Phase3OwnedString<255U> hud_type_label;
			const auto& source_ship = Ships[source->instance];
			if (capture_hud_ship_text(source_ship, hud_name, hud_type_label)) {
				if (hud_name.size != 0U) {
					contact.revealed_name = hud_name;
					contact.presence |=
						protocol::RadarContactsPresenceFlagRevealedName;
				}
				if (hud_type_label.size != 0U) {
					contact.hud_type_label = hud_type_label;
					contact.presence |=
						protocol::RadarContactsPresenceFlagHudTypeLabel;
				}
				if (const auto* installed = installed_ship_class(
						installed_manifest, source_ship.ship_info_index)) {
					contact.revealed_class_id = installed->class_id;
					contact.presence |=
						protocol::RadarContactsPresenceFlagRevealedClass;
				}
			}
		}
	}
	output.radar.presence |= protocol::RadarStatePresenceFlagBrightRange;
	output.radar.bright_range = std::max(0.0F, Radar_bright_range);
	return Phase3EngineCollectStatus::Collected;
}

Phase3EngineCollectStatus collect_hud_alerts(
	std::uint64_t sample_time,
	Phase3Projection& output) noexcept
{
	HudAlertSnapshot snapshot;
	if (!hud_get_alert_snapshot(snapshot)) {
		return Phase3EngineCollectStatus::InvalidSource;
	}

	auto& alert = output.hud_alert;
	alert.producer_sample_time_us = sample_time;
	alert.primary_fire_threat_active = snapshot.primary_fire_threat_active;
	switch (snapshot.missile_lock_state) {
	case HudMissileLockState::None:
		alert.missile_lock_state = protocol::HudAlertMissileLockState::None;
		break;
	case HudMissileLockState::Attempt:
		alert.missile_lock_state = protocol::HudAlertMissileLockState::Attempt;
		break;
	case HudMissileLockState::Acquired:
		alert.missile_lock_state = protocol::HudAlertMissileLockState::Acquired;
		break;
	default:
		return Phase3EngineCollectStatus::InvalidSource;
	}
	if (!snapshot.warning_active) {
		return Phase3EngineCollectStatus::Collected;
	}
	if (snapshot.warning_instance_id == 0U ||
		snapshot.warning_remaining_us == 0U) {
		return Phase3EngineCollectStatus::InvalidSource;
	}
	switch (snapshot.warning_kind) {
	case HudTextWarningKind::Launch:
		alert.warning_kind = protocol::HudAlertWarningKind::Launch;
		break;
	case HudTextWarningKind::Evaded:
		alert.warning_kind = protocol::HudAlertWarningKind::Evaded;
		break;
	case HudTextWarningKind::Collision:
		alert.warning_kind = protocol::HudAlertWarningKind::Collision;
		break;
	case HudTextWarningKind::Blast:
		alert.warning_kind = protocol::HudAlertWarningKind::Blast;
		break;
	case HudTextWarningKind::EngineWash:
		alert.warning_kind = protocol::HudAlertWarningKind::EngineWash;
		break;
	case HudTextWarningKind::Emp:
		alert.warning_kind = protocol::HudAlertWarningKind::Emp;
		break;
	case HudTextWarningKind::Other:
		alert.warning_kind = protocol::HudAlertWarningKind::Other;
		break;
	default:
		return Phase3EngineCollectStatus::InvalidSource;
	}
	std::size_t text_length = 0U;
	while (text_length < snapshot.warning_text.size() &&
		snapshot.warning_text[text_length] != '\0') {
		++text_length;
	}
	if (text_length == 0U || text_length > 511U ||
		!alert.warning_text.assign(snapshot.warning_text.data(), text_length)) {
		return Phase3EngineCollectStatus::InvalidSource;
	}
	alert.warning_instance_id = snapshot.warning_instance_id;
	alert.warning_remaining_us = snapshot.warning_remaining_us;
	alert.presence |= protocol::HudAlertStatePresenceFlagActiveWarning;
	return Phase3EngineCollectStatus::Collected;
}

Phase3EngineCollectStatus collect_threat(
	std::uint64_t sample_time,
	const Phase2ManifestCandidate* installed_manifest,
	Phase3IdentityRegistry& identities,
	Phase3Projection& output) noexcept
{
	output.threat.producer_sample_time_us = sample_time;
	if ((Player->threat_flags & HudThreatLockFlag) != 0) {
		output.threat.threat_level = protocol::ThreatLevel::LockAcquired;
	} else if ((Player->threat_flags & HudThreatAttemptLockFlag) != 0) {
		output.threat.threat_level = protocol::ThreatLevel::LockAttempt;
	} else if ((Player->threat_flags & HudThreatDumbfireFlag) != 0) {
		output.threat.threat_level = protocol::ThreatLevel::Dumbfire;
	}
	const auto resolve_ai_attacker = [&](int object_index,
		std::uint64_t& entity_id) {
		if (object_index < 0) return true;
		// AI threat indices are retained gameplay hints.  Object creation and
		// destruction can leave one stale for a tick; that must withdraw only
		// this optional reference, never terminate the cockpit telemetry stream.
		if (object_index >= MAX_OBJECTS) return true;
		auto& source = Objects[object_index];
		if (!valid_live_object(&source)) return true;
		RadarContactProjection public_attacker;
		if (!cockpit_track_visible(source, public_attacker)) {
			// The AI may retain an attacker after the cockpit has lost the
			// corresponding track.  That history is not an authorization to
			// allocate or expose a public sensor identity.
			return true;
		}
		const auto identity = resolve_object(identities, source);
		if (!resolved(identity)) return false;
		entity_id = identity.entity_id;
		return true;
	};
	const auto resolve_ai_weapon = [&](int object_index, int signature,
									 std::uint64_t& entity_id) {
		if (object_index < 0) return true;
		if (object_index >= MAX_OBJECTS) return true;
		auto& source = Objects[object_index];
		if (!valid_live_object(&source) || source.type != OBJ_WEAPON ||
			(signature > 0 && source.signature != signature)) {
			return true;
		}
		RadarContactProjection public_weapon;
		if (!cockpit_track_visible(source, public_weapon)) {
			return true;
		}
		const auto identity = resolve_object(identities, source);
		if (!resolved(identity)) return false;
		entity_id = identity.entity_id;
		return true;
	};
	if (!resolve_ai_attacker(Player_ai->attacker_objnum,
			output.threat.nearest_attacker_entity_id) ||
		!resolve_ai_weapon(Player_ai->danger_weapon_objnum,
			Player_ai->danger_weapon_signature,
			output.threat.dangerous_weapon_entity_id) ||
		!resolve_ai_weapon(Player_ai->nearest_locked_object, -1,
			output.threat.nearest_homing_entity_id)) {
		return Phase3EngineCollectStatus::InvalidSource;
	}
	if (output.threat.nearest_attacker_entity_id != 0U) {
		output.threat.presence |=
			protocol::ThreatStatePresenceFlagNearestAttacker;
	}
	if (output.threat.dangerous_weapon_entity_id != 0U) {
		output.threat.presence |=
			protocol::ThreatStatePresenceFlagDangerousWeapon;
	}
	if (output.threat.nearest_homing_entity_id != 0U) {
		output.threat.presence |=
			protocol::ThreatStatePresenceFlagNearestHoming;
	}

	std::size_t incoming_count = 0U;
	const auto is_incoming = [&](const object& missile_object,
								 const weapon& missile) noexcept {
		return missile.homing_object == Player_obj ||
			(Player_ai->danger_weapon_objnum ==
				 missile.objnum &&
			 (Player_ai->danger_weapon_signature <= 0 ||
			  Player_ai->danger_weapon_signature ==
				  missile_object.signature));
	};
	for (auto* item = GET_FIRST(&Missile_obj_list);
		 item != END_OF_LIST(&Missile_obj_list);
		 item = GET_NEXT(item)) {
		if (item->objnum < 0 || item->objnum >= MAX_OBJECTS) {
			// Missile list nodes can straddle object allocation/release.  They do
			// not describe a publishable threat until the referenced object is
			// coherent, so omit that node for this sample.
			continue;
		}
		auto& missile_object = Objects[item->objnum];
		if (!valid_live_object(&missile_object) ||
			missile_object.type != OBJ_WEAPON ||
			missile_object.instance < 0 ||
			missile_object.instance >= MAX_WEAPONS) {
			continue;
		}
		auto& missile = Weapons[missile_object.instance];
		if (!is_incoming(missile_object, missile)) continue;
		if (++incoming_count > MaximumPhase3IncomingMissiles) {
			return Phase3EngineCollectStatus::SourceLimitExceeded;
		}
	}
	for (auto* item = GET_FIRST(&Missile_obj_list);
		 item != END_OF_LIST(&Missile_obj_list);
		 item = GET_NEXT(item)) {
		if (item->objnum < 0 || item->objnum >= MAX_OBJECTS) continue;
		auto& missile_object = Objects[item->objnum];
		if (!valid_live_object(&missile_object) ||
			missile_object.type != OBJ_WEAPON ||
			missile_object.instance < 0 ||
			missile_object.instance >= MAX_WEAPONS) continue;
		auto& missile = Weapons[missile_object.instance];
		if (!is_incoming(missile_object, missile)) continue;
		if (missile.weapon_info_index < 0 ||
			missile.weapon_info_index >= weapon_info_size()) {
			continue;
		}
		const auto weapon_class_id = installed_weapon_class_id(
			installed_manifest, missile.weapon_info_index);
		if (weapon_class_id == 0U) {
			// Never expose a dynamic reference before its manifest
			// definition is installed and APPLIED.  This is an omission, not
			// a fatal telemetry error: enemy ordnance may legitimately be
			// outside the player's Phase 2 catalog.
			continue;
		}
		const auto identity = resolve_object(identities, missile_object);
		if (!resolved(identity))
			return Phase3EngineCollectStatus::IdentityFailure;
		auto& destination = output.threat.incoming_missiles[
			output.threat.incoming_missile_count++];
		destination.entity_id = identity.entity_id;
		destination.weapon_class_id = weapon_class_id;
		destination.guidance_type =
			guidance_type(Weapon_info[missile.weapon_info_index]);
		RadarContactProjection radar_projection;
		destination.radar_visibility =
			radar_project_contact(&missile_object, radar_projection)
			? radar_visibility(radar_projection.visibility)
			: protocol::RadarVisibility::NotVisible;
		copy_position(missile_object.pos, destination.position_world);
		copy_position(
			missile_object.phys_info.vel, destination.velocity_world);
		const CaptureOrientationBasis basis{
			{missile_object.orient.vec.rvec.xyz.x,
			 missile_object.orient.vec.rvec.xyz.y,
			 missile_object.orient.vec.rvec.xyz.z},
			{missile_object.orient.vec.uvec.xyz.x,
			 missile_object.orient.vec.uvec.xyz.y,
			 missile_object.orient.vec.uvec.xyz.z},
			{missile_object.orient.vec.fvec.xyz.x,
			 missile_object.orient.vec.fvec.xyz.y,
			 missile_object.orient.vec.fvec.xyz.z}};
		CaptureQuaternionf quaternion;
		if (convert_fso_orientation_to_local_to_world(
				basis, quaternion) !=
			QuaternionConversionStatus::Converted) {
			// A partially initialized orientation is not an incoming-missile
			// observation.  Withdraw only this optional item for the tick.
			--output.threat.incoming_missile_count;
			continue;
		}
		destination.orientation_local_to_world = {
			quaternion.w, quaternion.x, quaternion.y, quaternion.z};
	}
	std::sort(output.threat.incoming_missiles.begin(),
		output.threat.incoming_missiles.begin() +
			output.threat.incoming_missile_count,
		[](const Phase3IncomingMissile& left,
		   const Phase3IncomingMissile& right) {
			return left.entity_id < right.entity_id;
		});
	return Phase3EngineCollectStatus::Collected;
}

Phase3EngineCollectStatus collect_cargo(
	const Phase3EngineCollectInput& input,
	Phase3IdentityRegistry& identities,
	Phase3Projection& output) noexcept
{
	const auto& source = input.phase2_observation->player_cargo_scan;
	output.cargo.producer_sample_time_us = input.producer_sample_time_us;
	output.cargo.scan_phase = scan_phase(source.phase);
	if ((source.presence & protocol::CargoScanStatePresenceFlagTarget) != 0U) {
		if (Player_ai == nullptr || Player_ai->target_objnum < 0 ||
			Player_ai->target_objnum >= MAX_OBJECTS) {
			return Phase3EngineCollectStatus::Collected;
		}
		auto& target = Objects[Player_ai->target_objnum];
		if (!valid_live_object(&target) || target.type != OBJ_SHIP ||
			static_cast<std::uint32_t>(target.signature) !=
			source.target_capture_key.value) {
			// The scan/target pair can straddle a target switch.  Do not emit
			// stale cargo data and do not end the session for that transition.
			return Phase3EngineCollectStatus::Collected;
		}
		// A cargo scan is its own cockpit authorization.  The scanned ship may
		// deliberately be outside the CompleteShip closure, so it cannot be
		// looked up only among reconciled Phase 2 bindings.  Allocate the public
		// sensor identity after validating the authoritative scan/target pair;
		// this publishes only the cargo reference, never the ship's full state.
		const auto identity = resolve_object(identities, target);
		if (!resolved(identity)) return Phase3EngineCollectStatus::IdentityFailure;
		output.cargo.presence |= protocol::CargoScanStatePresenceFlagTarget;
		output.cargo.target_entity_id = identity.entity_id;
		if ((source.presence &
				protocol::CargoScanStatePresenceFlagSubsystem) != 0U) {
			const auto subsystem_id =
				installed_subsystem_id_from_source_key(
					input.installed_manifest, target,
					source.target_subsystem_source_key);
			if (subsystem_id != 0U) {
				output.cargo.presence |=
					protocol::CargoScanStatePresenceFlagSubsystem;
				output.cargo.target_subsystem_id = subsystem_id;
			}
		}
	} else if ((source.presence &
				 protocol::CargoScanStatePresenceFlagSubsystem) != 0U) {
		return Phase3EngineCollectStatus::Collected;
	}
	if ((source.presence & protocol::CargoScanStatePresenceFlagTiming) != 0U) {
		output.cargo.presence |= protocol::CargoScanStatePresenceFlagTiming;
		output.cargo.elapsed_us = source.elapsed_us;
		output.cargo.required_us = source.required_us;
	}
	if ((source.presence & protocol::CargoScanStatePresenceFlagValidity) != 0U) {
		if (source.validity_flags > 0xffU) {
			return Phase3EngineCollectStatus::InvalidSource;
		}
		output.cargo.presence |= protocol::CargoScanStatePresenceFlagValidity;
		output.cargo.validity_flags =
			static_cast<std::uint8_t>(source.validity_flags);
	}
	if ((source.presence & protocol::CargoScanStatePresenceFlagCargoText) != 0U) {
		if (!output.cargo.cargo_text.assign(
				source.cargo_text.data(), source.cargo_text.size())) {
			return Phase3EngineCollectStatus::SourceLimitExceeded;
		}
		output.cargo.presence |= protocol::CargoScanStatePresenceFlagCargoText;
		output.cargo.disclosure =
			static_cast<std::uint8_t>(protocol::DisclosureState::Revealed);
	}
	return Phase3EngineCollectStatus::Collected;
}

Phase3EngineCollectStatus collect_navigation(
	std::uint64_t sample_time,
	Phase3IdentityRegistry& identities,
	Phase3Projection& output) noexcept
{
	output.navigation.producer_sample_time_us = sample_time;
	if (CurrentNav < -1 || CurrentNav >= MAX_NAVPOINTS) {
		return Phase3EngineCollectStatus::InvalidSource;
	}
	const auto availability = EvaluateAutopilot(&Player_obj->pos);
	if (AutoPilotEngaged) {
		output.navigation.autopilot_state =
			protocol::AutopilotState::Engaged;
	} else if (CurrentNav < 0) {
		output.navigation.autopilot_state =
			protocol::AutopilotState::Refused;
	} else if (availability == AutopilotAvailability::Available) {
		output.navigation.autopilot_state =
			protocol::AutopilotState::Available;
	} else {
		output.navigation.autopilot_state =
			protocol::AutopilotState::Refused;
		output.navigation.presence |=
			protocol::NavigationStatePresenceFlagAutopilotRefusal;
		output.navigation.autopilot_refusal =
			autopilot_refusal(availability);
	}
	if (CurrentNav < 0) {
		output.navigation.presence |=
			protocol::NavigationStatePresenceFlagAutopilotRefusal;
		output.navigation.autopilot_refusal =
			protocol::AutopilotRefusal::NoValidNav;
	}
	for (std::size_t index = 0U; index < MAX_NAVPOINTS; ++index) {
		auto& source = Navs[index];
		if ((source.flags & NP_VALIDTYPE) == 0 ||
			(source.flags & NP_HIDDEN) != 0) {
			continue;
		}
		const auto* position = source.GetPosition();
		if (position == nullptr) return Phase3EngineCollectStatus::InvalidSource;
		auto& navpoint =
			output.navigation.navpoints[output.navigation.navpoint_count++];
		navpoint.type = static_cast<std::uint8_t>(
			(source.flags & NP_SHIP) != 0
			? protocol::NavPointType::Entity
			: protocol::NavPointType::Waypoint);
		navpoint.flags =
			(source.flags & NP_NOACCESS) != 0
				? protocol::NavPointFlagNoAccess : 0U;
		if ((source.flags & NP_VISITED) != 0)
			navpoint.flags |= protocol::NavPointFlagVisited;
		copy_position(*position, navpoint.position_world);
		std::size_t name_length = 0U;
		while (name_length < sizeof(source.m_NavName) &&
			source.m_NavName[name_length] != '\0') {
			++name_length;
		}
		if (name_length == sizeof(source.m_NavName) ||
			!navpoint.name.assign(source.m_NavName, name_length)) {
			return Phase3EngineCollectStatus::SourceLimitExceeded;
		}
		navpoint.navpoint_id = resolve_navpoint_id(output,
			source.m_NavName, name_length, navpoint.type);
		if (navpoint.navpoint_id == 0U)
			return Phase3EngineCollectStatus::SourceLimitExceeded;
		if ((source.flags & NP_SHIP) != 0 &&
			source.target_index >= 0 && source.target_index < MAX_OBJECTS) {
			auto& linked = Objects[source.target_index];
			RadarContactProjection projected;
			if (valid_live_object(&linked) &&
				radar_project_contact(&linked, projected)) {
				const auto identity = resolve_object(identities, linked);
				if (!resolved(identity)) {
					return Phase3EngineCollectStatus::IdentityFailure;
				}
				navpoint.presence |=
					protocol::NavPointPresenceFlagEntityLink;
				navpoint.linked_entity_id = identity.entity_id;
			}
		}
		if ((source.flags & NP_WAYPOINT) != 0) {
			if (source.target_index < 0 ||
				static_cast<std::size_t>(source.target_index) >=
					Waypoint_lists.size() ||
				source.waypoint_num < 0) {
				return Phase3EngineCollectStatus::InvalidSource;
			}
			const auto& list = Waypoint_lists[source.target_index];
			if (static_cast<std::size_t>(source.waypoint_num) >=
				list.get_waypoints().size()) {
				return Phase3EngineCollectStatus::InvalidSource;
			}
			navpoint.presence |=
				protocol::NavPointPresenceFlagWaypointLink;
			navpoint.waypoint_list_id =
				static_cast<std::uint32_t>(source.target_index) + 1U;
			navpoint.waypoint_index =
				static_cast<std::uint16_t>(source.waypoint_num);
			if (CurrentNav == static_cast<int>(index)) {
				const auto remaining =
					list.get_waypoints().size() -
					static_cast<std::size_t>(source.waypoint_num);
				if (remaining > MaximumPhase3RouteWaypoints) {
					return Phase3EngineCollectStatus::
						SourceLimitExceeded;
				}
				for (std::size_t waypoint =
						 static_cast<std::size_t>(
							 source.waypoint_num);
					 waypoint < list.get_waypoints().size();
					 ++waypoint) {
					const auto* route_position =
						list.get_waypoints()[waypoint].get_pos();
					if (route_position == nullptr) {
						return Phase3EngineCollectStatus::
							InvalidSource;
					}
					auto& route =
						output.navigation.route_waypoints[
							output.navigation
								.route_waypoint_count++];
					route.waypoint_list_id =
						navpoint.waypoint_list_id;
					route.waypoint_index =
						static_cast<std::uint16_t>(waypoint);
					copy_position(*route_position,
						route.position_world);
				}
				output.navigation.presence |=
					protocol::
						NavigationStatePresenceFlagWaypointRoute;
				output.navigation.current_route_index = 0U;
				if (Player_ship->ai_index >= 0 &&
					Player_ship->ai_index < MAX_AI_INFO) {
					output.navigation.route_speed_limit =
						std::max(0.0F,
							static_cast<float>(
								Ai_info[Player_ship->ai_index]
									.waypoint_speed_cap));
				}
			}
		}
		if (CurrentNav == static_cast<int>(index)) {
			output.navigation.presence |=
				protocol::NavigationStatePresenceFlagCurrentNavpoint;
			output.navigation.current_navpoint_id = navpoint.navpoint_id;
		}
	}
	return Phase3EngineCollectStatus::Collected;
}

} // namespace

Phase3EngineCollectStatus discover_phase3_catalog_dependencies(
	const Phase2ObservationDto& observation,
	Phase3CatalogDependencies& output) noexcept
{
	output = {};
	if (!phase2_current_thread_is_main())
		return Phase3EngineCollectStatus::NotMainThread;
	if ((Game_mode & GM_IN_MISSION) == 0 || Player_obj == nullptr ||
		Player_ship == nullptr || Player_ai == nullptr)
		return Phase3EngineCollectStatus::NoPlayer;
	const auto append = [](auto& keys, std::uint32_t& count,
		std::uint32_t key) noexcept {
		if (key == 0U) return true;
		for (std::uint32_t index = 0U; index < count; ++index)
			if (keys[index] == key) return true;
		if (count >= keys.size()) return false;
		keys[count++] = key;
		return true;
	};
	// TARGET_STATE is independent of radar visibility.  This is deliberately
	// the Target Box's live-object gate, not radar_project_contact(). Selecting
	// a ship here ensures the next manifest is installed before
	// collect_target_and_locks can publish its class identity, even when that
	// ship has no radar blip.
	if (auto* current_target = current_player_target()) {
		auto& target = *current_target;
		if (target.type == OBJ_SHIP &&
			target.instance >= 0 && target.instance < MAX_SHIPS &&
			valid_live_object(&target)) {
			const auto class_index = Ships[target.instance].ship_info_index;
			if (class_index >= 0 &&
				class_index < static_cast<int>(Ship_info.size()) &&
				!append(output.ship_class_source_keys,
					output.ship_class_count,
					static_cast<std::uint32_t>(class_index) + 1U)) {
				return Phase3EngineCollectStatus::SourceLimitExceeded;
			}
		}
	}
	// Radar HUD labels are self-contained and never create manifest
	// dependencies. A visible blip may reuse an already-installed class id, but
	// it must not expand the manifest merely to decorate the radar list.
	const auto& cargo = observation.player_cargo_scan;
	if ((cargo.presence & protocol::CargoScanStatePresenceFlagSubsystem) != 0U) {
		if ((cargo.presence & protocol::CargoScanStatePresenceFlagTarget) != 0U &&
			Player_ai->target_objnum >= 0 &&
			Player_ai->target_objnum < MAX_OBJECTS) {
			const auto& target = Objects[Player_ai->target_objnum];
			if (valid_live_object(&target) && target.type == OBJ_SHIP &&
				target.instance >= 0 && target.instance < MAX_SHIPS &&
				static_cast<std::uint32_t>(target.signature) ==
					cargo.target_capture_key.value) {
				const auto class_index = Ships[target.instance].ship_info_index;
				if (class_index >= 0 &&
					class_index < static_cast<int>(Ship_info.size()) &&
					!append(output.ship_class_source_keys,
						output.ship_class_count,
						static_cast<std::uint32_t>(class_index) + 1U)) {
					return Phase3EngineCollectStatus::SourceLimitExceeded;
				}
			}
		}
	}
	for (auto* item = GET_FIRST(&Missile_obj_list);
		 item != END_OF_LIST(&Missile_obj_list);
		 item = GET_NEXT(item)) {
		if (item->objnum < 0 || item->objnum >= MAX_OBJECTS)
			continue;
		const auto& missile_object = Objects[item->objnum];
		if (!valid_live_object(&missile_object) ||
			missile_object.type != OBJ_WEAPON ||
			missile_object.instance < 0 || missile_object.instance >= MAX_WEAPONS)
			continue;
		const auto& missile = Weapons[missile_object.instance];
		const auto incoming = missile.homing_object == Player_obj ||
			(Player_ai->danger_weapon_objnum == missile.objnum &&
				(Player_ai->danger_weapon_signature <= 0 ||
				 Player_ai->danger_weapon_signature == missile_object.signature));
		if (!incoming) continue;
		if (missile.weapon_info_index < 0 ||
			missile.weapon_info_index >= weapon_info_size())
			continue;
		if (!append(output.weapon_source_keys, output.weapon_count,
			static_cast<std::uint32_t>(missile.weapon_info_index) + 1U))
			return Phase3EngineCollectStatus::SourceLimitExceeded;
	}
	return Phase3EngineCollectStatus::Collected;
}

Phase3EngineCollectStatus collect_phase3_engine_projection(
	const Phase3EngineCollectInput& input,
	Phase3IdentityRegistry& identities,
	Phase3Projection& output,
	Phase3Projection& scratch,
	Phase3EngineCollectDiagnostic* diagnostic) noexcept
{
	if (diagnostic != nullptr) *diagnostic = {};
	const auto reject_before_transaction = [diagnostic](
		Phase3EngineCollectBlock block,
		Phase3EngineCollectStatus failure) noexcept {
		if (diagnostic != nullptr) {
			diagnostic->block = block;
			diagnostic->status = failure;
		}
		return failure;
	};
	if (!phase2_current_thread_is_main()) {
		return reject_before_transaction(
			Phase3EngineCollectBlock::Precondition,
			Phase3EngineCollectStatus::NotMainThread);
	}
	if ((Game_mode & GM_IN_MISSION) == 0 || Player == nullptr ||
		Player_obj == nullptr || Player_ship == nullptr) {
		return reject_before_transaction(
			Phase3EngineCollectBlock::Precondition,
			Phase3EngineCollectStatus::NoPlayer);
	}
	if (!identities.ready() || input.player_entity_id == 0U ||
		&output == &scratch ||
		Player_obj->type != OBJ_SHIP || Player_obj->signature <= 0 ||
		Player_ship->objnum < 0 || Player_ship->objnum >= MAX_OBJECTS ||
		&Objects[Player_ship->objnum] != Player_obj) {
		return reject_before_transaction(
			Phase3EngineCollectBlock::Precondition,
			Phase3EngineCollectStatus::InvalidSource);
	}
	if (!identities.begin_transaction()) {
		return reject_before_transaction(
			Phase3EngineCollectBlock::Precondition,
			Phase3EngineCollectStatus::InvalidSource);
	}
	scratch = output;
	const auto reject = [&identities, diagnostic](
		Phase3EngineCollectBlock block,
		Phase3EngineCollectStatus failure) noexcept {
		identities.rollback_transaction();
		if (diagnostic != nullptr) {
			diagnostic->block = block;
			diagnostic->status = failure;
		}
		return failure;
	};
	auto status = reconcile_phase2(input, identities);
	if (status != Phase3EngineCollectStatus::Collected)
		return reject(Phase3EngineCollectBlock::Precondition, status);
	const auto identity_changed =
		scratch.player_entity_id != input.player_entity_id;
	const auto refresh_flight =
		input.refresh_flight_controls || identity_changed;
	const auto refresh_systems =
		input.refresh_systems || identity_changed;
	// Resolve the mutable Target Box reference exactly once for this projection.
	// When both cadence families fire, TARGET_STATE and RADAR_CONTACTS therefore
	// observe the same validated index/signature/instance tuple. When only one
	// family fires, the other retains its own older producer sample time.
	auto* current_target = current_player_target();
	const auto raw_target_objnum =
		Player_ai != nullptr ? Player_ai->target_objnum : -1;
	if (identity_changed) reconstruct_in_place(scratch);
	scratch.player_entity_id = input.player_entity_id;
	if (refresh_flight) {
		reconstruct_in_place(scratch.target);
		reconstruct_in_place(scratch.hud_alert);
		scratch.lock_count = 0U;
		status = collect_target_and_locks(
			input.producer_sample_time_us,
			input.installed_manifest, identities, current_target, scratch);
		if (status != Phase3EngineCollectStatus::Collected)
			return reject(Phase3EngineCollectBlock::TargetLocks, status);
		status = collect_hud_alerts(input.producer_sample_time_us, scratch);
		if (status != Phase3EngineCollectStatus::Collected)
			return reject(Phase3EngineCollectBlock::HudAlerts, status);
	}
	if (refresh_systems) {
		reconstruct_in_place(scratch.radar);
		scratch.contact_count = 0U;
		reconstruct_in_place(scratch.threat);
		reconstruct_in_place(scratch.cargo);
		reconstruct_in_place(scratch.navigation);
		status = collect_radar(input.producer_sample_time_us,
			input.installed_manifest, identities, current_target,
			raw_target_objnum, scratch);
		if (status != Phase3EngineCollectStatus::Collected)
			return reject(Phase3EngineCollectBlock::Radar, status);
		status = collect_threat(input.producer_sample_time_us,
			input.installed_manifest, identities, scratch);
		if (status != Phase3EngineCollectStatus::Collected)
			return reject(Phase3EngineCollectBlock::Threat, status);
		status = collect_cargo(input, identities, scratch);
		if (status != Phase3EngineCollectStatus::Collected)
			return reject(Phase3EngineCollectBlock::Cargo, status);
		status = collect_navigation(
			input.producer_sample_time_us, identities, scratch);
		if (status != Phase3EngineCollectStatus::Collected)
			return reject(Phase3EngineCollectBlock::Navigation, status);
	}
	output = scratch;
	identities.commit_transaction();
	return Phase3EngineCollectStatus::Collected;
}

} // namespace telemetry::detail
