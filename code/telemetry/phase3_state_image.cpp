#include "telemetry/phase3_state_image.h"

#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_records.h"

#include <algorithm>
#include <array>
#include <new>
#include <vector>

namespace telemetry {
namespace {

using protocol::MutableByteView;
using protocol::PacketWriter;
using protocol::RecordType;
using protocol::StateAtom;

constexpr auto LockPayloadCapacity = CockpitSensorsLockPayloadCapacity;
constexpr auto TargetPayloadCapacity = CockpitSensorsTargetPayloadCapacity;
constexpr auto RadarPayloadCapacity = CockpitSensorsRadarPayloadCapacity;
constexpr auto ContactPayloadCapacity = CockpitSensorsContactPayloadCapacity;
constexpr auto ThreatPayloadCapacity = CockpitSensorsThreatPayloadCapacity;
constexpr auto HudAlertPayloadCapacity = CockpitSensorsHudAlertPayloadCapacity;
constexpr auto CargoPayloadCapacity = CockpitSensorsCargoPayloadCapacity;
constexpr auto NavigationPayloadCapacity =
	CockpitSensorsNavigationPayloadCapacity;

static_assert(Phase3CockpitSensorsCoverage == 0x07cbULL);
static_assert(Phase3StateDerivedEventCoverage == 0x001dULL);

bool write_identity(PacketWriter& writer, std::uint64_t entity_id)
{
	return writer.write_u64(entity_id);
}

bool assign_payload(PacketWriter& writer, StateAtom& atom)
{
	if (!writer.ok()) return false;
	const auto bytes = writer.written();
	atom.value.assign(bytes.data, bytes.data + bytes.size);
	return true;
}

void reset_atom(StateAtom& atom) noexcept
{
	atom.key.record_type = 0U;
	atom.key.identity.clear();
	atom.record_version = 1U;
	atom.lifecycle = protocol::StateRecordLifecycle::UpsertOnly;
	atom.has_cascade_owner = false;
	atom.cascade_owner.record_type = 0U;
	atom.cascade_owner.identity.clear();
	atom.value.clear();
}

bool set_entity_key(StateAtom& atom, RecordType type, std::uint64_t entity_id)
{
	std::array<std::uint8_t, 8U> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(entity_id)) return false;
	atom.key.record_type = static_cast<std::uint16_t>(type);
	atom.key.identity.assign(bytes.begin(), bytes.end());
	atom.has_cascade_owner = true;
	atom.cascade_owner.record_type =
		static_cast<std::uint16_t>(RecordType::EntityLifecycle);
	atom.cascade_owner.identity = atom.key.identity;
	return true;
}

bool set_contact_key(StateAtom& atom,
	std::uint64_t observer_id,
	std::uint64_t contact_id)
{
	std::array<std::uint8_t, 16U> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(observer_id) || !writer.write_u64(contact_id))
		return false;
	atom.key.record_type =
		static_cast<std::uint16_t>(RecordType::RadarContacts);
	atom.key.identity.assign(bytes.begin(), bytes.end());
	atom.lifecycle = protocol::StateRecordLifecycle::ExplicitCreateDelete;
	atom.has_cascade_owner = true;
	atom.cascade_owner.record_type =
		static_cast<std::uint16_t>(RecordType::EntityLifecycle);
	atom.cascade_owner.identity.assign(bytes.begin(), bytes.begin() + 8U);
	return true;
}

bool validate_encoded(StateAtom& atom)
{
	protocol::BusinessRecordMetadata metadata;
	protocol::RecordEnvelopeView envelope;
	envelope.raw_record_type = atom.key.record_type;
	envelope.record_version = atom.record_version;
	envelope.record_flags = protocol::RecordFlagNone;
	envelope.payload = {atom.value.empty() ? nullptr : atom.value.data(),
		atom.value.size()};
	return protocol::validate_business_record(envelope,
			   protocol::BusinessRecordContainer::FullSnapshot,
			   protocol::VersionMinor, metadata) ==
		protocol::ValidationError::None;
}

bool replace_session(StateAtom& atom,
	const Phase3Projection& projection)
{
	if (atom.key.record_type !=
			static_cast<std::uint16_t>(RecordType::SessionState) ||
		(atom.value.size() != 64U && atom.value.size() != 72U))
		return false;
	std::array<std::uint8_t, 72U> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	const auto* old = atom.value.data();
	const auto read_u32 = [](const std::uint8_t* value) {
		return static_cast<std::uint32_t>(value[0]) |
			(static_cast<std::uint32_t>(value[1]) << 8U) |
			(static_cast<std::uint32_t>(value[2]) << 16U) |
			(static_cast<std::uint32_t>(value[3]) << 24U);
	};
	const auto read_u64 = [](const std::uint8_t* value) {
		std::uint64_t result = 0U;
		for (std::size_t index = 0U; index < 8U; ++index)
			result |= static_cast<std::uint64_t>(value[index])
				<< (index * 8U);
		return result;
	};
	const auto presence = projection.player_entity_id == 0U
		? protocol::SessionStatePresenceFlagNone
		: protocol::SessionStatePresenceFlagObservedPlayer;
	if (!writer.write_u64(presence) ||
		!writer.write_u64(read_u64(old + 8U)) ||
		!writer.write_u64(read_u64(old + 16U)) ||
		!writer.write_u8(old[24U]) || !writer.write_u8(old[25U]) ||
		!writer.write_u8(old[26U]) || !writer.write_u8(0U) ||
		!writer.write_u32(read_u32(old + 28U)) ||
		!writer.write_u64(read_u64(old + 32U)) ||
		!writer.write_u64(Phase3CockpitSensorsCoverage) ||
		!writer.write_u64(Phase3StateDerivedEventCoverage) ||
		!writer.write_u64(0U) ||
		(projection.player_entity_id != 0U &&
			!writer.write_u64(projection.player_entity_id)))
		return false;
	return assign_payload(writer, atom) && validate_encoded(atom);
}

