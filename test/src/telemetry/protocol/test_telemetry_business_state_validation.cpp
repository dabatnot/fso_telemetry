#include "telemetry/protocol/telemetry_business_state_validation.h"

#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/protocol/telemetry_specialized_views.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{bytes.data(), bytes.size()};
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

std::vector<std::uint8_t> control_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	for (unsigned int index = 0; index < 6U; ++index) {
		append_f32(bytes, 0.0F);
	}
	append_u8(bytes, static_cast<std::uint8_t>(ControlMode::Ship));
	append_u32(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> weapon_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u16(bytes, 0U);
	append_u16(bytes, 0U);
	append_u16(bytes, 0U);
	append_u16(bytes, 0U);
	append_u32(bytes, 0U);
	append_u32(bytes, 0U);
	append_u32(bytes, 0U);
	append_u32(bytes, 0U);
	append_u16(bytes, 0U);
	append_u16(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> cargo_none_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(ScanPhase::NotScannable));
	append_u8(bytes, static_cast<std::uint8_t>(DisclosureState::Hidden));
	return bytes;
}

std::vector<std::uint8_t> docking_none_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(DockingPhase::None));
	append_u64(bytes, 0U);
	append_u16(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> support_none_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, 0U);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(SupportPhase::None));
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	append_u8(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> navigation_none_payload(std::uint64_t entity_id)
{
	std::vector<std::uint8_t> bytes;
	append_u64(bytes, entity_id);
	append_u64(bytes, NavigationStatePresenceFlagNone);
	append_u64(bytes, 100U);
	append_u8(bytes, static_cast<std::uint8_t>(AutopilotState::Disengaged));
	append_u16(bytes, 0U);
	return bytes;
}

std::vector<std::uint8_t> inactive_comm_state_payload()
{
	CommViewStatePayload payload;
	std::vector<std::uint8_t> bytes(CommViewStatePayloadSize);
	std::size_t written = 0U;
	EXPECT_EQ(ValidationError::None,
		encode_comm_view_state_payload(payload, mutable_byte_view(bytes), written));
	EXPECT_EQ(bytes.size(), written);
	return bytes;
}

std::vector<std::uint8_t> class_manifest_with_subsystem_payload()
{
	std::vector<std::uint8_t> bytes;
	append_u32(bytes, 1U);
	append_u32(bytes, 1U);
	append_u64(bytes, ClassManifestPresenceFlagSubsystems);
	append_string(bytes, "ship");
	append_u32(bytes, 0U);
	append_u32(bytes, 0U);
	append_f32(bytes, 1.0F);
	append_vec3(bytes);
	append_u16(bytes, 1U);
	std::vector<std::uint8_t> subsystem;
	append_u16(subsystem, 0U);
	append_u32(subsystem, 7U);
	append_u16(subsystem, 0U);
	append_u8(subsystem, static_cast<std::uint8_t>(SubsystemType::Engine));
	append_u8(subsystem, 0U);
	append_string(subsystem, "engine");
	for (unsigned int index = 0U; index < 5U; ++index) {
		append_f32(subsystem, 0.0F);
	}
	append_u32(subsystem, 0U);
	append_vlist_item(bytes, subsystem);
	return bytes;
}

std::vector<std::uint8_t> weapon_manifest_payload()
{
	std::vector<std::uint8_t> bytes;
	append_u32(bytes, 1U);
	append_u32(bytes, 1U);
	append_u64(bytes, WeaponManifestPresenceFlagNone);
	append_string(bytes, "weapon");
	append_u8(bytes, static_cast<std::uint8_t>(WeaponSubtype::Unknown));
	append_u64(bytes, 0U);
	append_f32(bytes, 0.0F);
	append_f32(bytes, 0.0F);
	append_f32(bytes, 0.0F);
	append_u64(bytes, 0U);
	append_f32(bytes, 0.0F);
	return bytes;
}

std::vector<std::uint8_t> record_envelope(RecordType type, const std::vector<std::uint8_t>& payload)
{
	std::vector<std::uint8_t> bytes;
	append_u16(bytes, static_cast<std::uint16_t>(type));
	append_u8(bytes, 1U);
	append_u8(bytes, RecordFlagNone);
	append_u16(bytes, static_cast<std::uint16_t>(payload.size()));
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	return bytes;
}

TransactionPart manifest_transaction_part(const ManifestPartPayload& payload, std::uint32_t message_id)
{
	TransactionPart part;
	part.session_id = 42U;
	part.message_type = MessageType::Manifest;
	part.transaction_id = payload.manifest_id;
	part.message_id = message_id;
	part.part_index = payload.part_index;
	part.part_count = payload.part_count;
	part.transaction_size = payload.transaction_size;
	part.transaction_sha256 = payload.transaction_sha256;
	part.producer_sample_time_us = payload.producer_sample_time_us;
	part.kind_or_flags = static_cast<std::uint16_t>(payload.manifest_kind);
	part.record_count = payload.record_count;
	part.records = payload.records;
	return part;
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
	const std::uint32_t presence = static_cast<std::uint32_t>(CargoScanStatePresenceFlagTarget) |
		(subsystem_id == 0U ? 0U : static_cast<std::uint32_t>(CargoScanStatePresenceFlagSubsystem));
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

StateImage make_phase1_image(std::uint64_t coverage = StateDomainCoverageBitPlayerKinematics,
	std::uint64_t player = 1U,
	VisibilityMode visibility = VisibilityMode::Cockpit)
{
	std::vector<StateAtom> atoms;
	atoms.push_back(atom(RecordType::SessionState, session_payload(visibility, 1U, coverage, player), 0));
	atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0));
	atoms.push_back(atom(RecordType::EntityLifecycle,
		lifecycle_payload(player, ObjectType::Ship, 0U, 0U), 8U, StateRecordLifecycle::ExplicitCreateDelete));
	atoms.push_back(atom(RecordType::FlightState, flight_payload(player), 8U));
	StateImage image;
	EXPECT_EQ(StateImageResult::Created, StateImage::create(std::move(atoms), image));
	return image;
}

