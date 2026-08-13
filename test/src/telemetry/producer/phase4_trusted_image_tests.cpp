#include "telemetry/phase4_trusted_image.h"
#include "telemetry/protocol/packet_writer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>

namespace {

using telemetry::detail::Phase4DockingRelation;
using telemetry::detail::Phase4EngineInventoryEntry;
using telemetry::detail::Phase4EntityProjectionInput;
using telemetry::detail::Phase4TrustedImageStatus;
using telemetry::detail::build_phase4_trusted_image;
using telemetry::detail::build_phase4_trusted_image_from_inventory;
using telemetry::detail::build_phase4_trusted_image_from_inventory_preallocated;
using telemetry::protocol::ObjectType;
using telemetry::protocol::RecordType;

telemetry::protocol::StateAtom ship_identity(std::uint64_t entity_id)
{
	telemetry::protocol::StateAtom atom;
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::ShipIdentity);
	atom.key.identity.resize(8U);
	for (std::size_t index = 0U; index < 8U; ++index)
		atom.key.identity[index] = static_cast<std::uint8_t>(entity_id >> (index * 8U));
	atom.has_cascade_owner = true;
	atom.cascade_owner.record_type = static_cast<std::uint16_t>(RecordType::EntityLifecycle);
	atom.cascade_owner.identity = atom.key.identity;
	std::array<std::uint8_t, 64U> payload{};
	telemetry::protocol::PacketWriter writer({payload.data(), payload.size()});
	if (!writer.write_u64(entity_id) || !writer.write_u64(0U) ||
		!writer.write_u64(1U) || !writer.write_u32(1U) ||
		!writer.write_utf8("ship", 255U) || !writer.write_u32(1U) ||
		!writer.write_u32(0U) || !writer.write_u32(1U) ||
		!writer.write_u16(telemetry::protocol::ShipRoleFlagMissionObject) ||
		!writer.write_f32(0.0F))
		return {};
	const auto bytes = writer.written();
	atom.value.assign(bytes.data, bytes.data + bytes.size);
	return atom;
}

Phase4EntityProjectionInput entity(std::uint64_t id, ObjectType type)
{
	Phase4EntityProjectionInput value;
	value.entity_id = id;
	value.object_type = type;
	value.class_id = (type == ObjectType::Ship || type == ObjectType::Weapon) ? 1U : 0U;
	value.sample_time_us = 50U;
	return value;
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes,
	std::size_t offset)
{
	std::uint64_t value = 0U;
	for (std::size_t index = 0U; index < 8U; ++index)
		value |= static_cast<std::uint64_t>(bytes[offset + index]) <<
			(index * 8U);
	return value;
}

float read_f32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	const auto bits = static_cast<std::uint32_t>(bytes[offset]) |
		(static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
		(static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
		(static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
	float value = 0.0F;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

TEST(TelemetryPhase4TrustedImage, CombinesEntityAndDockingRecordsInOneCandidate)
{
	const Phase4DockingRelation forward{1U, 2U, 3U, 4U, "Left", "Right"};
	const Phase4DockingRelation reverse{2U, 1U, 4U, 3U, "Right", "Left"};
	telemetry::protocol::StateImage image;
	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image({entity(1U, ObjectType::Ship), entity(2U, ObjectType::Ship),
			entity(3U, ObjectType::Weapon)}, {forward, reverse}, 60U, image));
	EXPECT_EQ(8U, image.records().size());
	std::size_t docking_count = 0U;
	for (const auto& record : image.records()) {
		if (record.key.record_type == static_cast<std::uint16_t>(RecordType::DockingState)) ++docking_count;
	}
	EXPECT_EQ(2U, docking_count);
}

TEST(TelemetryPhase4TrustedImage, LeavesOutputUntouchedWhenDockingCannotJoinTheEntityGraph)
{
	telemetry::protocol::StateImage image;
	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image({entity(1U, ObjectType::Waypoint)}, {}, 1U, image));
	const auto original_size = image.records().size();
	EXPECT_EQ(Phase4TrustedImageStatus::InvalidDocking,
		build_phase4_trusted_image({entity(1U, ObjectType::Waypoint)},
			{{1U, 2U, 0U, 0U, "", ""}}, 1U, image));
	EXPECT_EQ(original_size, image.records().size());
}

TEST(TelemetryPhase4TrustedImage, AddsInheritedShipRecordsOnlyForShipsInTheImage)
{
	telemetry::protocol::StateImage image;
	const auto inherited = ship_identity(1U);
	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image({entity(1U, ObjectType::Ship), entity(2U, ObjectType::Weapon)},
			{}, {inherited}, 1U, image));
	EXPECT_EQ(6U, image.records().size());
	EXPECT_EQ(static_cast<std::uint16_t>(RecordType::ShipIdentity), image.records()[2].key.record_type);
}

TEST(TelemetryPhase4TrustedImage, RejectsInheritedShipRecordForANonShip)
{
	telemetry::protocol::StateImage image;
	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image({entity(1U, ObjectType::Waypoint)}, {}, 1U, image));
	EXPECT_EQ(Phase4TrustedImageStatus::InvalidEntities,
		build_phase4_trusted_image({entity(1U, ObjectType::Waypoint)}, {},
			{ship_identity(1U)}, 1U, image));
	EXPECT_EQ(2U, image.records().size());
}