bool make_locks(const Phase3Projection& source, StateAtom& atom)
{
	reset_atom(atom);
	std::array<std::uint8_t, LockPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!write_identity(writer, source.player_entity_id) ||
		!writer.write_u64(protocol::LockStatePresenceFlagNone) ||
		!writer.write_u64(source.target.producer_sample_time_us) ||
		!writer.write_u16(static_cast<std::uint16_t>(source.lock_count)))
		return false;
	for (std::size_t index = 0U; index < source.lock_count; ++index) {
		const auto& item = source.locks[index];
		const auto item_size = 2U + 1U + 1U + 8U +
			((item.presence & protocol::LockItemPresenceFlagSubsystem) != 0U
					? 4U
					: 0U) +
			12U +
			((item.presence & protocol::LockItemPresenceFlagLockAttempt) != 0U
					? 8U
					: 0U);
		if (!writer.write_u8(1U) ||
			!writer.write_u16(static_cast<std::uint16_t>(item_size)) ||
			!writer.write_u16(item.presence) ||
			!writer.write_bool8(item.locked) ||
			!writer.write_bool8(item.target_in_lock_cone) ||
			!writer.write_u64(item.target_entity_id) ||
			((item.presence & protocol::LockItemPresenceFlagSubsystem) != 0U &&
				!writer.write_u32(item.subsystem_id)) ||
			!writer.write_f32(item.world_position[0]) ||
			!writer.write_f32(item.world_position[1]) ||
			!writer.write_f32(item.world_position[2]) ||
			((item.presence & protocol::LockItemPresenceFlagLockAttempt) != 0U &&
				!writer.write_u64(item.time_to_lock_remaining_us)))
			return false;
	}
	return set_entity_key(atom, RecordType::LockState, source.player_entity_id) &&
		assign_payload(writer, atom) && validate_encoded(atom);
}

