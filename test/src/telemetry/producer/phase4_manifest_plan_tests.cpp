#include "telemetry/phase4_manifest_plan.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

using telemetry::Phase2ManifestCandidate;
using telemetry::detail::Phase4CatalogDependencies;
using telemetry::detail::Phase4ManifestPlan;
using telemetry::detail::Phase4ManifestSourceStatus;
using telemetry::detail::prepare_phase4_manifest_source_in_place;
using telemetry::detail::plan_phase4_manifest;

TEST(TelemetryPhase4ManifestPlan, RequiresManifestForMissingOrChangedDefinitions)
{
	Phase4CatalogDependencies dependencies;
	dependencies.ship_class_source_keys[0] = 4U;
	dependencies.ship_class_count = 1U;
	dependencies.weapon_source_keys[0] = 9U;
	dependencies.weapon_count = 1U;
	EXPECT_EQ(Phase4ManifestPlan::ManifestRequired,
		plan_phase4_manifest(dependencies, nullptr));

	auto manifest = std::make_unique<Phase2ManifestCandidate>();
	manifest->class_record_count = 1U;
	manifest->class_records[0].source_key = 4U;
	manifest->weapon_record_count = 1U;
	manifest->weapon_records[0].source_key = 8U;
	EXPECT_EQ(Phase4ManifestPlan::ManifestRequired,
		plan_phase4_manifest(dependencies, manifest.get()));
	manifest->weapon_records[0].source_key = 9U;
	EXPECT_EQ(Phase4ManifestPlan::ReadyForKeyframe,
		plan_phase4_manifest(dependencies, manifest.get()));
}

TEST(TelemetryPhase4ManifestPlan, RejectsUnsortedOrDuplicateDependencies)
{
	Phase4CatalogDependencies dependencies;
	dependencies.ship_class_source_keys[0] = 4U;
	dependencies.ship_class_source_keys[1] = 4U;
	dependencies.ship_class_count = 2U;
	EXPECT_EQ(Phase4ManifestPlan::InvalidDependencies,
		plan_phase4_manifest(dependencies, nullptr));
}

TEST(TelemetryPhase4ManifestPlan, BuildsExactInventoryManifestWithShipWeaponDependencies)
{
	auto available = std::make_unique<telemetry::Phase2ManifestSource>();
	ASSERT_NE(nullptr, available);
	available->ship_class_count = 1U;
	available->ship_classes[0].source_key = 4U;
	available->ship_classes[0].bank_count = 1U;
	available->ship_classes[0].banks[0].weapon_source_key = 8U;
	available->ship_classes[0].countermeasure_weapon_source_key = 9U;
	available->weapon_count = 3U;
	available->weapons[0].source_key = 8U;
	available->weapons[1].source_key = 9U;
	available->weapons[2].source_key = 12U;
	Phase4CatalogDependencies dependencies;
	dependencies.ship_class_source_keys[0] = 4U;
	dependencies.ship_class_count = 1U;
	dependencies.weapon_source_keys[0] = 12U;
	dependencies.weapon_count = 1U;
	ASSERT_EQ(Phase4ManifestSourceStatus::Created,
		prepare_phase4_manifest_source_in_place(*available, dependencies));
	EXPECT_EQ(1U, available->referenced_ship_class_count);
	EXPECT_EQ(4U, available->referenced_ship_class_keys[0]);
	ASSERT_EQ(3U, available->referenced_weapon_count);
	EXPECT_EQ(8U, available->referenced_weapon_keys[0]);
	EXPECT_EQ(9U, available->referenced_weapon_keys[1]);
	EXPECT_EQ(12U, available->referenced_weapon_keys[2]);
}

TEST(TelemetryPhase4ManifestPlan, RefusesMissingDependentWeaponWithoutChangingOutput)
{
	auto available = std::make_unique<telemetry::Phase2ManifestSource>();
	ASSERT_NE(nullptr, available);
	available->ship_class_count = 1U;
	available->ship_classes[0].source_key = 4U;
	available->ship_classes[0].bank_count = 1U;
	available->ship_classes[0].banks[0].weapon_source_key = 8U;
	available->referenced_ship_class_count = 7U;
	Phase4CatalogDependencies dependencies;
	dependencies.ship_class_source_keys[0] = 4U;
	dependencies.ship_class_count = 1U;
	EXPECT_EQ(Phase4ManifestSourceStatus::MissingDefinition,
		prepare_phase4_manifest_source_in_place(*available, dependencies));
	EXPECT_EQ(7U, available->referenced_ship_class_count);
}

} // namespace
