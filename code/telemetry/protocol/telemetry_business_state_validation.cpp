#include "telemetry/protocol/telemetry_business_state_validation.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_capabilities.h"

#include <algorithm>
#include <array>
#include <new>
#include <string_view>

namespace telemetry::protocol {

namespace {

struct SessionFacts {
	std::uint64_t presence = 0;
	std::uint64_t producer_id = 0;
	std::uint64_t producer_sample_time_us = 0;
	AuthorityMode authority_mode = AuthorityMode::Solo;
	VisibilityMode visibility_mode = VisibilityMode::Cockpit;
	std::uint32_t capability_generation = 0;
	std::uint64_t capabilities = 0;
	std::uint64_t coverage = 0;
	std::uint64_t derived_events = 0;
	std::uint64_t exact_events = 0;
	std::uint64_t observed_entity_id = 0;
};

struct LifecycleFacts {
	std::uint64_t entity_id = 0;
	std::uint64_t presence = 0;
	ObjectType object_type = ObjectType::Unknown;
	std::uint32_t lifecycle_flags = 0;
	bool has_class = false;
	std::uint32_t class_id = 0;
	std::uint64_t parent_entity_id = 0;
};

struct TargetFacts {
	std::uint64_t presence = 0;
	std::uint64_t current_target = 0;
	std::uint64_t previous_target = 0;
	ObjectType revealed_object_type = ObjectType::Unknown;
	std::uint32_t revealed_class_id = 0;
	std::uint32_t target_subsystem_id = 0;
	std::uint32_t lock_subsystem_id = 0;
	std::uint64_t attacker_entity_id = 0;
	std::uint64_t dangerous_weapon_entity_id = 0;
	std::uint64_t nearest_locked_entity_id = 0;
};

struct RadarContactFacts {
	std::uint64_t observer_entity_id = 0;
	std::uint64_t contact_entity_id = 0;
	std::uint64_t presence = 0;
	ObjectType object_type = ObjectType::Unknown;
	std::uint32_t flags = 0;
	std::uint32_t revealed_class_id = 0;
};

struct DockingRelationFacts {
	std::uint64_t remote_entity_id = 0;
	std::uint16_t local_dockpoint_index = 0;
	std::uint16_t remote_dockpoint_index = 0;
	std::string_view local_dockpoint_name;
	std::string_view remote_dockpoint_name;
};

struct DockingFacts {
	std::uint64_t owner_entity_id = 0;
	std::uint64_t group_leader_entity_id = 0;
	std::uint16_t relation_count = 0;
	std::array<DockingRelationFacts, 64> relations{};
};

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept
{
	return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
		   (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t read_u64(const std::uint8_t* bytes) noexcept
{
	std::uint64_t result = 0;
	for (std::size_t index = 0; index < 8U; ++index) {
		result |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
	}
	return result;
}

std::array<std::uint8_t, 12> encoded_key(std::uint64_t entity_id, std::uint32_t secondary = 0) noexcept
{
	std::array<std::uint8_t, 12> result{};
	for (std::size_t index = 0; index < 8U; ++index) {
		result[index] = static_cast<std::uint8_t>(entity_id >> (index * 8U));
	}
	for (std::size_t index = 0; index < 4U; ++index) {
		result[8U + index] = static_cast<std::uint8_t>(secondary >> (index * 8U));
	}
	return result;
}

struct StateAtomKeyView {
	std::uint16_t record_type = 0;
	const std::uint8_t* identity = nullptr;
	std::size_t identity_size = 0;
};

bool state_atom_key_less(const StateAtomKey& left, const StateAtomKeyView& right) noexcept
{
	if (left.record_type != right.record_type) {
		return left.record_type < right.record_type;
	}
	const auto shared_size = std::min(left.identity.size(), right.identity_size);
	for (std::size_t index = 0; index < shared_size; ++index) {
		if (left.identity[index] != right.identity[index]) {
			return left.identity[index] < right.identity[index];
		}
	}
	return left.identity.size() < right.identity_size;
}

bool state_atom_key_equal(const StateAtomKey& left, const StateAtomKeyView& right) noexcept
{
	return left.record_type == right.record_type && left.identity.size() == right.identity_size &&
		   (right.identity_size == 0U ||
			std::equal(left.identity.begin(), left.identity.end(), right.identity));
}

const StateAtom* find_atom(const std::vector<StateAtom>& atoms,
	RecordType type,
	const std::uint8_t* identity,
	std::size_t identity_size) noexcept
{
	const StateAtomKeyView searched{static_cast<std::uint16_t>(type), identity, identity_size};
	const auto position = std::lower_bound(atoms.begin(), atoms.end(), searched, [](const StateAtom& atom,
		const StateAtomKeyView& key) { return state_atom_key_less(atom.key, key); });
	return position != atoms.end() && state_atom_key_equal(position->key, searched) ? &*position : nullptr;
}

const StateAtom* find_singleton(const std::vector<StateAtom>& atoms, RecordType type) noexcept
{
	return find_atom(atoms, type, nullptr, 0);
}

const StateAtom* find_owner(const std::vector<StateAtom>& atoms, RecordType type, std::uint64_t entity_id) noexcept
{
	const auto key = encoded_key(entity_id);
	return find_atom(atoms, type, key.data(), 8U);
}

const StateAtom* find_subsystem(const std::vector<StateAtom>& atoms,
	std::uint64_t entity_id,
	std::uint32_t subsystem_id) noexcept
{
	const auto key = encoded_key(entity_id, subsystem_id);
	return find_atom(atoms, RecordType::SubsystemState, key.data(), key.size());
}

bool has_record_type(const std::vector<StateAtom>& atoms, RecordType type) noexcept
{
	const auto raw_type = static_cast<std::uint16_t>(type);
	const auto position =
		std::lower_bound(atoms.begin(), atoms.end(), raw_type, [](const StateAtom& atom, std::uint16_t value) {
			return atom.key.record_type < value;
		});
	return position != atoms.end() && position->key.record_type == raw_type;
}

ValidationError parse_session(const StateAtom& atom, SessionFacts& facts) noexcept
{
	if (atom.value.size() != 64U && atom.value.size() != 72U) {
		return ValidationError::BadRecordLength;
	}
	const auto* bytes = atom.value.data();
	facts.presence = read_u64(bytes);
	facts.producer_id = read_u64(bytes + 8U);
	facts.producer_sample_time_us = read_u64(bytes + 16U);
	facts.authority_mode = static_cast<AuthorityMode>(bytes[24U]);
	facts.visibility_mode = static_cast<VisibilityMode>(bytes[25U]);
	facts.capability_generation = read_u32(bytes + 28U);
	facts.capabilities = read_u64(bytes + 32U);
	facts.coverage = read_u64(bytes + 40U);
	facts.derived_events = read_u64(bytes + 48U);
	facts.exact_events = read_u64(bytes + 56U);
	facts.observed_entity_id = (facts.presence & 1U) != 0U ? read_u64(bytes + 64U) : 0U;
	return ValidationError::None;
}

ValidationError parse_mission_generation(const StateAtom& atom, std::uint32_t& generation) noexcept
{
	if (atom.value.size() < 12U) {
		return ValidationError::BadRecordLength;
	}
	generation = read_u32(atom.value.data() + 8U);
	return generation != 0U ? ValidationError::None : ValidationError::OutOfRange;
}

bool is_phase1_player_kinematics_profile(std::uint8_t protocol_minor, std::uint64_t coverage) noexcept
{
	return protocol_minor == VersionMinorV1_1 && coverage == StateDomainCoverageBitPlayerKinematics;
}

bool is_phase2_complete_ship_profile(std::uint8_t protocol_minor, std::uint64_t coverage) noexcept
{
	constexpr auto complete_ship_coverage = StateDomainCoverageBitPlayerKinematics |
		StateDomainCoverageBitCoreShip | StateDomainCoverageBitControlInputs | StateDomainCoverageBitWeapons |
		StateDomainCoverageBitCargoDockSupport;
	static_assert(complete_ship_coverage == 0x0583ULL, "The Phase 2 complete ship coverage is frozen");
	return protocol_minor == VersionMinorV1_1 && coverage == complete_ship_coverage;
}

bool is_phase3_cockpit_sensors_profile(std::uint8_t protocol_minor,
	std::uint64_t coverage) noexcept
{
	constexpr auto cockpit_sensors_coverage =
		StateDomainCoverageBitPlayerKinematics |
		StateDomainCoverageBitCoreShip |
		StateDomainCoverageBitControlInputs |
		StateDomainCoverageBitRadarSensors |
		StateDomainCoverageBitTargeting |
		StateDomainCoverageBitWeapons |
		StateDomainCoverageBitCargoDockSupport |
		StateDomainCoverageBitNavigation;
	static_assert(cockpit_sensors_coverage == 0x07cbULL,
		"The Phase 3 cockpit sensor coverage is frozen");
	return protocol_minor == VersionMinorV1_1 &&
		coverage == cockpit_sensors_coverage;
}

constexpr std::size_t MaximumPhase2CompleteShipCount = 64U;

ValidationError parse_lifecycle(const StateAtom& atom, LifecycleFacts& facts) noexcept
{
	facts = LifecycleFacts{};
	PacketReader reader(ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()});
	std::uint64_t sample_time = 0;
	std::uint8_t object_type = 0;
	std::uint8_t lifecycle_phase = 0;
	if (!reader.read_u64(facts.entity_id) || !reader.read_u64(facts.presence) || !reader.read_u64(sample_time) ||
		!reader.read_u8(object_type) || !reader.read_u8(lifecycle_phase) ||
		!reader.read_u32(facts.lifecycle_flags)) {
		return ValidationError::BadRecordLength;
	}
	facts.object_type = static_cast<ObjectType>(object_type);
	std::uint32_t ignored_u32 = 0;
	if ((facts.presence & EntityLifecyclePresenceFlagSignature) != 0U && !reader.read_u32(ignored_u32)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & EntityLifecyclePresenceFlagNetSignature) != 0U && !reader.read_u32(ignored_u32)) {
		return ValidationError::BadRecordLength;
	}
	facts.has_class = (facts.presence & EntityLifecyclePresenceFlagClassReference) != 0U;
	if (facts.has_class && !reader.read_u32(facts.class_id)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & EntityLifecyclePresenceFlagParent) != 0U &&
		!reader.read_u64(facts.parent_entity_id)) {
		return ValidationError::BadRecordLength;
	}
	return ValidationError::None;
}

bool read_vlist_item(PacketReader& reader, PacketReader& item) noexcept
{
	std::uint8_t version = 0;
	std::uint16_t size = 0;
	return reader.read_u8(version) && version == 1U && reader.read_u16(size) && reader.subreader(size, item);
}

ValidationError parse_target(const StateAtom& atom, TargetFacts& facts) noexcept
{
	facts = TargetFacts{};
	PacketReader reader(ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()});
	std::uint64_t owner = 0;
	std::uint64_t sample_time = 0;
	if (!reader.read_u64(owner) || !reader.read_u64(facts.presence) || !reader.read_u64(sample_time) ||
		!reader.read_u64(facts.current_target)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagPreviousTarget) != 0U &&
		!reader.read_u64(facts.previous_target)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagRevealedIdentity) != 0U) {
		std::uint8_t object_type = 0;
		std::string_view ignored_name;
		std::uint32_t ignored_u32 = 0;
		if (!reader.read_u8(object_type) || !reader.read_utf8(255U, ignored_name) ||
			!reader.read_u32(facts.revealed_class_id) || !reader.read_u32(ignored_u32) ||
			!reader.read_u32(ignored_u32)) {
			return ValidationError::BadRecordLength;
		}
		facts.revealed_object_type = static_cast<ObjectType>(object_type);
	}
	if ((facts.presence & TargetStatePresenceFlagTimeOnTarget) != 0U && !reader.skip(8U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagTargetSubsystem) != 0U &&
		!reader.read_u32(facts.target_subsystem_id)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagLockSubsystem) != 0U &&
		!reader.read_u32(facts.lock_subsystem_id)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagLastStealthObservation) != 0U && !reader.skip(24U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagDistanceTrend) != 0U && !reader.skip(1U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagSpeedTrend) != 0U && !reader.skip(1U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagInCone) != 0U && !reader.skip(1U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagLead) != 0U && !reader.skip(16U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagAttacker) != 0U &&
		!reader.read_u64(facts.attacker_entity_id)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagDangerousWeapon) != 0U &&
		!reader.read_u64(facts.dangerous_weapon_entity_id)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagNearestLocked) != 0U &&
		!reader.read_u64(facts.nearest_locked_entity_id)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagExactHudDistance) != 0U && !reader.skip(4U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagExactHudSpeed) != 0U && !reader.skip(4U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & TargetStatePresenceFlagHudTypeLabel) != 0U) {
		std::string_view ignored_label;
		if (!reader.read_utf8(255U, ignored_label)) {
			return ValidationError::BadRecordLength;
		}
	}
	return reader.at_end() ? ValidationError::None : ValidationError::BadRecordLength;
}