bool make_target(const Phase3Projection& source, StateAtom& atom)
{
	reset_atom(atom);
	const auto& target = source.target;
	constexpr std::uint64_t SubsystemPresenceFlags =
		protocol::TargetStatePresenceFlagTargetSubsystem |
		protocol::TargetStatePresenceFlagLockSubsystem |
		protocol::TargetStatePresenceFlagHudTargetSubsystemLabel |
		protocol::TargetStatePresenceFlagHudLockSubsystemLabel;
	// Subsystem IDs are an identity disclosure.  Reject any intermediate
	// projection that attempts to expose one without the revealed target group.
	if ((target.presence & SubsystemPresenceFlags) != 0U &&
		(target.presence & protocol::TargetStatePresenceFlagRevealedIdentity) == 0U)
		return false;
	std::array<std::uint8_t, TargetPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(source.player_entity_id) ||
		!writer.write_u64(target.presence) ||
		!writer.write_u64(target.producer_sample_time_us) ||
		!writer.write_u64(target.current_target_entity_id) ||
		((target.presence & protocol::TargetStatePresenceFlagPreviousTarget) != 0U &&
			!writer.write_u64(target.previous_target_entity_id)) ||
		((target.presence & protocol::TargetStatePresenceFlagRevealedIdentity) != 0U &&
			(!writer.write_u8(static_cast<std::uint8_t>(target.revealed_object_type)) ||
			 !writer.write_utf8({target.revealed_name.bytes.data(),
				 target.revealed_name.size}, 255U) ||
			 !writer.write_u32(target.revealed_class_id) ||
			 !writer.write_u32(target.revealed_team_id) ||
			 !writer.write_u32(target.revealed_iff_id))) ||
		((target.presence & protocol::TargetStatePresenceFlagTimeOnTarget) != 0U &&
			!writer.write_u64(target.time_on_target_us)) ||
		((target.presence & protocol::TargetStatePresenceFlagTargetSubsystem) != 0U &&
			!writer.write_u32(target.target_subsystem_id)) ||
		((target.presence & protocol::TargetStatePresenceFlagLockSubsystem) != 0U &&
			!writer.write_u32(target.lock_subsystem_id)))
		return false;
	const auto vec3 = [&writer](const std::array<float, 3U>& value) {
		return writer.write_f32(value[0]) && writer.write_f32(value[1]) &&
			writer.write_f32(value[2]);
	};
	if (((target.presence & protocol::TargetStatePresenceFlagLastStealthObservation) != 0U &&
			(!vec3(target.last_stealth_position) ||
			 !vec3(target.last_stealth_velocity))) ||
		((target.presence & protocol::TargetStatePresenceFlagDistanceTrend) != 0U &&
			!writer.write_u8(target.distance_trend)) ||
		((target.presence & protocol::TargetStatePresenceFlagSpeedTrend) != 0U &&
			!writer.write_u8(target.speed_trend)) ||
		((target.presence & protocol::TargetStatePresenceFlagInCone) != 0U &&
			!writer.write_bool8(target.in_cone)) ||
		((target.presence & protocol::TargetStatePresenceFlagLead) != 0U &&
			(!vec3(target.lead_world) ||
			 !writer.write_u32(target.lead_bank_id))) ||
		((target.presence & protocol::TargetStatePresenceFlagAttacker) != 0U &&
			!writer.write_u64(target.attacker_entity_id)) ||
		((target.presence & protocol::TargetStatePresenceFlagDangerousWeapon) != 0U &&
			!writer.write_u64(target.dangerous_weapon_entity_id)) ||
		((target.presence & protocol::TargetStatePresenceFlagNearestLocked) != 0U &&
			!writer.write_u64(target.nearest_locked_entity_id)) ||
		((target.presence & protocol::TargetStatePresenceFlagExactHudDistance) != 0U &&
			!writer.write_f32(target.exact_hud_distance)) ||
		((target.presence & protocol::TargetStatePresenceFlagExactHudSpeed) != 0U &&
			!writer.write_f32(target.exact_hud_speed)) ||
		((target.presence & protocol::TargetStatePresenceFlagHudTypeLabel) != 0U &&
			!writer.write_utf8({target.hud_type_label.bytes.data(),
				target.hud_type_label.size}, 255U)) ||
		((target.presence & protocol::TargetStatePresenceFlagHudTargetColor) != 0U &&
			(!writer.write_u8(target.hud_target_color[0]) ||
			 !writer.write_u8(target.hud_target_color[1]) ||
			 !writer.write_u8(target.hud_target_color[2]) ||
			 !writer.write_u8(target.hud_target_color[3]))) ||
		((target.presence & protocol::TargetStatePresenceFlagHudTargetSubsystemLabel) != 0U &&
			!writer.write_utf8({target.hud_target_subsystem_label.bytes.data(),
				target.hud_target_subsystem_label.size}, 255U)) ||
		((target.presence & protocol::TargetStatePresenceFlagHudLockSubsystemLabel) != 0U &&
			!writer.write_utf8({target.hud_lock_subsystem_label.bytes.data(),
				 target.hud_lock_subsystem_label.size}, 255U)) ||
		((target.presence & protocol::TargetStatePresenceFlagHudTargetStrength) != 0U &&
			(!writer.write_f32(target.hud_hull_ratio) ||
			 !writer.write_bool8(target.hud_has_shields) ||
			 (target.hud_has_shields && !writer.write_f32(target.hud_shield_ratio)))))
		return false;
	if (!set_entity_key(atom, RecordType::TargetState, source.player_entity_id) ||
		!assign_payload(writer, atom)) {
		return false;
	}
	atom.record_version = 6U;
	return validate_encoded(atom);
}

bool make_radar(const Phase3Projection& source, StateAtom& atom)
{
	reset_atom(atom);
	const auto& radar = source.radar;
	std::array<std::uint8_t, RadarPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(source.player_entity_id) ||
		!writer.write_u64(radar.presence) ||
		!writer.write_u64(radar.producer_sample_time_us) ||
		!writer.write_u8(static_cast<std::uint8_t>(radar.mode)) ||
		!writer.write_f32(radar.selected_range) ||
		!writer.write_u8(static_cast<std::uint8_t>(radar.sensor_state)) ||
		!writer.write_f32(radar.sensor_current_hits) ||
		!writer.write_f32(radar.sensor_max_hits) ||
		((radar.presence & protocol::RadarStatePresenceFlagBrightRange) != 0U &&
			!writer.write_f32(radar.bright_range)) ||
		((radar.presence & protocol::RadarStatePresenceFlagPrimitiveRange) != 0U &&
			!writer.write_f32(radar.primitive_range)) ||
		((radar.presence & protocol::RadarStatePresenceFlagAwacs) != 0U &&
			(!writer.write_f32(radar.awacs_intensity) ||
			 !writer.write_f32(radar.awacs_range))) ||
		((radar.presence & protocol::RadarStatePresenceFlagEmp) != 0U &&
			(!writer.write_f32(radar.emp_intensity) ||
			 !writer.write_u64(radar.emp_remaining_us))) ||
		((radar.presence & protocol::RadarStatePresenceFlagJamming) != 0U &&
			(!writer.write_f32(radar.jamming_intensity) ||
			 !writer.write_f32(radar.distortion_intensity))) ||
		((radar.presence & protocol::RadarStatePresenceFlagVisibilityTimes) != 0U &&
			(!writer.write_u64(radar.first_visible_time_us) ||
			 !writer.write_u64(radar.last_contact_time_us))))
		return false;
	return set_entity_key(atom, RecordType::RadarState, source.player_entity_id) &&
		assign_payload(writer, atom) && validate_encoded(atom);
}

