#include "telemetry/cockpit_sensors_state_image.h"

#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_business_state_validation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace telemetry {
namespace {

using protocol::MutableByteView;
using protocol::PacketWriter;
using protocol::RecordType;
using protocol::StateAtom;
using protocol::StateAtomKey;
using detail::MaximumPhase2ShieldSegments;
using detail::MaximumPhase2SubsystemsPerShip;
using detail::Phase2CaptureReason;
using detail::Phase2CaptureStatus;
using detail::Phase2ObservationDto;
using detail::ShipLifecycleState;
using detail::ShipObservationDto;
using detail::ShipSubsystemKind;
using detail::ShipSubsystemObservation;

enum class CockpitCommonBuildStatus : std::uint8_t {
	Created = 0,
	InvalidInput,
	ManifestUnavailable,
	SourceMappingMissing,
	CapacityExceeded,
	AllocationFailed,
	Count,
};

struct CommonRecordInput {
	std::uint64_t producer_id = 0U;
	std::uint32_t negotiated_capability_generation = 0U;
	protocol::SessionPhase session_phase =
		protocol::SessionPhase::Synchronizing;
	detail::MissionObservationDto mission{};
	std::uint64_t player_entity_id = 0U;
	const detail::Phase2ObservationDto* observation = nullptr;
	const Phase2ManifestCandidate* installed_manifest = nullptr;
	std::uint32_t required_manifest_id = 0U;
	bool manifest_applied = false;
};

struct WeaponRecordInput {
	const detail::Phase2ObservationDto* observation = nullptr;
	const Phase2ManifestCandidate* installed_manifest = nullptr;
};

constexpr std::size_t SessionPayloadCapacity = 72U;
constexpr std::size_t MissionPayloadCapacity = 28U;
constexpr std::size_t LifecyclePayloadCapacity = 40U;
constexpr std::size_t IdentityPayloadCapacity = 704U;
constexpr std::size_t FlightPayloadCapacity = 84U;
constexpr std::size_t DamagePayloadCapacity = 48U;
constexpr std::size_t ShieldPayloadCapacity = 560U;
constexpr std::size_t SubsystemPayloadCapacity = 512U;
constexpr std::size_t EnergyPayloadCapacity = 96U;
constexpr std::size_t PropulsionPayloadCapacity = 112U;
constexpr std::size_t ControlPayloadCapacity = 96U;
constexpr std::size_t WeaponPayloadCapacity = 9216U;

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

void swap_atom_backing(StateAtom& left, StateAtom& right) noexcept
{
	std::swap(left.key.record_type, right.key.record_type);
	left.key.identity.swap(right.key.identity);
	std::swap(left.record_version, right.record_version);
	std::swap(left.lifecycle, right.lifecycle);
	std::swap(left.has_cascade_owner, right.has_cascade_owner);
	std::swap(
		left.cascade_owner.record_type, right.cascade_owner.record_type);
	left.cascade_owner.identity.swap(right.cascade_owner.identity);
	left.value.swap(right.value);
}

void sort_atoms(std::vector<StateAtom>& atoms) noexcept
{
	const auto sift_down = [&atoms](std::size_t root,
									std::size_t count) noexcept {
		for (;;) {
			const auto left = root * 2U + 1U;
			if (left >= count) return;
			auto largest = root;
			if (atoms[largest].key < atoms[left].key) largest = left;
			const auto right = left + 1U;
			if (right < count &&
				atoms[largest].key < atoms[right].key) {
				largest = right;
			}
			if (largest == root) return;
			swap_atom_backing(atoms[root], atoms[largest]);
			root = largest;
		}
	};
	for (auto start = atoms.size() / 2U; start > 0U; --start) {
		sift_down(start - 1U, atoms.size());
	}
	for (auto end = atoms.size(); end > 1U; --end) {
		swap_atom_backing(atoms[0], atoms[end - 1U]);
		sift_down(0U, end - 1U);
	}
}

bool assign_payload(const PacketWriter& writer, StateAtom& atom)
{
	if (!writer.ok()) {
		return false;
	}
	const auto bytes = writer.written();
	atom.value.assign(bytes.data, bytes.data + bytes.size);
	return true;
}

bool write_entity_identity(std::uint64_t entity_id, StateAtomKey& key)
{
	std::array<std::uint8_t, 8U> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(entity_id)) {
		return false;
	}
	key.identity.assign(bytes.begin(), bytes.end());
	return true;
}

bool write_subsystem_identity(std::uint64_t entity_id, std::uint32_t subsystem_id, StateAtomKey& key)
{
	std::array<std::uint8_t, 12U> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(entity_id) || !writer.write_u32(subsystem_id)) {
		return false;
	}
	key.identity.assign(bytes.begin(), bytes.end());
	return true;
}

void set_entity_atom(StateAtom& atom,
	RecordType type,
	std::uint64_t entity_id,
	const StateAtomKey& lifecycle_key)
{
	atom.key.record_type = static_cast<std::uint16_t>(type);
	atom.has_cascade_owner = true;
	atom.cascade_owner = lifecycle_key;
	(void)write_entity_identity(entity_id, atom.key);
}

bool normalize_entity_cascade_owner(StateAtom& atom) noexcept
{
	const auto type = static_cast<RecordType>(atom.key.record_type);
	if (type < RecordType::ShipIdentity || type > RecordType::EffectState)
		return true;
	if (atom.value.size() < sizeof(std::uint64_t) ||
		atom.cascade_owner.identity.capacity() < sizeof(std::uint64_t))
		return false;
	atom.has_cascade_owner = true;
	atom.cascade_owner.record_type =
		static_cast<std::uint16_t>(RecordType::EntityLifecycle);
	atom.cascade_owner.identity.resize(sizeof(std::uint64_t));
	std::copy_n(atom.value.begin(), sizeof(std::uint64_t),
		atom.cascade_owner.identity.begin());
	return true;
}

protocol::LifecyclePhase lifecycle_phase(ShipLifecycleState state) noexcept
{
	switch (state) {
	case ShipLifecycleState::Spawning:
		return protocol::LifecyclePhase::Spawning;
	case ShipLifecycleState::Departing:
	case ShipLifecycleState::Departed:
		return protocol::LifecyclePhase::Departing;
	case ShipLifecycleState::Dying:
		return protocol::LifecyclePhase::Dying;
	case ShipLifecycleState::Destroyed:
		return protocol::LifecyclePhase::Destroyed;
	case ShipLifecycleState::Removed:
	case ShipLifecycleState::Vanished:
		return protocol::LifecyclePhase::Removed;
	case ShipLifecycleState::Present:
	case ShipLifecycleState::Disabled:
	default:
		return protocol::LifecyclePhase::Active;
	}
}

protocol::SubsystemType subsystem_type(ShipSubsystemKind kind) noexcept
{
	switch (kind) {
	case ShipSubsystemKind::Other: return protocol::SubsystemType::Other;
	case ShipSubsystemKind::Engine: return protocol::SubsystemType::Engine;
	case ShipSubsystemKind::Weapon: return protocol::SubsystemType::Weapons;
	case ShipSubsystemKind::Turret: return protocol::SubsystemType::Turret;
	case ShipSubsystemKind::Radar: return protocol::SubsystemType::Radar;
	case ShipSubsystemKind::Sensors: return protocol::SubsystemType::Sensors;
	case ShipSubsystemKind::Communication: return protocol::SubsystemType::Communication;
	case ShipSubsystemKind::Navigation: return protocol::SubsystemType::Navigation;
	case ShipSubsystemKind::Reactor: return protocol::SubsystemType::Reactor;
	case ShipSubsystemKind::Generic:
	default: return protocol::SubsystemType::Unknown;
	}
}

const Phase2BankRecord* find_turret_bank(const Phase2ClassRecord& ship_class,
	std::uint32_t owner_subsystem_id,
	WeaponFamily source_family,
	std::uint16_t source_index) noexcept
{
	const Phase2BankRecord* found = nullptr;
	for (std::uint32_t index = 0U; index < ship_class.bank_count; ++index) {
		const auto& bank = ship_class.banks[index];
		if (bank.family != WeaponFamily::Turret ||
			bank.owner_subsystem_id != owner_subsystem_id ||
			bank.source_family != source_family ||
			bank.source_index != source_index) {
			continue;
		}
		if (found != nullptr) return nullptr;
		found = &bank;
	}
	return found;
}

const ShipObservationDto* find_player(const Phase2ObservationDto& observation) noexcept
{
	const ShipObservationDto* found = nullptr;
	for (const auto& ship : observation.ships) {
		if (ship.capture_key.value == observation.player_key.value) {
			if (found != nullptr) {
				return nullptr;
			}
			found = &ship;
		}
	}
	return found;
}

const ShipObservationDto* find_subject(
	const Phase2ObservationDto& observation,
	detail::Phase2CaptureLocalKey key) noexcept
{
	const ShipObservationDto* found = nullptr;
	for (const auto& ship : observation.ships) {
		if (ship.capture_key.value != key.value) continue;
		if (found != nullptr) return nullptr;
		found = &ship;
	}
	return found;
}

const Phase2ClassRecord* find_class(const Phase2ManifestCandidate& manifest,
	std::uint32_t source_key) noexcept
{
	const Phase2ClassRecord* found = nullptr;
	for (std::uint32_t index = 0U; index < manifest.class_record_count; ++index) {
		const auto& item = manifest.class_records[index];
		if (item.source_key == source_key) {
			if (found != nullptr) {
				return nullptr;
			}
			found = &item;
		}
	}
	return found;
}

const Phase2SubsystemRecord* find_subsystem(const Phase2ClassRecord& ship_class,
	std::uint32_t source_key) noexcept
{
	const Phase2SubsystemRecord* found = nullptr;
	for (std::uint32_t index = 0U; index < ship_class.subsystem_count; ++index) {
		const auto& item = ship_class.subsystems[index];
		if (item.source_key == source_key) {
			if (found != nullptr) {
				return nullptr;
			}
			found = &item;
		}
	}
	return found;
}

std::uint32_t find_auxiliary(const Phase2ManifestCandidate& manifest,
	AuxiliaryRegistry registry,
	std::uint32_t source_key) noexcept
{
	std::uint32_t found = 0U;
	for (std::uint32_t index = 0U; index < manifest.auxiliary_record_count; ++index) {
		const auto& item = manifest.auxiliary_records[index];
		if (item.registry == registry && item.source_key == source_key) {
			if (found != 0U) return 0U;
			found = item.public_id;
		}
	}
	return found;
}

bool finite_nonnegative(float value) noexcept
{
	return std::isfinite(value) && value >= 0.0F;
}

bool finite_bounded(float value, float minimum, float maximum) noexcept
{
	return std::isfinite(value) && value >= minimum && value <= maximum;
}

protocol::ControlMode control_mode(
	const detail::PlayerControlObservation& controls) noexcept
{
	if (controls.autopilot_engaged) return protocol::ControlMode::Autopilot;
	if (controls.player_use_ai || controls.engine_control_mode != 0) {
		return protocol::ControlMode::Unknown;
	}
	if (controls.mode == detail::PlayerControlModeObservation::Camera) {
		return protocol::ControlMode::View;
	}
	if (controls.flight_cursor_active) {
		return protocol::ControlMode::FlightCursor;
	}
	return protocol::ControlMode::Ship;
}

bool make_control(std::uint64_t entity_id,
	const detail::PlayerControlObservation& controls,
	StateAtom& atom)
{
	reset_atom(atom);
	const auto required_presence =
		protocol::ControlStatePresenceFlagCruise |
		protocol::ControlStatePresenceFlagRequestCounters;
	if (entity_id == 0U ||
		static_cast<std::uint8_t>(controls.mode) >=
			static_cast<std::uint8_t>(
				detail::PlayerControlModeObservation::Count) ||
		(controls.presence & ~protocol::KnownControlStatePresenceFlags) != 0U ||
		(controls.presence & required_presence) != required_presence ||
		(controls.action_flags & ~protocol::KnownControlFlags) != 0U ||
		!finite_bounded(controls.pitch, -1.0F, 1.0F) ||
		!finite_bounded(controls.heading, -1.0F, 1.0F) ||
		!finite_bounded(controls.bank, -1.0F, 1.0F) ||
		!finite_bounded(controls.vertical, -1.0F, 1.0F) ||
		!finite_bounded(controls.sideways, -1.0F, 1.0F) ||
		!finite_bounded(controls.forward, -1.0F, 1.0F) ||
		!finite_bounded(
			controls.forward_cruise_percent, -100.0F, 100.0F)) {
		return false;
	}
	const auto has_cursor =
		(controls.presence &
			protocol::ControlStatePresenceFlagFlightCursor) != 0U;
	if (has_cursor != controls.flight_cursor_active) return false;
	float deadzone = 0.0F;
	if (has_cursor) {
		if (!finite_bounded(
				controls.flight_cursor_pitch, -3.1415927F, 3.1415927F) ||
			!finite_bounded(
				controls.flight_cursor_heading, -3.1415927F, 3.1415927F) ||
			!finite_bounded(
				controls.flight_cursor_sensitivity, 0.0F, 1.0F) ||
			!finite_bounded(
				controls.flight_cursor_deadzone_extent, 0.0F, 3.1415927F) ||
			!finite_bounded(
				controls.effective_aim_extent, 0.000001F, 3.1415927F)) {
			return false;
		}
		deadzone = controls.flight_cursor_deadzone_extent /
			controls.effective_aim_extent;
		if (!finite_bounded(deadzone, 0.0F, 1.0F)) return false;
	}
	auto flags = controls.action_flags;
	const auto mode = control_mode(controls);
	const auto manual_normal =
		!controls.autopilot_engaged && !controls.player_use_ai &&
		controls.engine_control_mode == 0 &&
		mode != protocol::ControlMode::View;
	if (manual_normal && controls.afterburner_requested) {
		flags |= protocol::ControlFlagAfterburnerRequested;
	}
	std::array<std::uint8_t, ControlPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(entity_id) ||
		!writer.write_u64(controls.presence) ||
		!writer.write_u64(controls.sample_time_us) ||
		!writer.write_f32(controls.pitch) ||
		!writer.write_f32(controls.heading) ||
		!writer.write_f32(controls.bank) ||
		!writer.write_f32(controls.vertical) ||
		!writer.write_f32(controls.sideways) ||
		!writer.write_f32(controls.forward) ||
		!writer.write_u8(static_cast<std::uint8_t>(mode)) ||
		!writer.write_u32(flags) ||
		!writer.write_f32(controls.forward_cruise_percent) ||
		!writer.write_u16(controls.fire_primary_count) ||
		!writer.write_u16(controls.fire_secondary_count) ||
		!writer.write_u16(controls.fire_countermeasure_count) ||
		(has_cursor &&
			(!writer.write_f32(controls.flight_cursor_pitch) ||
			 !writer.write_f32(controls.flight_cursor_heading) ||
			 !writer.write_f32(controls.flight_cursor_sensitivity) ||
			 !writer.write_f32(deadzone)))) {
		return false;
	}
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::ControlState);
	if (!write_entity_identity(entity_id, atom.key)) return false;
	return assign_payload(writer, atom);
}