StateImage make_phase2_core_image(RecordType omitted = RecordType::Invalid)
{
	constexpr std::uint64_t coverage =
		StateDomainCoverageBitPlayerKinematics | StateDomainCoverageBitCoreShip;
	std::vector<StateAtom> atoms;
	auto add = [&](RecordType type, std::vector<std::uint8_t> payload, std::size_t key_size,
				   StateRecordLifecycle lifecycle = StateRecordLifecycle::UpsertOnly) {
		if (type != omitted) {
			atoms.push_back(atom(type, std::move(payload), key_size, lifecycle));
		}
	};
	add(RecordType::SessionState, session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U), 0U);
	add(RecordType::MissionState, mission_payload(), 0U);
	add(RecordType::EntityLifecycle,
		lifecycle_payload(1U,
			ObjectType::Ship,
			EntityLifecyclePresenceFlagClassReference,
			0U,
			1U),
		8U,
		StateRecordLifecycle::ExplicitCreateDelete);
	add(RecordType::ShipIdentity, ship_identity_payload(1U, 1U), 8U);
	add(RecordType::FlightState, flight_payload(1U), 8U);
	add(RecordType::DamageState, damage_payload(1U), 8U);
	add(RecordType::ShieldState, shield_payload(1U), 8U);
	add(RecordType::EnergyState, energy_payload(1U), 8U);
	add(RecordType::PropulsionState, propulsion_payload(1U), 8U);
	add(RecordType::SubsystemState, subsystem_payload(1U, 7U), 12U);
	return image_from_atoms(std::move(atoms));
}

StateImage make_phase2_complete_image(std::size_t ship_count = 1U,
	RecordType omitted = RecordType::Invalid)
{
	constexpr std::uint64_t coverage = StateDomainCoverageBitPlayerKinematics |
		StateDomainCoverageBitCoreShip | StateDomainCoverageBitControlInputs |
		StateDomainCoverageBitWeapons | StateDomainCoverageBitCargoDockSupport;
	std::vector<StateAtom> atoms;
	auto add = [&](RecordType type, std::vector<std::uint8_t> payload, std::size_t key_size,
				   StateRecordLifecycle lifecycle = StateRecordLifecycle::UpsertOnly) {
		if (type != omitted) {
			atoms.push_back(atom(type, std::move(payload), key_size, lifecycle));
		}
	};
	add(RecordType::SessionState, session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U), 0U);
	add(RecordType::MissionState, mission_payload(), 0U);
	add(RecordType::ControlState, control_payload(1U), 8U);
	add(RecordType::CargoScanState, cargo_none_payload(1U), 8U);
	for (std::uint64_t entity_id = 1U; entity_id <= ship_count; ++entity_id) {
		add(RecordType::EntityLifecycle,
			lifecycle_payload(entity_id,
				ObjectType::Ship,
				EntityLifecyclePresenceFlagClassReference,
				0U,
				1U),
			8U,
			StateRecordLifecycle::ExplicitCreateDelete);
		add(RecordType::ShipIdentity, ship_identity_payload(entity_id, 1U), 8U);
		add(RecordType::FlightState, flight_payload(entity_id), 8U);
		add(RecordType::DamageState, damage_payload(entity_id), 8U);
		add(RecordType::ShieldState, shield_payload(entity_id), 8U);
		add(RecordType::EnergyState, energy_payload(entity_id), 8U);
		add(RecordType::PropulsionState, propulsion_payload(entity_id), 8U);
		add(RecordType::SubsystemState, subsystem_payload(entity_id, 7U), 12U);
		add(RecordType::WeaponState, weapon_payload(entity_id), 8U);
		add(RecordType::DockingState, docking_none_payload(entity_id), 8U);
		add(RecordType::SupportState, support_none_payload(entity_id), 8U);
	}
	return image_from_atoms(std::move(atoms));
}

