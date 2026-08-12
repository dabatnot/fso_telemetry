#include "telemetry/phase4_catalog_bindings.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

using telemetry::Phase2ManifestCandidate;
using telemetry::detail::Phase4CatalogBindingStatus;
using telemetry::detail::Phase4EngineInventoryEntry;
using telemetry::detail::Phase4EntityProjectionInput;
using telemetry::detail::bind_phase4_catalogs;
using telemetry::protocol::ObjectType;

Phase4EngineInventoryEntry entry(std::uint64_t entity_id, ObjectType type,
	std::uint32_t source_class_key = 0U)
{
	Phase4EngineInventoryEntry result;
	result.identity = {100U + entity_id, type};
	result.entity_id = entity_id;
	result.source_class_key = source_class_key;
	result.orientation_local_to_world = {{0.0F, 0.0F, 0.0F, 1.0F}};
	return result;
}

TEST(TelemetryPhase4CatalogBindings, ResolvesOnlyInstalledPublicClasses)
{
	auto manifest = std::make_unique<Phase2ManifestCandidate>();
	manifest->class_record_count = 1U;
	manifest->class_records[0].source_key = 11U;
	manifest->class_records[0].class_id = 101U;
	manifest->weapon_record_count = 1U;
	manifest->weapon_records[0].source_key = 12U;
	manifest->weapon_records[0].weapon_class_id = 102U;
	std::vector<Phase4EngineInventoryEntry> inventory{
		entry(1U, ObjectType::Ship, 11U), entry(2U, ObjectType::Weapon, 12U),
		entry(3U, ObjectType::Waypoint)};
	std::vector<Phase4EntityProjectionInput> output;
	output.reserve(inventory.size());

	ASSERT_EQ(Phase4CatalogBindingStatus::Created,
		bind_phase4_catalogs(inventory, manifest.get(), 99U, output));
	ASSERT_EQ(3U, output.size());
	EXPECT_EQ(101U, output[0].class_id);
	EXPECT_EQ(102U, output[1].class_id);
	EXPECT_EQ(0U, output[2].class_id);
	EXPECT_EQ(99U, output[2].sample_time_us);
}

TEST(TelemetryPhase4CatalogBindings, RejectsMissingOrAmbiguousPublicDefinitionsAtomically)
{
	auto manifest = std::make_unique<Phase2ManifestCandidate>();
	std::vector<Phase4EngineInventoryEntry> inventory{entry(1U, ObjectType::Ship, 11U)};
	std::vector<Phase4EntityProjectionInput> output;
	output.reserve(1U);
	output.push_back({});
	EXPECT_EQ(Phase4CatalogBindingStatus::MissingClass,
		bind_phase4_catalogs(inventory, manifest.get(), 1U, output));
	EXPECT_EQ(1U, output.size());

	manifest->class_record_count = 2U;
	manifest->class_records[0].source_key = 11U;
	manifest->class_records[1].source_key = 11U;
	EXPECT_EQ(Phase4CatalogBindingStatus::DuplicateDefinition,
		bind_phase4_catalogs(inventory, manifest.get(), 1U, output));
	EXPECT_EQ(1U, output.size());
}

} // namespace
