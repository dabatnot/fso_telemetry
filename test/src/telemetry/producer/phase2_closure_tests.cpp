#include <gtest/gtest.h>

#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/phase2_closure.h"
#include "telemetry/phase2_wp03_allocation_tracker.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using namespace telemetry;

enum class InvalidEdgeCase { SupportSignature, NonReciprocalDock, LeaderOutsideComponent };

Phase2ClosureNode ship(std::uint64_t source_key, std::uint32_t class_key)
{
	Phase2ClosureNode node{};
	node.source_key = source_key;
	node.instance_signature = static_cast<std::uint32_t>(source_key);
	node.ship_class_key = class_key;
	node.valid = true;
	return node;
}

std::unique_ptr<Phase2ClosureInput> invalid_extension(InvalidEdgeCase edge)
{
	auto input = std::make_unique<Phase2ClosureInput>();
	input->player_source_key = 10;
	input->nodes[0] = ship(10, 100);
	input->nodes[1] = ship(20, 200);
	input->node_count = 2;
	switch (edge) {
	case InvalidEdgeCase::SupportSignature:
		input->nodes[0].support_source_key = 20;
		input->nodes[0].support_signature_valid = false;
		break;
	case InvalidEdgeCase::NonReciprocalDock:
		input->nodes[0].dock_source_keys[0] = 20;
		input->nodes[0].dock_source_count = 1;
		break;
	case InvalidEdgeCase::LeaderOutsideComponent:
		input->nodes[0].group_leader_source_key = 20;
		input->nodes[0].group_leader_component_valid = false;
		break;
	}
	return input;
}

std::unique_ptr<Phase2ClosureInput> fingerprint_input()
{
	auto input = std::make_unique<Phase2ClosureInput>();
	input->player_source_key = 1;
	input->nodes[0] = ship(1, 101);
	input->nodes[1] = ship(2, 102);
	input->node_count = 2;
	input->nodes[0].support_source_key = 2;
	input->nodes[0].support_signature_valid = true;
	input->nodes[0].effective_mass = 10.0F;
	input->nodes[1].effective_mass = 20.0F;
	return input;
}

bool contains_ship(const Phase2Closure& closure, std::uint64_t source_key)
{
	return std::find(closure.ship_entities.begin(),
		       closure.ship_entities.begin() + closure.ship_entity_count,
		       source_key) != closure.ship_entities.begin() + closure.ship_entity_count;
}

TEST(Phase2Closure, P2TST029CoreGateIgnoresInvalidExtensionWhileCompleteShipFailsClosed)
{
	for (const auto edge : {InvalidEdgeCase::SupportSignature,
		     InvalidEdgeCase::NonReciprocalDock,
		     InvalidEdgeCase::LeaderOutsideComponent}) {
		auto input = invalid_extension(edge);
		auto core = std::make_unique<Phase2Closure>();
		ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(*input, Phase2Profile::CoreGate, *core))
			<< static_cast<unsigned>(edge);
		ASSERT_EQ(1u, core->ship_entity_count);
		EXPECT_TRUE(contains_ship(*core, input->player_source_key));

		auto complete = std::make_unique<Phase2Closure>();
		EXPECT_EQ(Phase2ClosureError::InvalidAuthorizedEdge,
			build_phase2_closure(*input, Phase2Profile::CompleteShip, *complete))
			<< static_cast<unsigned>(edge);
		EXPECT_EQ(0u, complete->ship_entity_count);
	}
}