StateImage make_phase2_complete_image_with_capabilities(std::uint64_t capabilities)
{
	auto atoms = make_phase2_complete_image().records();
	auto session = std::find_if(atoms.begin(), atoms.end(), [](const StateAtom& atom) {
		return atom.key.record_type == static_cast<std::uint16_t>(RecordType::SessionState);
	});
	EXPECT_NE(atoms.end(), session);
	if (session != atoms.end() && session->value.size() >= 40U) {
		for (unsigned int byte = 0U; byte < 8U; ++byte) {
			session->value[32U + byte] =
				static_cast<std::uint8_t>(capabilities >> (byte * 8U));
		}
	}
	return image_from_atoms(std::move(atoms));
}

BusinessStateValidationContext phase2_complete_context()
{
	static constexpr std::uint32_t SubsystemIds[]{7U};
	static constexpr BusinessClassCatalogEntry ClassCatalog[]{{1U, SubsystemIds, 1U}};
	auto context = valid_context();
	context.protocol_minor = VersionMinorV1_1;
	context.required_manifest_id = 1U;
	context.weapon_manifest_installed = true;
	context.class_catalog = ClassCatalog;
	context.class_catalog_count = 1U;
	return context;
}

TEST(TelemetryProtocolBusinessStateValidation, CompleteShipK1HasExactlyFourPlusTenKPlusNRecords)
{
	const auto image = make_phase2_complete_image();
	constexpr std::size_t ShipCount = 1U;
	constexpr std::size_t SubsystemCount = ShipCount;
	ASSERT_EQ(4U + 10U * ShipCount + SubsystemCount, image.records().size());
	auto context = phase2_complete_context();
	BusinessStateImageValidator validator(context);
	EXPECT_EQ(ValidationError::None, validator.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, CompleteShipK1RejectsMandatoryRecordAndManifestOmissions)
{
	constexpr std::array<RecordType, 5> OmittedRecords{{
		RecordType::ControlState,
		RecordType::WeaponState,
		RecordType::CargoScanState,
		RecordType::DockingState,
		RecordType::SupportState,
	}};
	auto context = phase2_complete_context();
	BusinessStateImageValidator validator(context);
	for (const auto omitted : OmittedRecords) {
		SCOPED_TRACE(static_cast<std::uint16_t>(omitted));
		EXPECT_EQ(ValidationError::InvalidAbsence,
			validator.validate(make_phase2_complete_image(1U, omitted)));
	}

	const auto complete = make_phase2_complete_image();
	auto missing_class_manifest = phase2_complete_context();
	missing_class_manifest.class_manifest_installed = false;
	BusinessStateImageValidator missing_class_validator(missing_class_manifest);
	EXPECT_EQ(ValidationError::MissingManifest, missing_class_validator.validate(complete));

	auto missing_weapon_manifest = phase2_complete_context();
	missing_weapon_manifest.weapon_manifest_installed = false;
	BusinessStateImageValidator missing_weapon_validator(missing_weapon_manifest);
	EXPECT_EQ(ValidationError::MissingManifest, missing_weapon_validator.validate(complete));
}

TEST(TelemetryProtocolBusinessStateValidation, CompleteShipWaitsForAtomicClassAndWeaponManifestTransaction)
{
	const auto class_record =
		record_envelope(RecordType::ClassManifest, class_manifest_with_subsystem_payload());
	const auto weapon_record = record_envelope(RecordType::WeaponManifest, weapon_manifest_payload());
	std::vector<std::uint8_t> transaction_region = class_record;
	transaction_region.insert(transaction_region.end(), weapon_record.begin(), weapon_record.end());
	Sha256Digest digest{};
	ASSERT_TRUE(sha256(byte_view(transaction_region), digest));

	auto make_part = [&](std::uint16_t part_index, const std::vector<std::uint8_t>& records) {
		ManifestPartPayload payload;
		payload.manifest_id = 1U;
		payload.part_index = part_index;
		payload.part_count = 2U;
		payload.transaction_size = static_cast<std::uint32_t>(transaction_region.size());
		payload.transaction_sha256 = digest;
		payload.producer_sample_time_us = 100U;
		payload.manifest_kind = ManifestKind::FullRequired;
		payload.record_count = 1U;
		payload.records = byte_view(records);
		std::vector<std::uint8_t> encoded(ManifestPartPayloadPrefixSize + records.size());
		std::size_t written = 0U;
		EXPECT_EQ(ValidationError::None,
			encode_manifest_part_payload(payload, mutable_byte_view(encoded), written));
		EXPECT_EQ(encoded.size(), written);
		ManifestPartPayload decoded;
		EXPECT_EQ(ValidationError::None, decode_manifest_part_payload(byte_view(encoded), decoded));
		return std::make_pair(std::move(encoded), decoded);
	};

	auto class_part = make_part(0U, class_record);
	auto weapon_part = make_part(1U, weapon_record);
	TelemetryTransactionAssembler assembler;
	CompletedTransaction completed;
	EXPECT_EQ(TransactionAssemblyResult::Accepted,
		assembler.ingest(manifest_transaction_part(class_part.second, 10U), 0U, completed));
	EXPECT_TRUE(completed.parts.empty());

	const auto image = make_phase2_complete_image();
	auto unavailable = phase2_complete_context();
	unavailable.class_manifest_installed = false;
	unavailable.weapon_manifest_installed = false;
	BusinessStateImageValidator unavailable_validator(unavailable);
	EXPECT_EQ(ValidationError::MissingManifest, unavailable_validator.validate(image));

	ASSERT_EQ(TransactionAssemblyResult::Completed,
		assembler.ingest(manifest_transaction_part(weapon_part.second, 11U), 1U, completed));
	ASSERT_EQ(2U, completed.parts.size());
	for (const auto& part : completed.parts) {
		RecordEnvelopeIterator iterator(part.records_view(), part.record_count, RecordFlagPolicy::RequireNone);
		RecordEnvelopeView envelope;
		bool has_value = false;
		ASSERT_EQ(ValidationError::None, iterator.next(envelope, has_value));
		ASSERT_TRUE(has_value);
		BusinessRecordMetadata metadata;
		EXPECT_EQ(ValidationError::None,
			validate_business_record(
				envelope, BusinessRecordContainer::Manifest, VersionMinorV1_1, metadata));
		EXPECT_EQ(ValidationError::None, iterator.next(envelope, has_value));
		EXPECT_FALSE(has_value);
	}

	auto installed = phase2_complete_context();
	BusinessStateImageValidator installed_validator(installed);
	EXPECT_EQ(ValidationError::None, installed_validator.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, CoreGateRejectsEveryMandatoryCoreRecordOmission)
{
	constexpr std::array<std::pair<RecordType, ValidationError>, 8> OmittedRecords{{
		{RecordType::EntityLifecycle, ValidationError::UnknownEntity},
		{RecordType::ShipIdentity, ValidationError::InvalidAbsence},
		{RecordType::FlightState, ValidationError::InvalidAbsence},
		{RecordType::DamageState, ValidationError::InvalidAbsence},
		{RecordType::ShieldState, ValidationError::InvalidAbsence},
		{RecordType::EnergyState, ValidationError::InvalidAbsence},
		{RecordType::PropulsionState, ValidationError::InvalidAbsence},
		{RecordType::SubsystemState, ValidationError::InvalidAbsence},
	}};
	const std::uint32_t subsystem_ids[] = {7U};
	const BusinessClassCatalogEntry classes[] = {{1U, subsystem_ids, 1U}};
	auto context = valid_context();
	context.protocol_minor = VersionMinorV1_1;
	context.required_manifest_id = 1U;
	context.class_catalog = classes;
	context.class_catalog_count = 1U;
	BusinessStateImageValidator validator(context);
	ASSERT_EQ(ValidationError::None, validator.validate(make_phase2_core_image()));
	for (const auto omitted : OmittedRecords) {
		SCOPED_TRACE(static_cast<std::uint16_t>(omitted.first));
		EXPECT_EQ(omitted.second,
			validator.validate(make_phase2_core_image(omitted.first)));
	}

	auto missing_manifest = context;
	missing_manifest.class_manifest_installed = false;
	BusinessStateImageValidator missing_manifest_validator(missing_manifest);
	EXPECT_EQ(ValidationError::MissingManifest,
		missing_manifest_validator.validate(make_phase2_core_image()));
}

TEST(TelemetryProtocolBusinessStateValidation, Phase2RecordsCannotAppearWithoutTheirDomains)
{
	const std::uint32_t subsystem_ids[] = {7U};
	const BusinessClassCatalogEntry classes[] = {{1U, subsystem_ids, 1U}};
	auto context = valid_context();
	context.protocol_minor = VersionMinorV1_1;
	context.required_manifest_id = 1U;
	context.weapon_manifest_installed = true;
	context.class_catalog = classes;
	context.class_catalog_count = 1U;
	BusinessStateImageValidator validator(context);

	const std::array<std::pair<RecordType, std::vector<std::uint8_t>>, 3> records{{
		{RecordType::ControlState, control_payload(1U)},
		{RecordType::WeaponState, weapon_payload(1U)},
		{RecordType::SupportState, support_none_payload(1U)},
	}};
	for (const auto& record : records) {
		auto image = make_phase2_core_image();
		auto atoms = image.records();
		atoms.push_back(atom(record.first, record.second, 8U));
		SCOPED_TRACE(static_cast<std::uint16_t>(record.first));
		EXPECT_EQ(ValidationError::InvalidAbsence,
			validator.validate(image_from_atoms(std::move(atoms))));
	}
}

TEST(TelemetryProtocolBusinessStateValidation, CompleteShipRejectsSpecializedRecordsAndCapabilities)
{
	auto context = phase2_complete_context();
	BusinessStateImageValidator validator(context);
	const std::array<std::pair<RecordType, std::vector<std::uint8_t>>, 6> forbidden{{
		{RecordType::LockState, lock_payload(1U)},
		{RecordType::TargetState, target_payload(1U, 0U)},
		{RecordType::RadarState, radar_payload(1U)},
		{RecordType::RadarContacts,
			radar_contact_payload(1U, 2U, ObjectType::Weapon, ContactFlagBomb)},
		{RecordType::ThreatState, threat_payload(1U)},
		{RecordType::NavigationState, navigation_none_payload(1U)},
	}};
	for (const auto& record : forbidden) {
		auto atoms = make_phase2_complete_image().records();
		const auto key_size = record.first == RecordType::RadarContacts ? 16U : 8U;
		atoms.push_back(atom(record.first,
			record.second,
			key_size,
			record.first == RecordType::RadarContacts ? StateRecordLifecycle::ExplicitCreateDelete
													 : StateRecordLifecycle::UpsertOnly));
		SCOPED_TRACE(static_cast<std::uint16_t>(record.first));
		EXPECT_EQ(ValidationError::InvalidAbsence,
			validator.validate(image_from_atoms(std::move(atoms))));
	}

	auto comm_atoms = make_phase2_complete_image().records();
	comm_atoms.push_back(atom(RecordType::CommViewState, inactive_comm_state_payload(), 0U));
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validator.validate(image_from_atoms(std::move(comm_atoms))));
}

TEST(TelemetryProtocolBusinessStateValidation,
	S12TST009FinalInjectsRecords15Through19And23PlusBothValidCapabilityPairs)
{
	static_assert(static_cast<std::uint16_t>(RecordType::LockState) == 15U);
	static_assert(static_cast<std::uint16_t>(RecordType::TargetState) == 16U);
	static_assert(static_cast<std::uint16_t>(RecordType::RadarState) == 17U);
	static_assert(static_cast<std::uint16_t>(RecordType::RadarContacts) == 18U);
	static_assert(static_cast<std::uint16_t>(RecordType::ThreatState) == 19U);
	static_assert(static_cast<std::uint16_t>(RecordType::NavigationState) == 23U);

	auto context = phase2_complete_context();
	BusinessStateImageValidator validator(context);
	const std::array<std::pair<RecordType, std::vector<std::uint8_t>>, 6> specialized_records{{
		{RecordType::LockState, lock_payload(1U)},
		{RecordType::TargetState, target_payload(1U, 0U)},
		{RecordType::RadarState, radar_payload(1U)},
		{RecordType::RadarContacts,
			radar_contact_payload(1U, 2U, ObjectType::Weapon, ContactFlagBomb)},
		{RecordType::ThreatState, threat_payload(1U)},
		{RecordType::NavigationState, navigation_none_payload(1U)},
	}};
	for (const auto& record : specialized_records) {
		auto atoms = make_phase2_complete_image().records();
		const auto contacts = record.first == RecordType::RadarContacts;
		atoms.push_back(atom(record.first,
			record.second,
			contacts ? 16U : 8U,
			contacts ? StateRecordLifecycle::ExplicitCreateDelete
					 : StateRecordLifecycle::UpsertOnly));
		SCOPED_TRACE(static_cast<std::uint16_t>(record.first));
		EXPECT_EQ(ValidationError::InvalidAbsence,
			validator.validate(image_from_atoms(std::move(atoms))));
	}

	auto comm_atoms = make_phase2_complete_image().records();
	comm_atoms.push_back(atom(RecordType::CommViewState, inactive_comm_state_payload(), 0U));
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validator.validate(image_from_atoms(std::move(comm_atoms))));

	constexpr std::uint64_t VideoPair =
		static_cast<std::uint64_t>(CapabilityTargetVideoH264) |
		static_cast<std::uint64_t>(CapabilityTargetVideoRemoteRender);
	constexpr std::uint64_t CommViewPair =
		static_cast<std::uint64_t>(CapabilityCommViewLocalAssets) |
		static_cast<std::uint64_t>(CapabilityCommViewAuthoritativeSource);
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validator.validate(make_phase2_complete_image_with_capabilities(VideoPair)));
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validator.validate(make_phase2_complete_image_with_capabilities(CommViewPair)));
}

