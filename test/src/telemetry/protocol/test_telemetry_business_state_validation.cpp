#include "telemetry/protocol/telemetry_business_state_validation.h"

#include "telemetry/protocol/packet_writer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
MutableByteView mutable_byte_view(Container& bytes)
{
	return MutableByteView{bytes.data(), bytes.size()};
}

std::vector<std::uint8_t> session_payload(VisibilityMode visibility = VisibilityMode::Cockpit,
	std::uint64_t producer_id = 1,
	std::uint64_t coverage = StateDomainCoverageBitCoreShip,
	std::uint64_t observed_entity_id = 0)
{
	std::vector<std::uint8_t> bytes(observed_entity_id == 0U ? 64U : 72U);
	PacketWriter writer(mutable_byte_view(bytes));
	EXPECT_TRUE(writer.write_u64(
		observed_entity_id == 0U ? SessionStatePresenceFlagNone : SessionStatePresenceFlagObservedPlayer));
	EXPECT_TRUE(writer.write_u64(producer_id));
	EXPECT_TRUE(writer.write_u64(100));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(AuthorityMode::Solo)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(visibility)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(SessionPhase::Live)));
	EXPECT_TRUE(writer.write_u8(0));
	EXPECT_TRUE(writer.write_u32(1));
	EXPECT_TRUE(writer.write_u64(0));
	EXPECT_TRUE(writer.write_u64(coverage));
	EXPECT_TRUE(writer.write_u64(0));
	EXPECT_TRUE(writer.write_u64(0));
	if (observed_entity_id != 0U) {
		EXPECT_TRUE(writer.write_u64(observed_entity_id));
	}
	EXPECT_EQ(bytes.size(), writer.size());
	return bytes;
}

void append_u8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
{
	bytes.push_back(value);
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (unsigned int shift = 0; shift < 32U; shift += 8U) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for (unsigned int shift = 0; shift < 64U; shift += 8U) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void append_f32(std::vector<std::uint8_t>& bytes, float value)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	append_u32(bytes, bits);
}

void append_string(std::vector<std::uint8_t>& bytes, std::string_view value)
{
	append_u16(bytes, static_cast<std::uint16_t>(value.size()));
	bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_vec3(std::vector<std::uint8_t>& bytes)
{
	append_f32(bytes, 0.0F);
	append_f32(bytes, 0.0F);
	append_f32(bytes, 0.0F);
}

void append_vlist_item(std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& item)
{
	append_u8(bytes, 1U);
	append_u16(bytes, static_cast<std::uint16_t>(item.size()));
	bytes.insert(bytes.end(), item.begin(), item.end());
}

std::vector<std::uint8_t> mission_payload()
{
	std::vector<std::uint8_t> bytes(28U);
	PacketWriter writer(mutable_byte_view(bytes));
	EXPECT_TRUE(writer.write_u64(0));
	EXPECT_TRUE(writer.write_u32(1));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(MissionPhase::Active)));
	EXPECT_TRUE(writer.write_u8(0));
	EXPECT_TRUE(writer.write_u16(0));
	EXPECT_TRUE(writer.write_f32(1.0F));
	EXPECT_TRUE(writer.write_u64(100));
	EXPECT_EQ(bytes.size(), writer.size());
	return bytes;
}

std::vector<std::uint8_t> waypoint_lifecycle_payload(std::uint64_t entity_id = 1)
{
	std::vector<std::uint8_t> bytes(30U);
	PacketWriter writer(mutable_byte_view(bytes));
	EXPECT_TRUE(writer.write_u64(entity_id));
	EXPECT_TRUE(writer.write_u64(0));
	EXPECT_TRUE(writer.write_u64(100));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(ObjectType::Waypoint)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(LifecyclePhase::Active)));
	EXPECT_TRUE(writer.write_u32(0));
	EXPECT_EQ(bytes.size(), writer.size());
	return bytes;
}

