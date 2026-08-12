#include "telemetry/phase4_catalog_dependencies.h"

#include <gtest/gtest.h>

namespace {

using telemetry::detail::Phase4CatalogDependencies;
using telemetry::detail::Phase4CatalogDependencyStatus;
using telemetry::detail::Phase4EngineInventoryEntry;
using telemetry::detail::discover_phase4_catalog_dependencies;
using telemetry::protocol::ObjectType;

Phase4EngineInventoryEntry entry(std::uint64_t entity_id, ObjectType type,
	std::uint32_t source_class_key = 0U)
{
	Phase4EngineInventoryEntry result;
	result.entity_id = entity_id;
	result.identity = {entity_id + 100U, type};
	result.source_class_key = source_class_key;
	return result;
}

TEST(TelemetryPhase4CatalogDependencies, SelectsUniqueSortedClassesFromExactInventory)
{
	const std::vector<Phase4EngineInventoryEntry> inventory{
		entry(1U, ObjectType::Weapon, 9U), entry(2U, ObjectType::Ship, 3U),
		entry(3U, ObjectType::Weapon, 2U), entry(4U, ObjectType::Ship, 3U),
		entry(5U, ObjectType::Waypoint)};
	Phase4CatalogDependencies output;
	ASSERT_EQ(Phase4CatalogDependencyStatus::Collected,
		discover_phase4_catalog_dependencies(inventory, output));
	ASSERT_EQ(1U, output.ship_class_count);
	EXPECT_EQ(3U, output.ship_class_source_keys[0]);
	ASSERT_EQ(2U, output.weapon_count);
	EXPECT_EQ(2U, output.weapon_source_keys[0]);
	EXPECT_EQ(9U, output.weapon_source_keys[1]);
}

TEST(TelemetryPhase4CatalogDependencies, RejectsMissingOrForbiddenClassKeysWithoutMutation)
{
	Phase4CatalogDependencies output;
	output.ship_class_count = 7U;
	const std::vector<Phase4EngineInventoryEntry> missing_ship{
		entry(1U, ObjectType::Ship)};
	EXPECT_EQ(Phase4CatalogDependencyStatus::InvalidInventory,
		discover_phase4_catalog_dependencies(missing_ship, output));
	EXPECT_EQ(7U, output.ship_class_count);

	const std::vector<Phase4EngineInventoryEntry> forbidden_waypoint{
		entry(1U, ObjectType::Waypoint, 1U)};
	EXPECT_EQ(Phase4CatalogDependencyStatus::InvalidInventory,
		discover_phase4_catalog_dependencies(forbidden_waypoint, output));
	EXPECT_EQ(7U, output.ship_class_count);
}

} // namespace