bool make_mission(const CommonRecordInput& input, StateAtom& atom)
{
	reset_atom(atom);
	std::array<std::uint8_t, MissionPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(protocol::MissionStatePresenceFlagNone) ||
		!writer.write_u32(input.mission.mission_generation) ||
		!writer.write_u8(static_cast<std::uint8_t>(input.mission.phase)) ||
		!writer.write_bool8(input.mission.paused) || !writer.write_zeroes(2U) ||
		!writer.write_f32(input.mission.time_compression) ||
		!writer.write_u64(input.mission.producer_sample_time_us)) {
		return false;
	}
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::MissionState);
	return assign_payload(writer, atom);
}

bool make_lifecycle(const CommonRecordInput& input,
	const ShipObservationDto& ship,
	const Phase2ClassRecord& ship_class,
	StateAtom& atom,
	ShipLifecycleState override_state = ShipLifecycleState::Count)
{
	reset_atom(atom);
	std::array<std::uint8_t, LifecyclePayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	auto presence = static_cast<std::uint64_t>(protocol::EntityLifecyclePresenceFlagClassReference);
	const auto phase = lifecycle_phase(
		override_state == ShipLifecycleState::Count
			? ship.lifecycle.state : override_state);
	if (phase == protocol::LifecyclePhase::Spawning) {
		presence |= protocol::EntityLifecyclePresenceFlagArrivalMode;
	}
	if (phase == protocol::LifecyclePhase::Departing) {
		presence |= protocol::EntityLifecyclePresenceFlagDepartureMode;
	}
	if (!writer.write_u64(input.player_entity_id) || !writer.write_u64(presence) ||
		!writer.write_u64(ship.lifecycle.sample_time_us) ||
		!writer.write_u8(static_cast<std::uint8_t>(protocol::ObjectType::Ship)) ||
		!writer.write_u8(static_cast<std::uint8_t>(phase)) ||
		!writer.write_u32(ship.lifecycle.lifecycle_flags) ||
		!writer.write_u32(ship_class.class_id) ||
		((presence & protocol::EntityLifecyclePresenceFlagArrivalMode) != 0U &&
			!writer.write_u8(static_cast<std::uint8_t>(protocol::TransitMode::None))) ||
		((presence & protocol::EntityLifecyclePresenceFlagDepartureMode) != 0U &&
			!writer.write_u8(static_cast<std::uint8_t>(protocol::TransitMode::None))) ||
		!write_entity_identity(input.player_entity_id, atom.key)) {
		return false;
	}
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::EntityLifecycle);
	atom.lifecycle = protocol::StateRecordLifecycle::ExplicitCreateDelete;
	return assign_payload(writer, atom);
}

bool make_identity(const CommonRecordInput& input,
	const ShipObservationDto& ship,
	const Phase2ClassRecord& ship_class,
	const StateAtomKey& lifecycle_key,
	StateAtom& atom)
{
	reset_atom(atom);
	const auto allowed = protocol::ShipIdentityPresenceFlagDisplayName |
		protocol::ShipIdentityPresenceFlagCallsign | protocol::ShipIdentityPresenceFlagWing;
	if (ship.identity.internal_name.empty() || (ship.identity.presence & ~allowed) != 0U ||
		(ship.identity.role_flags & ~protocol::KnownShipRoleFlags) != 0U) {
		return false;
	}
	std::array<std::uint8_t, IdentityPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(input.player_entity_id) || !writer.write_u64(ship.identity.presence) ||
		!writer.write_u64(ship.identity.sample_time_us) || !writer.write_u32(ship_class.class_id) ||
		!writer.write_utf8(ship.identity.internal_name.view(), 255U) ||
		((ship.identity.presence & protocol::ShipIdentityPresenceFlagDisplayName) != 0U &&
			!writer.write_utf8(ship.identity.display_name.view(), 255U)) ||
		((ship.identity.presence & protocol::ShipIdentityPresenceFlagCallsign) != 0U &&
			!writer.write_utf8(ship.identity.callsign.view(), 127U)) ||
		!writer.write_u32(ship_class.species_id) || !writer.write_u32(0U) ||
		!writer.write_u32(ship_class.iff_id) ||
		!writer.write_u16(ship.identity.role_flags == 0U
			? protocol::ShipRoleFlagPlayer | protocol::ShipRoleFlagMissionObject
			: ship.identity.role_flags) ||
		!writer.write_f32(ship.flight.radius) ||
		((ship.identity.presence & protocol::ShipIdentityPresenceFlagWing) != 0U &&
			(ship_class.wing_id == 0U || !writer.write_u32(ship_class.wing_id) ||
			 !writer.write_u16(ship.identity.wing_position) ||
			 !writer.write_utf8(ship.identity.wing_name.view(), 127U)))) {
		return false;
	}
	set_entity_atom(atom, RecordType::ShipIdentity, input.player_entity_id, lifecycle_key);
	return assign_payload(writer, atom);
}

bool make_flight(const CommonRecordInput& input,
	const ShipObservationDto& ship,
	const StateAtomKey& lifecycle_key,
	StateAtom& atom)
{
	reset_atom(atom);
	const auto& value = ship.flight;
	std::array<std::uint8_t, FlightPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (value.presence != protocol::FlightStatePresenceFlagNone ||
		!writer.write_u64(input.player_entity_id) || !writer.write_u64(value.presence) ||
		!writer.write_u64(value.sample_time_us)) {
		return false;
	}
	for (const auto item : value.position_world) if (!writer.write_f32(item)) return false;
	for (const auto item : value.orientation_local_to_world) if (!writer.write_f32(item)) return false;
	for (const auto item : value.velocity_world) if (!writer.write_f32(item)) return false;
	for (const auto item : value.rotational_velocity_local) if (!writer.write_f32(item)) return false;
	if (!writer.write_f32(value.radius) || !writer.write_u32(value.physics_mode_flags)) {
		return false;
	}
	set_entity_atom(atom, RecordType::FlightState, input.player_entity_id, lifecycle_key);
	return assign_payload(writer, atom);
}

bool make_damage(const CommonRecordInput& input,
	const ShipObservationDto& ship,
	const Phase2ClassRecord& ship_class,
	const StateAtomKey& lifecycle_key,
	StateAtom& atom,
	bool permit_complete_domain_default = false)
{
	reset_atom(atom);
	const auto& value = ship.damage;
	const auto allowed = protocol::DamageStatePresenceFlagArmor | protocol::DamageStatePresenceFlagGuardian;
	if ((value.presence & ~allowed) != 0U) {
		return false;
	}
	std::array<std::uint8_t, DamagePayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	const auto protection = value.protection_flags;
	const auto hull = value.hull_current < 0.0F ? 0.0F : value.hull_current;
	const auto hull_maximum =
		permit_complete_domain_default &&
			value.hull_maximum == 0.0F
		? 1.0F : value.hull_maximum;
	auto armor_id = ship_class.armor_id;
	if ((value.presence & protocol::DamageStatePresenceFlagArmor) != 0U &&
		value.armor_source_key.value != 0U) {
		armor_id = find_auxiliary(*input.installed_manifest,
			AuxiliaryRegistry::Armor, value.armor_source_key.value);
	}
	if ((protection & ~protocol::KnownProtectionFlags) != 0U ||
		(((value.presence & protocol::DamageStatePresenceFlagGuardian) != 0U) !=
			((protection & protocol::ProtectionFlagGuardian) != 0U))) return false;
	if (!writer.write_u64(input.player_entity_id) || !writer.write_u64(value.presence) ||
		!writer.write_u64(value.sample_time_us) || !writer.write_f32(hull) ||
		!writer.write_f32(hull_maximum) || !writer.write_u16(protection) ||
		((value.presence & protocol::DamageStatePresenceFlagArmor) != 0U &&
			(armor_id == 0U || !writer.write_u32(armor_id))) ||
		((value.presence & protocol::DamageStatePresenceFlagGuardian) != 0U &&
			!writer.write_f32(value.guardian_threshold))) {
		return false;
	}
	set_entity_atom(atom, RecordType::DamageState, input.player_entity_id, lifecycle_key);
	return assign_payload(writer, atom);
}

bool make_shields(const CommonRecordInput& input,
	const ShipObservationDto& ship,
	const StateAtomKey& lifecycle_key,
	StateAtom& atom)
{
	reset_atom(atom);
	const auto& value = ship.shields;
	if ((!value.has_shields && (value.segment_count != 0U || value.presence != 0U)) ||
		(value.has_shields && (value.segment_count == 0U ||
			value.segment_count > MaximumPhase2ShieldSegments))) {
		return false;
	}
	std::array<std::uint8_t, ShieldPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(input.player_entity_id) || !writer.write_u64(value.presence) ||
		!writer.write_u64(value.sample_time_us) || !writer.write_bool8(value.has_shields) ||
		!writer.write_u16(value.segment_count) || !writer.write_zeroes(2U)) {
		return false;
	}
	for (std::size_t i = 0U; i < value.segment_count; ++i)
		if (!writer.write_f32(value.segment_current_hits[i])) return false;
	for (std::size_t i = 0U; i < value.segment_count; ++i)
		if (!writer.write_f32(value.segment_maximum_hits[i])) return false;
	if (((value.presence & protocol::ShieldStatePresenceFlagRechargeMax) != 0U &&
			!writer.write_f32(value.recharge_maximum)) ||
		((value.presence & protocol::ShieldStatePresenceFlagRegenRate) != 0U &&
			!writer.write_f32(value.regeneration_rate)) ||
		((value.presence & protocol::ShieldStatePresenceFlagDeferredTransfer) != 0U &&
			!writer.write_f32(value.deferred_transfer))) {
		return false;
	}
	set_entity_atom(atom, RecordType::ShieldState, input.player_entity_id, lifecycle_key);
	return assign_payload(writer, atom);
}