TEST(TelemetryPhase4TrustedImage,
	BuildsTheExactClosedInventoryWithUniversalLifecycleAndFlight)
{
	auto manifest = std::make_unique<telemetry::Phase2ManifestCandidate>();
	ASSERT_NE(nullptr, manifest);
	manifest->class_record_count = 1U;
	manifest->class_records[0].source_key = 10U;
	manifest->class_records[0].class_id = 100U;
	manifest->weapon_record_count = 1U;
	manifest->weapon_records[0].source_key = 20U;
	manifest->weapon_records[0].weapon_class_id = 200U;
	constexpr std::array<ObjectType, 8U> types{{ObjectType::Ship,
		ObjectType::Weapon, ObjectType::Asteroid, ObjectType::Debris,
		ObjectType::JumpNode, ObjectType::Waypoint, ObjectType::Fireball,
		ObjectType::Other}};
	std::vector<Phase4EngineInventoryEntry> inventory;
	inventory.reserve(types.size());
	for (std::size_t index = 0U; index < types.size(); ++index) {
		Phase4EngineInventoryEntry entry;
		entry.identity = {1000U + index, types[index]};
		entry.entity_id = index + 1U;
		entry.source_class_key = types[index] == ObjectType::Ship ? 10U :
			types[index] == ObjectType::Weapon ? 20U : 0U;
		entry.static_marker = types[index] == ObjectType::JumpNode ||
			types[index] == ObjectType::Waypoint;
		inventory.push_back(entry);
	}
	telemetry::protocol::StateImage image;
	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image_from_inventory(inventory, manifest.get(), {},
			75U, image));
	// StateImage retains the shared transaction metadata in addition to the
	// two entity-owned records for each inventory entry.
	ASSERT_EQ(types.size() * 2U + 1U, image.records().size());
	for (std::size_t index = 0U; index < types.size(); ++index) {
		const auto id = index + 1U;
		const telemetry::protocol::StateAtom* lifecycle = nullptr;
		const telemetry::protocol::StateAtom* flight = nullptr;
		for (const auto& record : image.records()) {
			if (record.key.identity.size() != 8U ||
				read_u64(record.key.identity, 0U) != id) continue;
			if (record.key.record_type == static_cast<std::uint16_t>(RecordType::EntityLifecycle)) lifecycle = &record;
			if (record.key.record_type == static_cast<std::uint16_t>(RecordType::FlightState)) flight = &record;
		}
		ASSERT_NE(nullptr, lifecycle);
		ASSERT_NE(nullptr, flight);
		EXPECT_EQ(static_cast<std::uint8_t>(types[index]), lifecycle->value[24U]);
		const auto presence = read_u64(lifecycle->value, 8U);
		const auto expected_class = types[index] == ObjectType::Ship ||
			types[index] == ObjectType::Weapon;
		EXPECT_EQ(expected_class, (presence &
			telemetry::protocol::EntityLifecyclePresenceFlagClassReference) != 0U);
		if (inventory[index].static_marker) {
			EXPECT_FLOAT_EQ(0.0F, read_f32(flight->value, 52U));
			EXPECT_FLOAT_EQ(0.0F, read_f32(flight->value, 56U));
			EXPECT_FLOAT_EQ(0.0F, read_f32(flight->value, 60U));
			EXPECT_FLOAT_EQ(0.0F, read_f32(flight->value, 64U));
			EXPECT_FLOAT_EQ(0.0F, read_f32(flight->value, 68U));
			EXPECT_FLOAT_EQ(0.0F, read_f32(flight->value, 72U));
			EXPECT_FLOAT_EQ(1.0F, read_f32(flight->value, 48U));
		}
	}
}

TEST(TelemetryPhase4TrustedImage, ResolvesInventoryOnlyThroughInstalledManifest)
{
	auto manifest = std::make_unique<telemetry::Phase2ManifestCandidate>();
	ASSERT_NE(nullptr, manifest);
	manifest->class_record_count = 1U;
	manifest->class_records[0].source_key = 7U;
	manifest->class_records[0].class_id = 42U;
	Phase4EngineInventoryEntry ship;
	ship.identity = {100U, ObjectType::Ship};
	ship.entity_id = 1U;
	ship.source_class_key = 7U;
	telemetry::protocol::StateImage image;
	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image_from_inventory({ship}, manifest.get(), {}, 75U, image));
	EXPECT_EQ(3U, image.records().size());
	EXPECT_EQ(Phase4TrustedImageStatus::InvalidEntities,
		build_phase4_trusted_image_from_inventory({ship}, nullptr, {}, 75U, image));
	EXPECT_EQ(3U, image.records().size());
}

TEST(TelemetryPhase4TrustedImage,
	PreallocatedFirstSlicePublishesOnlyUniversalRecords)
{
	auto manifest = std::make_unique<telemetry::Phase2ManifestCandidate>();
	manifest->class_record_count = 1U;
	manifest->class_records[0].source_key = 7U;
	manifest->class_records[0].class_id = 42U;
	Phase4EngineInventoryEntry ship;
	ship.identity = {100U, ObjectType::Ship};
	ship.entity_id = 1U;
	ship.source_class_key = 7U;
	std::vector<Phase4EngineInventoryEntry> inventory;
	inventory.reserve(1U);
	inventory.push_back(ship);
	std::vector<Phase4EntityProjectionInput> projections;
	projections.reserve(1U);
	telemetry::detail::Phase4StateImagePool pool;
	ASSERT_TRUE(pool.provision(1U));
	telemetry::protocol::StateImage image;

	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image_from_inventory_preallocated(
			inventory, manifest.get(), 75U, projections, pool, image));
	ASSERT_EQ(2U, image.records().size());
	EXPECT_EQ(static_cast<std::uint16_t>(RecordType::EntityLifecycle),
		image.records()[0].key.record_type);
	EXPECT_EQ(static_cast<std::uint16_t>(RecordType::FlightState),
		image.records()[1].key.record_type);
}

} // namespace