bool make_contact(const Phase3Projection& source,
	const Phase3RadarContact& contact,
	StateAtom& atom)
{
	reset_atom(atom);
	constexpr std::uint64_t RevealedIdentityFlags =
		protocol::RadarContactsPresenceFlagRevealedName |
		protocol::RadarContactsPresenceFlagRevealedClass |
		protocol::RadarContactsPresenceFlagRevealedTeamIff |
		protocol::RadarContactsPresenceFlagHudTypeLabel;
	// A distorted track is an authorized sensor observation, not an identity
	// disclosure.  Keep this check at the serialization boundary as well as in
	// the engine collector so an invalid intermediate projection cannot reveal
	// a hidden contact through a later code path.
	if (contact.visibility != protocol::RadarVisibility::Visible &&
		(contact.presence & RevealedIdentityFlags) != 0U) {
		return false;
	}
	std::array<std::uint8_t, ContactPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	const auto vec3 = [&writer](const std::array<float, 3U>& value) {
		return writer.write_f32(value[0]) && writer.write_f32(value[1]) &&
			writer.write_f32(value[2]);
	};
	if (!writer.write_u64(source.player_entity_id) ||
		!writer.write_u64(contact.entity_id) ||
		!writer.write_u64(contact.presence) ||
		!writer.write_u64(contact.producer_sample_time_us) ||
		!writer.write_u8(static_cast<std::uint8_t>(contact.object_type)) ||
		!writer.write_u8(contact.category) ||
		!writer.write_u8(static_cast<std::uint8_t>(contact.visibility)) ||
		!vec3(contact.position_world) || !vec3(contact.velocity_world) ||
		!vec3(contact.radar_local_position) ||
		!writer.write_f32(contact.radar_projection_distance) ||
		!writer.write_f32(contact.radius) ||
		!writer.write_u32(contact.flags) ||
		((contact.presence & protocol::RadarContactsPresenceFlagIconSize) != 0U &&
			!writer.write_f32(contact.icon_size)) ||
		((contact.presence & protocol::RadarContactsPresenceFlagRevealedName) != 0U &&
			!writer.write_utf8({contact.revealed_name.bytes.data(),
				contact.revealed_name.size}, 255U)) ||
		((contact.presence & protocol::RadarContactsPresenceFlagRevealedClass) != 0U &&
			!writer.write_u32(contact.revealed_class_id)) ||
		((contact.presence & protocol::RadarContactsPresenceFlagRevealedTeamIff) != 0U &&
			(!writer.write_u32(contact.revealed_team_id) ||
			 !writer.write_u32(contact.revealed_iff_id))) ||
		((contact.presence & protocol::RadarContactsPresenceFlagDetectionTimes) != 0U &&
			(!writer.write_u64(contact.first_detection_time_us) ||
			 !writer.write_u64(contact.last_detection_time_us))) ||
		((contact.presence & protocol::RadarContactsPresenceFlagConfidence) != 0U &&
			!writer.write_f32(contact.confidence)) ||
		((contact.presence & protocol::RadarContactsPresenceFlagHudTypeLabel) != 0U &&
			!writer.write_utf8({contact.hud_type_label.bytes.data(),
				contact.hud_type_label.size}, 255U)) ||
		((contact.presence & protocol::RadarContactsPresenceFlagRadarVisual) != 0U &&
			(!writer.write_u8(contact.radar_blip_color[0]) ||
			 !writer.write_u8(contact.radar_blip_color[1]) ||
			 !writer.write_u8(contact.radar_blip_color[2]) ||
			 !writer.write_u8(contact.radar_blip_color[3]) ||
			 !writer.write_u8(contact.radar_blip_type))))
		return false;
	if (!set_contact_key(atom, source.player_entity_id, contact.entity_id))
		return false;
	atom.record_version = 4U;
	return assign_payload(writer, atom) && validate_encoded(atom);
}