bool make_subsystem(const CommonRecordInput& input,
	const ShipSubsystemObservation& value,
	const Phase2ClassRecord& ship_class,
	const Phase2SubsystemRecord& definition,
	const StateAtomKey* lifecycle_key,
	StateAtom& atom)
{
	reset_atom(atom);
	const auto allowed = protocol::SubsystemStatePresenceFlagArmor |
		protocol::SubsystemStatePresenceFlagPerturbation |
		protocol::SubsystemStatePresenceFlagAnimatedTransform |
		protocol::SubsystemStatePresenceFlagTypeAggregate |
		protocol::SubsystemStatePresenceFlagTurret;
	const auto presence = value.presence & allowed;
	std::array<std::uint8_t, SubsystemPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if ((value.presence & ~allowed) != 0U ||
		!writer.write_u64(input.player_entity_id) || !writer.write_u32(definition.subsystem_id) ||
		!writer.write_u64(presence) || !writer.write_u64(value.sample_time_us) ||
		!writer.write_u16(static_cast<std::uint16_t>(definition.canonical_index)) ||
		!writer.write_u8(static_cast<std::uint8_t>(subsystem_type(value.kind))) ||
		!writer.write_f32(value.hits_current) || !writer.write_f32(value.hits_maximum) ||
		!writer.write_u32(static_cast<std::uint32_t>(value.raw_flags)) ||
		((presence & protocol::SubsystemStatePresenceFlagArmor) != 0U &&
			(definition.armor_id == 0U || !writer.write_u32(definition.armor_id))) ||
		((presence & protocol::SubsystemStatePresenceFlagPerturbation) != 0U &&
			!writer.write_u64(value.disruption_remaining_us)) ||
		((presence & protocol::SubsystemStatePresenceFlagAnimatedTransform) != 0U &&
			(!writer.write_f32(value.position_local[0]) ||
			 !writer.write_f32(value.position_local[1]) ||
			 !writer.write_f32(value.position_local[2]) ||
			 !writer.write_f32(value.orientation_local[0]) ||
			 !writer.write_f32(value.orientation_local[1]) ||
			 !writer.write_f32(value.orientation_local[2]) ||
			 !writer.write_f32(value.orientation_local[3]))) ||
		((presence & protocol::SubsystemStatePresenceFlagTypeAggregate) != 0U &&
			(!writer.write_f32(value.aggregate_current_hits) ||
			 !writer.write_f32(value.aggregate_maximum_hits)))) {
		return false;
	}
	const auto has_turret =
		(presence & protocol::SubsystemStatePresenceFlagTurret) != 0U;
	if (has_turret != value.turret.has_value() ||
		(has_turret && value.kind != ShipSubsystemKind::Turret)) {
		return false;
	}
	if (has_turret) {
		const auto& turret = *value.turret;
		const auto bank_count =
			static_cast<std::uint16_t>(
				turret.turret_primary_bank_count +
				turret.turret_secondary_bank_count);
		if (turret.turret_firing_point_count > 64U ||
			(turret.turret_firing_point_count != 0U &&
				turret.turret_next_fire_pos < 0) ||
			bank_count > 64U ||
			turret.turret_next_fire_remaining_us >
				86'400'000'000ULL ||
			turret.turret_animation_remaining_us >
				86'400'000'000ULL) {
			return false;
		}
		double direction_norm = 0.0;
		for (const auto component :
				turret.turret_current_direction_local) {
			if (!finite_bounded(component, -1.0F, 1.0F)) return false;
			direction_norm += static_cast<double>(component) * component;
		}
		if (!std::isfinite(direction_norm) ||
			std::fabs(direction_norm - 1.0) > 0.0001) {
			return false;
		}
		auto rate_multiplier = turret.turret_rof_scaler;
		if (rate_multiplier < 0.0F) {
			rate_multiplier = 1.0F;
		} else if (rate_multiplier == 0.0F) {
			rate_multiplier = static_cast<float>(
				std::max<std::uint16_t>(
					turret.turret_firing_point_count, 1U));
		}
		if (!finite_bounded(rate_multiplier, 0.0F, 1024.0F)) {
			return false;
		}
		auto turret_presence =
			protocol::TurretStatePresenceFlagCooldown |
			protocol::TurretStatePresenceFlagRateMultiplier |
			protocol::TurretStatePresenceFlagBanks;
		if (turret.turret_firing_point_count != 0U) {
			turret_presence |=
				protocol::TurretStatePresenceFlagNextFirePoint;
		}
		protocol::AnimationState animation =
			protocol::AnimationState::None;
		if (turret.turret_animation == 1) {
			animation = protocol::AnimationState::Moving;
			turret_presence |=
				protocol::TurretStatePresenceFlagAnimation;
		} else if (turret.turret_animation == 2) {
			animation = protocol::AnimationState::Stopped;
			turret_presence |=
				protocol::TurretStatePresenceFlagAnimation;
		} else if (turret.turret_animation != 0) {
			return false;
		}
		if (!writer.write_u32(turret_presence) ||
			!writer.write_u64(0U)) {
			return false;
		}
		for (const auto component :
				turret.turret_current_direction_local) {
			if (!writer.write_f32(component)) return false;
		}
		if (((turret_presence &
				protocol::TurretStatePresenceFlagNextFirePoint) != 0U &&
				(!writer.write_u16(static_cast<std::uint16_t>(
					static_cast<std::uint32_t>(
						turret.turret_next_fire_pos) %
					turret.turret_firing_point_count)) ||
				 !writer.write_u16(0U))) ||
			!writer.write_u64(
				turret.turret_next_fire_remaining_us) ||
			!writer.write_f32(rate_multiplier) ||
			((turret_presence &
				protocol::TurretStatePresenceFlagAnimation) != 0U &&
				(!writer.write_u8(
					static_cast<std::uint8_t>(animation)) ||
				 !writer.write_u64(
					turret.turret_animation_remaining_us))) ||
			!writer.write_u16(bank_count)) {
			return false;
		}
		const auto write_bank =
			[&](WeaponFamily source_family,
				std::uint16_t source_index,
				const detail::ShipTurretBankRawObservation& facts) {
				const auto* bank = find_turret_bank(ship_class,
					definition.subsystem_id,
					source_family,
					source_index);
				if (bank == nullptr || bank->bank_id == 0U ||
					bank->weapon_class_id == 0U ||
					(bank->consumes_ammunition &&
						(facts.turret_ammunition_current < 0 ||
						 facts.turret_ammunition_capacity < 0 ||
						 facts.turret_ammunition_current >
							facts.turret_ammunition_capacity)) ||
					facts.turret_cooldown_remaining_us >
						86'400'000'000ULL) {
					return false;
				}
				const auto item_presence = bank->consumes_ammunition
					? protocol::TurretBankPresenceFlagAmmo
					: protocol::TurretBankPresenceFlagNone;
				std::array<std::uint8_t, 32U> item_bytes{};
				PacketWriter item(MutableByteView{
					item_bytes.data(), item_bytes.size()});
				if (!item.write_u16(item_presence) ||
					!item.write_u8(static_cast<std::uint8_t>(
						source_family)) ||
					!item.write_u8(0U) ||
					!item.write_u16(source_index) ||
					!item.write_u32(bank->bank_id) ||
					!item.write_u32(bank->weapon_class_id) ||
					(bank->consumes_ammunition &&
						(!item.write_u32(static_cast<std::uint32_t>(
							facts.turret_ammunition_current)) ||
						 !item.write_f32(bank->capacity))) ||
					!item.write_u64(
						facts.turret_cooldown_remaining_us) ||
					!writer.write_u8(1U) ||
					!writer.write_u16(
						static_cast<std::uint16_t>(item.size())) ||
					!writer.write_bytes(item.written())) {
					return false;
				}
				return true;
			};
		for (std::uint16_t bank = 0U;
			 bank < turret.turret_primary_bank_count;
			 ++bank) {
			if (!write_bank(WeaponFamily::Primary,
					bank,
					turret.turret_primary_banks[bank])) {
				return false;
			}
		}
		for (std::uint16_t bank = 0U;
			 bank < turret.turret_secondary_bank_count;
			 ++bank) {
			if (!write_bank(WeaponFamily::Secondary,
					bank,
					turret.turret_secondary_banks[bank])) {
				return false;
			}
		}
	}
	if (!write_subsystem_identity(
			input.player_entity_id, definition.subsystem_id, atom.key)) {
		return false;
	}
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::SubsystemState);
	atom.lifecycle = protocol::StateRecordLifecycle::ExplicitCreateDelete;
	if (lifecycle_key != nullptr) {
		atom.has_cascade_owner = true;
		atom.cascade_owner = *lifecycle_key;
	}
	return assign_payload(writer, atom);
}

bool make_energy(const CommonRecordInput& input,
	const ShipObservationDto& ship,
	const StateAtomKey& lifecycle_key,
	StateAtom& atom)
{
	reset_atom(atom);
	const auto& value = ship.energy;
	const auto allowed = protocol::EnergyStatePresenceFlagWeaponEnergy |
		protocol::EnergyStatePresenceFlagRegeneration |
		protocol::EnergyStatePresenceFlagDeferredTransfers |
		protocol::EnergyStatePresenceFlagPowerOutput |
		protocol::EnergyStatePresenceFlagEngineIntegrity;
	if ((value.presence & ~allowed) != 0U) {
		return false;
	}
	const auto any_index = value.shield_recharge_index != 0U ||
		value.weapon_recharge_index != 0U || value.engine_recharge_index != 0U;
	const auto ets_mode = value.ets_available ? protocol::EtsMode::Available
		: (value.ets_mode == protocol::EtsMode::Locked || any_index
			? protocol::EtsMode::Locked : protocol::EtsMode::Absent);
	std::array<std::uint8_t, EnergyPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(input.player_entity_id) || !writer.write_u64(value.presence) ||
		!writer.write_u64(value.sample_time_us) ||
		!writer.write_u8(static_cast<std::uint8_t>(ets_mode)) ||
		!writer.write_u8(value.shield_recharge_index) ||
		!writer.write_u8(value.weapon_recharge_index) ||
		!writer.write_u8(value.engine_recharge_index) || !writer.write_u8(0U) ||
		((value.presence & protocol::EnergyStatePresenceFlagWeaponEnergy) != 0U &&
			(!writer.write_f32(value.weapon_energy_current) ||
			 !writer.write_f32(value.weapon_energy_maximum))) ||
		((value.presence & protocol::EnergyStatePresenceFlagRegeneration) != 0U &&
			(!writer.write_f32(value.shield_regeneration_rate) ||
			 !writer.write_f32(value.weapon_regeneration_rate))) ||
		((value.presence & protocol::EnergyStatePresenceFlagDeferredTransfers) != 0U &&
			(!writer.write_f32(value.deferred_weapon_transfer) ||
			 !writer.write_f32(value.deferred_shield_transfer))) ||
		((value.presence & protocol::EnergyStatePresenceFlagPowerOutput) != 0U &&
			!writer.write_f32(value.power_output)) ||
		((value.presence & protocol::EnergyStatePresenceFlagEngineIntegrity) != 0U &&
			(!writer.write_f32(value.engine_integrity_current) ||
			 !writer.write_f32(value.engine_integrity_maximum)))) {
		return false;
	}
	set_entity_atom(atom, RecordType::EnergyState, input.player_entity_id, lifecycle_key);
	return assign_payload(writer, atom);
}

bool make_propulsion(const CommonRecordInput& input,
	const ShipObservationDto& ship,
	const StateAtomKey& lifecycle_key,
	StateAtom& atom)
{
	reset_atom(atom);
	const auto& value = ship.propulsion;
	const auto allowed = protocol::PropulsionStatePresenceFlagFuel |
		protocol::PropulsionStatePresenceFlagConsumption |
		protocol::PropulsionStatePresenceFlagEngagement |
		protocol::PropulsionStatePresenceFlagDynamics |
		protocol::PropulsionStatePresenceFlagEngineWash;
	if ((value.presence & ~allowed) != 0U ||
		(value.propulsion_flags & ~protocol::KnownPropulsionFlags) != 0U) {
		return false;
	}
	std::array<std::uint8_t, PropulsionPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(input.player_entity_id) || !writer.write_u64(value.presence) ||
		!writer.write_u64(value.sample_time_us) ||
		!writer.write_u16(static_cast<std::uint16_t>(value.propulsion_flags)) ||
		!writer.write_u16(0U) ||
		((value.presence & protocol::PropulsionStatePresenceFlagFuel) != 0U &&
			(!writer.write_f32(value.afterburner_fuel) ||
			 !writer.write_f32(value.afterburner_capacity))) ||
		((value.presence & protocol::PropulsionStatePresenceFlagConsumption) != 0U &&
			(!writer.write_f32(value.burn_rate) || !writer.write_f32(value.recovery_rate)))) {
		return false;
	}
	if ((value.presence & protocol::PropulsionStatePresenceFlagEngagement) != 0U &&
		(!writer.write_f32(value.minimum_to_engage) ||
		 !writer.write_u64(value.cooldown_remaining_us) ||
		 !writer.write_u64(value.time_since_last_stop_us) ||
		 !writer.write_f32(value.fuel_at_last_engagement))) {
		return false;
	}
	if ((value.presence & protocol::PropulsionStatePresenceFlagDynamics) != 0U) {
		if (!writer.write_f32(value.forward_acceleration_time_constant)) return false;
		for (const auto component : value.afterburner_max_velocity)
			if (!writer.write_f32(component)) return false;
	}
	if ((value.presence & protocol::PropulsionStatePresenceFlagEngineWash) != 0U &&
		!writer.write_f32(value.engine_wash_intensity)) return false;
	set_entity_atom(atom, RecordType::PropulsionState, input.player_entity_id, lifecycle_key);
	return assign_payload(writer, atom);
}

const Phase2WeaponRecord* find_weapon_record(
	const Phase2ManifestCandidate& manifest,
	std::uint32_t source_key) noexcept
{
	const Phase2WeaponRecord* found = nullptr;
	for (std::uint32_t index = 0U;
		 index < manifest.weapon_record_count; ++index) {
		const auto& candidate = manifest.weapon_records[index];
		if (candidate.source_key != source_key) continue;
		if (found != nullptr &&
			(found->weapon_class_id != candidate.weapon_class_id ||
			 found->class_flags != candidate.class_flags))
			return nullptr;
		found = &candidate;
	}
	return found;
}

const Phase2BankRecord* find_hull_bank(
	const Phase2ClassRecord& ship_class,
	WeaponFamily family,
	std::uint16_t index) noexcept
{
	const Phase2BankRecord* found = nullptr;
	for (std::uint32_t candidate_index = 0U;
		 candidate_index < ship_class.bank_count; ++candidate_index) {
		const auto& candidate = ship_class.banks[candidate_index];
		if (candidate.family != family || candidate.owner_subsystem_id != 0U ||
			candidate.canonical_index != index)
			continue;
		if (found != nullptr) return nullptr;
		found = &candidate;
	}
	return found;
}