ValidationError parse_radar_contact(const StateAtom& atom, RadarContactFacts& facts) noexcept
{
	facts = RadarContactFacts{};
	PacketReader reader(ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()});
	std::uint64_t sample_time = 0;
	std::uint8_t object_type = 0;
	std::uint8_t ignored_u8 = 0;
	if (!reader.read_u64(facts.observer_entity_id) || !reader.read_u64(facts.contact_entity_id) ||
		!reader.read_u64(facts.presence) || !reader.read_u64(sample_time) || !reader.read_u8(object_type) ||
		!reader.read_u8(ignored_u8) || !reader.read_u8(ignored_u8) ||
		!reader.skip(atom.record_version >= 2U ? 44U : 28U) ||
		!reader.read_u32(facts.flags)) {
		return ValidationError::BadRecordLength;
	}
	facts.object_type = static_cast<ObjectType>(object_type);
	if ((facts.presence & RadarContactsPresenceFlagIconSize) != 0U && !reader.skip(4U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & RadarContactsPresenceFlagRevealedName) != 0U) {
		std::string_view ignored_name;
		if (!reader.read_utf8(255U, ignored_name)) {
			return ValidationError::BadRecordLength;
		}
	}
	if ((facts.presence & RadarContactsPresenceFlagRevealedClass) != 0U &&
		!reader.read_u32(facts.revealed_class_id)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & RadarContactsPresenceFlagRevealedTeamIff) != 0U && !reader.skip(8U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & RadarContactsPresenceFlagDetectionTimes) != 0U && !reader.skip(16U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & RadarContactsPresenceFlagConfidence) != 0U && !reader.skip(4U)) {
		return ValidationError::BadRecordLength;
	}
	if ((facts.presence & RadarContactsPresenceFlagHudTypeLabel) != 0U) {
		std::string_view ignored_label;
		if (!reader.read_utf8(255U, ignored_label)) {
			return ValidationError::BadRecordLength;
		}
	}
	return reader.at_end() ? ValidationError::None : ValidationError::BadRecordLength;
}

ValidationError parse_docking(const StateAtom& atom, DockingFacts& facts) noexcept
{
	facts = DockingFacts{};
	PacketReader reader(ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()});
	std::uint64_t presence = 0;
	std::uint64_t sample_time = 0;
	std::uint8_t phase = 0;
	if (!reader.read_u64(facts.owner_entity_id) || !reader.read_u64(presence) ||
		!reader.read_u64(sample_time) || !reader.read_u8(phase) ||
		!reader.read_u64(facts.group_leader_entity_id) || !reader.read_u16(facts.relation_count) ||
		facts.relation_count > facts.relations.size()) {
		return ValidationError::BadRecordLength;
	}
	for (std::size_t index = 0; index < facts.relation_count; ++index) {
		PacketReader item;
		auto& relation = facts.relations[index];
		if (!read_vlist_item(reader, item) || !item.read_u64(relation.remote_entity_id) ||
			!item.read_u16(relation.local_dockpoint_index) ||
			!item.read_u16(relation.remote_dockpoint_index) ||
			!item.read_utf8(127U, relation.local_dockpoint_name) ||
			!item.read_utf8(127U, relation.remote_dockpoint_name) || !item.at_end()) {
			return ValidationError::BadRecordLength;
		}
	}
	return reader.at_end() ? ValidationError::None : ValidationError::BadRecordLength;
}

bool sorted_unique_nonzero(const std::uint32_t* values, std::size_t count) noexcept
{
	if (count != 0 && values == nullptr) {
		return false;
	}
	for (std::size_t index = 0; index < count; ++index) {
		if (values[index] == 0 || (index != 0 && values[index - 1U] >= values[index])) {
			return false;
		}
	}
	return true;
}

