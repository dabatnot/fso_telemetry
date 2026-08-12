#include "telemetry/phase4_runtime_storage.h"

#include <gtest/gtest.h>

namespace {

using telemetry::detail::Phase4EntityRegistryStatus;
using telemetry::detail::Phase4MaximumDockingRelations;
using telemetry::detail::Phase4MaximumLiveEntities;
using telemetry::detail::Phase4RuntimeStorage;
using telemetry::protocol::ObjectType;

TEST(TelemetryPhase4RuntimeStorage,
	ProvisionsBoundedSharedAndPerClientStorageBeforeCapture)
{
	Phase4RuntimeStorage storage;
	ASSERT_TRUE(storage.provision(2U));
	ASSERT_TRUE(storage.ready());
	EXPECT_EQ(2U, storage.client_count());
	EXPECT_GT(storage.owned_backing_bytes(), 0U);
	EXPECT_TRUE(storage.identities().ready());
	EXPECT_EQ(Phase4MaximumLiveEntities, storage.inventory().capacity());
	EXPECT_EQ(Phase4MaximumLiveEntities, storage.projections().capacity());
	EXPECT_EQ(Phase4MaximumDockingRelations,
		storage.docking_relations().capacity());
	ASSERT_NE(nullptr, storage.image_pool(0U));
	ASSERT_NE(nullptr, storage.image_pool(1U));
	EXPECT_EQ(nullptr, storage.image_pool(2U));
	EXPECT_EQ(Phase4MaximumLiveEntities,
		storage.image_pool(0U)->maximum_entities());
}

TEST(TelemetryPhase4RuntimeStorage,
	MissionResetRetainsCapacityAndNeverRestartsEntityIds)
{
	Phase4RuntimeStorage storage;
	ASSERT_TRUE(storage.provision(1U));
	const auto first = storage.identities().resolve({11U, ObjectType::Ship});
	ASSERT_EQ(Phase4EntityRegistryStatus::Allocated, first.status);
	storage.inventory().push_back({});
	storage.projections().push_back({});
	const auto inventory_capacity = storage.inventory().capacity();
	const auto projection_capacity = storage.projections().capacity();

	storage.reset_mission();
	EXPECT_TRUE(storage.inventory().empty());
	EXPECT_TRUE(storage.projections().empty());
	EXPECT_EQ(inventory_capacity, storage.inventory().capacity());
	EXPECT_EQ(projection_capacity, storage.projections().capacity());
	const auto second = storage.identities().resolve({12U, ObjectType::Ship});
	ASSERT_EQ(Phase4EntityRegistryStatus::Allocated, second.status);
	EXPECT_GT(second.entity_id, first.entity_id);
}

TEST(TelemetryPhase4RuntimeStorage, RejectsInvalidClientBoundsWithoutReadiness)
{
	Phase4RuntimeStorage storage;
	EXPECT_FALSE(storage.provision(0U));
	EXPECT_FALSE(storage.ready());
	EXPECT_FALSE(storage.provision(5U));
	EXPECT_FALSE(storage.ready());
}

} // namespace