TEST(Phase2Closure, P2REQ019CompleteShipComputesTheLeastTransitiveFixedPointForEveryMember)
{
	auto input_storage = std::make_unique<Phase2ClosureInput>();
	auto& input = *input_storage;
	input.player_source_key = 1;
	input.nodes[0] = ship(1, 101);
	input.nodes[1] = ship(2, 102);
	input.nodes[2] = ship(3, 103);
	input.nodes[3] = ship(4, 104);
	input.node_count = 4;

	input.nodes[0].support_source_key = 2;
	input.nodes[0].support_signature_valid = true;
	input.nodes[1].dock_source_keys[0] = 3;
	input.nodes[1].dock_source_count = 1;
	input.nodes[2].dock_source_keys[0] = 2;
	input.nodes[2].dock_source_count = 1;
	input.nodes[2].group_leader_source_key = 4;
	input.nodes[2].group_leader_component_valid = true;

	auto closure = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	ASSERT_EQ(4u, closure->ship_entity_count);
	for (std::uint64_t key = 1; key <= 4; ++key) {
		EXPECT_TRUE(contains_ship(*closure, key));
	}
}

TEST(Phase2Closure, P2TST029ForbiddenRootsAndCargoNeverEnlargeTheFixedPoint)
{
	auto input_storage = std::make_unique<Phase2ClosureInput>();
	auto& input = *input_storage;
	input.player_source_key = 1;
	input.nodes[0] = ship(1, 101);
	input.nodes[1] = ship(2, 102);
	input.node_count = 2;
	input.nodes[0].team_source_key = 2;
	input.nodes[0].proximity_source_key = 2;
	input.nodes[0].target_source_key = 2;
	input.nodes[0].sensor_source_key = 2;
	input.nodes[0].parent_source_key = 2;
	input.nodes[0].event_source_key = 2;
	input.nodes[0].cargo_target_source_key = 2;

	auto closure = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::UnauthorizedReference,
		build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	EXPECT_EQ(0u, closure->ship_entity_count);

	input.nodes[0].cargo_target_source_key = 0;
	ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	EXPECT_EQ(1u, closure->ship_entity_count);
	EXPECT_FALSE(contains_ship(*closure, 2));
}

TEST(Phase2Closure, P2REQ019CargoReferenceIsRetainedOnlyAfterAnotherAllowlistedEdgeAddsTheTarget)
{
	auto input_storage = std::make_unique<Phase2ClosureInput>();
	auto& input = *input_storage;
	input.player_source_key = 1;
	input.nodes[0] = ship(1, 101);
	input.nodes[1] = ship(2, 102);
	input.node_count = 2;
	input.nodes[0].support_source_key = 2;
	input.nodes[0].support_signature_valid = true;
	input.nodes[0].cargo_target_source_key = 2;

	auto closure = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	ASSERT_EQ(2u, closure->ship_entity_count);
	EXPECT_TRUE(closure->references_cargo_target(1, 2));
}