ValidationError validate_context(const BusinessStateValidationContext& context) noexcept
{
	if (!is_supported_version_minor(context.protocol_minor)) {
		return ValidationError::UnsupportedMinor;
	}
	if ((context.class_catalog_count != 0 && context.class_catalog == nullptr) ||
		(context.weapon_class_count != 0 && context.weapon_class_ids == nullptr) ||
		(context.cockpit_entity_count != 0 && context.cockpit_entity_ids == nullptr) ||
		!sorted_unique_nonzero(context.weapon_class_ids, context.weapon_class_count)) {
		return ValidationError::InvalidStateTransition;
	}
	for (std::size_t index = 0; index < context.class_catalog_count; ++index) {
		const auto& entry = context.class_catalog[index];
		if (entry.class_id == 0 || (index != 0 && context.class_catalog[index - 1U].class_id >= entry.class_id) ||
			!sorted_unique_nonzero(entry.subsystem_ids, entry.subsystem_count)) {
			return ValidationError::InvalidStateTransition;
		}
	}
	if (context.weapon_class_flags != nullptr) {
		for (std::size_t index = 0; index < context.weapon_class_count; ++index) {
			if ((context.weapon_class_flags[index] & ReservedWeaponClassFlags) != 0U) {
				return ValidationError::InvalidStateTransition;
			}
		}
	}
	for (std::size_t index = 0; index < context.cockpit_entity_count; ++index) {
		if (context.cockpit_entity_ids[index] == 0 ||
			(index != 0 && context.cockpit_entity_ids[index - 1U] >= context.cockpit_entity_ids[index])) {
			return ValidationError::InvalidStateTransition;
		}
	}
	return ValidationError::None;
}

const BusinessClassCatalogEntry*
find_class(const BusinessStateValidationContext& context, std::uint32_t class_id) noexcept
{
	if (context.class_catalog_count == 0) {
		return nullptr;
	}
	const auto begin = context.class_catalog;
	const auto end = begin + context.class_catalog_count;
	const auto position = std::lower_bound(begin, end, class_id, [](const BusinessClassCatalogEntry& entry,
		std::uint32_t value) { return entry.class_id < value; });
	return position != end && position->class_id == class_id ? position : nullptr;
}

bool has_weapon_class(const BusinessStateValidationContext& context, std::uint32_t class_id) noexcept
{
	return context.weapon_class_ids != nullptr &&
		   std::binary_search(
			   context.weapon_class_ids, context.weapon_class_ids + context.weapon_class_count, class_id);
}

const std::uint64_t*
find_weapon_class_flags(const BusinessStateValidationContext& context, std::uint32_t class_id) noexcept
{
	if (context.weapon_class_ids == nullptr || context.weapon_class_flags == nullptr) {
		return nullptr;
	}
	const auto position = std::lower_bound(
		context.weapon_class_ids, context.weapon_class_ids + context.weapon_class_count, class_id);
	if (position == context.weapon_class_ids + context.weapon_class_count || *position != class_id) {
		return nullptr;
	}
	return context.weapon_class_flags + (position - context.weapon_class_ids);
}

bool cockpit_entity_allowed(const BusinessStateValidationContext& context, std::uint64_t entity_id) noexcept
{
	return context.cockpit_entity_ids != nullptr && std::binary_search(context.cockpit_entity_ids,
		context.cockpit_entity_ids + context.cockpit_entity_count,
		entity_id);
}

ValidationError validate_entity_reference(const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode,
	std::uint64_t entity_id,
	bool sensor_identity_allowed = false) noexcept
{
	if (entity_id == 0) {
		return ValidationError::UnknownEntity;
	}
	if (find_owner(atoms, RecordType::EntityLifecycle, entity_id) == nullptr &&
		!sensor_identity_allowed) {
		return ValidationError::UnknownEntity;
	}
	if (visibility_mode == VisibilityMode::Cockpit && context.enforce_cockpit_entity_allowlist &&
		!cockpit_entity_allowed(context, entity_id)) {
		return ValidationError::VisibilityViolation;
	}
	return ValidationError::None;
}

ValidationError lifecycle_for_entity(const std::vector<StateAtom>& atoms,
	std::uint64_t entity_id,
	LifecycleFacts& facts) noexcept
{
	const auto* lifecycle = find_owner(atoms, RecordType::EntityLifecycle, entity_id);
	return lifecycle == nullptr ? ValidationError::UnknownEntity : parse_lifecycle(*lifecycle, facts);
}

ValidationError validate_subsystem_reference(const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode,
	std::uint64_t entity_id,
	std::uint32_t subsystem_id,
	bool sensor_identity_allowed = false) noexcept
{
	if (const auto error = validate_entity_reference(
			atoms, context, visibility_mode, entity_id,
			sensor_identity_allowed);
		error != ValidationError::None) {
		return error;
	}
	if (find_owner(atoms, RecordType::EntityLifecycle, entity_id) == nullptr) {
		// Sensor-only identities deliberately have no ENTITY_LIFECYCLE or
		// SUBSYSTEM_STATE. The non-zero subsystem key remains an opaque public
		// identity whose catalog resolution is enforced by the producer.
		return sensor_identity_allowed && subsystem_id != 0U
			? ValidationError::None
			: ValidationError::UnknownEntity;
	}
	if (find_subsystem(atoms, entity_id, subsystem_id) == nullptr) {
		return ValidationError::UnknownEntity;
	}
	LifecycleFacts lifecycle;
	if (const auto error = lifecycle_for_entity(atoms, entity_id, lifecycle); error != ValidationError::None) {
		return error;
	}
	if (lifecycle.object_type != ObjectType::Ship || !lifecycle.has_class) {
		return ValidationError::InvalidStateTransition;
	}
	const auto* class_entry = find_class(context, lifecycle.class_id);
	if (!context.class_manifest_installed || class_entry == nullptr) {
		return ValidationError::MissingManifest;
	}
	if (class_entry->subsystem_count == 0U || class_entry->subsystem_ids == nullptr ||
		!std::binary_search(class_entry->subsystem_ids,
			class_entry->subsystem_ids + class_entry->subsystem_count,
			subsystem_id)) {
		return ValidationError::MissingManifest;
	}
	return ValidationError::None;
}

ValidationError validate_revealed_identity(const BusinessStateValidationContext& context,
	const LifecycleFacts& lifecycle,
	ObjectType object_type,
	std::uint32_t class_id,
	bool class_is_required) noexcept
{
	if (object_type != lifecycle.object_type) {
		return ValidationError::InvalidStateTransition;
	}
	if (class_id == 0U) {
		return class_is_required ? ValidationError::InvalidAbsence : ValidationError::None;
	}
	if (!lifecycle.has_class || lifecycle.class_id != class_id) {
		return ValidationError::InvalidStateTransition;
	}
	if (object_type == ObjectType::Ship) {
		return context.class_manifest_installed && find_class(context, class_id) != nullptr ?
			ValidationError::None :
			ValidationError::MissingManifest;
	}
	if (object_type == ObjectType::Weapon) {
		return context.weapon_manifest_installed && has_weapon_class(context, class_id) ?
			ValidationError::None :
			ValidationError::MissingManifest;
	}
	return ValidationError::InvalidStateTransition;
}

ValidationError validate_sensor_revealed_identity(
	const BusinessStateValidationContext& context,
	ObjectType object_type,
	std::uint32_t class_id,
	bool class_is_required) noexcept
{
	if (class_id == 0U) {
		return class_is_required
			? ValidationError::InvalidAbsence
			: ValidationError::None;
	}
	if (object_type == ObjectType::Ship) {
		return context.class_manifest_installed &&
				find_class(context, class_id) != nullptr
			? ValidationError::None
			: ValidationError::MissingManifest;
	}
	if (object_type == ObjectType::Weapon) {
		return context.weapon_manifest_installed &&
				has_weapon_class(context, class_id)
			? ValidationError::None
			: ValidationError::MissingManifest;
	}
	return ValidationError::InvalidStateTransition;
}