bool make_threat(const Phase3Projection& source, StateAtom& atom)
{
	reset_atom(atom);
	const auto& threat = source.threat;
	std::array<std::uint8_t, ThreatPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(source.player_entity_id) ||
		!writer.write_u64(threat.presence) ||
		!writer.write_u64(threat.producer_sample_time_us) ||
		!writer.write_u8(static_cast<std::uint8_t>(threat.threat_level)) ||
		((threat.presence & protocol::ThreatStatePresenceFlagNearestAttacker) != 0U &&
			!writer.write_u64(threat.nearest_attacker_entity_id)) ||
		((threat.presence & protocol::ThreatStatePresenceFlagDangerousWeapon) != 0U &&
			!writer.write_u64(threat.dangerous_weapon_entity_id)) ||
		((threat.presence & protocol::ThreatStatePresenceFlagNearestHoming) != 0U &&
			!writer.write_u64(threat.nearest_homing_entity_id)) ||
		!writer.write_u16(static_cast<std::uint16_t>(
			threat.incoming_missile_count)))
		return false;
	for (std::size_t index = 0U;
		 index < threat.incoming_missile_count; ++index) {
		const auto& missile = threat.incoming_missiles[index];
		const auto item_size = 2U + 1U + 1U + 8U + 4U + 8U +
			((missile.presence &
				 protocol::IncomingMissilePresenceFlagHomingSubsystem) != 0U
					? 4U
					: 0U) +
			12U + 16U + 12U;
		if (!writer.write_u8(1U) ||
			!writer.write_u16(static_cast<std::uint16_t>(item_size)) ||
			!writer.write_u16(missile.presence) ||
			!writer.write_u8(missile.guidance_type) ||
			!writer.write_u8(static_cast<std::uint8_t>(
				missile.radar_visibility)) ||
			!writer.write_u64(missile.entity_id) ||
			!writer.write_u32(missile.weapon_class_id) ||
			!writer.write_u64(source.player_entity_id) ||
			((missile.presence &
				 protocol::IncomingMissilePresenceFlagHomingSubsystem) != 0U &&
				!writer.write_u32(missile.homing_subsystem_id)))
			return false;
		for (const auto value : missile.position_world)
			if (!writer.write_f32(value)) return false;
		for (const auto value : missile.orientation_local_to_world)
			if (!writer.write_f32(value)) return false;
		for (const auto value : missile.velocity_world)
			if (!writer.write_f32(value)) return false;
	}
	return set_entity_key(atom, RecordType::ThreatState, source.player_entity_id) &&
		assign_payload(writer, atom) && validate_encoded(atom);
}

bool make_hud_alert(const Phase3Projection& source, StateAtom& atom)
{
	reset_atom(atom);
	const auto& alert = source.hud_alert;
	std::array<std::uint8_t, HudAlertPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(source.player_entity_id) ||
		!writer.write_u64(alert.presence) ||
		!writer.write_u64(alert.producer_sample_time_us) ||
		!writer.write_bool8(alert.primary_fire_threat_active) ||
		!writer.write_u8(static_cast<std::uint8_t>(alert.missile_lock_state)) ||
		((alert.presence &
			 protocol::HudAlertStatePresenceFlagActiveWarning) != 0U &&
			(!writer.write_u8(static_cast<std::uint8_t>(alert.warning_kind)) ||
			 !writer.write_u64(alert.warning_instance_id) ||
			 !writer.write_u64(alert.warning_remaining_us) ||
			 !writer.write_utf8({alert.warning_text.bytes.data(),
				 alert.warning_text.size}, 511U)))) {
		return false;
	}
	return set_entity_key(atom, RecordType::HudAlertState,
			   source.player_entity_id) &&
		assign_payload(writer, atom) && validate_encoded(atom);
}

bool make_cargo(const Phase3Projection& source, StateAtom& atom)
{
	reset_atom(atom);
	const auto& cargo = source.cargo;
	std::array<std::uint8_t, CargoPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(source.player_entity_id) ||
		!writer.write_u64(cargo.presence) ||
		!writer.write_u64(cargo.producer_sample_time_us) ||
		!writer.write_u8(cargo.scan_phase) ||
		!writer.write_u8(cargo.disclosure) ||
		((cargo.presence & protocol::CargoScanStatePresenceFlagTarget) != 0U &&
			!writer.write_u64(cargo.target_entity_id)) ||
		((cargo.presence & protocol::CargoScanStatePresenceFlagSubsystem) != 0U &&
			!writer.write_u32(cargo.target_subsystem_id)) ||
		((cargo.presence & protocol::CargoScanStatePresenceFlagTiming) != 0U &&
			(!writer.write_u64(cargo.elapsed_us) ||
			 !writer.write_u64(cargo.required_us))) ||
		((cargo.presence & protocol::CargoScanStatePresenceFlagValidity) != 0U &&
			!writer.write_u8(cargo.validity_flags)) ||
		((cargo.presence & protocol::CargoScanStatePresenceFlagCargoText) != 0U &&
			!writer.write_utf8({cargo.cargo_text.bytes.data(),
				cargo.cargo_text.size}, 511U)))
		return false;
	return set_entity_key(atom, RecordType::CargoScanState,
			   source.player_entity_id) &&
		assign_payload(writer, atom) && validate_encoded(atom);
}