std::vector<std::uint8_t> lifecycle_payload(std::uint64_t entity_id,
	ObjectType object_type,
	std::uint64_t presence,
	std::uint32_t lifecycle_flags,
	std::uint32_t class_id = 0,
	std::uint64_t parent_entity_id = 0)
{
	const auto size = 30U + (class_id == 0 ? 0U : 4U) + (parent_entity_id == 0 ? 0U : 8U);
	std::vector<std::uint8_t> bytes(size);
	PacketWriter writer(mutable_byte_view(bytes));
	EXPECT_TRUE(writer.write_u64(entity_id));
	EXPECT_TRUE(writer.write_u64(presence));
	EXPECT_TRUE(writer.write_u64(100));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(object_type)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(LifecyclePhase::Active)));
	EXPECT_TRUE(writer.write_u32(lifecycle_flags));
	if (class_id != 0) {
		EXPECT_TRUE(writer.write_u32(class_id));
	}
	if (parent_entity_id != 0) {
		EXPECT_TRUE(writer.write_u64(parent_entity_id));
	}
	EXPECT_EQ(bytes.size(), writer.size());
	return bytes;
}

std::vector<std::uint8_t> ship_identity_payload(std::uint64_t entity_id, std::uint32_t class_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u32(bytes, class_id);
	append_string(bytes, "ship");
	append_u32(bytes, 0U);
	append_u32(bytes, 0U);
	append_u32(bytes, 0U);
	append_u16(bytes, 0U);
	append_f32(bytes, 0.0F);
	return bytes;
}

std::vector<std::uint8_t> flight_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_vec3(bytes);
	append_f32(bytes, 1.0F);
	append_f32(bytes, 0.0F);
	append_f32(bytes, 0.0F);
	append_f32(bytes, 0.0F);
	append_vec3(bytes);
	append_vec3(bytes);
	append_f32(bytes, 0.0F);
	append_u32(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> damage_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_f32(bytes, 50.0F);
	append_f32(bytes, 100.0F);
	append_u16(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> shield_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u8(bytes, 0U);
	append_u16(bytes, 0U);
	append_u16(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> energy_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(EtsMode::Absent));
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> propulsion_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u16(bytes, 0U);
	append_u16(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> subsystem_payload(std::uint64_t entity_id, std::uint32_t subsystem_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u32(bytes, subsystem_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u16(bytes, 0U);
	append_u8(bytes, static_cast<std::uint8_t>(SubsystemType::Engine));
	append_f32(bytes, 0.0F);
	append_f32(bytes, 0.0F);
	append_u32(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> lock_payload(std::uint64_t entity_id,
	std::uint64_t target_entity_id = 0,
	std::uint32_t subsystem_id = 0)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u16(bytes, target_entity_id == 0U ? 0U : 1U);
	if (target_entity_id != 0U) {
		std::vector<std::uint8_t> item;
		append_u16(item, subsystem_id == 0U ? LockItemPresenceFlagNone : LockItemPresenceFlagSubsystem);
		append_u8(item, 0U);
		append_u8(item, 1U);
		append_u64(item, target_entity_id);
		if (subsystem_id != 0U) {
			append_u32(item, subsystem_id);
		}
		append_vec3(item);
		append_vlist_item(bytes, item);
	}
	return bytes;
}

std::vector<std::uint8_t> target_payload(std::uint64_t entity_id,
	std::uint64_t current_target,
	ObjectType revealed_type = ObjectType::Unknown,
	std::uint32_t revealed_class_id = 0)
{
	const auto presence = revealed_type == ObjectType::Unknown ? TargetStatePresenceFlagNone :
		TargetStatePresenceFlagRevealedIdentity;
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, presence);
	append_u64(bytes, 100U);
	append_u64(bytes, current_target);
	if ((presence & TargetStatePresenceFlagRevealedIdentity) != 0U) {
		append_u8(bytes, static_cast<std::uint8_t>(revealed_type));
		append_string(bytes, "target");
		append_u32(bytes, revealed_class_id);
		append_u32(bytes, 0U);
		append_u32(bytes, 0U);
	}
	return bytes;
}

std::vector<std::uint8_t> target_subsystem_payload(std::uint64_t entity_id,
	std::uint64_t current_target,
	std::uint32_t subsystem_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, TargetStatePresenceFlagTargetSubsystem);
	append_u64(bytes, 100U);
	append_u64(bytes, current_target);
	append_u32(bytes, subsystem_id);
	return bytes;
}

std::vector<std::uint8_t> radar_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(RadarMode::Short));
	append_f32(bytes, 0.0F);
	append_u8(bytes, static_cast<std::uint8_t>(SensorState::Offline));
	append_f32(bytes, 0.0F);
	append_f32(bytes, 1.0F);
	return bytes;
}

std::vector<std::uint8_t> radar_contact_payload(std::uint64_t observer_entity_id,
	std::uint64_t contact_entity_id,
	ObjectType object_type,
	std::uint32_t flags,
	std::uint32_t revealed_class_id = 0)
{
	const auto presence = revealed_class_id == 0U ? RadarContactsPresenceFlagNone :
		RadarContactsPresenceFlagRevealedClass;
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, observer_entity_id);
	append_u64(bytes, contact_entity_id);
	append_u64(bytes, presence);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(object_type));
	append_u8(bytes, static_cast<std::uint8_t>(RadarCategory::Weapon));
	append_u8(bytes, static_cast<std::uint8_t>(RadarVisibility::Visible));
	append_vec3(bytes);
	append_vec3(bytes);
	append_f32(bytes, 1.0F);
	append_u32(bytes, flags);
	if (revealed_class_id != 0U) {
		append_u32(bytes, revealed_class_id);
	}
	return bytes;
}