ValidationError validate_lock_references(const StateAtom& atom,
	const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode,
	bool sensor_identity_allowed) noexcept
{
	PacketReader reader(ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()});
	std::uint64_t ignored_u64 = 0;
	std::uint16_t count = 0;
	if (!reader.read_u64(ignored_u64) || !reader.read_u64(ignored_u64) || !reader.read_u64(ignored_u64) ||
		!reader.read_u16(count)) {
		return ValidationError::BadRecordLength;
	}
	for (std::size_t index = 0; index < count; ++index) {
		PacketReader item;
		std::uint16_t presence = 0;
		bool ignored_bool = false;
		std::uint64_t target = 0;
		if (!read_vlist_item(reader, item) || !item.read_u16(presence) || !item.read_bool8(ignored_bool) ||
			!item.read_bool8(ignored_bool) || !item.read_u64(target)) {
			return ValidationError::BadRecordLength;
		}
		if (const auto error = validate_entity_reference(
				atoms, context, visibility_mode, target,
				sensor_identity_allowed);
			error != ValidationError::None) {
			return error;
		}
		if ((presence & LockItemPresenceFlagSubsystem) != 0U) {
			std::uint32_t subsystem_id = 0;
			if (!item.read_u32(subsystem_id)) {
				return ValidationError::BadRecordLength;
			}
			if (const auto error =
					validate_subsystem_reference(atoms, context,
						visibility_mode, target, subsystem_id,
						sensor_identity_allowed);
				error != ValidationError::None) {
				return error;
			}
		}
		const auto tail_size = 12U + ((presence & LockItemPresenceFlagLockAttempt) != 0U ? 8U : 0U);
		if (!item.skip(tail_size) || !item.at_end()) {
			return ValidationError::BadRecordLength;
		}
	}
	return reader.at_end() ? ValidationError::None : ValidationError::BadRecordLength;
}

ValidationError validate_target_references(const StateAtom& atom,
	const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode,
	bool sensor_identity_allowed) noexcept
{
	TargetFacts facts;
	if (const auto error = parse_target(atom, facts); error != ValidationError::None) {
		return error;
	}
	const auto validate_if_present = [&](std::uint64_t flag, std::uint64_t entity_id) {
		return (facts.presence & flag) == 0U ? ValidationError::None :
			validate_entity_reference(atoms, context, visibility_mode,
				entity_id, sensor_identity_allowed);
	};
	if (facts.current_target != 0U) {
		if (const auto error = validate_entity_reference(atoms, context,
				visibility_mode, facts.current_target,
				sensor_identity_allowed);
			error != ValidationError::None) {
			return error;
		}
	}
	if (const auto error =
			validate_if_present(TargetStatePresenceFlagPreviousTarget, facts.previous_target);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_if_present(TargetStatePresenceFlagAttacker, facts.attacker_entity_id);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error =
			validate_if_present(TargetStatePresenceFlagDangerousWeapon, facts.dangerous_weapon_entity_id);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error =
			validate_if_present(TargetStatePresenceFlagNearestLocked, facts.nearest_locked_entity_id);
		error != ValidationError::None) {
		return error;
	}
	if ((facts.presence & TargetStatePresenceFlagDangerousWeapon) != 0U) {
		LifecycleFacts dangerous;
		if (const auto* lifecycle = find_owner(atoms,
				RecordType::EntityLifecycle,
				facts.dangerous_weapon_entity_id);
			lifecycle != nullptr) {
			if (const auto error = parse_lifecycle(*lifecycle, dangerous);
				error != ValidationError::None) {
				return error;
			}
			if (dangerous.object_type != ObjectType::Weapon) {
				return ValidationError::InvalidStateTransition;
			}
		} else if (!sensor_identity_allowed) {
			return ValidationError::UnknownEntity;
		}
	}
	if ((facts.presence & TargetStatePresenceFlagTargetSubsystem) != 0U) {
		if (const auto error = validate_subsystem_reference(
				atoms, context, visibility_mode, facts.current_target,
				facts.target_subsystem_id, sensor_identity_allowed);
			error != ValidationError::None) {
			return error;
		}
	}
	if ((facts.presence & TargetStatePresenceFlagLockSubsystem) != 0U) {
		if (const auto error = validate_subsystem_reference(
				atoms, context, visibility_mode, facts.current_target,
				facts.lock_subsystem_id, sensor_identity_allowed);
			error != ValidationError::None) {
			return error;
		}
	}
	if ((facts.presence & TargetStatePresenceFlagRevealedIdentity) != 0U) {
		const auto* lifecycle = find_owner(atoms, RecordType::EntityLifecycle,
			facts.current_target);
		ValidationError error = ValidationError::None;
		if (lifecycle != nullptr) {
			LifecycleFacts target;
			error = parse_lifecycle(*lifecycle, target);
			if (error == ValidationError::None) {
				error = validate_revealed_identity(context, target,
					facts.revealed_object_type,
					facts.revealed_class_id, false);
			}
		} else {
			error = sensor_identity_allowed
				? validate_sensor_revealed_identity(context,
					facts.revealed_object_type,
					facts.revealed_class_id, false)
				: ValidationError::UnknownEntity;
		}
		if (error != ValidationError::None) {
			return error;
		}
	}
	return ValidationError::None;
}

ValidationError validate_radar_contact_references(const StateAtom& atom,
	const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode,
	bool sensor_identity_allowed) noexcept
{
	RadarContactFacts facts;
	if (const auto error = parse_radar_contact(atom, facts); error != ValidationError::None) {
		return error;
	}
	if (const auto error =
			validate_entity_reference(atoms, context, visibility_mode,
				facts.contact_entity_id, sensor_identity_allowed);
		error != ValidationError::None) {
		return error;
	}
	const auto* lifecycle = find_owner(atoms, RecordType::EntityLifecycle,
		facts.contact_entity_id);
	LifecycleFacts contact;
	if (lifecycle != nullptr) {
		if (const auto error = parse_lifecycle(*lifecycle, contact);
			error != ValidationError::None) {
			return error;
		}
		if (facts.object_type != contact.object_type) {
			return ValidationError::InvalidStateTransition;
		}
	}
	if ((facts.presence & RadarContactsPresenceFlagRevealedClass) != 0U) {
		const auto error = lifecycle != nullptr
			? validate_revealed_identity(context, contact,
				facts.object_type, facts.revealed_class_id, true)
			: validate_sensor_revealed_identity(context,
				facts.object_type, facts.revealed_class_id, true);
		if (error != ValidationError::None) {
			return error;
		}
	}
	const auto radar_bomb = (facts.flags & ContactFlagBomb) != 0U;
	if (facts.object_type == ObjectType::Weapon) {
		const auto class_id = lifecycle != nullptr
			? contact.class_id
			: facts.revealed_class_id;
		const auto* class_flags = class_id != 0U
			? find_weapon_class_flags(context, class_id)
			: nullptr;
		if (lifecycle == nullptr && !radar_bomb && class_id == 0U) {
			// A sensor-only weapon contact may intentionally omit its class.
		} else if (!context.weapon_manifest_installed || class_flags == nullptr) {
			return ValidationError::MissingManifest;
		} else if (radar_bomb !=
			((*class_flags & WeaponClassFlagBomb) != 0U)) {
			return ValidationError::InvalidStateTransition;
		}
	} else if (radar_bomb) {
		return ValidationError::InvalidStateTransition;
	}
	const auto* target_atom = find_owner(atoms, RecordType::TargetState, facts.observer_entity_id);
	if (target_atom != nullptr) {
		TargetFacts target;
		if (const auto error = parse_target(*target_atom, target); error != ValidationError::None) {
			return error;
		}
		const auto is_current_target = target.current_target == facts.contact_entity_id;
		if (((facts.flags & ContactFlagCurrentTarget) != 0U) != is_current_target) {
			return ValidationError::InvalidStateTransition;
		}
	}
	return ValidationError::None;
}