TEST(Phase2Closure, P2REQ031RejectsTheReal1025PerShipAnd4097AggregateCasesAtomically)
{
	auto input_storage = std::make_unique<Phase2ClosureInput>();
	auto& input = *input_storage;
	input.player_source_key = 1;
	input.nodes[0] = ship(1, 101);
	input.node_count = 1;
	input.nodes[0].subsystem_key_count = 1025;
	for (std::uint32_t i = 0; i < Phase2ClosureLimits::MaxSubsystemsPerShip; ++i) {
		input.nodes[0].subsystem_keys[i] = i + 1;
	}

	auto closure = std::make_unique<Phase2Closure>();
	closure->ship_entity_count = 7; // caller-owned sentinel must survive refusal
	closure->subsystem_count = 11;
	EXPECT_EQ(Phase2ClosureError::TooManySubsystemsPerShip,
		build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	EXPECT_EQ(7u, closure->ship_entity_count);
	EXPECT_EQ(11u, closure->subsystem_count);

	input = {};
	input.player_source_key = 1;
	input.node_count = 5;
	for (std::uint32_t ship_index = 0; ship_index < 5; ++ship_index) {
		input.nodes[ship_index] = ship(ship_index + 1, ship_index + 100);
		input.nodes[ship_index].subsystem_key_count = ship_index < 4 ? 1024 : 1;
		for (std::uint32_t subsystem = 0; subsystem < input.nodes[ship_index].subsystem_key_count; ++subsystem) {
			input.nodes[ship_index].subsystem_keys[subsystem] = ship_index * 1024 + subsystem + 1;
		}
		if (ship_index + 1 < 5) {
			input.nodes[ship_index].support_source_key = ship_index + 2;
			input.nodes[ship_index].support_signature_valid = true;
		}
	}
	EXPECT_EQ(Phase2ClosureError::TooManyAggregateSubsystems,
		build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	EXPECT_EQ(7u, closure->ship_entity_count);
	EXPECT_EQ(11u, closure->subsystem_count);
}

TEST(Phase2Closure, P2REQ031AcceptsExactly64ShipsAnd4096AggregateSubsystems)
{
	auto input_storage = std::make_unique<Phase2ClosureInput>();
	auto& input = *input_storage;
	input.player_source_key = 1;
	input.node_count = 64;
	for (std::uint32_t ship_index = 0; ship_index < 64; ++ship_index) {
		input.nodes[ship_index] = ship(ship_index + 1, ship_index + 100);
		input.nodes[ship_index].subsystem_key_count = 64;
		for (std::uint32_t subsystem = 0; subsystem < 64; ++subsystem) {
			input.nodes[ship_index].subsystem_keys[subsystem] = ship_index * 64 + subsystem + 1;
		}
		if (ship_index + 1 < 64) {
			input.nodes[ship_index].support_source_key = ship_index + 2;
			input.nodes[ship_index].support_signature_valid = true;
		}
	}
	auto closure = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	EXPECT_EQ(64u, closure->ship_entity_count);
	EXPECT_EQ(4096u, closure->subsystem_count);
}

TEST(Phase2Closure, P2REQ031RejectsK65Atomically)
{
	auto input_storage = std::make_unique<Phase2ClosureInput>();
	auto& input = *input_storage;
	input.player_source_key = 1;
	input.node_count = 65;
	for (std::uint32_t i = 0; i < Phase2ClosureLimits::MaxShips; ++i) {
		input.nodes[i] = ship(i + 1, i + 100);
		if (i + 1 < Phase2ClosureLimits::MaxShips) {
			input.nodes[i].support_source_key = i + 2;
			input.nodes[i].support_signature_valid = true;
		}
	}
	auto closure = std::make_unique<Phase2Closure>();
	closure->ship_entity_count = 9;
	EXPECT_EQ(Phase2ClosureError::TooManyShips,
		build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	EXPECT_EQ(9u, closure->ship_entity_count);
}

TEST(Phase2Closure, D2004CanonicalPreIdClosureAndFingerprintsAreOrderIndependent)
{
	auto ordered_storage = std::make_unique<Phase2ClosureInput>();
	auto& ordered = *ordered_storage;
	ordered.player_source_key = 1;
	ordered.nodes[0] = ship(1, 101);
	ordered.nodes[1] = ship(2, 102);
	ordered.node_count = 2;
	ordered.nodes[0].support_source_key = 2;
	ordered.nodes[0].support_signature_valid = true;
	auto reversed = std::make_unique<Phase2ClosureInput>(ordered);
	std::reverse(reversed->nodes.begin(), reversed->nodes.begin() + reversed->node_count);

	auto first = std::make_unique<Phase2Closure>();
	auto second = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(ordered, Phase2Profile::CompleteShip, *first));
	ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(*reversed, Phase2Profile::CompleteShip, *second));
	EXPECT_EQ(first->topology_fingerprint, second->topology_fingerprint);
	EXPECT_EQ(first->catalog_fingerprint, second->catalog_fingerprint);
	EXPECT_EQ(first->ship_entities, second->ship_entities);
	EXPECT_EQ(first->class_descriptors, second->class_descriptors);

}

TEST(Phase2Closure, FingerprintsRespectSemanticScopesCanonicalSortingAndSha256Properties)
{
	auto input = fingerprint_input();
	auto core = std::make_unique<Phase2Closure>();
	auto complete = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(*input, Phase2Profile::CoreGate, *core));
	ASSERT_EQ(Phase2ClosureError::None, build_phase2_closure(*input, Phase2Profile::CompleteShip, *complete));

	EXPECT_NE(core->topology_fingerprint, complete->topology_fingerprint);
	EXPECT_NE(core->catalog_fingerprint, complete->catalog_fingerprint);

	auto reordered = std::make_unique<Phase2ClosureInput>(*input);
	std::reverse(reordered->nodes.begin(), reordered->nodes.begin() + reordered->node_count);
	auto reordered_complete = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*reordered, Phase2Profile::CompleteShip, *reordered_complete));
	EXPECT_EQ(complete->topology_fingerprint, reordered_complete->topology_fingerprint);
	EXPECT_EQ(complete->catalog_fingerprint, reordered_complete->catalog_fingerprint);

	auto topology_change = std::make_unique<Phase2ClosureInput>(*input);
	topology_change->nodes[1].instance_signature += 1000;
	auto changed = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*topology_change, Phase2Profile::CompleteShip, *changed));
	EXPECT_NE(complete->topology_fingerprint, changed->topology_fingerprint);
	EXPECT_EQ(complete->catalog_fingerprint, changed->catalog_fingerprint);

	auto raw_key_change = std::make_unique<Phase2ClosureInput>(*input);
	raw_key_change->nodes[1].source_key += 1000;
	raw_key_change->nodes[0].support_source_key = raw_key_change->nodes[1].source_key;
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*raw_key_change, Phase2Profile::CompleteShip, *changed));
	EXPECT_EQ(complete->topology_fingerprint, changed->topology_fingerprint);
	EXPECT_EQ(complete->catalog_fingerprint, changed->catalog_fingerprint);

	auto catalog_change = std::make_unique<Phase2ClosureInput>(*input);
	catalog_change->nodes[1].effective_mass += 1.0F;
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*catalog_change, Phase2Profile::CompleteShip, *changed));
	EXPECT_EQ(complete->topology_fingerprint, changed->topology_fingerprint);
	EXPECT_NE(complete->catalog_fingerprint, changed->catalog_fingerprint);

	auto excluded_change = std::make_unique<Phase2ClosureInput>(*input);
	excluded_change->manifest_generation = 99;
	excluded_change->nodes[1].engine_index += 77;
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*excluded_change, Phase2Profile::CompleteShip, *changed));
	EXPECT_EQ(complete->topology_fingerprint, changed->topology_fingerprint);
	EXPECT_EQ(complete->catalog_fingerprint, changed->catalog_fingerprint);
}