std::vector<std::uint8_t> threat_payload(std::uint64_t entity_id,
	std::uint64_t missile_entity_id = 0,
	std::uint32_t weapon_class_id = 0,
	std::uint64_t dangerous_entity_id = 0)
{
	const auto presence = dangerous_entity_id == 0U ? ThreatStatePresenceFlagNone :
		ThreatStatePresenceFlagDangerousWeapon;
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, presence);
	append_u64(bytes, 100U);
	append_u8(bytes,
		static_cast<std::uint8_t>(missile_entity_id == 0U ? ThreatLevel::None : ThreatLevel::Dumbfire));
	if (dangerous_entity_id != 0U) {
		append_u64(bytes, dangerous_entity_id);
	}
	append_u16(bytes, missile_entity_id == 0U ? 0U : 1U);
	if (missile_entity_id != 0U) {
		std::vector<std::uint8_t> item;
		append_u16(item, IncomingMissilePresenceFlagNone);
		append_u8(item, static_cast<std::uint8_t>(GuidanceType::None));
		append_u8(item, static_cast<std::uint8_t>(RadarVisibility::Visible));
		append_u64(item, missile_entity_id);
		append_u32(item, weapon_class_id);
		append_u64(item, entity_id);
		append_vec3(item);
		append_f32(item, 1.0F);
		append_f32(item, 0.0F);
		append_f32(item, 0.0F);
		append_f32(item, 0.0F);
		append_vec3(item);
		append_vlist_item(bytes, item);
	}
	return bytes;
}

std::vector<std::uint8_t> cargo_payload(std::uint64_t entity_id,
	std::uint64_t target_entity_id,
	std::uint32_t subsystem_id = 0)
{
	const auto presence = CargoScanStatePresenceFlagTarget |
		(subsystem_id == 0U ? 0U : CargoScanStatePresenceFlagSubsystem);
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, presence);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(ScanPhase::Scanning));
	append_u8(bytes, static_cast<std::uint8_t>(DisclosureState::Hidden));
	append_u64(bytes, target_entity_id);
	if (subsystem_id != 0U) {
		append_u32(bytes, subsystem_id);
	}
	return bytes;
}

std::vector<std::uint8_t> docking_payload(std::uint64_t entity_id,
	std::uint64_t remote_entity_id,
	std::uint16_t local_index,
	std::uint16_t remote_index,
	std::string_view local_name,
	std::string_view remote_name)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(DockingPhase::Docked));
	append_u64(bytes, entity_id);
	append_u16(bytes, 1U);
	std::vector<std::uint8_t> item;
	append_u64(item, remote_entity_id);
	append_u16(item, local_index);
	append_u16(item, remote_index);
	append_string(item, local_name);
	append_string(item, remote_name);
	append_vlist_item(bytes, item);
	return bytes;
}

std::vector<std::uint8_t> support_payload(std::uint64_t entity_id, std::uint64_t support_entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, SupportStatePresenceFlagSupportEntity);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(SupportPhase::Approaching));
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	append_u64(bytes, support_entity_id);
	return bytes;
}

StateImage image_from_atoms(std::vector<StateAtom> atoms)
{
	StateImage image;
	EXPECT_EQ(StateImageResult::Created, StateImage::create(std::move(atoms), image));
	return image;
}