bool make_navigation(const Phase3Projection& source, StateAtom& atom)
{
	reset_atom(atom);
	const auto& navigation = source.navigation;
	try {
		// Preallocated live-image atoms retain this capacity between captures.
		// The allocating convenience builder reaches the same code, but the
		// steady-state path therefore needs no separate 512-KiB scratch block.
		atom.value.resize(NavigationPayloadCapacity);
	} catch (const std::bad_alloc&) {
		return false;
	}
	PacketWriter writer({atom.value.data(), atom.value.size()});
	if (!writer.write_u64(source.player_entity_id) ||
		!writer.write_u64(navigation.presence) ||
		!writer.write_u64(navigation.producer_sample_time_us) ||
		!writer.write_u8(static_cast<std::uint8_t>(
			navigation.autopilot_state)) ||
		!writer.write_u16(static_cast<std::uint16_t>(
			navigation.navpoint_count)))
		return false;
	for (std::size_t index = 0U; index < navigation.navpoint_count; ++index) {
		const auto& navpoint = navigation.navpoints[index];
		const auto item_size = 2U + 1U + 1U + 4U + 2U +
			navpoint.name.size + 12U +
			((navpoint.presence &
				 protocol::NavPointPresenceFlagEntityLink) != 0U
					? 8U
					: 0U) +
			((navpoint.presence &
				 protocol::NavPointPresenceFlagWaypointLink) != 0U
					? 6U
					: 0U);
		if (!writer.write_u8(1U) ||
			!writer.write_u16(static_cast<std::uint16_t>(item_size)) ||
			!writer.write_u16(navpoint.presence) ||
			!writer.write_u8(navpoint.type) ||
			!writer.write_u8(navpoint.flags) ||
			!writer.write_u32(navpoint.navpoint_id) ||
			!writer.write_utf8({navpoint.name.bytes.data(),
				navpoint.name.size}, 255U))
			return false;
		for (const auto value : navpoint.position_world)
			if (!writer.write_f32(value)) return false;
		if ((navpoint.presence &
				protocol::NavPointPresenceFlagEntityLink) != 0U &&
			!writer.write_u64(navpoint.linked_entity_id))
			return false;
		if ((navpoint.presence &
				protocol::NavPointPresenceFlagWaypointLink) != 0U &&
			(!writer.write_u32(navpoint.waypoint_list_id) ||
			 !writer.write_u16(navpoint.waypoint_index)))
			return false;
	}
	if (((navigation.presence &
			 protocol::NavigationStatePresenceFlagCurrentNavpoint) != 0U &&
			!writer.write_u32(navigation.current_navpoint_id)) ||
		((navigation.presence &
			 protocol::NavigationStatePresenceFlagAutopilotRefusal) != 0U &&
			!writer.write_u8(static_cast<std::uint8_t>(
				navigation.autopilot_refusal))))
		return false;
	if ((navigation.presence &
			protocol::NavigationStatePresenceFlagWaypointRoute) != 0U) {
		if (!writer.write_u16(static_cast<std::uint16_t>(
				navigation.route_waypoint_count)) ||
			!writer.write_u8(1U) || !writer.write_u16(20U))
			return false;
		for (std::size_t index = 0U;
			 index < navigation.route_waypoint_count; ++index) {
			const auto& waypoint = navigation.route_waypoints[index];
			if (!writer.write_u32(waypoint.waypoint_list_id) ||
				!writer.write_u16(waypoint.waypoint_index) ||
				!writer.write_u16(0U))
				return false;
			for (const auto value : waypoint.position_world)
				if (!writer.write_f32(value)) return false;
		}
		if (!writer.write_u16(navigation.current_route_index) ||
			!writer.write_f32(navigation.route_speed_limit))
			return false;
	}
	if (!writer.ok()) return false;
	atom.value.resize(writer.written().size);
	return set_entity_key(atom, RecordType::NavigationState,
			   source.player_entity_id) &&
		validate_encoded(atom);
}

} // namespace

bool CockpitSensorsStateImagePool::provision(
	std::size_t maximum_subjects,
	std::size_t maximum_subsystems,
	std::size_t maximum_dock_relations,
	std::size_t maximum_support_latches) noexcept
{
	constexpr std::size_t AdditionalRecords =
		6U + MaximumPhase3Contacts;
	if (!m_complete.provision(maximum_subjects, maximum_subsystems,
			maximum_dock_relations, maximum_support_latches,
			AdditionalRecords))
		return false;
	const auto base_records =
		4U + 10U * maximum_subjects + maximum_subsystems;
	constexpr std::array<std::size_t, 6U> FixedPayloadCapacities{{
		LockPayloadCapacity, TargetPayloadCapacity,
		RadarPayloadCapacity, ThreatPayloadCapacity,
		HudAlertPayloadCapacity, NavigationPayloadCapacity}};
	try {
		for (auto& slot : m_complete.m_slots) {
			if (!slot.records || slot.records->size() <
					base_records + AdditionalRecords) {
				reset();
				return false;
			}
			// The complete-domain cargo slot is overwritten by the filtered
			// cockpit cargo record and therefore needs the final payload capacity.
			(*slot.records)[3U].value.reserve(CargoPayloadCapacity);
			for (std::size_t index = 0U;
				 index < AdditionalRecords; ++index) {
				auto& atom = (*slot.records)[base_records + index];
				atom.key.identity.reserve(index < 6U ? 8U : 16U);
				atom.cascade_owner.identity.reserve(8U);
				atom.value.reserve(index < FixedPayloadCapacities.size()
					? FixedPayloadCapacities[index]
					: ContactPayloadCapacity);
			}
		}
		return true;
	} catch (const std::bad_alloc&) {
		reset();
		return false;
	}
}

void CockpitSensorsStateImagePool::reset() noexcept
{
	m_complete.reset();
}