std::uint32_t find_pattern_id(
	const Phase2ManifestCandidate& manifest,
	std::uint8_t source_code) noexcept
{
	if (source_code == 0U) return 0U;
	if (source_code > 5U) return 0U;
	std::uint32_t found = 0U;
	for (std::uint32_t index = 0U;
		 index < manifest.auxiliary_record_count;
		 ++index) {
		const auto& candidate = manifest.auxiliary_records[index];
		if (candidate.registry != AuxiliaryRegistry::Pattern ||
			candidate.source_key !=
				static_cast<std::uint32_t>(source_code) + 1U) {
			continue;
		}
		if (candidate.public_id == 0U || found != 0U) return 0U;
		found = candidate.public_id;
	}
	return found;
}

struct ResolvedWeaponSubject {
	const ShipObservationDto* ship = nullptr;
	const Phase2ClassRecord* ship_class = nullptr;
	std::uint64_t entity_id = 0U;
};

bool make_weapon_state(
	const WeaponRecordInput& input,
	const ResolvedWeaponSubject& subject,
	StateAtom& atom) noexcept
{
	reset_atom(atom);
	const auto& weapons = subject.ship->weapons;
	const auto& ship_class = *subject.ship_class;
	std::array<const Phase2BankRecord*,
		detail::MaximumPhase2WeaponBanksPerFamily> primary{};
	std::array<const Phase2BankRecord*,
		detail::MaximumPhase2WeaponBanksPerFamily> secondary{};
	const auto resolve_banks =
		[&](WeaponFamily family,
			std::uint8_t count,
			const auto& source,
			auto& output) noexcept {
			for (std::uint16_t index = 0U; index < count; ++index) {
				const auto* bank =
					find_hull_bank(ship_class, family, index);
				const auto* weapon = find_weapon_record(
					*input.installed_manifest,
					source[index].weapon_class_source_key.value);
				if (bank == nullptr || bank->bank_id == 0U ||
					bank->weapon_class_id == 0U ||
					weapon == nullptr ||
					weapon->weapon_class_id != bank->weapon_class_id) {
					return false;
				}
				output[index] = bank;
			}
			return true;
		};
	if (!resolve_banks(WeaponFamily::Primary,
			weapons.primary_bank_count,
			weapons.primary_banks,
			primary) ||
		!resolve_banks(WeaponFamily::Secondary,
			weapons.secondary_bank_count,
			weapons.secondary_banks,
			secondary)) {
		return false;
	}
	const auto selected =
		[](std::int32_t index,
			std::uint8_t count,
			const auto& banks,
			bool allow_absent,
			std::uint32_t& output) noexcept {
			output = 0U;
			if (index == -1 && allow_absent) return true;
			if (index < 0 || index >= count || banks[index] == nullptr) {
				return false;
			}
			output = banks[index]->bank_id;
			return true;
		};
	std::uint32_t current_primary = 0U;
	std::uint32_t current_secondary = 0U;
	std::uint32_t previous_primary = 0U;
	std::uint32_t previous_secondary = 0U;
	std::uint32_t targeting_laser = 0U;
	std::uint32_t swarm_origin = 0U;
	if (!selected(weapons.current_primary_bank,
			weapons.primary_bank_count,
			primary,
			true,
			current_primary) ||
		!selected(weapons.current_secondary_bank,
			weapons.secondary_bank_count,
			secondary,
			true,
			current_secondary) ||
		!selected(weapons.previous_primary_bank,
			weapons.primary_bank_count,
			primary,
			true,
			previous_primary) ||
		!selected(weapons.previous_secondary_bank,
			weapons.secondary_bank_count,
			secondary,
			true,
			previous_secondary)) {
		return false;
	}
	std::uint64_t presence = 0U;
	if (weapons.previous_primary_bank >= 0) {
		presence |= protocol::WeaponStatePresenceFlagPreviousPrimary;
	}
	if (weapons.previous_secondary_bank >= 0) {
		presence |= protocol::WeaponStatePresenceFlagPreviousSecondary;
	}
	if (weapons.targeting_laser_active) {
		if (!selected(weapons.targeting_laser_bank,
				weapons.primary_bank_count,
				primary,
				false,
				targeting_laser)) {
			return false;
		}
		presence |= protocol::WeaponStatePresenceFlagTargetingLaser;
	}
	if (weapons.swarm_remaining > 0U) {
		if (!selected(weapons.swarm_secondary_bank,
				weapons.secondary_bank_count,
				secondary,
				false,
				swarm_origin) ||
			weapons.swarm_remaining > 4096U) {
			return false;
		}
		presence |= protocol::WeaponStatePresenceFlagSwarm;
	}
	if (weapons.remote_detonaters_active > 0U) {
		if (weapons.remote_detonation_remaining_us >
			3'600'000'000ULL) return false;
		presence |= protocol::WeaponStatePresenceFlagRemoteDetonation;
	}
	if (weapons.per_burst_rotation_active) {
		if (!std::isfinite(weapons.per_burst_rotation)) return false;
		presence |= protocol::WeaponStatePresenceFlagPerBurstRotation;
	}
	const auto tertiary_count = weapons.tertiary_bank_count;
	const Phase2BankRecord* tertiary = nullptr;
	if (tertiary_count > 0U) {
		if (weapons.current_tertiary_bank < 0 ||
			weapons.current_tertiary_bank >= tertiary_count ||
			weapons.tertiary_ammunition_current < 0 ||
			weapons.tertiary_ammunition_initial < 0 ||
			weapons.tertiary_ammunition_capacity < 0 ||
			weapons.tertiary_ammunition_current >
				weapons.tertiary_ammunition_initial ||
			weapons.tertiary_cooldown_remaining_us >
				3'600'000'000ULL ||
			weapons.tertiary_rearm_remaining_us >
				3'600'000'000ULL) {
			return false;
		}
		for (std::uint16_t index = 0U; index < tertiary_count; ++index) {
			const auto* candidate =
				find_hull_bank(ship_class, WeaponFamily::Tertiary, index);
			if (candidate == nullptr || candidate->bank_id == 0U ||
				candidate->weapon_class_id != 0U) {
				return false;
			}
			if (index == weapons.current_tertiary_bank) tertiary = candidate;
		}
		if (tertiary == nullptr) return false;
		presence |= protocol::WeaponStatePresenceFlagTertiary;
	} else if (weapons.current_tertiary_bank != -1) {
		return false;
	}
	const auto has_countermeasure =
		(weapons.presence &
			protocol::WeaponStatePresenceFlagCountermeasure) != 0U;
	const auto class_has_countermeasure =
		ship_class.countermeasure_installed ||
		ship_class.countermeasure_weapon_class_id != 0U ||
		ship_class.countermeasure_initial_count != 0U;
	if (has_countermeasure != class_has_countermeasure) return false;
	const Phase2WeaponRecord* countermeasure = nullptr;
	if (has_countermeasure) {
		countermeasure = find_weapon_record(*input.installed_manifest,
			weapons.countermeasure_class_source_key.value);
		if (countermeasure == nullptr ||
			countermeasure->weapon_class_id !=
				ship_class.countermeasure_weapon_class_id ||
			(countermeasure->class_flags &
				protocol::WeaponClassFlagCountermeasure) == 0U ||
			weapons.countermeasure_maximum !=
				ship_class.countermeasure_initial_count ||
			weapons.countermeasure_count >
				weapons.countermeasure_maximum ||
			weapons.countermeasure_cooldown_remaining_us >
				3'600'000'000ULL) {
			return false;
		}
		presence |= protocol::WeaponStatePresenceFlagCountermeasure;
	} else if (class_has_countermeasure ||
		weapons.countermeasure_count != 0U ||
		weapons.countermeasure_maximum != 0U) {
		return false;
	}
	std::array<std::uint8_t, WeaponPayloadCapacity> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(subject.entity_id) ||
		!writer.write_u64(presence) ||
		!writer.write_u64(weapons.sample_time_us) ||
		!writer.write_u16(weapons.primary_bank_count) ||
		!writer.write_u16(weapons.secondary_bank_count) ||
		!writer.write_u16(tertiary_count) ||
		!writer.write_u16(0U) ||
		!writer.write_u32(current_primary) ||
		!writer.write_u32(current_secondary) ||
		!writer.write_u32(tertiary == nullptr ? 0U : tertiary->bank_id) ||
		!writer.write_u32(
			static_cast<std::uint32_t>(weapons.raw_weapon_flags)) ||
		((presence &
			 protocol::WeaponStatePresenceFlagPreviousPrimary) != 0U &&
			!writer.write_u32(previous_primary)) ||
		((presence &
			 protocol::WeaponStatePresenceFlagPreviousSecondary) != 0U &&
			!writer.write_u32(previous_secondary)) ||
		((presence &
			 protocol::WeaponStatePresenceFlagTargetingLaser) != 0U &&
			!writer.write_u32(targeting_laser)) ||
		((presence & protocol::WeaponStatePresenceFlagSwarm) != 0U &&
			(!writer.write_u16(weapons.swarm_remaining) ||
			 !writer.write_u32(swarm_origin))) ||
		((presence &
			 protocol::WeaponStatePresenceFlagRemoteDetonation) != 0U &&
			!writer.write_u64(
				weapons.remote_detonation_remaining_us)) ||
		((presence &
			 protocol::WeaponStatePresenceFlagPerBurstRotation) != 0U &&
			!writer.write_f32(weapons.per_burst_rotation)) ||
		!writer.write_u16(weapons.primary_bank_count)) {
		return false;
	}
	for (std::uint16_t index = 0U;
		 index < weapons.primary_bank_count;
		 ++index) {
		const auto& source = weapons.primary_banks[index];
		const auto* weapon = find_weapon_record(*input.installed_manifest,
			source.weapon_class_source_key.value);
		if (source.primary_slot < -1 || source.primary_slot > 255 ||
			source.primary_fire_point > 255U ||
			source.simultaneous_slots == 0U ||
			source.simultaneous_slots > 256U ||
			source.cooldown_remaining_us > 3'600'000'000ULL) {
			return false;
		}
		const auto pattern_id = find_pattern_id(
			*input.installed_manifest,
			source.firing_pattern_source_code);
		if ((source.firing_pattern_source_code == 0U) !=
				(pattern_id == 0U) ||
			pattern_id > std::numeric_limits<std::uint16_t>::max() ||
			(primary[index]->pattern_id != 0U &&
			 primary[index]->pattern_id != pattern_id)) {
			return false;
		}
		std::uint16_t item_presence =
			protocol::PrimaryBankPresenceFlagNone;
		const auto ballistic =
			(weapon->class_flags &
				protocol::WeaponClassFlagBallistic) != 0U;
		if (ballistic) {
			if (source.ammunition_current > source.ammunition_initial ||
				source.rearm_remaining_us > 3'600'000'000ULL) return false;
			item_presence |=
				protocol::PrimaryBankPresenceFlagBallisticAmmo |
				protocol::PrimaryBankPresenceFlagRearm;
		}
		if (source.burst_counter != 0 || source.burst_seed != 0U)
			item_presence |= protocol::PrimaryBankPresenceFlagBurst;
		if (source.substitution_pattern_index != 0U)
			item_presence |= protocol::PrimaryBankPresenceFlagSubstitution;
		if (source.fof_cooldown_remaining_us != 0U) {
			if (source.fof_cooldown_remaining_us > 3'600'000'000ULL)
				return false;
			item_presence |= protocol::PrimaryBankPresenceFlagFofCooldown;
		}
		std::array<std::uint8_t, 96U> item_bytes{};
		PacketWriter item({item_bytes.data(), item_bytes.size()});
		if (!item.write_u16(item_presence) ||
			!item.write_u16(index) ||
			!item.write_u32(primary[index]->bank_id) ||
			!item.write_u32(weapon->weapon_class_id) ||
			!item.write_u64(source.cooldown_remaining_us) ||
			!item.write_u16(static_cast<std::uint16_t>(
				std::max(source.primary_slot, 0))) ||
			!item.write_u16(source.primary_fire_point) ||
			!item.write_u16(source.simultaneous_slots) ||
			!item.write_u16(static_cast<std::uint16_t>(pattern_id)) ||
			(ballistic &&
				(!item.write_u32(source.ammunition_current) ||
				 !item.write_u32(source.ammunition_initial) ||
				 !item.write_f32(primary[index]->capacity) ||
				 !item.write_u64(source.rearm_remaining_us))) ||
			((item_presence & protocol::PrimaryBankPresenceFlagBurst) != 0U &&
				(!item.write_u16(static_cast<std::uint16_t>(
					source.burst_counter)) ||
				 !item.write_u16(0U) ||
				 !item.write_u32(source.burst_seed))) ||
			((item_presence &
				 protocol::PrimaryBankPresenceFlagSubstitution) != 0U &&
				(!item.write_u16(source.substitution_pattern_index) ||
				 !item.write_u16(0U))) ||
			((item_presence &
				 protocol::PrimaryBankPresenceFlagFofCooldown) != 0U &&
				!item.write_u64(source.fof_cooldown_remaining_us)) ||
			!writer.write_u8(1U) ||
			!writer.write_u16(static_cast<std::uint16_t>(item.size())) ||
			!writer.write_bytes(item.written())) {
			return false;
		}
	}
	if (!writer.write_u16(weapons.secondary_bank_count)) return false;
	for (std::uint16_t index = 0U;
		 index < weapons.secondary_bank_count;
		 ++index) {
		const auto& source = weapons.secondary_banks[index];
		const auto* weapon = find_weapon_record(*input.installed_manifest,
			source.weapon_class_source_key.value);
		if (source.secondary_slot < -1 || source.secondary_slot > 255 ||
			source.cooldown_remaining_us > 3'600'000'000ULL) return false;
		std::uint16_t item_presence =
			protocol::SecondaryBankPresenceFlagNone;
		const auto ammunition =
			(weapon->class_flags &
				protocol::WeaponClassFlagAmmoless) == 0U;
		if (ammunition) {
			if (source.ammunition_current > source.ammunition_initial ||
				source.rearm_remaining_us > 3'600'000'000ULL) return false;
			item_presence |= protocol::SecondaryBankPresenceFlagAmmo |
				protocol::SecondaryBankPresenceFlagRearm;
		}
		if (source.burst_counter != 0 || source.burst_seed != 0U)
			item_presence |= protocol::SecondaryBankPresenceFlagBurst;
		if (source.substitution_pattern_index != 0U)
			item_presence |= protocol::SecondaryBankPresenceFlagSubstitution;
		std::array<std::uint8_t, 80U> item_bytes{};
		PacketWriter item({item_bytes.data(), item_bytes.size()});
		if (!item.write_u16(item_presence) ||
			!item.write_u16(index) ||
			!item.write_u32(secondary[index]->bank_id) ||
			!item.write_u32(weapon->weapon_class_id) ||
			!item.write_u64(source.cooldown_remaining_us) ||
			!item.write_u16(static_cast<std::uint16_t>(
				std::max(source.secondary_slot, 0))) ||
			!item.write_u16(0U) ||
			(ammunition &&
				(!item.write_u32(source.ammunition_current) ||
				 !item.write_u32(source.ammunition_initial) ||
				 !item.write_f32(secondary[index]->capacity) ||
				 !item.write_u64(source.rearm_remaining_us))) ||
			((item_presence & protocol::SecondaryBankPresenceFlagBurst) != 0U &&
				(!item.write_u16(static_cast<std::uint16_t>(
					source.burst_counter)) ||
				 !item.write_u16(0U) ||
				 !item.write_u32(source.burst_seed))) ||
			((item_presence &
				 protocol::SecondaryBankPresenceFlagSubstitution) != 0U &&
				(!item.write_u16(source.substitution_pattern_index) ||
				 !item.write_u16(0U))) ||
			!writer.write_u8(1U) ||
			!writer.write_u16(static_cast<std::uint16_t>(item.size())) ||
			!writer.write_bytes(item.written())) {
			return false;
		}
	}
	if (tertiary != nullptr &&
		(!writer.write_u32(tertiary->bank_id) ||
		 !writer.write_u32(static_cast<std::uint32_t>(
			 weapons.tertiary_ammunition_current)) ||
		 !writer.write_u32(static_cast<std::uint32_t>(
			 weapons.tertiary_ammunition_initial)) ||
		 !writer.write_f32(static_cast<float>(
			 weapons.tertiary_ammunition_capacity)) ||
		 !writer.write_u64(weapons.tertiary_cooldown_remaining_us) ||
		 !writer.write_u64(weapons.tertiary_rearm_remaining_us))) {
		return false;
	}
	if (countermeasure != nullptr) {
		auto flags = protocol::CountermeasureStateFlagNone;
		if (!weapons.countermeasures_enabled) {
			flags = protocol::CountermeasureStateFlagLocked;
		} else if (weapons.countermeasure_count > 0U &&
			weapons.countermeasure_cooldown_remaining_us == 0U) {
			flags = protocol::CountermeasureStateFlagAvailable;
		}
		if (!writer.write_u16(
				protocol::CountermeasureStatePresenceFlagClass) ||
			!writer.write_u16(flags) ||
			!writer.write_u32(countermeasure->weapon_class_id) ||
			!writer.write_u32(weapons.countermeasure_count) ||
			!writer.write_u32(weapons.countermeasure_maximum) ||
			!writer.write_u64(
				weapons.countermeasure_cooldown_remaining_us)) {
			return false;
		}
	}
	atom.key.record_type =
		static_cast<std::uint16_t>(RecordType::WeaponState);
	if (!write_entity_identity(subject.entity_id, atom.key) ||
		!assign_payload(writer, atom)) {
		return false;
	}
	return true;
}

constexpr std::size_t CargoPayloadCapacity = 608U;
constexpr std::size_t DockingPayloadCapacity = 18'000U;
constexpr std::size_t SupportPayloadCapacity = 40U;
constexpr std::size_t MaximumCockpitCommonRecords =
	4U + 10U * detail::MaximumPhase2ObservationShips +
	Phase2ManifestLimits::MaxAggregateSubsystems;

struct ResolvedCockpitSubject {
	const ShipObservationDto* ship = nullptr;
	const Phase2ClassRecord* ship_class = nullptr;
	std::uint64_t entity_id = 0U;
	std::uint64_t component_leader_entity_id = 0U;
};

struct ResolvedCockpitDomain {
	std::array<ResolvedCockpitSubject,
		detail::MaximumPhase2ObservationShips> subjects{};
	std::size_t subject_count = 0U;
	std::size_t subsystem_count = 0U;
	std::size_t maximum_relations = 0U;
};

struct PreparedCockpitInput {
	CockpitSensorsStateImageInput input{};
	detail::Phase2Wp07CleanupRing cleanup_ring{};
	detail::Phase2Wp07SupportTerminalRing support_ring{};
	detail::Phase2Wp07EpisodeLatches latches{};
	detail::Phase2Wp07CleanupBatch cleanup_batch{};
	bool has_cleanup_ring = false;
	bool has_support_ring = false;
	bool has_latches = false;
};

CockpitCommonBuildStatus drain_status(
	detail::Phase2Wp07DrainStatus status) noexcept
{
	switch (status) {
	case detail::Phase2Wp07DrainStatus::Drained:
	case detail::Phase2Wp07DrainStatus::NoFacts:
		return CockpitCommonBuildStatus::Created;
	case detail::Phase2Wp07DrainStatus::RingOverflow:
	case detail::Phase2Wp07DrainStatus::TableOverflow:
		return CockpitCommonBuildStatus::CapacityExceeded;
	case detail::Phase2Wp07DrainStatus::InvalidInput:
	case detail::Phase2Wp07DrainStatus::Count:
	default:
		return CockpitCommonBuildStatus::InvalidInput;
	}
}

CockpitCommonBuildStatus prepare_cockpit_input(
	const CockpitSensorsStateImageInput& source,
	PreparedCockpitInput& prepared) noexcept
{
	prepared = {};
	prepared.input = source;
	if (source.session_slot >=
		detail::Phase2Wp07EpisodeLatches::SessionCapacity)
		return CockpitCommonBuildStatus::InvalidInput;
	if (source.episode_latches != nullptr) {
		prepared.has_latches = true;
		prepared.latches = *source.episode_latches;
		if (!prepared.latches.activate_session(source.session_slot))
			return CockpitCommonBuildStatus::InvalidInput;
		prepared.input.episode_latches = &prepared.latches;
	}
	if (source.support_terminal_ring != nullptr) {
		if (!prepared.has_latches)
			return CockpitCommonBuildStatus::InvalidInput;
		prepared.has_support_ring = true;
		prepared.support_ring = *source.support_terminal_ring;
		const auto status = drain_status(
			detail::drain_support_terminals_once(
				prepared.support_ring, prepared.latches));
		if (status != CockpitCommonBuildStatus::Created)
			return status;
		prepared.input.support_terminal_ring = nullptr;
	}
	if (source.cleanup_ring != nullptr) {
		if (source.subject_count >
			detail::Phase2Wp07CleanupRing::Capacity ||
			(source.subject_count != 0U && source.subjects == nullptr))
			return CockpitCommonBuildStatus::InvalidInput;
		std::array<std::uint32_t,
			detail::Phase2Wp07CleanupRing::Capacity> closure{};
		for (std::size_t index = 0U;
			 index < source.subject_count; ++index)
			closure[index] = source.subjects[index].capture_key.value;
		prepared.has_cleanup_ring = true;
		prepared.cleanup_ring = *source.cleanup_ring;
		const auto status = drain_status(detail::drain_cleanup_once(
			prepared.cleanup_ring,
			closure.data(), source.subject_count,
			prepared.cleanup_batch));
		if (status != CockpitCommonBuildStatus::Created)
			return status;
		prepared.input.cleanup_ring = nullptr;
		prepared.input.cleanup_batch = &prepared.cleanup_batch;
	}
	return CockpitCommonBuildStatus::Created;
}

void commit_cockpit_input(
	const CockpitSensorsStateImageInput& destination,
	const PreparedCockpitInput& prepared) noexcept
{
	if (prepared.has_cleanup_ring)
		*destination.cleanup_ring = prepared.cleanup_ring;
	if (prepared.has_support_ring)
		*destination.support_terminal_ring = prepared.support_ring;
	if (prepared.has_latches)
		*destination.episode_latches = prepared.latches;
}

std::uint64_t entity_for_capture(
	const ResolvedCockpitDomain& resolved,
	std::uint32_t capture_key) noexcept
{
	for (std::size_t index = 0U; index < resolved.subject_count; ++index)
		if (resolved.subjects[index].ship->capture_key.value == capture_key)
			return resolved.subjects[index].entity_id;
	return 0U;
}

CockpitCommonBuildStatus resolve_cockpit_domain(
	const CockpitSensorsStateImageInput& input,
	ResolvedCockpitDomain& resolved) noexcept
{
	resolved = {};
	const auto no_player_capture =
		input.observation != nullptr &&
		input.observation->capture.status == Phase2CaptureStatus::NoPlayer &&
		input.observation->capture.reason ==
			detail::Phase2CaptureReason::None &&
		input.observation->player_key.value == 0U &&
		input.observation->producer_sample_time_us == 0U &&
		input.observation->ships.empty() &&
		input.subject_count == 0U &&
		input.player_entity_id == 0U;
	if (input.observation == nullptr || input.installed_manifest == nullptr ||
		input.subject_count > detail::MaximumPhase2ObservationShips ||
		(input.subject_count != 0U && input.subjects == nullptr) ||
		(input.observation->capture.status != Phase2CaptureStatus::Valid &&
		 !no_player_capture) ||
		input.installed_manifest->manifest_id == 0U)
		return CockpitCommonBuildStatus::InvalidInput;
	if ((input.cleanup_ring != nullptr &&
			input.cleanup_ring->overflowed()) ||
		(input.support_terminal_ring != nullptr &&
			input.support_terminal_ring->overflowed()))
		return CockpitCommonBuildStatus::CapacityExceeded;
	if (input.subject_count == 0U)
		return input.player_entity_id == 0U
			? CockpitCommonBuildStatus::Created
			: CockpitCommonBuildStatus::InvalidInput;
	if (input.player_entity_id == 0U ||
		input.subject_count != input.observation->ships.size())
		return CockpitCommonBuildStatus::InvalidInput;

	bool player_seen = false;
	for (std::size_t binding_index = 0U;
		 binding_index < input.subject_count; ++binding_index) {
		const auto& binding = input.subjects[binding_index];
		if (binding.capture_key.value == 0U || binding.entity_id == 0U)
			return CockpitCommonBuildStatus::InvalidInput;
		for (std::size_t previous = 0U; previous < binding_index; ++previous)
			if (input.subjects[previous].capture_key.value ==
					binding.capture_key.value ||
				input.subjects[previous].entity_id == binding.entity_id)
				return CockpitCommonBuildStatus::InvalidInput;
		const auto* source = find_subject(
			*input.observation, binding.capture_key);
		if (source == nullptr ||
			source->identity.class_source_key.value == 0U ||
			source->docking.relation_count >
				detail::MaximumPhase2DockRelationsPerShip ||
			source->subsystems.count >
				Phase2ManifestLimits::MaxSubsystemsPerShip)
			return CockpitCommonBuildStatus::InvalidInput;
		const auto* ship_class = find_class(*input.installed_manifest,
			source->identity.class_source_key.value);
		if (ship_class == nullptr || ship_class->class_id == 0U ||
			ship_class->subsystem_count != source->subsystems.count)
			return CockpitCommonBuildStatus::SourceMappingMissing;
		auto& destination = resolved.subjects[resolved.subject_count++];
		destination.ship = source;
		destination.ship_class = ship_class;
		destination.entity_id = binding.entity_id;
		resolved.subsystem_count += source->subsystems.count;
		resolved.maximum_relations =
			std::max(resolved.maximum_relations,
				static_cast<std::size_t>(
					source->docking.relation_count));
		if (binding.capture_key.value ==
			input.observation->player_key.value) {
			if (binding.entity_id != input.player_entity_id)
				return CockpitCommonBuildStatus::InvalidInput;
			player_seen = true;
		}
	}
	if (!player_seen ||
		resolved.subsystem_count >
			Phase2ManifestLimits::MaxAggregateSubsystems)
		return CockpitCommonBuildStatus::InvalidInput;
	std::sort(resolved.subjects.begin(),
		resolved.subjects.begin() + resolved.subject_count,
		[](const ResolvedCockpitSubject& left,
			const ResolvedCockpitSubject& right) noexcept {
			return left.entity_id < right.entity_id;
		});

	for (std::size_t index = 0U; index < resolved.subject_count; ++index) {
		const auto& subject = resolved.subjects[index];
		for (std::size_t relation_index = 0U;
			 relation_index < subject.ship->docking.relation_count;
			 ++relation_index) {
			const auto& relation =
				subject.ship->docking.relations[relation_index];
			const auto remote_id = entity_for_capture(
				resolved, relation.remote_capture_key.value);
			if (remote_id == 0U || remote_id == subject.entity_id)
				return CockpitCommonBuildStatus::SourceMappingMissing;
			const ResolvedCockpitSubject* remote = nullptr;
			for (std::size_t r = 0U; r < resolved.subject_count; ++r)
				if (resolved.subjects[r].entity_id == remote_id)
					remote = &resolved.subjects[r];
			bool inverse = false;
			if (remote != nullptr)
				for (std::size_t r = 0U;
					 r < remote->ship->docking.relation_count; ++r) {
					const auto& candidate =
						remote->ship->docking.relations[r];
					inverse = inverse ||
						(candidate.remote_capture_key.value ==
							subject.ship->capture_key.value &&
						 candidate.local_dockpoint ==
							relation.remote_dockpoint &&
						 candidate.remote_dockpoint ==
							relation.local_dockpoint &&
						 candidate.local_dock_bay_name.view() ==
							relation.remote_dock_bay_name.view() &&
						 candidate.remote_dock_bay_name.view() ==
							relation.local_dock_bay_name.view());
				}
			if (!inverse)
				return CockpitCommonBuildStatus::InvalidInput;
		}
	}
	std::array<bool, detail::MaximumPhase2ObservationShips> visited{};
	std::array<std::size_t, detail::MaximumPhase2ObservationShips> stack{};
	for (std::size_t root = 0U; root < resolved.subject_count; ++root) {
		if (visited[root]) continue;
		std::array<std::size_t,
			detail::MaximumPhase2ObservationShips> members{};
		std::size_t member_count = 0U;
		std::size_t stack_count = 0U;
		stack[stack_count++] = root;
		visited[root] = true;
		std::uint64_t leader = 0U;
		while (stack_count != 0U) {
			const auto current = stack[--stack_count];
			members[member_count++] = current;
			const auto& subject = resolved.subjects[current];
			if (subject.ship->docking.dock_leader) {
				if (leader != 0U)
					return CockpitCommonBuildStatus::InvalidInput;
				leader = subject.entity_id;
			}
			for (std::size_t relation = 0U;
				 relation < subject.ship->docking.relation_count;
				 ++relation) {
				const auto remote_id = entity_for_capture(resolved,
					subject.ship->docking.relations[relation]
						.remote_capture_key.value);
				for (std::size_t peer = 0U;
					 peer < resolved.subject_count; ++peer)
					if (!visited[peer] &&
						resolved.subjects[peer].entity_id ==
							remote_id) {
						visited[peer] = true;
						stack[stack_count++] = peer;
					}
			}
		}
		for (std::size_t index = 0U; index < member_count; ++index)
			resolved.subjects[members[index]]
				.component_leader_entity_id = leader;
	}
	const auto& cargo = input.observation->player_cargo_scan;
	if ((cargo.presence &
			protocol::CargoScanStatePresenceFlagTarget) != 0U &&
		entity_for_capture(resolved,
			cargo.target_capture_key.value) == 0U)
		return CockpitCommonBuildStatus::SourceMappingMissing;
	return CockpitCommonBuildStatus::Created;
}

bool make_cockpit_identity(
	const CommonRecordInput& bridge,
	const ResolvedCockpitSubject& subject,
	const StateAtomKey& lifecycle_key,
	StateAtom& atom) noexcept
{
	if (!subject.ship->identity.internal_name.empty())
		return make_identity(bridge, *subject.ship,
			*subject.ship_class, lifecycle_key, atom);
	reset_atom(atom);
	std::array<std::uint8_t, IdentityPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(subject.entity_id) ||
		!writer.write_u64(0U) ||
		!writer.write_u64(
			bridge.observation->producer_sample_time_us) ||
		!writer.write_u32(subject.ship_class->class_id) ||
		!writer.write_utf8("ship", 255U) ||
		!writer.write_u32(subject.ship_class->species_id) ||
		!writer.write_u32(0U) ||
		!writer.write_u32(subject.ship_class->iff_id) ||
		!writer.write_u16(protocol::ShipRoleFlagMissionObject) ||
		!writer.write_f32(subject.ship->flight.radius))
		return false;
	set_entity_atom(atom, RecordType::ShipIdentity,
		subject.entity_id, lifecycle_key);
	return assign_payload(writer, atom);
}

bool make_docking(const CockpitSensorsStateImageInput& input,
	const ResolvedCockpitDomain& resolved,
	const ResolvedCockpitSubject& subject,
	std::uint64_t leader, StateAtom& atom) noexcept
{
	reset_atom(atom);
	const auto& docking = subject.ship->docking;
	std::array<std::uint8_t, DockingPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(subject.entity_id) ||
		!writer.write_u64(0U) ||
		!writer.write_u64(
			input.observation->producer_sample_time_us) ||
		!writer.write_u8(static_cast<std::uint8_t>(
			input.docking_phase_override == protocol::DockingPhase::None
				? docking.phase : input.docking_phase_override)) ||
		!writer.write_u64(leader) ||
		!writer.write_u16(docking.relation_count)) return false;
	std::array<std::size_t,
		detail::MaximumPhase2DockRelationsPerShip> order{};
	for (std::size_t index = 0U;
		 index < docking.relation_count; ++index) order[index] = index;
	std::sort(order.begin(), order.begin() + docking.relation_count,
		[&](std::size_t left, std::size_t right) {
			return entity_for_capture(resolved,
				docking.relations[left].remote_capture_key.value) <
				entity_for_capture(resolved,
					docking.relations[right].remote_capture_key.value);
		});
	for (std::size_t sorted = 0U;
		 sorted < docking.relation_count; ++sorted) {
		const auto& relation = docking.relations[order[sorted]];
		std::array<std::uint8_t, 288U> item_bytes{};
		PacketWriter item({item_bytes.data(), item_bytes.size()});
		if (!item.write_u64(entity_for_capture(resolved,
				relation.remote_capture_key.value)) ||
			!item.write_u16(relation.local_dockpoint) ||
			!item.write_u16(relation.remote_dockpoint) ||
			!item.write_utf8(
				relation.local_dock_bay_name.view(), 127U) ||
			!item.write_utf8(
				relation.remote_dock_bay_name.view(), 127U) ||
			!writer.write_u8(1U) ||
			!writer.write_u16(static_cast<std::uint16_t>(
				item.size())) ||
			!writer.write_bytes(item.written())) return false;
	}
	atom.key.record_type =
		static_cast<std::uint16_t>(RecordType::DockingState);
	return write_entity_identity(subject.entity_id, atom.key) &&
		assign_payload(writer, atom);
}

protocol::SupportPhase support_phase(
	detail::ShipSupportPhase phase) noexcept
{
	switch (phase) {
	case detail::ShipSupportPhase::Queued:
		return protocol::SupportPhase::Requested;
	case detail::ShipSupportPhase::OnWay:
		return protocol::SupportPhase::Approaching;
	case detail::ShipSupportPhase::Docking:
		return protocol::SupportPhase::Docking;
	case detail::ShipSupportPhase::Repairing:
		return protocol::SupportPhase::Repairing;
	case detail::ShipSupportPhase::Rearming:
		return protocol::SupportPhase::Rearming;
	case detail::ShipSupportPhase::Aborted:
		return protocol::SupportPhase::Aborted;
	case detail::ShipSupportPhase::Obstructed:
		return protocol::SupportPhase::Obstructed;
	case detail::ShipSupportPhase::None:
	default:
		return protocol::SupportPhase::None;
	}
}

bool make_support(const CockpitSensorsStateImageInput& input,
	const ResolvedCockpitDomain& resolved,
	const ResolvedCockpitSubject& subject, StateAtom& atom) noexcept
{
	reset_atom(atom);
	const auto& support = subject.ship->support;
	if (!finite_nonnegative(support.raw_hull_repair_work) ||
		!finite_nonnegative(support.raw_shield_repair_work) ||
		!finite_nonnegative(support.raw_subsystem_repair_work) ||
		!finite_nonnegative(support.raw_weapon_energy_rearm_work))
		return false;
	auto presence = support.presence;
	auto wire_phase = support_phase(support.phase);
	std::uint64_t support_id = 0U;
	if ((presence &
			protocol::SupportStatePresenceFlagSupportEntity) != 0U) {
		support_id = entity_for_capture(resolved,
			support.support_capture_key.value);
		if (support_id == 0U) return false;
	}
	if (input.episode_latches != nullptr) {
		const auto terminal = input.episode_latches->value(
			input.session_slot, subject.ship->capture_key.value);
		if (terminal.assisted_signature != 0U) {
			if (terminal.reason ==
				detail::SupportTransitionReason::Broken)
				wire_phase = protocol::SupportPhase::Obstructed;
			else if (terminal.reason ==
					detail::SupportTransitionReason::Abort ||
				terminal.reason ==
					detail::SupportTransitionReason::Killed)
				wire_phase = protocol::SupportPhase::Aborted;
			else
				wire_phase = protocol::SupportPhase::None;
		}
	}
	std::array<std::uint8_t, SupportPayloadCapacity> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(subject.entity_id) ||
		!writer.write_u64(presence) ||
		!writer.write_u64(
			input.observation->producer_sample_time_us) ||
		!writer.write_u8(static_cast<std::uint8_t>(wire_phase)) ||
		!writer.write_u8(static_cast<std::uint8_t>(
			support.raw_support_flags)) ||
		!writer.write_zeroes(3U) ||
		(support_id != 0U && !writer.write_u64(support_id)))
		return false;
	atom.key.record_type =
		static_cast<std::uint16_t>(RecordType::SupportState);
	return write_entity_identity(subject.entity_id, atom.key) &&
		assign_payload(writer, atom);
}

std::size_t cockpit_common_record_count(
	const ResolvedCockpitDomain& resolved) noexcept
{
	return resolved.subject_count == 0U ? 2U :
		4U + 10U * resolved.subject_count +
			resolved.subsystem_count;
}

void reserve_cockpit_common_payload_inventory(
	std::vector<StateAtom>& records,
	std::size_t maximum_subjects,
	std::size_t maximum_subsystems)
{
	std::size_t cursor = 0U;
	const auto reserve = [&](RecordType type, std::size_t count,
							 std::size_t capacity) {
		for (std::size_t index = 0U; index < count; ++index) {
			auto& record = records[cursor++];
			record.key.record_type =
				static_cast<std::uint16_t>(type);
			record.value.reserve(capacity);
		}
	};
	reserve(RecordType::SessionState, 1U, SessionPayloadCapacity);
	reserve(RecordType::MissionState, 1U, MissionPayloadCapacity);
	reserve(RecordType::ControlState, 1U, ControlPayloadCapacity);
	reserve(RecordType::CargoScanState, 1U, CargoPayloadCapacity);
	reserve(RecordType::EntityLifecycle, maximum_subjects,
		LifecyclePayloadCapacity);
	reserve(RecordType::ShipIdentity, maximum_subjects,
		IdentityPayloadCapacity);
	reserve(RecordType::FlightState, maximum_subjects,
		FlightPayloadCapacity);
	reserve(RecordType::DamageState, maximum_subjects,
		DamagePayloadCapacity);
	reserve(RecordType::ShieldState, maximum_subjects,
		ShieldPayloadCapacity);
	reserve(RecordType::EnergyState, maximum_subjects,
		EnergyPayloadCapacity);
	reserve(RecordType::PropulsionState, maximum_subjects,
		PropulsionPayloadCapacity);
	reserve(RecordType::WeaponState, maximum_subjects,
		WeaponPayloadCapacity);
	reserve(RecordType::DockingState, maximum_subjects,
		DockingPayloadCapacity);
	reserve(RecordType::SupportState, maximum_subjects,
		SupportPayloadCapacity);
	reserve(RecordType::SubsystemState, maximum_subsystems,
		SubsystemPayloadCapacity);
	// A specialized final-image pool may reserve additional records after the
	// complete-domain inventory. Those records are initialized by its owner.
	if (cursor > records.size()) throw std::bad_alloc();
}

bool normalize_cockpit_common_payload_backings(
	std::vector<StateAtom>& records,
	std::vector<StateAtom>& spares,
	std::size_t spare_count,
	const ResolvedCockpitDomain& resolved,
	std::size_t record_count = 0U) noexcept
{
	if (spare_count > spares.size())
		return false;
	std::array<RecordType, MaximumCockpitCommonRecords>
		required_types{};
	std::array<std::size_t, MaximumCockpitCommonRecords>
		required_capacities{};
	std::size_t count = 0U;
	const auto append = [&](RecordType type,
							std::size_t capacity) noexcept {
		if (count >= required_types.size()) return false;
		required_types[count] = type;
		required_capacities[count] = capacity;
		++count;
		return true;
	};
	if (!append(RecordType::SessionState,
			SessionPayloadCapacity) ||
		!append(RecordType::MissionState,
			MissionPayloadCapacity))
		return false;
	if (resolved.subject_count != 0U &&
		(!append(RecordType::ControlState,
			 ControlPayloadCapacity) ||
		 !append(RecordType::CargoScanState,
			 CargoPayloadCapacity)))
		return false;
	for (std::size_t subject_index = 0U;
		 subject_index < resolved.subject_count; ++subject_index) {
		const auto* ship = resolved.subjects[subject_index].ship;
		if (ship == nullptr ||
			!append(RecordType::EntityLifecycle,
				LifecyclePayloadCapacity) ||
			!append(RecordType::ShipIdentity,
				IdentityPayloadCapacity) ||
			!append(RecordType::FlightState,
				FlightPayloadCapacity) ||
			!append(RecordType::DamageState,
				DamagePayloadCapacity) ||
			!append(RecordType::ShieldState,
				ShieldPayloadCapacity) ||
			!append(RecordType::EnergyState,
				EnergyPayloadCapacity) ||
			!append(RecordType::PropulsionState,
				PropulsionPayloadCapacity))
			return false;
		for (std::size_t subsystem = 0U;
			 subsystem < ship->subsystems.count; ++subsystem)
			if (!append(RecordType::SubsystemState,
					SubsystemPayloadCapacity))
				return false;
		if (!append(RecordType::WeaponState,
				WeaponPayloadCapacity) ||
			!append(RecordType::DockingState,
				DockingPayloadCapacity) ||
			!append(RecordType::SupportState,
				SupportPayloadCapacity))
			return false;
	}
	if (record_count == 0U) record_count = records.size();
	if (count != record_count || record_count > records.size()) return false;

	for (std::size_t target = 0U; target < count; ++target) {
		const auto required_type =
			static_cast<std::uint16_t>(required_types[target]);
		const auto required_capacity =
			required_capacities[target];
		std::size_t source = records.size();
		std::size_t spare_source = spare_count;
		for (std::size_t candidate = target;
			 candidate < records.size(); ++candidate) {
			const auto& available = records[candidate];
			if (available.key.record_type == required_type &&
				available.value.capacity() >= required_capacity) {
				source = candidate;
				break;
			}
		}
		if (source == records.size())
			for (std::size_t candidate = 0U;
				 candidate < spare_count; ++candidate) {
				const auto& available = spares[candidate];
				if (available.key.record_type == required_type &&
					available.value.capacity() >=
						required_capacity) {
					spare_source = candidate;
					break;
				}
			}
		if (source < records.size()) {
			if (source != target)
				swap_atom_backing(
					records[target], records[source]);
		} else if (spare_source < spare_count) {
			swap_atom_backing(
				records[target], spares[spare_source]);
		} else {
			return false;
		}
	}
	return true;
}

bool refreshes_record(const CockpitSensorsStateImageInput& input,
	const StateAtom& atom) noexcept
{
	const auto type =
		static_cast<RecordType>(atom.key.record_type);
	switch (type) {
	case RecordType::FlightState:
	case RecordType::ControlState:
		return input.refresh_flight_controls;
	case RecordType::ShipIdentity:
	case RecordType::EntityLifecycle:
	case RecordType::DamageState:
	case RecordType::ShieldState:
	case RecordType::SubsystemState:
	case RecordType::EnergyState:
	case RecordType::PropulsionState:
	case RecordType::WeaponState:
	case RecordType::CargoScanState:
	case RecordType::DockingState:
	case RecordType::SupportState:
		return input.refresh_systems;
	default:
		// Session and mission remain tick-driven. Lifecycle is retained on a
		// FlightControls-only capture; a lifecycle invalidation forces All
		// upstream, while the systems capture owns the refreshed lifecycle
		// sample on ordinary cadence overlap.
		return true;
	}
}

bool copy_retained_record(const CockpitSensorsStateImageInput& input,
	RecordType type,
	std::uint64_t entity_id,
	StateAtom& output,
	std::uint32_t subsystem_id = 0U) noexcept
{
	if (input.retained_state == nullptr) return false;
	output.key.record_type = static_cast<std::uint16_t>(type);
	if (type == RecordType::SubsystemState
			? !write_subsystem_identity(
				entity_id, subsystem_id, output.key)
			: !write_entity_identity(entity_id, output.key))
		return false;
	const auto& retained = input.retained_state->records();
	const auto found = std::lower_bound(
		retained.begin(), retained.end(), output.key,
		[](const StateAtom& atom,
			const StateAtomKey& key) noexcept {
			return atom.key < key;
		});
	if (found == retained.end() ||
		found->key != output.key ||
		found->value.size() > output.value.capacity() ||
		found->cascade_owner.identity.size() >
			output.cascade_owner.identity.capacity())
		return false;
	output.value.resize(found->value.size());
	std::copy(found->value.begin(), found->value.end(),
		output.value.begin());
	output.record_version = found->record_version;
	output.lifecycle = found->lifecycle;
	output.has_cascade_owner = found->has_cascade_owner;
	output.cascade_owner.record_type =
		found->cascade_owner.record_type;
	output.cascade_owner.identity.resize(
		found->cascade_owner.identity.size());
	std::copy(found->cascade_owner.identity.begin(),
		found->cascade_owner.identity.end(),
		output.cascade_owner.identity.begin());
	return true;
}

bool retain_record_or_write_key(
	const CockpitSensorsStateImageInput& input,
	bool sparse_patch,
	RecordType type,
	std::uint64_t entity_id,
	StateAtom& output,
	std::uint32_t subsystem_id = 0U) noexcept
{
	if (!sparse_patch)
		return copy_retained_record(
			input, type, entity_id, output, subsystem_id);
	output.key.record_type = static_cast<std::uint16_t>(type);
	return type == RecordType::SubsystemState
		? write_subsystem_identity(
			  entity_id, subsystem_id, output.key)
		: write_entity_identity(entity_id, output.key);
}

CockpitCommonBuildStatus fill_cockpit_common_records(
	const CockpitSensorsStateImageInput& input,
	const ResolvedCockpitDomain& resolved,
	std::vector<StateAtom>& records,
	CockpitSensorsStateImageBuildDiagnostic* diagnostic = nullptr) noexcept
{
	const auto common_record_count = cockpit_common_record_count(resolved);
	if (records.size() < common_record_count)
		return CockpitCommonBuildStatus::CapacityExceeded;
	if (input.cleanup_batch != nullptr &&
		input.cleanup_batch->count >
			input.cleanup_batch->intents.size())
		return CockpitCommonBuildStatus::CapacityExceeded;
	const auto sample =
		input.observation->capture.status == Phase2CaptureStatus::NoPlayer
		? input.mission.producer_sample_time_us
		: input.observation->producer_sample_time_us;
	CommonRecordInput bridge{};
	bridge.producer_id = input.producer_id;
	bridge.negotiated_capability_generation =
		input.negotiated_capability_generation;
	bridge.session_phase = input.session_phase;
	bridge.mission = input.mission;
	if (bridge.mission.mission_generation == 0U)
		bridge.mission.mission_generation = 1U;
	bridge.mission.producer_sample_time_us = sample;
	bridge.observation = input.observation;
	bridge.installed_manifest = input.installed_manifest;
	if (!make_mission(bridge, records[1]))
		return CockpitCommonBuildStatus::InvalidInput;
	if (resolved.subject_count == 0U) {
		return CockpitCommonBuildStatus::Created;
	}

	auto controls = input.observation->player_controls;
	controls.sample_time_us = sample;
	controls.presence |=
		protocol::ControlStatePresenceFlagCruise |
		protocol::ControlStatePresenceFlagRequestCounters;
	if (!(input.refresh_flight_controls
			? make_control(input.player_entity_id, controls, records[2])
			: retain_record_or_write_key(input, false,
				RecordType::ControlState,
				input.player_entity_id, records[2])))
		return CockpitCommonBuildStatus::InvalidInput;

	std::size_t cursor = 4U;
	for (std::size_t subject_index = 0U;
		 subject_index < resolved.subject_count; ++subject_index) {
		const auto& subject = resolved.subjects[subject_index];
		bridge.player_entity_id = subject.entity_id;
		const auto lifecycle_index = cursor++;
		auto lifecycle_override = ShipLifecycleState::Count;
		if (input.cleanup_batch != nullptr) {
			for (std::size_t cleanup_index = 0U;
				 cleanup_index < input.cleanup_batch->count;
				 ++cleanup_index) {
				const auto intent =
					input.cleanup_batch->intents[cleanup_index];
				if (!intent.event_ready || !intent.fence_ready ||
					!intent.new_entity_id_request ||
					intent.event_order >= intent.purge_order ||
					intent.replacement_entity_id == 0U)
					return CockpitCommonBuildStatus::InvalidInput;
				for (std::size_t previous = 0U;
					 previous < cleanup_index; ++previous)
					if (input.cleanup_batch->intents[previous]
							.object_signature ==
						intent.object_signature)
						return CockpitCommonBuildStatus::InvalidInput;
				if (intent.object_signature !=
					subject.ship->capture_key.value) continue;
				switch (intent.mode) {
				case detail::ShipCleanupMode::Destroyed:
					lifecycle_override =
						ShipLifecycleState::Destroyed;
					break;
				case detail::ShipCleanupMode::Departed:
					lifecycle_override =
						ShipLifecycleState::Departed;
					break;
				case detail::ShipCleanupMode::Vanished:
					lifecycle_override =
						ShipLifecycleState::Vanished;
					break;
				case detail::ShipCleanupMode::Count:
					return CockpitCommonBuildStatus::InvalidInput;
				}
			}
		}
		if (!(input.refresh_systems
				? make_lifecycle(bridge, *subject.ship,
					*subject.ship_class,
					records[lifecycle_index],
					lifecycle_override)
				: retain_record_or_write_key(input, false,
					RecordType::EntityLifecycle,
					subject.entity_id,
					records[lifecycle_index])))
			return CockpitCommonBuildStatus::InvalidInput;
		const auto& lifecycle_key = records[lifecycle_index].key;
		auto& identity_record = records[cursor++];
		auto& flight_record = records[cursor++];
		auto& damage_record = records[cursor++];
		auto& shield_record = records[cursor++];
		auto& energy_record = records[cursor++];
		auto& propulsion_record = records[cursor++];
		if (!(input.refresh_systems
				? make_cockpit_identity(bridge, subject,
					lifecycle_key, identity_record)
				: retain_record_or_write_key(input, false,
					RecordType::ShipIdentity,
					subject.entity_id, identity_record)) ||
			!(input.refresh_flight_controls
				? make_flight(bridge, *subject.ship,
					lifecycle_key, flight_record)
				: retain_record_or_write_key(input, false,
					RecordType::FlightState,
					subject.entity_id, flight_record)) ||
			!(input.refresh_systems
				? make_damage(bridge, *subject.ship,
					*subject.ship_class, lifecycle_key, damage_record,
					true)
				: retain_record_or_write_key(input, false,
					RecordType::DamageState,
					subject.entity_id, damage_record)) ||
			!(input.refresh_systems
				? make_shields(bridge, *subject.ship,
					lifecycle_key, shield_record)
				: retain_record_or_write_key(input, false,
					RecordType::ShieldState,
					subject.entity_id, shield_record)) ||
			!(input.refresh_systems
				? make_energy(bridge, *subject.ship,
					lifecycle_key, energy_record)
				: retain_record_or_write_key(input, false,
					RecordType::EnergyState,
					subject.entity_id, energy_record)) ||
			!(input.refresh_systems
				? make_propulsion(bridge, *subject.ship,
					lifecycle_key, propulsion_record)
				: retain_record_or_write_key(input, false,
					RecordType::PropulsionState,
					subject.entity_id, propulsion_record)))
			return CockpitCommonBuildStatus::InvalidInput;
		for (std::size_t subsystem = 0U;
			 subsystem < subject.ship->subsystems.count; ++subsystem) {
			const auto& source =
				subject.ship->subsystems.values[subsystem];
			const auto* definition = find_subsystem(
				*subject.ship_class, source.source_key.value);
			auto& subsystem_record = records[cursor++];
			if (definition == nullptr ||
				!(input.refresh_systems
					? make_subsystem(bridge, source,
						*subject.ship_class, *definition,
						&lifecycle_key, subsystem_record)
					: retain_record_or_write_key(input, false,
						RecordType::SubsystemState,
						subject.entity_id, subsystem_record,
						definition->subsystem_id))) {
				if (diagnostic != nullptr) {
					diagnostic->stage =
						CockpitSensorsStateImageBuildStage::FillBusiness;
					diagnostic->record_type =
						static_cast<std::uint16_t>(
							RecordType::SubsystemState);
					diagnostic->record_index = subsystem;
				}
				return CockpitCommonBuildStatus::SourceMappingMissing;
			}
		}
		WeaponRecordInput weapon_input{};
		weapon_input.observation = input.observation;
		weapon_input.installed_manifest = input.installed_manifest;
		ResolvedWeaponSubject weapon_subject{
			subject.ship, subject.ship_class, subject.entity_id};
		auto& weapon_record = records[cursor++];
		if (!(input.refresh_systems
				? make_weapon_state(
					weapon_input, weapon_subject, weapon_record)
				: retain_record_or_write_key(input, false,
					RecordType::WeaponState,
					subject.entity_id, weapon_record))) {
			if (diagnostic != nullptr) {
				diagnostic->stage =
					CockpitSensorsStateImageBuildStage::FillBusiness;
				diagnostic->record_type =
					static_cast<std::uint16_t>(
						RecordType::WeaponState);
				diagnostic->record_index = subject_index;
			}
			return CockpitCommonBuildStatus::SourceMappingMissing;
		}
		auto& docking_record = records[cursor++];
		auto& support_record = records[cursor++];
		if (!(input.refresh_systems
				? make_docking(input, resolved, subject,
					subject.component_leader_entity_id,
					docking_record)
				: retain_record_or_write_key(input, false,
					RecordType::DockingState,
					subject.entity_id, docking_record)) ||
			!(input.refresh_systems
				? make_support(input, resolved, subject, support_record)
				: retain_record_or_write_key(input, false,
					RecordType::SupportState,
					subject.entity_id, support_record)))
			return CockpitCommonBuildStatus::InvalidInput;
	}
	if (cursor != common_record_count)
		return CockpitCommonBuildStatus::CapacityExceeded;
	for (std::size_t record_index = 0U;
		 record_index < common_record_count; ++record_index) {
		if (record_index == 0U || record_index == 3U)
			continue;
		const auto& record = records[record_index];
		if (input.retained_state != nullptr &&
			!refreshes_record(input, record))
			continue;
		protocol::BusinessRecordMetadata metadata{};
		const protocol::RecordEnvelopeView envelope{
			record.key.record_type,
			record.record_version,
			protocol::RecordFlagNone,
			{record.value.empty() ? nullptr : record.value.data(),
				record.value.size()}};
		const auto business_error =
			protocol::validate_business_record(envelope,
				protocol::BusinessRecordContainer::FullSnapshot,
				protocol::VersionMinor,
				metadata);
		if (business_error != protocol::ValidationError::None) {
			if (diagnostic != nullptr) {
				diagnostic->stage =
					CockpitSensorsStateImageBuildStage::FillBusiness;
				diagnostic->business_error = business_error;
				diagnostic->record_type = record.key.record_type;
				diagnostic->record_index = record_index;
			}
			return CockpitCommonBuildStatus::InvalidInput;
		}
		if (!normalize_entity_cascade_owner(records[record_index]))
			return CockpitCommonBuildStatus::CapacityExceeded;
	}
	if (input.omit_record_for_test.has_value()) {
		for (const auto& record : records)
			if (record.key.record_type ==
				static_cast<std::uint16_t>(
					*input.omit_record_for_test))
				return CockpitCommonBuildStatus::InvalidInput;
	}
	return CockpitCommonBuildStatus::Created;
}

} // namespace