StateAtom atom(RecordType type,
	std::vector<std::uint8_t> value,
	std::size_t key_size,
	StateRecordLifecycle lifecycle = StateRecordLifecycle::UpsertOnly)
{
	StateAtom result;
	result.key.record_type = static_cast<std::uint16_t>(type);
	result.key.identity.assign(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(key_size));
	result.record_version = 1;
	result.lifecycle = lifecycle;
	result.value = std::move(value);
	return result;
}

void append_ship_atoms(std::vector<StateAtom>& atoms, std::uint64_t entity_id, std::uint32_t class_id)
{
	atoms.push_back(atom(RecordType::EntityLifecycle,
		lifecycle_payload(entity_id,
			ObjectType::Ship,
			EntityLifecyclePresenceFlagClassReference,
			0U,
			class_id),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));
	atoms.push_back(atom(RecordType::ShipIdentity, ship_identity_payload(entity_id, class_id), 8U));
	atoms.push_back(atom(RecordType::FlightState, flight_payload(entity_id), 8U));
	atoms.push_back(atom(RecordType::DamageState, damage_payload(entity_id), 8U));
	atoms.push_back(atom(RecordType::ShieldState, shield_payload(entity_id), 8U));
	atoms.push_back(atom(RecordType::EnergyState, energy_payload(entity_id), 8U));
	atoms.push_back(atom(RecordType::PropulsionState, propulsion_payload(entity_id), 8U));
}

StateImage make_image(std::vector<std::uint8_t> session, bool include_mission = true)
{
	std::vector<StateAtom> atoms;
	atoms.push_back(atom(RecordType::SessionState, std::move(session), 0));
	if (include_mission) {
		atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0));
	}
	atoms.push_back(atom(RecordType::EntityLifecycle,
		waypoint_lifecycle_payload(),
		8,
		StateRecordLifecycle::ExplicitCreateDelete));
	StateImage image;
	EXPECT_EQ(StateImageResult::Created, StateImage::create(std::move(atoms), image));
	return image;
}

BusinessStateValidationContext valid_context()
{
	BusinessStateValidationContext context;
	context.class_manifest_installed = true;
	return context;
}