TEST(TelemetryProtocolBusinessStateValidation, CompleteShipAcceptsK2AndK64ButRejectsK65)
{
	auto context = phase2_complete_context();
	BusinessStateImageValidator validator(context);
	for (const std::size_t ship_count : {2U, 64U}) {
		const auto image = make_phase2_complete_image(ship_count);
		const auto subsystem_count = ship_count;
		SCOPED_TRACE(ship_count);
		EXPECT_EQ(4U + 10U * ship_count + subsystem_count, image.records().size());
		EXPECT_EQ(ValidationError::None, validator.validate(image));
	}

	const auto over_limit = make_phase2_complete_image(65U);
	EXPECT_EQ(ValidationError::ResourceLimit, validator.validate(over_limit));
}

TEST(TelemetryProtocolBusinessStateValidation, CompleteShipCoverageCannotChangeByDelta)
{
	const auto baseline = make_phase1_image(StateDomainCoverageBitPlayerKinematics);
	const auto promoted = make_phase2_complete_image();
	auto baseline_context = valid_context();
	baseline_context.protocol_minor = VersionMinorV1_1;
	baseline_context.required_manifest_id = 0U;
	BusinessStateImageValidator baseline_validator(baseline_context);
	ASSERT_EQ(ValidationError::None, baseline_validator.validate(baseline));

	auto phase2_context = phase2_complete_context();
	BusinessStateImageValidator phase2_validator(phase2_context);
	ASSERT_EQ(ValidationError::None, phase2_validator.validate(promoted));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		phase2_validator.validate_delta_transition(baseline, promoted));
}