bool CockpitSensorsStateImagePool::provision(
	std::size_t maximum_subjects,
	std::size_t maximum_subsystems,
	std::size_t maximum_dock_relations,
	std::size_t maximum_support_latches) noexcept
{
	if (maximum_subjects == 0U ||
		maximum_subjects > detail::MaximumPhase2ObservationShips ||
		maximum_subsystems > Phase2ManifestLimits::MaxAggregateSubsystems ||
		maximum_dock_relations >
			detail::MaximumPhase2DockRelationsPerShip ||
		maximum_support_latches >
			detail::Phase2Wp07EpisodeLatches::Capacity)
		return false;
	reset();
	const auto common_records =
		4U + 10U * maximum_subjects + maximum_subsystems;
	const auto maximum_records =
		common_records + 7U + MaximumPhase3Contacts;
	try {
		for (auto& slot : m_slots) {
			slot.records =
				std::make_shared<std::vector<StateAtom>>();
			slot.records->resize(maximum_records);
			slot.spares.resize(maximum_records);
			slot.spare_count = 0U;
			for (auto& atom : *slot.records) {
				atom.key.identity.reserve(12U);
				atom.cascade_owner.identity.reserve(8U);
			}
			reserve_cockpit_common_payload_inventory(
				*slot.records, maximum_subjects,
				maximum_subsystems);
		}
		// Keep the existing WPxx budget boundary unchanged in this lot. Its
		// historical state-image category prices the common image inventory;
		// step 6 of the architecture audit will replace that accounting with a
		// single inventory of the final product storage.
		m_owned_backing_bytes = owned_backing_bytes();
		for (auto& slot : m_slots) {
			(*slot.records)[3U].value.reserve(
				CockpitSensorsCargoPayloadCapacity);
			detail::reserve_cockpit_sensor_payload_inventory(
				*slot.records, common_records);
			auto& comm = (*slot.records)[common_records + 6U + MaximumPhase3Contacts];
			comm.key.record_type = static_cast<std::uint16_t>(protocol::RecordType::CommViewState);
			comm.value.reserve(protocol::CommViewStatePayloadSize);
		}
		m_maximum_subjects = maximum_subjects;
		m_maximum_subsystems = maximum_subsystems;
		m_maximum_dock_relations = maximum_dock_relations;
		m_maximum_support_latches = maximum_support_latches;
		m_ready = true;
		return true;
	} catch (const std::bad_alloc&) {
		reset();
		return false;
	}
}