TEST(Phase2Closure, P2REQ019TopologyFingerprintCoversAuthorizedEdgeKindsNotOnlyMemberSignatures)
{
	auto support = std::make_unique<Phase2ClosureInput>();
	support->player_source_key = 1;
	support->nodes[0] = ship(1, 101);
	support->nodes[1] = ship(2, 102);
	support->node_count = 2;
	support->nodes[0].support_source_key = 2;
	support->nodes[0].support_signature_valid = true;

	auto docking = std::make_unique<Phase2ClosureInput>(*support);
	docking->nodes[0].support_source_key = 0;
	docking->nodes[0].support_signature_valid = false;
	docking->nodes[0].dock_source_keys[0] = 2;
	docking->nodes[0].dock_source_count = 1;
	docking->nodes[1].dock_source_keys[0] = 1;
	docking->nodes[1].dock_source_count = 1;

	auto support_closure = std::make_unique<Phase2Closure>();
	auto docking_closure = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*support, Phase2Profile::CompleteShip, *support_closure));
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*docking, Phase2Profile::CompleteShip, *docking_closure));
	ASSERT_EQ(support_closure->ship_entities, docking_closure->ship_entities);
	EXPECT_NE(support_closure->topology_fingerprint, docking_closure->topology_fingerprint)
		<< "P2-REQ-019 requires the topology fingerprint to cover support, leader, and docking relations.";
}