TEST(TelemetryProtocolBusinessStateValidation, GenericCargoDomainDoesNotRequirePerShipDockingOrSupport)
{
	constexpr std::uint64_t generic_coverage =
		StateDomainCoverageBitCoreShip | StateDomainCoverageBitCargoDockSupport;
	std::vector<StateAtom> atoms;
	atoms.push_back(atom(RecordType::SessionState,
		session_payload(VisibilityMode::Cockpit, 1U, generic_coverage, 1U),
		0U));
	atoms.push_back(atom(RecordType::MissionState, mission_payload(), 0U));
	append_ship_atoms(atoms, 1U, 1U);
	atoms.push_back(atom(RecordType::CargoScanState, cargo_none_payload(1U), 8U));
	const auto image = image_from_atoms(std::move(atoms));

	const BusinessClassCatalogEntry classes[] = {{1U, nullptr, 0U}};
	auto context = valid_context();
	context.class_catalog = classes;
	context.class_catalog_count = 1U;
	BusinessStateImageValidator validator(context);
	EXPECT_EQ(ValidationError::None, validator.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation, Fstl11PlayerKinematicsRequiresCockpitEvenWhenTrustedFullStateIsAuthorized)
{
	auto context = valid_context();
	context.protocol_minor = VersionMinorV1_1;
	context.required_manifest_id = 0U;
	context.trusted_full_state_authorized = true;
	context.source_endpoint_allowlisted = true;
	BusinessStateImageValidator validator(context);
	const auto trusted = make_phase1_image(
		StateDomainCoverageBitPlayerKinematics, 1U, VisibilityMode::TrustedFullState);
	EXPECT_EQ(ValidationError::VisibilityViolation, validator.validate(trusted));
}

TEST(TelemetryProtocolBusinessStateValidation, Fstl11PlayerKinematicsProfileAndDeltaInvariants)
{
	auto context = valid_context();
	context.protocol_minor = VersionMinorV1_1;
	context.required_manifest_id = 0U;
	BusinessStateImageValidator validator(context);
	const auto baseline = make_phase1_image();
	EXPECT_EQ(ValidationError::None, validator.validate(baseline));
	EXPECT_EQ(ValidationError::None, validator.validate_delta_transition(baseline, baseline));
	const auto changed_coverage = make_phase1_image(StateDomainCoverageBitPlayerKinematics |
		StateDomainCoverageBitCoreShip);
	EXPECT_EQ(ValidationError::MissingManifest,
		validator.validate_delta_transition(baseline, changed_coverage));
	const auto changed_player = make_phase1_image(StateDomainCoverageBitPlayerKinematics, 2U);
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validator.validate_delta_transition(baseline, changed_player));
}