bool CockpitSensorsStateImagePool::append_preallocated_records(
	protocol::StateImage& image, std::size_t count) noexcept
{
	auto* records = image.mutable_preallocated_records_if_unique();
	if (records == nullptr) return false;
	for (auto& slot : m_complete.m_slots) {
		if (!slot.records || slot.records.get() != records ||
			slot.spare_count < count ||
			records->size() + count > records->capacity())
			continue;
		for (std::size_t index = 0U; index < count; ++index) {
			records->emplace_back();
			std::swap(records->back(),
				slot.spares[--slot.spare_count]);
		}
		return true;
	}
	return false;
}

Phase3StateImageBuildStatus build_cockpit_sensors_state_image_preallocated(
	const Phase2CompleteDomainInput& input,
	CockpitSensorsStateImagePool& pool,
	const Phase3Projection& projection,
	protocol::StateImage& output,
	Phase2StateImageBuildDiagnostic* diagnostic,
	Phase2StateImageRebuildSet* rebuilt) noexcept
{
	if (!pool.ready() ||
		projection.lock_count > MaximumPhase3Locks ||
		projection.contact_count > MaximumPhase3Contacts ||
		projection.threat.incoming_missile_count >
			MaximumPhase3IncomingMissiles ||
		projection.navigation.navpoint_count > MaximumPhase3Navpoints ||
		projection.navigation.route_waypoint_count >
			MaximumPhase3RouteWaypoints)
		return Phase3StateImageBuildStatus::InvalidInput;
	Phase2StateImageRebuildSet base_rebuilt{};
	const auto base_status = build_phase2_complete_domain_preallocated(
		input, pool.m_complete, output, diagnostic, &base_rebuilt);
	if (base_status == Phase2StateImageBuildStatus::AllocationFailed)
		return Phase3StateImageBuildStatus::AllocationFailed;
	if (base_status == Phase2StateImageBuildStatus::CapacityExceeded)
		return Phase3StateImageBuildStatus::CapacityExceeded;
	if (base_status != Phase2StateImageBuildStatus::Created)
		return Phase3StateImageBuildStatus::InvalidInput;
	auto* records = output.mutable_preallocated_records_if_unique();
	if (records == nullptr || records->empty())
		return Phase3StateImageBuildStatus::CapacityExceeded;
	auto session = std::find_if(records->begin(), records->end(),
		[](const StateAtom& atom) {
			return atom.key.record_type ==
				static_cast<std::uint16_t>(RecordType::SessionState);
		});
	if (session == records->end() || !replace_session(*session, projection))
		return Phase3StateImageBuildStatus::EncodingFailed;
	if (projection.player_entity_id != 0U) {
		auto cargo = std::find_if(records->begin(), records->end(),
			[](const StateAtom& atom) {
				return atom.key.record_type ==
					static_cast<std::uint16_t>(RecordType::CargoScanState);
			});
		const auto extra = 6U + projection.contact_count;
		if (cargo == records->end() ||
			!pool.append_preallocated_records(output, extra))
			return Phase3StateImageBuildStatus::CapacityExceeded;
		records = output.mutable_preallocated_records_if_unique();
		if (records == nullptr) return Phase3StateImageBuildStatus::CapacityExceeded;
		cargo = std::find_if(records->begin(), records->end(),
			[](const StateAtom& atom) {
				return atom.key.record_type ==
					static_cast<std::uint16_t>(RecordType::CargoScanState);
			});
		const auto start = records->size() - extra;
		if (cargo == records->end() || !make_cargo(projection, *cargo) ||
			!make_locks(projection, (*records)[start]) ||
			!make_target(projection, (*records)[start + 1U]) ||
			!make_radar(projection, (*records)[start + 2U]) ||
			!make_threat(projection, (*records)[start + 3U]) ||
			!make_hud_alert(projection, (*records)[start + 4U]) ||
			!make_navigation(projection, (*records)[start + 5U]))
			return Phase3StateImageBuildStatus::EncodingFailed;
		for (std::size_t index = 0U;
			 index < projection.contact_count; ++index)
			if (!make_contact(projection, projection.contacts[index],
					(*records)[start + 6U + index]))
				return Phase3StateImageBuildStatus::EncodingFailed;
	}
	std::sort(records->begin(), records->end(),
		[](const StateAtom& left, const StateAtom& right) noexcept {
			return left.key < right.key;
		});
	protocol::StateImageInvalidRecordReason reason{};
	if (output.refresh_preallocated_metadata(reason) !=
		protocol::StateImageResult::Created)
		return Phase3StateImageBuildStatus::EncodingFailed;
	if (rebuilt != nullptr) {
		if (output.records().size() > rebuilt->canonical_indices.size())
			return Phase3StateImageBuildStatus::CapacityExceeded;
		rebuilt->count = output.records().size();
		for (std::size_t index = 0U; index < rebuilt->count; ++index)
			rebuilt->canonical_indices[index] =
				static_cast<std::uint16_t>(index);
		rebuilt->patch_applied = false;
		rebuilt->exhaustive = true;
	}
	return Phase3StateImageBuildStatus::Created;
}