TEST(Phase2Closure, D2006CatalogFingerprintCoversClassAndSubsystemDescriptorsNotOnlyMass)
{
	auto baseline = fingerprint_input();
	baseline->nodes[0].subsystem_keys[0] = 501;
	baseline->nodes[0].subsystem_key_count = 1;
	auto original = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*baseline, Phase2Profile::CompleteShip, *original));

	auto class_change = std::make_unique<Phase2ClosureInput>(*baseline);
	class_change->nodes[1].ship_class_key += 1;
	auto changed = std::make_unique<Phase2Closure>();
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*class_change, Phase2Profile::CompleteShip, *changed));
	EXPECT_NE(original->catalog_fingerprint, changed->catalog_fingerprint)
		<< "P2-REQ-018 requires every canonical pre-ID class descriptor change to rebuild the catalog.";

	auto subsystem_change = std::make_unique<Phase2ClosureInput>(*baseline);
	subsystem_change->nodes[0].subsystem_keys[0] += 1;
	ASSERT_EQ(Phase2ClosureError::None,
		build_phase2_closure(*subsystem_change, Phase2Profile::CompleteShip, *changed));
	EXPECT_NE(original->catalog_fingerprint, changed->catalog_fingerprint)
		<< "P2-REQ-015/017 require subsystem definitions to participate in the catalog fingerprint.";
}

TEST(Phase2Closure, D2020InvalidSourceNeverLeaksOrPartiallyPublishes)
{
	auto input_storage = std::make_unique<Phase2ClosureInput>();
	auto& input = *input_storage;
	input.player_source_key = 1;
	input.nodes[0] = ship(1, 101);
	input.node_count = 1;
	input.nodes[0].support_source_key = UINT64_MAX;
	auto closure = std::make_unique<Phase2Closure>();
	closure->ship_entity_count = 9;
	EXPECT_EQ(Phase2ClosureError::InvalidSource,
		build_phase2_closure(input, Phase2Profile::CompleteShip, *closure));
	EXPECT_EQ(9u, closure->ship_entity_count);
}

TEST(Phase2Closure, BuildIsNoexceptAndUsesCallerOwnedBoundedStorage)
{
	static_assert(Phase2ClosureLimits::MaxShips == 64);
	static_assert(Phase2ClosureLimits::MaxSubsystemsPerShip == 1024);
	static_assert(Phase2ClosureLimits::MaxAggregateSubsystems == 4096);
	static_assert(noexcept(build_phase2_closure(
		std::declval<const Phase2ClosureInput&>(), Phase2Profile::CoreGate, std::declval<Phase2Closure&>())));
	auto input_storage = std::make_unique<Phase2ClosureInput>();
	auto& input = *input_storage;
	input.player_source_key = 1;
	input.nodes[0] = ship(1, 1);
	input.node_count = 1;
	auto output = std::make_unique<Phase2Closure>();
	test::wp03::GlobalAllocationScope allocation_scope;
	const auto result = build_phase2_closure(input, Phase2Profile::CoreGate, *output);
	const auto allocations = allocation_scope.finish();
	EXPECT_EQ(Phase2ClosureError::None, result);
	EXPECT_EQ(0u, allocations);
}

TEST(Phase2BuildContract, P2REQ049FutureProductionFilesMustBeExplicitWithoutGlob)
{
	std::ifstream input(std::string(FSO_PHASE2_SOURCE_ROOT) + "/code/source_groups.cmake", std::ios::binary);
	ASSERT_TRUE(input.is_open());
	std::ostringstream buffer;
	buffer << input.rdbuf();
	const auto cmake = buffer.str();
	for (const auto* required : {"telemetry/phase2_closure.cpp",
		     "telemetry/phase2_closure.h",
		     "telemetry/phase2_manifest_builder.cpp",
		     "telemetry/phase2_manifest_builder.h"}) {
		EXPECT_NE(std::string::npos, cmake.find(required)) << required;
	}
	EXPECT_EQ(std::string::npos, cmake.find("telemetry/phase2_*.cpp"));
}

} // namespace