ValidationError validate_threat_references(const StateAtom& atom,
	const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode,
	bool sensor_identity_allowed) noexcept
{
	PacketReader reader(ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()});
	std::uint64_t owner = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample_time = 0;
	std::uint8_t threat_level = 0;
	if (!reader.read_u64(owner) || !reader.read_u64(presence) || !reader.read_u64(sample_time) ||
		!reader.read_u8(threat_level)) {
		return ValidationError::BadRecordLength;
	}
	std::uint64_t attacker = 0;
	std::uint64_t dangerous = 0;
	std::uint64_t nearest_homing = 0;
	if ((presence & ThreatStatePresenceFlagNearestAttacker) != 0U && !reader.read_u64(attacker)) {
		return ValidationError::BadRecordLength;
	}
	if ((presence & ThreatStatePresenceFlagDangerousWeapon) != 0U && !reader.read_u64(dangerous)) {
		return ValidationError::BadRecordLength;
	}
	if ((presence & ThreatStatePresenceFlagNearestHoming) != 0U && !reader.read_u64(nearest_homing)) {
		return ValidationError::BadRecordLength;
	}
	const auto validate_if_present = [&](std::uint64_t flag, std::uint64_t entity_id) {
		return (presence & flag) == 0U ? ValidationError::None :
			validate_entity_reference(atoms, context, visibility_mode,
				entity_id, sensor_identity_allowed);
	};
	if (const auto error = validate_if_present(ThreatStatePresenceFlagNearestAttacker, attacker);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_if_present(ThreatStatePresenceFlagDangerousWeapon, dangerous);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_if_present(ThreatStatePresenceFlagNearestHoming, nearest_homing);
		error != ValidationError::None) {
		return error;
	}

	std::uint16_t count = 0;
	if (!reader.read_u16(count)) {
		return ValidationError::BadRecordLength;
	}
	std::array<std::uint64_t, 256> missile_ids{};
	std::array<bool, 256> missile_is_homing{};
	for (std::size_t index = 0; index < count; ++index) {
		PacketReader item;
		std::uint16_t item_presence = 0;
		std::uint8_t guidance = 0;
		std::uint8_t ignored_visibility = 0;
		std::uint32_t weapon_class_id = 0;
		std::uint64_t target = 0;
		if (!read_vlist_item(reader, item) || !item.read_u16(item_presence) || !item.read_u8(guidance) ||
			!item.read_u8(ignored_visibility) || !item.read_u64(missile_ids[index]) ||
			!item.read_u32(weapon_class_id) || !item.read_u64(target)) {
			return ValidationError::BadRecordLength;
		}
		if (target != owner) {
			return ValidationError::InvalidStateTransition;
		}
		if (const auto error =
				validate_entity_reference(atoms, context, visibility_mode,
					missile_ids[index], sensor_identity_allowed);
			error != ValidationError::None) {
			return error;
		}
		const auto* lifecycle = find_owner(atoms,
			RecordType::EntityLifecycle, missile_ids[index]);
		LifecycleFacts missile;
		if (lifecycle != nullptr) {
			if (const auto error = parse_lifecycle(*lifecycle, missile);
				error != ValidationError::None) {
				return error;
			}
			if (missile.object_type != ObjectType::Weapon ||
				!missile.has_class ||
				missile.class_id != weapon_class_id) {
				return ValidationError::InvalidStateTransition;
			}
		}
		if (!context.weapon_manifest_installed || !has_weapon_class(context, weapon_class_id) ||
			find_weapon_class_flags(context, weapon_class_id) == nullptr) {
			return ValidationError::MissingManifest;
		}
		missile_is_homing[index] = guidance != static_cast<std::uint8_t>(GuidanceType::None);
		if ((item_presence & IncomingMissilePresenceFlagHomingSubsystem) != 0U) {
			std::uint32_t subsystem_id = 0;
			if (!item.read_u32(subsystem_id)) {
				return ValidationError::BadRecordLength;
			}
			if (const auto error =
					validate_subsystem_reference(atoms, context, visibility_mode, owner, subsystem_id);
				error != ValidationError::None) {
				return error;
			}
		}
		if (!item.skip(40U) || !item.at_end()) {
			return ValidationError::BadRecordLength;
		}
	}
	if (!reader.at_end()) {
		return ValidationError::BadRecordLength;
	}
	const auto missile_index = [&](std::uint64_t entity_id) {
		for (std::size_t index = 0; index < count; ++index) {
			if (missile_ids[index] == entity_id) {
				return index;
			}
		}
		return static_cast<std::size_t>(count);
	};
	if ((presence & ThreatStatePresenceFlagDangerousWeapon) != 0U && missile_index(dangerous) == count) {
		return ValidationError::InvalidStateTransition;
	}
	if ((presence & ThreatStatePresenceFlagNearestHoming) != 0U) {
		const auto index = missile_index(nearest_homing);
		if (index == count || !missile_is_homing[index]) {
			return ValidationError::InvalidStateTransition;
		}
	}
	return ValidationError::None;
}

ValidationError validate_cargo_references(const StateAtom& atom,
	const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode,
	bool sensor_identity_allowed) noexcept
{
	PacketReader reader(ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()});
	std::uint64_t ignored_u64 = 0;
	std::uint64_t presence = 0;
	std::uint8_t ignored_u8 = 0;
	if (!reader.read_u64(ignored_u64) || !reader.read_u64(presence) || !reader.read_u64(ignored_u64) ||
		!reader.read_u8(ignored_u8) || !reader.read_u8(ignored_u8)) {
		return ValidationError::BadRecordLength;
	}
	if ((presence & CargoScanStatePresenceFlagTarget) == 0U) {
		return ValidationError::None;
	}
	std::uint64_t target = 0;
	if (!reader.read_u64(target)) {
		return ValidationError::BadRecordLength;
	}
	if (const auto error = validate_entity_reference(atoms, context,
			visibility_mode, target, sensor_identity_allowed);
		error != ValidationError::None) {
		return error;
	}
	if ((presence & CargoScanStatePresenceFlagSubsystem) != 0U) {
		std::uint32_t subsystem_id = 0;
		if (!reader.read_u32(subsystem_id)) {
			return ValidationError::BadRecordLength;
		}
		return validate_subsystem_reference(atoms, context,
			visibility_mode, target, subsystem_id,
			sensor_identity_allowed);
	}
	return ValidationError::None;
}

ValidationError validate_docking_references(const StateAtom& atom,
	const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode) noexcept
{
	DockingFacts facts;
	if (const auto error = parse_docking(atom, facts); error != ValidationError::None) {
		return error;
	}
	if (facts.group_leader_entity_id != 0U) {
		if (const auto error = validate_entity_reference(
				atoms, context, visibility_mode, facts.group_leader_entity_id);
			error != ValidationError::None) {
			return error;
		}
	}
	for (std::size_t index = 0; index < facts.relation_count; ++index) {
		const auto& relation = facts.relations[index];
		if (relation.remote_entity_id == facts.owner_entity_id) {
			return ValidationError::InvalidStateTransition;
		}
		if (const auto error = validate_entity_reference(
				atoms, context, visibility_mode, relation.remote_entity_id);
			error != ValidationError::None) {
			return error;
		}
		const auto* remote_atom = find_owner(atoms, RecordType::DockingState, relation.remote_entity_id);
		if (remote_atom == nullptr) {
			continue;
		}
		DockingFacts remote;
		if (const auto error = parse_docking(*remote_atom, remote); error != ValidationError::None) {
			return error;
		}
		bool inverse_found = false;
		for (std::size_t remote_index = 0; remote_index < remote.relation_count; ++remote_index) {
			const auto& inverse = remote.relations[remote_index];
			if (inverse.remote_entity_id == facts.owner_entity_id &&
				inverse.local_dockpoint_index == relation.remote_dockpoint_index &&
				inverse.remote_dockpoint_index == relation.local_dockpoint_index &&
				inverse.local_dockpoint_name == relation.remote_dockpoint_name &&
				inverse.remote_dockpoint_name == relation.local_dockpoint_name) {
				inverse_found = true;
				break;
			}
		}
		if (!inverse_found) {
			return ValidationError::InvalidStateTransition;
		}
	}
	return ValidationError::None;
}

ValidationError validate_support_reference(const StateAtom& atom,
	const std::vector<StateAtom>& atoms,
	const BusinessStateValidationContext& context,
	VisibilityMode visibility_mode) noexcept
{
	PacketReader reader(ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()});
	std::uint64_t ignored_u64 = 0;
	std::uint64_t presence = 0;
	if (!reader.read_u64(ignored_u64) || !reader.read_u64(presence) || !reader.read_u64(ignored_u64) ||
		!reader.skip(5U)) {
		return ValidationError::BadRecordLength;
	}
	if ((presence & SupportStatePresenceFlagSupportEntity) == 0U) {
		return ValidationError::None;
	}
	std::uint64_t support_entity_id = 0;
	if (!reader.read_u64(support_entity_id)) {
		return ValidationError::BadRecordLength;
	}
	if (const auto error =
			validate_entity_reference(atoms, context, visibility_mode, support_entity_id);
		error != ValidationError::None) {
		return error;
	}
	LifecycleFacts support;
	if (const auto error = lifecycle_for_entity(atoms, support_entity_id, support);
		error != ValidationError::None) {
		return error;
	}
	return support.object_type == ObjectType::Ship ? ValidationError::None :
		ValidationError::InvalidStateTransition;
}