TEST(TelemetryProtocolBusinessStateValidation, Fstl11PlayerKinematicsConvergesAfterLossDuplicationAndReorder)
{
	auto context = valid_context(); context.protocol_minor = VersionMinorV1_1; context.required_manifest_id = 0U;
	BusinessStateImageValidator validator(context);
	const auto baseline = make_phase1_image();
	ClientReplicationModel client;
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(1U, 0U, 1U, 10'000'001U));
	ProtocolRateLimiter limiter;
	ASSERT_EQ(ValidationError::None, ProtocolRateLimiter::configure({}, 0U, limiter));
	ResyncRequestPayload resync;
	const auto endpoint = EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, 42042U);
	ClientResyncChannel channel{1U, endpoint, limiter, resync};
	ASSERT_EQ(SnapshotCommitResult::Committed, client.commit_snapshot(1U, 0U, baseline, 2U, channel, &validator));
	auto flight = *std::find_if(baseline.records().begin(), baseline.records().end(), [](const StateAtom& atom) {
		return atom.key.record_type == static_cast<std::uint16_t>(RecordType::FlightState);
	});
	// Sequence 1 carries a real kinematic change (position, quaternion, velocity and rotation).
	const std::array<float, 13> changed_kinematics{{10.F, 20.F, 30.F, .5F, .5F, .5F, .5F,
		4.F, 5.F, 6.F, .1F, .2F, .3F}};
	std::memcpy(flight.value.data() + 24U, changed_kinematics.data(), sizeof(changed_kinematics));
	CumulativeStateDelta older{1U, 1U, 101U, {{StateMutationKind::Upsert, flight}}};
	// Sequence 2 is independently cumulative from the active baseline and returns the atom exactly to baseline.
	CumulativeStateDelta latest{1U, 2U, 102U, {{StateMutationKind::Upsert,
		*std::find_if(baseline.records().begin(), baseline.records().end(), [](const StateAtom& atom) {
			return atom.key.record_type == static_cast<std::uint16_t>(RecordType::FlightState);
		})}}};
	// Sequence 1 is lost; sequence 2 arrives first, then the reordered change and a duplicate return.
	ASSERT_EQ(ClientDeltaResult::Applied, client.receive_delta(latest, 2U, 1U, endpoint, limiter, resync, &validator));
	EXPECT_EQ(ClientDeltaResult::IgnoredOldSequence, client.receive_delta(older, 3U, 1U, endpoint, limiter, resync, &validator));
	EXPECT_EQ(ClientDeltaResult::IgnoredOldSequence, client.receive_delta(latest, 4U, 1U, endpoint, limiter, resync, &validator));
	StateImage expected;
	ASSERT_EQ(StateDeltaApplyResult::Applied, apply_cumulative_state_delta(baseline, latest, &validator, expected));
	EXPECT_EQ(expected, client.published());
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

