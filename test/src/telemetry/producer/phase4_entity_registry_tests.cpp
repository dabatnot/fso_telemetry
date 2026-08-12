#include "telemetry/phase4_entity_registry.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <type_traits>

namespace {

using telemetry::detail::Phase4EntityIdentityKey;
using telemetry::detail::Phase4EntityRegistry;
using telemetry::detail::Phase4EntityRegistryStatus;
using telemetry::protocol::ObjectType;

static_assert(Phase4EntityRegistry::Capacity == 65'536U);
static_assert(std::is_final_v<Phase4EntityRegistry>);
static_assert(std::is_nothrow_default_constructible_v<Phase4EntityRegistry>);

Phase4EntityIdentityKey key(std::uint64_t identity, ObjectType type) noexcept
{
	return {identity, type};
}

TEST(TelemetryPhase4EntityRegistry, RejectsUnknownAndNonExportableKeysBeforeMutation)
{
	Phase4EntityRegistry registry;
	EXPECT_EQ(Phase4EntityRegistryStatus::InvalidKey,
		registry.resolve(key(0U, ObjectType::Ship)).status);
	EXPECT_EQ(Phase4EntityRegistryStatus::InvalidKey,
		registry.resolve(key(1U, ObjectType::Unknown)).status);
	EXPECT_EQ(Phase4EntityRegistryStatus::NotReady,
		registry.resolve(key(1U, ObjectType::Ship)).status);
	EXPECT_EQ(0U, registry.identity_count());
	EXPECT_EQ(0U, registry.active_count());
}

TEST(TelemetryPhase4EntityRegistry, AllocatesEveryClosedExportableTypeWithStableMonotoneIds)
{
	Phase4EntityRegistry registry;
	ASSERT_EQ(Phase4EntityRegistryStatus::Allocated, registry.provision());
	EXPECT_EQ(Phase4EntityRegistryStatus::Existing, registry.provision());

	constexpr std::array<ObjectType, 8> types{{
		ObjectType::Ship, ObjectType::Weapon, ObjectType::Asteroid, ObjectType::Debris,
		ObjectType::JumpNode, ObjectType::Waypoint, ObjectType::Fireball, ObjectType::Other,
	}};
	for (std::size_t index = 0U; index < types.size(); ++index) {
		const auto resolved = registry.resolve(key(100U + index, types[index]));
		ASSERT_EQ(Phase4EntityRegistryStatus::Allocated, resolved.status);
		EXPECT_EQ(index + 1U, resolved.entity_id);
		const auto stable = registry.resolve(key(100U + index, types[index]));
		EXPECT_EQ(Phase4EntityRegistryStatus::Existing, stable.status);
		EXPECT_EQ(resolved.entity_id, stable.entity_id);
	}
	EXPECT_EQ(types.size(), registry.identity_count());
	EXPECT_EQ(types.size(), registry.active_count());
	EXPECT_EQ(types.size(), registry.last_allocated_entity_id());
}

TEST(TelemetryPhase4EntityRegistry, RetiredEntityIdsRemainTombstonedAndNeverReused)
{
	Phase4EntityRegistry registry;
	ASSERT_EQ(Phase4EntityRegistryStatus::Allocated, registry.provision());
	const auto ship = registry.resolve(key(7U, ObjectType::Ship));
	ASSERT_EQ(Phase4EntityRegistryStatus::Allocated, ship.status);
	EXPECT_EQ(Phase4EntityRegistryStatus::Retired, registry.retire(key(7U, ObjectType::Ship)));
	EXPECT_EQ(0U, registry.active_count());
	const auto retired = registry.resolve(key(7U, ObjectType::Ship));
	EXPECT_EQ(Phase4EntityRegistryStatus::AlreadyRetired, retired.status);
	EXPECT_EQ(ship.entity_id, retired.entity_id);
	EXPECT_EQ(Phase4EntityRegistryStatus::AlreadyRetired,
		registry.retire(key(7U, ObjectType::Ship)));

	const auto replacement = registry.resolve(key(8U, ObjectType::Ship));
	EXPECT_EQ(Phase4EntityRegistryStatus::Allocated, replacement.status);
	EXPECT_EQ(ship.entity_id + 1U, replacement.entity_id);
	EXPECT_EQ(2U, registry.identity_count());
	EXPECT_EQ(1U, registry.active_count());
}

TEST(TelemetryPhase4EntityRegistry, ResetIsTheOnlyOperationThatRestartsTheSessionCounter)
{
	Phase4EntityRegistry registry;
	ASSERT_EQ(Phase4EntityRegistryStatus::Allocated, registry.provision());
	ASSERT_EQ(1U, registry.resolve(key(3U, ObjectType::Weapon)).entity_id);
	registry.reset_session();
	EXPECT_EQ(0U, registry.identity_count());
	EXPECT_EQ(0U, registry.last_allocated_entity_id());
	EXPECT_EQ(1U, registry.resolve(key(4U, ObjectType::Weapon)).entity_id);
}

} // namespace