bool patch_cockpit_sensors_state_image_preallocated(
	protocol::StateImage& current,
	protocol::StateImage& candidate,
	Phase2StateImageRebuildSet& rebuilt) noexcept
{
	rebuilt = {};
	auto* current_records = current.mutable_preallocated_records_if_unique();
	auto* candidate_records =
		candidate.mutable_preallocated_records_if_unique();
	if (current_records == nullptr || candidate_records == nullptr ||
		current_records->size() != candidate_records->size() ||
		current_records->size() > rebuilt.canonical_indices.size())
		return false;
	for (std::size_t index = 0U; index < current_records->size(); ++index)
		if ((*current_records)[index].key != (*candidate_records)[index].key)
			return false;
	for (std::size_t index = 0U; index < current_records->size(); ++index) {
		if ((*current_records)[index] == (*candidate_records)[index])
			continue;
		rebuilt.canonical_indices[rebuilt.count++] =
			static_cast<std::uint16_t>(index);
		std::swap((*current_records)[index], (*candidate_records)[index]);
	}
	protocol::StateImageInvalidRecordReason reason{};
	if (current.refresh_preallocated_metadata(reason) !=
		protocol::StateImageResult::Created) {
		for (std::size_t dirty = 0U; dirty < rebuilt.count; ++dirty) {
			const auto index = rebuilt.canonical_indices[dirty];
			std::swap((*current_records)[index], (*candidate_records)[index]);
		}
		rebuilt = {};
		return false;
	}
	rebuilt.patch_applied = true;
	rebuilt.exhaustive = false;
	return true;
}

bool rollback_cockpit_sensors_state_image_preallocated(
	protocol::StateImage& current,
	protocol::StateImage& candidate,
	const Phase2StateImageRebuildSet& rebuilt) noexcept
{
	if (!rebuilt.patch_applied)
		return false;
	auto* current_records = current.mutable_preallocated_records_if_unique();
	auto* candidate_records =
		candidate.mutable_preallocated_records_if_unique();
	if (current_records == nullptr || candidate_records == nullptr ||
		current_records->size() != candidate_records->size())
		return false;
	for (std::size_t dirty = 0U; dirty < rebuilt.count; ++dirty) {
		const auto index = rebuilt.canonical_indices[dirty];
		if (index >= current_records->size())
			return false;
		std::swap((*current_records)[index], (*candidate_records)[index]);
	}
	protocol::StateImageInvalidRecordReason reason{};
	return current.refresh_preallocated_metadata(reason) ==
		protocol::StateImageResult::Created;
}

Phase3StateImageBuildStatus build_phase3_cockpit_sensor_state_image(
	const protocol::StateImage& complete_ship,
	const Phase3Projection& projection,
	protocol::StateImage& output) noexcept
{
	if (complete_ship.empty() ||
		projection.lock_count > MaximumPhase3Locks ||
		projection.contact_count > MaximumPhase3Contacts ||
		projection.threat.incoming_missile_count >
			MaximumPhase3IncomingMissiles ||
		projection.navigation.navpoint_count > MaximumPhase3Navpoints ||
		projection.navigation.route_waypoint_count >
			MaximumPhase3RouteWaypoints)
		return Phase3StateImageBuildStatus::InvalidInput;
	if (projection.player_entity_id == 0U) {
		if (complete_ship.records().size() != 2U)
			return Phase3StateImageBuildStatus::InvalidInput;
	}
	try {
		auto records = complete_ship.records();
		auto session = std::find_if(records.begin(), records.end(),
			[](const StateAtom& atom) {
				return atom.key.record_type ==
					static_cast<std::uint16_t>(RecordType::SessionState);
			});
		if (session == records.end() || !replace_session(*session, projection))
			return Phase3StateImageBuildStatus::EncodingFailed;
		if (projection.player_entity_id != 0U) {
			records.erase(std::remove_if(records.begin(), records.end(),
				[&](const StateAtom& atom) {
					return atom.key.record_type ==
						static_cast<std::uint16_t>(
							RecordType::CargoScanState);
				}), records.end());
			const auto start = records.size();
			records.resize(start + 7U + projection.contact_count);
			if (!make_locks(projection, records[start]) ||
				!make_target(projection, records[start + 1U]) ||
				!make_radar(projection, records[start + 2U]) ||
				!make_threat(projection, records[start + 3U]) ||
				!make_hud_alert(projection, records[start + 4U]) ||
				!make_cargo(projection, records[start + 5U]) ||
				!make_navigation(projection, records[start + 6U]))
				return Phase3StateImageBuildStatus::EncodingFailed;
			for (std::size_t index = 0U;
				 index < projection.contact_count; ++index)
				if (!make_contact(projection, projection.contacts[index],
						records[start + 7U + index]))
					return Phase3StateImageBuildStatus::EncodingFailed;
		}
		protocol::StateImage candidate;
		protocol::StateImageInvalidRecordReason reason{};
		const auto status =
			protocol::StateImage::create(std::move(records), candidate, reason);
		if (status == protocol::StateImageResult::AllocationFailed)
			return Phase3StateImageBuildStatus::AllocationFailed;
		if (status != protocol::StateImageResult::Created)
			return Phase3StateImageBuildStatus::EncodingFailed;
		output = std::move(candidate);
		return Phase3StateImageBuildStatus::Created;
	} catch (const std::bad_alloc&) {
		return Phase3StateImageBuildStatus::AllocationFailed;
	}
}

} // namespace telemetry