enum class ParentChainVisit : std::uint8_t {
	Unvisited = 0,
	Visiting = 1,
	Valid = 2,
};

ValidationError validate_parent_chain(const std::vector<StateAtom>& atoms,
	std::size_t lifecycle_begin_index,
	std::vector<ParentChainVisit>& visits,
	std::uint64_t entity_id,
	std::uint64_t parent_entity_id) noexcept
{
	// Mark each lifecycle at most once across all chains. The first walk detects
	// a back-edge; the second commits the acyclic path as globally valid. This
	// keeps the complete graph validation O(N log N), including key lookups.
	auto current = parent_entity_id;
	while (current != 0U) {
		if (current == entity_id) {
			return ValidationError::InvalidStateTransition;
		}
		const auto* parent_atom = find_owner(atoms, RecordType::EntityLifecycle, current);
		if (parent_atom == nullptr) {
			return ValidationError::UnknownEntity;
		}
		const auto atom_index = static_cast<std::size_t>(parent_atom - atoms.data());
		if (atom_index < lifecycle_begin_index || atom_index - lifecycle_begin_index >= visits.size()) {
			return ValidationError::UnknownEntity;
		}
		auto& visit = visits[atom_index - lifecycle_begin_index];
		if (visit == ParentChainVisit::Valid) {
			break;
		}
		if (visit == ParentChainVisit::Visiting) {
			return ValidationError::InvalidStateTransition;
		}
		visit = ParentChainVisit::Visiting;
		LifecycleFacts parent;
		if (const auto error = parse_lifecycle(*parent_atom, parent); error != ValidationError::None) {
			return error;
		}
		current = parent.parent_entity_id;
	}

	current = parent_entity_id;
	while (current != 0U) {
		const auto* parent_atom = find_owner(atoms, RecordType::EntityLifecycle, current);
		if (parent_atom == nullptr) {
			return ValidationError::UnknownEntity;
		}
		const auto atom_index = static_cast<std::size_t>(parent_atom - atoms.data());
		auto& visit = visits[atom_index - lifecycle_begin_index];
		if (visit == ParentChainVisit::Valid) {
			break;
		}
		LifecycleFacts parent;
		if (const auto error = parse_lifecycle(*parent_atom, parent); error != ValidationError::None) {
			return error;
		}
		visit = ParentChainVisit::Valid;
		current = parent.parent_entity_id;
	}
	return ValidationError::None;
}

bool requires_ship_scope(RecordType type) noexcept
{
	return type == RecordType::ShipIdentity || type == RecordType::ControlState ||
		   type == RecordType::SubsystemState || type == RecordType::EnergyState ||
		   type == RecordType::PropulsionState || type == RecordType::WeaponState ||
		   type == RecordType::LockState || type == RecordType::TargetState || type == RecordType::RadarState ||
		   type == RecordType::ThreatState || type == RecordType::CargoScanState ||
		   type == RecordType::NavigationState;
}

bool observed_player_scope(RecordType type) noexcept
{
	return type == RecordType::ControlState || type == RecordType::LockState || type == RecordType::TargetState ||
		   type == RecordType::RadarState || type == RecordType::RadarContacts ||
		   type == RecordType::ThreatState || type == RecordType::CargoScanState ||
		   type == RecordType::NavigationState;
}

bool domain_allows_record(RecordType type, std::uint64_t coverage) noexcept
{
	if (type == RecordType::ControlState) {
		return (coverage & StateDomainCoverageBitControlInputs) != 0U;
	}
	if (type == RecordType::LockState || type == RecordType::TargetState) {
		return (coverage & StateDomainCoverageBitTargeting) != 0U;
	}
	if (type == RecordType::RadarState || type == RecordType::RadarContacts || type == RecordType::ThreatState) {
		return (coverage & StateDomainCoverageBitRadarSensors) != 0U;
	}
	if (type == RecordType::WeaponState) {
		return (coverage & StateDomainCoverageBitWeapons) != 0U;
	}
	if (type == RecordType::CargoScanState || type == RecordType::DockingState ||
		type == RecordType::SupportState) {
		return (coverage & StateDomainCoverageBitCargoDockSupport) != 0U;
	}
	if (type == RecordType::NavigationState) {
		return (coverage & StateDomainCoverageBitNavigation) != 0U;
	}
	if (type == RecordType::EffectState) {
		return (coverage & StateDomainCoverageBitLowFrequencyEffects) != 0U;
	}
	return true;
}

} // namespace

BusinessStateImageValidator::BusinessStateImageValidator(const BusinessStateValidationContext& context) noexcept
	: m_context(context)
{
}