TEST(TelemetryProtocolBusinessStateValidation, MinimalEngineNeutralSnapshotIsCompleteAndValid)
{
	const auto image = make_image(session_payload());
	auto context = valid_context();
	BusinessStateImageValidator validator(context);
	EXPECT_EQ(ValidationError::None, validator.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, SessionMissionManifestAndVisibilityGatesAreTransactional)
{
	const auto image = make_image(session_payload());
	auto missing_manifest = valid_context();
	missing_manifest.class_manifest_installed = false;
	BusinessStateImageValidator missing_manifest_validator(missing_manifest);
	EXPECT_EQ(ValidationError::MissingManifest, missing_manifest_validator.validate(image));

	auto visibility = valid_context();
	visibility.enforce_cockpit_entity_allowlist = true;
	BusinessStateImageValidator visibility_validator(visibility);
	EXPECT_EQ(ValidationError::VisibilityViolation, visibility_validator.validate(image));

	const auto no_mission = make_image(session_payload(), false);
	auto context = valid_context();
	BusinessStateImageValidator missing_mission_validator(context);
	EXPECT_EQ(ValidationError::InvalidAbsence, missing_mission_validator.validate(no_mission));
}

TEST(TelemetryProtocolBusinessStateValidation, TrustedFullStateRequiresBothOptInAndAllowlistedSource)
{
	const auto image = make_image(session_payload(VisibilityMode::TrustedFullState));
	auto context = valid_context();
	BusinessStateImageValidator closed(context);
	EXPECT_EQ(ValidationError::VisibilityViolation, closed.validate(image));

	context.trusted_full_state_authorized = true;
	context.source_endpoint_allowlisted = true;
	BusinessStateImageValidator allowed(context);
	EXPECT_EQ(ValidationError::None, allowed.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, ImmutableSessionFactsCannotChangeAcrossBaselines)
{
	const auto image = make_image(session_payload(VisibilityMode::Cockpit, 2));
	auto context = valid_context();
	context.previous_session.enforce = true;
	context.previous_session.producer_id = 1;
	context.previous_session.authority_mode = AuthorityMode::Solo;
	context.previous_session.visibility_mode = VisibilityMode::Cockpit;
	context.previous_session.state_domain_coverage = StateDomainCoverageBitCoreShip;
	context.previous_session.minimum_producer_sample_time_us = 100;
	context.previous_session.capability_generation = 1;
	context.previous_session.negotiated_capabilities = 0;
	BusinessStateImageValidator validator(context);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validator.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, ParentReferencesResolveAndCannotLeakHiddenEntities)
{
	std::vector<StateAtom> atoms;
	atoms.push_back(atom(RecordType::SessionState, session_payload(), 0));
	atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0));
	atoms.push_back(atom(RecordType::EntityLifecycle,
		lifecycle_payload(1,
			ObjectType::Waypoint,
			EntityLifecyclePresenceFlagParent,
			0,
			0,
			2),
		8,
		StateRecordLifecycle::ExplicitCreateDelete));
	auto image = image_from_atoms(atoms);
	auto context = valid_context();
	BusinessStateImageValidator missing_parent(context);
	EXPECT_EQ(ValidationError::UnknownEntity, missing_parent.validate(image));

	atoms.push_back(atom(RecordType::EntityLifecycle,
		waypoint_lifecycle_payload(2),
		8,
		StateRecordLifecycle::ExplicitCreateDelete));
	image = image_from_atoms(std::move(atoms));
	BusinessStateImageValidator complete(context);
	EXPECT_EQ(ValidationError::None, complete.validate(image));

	const std::uint64_t visible_only_child[] = {1};
	context.enforce_cockpit_entity_allowlist = true;
	context.cockpit_entity_ids = visible_only_child;
	context.cockpit_entity_count = 1;
	BusinessStateImageValidator hidden_parent(context);
	EXPECT_EQ(ValidationError::VisibilityViolation, hidden_parent.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, WeaponBombFlagMatchesTheInstalledManifestClass)
{
	std::vector<StateAtom> atoms;
	atoms.push_back(atom(RecordType::SessionState, session_payload(), 0));
	atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0));
	atoms.push_back(atom(RecordType::EntityLifecycle,
		lifecycle_payload(7,
			ObjectType::Weapon,
			EntityLifecyclePresenceFlagClassReference,
			EntityLifecycleFlagBomb,
			3),
		8,
		StateRecordLifecycle::ExplicitCreateDelete));
	const auto image = image_from_atoms(std::move(atoms));

	const std::uint32_t weapon_ids[] = {3};
	const std::uint64_t bomb_flags[] = {WeaponClassFlagBomb};
	auto context = valid_context();
	context.weapon_manifest_installed = true;
	context.weapon_class_ids = weapon_ids;
	context.weapon_class_flags = bomb_flags;
	context.weapon_class_count = 1;
	BusinessStateImageValidator valid(context);
	EXPECT_EQ(ValidationError::None, valid.validate(image));

	const std::uint64_t ordinary_flags[] = {0};
	context.weapon_class_flags = ordinary_flags;
	BusinessStateImageValidator mismatch(context);
	EXPECT_EQ(ValidationError::InvalidStateTransition, mismatch.validate(image));

	context.weapon_class_flags = nullptr;
	BusinessStateImageValidator missing_flags(context);
	EXPECT_EQ(ValidationError::MissingManifest, missing_flags.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, TargetAndLockReferencesMustResolveInsideTheVisibleImage)
{
	const auto coverage = StateDomainCoverageBitCoreShip | StateDomainCoverageBitTargeting;
	std::vector<StateAtom> atoms;
	atoms.push_back(atom(RecordType::SessionState,
		session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U),
		0U));
	atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	append_ship_atoms(atoms, 1U, 1U);
	atoms.push_back(atom(RecordType::LockState, lock_payload(1U), 8U));
	atoms.push_back(atom(RecordType::TargetState, target_payload(1U, 2U), 8U));

	const BusinessClassCatalogEntry classes[] = {{1U, nullptr, 0U}};
	auto context = valid_context();
	context.class_catalog = classes;
	context.class_catalog_count = 1U;
	auto image = image_from_atoms(atoms);
	BusinessStateImageValidator unknown_target(context);
	EXPECT_EQ(ValidationError::UnknownEntity, unknown_target.validate(image));

	atoms.push_back(atom(RecordType::EntityLifecycle,
		waypoint_lifecycle_payload(2U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));
	image = image_from_atoms(std::move(atoms));
	BusinessStateImageValidator complete(context);
	EXPECT_EQ(ValidationError::None, complete.validate(image));

	const std::uint64_t observer_only[] = {1U};
	context.enforce_cockpit_entity_allowlist = true;
	context.cockpit_entity_ids = observer_only;
	context.cockpit_entity_count = 1U;
	BusinessStateImageValidator hidden_target(context);
	EXPECT_EQ(ValidationError::VisibilityViolation, hidden_target.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, TargetSubsystemMustBelongToTheInstalledClassCatalog)
{
	const auto coverage = StateDomainCoverageBitCoreShip | StateDomainCoverageBitTargeting;
	std::vector<StateAtom> atoms;
	atoms.push_back(atom(RecordType::SessionState,
		session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U),
		0U));
	atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	append_ship_atoms(atoms, 1U, 1U);
	append_ship_atoms(atoms, 2U, 2U);
	atoms.push_back(atom(RecordType::SubsystemState, subsystem_payload(2U, 7U), 12U));
	atoms.push_back(atom(RecordType::LockState, lock_payload(1U, 2U, 7U), 8U));
	atoms.push_back(atom(RecordType::TargetState, target_subsystem_payload(1U, 2U, 7U), 8U));
	const auto image = image_from_atoms(std::move(atoms));

	const std::uint32_t subsystem_ids[] = {7U};
	const BusinessClassCatalogEntry classes[] = {{1U, nullptr, 0U}, {2U, subsystem_ids, 1U}};
	auto context = valid_context();
	context.class_catalog = classes;
	context.class_catalog_count = 2U;
	BusinessStateImageValidator valid(context);
	EXPECT_EQ(ValidationError::None, valid.validate(image));

	const BusinessClassCatalogEntry missing_subsystem[] = {{1U, nullptr, 0U}, {2U, nullptr, 0U}};
	context.class_catalog = missing_subsystem;
	BusinessStateImageValidator missing_catalog_entry(context);
	EXPECT_EQ(ValidationError::MissingManifest, missing_catalog_entry.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, RadarContactMatchesLifecycleCatalogBombAndCurrentTarget)
{
	const auto coverage = StateDomainCoverageBitCoreShip | StateDomainCoverageBitRadarSensors |
		StateDomainCoverageBitTargeting;
	std::vector<StateAtom> base;
	base.push_back(atom(RecordType::SessionState,
		session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U),
		0U));
	base.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	append_ship_atoms(base, 1U, 1U);
	base.push_back(atom(RecordType::LockState, lock_payload(1U), 8U));
	base.push_back(atom(RecordType::TargetState, target_payload(1U, 2U), 8U));
	base.push_back(atom(RecordType::RadarState, radar_payload(1U), 8U));
	base.push_back(atom(RecordType::ThreatState, threat_payload(1U), 8U));
	base.push_back(atom(RecordType::EntityLifecycle,
		lifecycle_payload(2U,
			ObjectType::Weapon,
			EntityLifecyclePresenceFlagClassReference,
			EntityLifecycleFlagBomb,
			3U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));

	const BusinessClassCatalogEntry classes[] = {{1U, nullptr, 0U}};
	const std::uint32_t weapon_ids[] = {3U};
	const std::uint64_t weapon_flags[] = {WeaponClassFlagBomb};
	auto context = valid_context();
	context.class_catalog = classes;
	context.class_catalog_count = 1U;
	context.weapon_manifest_installed = true;
	context.weapon_class_ids = weapon_ids;
	context.weapon_class_flags = weapon_flags;
	context.weapon_class_count = 1U;

	auto valid_atoms = base;
	valid_atoms.push_back(atom(RecordType::RadarContacts,
		radar_contact_payload(1U, 2U, ObjectType::Weapon, ContactFlagBomb | ContactFlagCurrentTarget, 3U),
		16U,
		StateRecordLifecycle::ExplicitCreateDelete));
	auto image = image_from_atoms(std::move(valid_atoms));
	BusinessStateImageValidator valid(context);
	EXPECT_EQ(ValidationError::None, valid.validate(image));

	auto missing_bomb_atoms = base;
	missing_bomb_atoms.push_back(atom(RecordType::RadarContacts,
		radar_contact_payload(1U, 2U, ObjectType::Weapon, ContactFlagCurrentTarget),
		16U,
		StateRecordLifecycle::ExplicitCreateDelete));
	image = image_from_atoms(std::move(missing_bomb_atoms));
	BusinessStateImageValidator missing_bomb(context);
	EXPECT_EQ(ValidationError::InvalidStateTransition, missing_bomb.validate(image));

	auto stale_target_atoms = base;
	stale_target_atoms.push_back(atom(RecordType::RadarContacts,
		radar_contact_payload(1U, 2U, ObjectType::Weapon, ContactFlagBomb),
		16U,
		StateRecordLifecycle::ExplicitCreateDelete));
	image = image_from_atoms(std::move(stale_target_atoms));
	BusinessStateImageValidator stale_target(context);
	EXPECT_EQ(ValidationError::InvalidStateTransition, stale_target.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, ThreatMissilesResolveAndMatchTheirWeaponCatalogClass)
{
	const auto coverage = StateDomainCoverageBitCoreShip | StateDomainCoverageBitRadarSensors;
	std::vector<StateAtom> base;
	base.push_back(atom(RecordType::SessionState,
		session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U),
		0U));
	base.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	append_ship_atoms(base, 1U, 1U);
	base.push_back(atom(RecordType::RadarState, radar_payload(1U), 8U));
	base.push_back(atom(RecordType::EntityLifecycle,
		lifecycle_payload(2U,
			ObjectType::Weapon,
			EntityLifecyclePresenceFlagClassReference,
			0U,
			3U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));

	const BusinessClassCatalogEntry classes[] = {{1U, nullptr, 0U}};
	const std::uint32_t weapon_ids[] = {3U, 4U};
	const std::uint64_t weapon_flags[] = {0U, 0U};
	auto context = valid_context();
	context.class_catalog = classes;
	context.class_catalog_count = 1U;
	context.weapon_manifest_installed = true;
	context.weapon_class_ids = weapon_ids;
	context.weapon_class_flags = weapon_flags;
	context.weapon_class_count = 2U;

	auto valid_atoms = base;
	valid_atoms.push_back(atom(RecordType::ThreatState, threat_payload(1U, 2U, 3U, 2U), 8U));
	auto image = image_from_atoms(std::move(valid_atoms));
	BusinessStateImageValidator valid(context);
	EXPECT_EQ(ValidationError::None, valid.validate(image));

	auto mismatched_atoms = base;
	mismatched_atoms.push_back(atom(RecordType::ThreatState, threat_payload(1U, 2U, 4U), 8U));
	image = image_from_atoms(std::move(mismatched_atoms));
	BusinessStateImageValidator mismatched_class(context);
	EXPECT_EQ(ValidationError::InvalidStateTransition, mismatched_class.validate(image));

	auto unknown_atoms = base;
	unknown_atoms.push_back(atom(RecordType::ThreatState, threat_payload(1U, 99U, 3U), 8U));
	image = image_from_atoms(std::move(unknown_atoms));
	BusinessStateImageValidator unknown_missile(context);
	EXPECT_EQ(ValidationError::UnknownEntity, unknown_missile.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, CargoAndSupportReferencesCannotNameUnknownOrWrongTypeEntities)
{
	const auto coverage = StateDomainCoverageBitCoreShip | StateDomainCoverageBitCargoDockSupport;
	std::vector<StateAtom> cargo_atoms;
	cargo_atoms.push_back(atom(RecordType::SessionState,
		session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U),
		0U));
	cargo_atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	append_ship_atoms(cargo_atoms, 1U, 1U);
	cargo_atoms.push_back(atom(RecordType::CargoScanState, cargo_payload(1U, 99U), 8U));
	const auto cargo_image = image_from_atoms(std::move(cargo_atoms));
	const BusinessClassCatalogEntry classes[] = {{1U, nullptr, 0U}};
	auto context = valid_context();
	context.class_catalog = classes;
	context.class_catalog_count = 1U;
	BusinessStateImageValidator unknown_cargo_target(context);
	EXPECT_EQ(ValidationError::UnknownEntity, unknown_cargo_target.validate(cargo_image));

	std::vector<StateAtom> support_atoms;
	support_atoms.push_back(atom(RecordType::SessionState,
		session_payload(VisibilityMode::Cockpit, 1U, coverage),
		0U));
	support_atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	support_atoms.push_back(atom(RecordType::EntityLifecycle,
		waypoint_lifecycle_payload(1U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));
	support_atoms.push_back(atom(RecordType::EntityLifecycle,
		waypoint_lifecycle_payload(2U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));
	support_atoms.push_back(atom(RecordType::SupportState, support_payload(1U, 2U), 8U));
	const auto support_image = image_from_atoms(std::move(support_atoms));
	BusinessStateImageValidator wrong_support_type(context);
	EXPECT_EQ(ValidationError::InvalidStateTransition, wrong_support_type.validate(support_image));
}

TEST(TelemetryProtocolBusinessStateValidation, PublishedDockingRelationsMustHaveAnExactInverse)
{
	const auto coverage = StateDomainCoverageBitCoreShip | StateDomainCoverageBitCargoDockSupport;
	std::vector<StateAtom> base;
	base.push_back(atom(RecordType::SessionState,
		session_payload(VisibilityMode::Cockpit, 1U, coverage),
		0U));
	base.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	base.push_back(atom(RecordType::EntityLifecycle,
		waypoint_lifecycle_payload(1U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));
	base.push_back(atom(RecordType::EntityLifecycle,
		waypoint_lifecycle_payload(2U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));
	base.push_back(atom(RecordType::DockingState, docking_payload(1U, 2U, 3U, 4U, "alpha", "beta"), 8U));

	auto valid_atoms = base;
	valid_atoms.push_back(atom(RecordType::DockingState,
		docking_payload(2U, 1U, 4U, 3U, "beta", "alpha"),
		8U));
	auto image = image_from_atoms(std::move(valid_atoms));
	auto context = valid_context();
	BusinessStateImageValidator valid(context);
	EXPECT_EQ(ValidationError::None, valid.validate(image));

	auto mismatched_atoms = base;
	mismatched_atoms.push_back(atom(RecordType::DockingState,
		docking_payload(2U, 1U, 4U, 3U, "wrong", "alpha"),
		8U));
	image = image_from_atoms(std::move(mismatched_atoms));
	BusinessStateImageValidator mismatch(context);
	EXPECT_EQ(ValidationError::InvalidStateTransition, mismatch.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, ParentChainsCannotContainCycles)
{
	std::vector<StateAtom> atoms;
	atoms.push_back(atom(RecordType::SessionState, session_payload(), 0U));
	atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	atoms.push_back(atom(RecordType::EntityLifecycle,
		lifecycle_payload(1U, ObjectType::Waypoint, EntityLifecyclePresenceFlagParent, 0U, 0U, 2U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));
	atoms.push_back(atom(RecordType::EntityLifecycle,
		lifecycle_payload(2U, ObjectType::Waypoint, EntityLifecyclePresenceFlagParent, 0U, 0U, 1U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete));
	const auto image = image_from_atoms(std::move(atoms));
	auto context = valid_context();
	BusinessStateImageValidator validator(context);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validator.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, LargeParentForestUsesCompleteKeysAndValidatesAcyclicChains)
{
	constexpr std::uint64_t EntityCount = 1024U;
	std::vector<StateAtom> atoms;
	atoms.reserve(static_cast<std::size_t>(EntityCount) + 2U);
	atoms.push_back(atom(RecordType::SessionState, session_payload(), 0U));
	atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	for (std::uint64_t entity_id = 1U; entity_id <= EntityCount; ++entity_id) {
		const auto parent_entity_id = entity_id == EntityCount ? 0U : entity_id + 1U;
		const auto presence = parent_entity_id == 0U ? 0U : EntityLifecyclePresenceFlagParent;
		atoms.push_back(atom(RecordType::EntityLifecycle,
			lifecycle_payload(entity_id, ObjectType::Waypoint, presence, 0U, 0U, parent_entity_id),
			8U,
			StateRecordLifecycle::ExplicitCreateDelete));
	}

	const auto image = image_from_atoms(std::move(atoms));
	auto context = valid_context();
	BusinessStateImageValidator validator(context);
	EXPECT_EQ(ValidationError::None, validator.validate(image));
}

} // namespace
