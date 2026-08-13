#include "telemetry/phase4_entity_projection.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

using telemetry::detail::Phase4EntityProjectionInput;
using telemetry::detail::Phase4EntityProjectionStatus;
using telemetry::detail::project_phase4_entity;
using telemetry::protocol::ObjectType;
using telemetry::protocol::RecordType;
using telemetry::protocol::StateAtom;

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	std::uint64_t value = 0U;
	for (std::size_t index = 0U; index < 8U; ++index)
		value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
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

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	return static_cast<std::uint32_t>(bytes[offset]) |
		(static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
		(static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
		(static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

Phase4EntityProjectionInput input(ObjectType type) noexcept
{
	Phase4EntityProjectionInput result;
	result.entity_id = 42U;
	result.object_type = type;
	result.sample_time_us = 99U;
	result.class_id = (type == ObjectType::Ship || type == ObjectType::Weapon) ? 7U : 0U;
	result.position_world = {{1.0F, 2.0F, 3.0F}};
	result.radius = 4.0F;
	return result;
}

TEST(TelemetryPhase4EntityProjection, EmitsLifecycleAndFlightForEveryExportableType)
{
	constexpr std::array<ObjectType, 8> types{{
		ObjectType::Ship, ObjectType::Weapon, ObjectType::Asteroid, ObjectType::Debris,
		ObjectType::JumpNode, ObjectType::Waypoint, ObjectType::Fireball, ObjectType::Other,
	}};
	for (const auto type : types) {
		StateAtom lifecycle;
		StateAtom flight;
		ASSERT_EQ(Phase4EntityProjectionStatus::Created,
			project_phase4_entity(input(type), lifecycle, flight));
		EXPECT_EQ(static_cast<std::uint16_t>(RecordType::EntityLifecycle), lifecycle.key.record_type);
		EXPECT_EQ(static_cast<std::uint16_t>(RecordType::FlightState), flight.key.record_type);
		EXPECT_EQ(42U, read_u64(lifecycle.value, 0U));
		EXPECT_EQ(static_cast<std::uint8_t>(type), lifecycle.value[24U]);
		EXPECT_EQ(42U, read_u64(flight.value, 0U));
	}
}

TEST(TelemetryPhase4EntityProjection, ClassReferencesAreRestrictedToShipsAndWeapons)
{
	for (const auto type : {ObjectType::Ship, ObjectType::Weapon}) {
		StateAtom lifecycle;
		StateAtom flight;
		ASSERT_EQ(Phase4EntityProjectionStatus::Created,
			project_phase4_entity(input(type), lifecycle, flight));
		EXPECT_NE(0U, read_u64(lifecycle.value, 8U) &
			telemetry::protocol::EntityLifecyclePresenceFlagClassReference);
		EXPECT_EQ(7U, read_u32(lifecycle.value, 30U));
	}
	for (const auto type : {ObjectType::Asteroid, ObjectType::Debris, ObjectType::JumpNode,
			 ObjectType::Waypoint, ObjectType::Fireball, ObjectType::Other}) {
		StateAtom lifecycle;
		StateAtom flight;
		ASSERT_EQ(Phase4EntityProjectionStatus::Created,
			project_phase4_entity(input(type), lifecycle, flight));
		EXPECT_EQ(0U, read_u64(lifecycle.value, 8U) &
			telemetry::protocol::EntityLifecyclePresenceFlagClassReference);
	}
}

TEST(TelemetryPhase4EntityProjection, StaticMarkersRequireIdentityPoseAndZeroVelocities)
{
	auto marker = input(ObjectType::Waypoint);
	marker.static_marker = true;
	StateAtom lifecycle;
	StateAtom flight;
	ASSERT_EQ(Phase4EntityProjectionStatus::Created,
		project_phase4_entity(marker, lifecycle, flight));
	EXPECT_FLOAT_EQ(0.0F, read_f32(flight.value, 44U));
	EXPECT_FLOAT_EQ(1.0F, read_f32(flight.value, 48U));
	EXPECT_FLOAT_EQ(0.0F, read_f32(flight.value, 52U));

	marker.velocity_world[0] = 1.0F;
	EXPECT_EQ(Phase4EntityProjectionStatus::InvalidPose,
		project_phase4_entity(marker, lifecycle, flight));
}

TEST(TelemetryPhase4EntityProjection, RejectsUnknownTypesInvalidClassesAndInvalidLifecycleFlags)
{
	StateAtom lifecycle;
	StateAtom flight;
	auto unknown = input(ObjectType::Unknown);
	EXPECT_EQ(Phase4EntityProjectionStatus::InvalidLifecycle,
		project_phase4_entity(unknown, lifecycle, flight));
	auto asteroid_with_class = input(ObjectType::Asteroid);
	asteroid_with_class.class_id = 7U;
	EXPECT_EQ(Phase4EntityProjectionStatus::InvalidLifecycle,
		project_phase4_entity(asteroid_with_class, lifecycle, flight));
	auto ship_without_class = input(ObjectType::Ship);
	ship_without_class.class_id = 0U;
	EXPECT_EQ(Phase4EntityProjectionStatus::InvalidLifecycle,
		project_phase4_entity(ship_without_class, lifecycle, flight));
	auto bomb_asteroid = input(ObjectType::Asteroid);
	bomb_asteroid.lifecycle_flags = telemetry::protocol::EntityLifecycleFlagBomb;
	EXPECT_EQ(Phase4EntityProjectionStatus::InvalidLifecycle,
		project_phase4_entity(bomb_asteroid, lifecycle, flight));
	auto unknown_phase = input(ObjectType::Ship);
	unknown_phase.lifecycle_phase =
		static_cast<telemetry::protocol::LifecyclePhase>(6U);
	EXPECT_EQ(Phase4EntityProjectionStatus::InvalidLifecycle,
		project_phase4_entity(unknown_phase, lifecycle, flight));
	auto reserved_flag = input(ObjectType::Ship);
	reserved_flag.lifecycle_flags = 0x80000000U;
	EXPECT_EQ(Phase4EntityProjectionStatus::InvalidLifecycle,
		project_phase4_entity(reserved_flag, lifecycle, flight));
}

} // namespace