ValidationError BusinessStateImageValidator::validate(const StateImage& image) const noexcept
{
	if (const auto error = validate_context(m_context); error != ValidationError::None) {
		return error;
	}
	const auto& atoms = image.records();
	for (const auto& atom : atoms) {
		BusinessRecordMetadata metadata;
		RecordEnvelopeView envelope;
		envelope.raw_record_type = atom.key.record_type;
		envelope.record_version = atom.record_version;
		envelope.record_flags = RecordFlagNone;
		envelope.payload = ByteView{atom.value.empty() ? nullptr : atom.value.data(), atom.value.size()};
		if (const auto error = validate_business_record(
				envelope, BusinessRecordContainer::FullSnapshot, m_context.protocol_minor, metadata);
			error != ValidationError::None) {
			return error;
		}
		if (!metadata.state_atom || atom.key.identity.size() != metadata.key_size ||
			atom.value.size() < metadata.key_size ||
			!std::equal(atom.key.identity.begin(), atom.key.identity.end(), atom.value.begin())) {
			return ValidationError::BadRecordLength;
		}
	}

	const auto* session_atom = find_singleton(atoms, RecordType::SessionState);
	const auto* mission_atom = find_singleton(atoms, RecordType::MissionState);
	if (session_atom == nullptr || mission_atom == nullptr) {
		return ValidationError::InvalidAbsence;
	}
	SessionFacts session;
	if (const auto error = parse_session(*session_atom, session); error != ValidationError::None) {
		return error;
	}
	const bool phase1_player_kinematics =
		is_phase1_player_kinematics_profile(m_context.protocol_minor, session.coverage);
	const bool phase2_complete_ship =
		is_phase2_complete_ship_profile(m_context.protocol_minor, session.coverage);
	const bool phase3_cockpit_sensors =
		is_phase3_cockpit_sensors_profile(m_context.protocol_minor, session.coverage);
	if (phase1_player_kinematics) {
		if (m_context.required_manifest_id != 0U) {
			return ValidationError::InvalidStateTransition;
		}
		if (session.capabilities != 0U) {
			return ValidationError::CapabilityNotNegotiated;
		}
		if (session.derived_events != 0U || session.exact_events != 0U) {
			return ValidationError::InvalidStateTransition;
		}
		const auto expected_record_count = session.observed_entity_id == 0U ? 2U : 4U;
		if (atoms.size() != expected_record_count) {
			return ValidationError::InvalidAbsence;
		}
		if (session.observed_entity_id != 0U) {
			const auto* flight = find_owner(atoms, RecordType::FlightState, session.observed_entity_id);
			if (flight == nullptr || flight->value.size() < 16U || read_u64(flight->value.data() + 8U) != 0U) {
				return ValidationError::InvalidAbsence;
			}
		}
	} else if (m_context.protocol_minor == VersionMinorV1_1 &&
		(session.coverage & StateDomainCoverageBitCoreShip) != 0U && m_context.required_manifest_id == 0U) {
		return ValidationError::MissingManifest;
	}
	constexpr std::uint64_t SpecializedCommVideoCapabilities =
		static_cast<std::uint64_t>(CapabilityCommViewLocalAssets) |
		static_cast<std::uint64_t>(CapabilityCommViewAuthoritativeSource) |
		static_cast<std::uint64_t>(CapabilityTargetVideoH264) |
		static_cast<std::uint64_t>(CapabilityTargetVideoRemoteRender);
	if (phase2_complete_ship &&
		(session.capabilities & SpecializedCommVideoCapabilities) != 0U) {
		// 0x0583 never negotiates COMM-view or target-video specialization.
		// Reject isolated bits with the same oracle as a complete pair.
		return ValidationError::CapabilityNotNegotiated;
	}
	if (validate_emittable_active_capabilities(session.capabilities) != ValidationError::None) {
		return ValidationError::CapabilityNotNegotiated;
	}
	if (m_context.enforce_negotiated_capabilities && session.capabilities != m_context.negotiated_capabilities) {
		return ValidationError::CapabilityNotNegotiated;
	}
	if (phase1_player_kinematics && session.visibility_mode != VisibilityMode::Cockpit) {
		return ValidationError::VisibilityViolation;
	}
	if (session.visibility_mode == VisibilityMode::TrustedFullState &&
		(!m_context.trusted_full_state_authorized || !m_context.source_endpoint_allowlisted ||
			session.authority_mode == AuthorityMode::MultiplayerClient)) {
		return ValidationError::VisibilityViolation;
	}
	if (phase1_player_kinematics && session.authority_mode != AuthorityMode::Solo) {
		return ValidationError::InvalidStateTransition;
	}
	const auto exact_without_derived = session.exact_events & ~session.derived_events;
	if ((exact_without_derived & ~m_context.exact_event_hook_families) != 0U ||
		((session.exact_events & EventFamilyBitCommunication) != 0U &&
			!m_context.communication_exact_hook_available)) {
		return ValidationError::InvalidStateTransition;
	}
	if ((session.coverage & StateDomainCoverageBitCoreShip) != 0U && !m_context.class_manifest_installed) {
		return ValidationError::MissingManifest;
	}
	if ((session.coverage & StateDomainCoverageBitWeapons) != 0U && !m_context.weapon_manifest_installed) {
		return ValidationError::MissingManifest;
	}

	const auto& previous = m_context.previous_session;
	if (previous.enforce) {
		if (session.producer_id != previous.producer_id || session.authority_mode != previous.authority_mode ||
			session.visibility_mode != previous.visibility_mode ||
			session.coverage != previous.state_domain_coverage ||
			session.derived_events != previous.event_coverage_state_derived ||
			session.exact_events != previous.event_coverage_exact ||
			session.producer_sample_time_us < previous.minimum_producer_sample_time_us) {
			return ValidationError::InvalidStateTransition;
		}
		if (session.capabilities == previous.negotiated_capabilities) {
			if (session.capability_generation != previous.capability_generation) {
				return ValidationError::StaleGeneration;
			}
		} else if ((session.capabilities & ~previous.negotiated_capabilities) != 0U ||
			(previous.negotiated_capabilities & CapabilityUpdate) == 0U ||
			session.capability_generation <= previous.capability_generation) {
			return ValidationError::StaleGeneration;
		}
	}

	LifecycleFacts observed_lifecycle;
	if (session.observed_entity_id != 0) {
		const auto* observed = find_owner(atoms, RecordType::EntityLifecycle, session.observed_entity_id);
		if (observed == nullptr) {
			return phase2_complete_ship
				? ValidationError::InvalidAbsence
				: ValidationError::UnknownEntity;
		}
		if (const auto error = parse_lifecycle(*observed, observed_lifecycle); error != ValidationError::None) {
			return error;
		}
		if (observed_lifecycle.object_type != ObjectType::Ship) {
			return ValidationError::InvalidStateTransition;
		}
		if (phase1_player_kinematics && observed_lifecycle.presence != 0U) {
			return ValidationError::InvalidAbsence;
		}
	}

	for (const auto& atom : atoms) {
		const auto type = static_cast<RecordType>(atom.key.record_type);
		if (type == RecordType::SessionState || type == RecordType::MissionState ||
			type == RecordType::CommViewState) {
			continue;
		}
		if (atom.key.identity.size() < 8U) {
			return ValidationError::BadRecordLength;
		}
		const auto owner_id = read_u64(atom.key.identity.data());
		const auto* lifecycle_atom = find_owner(atoms, RecordType::EntityLifecycle, owner_id);
		if (lifecycle_atom == nullptr) {
			return phase2_complete_ship
				? ValidationError::InvalidAbsence
				: ValidationError::UnknownEntity;
		}
		LifecycleFacts lifecycle;
		if (const auto error = parse_lifecycle(*lifecycle_atom, lifecycle); error != ValidationError::None) {
			return error;
		}
		if (session.visibility_mode == VisibilityMode::Cockpit && m_context.enforce_cockpit_entity_allowlist &&
			!cockpit_entity_allowed(m_context, owner_id)) {
			return ValidationError::VisibilityViolation;
		}
		if (!domain_allows_record(type, session.coverage)) {
			return ValidationError::InvalidAbsence;
		}
		if (requires_ship_scope(type) && lifecycle.object_type != ObjectType::Ship) {
			return ValidationError::InvalidStateTransition;
		}
		if (type == RecordType::SubsystemState) {
			const auto subsystem_id = read_u32(atom.key.identity.data() + 8U);
			const auto* class_entry = lifecycle.has_class ? find_class(m_context, lifecycle.class_id) : nullptr;
			if (!m_context.class_manifest_installed || class_entry == nullptr ||
				class_entry->subsystem_count == 0U || class_entry->subsystem_ids == nullptr ||
				!std::binary_search(class_entry->subsystem_ids,
					class_entry->subsystem_ids + class_entry->subsystem_count,
					subsystem_id)) {
				return ValidationError::MissingManifest;
			}
		}
		if (observed_player_scope(type) && owner_id != session.observed_entity_id) {
			return ValidationError::VisibilityViolation;
		}
		if (type == RecordType::FlightState) {
			const auto presence = read_u64(atom.value.data() + 8U);
			if ((presence & 0x07ffULL) != 0U && (session.coverage & StateDomainCoverageBitPrediction) == 0U) {
				return ValidationError::InvalidAbsence;
			}
			if ((presence & FlightStatePresenceFlagCosmeticThrust) != 0U &&
				(session.coverage & StateDomainCoverageBitLowFrequencyEffects) == 0U) {
				return ValidationError::InvalidAbsence;
			}
		}
	}

	const auto lifecycle_type = static_cast<std::uint16_t>(RecordType::EntityLifecycle);
	const auto lifecycle_begin =
		std::lower_bound(atoms.begin(), atoms.end(), lifecycle_type, [](const StateAtom& atom, std::uint16_t value) {
			return atom.key.record_type < value;
		});
	const auto lifecycle_end =
		std::upper_bound(lifecycle_begin, atoms.end(), lifecycle_type, [](std::uint16_t value, const StateAtom& atom) {
			return value < atom.key.record_type;
		});
	const auto lifecycle_begin_index =
		static_cast<std::size_t>(std::distance(atoms.begin(), lifecycle_begin));
	const auto lifecycle_count = static_cast<std::size_t>(std::distance(lifecycle_begin, lifecycle_end));
	std::vector<ParentChainVisit> parent_chain_visits;
	std::size_t phase2_ship_count = 0U;
	for (auto iterator = lifecycle_begin; iterator != lifecycle_end; ++iterator) {
		const auto& atom = *iterator;
		LifecycleFacts lifecycle;
		if (const auto error = parse_lifecycle(atom, lifecycle); error != ValidationError::None) {
			return error;
		}
		if (phase2_complete_ship && lifecycle.object_type == ObjectType::Ship &&
			++phase2_ship_count > MaximumPhase2CompleteShipCount) {
			return ValidationError::ResourceLimit;
		}
		if (session.visibility_mode == VisibilityMode::Cockpit && m_context.enforce_cockpit_entity_allowlist &&
			!cockpit_entity_allowed(m_context, lifecycle.entity_id)) {
			return ValidationError::VisibilityViolation;
		}
		const BusinessClassCatalogEntry* class_entry = nullptr;
		if (lifecycle.object_type == ObjectType::Ship && !phase1_player_kinematics) {
			if (!m_context.class_manifest_installed || !lifecycle.has_class ||
				(class_entry = find_class(m_context, lifecycle.class_id)) == nullptr) {
				return ValidationError::MissingManifest;
			}
			const std::array<RecordType, 6> required{{RecordType::ShipIdentity,
				RecordType::FlightState,
				RecordType::DamageState,
				RecordType::ShieldState,
				RecordType::EnergyState,
				RecordType::PropulsionState}};
			for (const auto type : required) {
				if (find_owner(atoms, type, lifecycle.entity_id) == nullptr) {
					return ValidationError::InvalidAbsence;
				}
			}
			for (std::size_t index = 0; index < class_entry->subsystem_count; ++index) {
				if (find_subsystem(atoms, lifecycle.entity_id, class_entry->subsystem_ids[index]) == nullptr) {
					return ValidationError::InvalidAbsence;
				}
			}
			const auto* identity = find_owner(atoms, RecordType::ShipIdentity, lifecycle.entity_id);
			if (identity == nullptr || identity->value.size() < 28U || read_u32(identity->value.data() + 24U) != lifecycle.class_id) {
				return ValidationError::MissingManifest;
			}
			if ((session.coverage & StateDomainCoverageBitWeapons) != 0U &&
				find_owner(atoms, RecordType::WeaponState, lifecycle.entity_id) == nullptr) {
				return ValidationError::InvalidAbsence;
			}
			if (phase2_complete_ship &&
				(find_owner(atoms, RecordType::DockingState, lifecycle.entity_id) == nullptr ||
					find_owner(atoms, RecordType::SupportState, lifecycle.entity_id) == nullptr)) {
				return ValidationError::InvalidAbsence;
			}
		} else if (lifecycle.object_type == ObjectType::Ship) {
			if (lifecycle.entity_id != session.observed_entity_id || lifecycle.presence != 0U ||
				lifecycle.has_class || find_owner(atoms, RecordType::FlightState, lifecycle.entity_id) == nullptr) {
				return ValidationError::InvalidAbsence;
			}
		} else if (lifecycle.object_type == ObjectType::Weapon) {
			if (!m_context.weapon_manifest_installed || !lifecycle.has_class ||
				!has_weapon_class(m_context, lifecycle.class_id)) {
				return ValidationError::MissingManifest;
			}
			const auto* class_flags = find_weapon_class_flags(m_context, lifecycle.class_id);
			if (class_flags == nullptr) {
				return ValidationError::MissingManifest;
			}
			const auto lifecycle_bomb = (lifecycle.lifecycle_flags & EntityLifecycleFlagBomb) != 0U;
			const auto class_bomb = (*class_flags & WeaponClassFlagBomb) != 0U;
			if (lifecycle_bomb != class_bomb) {
				return ValidationError::InvalidStateTransition;
			}
		}
		if (lifecycle.parent_entity_id != 0) {
			if (lifecycle.parent_entity_id == lifecycle.entity_id) {
				return ValidationError::InvalidStateTransition;
			}
			if (const auto error = validate_entity_reference(
					atoms, m_context, session.visibility_mode, lifecycle.parent_entity_id);
				error != ValidationError::None) {
				return error;
			}
			if (parent_chain_visits.empty()) {
				try {
					parent_chain_visits.resize(lifecycle_count, ParentChainVisit::Unvisited);
				} catch (const std::bad_alloc&) {
					return ValidationError::ResourceLimit;
				}
			}
			if (const auto error = validate_parent_chain(atoms,
					lifecycle_begin_index,
					parent_chain_visits,
					lifecycle.entity_id,
					lifecycle.parent_entity_id);
				error != ValidationError::None) {
				return error;
			}
		}
	}

	for (const auto& atom : atoms) {
		ValidationError error = ValidationError::None;
		switch (static_cast<RecordType>(atom.key.record_type)) {
		case RecordType::LockState:
			error = validate_lock_references(atom, atoms, m_context,
				session.visibility_mode, phase3_cockpit_sensors);
			break;
		case RecordType::TargetState:
			error = validate_target_references(atom, atoms, m_context,
				session.visibility_mode, phase3_cockpit_sensors);
			break;
		case RecordType::RadarContacts:
			error = validate_radar_contact_references(atom, atoms, m_context,
				session.visibility_mode, phase3_cockpit_sensors);
			break;
		case RecordType::ThreatState:
			error = validate_threat_references(atom, atoms, m_context,
				session.visibility_mode, phase3_cockpit_sensors);
			break;
		case RecordType::CargoScanState:
			error = validate_cargo_references(atom, atoms, m_context,
				session.visibility_mode, phase3_cockpit_sensors);
			break;
		case RecordType::DockingState:
			error = validate_docking_references(atom, atoms, m_context, session.visibility_mode);
			break;
		case RecordType::SupportState:
			error = validate_support_reference(atom, atoms, m_context, session.visibility_mode);
			break;
		default:
			break;
		}
		if (error != ValidationError::None) {
			return error;
		}
	}

	const auto require_observed = [&](RecordType type, std::uint64_t domain) {
		return (session.coverage & domain) == 0U || session.observed_entity_id == 0 ||
			   find_owner(atoms, type, session.observed_entity_id) != nullptr;
	};
	if (!require_observed(RecordType::ControlState, StateDomainCoverageBitControlInputs) ||
		!require_observed(RecordType::RadarState, StateDomainCoverageBitRadarSensors) ||
		!require_observed(RecordType::ThreatState, StateDomainCoverageBitRadarSensors) ||
		!require_observed(RecordType::LockState, StateDomainCoverageBitTargeting) ||
		!require_observed(RecordType::TargetState, StateDomainCoverageBitTargeting) ||
		!require_observed(RecordType::CargoScanState, StateDomainCoverageBitCargoDockSupport) ||
		!require_observed(RecordType::NavigationState, StateDomainCoverageBitNavigation)) {
		return ValidationError::InvalidAbsence;
	}

	const auto comm_active = (session.capabilities & CommViewCapabilityPair) == CommViewCapabilityPair;
	if (comm_active != has_record_type(atoms, RecordType::CommViewState)) {
		return comm_active ? ValidationError::InvalidAbsence : ValidationError::CapabilityNotNegotiated;
	}
	return ValidationError::None;
}