void CockpitSensorsStateImagePool::reset() noexcept
{
	for (auto& slot : m_slots) {
		slot.records.reset();
		slot.spares.clear();
		slot.spares.shrink_to_fit();
		slot.spare_count = 0U;
	}
	m_maximum_subjects = 0U;
	m_maximum_subsystems = 0U;
	m_maximum_dock_relations = 0U;
	m_maximum_support_latches = 0U;
	m_owned_backing_bytes = 0U;
	m_ready = false;
}

std::size_t CockpitSensorsStateImagePool::owned_backing_bytes() const noexcept
{
	if (m_owned_backing_bytes != 0U) return m_owned_backing_bytes;
	std::size_t total = 0U;
	for (const auto& slot : m_slots) {
		const auto add_atoms = [&total](
			const std::vector<StateAtom>& atoms) noexcept {
			total += atoms.capacity() * sizeof(StateAtom);
			for (const auto& atom : atoms)
				total += atom.key.identity.capacity() +
					atom.cascade_owner.identity.capacity() +
					atom.value.capacity();
		};
		if (slot.records) add_atoms(*slot.records);
		add_atoms(slot.spares);
	}
	return total;
}

Phase3StateImageBuildStatus build_cockpit_sensors_state_image_preallocated(
	const CockpitSensorsStateImageInput& input,
	CockpitSensorsStateImagePool& pool,
	const Phase3Projection& projection,
	protocol::StateImage& output,
	CockpitSensorsStateImageBuildDiagnostic* diagnostic,
	CockpitSensorsStateImageRebuildSet* rebuilt) noexcept
{
	if (diagnostic != nullptr) *diagnostic = {};
	if (rebuilt != nullptr) *rebuilt = {};
	if (!pool.ready() ||
		projection.player_entity_id != input.player_entity_id ||
		projection.lock_count > MaximumPhase3Locks ||
		projection.contact_count > MaximumPhase3Contacts ||
		projection.threat.incoming_missile_count >
			MaximumPhase3IncomingMissiles ||
		projection.navigation.navpoint_count > MaximumPhase3Navpoints ||
		projection.navigation.route_waypoint_count >
			MaximumPhase3RouteWaypoints)
		return Phase3StateImageBuildStatus::InvalidInput;
	PreparedCockpitInput prepared{};
	if (const auto status =
			prepare_cockpit_input(input, prepared);
		status != CockpitCommonBuildStatus::Created) {
		if (diagnostic != nullptr)
			diagnostic->stage = CockpitSensorsStateImageBuildStage::Prepare;
		return status == CockpitCommonBuildStatus::CapacityExceeded
			? Phase3StateImageBuildStatus::CapacityExceeded
			: Phase3StateImageBuildStatus::InvalidInput;
	}
	ResolvedCockpitDomain resolved{};
	if (const auto status =
			resolve_cockpit_domain(prepared.input, resolved);
		status != CockpitCommonBuildStatus::Created) {
		if (diagnostic != nullptr)
			diagnostic->stage = CockpitSensorsStateImageBuildStage::Resolve;
		return status == CockpitCommonBuildStatus::CapacityExceeded
			? Phase3StateImageBuildStatus::CapacityExceeded
			: Phase3StateImageBuildStatus::InvalidInput;
	}
	if (resolved.subject_count > pool.m_maximum_subjects ||
		resolved.subsystem_count > pool.m_maximum_subsystems ||
		resolved.maximum_relations > pool.m_maximum_dock_relations ||
		input.expected_cargo_authority_generation !=
			input.cargo_authority_generation)
		return Phase3StateImageBuildStatus::CapacityExceeded;
	CockpitSensorsStateImagePool::Slot* slot = nullptr;
	for (auto& candidate : pool.m_slots)
		if (candidate.records && candidate.records.use_count() == 1L) {
			slot = &candidate;
			break;
		}
	if (slot == nullptr)
		return Phase3StateImageBuildStatus::AllocationFailed;
	const auto common_record_count = cockpit_common_record_count(resolved);
	const auto sensor_record_count = projection.player_entity_id == 0U
		? 0U
		: 6U + projection.contact_count;
	const auto communication_record_count = input.communication_view_state == nullptr ? 0U : 1U;
	const auto final_record_count =
		common_record_count + sensor_record_count + communication_record_count;
	if (slot->records->size() + slot->spare_count < final_record_count)
		return Phase3StateImageBuildStatus::CapacityExceeded;
	while (slot->records->size() > final_record_count) {
		if (slot->spare_count >= slot->spares.size())
			return Phase3StateImageBuildStatus::CapacityExceeded;
		swap_atom_backing(slot->spares[slot->spare_count++],
			slot->records->back());
		slot->records->pop_back();
	}
	while (slot->records->size() < final_record_count) {
		if (slot->spare_count == 0U)
			return Phase3StateImageBuildStatus::CapacityExceeded;
		slot->records->emplace_back();
		swap_atom_backing(slot->records->back(),
			slot->spares[--slot->spare_count]);
	}
	if (!normalize_cockpit_common_payload_backings(
			*slot->records, slot->spares, slot->spare_count,
			resolved, common_record_count) ||
		(sensor_record_count != 0U &&
			!detail::normalize_cockpit_sensor_payload_backings(
				*slot->records, slot->spares,
				slot->spare_count, common_record_count,
				projection.contact_count,
				input.communication_view_state != nullptr)))
		return Phase3StateImageBuildStatus::CapacityExceeded;
	if (const auto status = fill_cockpit_common_records(
			prepared.input, resolved, *slot->records,
			diagnostic);
		status != CockpitCommonBuildStatus::Created) {
		if (diagnostic != nullptr &&
			diagnostic->stage == CockpitSensorsStateImageBuildStage::None)
			diagnostic->stage = CockpitSensorsStateImageBuildStage::FillBusiness;
		return status == CockpitCommonBuildStatus::CapacityExceeded
			? Phase3StateImageBuildStatus::CapacityExceeded
			: Phase3StateImageBuildStatus::InvalidInput;
	}
	if (!detail::fill_cockpit_sensor_records(
			input, projection, *slot->records,
			common_record_count))
		return Phase3StateImageBuildStatus::EncodingFailed;
	sort_atoms(*slot->records);
	for (std::size_t index = 0U; index < slot->records->size(); ++index) {
		auto& record = (*slot->records)[index];
		protocol::BusinessRecordMetadata metadata{};
		const protocol::RecordEnvelopeView envelope{
			record.key.record_type, record.record_version,
			protocol::RecordFlagNone,
			{record.value.empty() ? nullptr : record.value.data(),
				record.value.size()}};
		const auto error = protocol::validate_business_record(
			envelope,
			protocol::BusinessRecordContainer::FullSnapshot,
			protocol::VersionMinor, metadata);
		if (error != protocol::ValidationError::None) {
			if (diagnostic != nullptr) {
				diagnostic->stage =
					CockpitSensorsStateImageBuildStage::FillBusiness;
				diagnostic->business_error = error;
				diagnostic->record_type = record.key.record_type;
				diagnostic->record_index = index;
			}
			return Phase3StateImageBuildStatus::EncodingFailed;
		}
		if (!normalize_entity_cascade_owner(record))
			return Phase3StateImageBuildStatus::CapacityExceeded;
	}
	protocol::StateImage candidate;
	protocol::StateImageInvalidRecordReason reason{};
	std::shared_ptr<const std::vector<StateAtom>> immutable =
		slot->records;
	const auto adopted = protocol::StateImage::adopt_preallocated(
		immutable, candidate, reason);
	if (adopted == protocol::StateImageResult::AllocationFailed)
		return Phase3StateImageBuildStatus::AllocationFailed;
	if (adopted != protocol::StateImageResult::Created) {
		if (diagnostic != nullptr) {
			diagnostic->stage = CockpitSensorsStateImageBuildStage::Adopt;
			diagnostic->structural_reason = reason;
		}
		return Phase3StateImageBuildStatus::EncodingFailed;
	}
	if (rebuilt != nullptr) {
		if (candidate.records().size() >
			rebuilt->canonical_indices.size())
			return Phase3StateImageBuildStatus::CapacityExceeded;
		rebuilt->count = candidate.records().size();
		for (std::size_t index = 0U; index < rebuilt->count; ++index)
			rebuilt->canonical_indices[index] =
				static_cast<std::uint16_t>(index);
		rebuilt->exhaustive = true;
	}
	if (input.cargo_authority_generation != 0U &&
		input.m_last_consumed_cargo_generation !=
			input.cargo_authority_generation) {
		input.m_last_consumed_cargo_generation =
			input.cargo_authority_generation;
		++input.m_cargo_authority_consume_count;
	}
	commit_cockpit_input(input, prepared);
	output = std::move(candidate);
	return Phase3StateImageBuildStatus::Created;
}

} // namespace telemetry