TEST(TelemetryProtocolBusinessStateValidation,
	Phase3CockpitSensorsAcceptSensorOnlyIdentitiesButStillEnforceVisibility)
{
	constexpr auto coverage =
		StateDomainCoverageBitPlayerKinematics |
		StateDomainCoverageBitCoreShip |
		StateDomainCoverageBitControlInputs |
		StateDomainCoverageBitRadarSensors |
		StateDomainCoverageBitTargeting |
		StateDomainCoverageBitWeapons |
		StateDomainCoverageBitCargoDockSupport |
		StateDomainCoverageBitNavigation;
	static_assert(coverage == 0x07cbULL);

	auto atoms = make_phase2_complete_image().records();
	auto session = std::find_if(atoms.begin(), atoms.end(), [](const StateAtom& value) {
		return value.key.record_type ==
			static_cast<std::uint16_t>(RecordType::SessionState);
	});
	ASSERT_NE(atoms.end(), session);
	session->value = session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U);
	auto cargo = std::find_if(atoms.begin(), atoms.end(), [](const StateAtom& value) {
		return value.key.record_type ==
			static_cast<std::uint16_t>(RecordType::CargoScanState);
	});
	ASSERT_NE(atoms.end(), cargo);
	cargo->value = cargo_payload(1U, 2U);

	atoms.push_back(atom(RecordType::LockState, lock_payload(1U, 2U), 8U));
	atoms.push_back(atom(RecordType::TargetState, target_payload(1U, 2U), 8U));
	atoms.push_back(atom(RecordType::RadarState, radar_payload(1U), 8U));
	atoms.push_back(atom(RecordType::RadarContacts,
		radar_contact_payload(1U, 2U, ObjectType::Ship,
			ContactFlagCurrentTarget),
		16U,
		StateRecordLifecycle::ExplicitCreateDelete));
	atoms.push_back(atom(RecordType::ThreatState,
		threat_payload(1U, 3U, 3U, 3U), 8U));
	atoms.push_back(atom(RecordType::NavigationState,
		navigation_none_payload(1U), 8U));
	const auto image = image_from_atoms(std::move(atoms));

	const std::uint32_t weapon_ids[] = {3U};
	const std::uint64_t weapon_flags[] = {0U};
	auto context = phase2_complete_context();
	context.weapon_class_ids = weapon_ids;
	context.weapon_class_flags = weapon_flags;
	context.weapon_class_count = 1U;
	BusinessStateImageValidator valid(context);
	EXPECT_EQ(ValidationError::None, valid.validate(image));

	const std::uint64_t observer_only[] = {1U};
	context.enforce_cockpit_entity_allowlist = true;
	context.cockpit_entity_ids = observer_only;
	context.cockpit_entity_count = 1U;
	BusinessStateImageValidator hidden_sensor_identity(context);
	EXPECT_EQ(ValidationError::VisibilityViolation,
		hidden_sensor_identity.validate(image));
}