ValidationError BusinessStateImageValidator::validate_delta_transition(const StateImage& baseline,
	const StateImage& candidate) const noexcept
{
	if (const auto error = validate(candidate); error != ValidationError::None) {
		return error;
	}
	const auto& baseline_atoms = baseline.records();
	const auto& candidate_atoms = candidate.records();
	const auto* baseline_session_atom = find_singleton(baseline_atoms, RecordType::SessionState);
	const auto* candidate_session_atom = find_singleton(candidate_atoms, RecordType::SessionState);
	const auto* baseline_mission_atom = find_singleton(baseline_atoms, RecordType::MissionState);
	const auto* candidate_mission_atom = find_singleton(candidate_atoms, RecordType::MissionState);
	if (baseline_session_atom == nullptr || candidate_session_atom == nullptr || baseline_mission_atom == nullptr ||
		candidate_mission_atom == nullptr) {
		return ValidationError::InvalidAbsence;
	}

	SessionFacts baseline_session;
	SessionFacts candidate_session;
	std::uint32_t baseline_mission_generation = 0;
	std::uint32_t candidate_mission_generation = 0;
	if (const auto error = parse_session(*baseline_session_atom, baseline_session); error != ValidationError::None) {
		return error;
	}
	if (const auto error = parse_session(*candidate_session_atom, candidate_session); error != ValidationError::None) {
		return error;
	}
	if (const auto error = parse_mission_generation(*baseline_mission_atom, baseline_mission_generation);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error = parse_mission_generation(*candidate_mission_atom, candidate_mission_generation);
		error != ValidationError::None) {
		return error;
	}
	if (baseline_mission_generation != candidate_mission_generation) {
		return ValidationError::InvalidStateTransition;
	}
	if (baseline_session.producer_id != candidate_session.producer_id ||
		baseline_session.authority_mode != candidate_session.authority_mode ||
		baseline_session.visibility_mode != candidate_session.visibility_mode ||
		baseline_session.coverage != candidate_session.coverage ||
		baseline_session.derived_events != candidate_session.derived_events ||
		baseline_session.exact_events != candidate_session.exact_events ||
		candidate_session.producer_sample_time_us < baseline_session.producer_sample_time_us) {
		return ValidationError::InvalidStateTransition;
	}
	if (baseline_session.capabilities == candidate_session.capabilities) {
		if (baseline_session.capability_generation != candidate_session.capability_generation) {
			return ValidationError::StaleGeneration;
		}
	} else if ((candidate_session.capabilities & ~baseline_session.capabilities) != 0U ||
		(baseline_session.capabilities & CapabilityUpdate) == 0U ||
		candidate_session.capability_generation <= baseline_session.capability_generation) {
		return ValidationError::StaleGeneration;
	}
	if ((baseline_session.coverage & StateDomainCoverageBitPlayerKinematics) != 0U &&
		baseline_session.observed_entity_id != candidate_session.observed_entity_id) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

} // namespace telemetry::protocol