TEST(TelemetryProtocolBusinessStateValidation,
	Phase3RadarContactConvergesAfterLostDelta)
{
	constexpr auto coverage =
		StateDomainCoverageBitPlayerKinematics |
		StateDomainCoverageBitCoreShip |
		StateDomainCoverageBitControlInputs |
		StateDomainCoverageBitRadarSensors |
		StateDomainCoverageBitTargeting |
		StateDomainCoverageBitWeapons |
		StateDomainCoverageBitCargoDockSupport |
		StateDomainCoverageBitNavigation;
	auto atoms = make_phase2_complete_image().records();
	auto session = std::find_if(atoms.begin(), atoms.end(), [](const StateAtom& value) {
		return value.key.record_type ==
			static_cast<std::uint16_t>(RecordType::SessionState);
	});
	ASSERT_NE(atoms.end(), session);
	session->value = session_payload(VisibilityMode::Cockpit, 1U, coverage, 1U);
	auto cargo = std::find_if(atoms.begin(), atoms.end(), [](const StateAtom& value) {
		return value.key.record_type ==
			static_cast<std::uint16_t>(RecordType::CargoScanState);
	});
	ASSERT_NE(atoms.end(), cargo);
	cargo->value = cargo_payload(1U, 2U);
	atoms.push_back(atom(RecordType::LockState, lock_payload(1U, 2U), 8U));
	atoms.push_back(atom(RecordType::TargetState, target_payload(1U, 2U), 8U));
	atoms.push_back(atom(RecordType::RadarState, radar_payload(1U), 8U));
	const auto contact = atom(RecordType::RadarContacts,
		radar_contact_payload(1U, 2U, ObjectType::Ship,
			ContactFlagCurrentTarget),
		16U, StateRecordLifecycle::ExplicitCreateDelete);
	atoms.push_back(contact);
	atoms.push_back(atom(RecordType::ThreatState, threat_payload(1U), 8U));
	atoms.push_back(atom(RecordType::NavigationState,
		navigation_none_payload(1U), 8U));
	const auto baseline = image_from_atoms(std::move(atoms));

	auto context = phase2_complete_context();
	BusinessStateImageValidator validator(context);
	ASSERT_EQ(ValidationError::None, validator.validate(baseline));
	ClientReplicationModel client;
	ASSERT_EQ(ManifestInstallResult::Installed, client.install_manifest(1U));
	ASSERT_EQ(SnapshotCandidateResult::Known,
		client.note_snapshot_candidate(7U, 1U, 1U, 10'000'001U));
	ProtocolRateLimiter limiter;
	ASSERT_EQ(ValidationError::None,
		ProtocolRateLimiter::configure({}, 0U, limiter));
	ResyncRequestPayload resync;
	const auto endpoint =
		EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, 42043U);
	ClientResyncChannel channel{1U, endpoint, limiter, resync};
	ASSERT_EQ(SnapshotCommitResult::Committed,
		client.commit_snapshot(7U, 1U, baseline, 2U, channel, &validator));

	auto changed_contact = contact;
	changed_contact.value[24U] = 101U;
	const CumulativeStateDelta lost{7U, 1U, 101U,
		{{StateMutationKind::Upsert, changed_contact}}};
	auto latest_contact = contact;
	latest_contact.value[24U] = 102U;
	const CumulativeStateDelta latest{7U, 2U, 102U,
		{{StateMutationKind::Upsert, latest_contact}}};

	ASSERT_EQ(ClientDeltaResult::Applied,
		client.receive_delta(latest, 2U, 1U, endpoint, limiter, resync,
			&validator));
	EXPECT_EQ(ClientDeltaResult::IgnoredOldSequence,
		client.receive_delta(lost, 3U, 1U, endpoint, limiter, resync,
			&validator));
	StateImage expected;
	ASSERT_EQ(StateDeltaApplyResult::Applied,
		apply_cumulative_state_delta(baseline, latest, &validator, expected));
	EXPECT_EQ(expected, client.published());
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
		const std::uint32_t presence = parent_entity_id == 0U
			? 0U
			: static_cast<std::uint32_t>(EntityLifecyclePresenceFlagParent);
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
