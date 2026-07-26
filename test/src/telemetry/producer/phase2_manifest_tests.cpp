#include <gtest/gtest.h>

#include "telemetry/entity_id_registry.h"
#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase2_wp03_allocation_tracker.h"
#include "telemetry/phase2_wp03_test_support.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <string>
#include <vector>

namespace {

using namespace telemetry;
using namespace telemetry::protocol;

constexpr std::size_t MaxClassManifestTransactionBytes =
	64u * (65535u + RecordEnvelopeHeaderSize);
// Phase 2 always omits the four-byte WEAPON_MANIFEST countermeasure group.
constexpr std::size_t MaxWeaponManifestRecordBytes = 660;
constexpr std::size_t FullRequiredTheoreticalUpperBound =
	MaxClassManifestTransactionBytes + 4096u * MaxWeaponManifestRecordBytes;
// Test-side lower bound from fields explicitly populated by NearMaxTransaction:
// 4096 subsystem items (805-byte payload + 3-byte item wrapper), eight banks
// per class with 64 firepoints, and 4096 maximal optional WeaponManifestV1s.
constexpr std::size_t NearMaxSubsystemBytes = 4096u * 808u;
constexpr std::size_t NearMaxBankBytes = 64u * (4u * 790u + 4u * 794u);
constexpr std::size_t NearMaxWeaponBytes = 4096u * MaxWeaponManifestRecordBytes;
constexpr std::size_t NearMaxLegalLowerBound =
	NearMaxSubsystemBytes + NearMaxBankBytes + NearMaxWeaponBytes;
static_assert(FullRequiredTheoreticalUpperBound == 6'897'984u);
static_assert(NearMaxLegalLowerBound == 6'418'432u);
static_assert(FullRequiredTheoreticalUpperBound < 14u * 1024u * 1024u,
	"A legal Phase 2 FullRequired catalog cannot occupy fourteen MiB.");

class CountingFailResource final : public std::pmr::memory_resource {
  public:
	std::size_t allocations = 0;
	bool fail = false;

  private:
	void* do_allocate(std::size_t bytes, std::size_t alignment) override
	{
		++allocations;
		if (fail) {
			throw std::bad_alloc();
		}
		return std::pmr::new_delete_resource()->allocate(bytes, alignment);
	}
	void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
	{
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
	}
	bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

struct ProvisionedSlot {
	struct State {
		CountingFailResource fallback;
		std::vector<std::uint8_t> active_arena;
		std::vector<std::uint8_t> staged_arena;
		Phase2ManifestStorage storage;
		Phase2ManifestSlot slot;

		State()
		: active_arena(MaxTransactionSize),
		  staged_arena(MaxTransactionSize),
		  storage(MutableByteView{active_arena.data(), active_arena.size()},
			  MutableByteView{staged_arena.data(), staged_arena.size()},
			  &fallback),
		  slot(storage)
		{
		}
	};

	std::unique_ptr<State> state;
	CountingFailResource& fallback;
	Phase2ManifestSlot& slot;

	ProvisionedSlot() : state(std::make_unique<State>()), fallback(state->fallback), slot(state->slot) {}
};

std::unique_ptr<Phase2ManifestSource> minimal_source()
{
	auto source = std::make_unique<Phase2ManifestSource>();
	source->ship_classes[0].source_key = 41;
	source->ship_classes[0].name = "Ulysses";
	source->ship_classes[0].model_mass = 100.0F;
	source->ship_classes[0].density_provenance = 1.0F;
	source->ship_classes[0].effective_mass = 100.0F;
	source->ship_class_count = 1;
	source->referenced_ship_class_keys[0] = 41;
	source->referenced_ship_class_count = 1;
	return source;
}

std::vector<std::uint32_t> static_ids(const Phase2ManifestCandidate& candidate)
{
	std::vector<std::uint32_t> result;
	for (std::uint32_t i = 0; i < candidate.class_record_count; ++i) {
		const auto& record = candidate.class_records[i];
		result.push_back(record.class_id);
		for (std::uint32_t subsystem = 0; subsystem < record.subsystem_count; ++subsystem) {
			result.push_back(record.subsystems[subsystem].subsystem_id);
		}
		for (std::uint32_t bank = 0; bank < record.bank_count; ++bank) {
			result.push_back(record.banks[bank].bank_id);
		}
	}
	for (std::uint32_t i = 0; i < candidate.weapon_record_count; ++i) {
		result.push_back(candidate.weapon_records[i].weapon_class_id);
	}
	return result;
}

bool nonzero_unique(std::vector<std::uint32_t> ids)
{
	if (std::find(ids.begin(), ids.end(), 0) != ids.end()) return false;
	std::sort(ids.begin(), ids.end());
	return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

bool ids_are_bijective(const Phase2ManifestCandidate& candidate)
{
	std::vector<std::uint32_t> classes;
	std::vector<std::uint32_t> weapons;
	for (std::uint32_t i = 0; i < candidate.class_record_count; ++i) {
		classes.push_back(candidate.class_records[i].class_id);
		std::vector<std::uint32_t> subsystems;
		std::vector<std::uint32_t> banks;
		for (std::uint32_t j = 0; j < candidate.class_records[i].subsystem_count; ++j) {
			subsystems.push_back(candidate.class_records[i].subsystems[j].subsystem_id);
		}
		for (std::uint32_t j = 0; j < candidate.class_records[i].bank_count; ++j) {
			banks.push_back(candidate.class_records[i].banks[j].bank_id);
		}
		if (!nonzero_unique(subsystems) || !nonzero_unique(banks)) return false;
	}
	for (std::uint32_t i = 0; i < candidate.weapon_record_count; ++i) {
		weapons.push_back(candidate.weapon_records[i].weapon_class_id);
	}
	return nonzero_unique(classes) && nonzero_unique(weapons);
}

TEST(Phase2Manifest, P2TST020FullRequiredContainsClassAndAnExplicitEmptyWeaponCatalog)
{
	ProvisionedSlot provisioned;
	auto source_storage = minimal_source();
	auto& source = *source_storage;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& candidate = provisioned.slot.staged_candidate();
	EXPECT_EQ(ManifestKind::FullRequired, candidate.kind);
	EXPECT_EQ(1u, candidate.class_record_count);
	EXPECT_EQ(0u, candidate.weapon_record_count);
	EXPECT_EQ(1u, candidate.manifest_id);
}

TEST(Phase2Manifest, P2REQ015CatalogsAreExhaustiveLeastPrivilegeAndRejectMissingOrDuplicateDefinitions)
{
	ProvisionedSlot provisioned;
	auto source_storage = test::wp03::make_source(test::wp03::SourceCase::TwoClassesThreeWeaponsWithDecoys);
	auto& source = *source_storage;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& candidate = provisioned.slot.staged_candidate();
	EXPECT_EQ(2u, candidate.class_record_count);
	EXPECT_EQ(3u, candidate.weapon_record_count);

	auto missing_storage = std::make_unique<Phase2ManifestSource>(source);
	auto& missing = *missing_storage;
	test::wp03::erase_weapon_definition(missing, source.referenced_weapon_keys[1]);
	EXPECT_EQ(Phase2ManifestError::MissingRequiredDefinition, provisioned.slot.rebuild(missing));
	EXPECT_EQ(candidate.manifest_id, provisioned.slot.staged_manifest_id());

	auto duplicate_storage = std::make_unique<Phase2ManifestSource>(source);
	auto& duplicate = *duplicate_storage;
	test::wp03::duplicate_weapon_definition(duplicate, source.referenced_weapon_keys[1]);
	EXPECT_EQ(Phase2ManifestError::DuplicateDefinition, provisioned.slot.rebuild(duplicate));
	EXPECT_EQ(candidate.manifest_id, provisioned.slot.staged_manifest_id());
}

TEST(Phase2Manifest, P2TST021CanonicalIdsAndWireBytesIgnoreEngineOrder)
{
	auto first = test::wp03::make_source(test::wp03::SourceCase::TwoClassesThreeWeaponsWithDecoys);
	auto second = std::make_unique<Phase2ManifestSource>(*first);
	test::wp03::reverse_engine_order(*second);
	ProvisionedSlot a;
	ProvisionedSlot b;
	ASSERT_EQ(Phase2ManifestError::None, a.slot.rebuild(*first));
	ASSERT_EQ(Phase2ManifestError::None, b.slot.rebuild(*second));
	const auto& left = a.slot.staged_candidate();
	const auto& right = b.slot.staged_candidate();
	ASSERT_EQ(left.encoded_size, right.encoded_size);
	EXPECT_TRUE(std::equal(left.encoded_bytes.begin(), left.encoded_bytes.end(), right.encoded_bytes.begin()));
	EXPECT_EQ(static_ids(left), static_ids(right));
}

TEST(Phase2Manifest, D2008IdsAreStableWithinGenerationAcrossOrderAndRespawnAndFormBijections)
{
	ProvisionedSlot provisioned;
	auto source_storage =
		test::wp03::make_source(test::wp03::SourceCase::CanonicalIdsWithHullTurretAndTertiaryBanks);
	auto& source = *source_storage;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto first = static_ids(provisioned.slot.staged_candidate());

	test::wp03::reverse_engine_order(source);
	ASSERT_EQ(Phase2ManifestError::NoCatalogChange, provisioned.slot.rebuild(source));
	const auto second = static_ids(provisioned.slot.staged_candidate());
	EXPECT_EQ(first, second);
	EXPECT_TRUE(ids_are_bijective(provisioned.slot.staged_candidate()));

	telemetry::detail::EntityIdRegistry registry;
	telemetry::detail::PlayerObservationKey first_key{};
	first_key.object_signature = 10;
	telemetry::detail::PlayerObservationKey respawn_key{};
	respawn_key.object_signature = 11;
	const auto first_entity = registry.resolve(first_key);
	const auto respawn_entity = registry.resolve(respawn_key);
	EXPECT_EQ(telemetry::detail::EntityIdResolveStatus::Allocated, first_entity.status);
	EXPECT_EQ(telemetry::detail::EntityIdResolveStatus::Allocated, respawn_entity.status);
	EXPECT_NE(first_entity.entity_id, respawn_entity.entity_id);
	source.player_instance_signature += 1; // respawn changes topology, not the installed static-ID generation
	EXPECT_EQ(Phase2ManifestError::TopologyOnly, provisioned.slot.rebuild(source));
}

TEST(Phase2Manifest, P2TST022SemanticDescriptorsNotNamesDefineIdentity)
{
	ProvisionedSlot provisioned;
	auto source_storage = minimal_source();
	auto& source = *source_storage;
	source.ship_classes[1] = source.ship_classes[0];
	source.ship_classes[1].source_key = 42;
	source.ship_classes[1].effective_mass = 200.0F;
	source.ship_class_count = 2;
	source.referenced_ship_class_keys[1] = 42;
	source.referenced_ship_class_count = 2;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& candidate = provisioned.slot.staged_candidate();
	ASSERT_EQ(2u, candidate.class_record_count);
	EXPECT_NE(candidate.class_records[0].class_id, candidate.class_records[1].class_id);

	source.auxiliary_entries[0] = {AuxiliaryRegistry::Species, 11, "duplicate"};
	source.auxiliary_entries[1] = {AuxiliaryRegistry::Species, 12, "duplicate"};
	source.auxiliary_entry_count = 2;
	EXPECT_EQ(Phase2ManifestError::AmbiguousAuxiliaryName, provisioned.slot.rebuild(source));
}

TEST(Phase2Manifest, D2008AuxiliaryIdsFollowLexicographicCanonicalKeysNotEngineIndexPlusOne)
{
	ProvisionedSlot provisioned;
	auto source_storage = minimal_source();
	auto& source = *source_storage;
	source.auxiliary_entries[0] = {AuxiliaryRegistry::Species, 90, "Zulu"};
	source.auxiliary_entries[1] = {AuxiliaryRegistry::Species, 3, "Alpha"};
	source.auxiliary_entries[2] = {AuxiliaryRegistry::Species, 71, "Mike"};
	source.auxiliary_entry_count = 3;
	source.ship_classes[1] = source.ship_classes[0];
	source.ship_classes[1].source_key = 42;
	source.ship_classes[1].name = "Second";
	source.ship_classes[1].species_index = 3;
	source.ship_class_count = 2;
	source.referenced_ship_class_keys[1] = 42;
	source.referenced_ship_class_count = 2;
	source.ship_classes[0].species_index = 90;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& records = provisioned.slot.staged_candidate().class_records;
	for (std::uint32_t i = 0; i < provisioned.slot.staged_candidate().class_record_count; ++i) {
		EXPECT_EQ(records[i].name == "Second" ? 1u : 2u, records[i].species_id);
	}
}

TEST(Phase2Manifest, P2REQ050BuilderRejectsInvalidUtf8AndEmbeddedNulAtomically)
{
	enum class MalformedString { InvalidUtf8, EmbeddedNul };
	for (const auto malformed : {MalformedString::InvalidUtf8, MalformedString::EmbeddedNul}) {
		ProvisionedSlot provisioned;
		auto baseline = minimal_source();
		ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(*baseline));
		const auto sentinel = provisioned.slot.staged_manifest_id();
		auto source_storage = minimal_source();
		auto& source = *source_storage;
		source.ship_classes[0].name =
			malformed == MalformedString::InvalidUtf8 ? std::string("\xc3\x28", 2) : std::string("A\0B", 3);
		EXPECT_EQ(Phase2ManifestError::InvalidString, provisioned.slot.rebuild(source));
		EXPECT_EQ(sentinel, provisioned.slot.staged_manifest_id());
	}
}

TEST(Phase2Manifest, P2TST027MinusOneMapsToZeroOnlyForExplicitAbsence)
{
	ProvisionedSlot provisioned;
	auto source_storage = minimal_source();
	auto& source = *source_storage;
	source.ship_classes[0].species_index = -1;
	source.ship_classes[0].ship_type_index = -1;
	source.ship_classes[0].wing_index = -1;
	source.ship_classes[0].armor_index = -1;
	source.ship_classes[0].damage_type_index = -1;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& record = provisioned.slot.staged_candidate().class_records[0];
	EXPECT_EQ(0u, record.species_id);
	EXPECT_EQ(0u, record.ship_type_id);
	EXPECT_FALSE(record.has_wing());
	EXPECT_FALSE(record.has_armor());
	EXPECT_EQ(0u, record.damage_type_id);

	const auto sentinel = provisioned.slot.staged_manifest_id();
	source.ship_classes[0].required_model_index = -1;
	EXPECT_EQ(Phase2ManifestError::InvalidSource, provisioned.slot.rebuild(source));
	EXPECT_EQ(sentinel, provisioned.slot.staged_manifest_id());
}

TEST(Phase2Manifest, P2TST023AcceptsAllExactSourceBoundsAndRejectsEveryPlusOneAtomically)
{
	for (const auto exact_case : {test::wp03::BoundaryCase::String65535,
		     test::wp03::BoundaryCase::RecordCount65535,
		     test::wp03::BoundaryCase::RecordLength65535,
		     test::wp03::BoundaryCase::ClassBanks192}) {
		ProvisionedSlot accepted;
		const auto exact = test::wp03::make_boundary_source(exact_case);
		ASSERT_EQ(Phase2ManifestError::None, accepted.slot.rebuild(*exact));
	}

	for (const auto overflow_case : {test::wp03::BoundaryCase::String65536,
		     test::wp03::BoundaryCase::RecordCount65536,
		     test::wp03::BoundaryCase::RecordLength65536,
		     test::wp03::BoundaryCase::ClassBanks193}) {
		ProvisionedSlot rejected;
		auto baseline = minimal_source();
		ASSERT_EQ(Phase2ManifestError::None, rejected.slot.rebuild(*baseline));
		const auto sentinel_id = rejected.slot.staged_manifest_id();
		const auto overflow = test::wp03::make_boundary_source(overflow_case);
		EXPECT_EQ(Phase2ManifestError::SourceLimitExceeded, rejected.slot.rebuild(*overflow));
		EXPECT_EQ(sentinel_id, rejected.slot.staged_manifest_id());
	}
}

TEST(Phase2Manifest, P2TST039UsesReal1025PerShipAnd4097AggregateCases)
{
	ProvisionedSlot provisioned;
	auto exact = test::wp03::make_subsystem_source({1024, 1024, 1024, 1024});
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(*exact));
	EXPECT_EQ(4096u, provisioned.slot.staged_candidate().aggregate_subsystem_count);
	const auto sentinel = provisioned.slot.staged_manifest_id();

	auto per_ship = test::wp03::make_subsystem_source({1025});
	EXPECT_EQ(Phase2ManifestError::TooManySubsystemsPerShip, provisioned.slot.rebuild(*per_ship));
	EXPECT_EQ(sentinel, provisioned.slot.staged_manifest_id());

	auto aggregate = test::wp03::make_subsystem_source({1024, 1024, 1024, 1024, 1});
	EXPECT_EQ(Phase2ManifestError::TooManyAggregateSubsystems, provisioned.slot.rebuild(*aggregate));
	EXPECT_EQ(sentinel, provisioned.slot.staged_manifest_id());
}

TEST(Phase2Manifest, P2TST039SubsystemCanonicalIndexAndSourceBijectionRejectAmbiguity)
{
	ProvisionedSlot provisioned;
	auto source_storage = test::wp03::make_subsystem_source({1024});
	auto& source = *source_storage;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& record = provisioned.slot.staged_candidate().class_records[0];
	for (std::uint32_t i = 0; i < 1024; ++i) {
		EXPECT_EQ(i, record.subsystems[i].canonical_index);
		EXPECT_EQ(i + 1, record.subsystems[i].subsystem_id);
	}

	source.ship_classes[0].subsystems[1].system_info_key =
		source.ship_classes[0].subsystems[0].system_info_key;
	EXPECT_EQ(Phase2ManifestError::DuplicateSystemInfo, provisioned.slot.rebuild(source));
	source.ship_classes[0].subsystems[1].system_info_key = UINT32_MAX;
	EXPECT_EQ(Phase2ManifestError::ForeignSystemInfo, provisioned.slot.rebuild(source));
}

TEST(Phase2Manifest, P2TST039ZeroMaximumRequiresZeroCurrentAndInstanceSystemInfoBijection)
{
	ProvisionedSlot provisioned;
	auto source_storage = test::wp03::make_subsystem_source({1});
	auto& source = *source_storage;
	source.ship_classes[0].subsystems[0].max_hits = 0.0F;
	source.ship_classes[0].subsystems[0].current_hits = 0.0F;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& subsystem = provisioned.slot.staged_candidate().class_records[0].subsystems[0];
	EXPECT_EQ(0u, subsystem.canonical_index);
	EXPECT_EQ(1u, subsystem.subsystem_id);

	source.ship_classes[0].subsystems[0].current_hits = 0.01F;
	EXPECT_EQ(Phase2ManifestError::InvalidZeroMaximumState, provisioned.slot.rebuild(source));
}

TEST(Phase2Manifest, P2TST039CoversZeroOneAnd1024AndDefinitionChangesRequireManifestKeyframe)
{
	for (const std::uint32_t count : {0u, 1u, 1024u}) {
		ProvisionedSlot provisioned;
		auto source = test::wp03::make_subsystem_source({count});
		ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(*source));
		EXPECT_EQ(count, provisioned.slot.staged_candidate().class_records[0].subsystem_count);
	}

	ProvisionedSlot provisioned;
	auto source_storage = test::wp03::make_subsystem_source({1});
	auto& source = *source_storage;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_manifest_applied(1));
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_dependent_snapshot_applied(1, 1));
	source.ship_classes[0].subsystems[0].max_hits += 1.0F;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	EXPECT_EQ(2u, provisioned.slot.staged_manifest_id());
	EXPECT_TRUE(provisioned.slot.keyframe_required_for(2));
	EXPECT_EQ(0u, provisioned.slot.staged_candidate().lifecycle_create_record_count);
	EXPECT_EQ(0u, provisioned.slot.staged_candidate().lifecycle_delete_record_count);
}

TEST(Phase2Manifest, P2TST047PublishesEffectiveMassInertiaAndExactAngleProvenanceNotDensity)
{
	ProvisionedSlot provisioned;
	auto source_storage = minimal_source();
	auto& source = *source_storage;
	source.ship_classes[0].model_mass = 10.0F;
	source.ship_classes[0].density_provenance = 2.0F;
	source.ship_classes[0].effective_mass = 20.0F;
	source.ship_classes[0].model_inertia = {2.0F, 4.0F, 8.0F};
	source.ship_classes[0].effective_inertia = {1.0F, 2.0F, 4.0F};
	source.ship_classes[0].full_angle_degrees_provenance = {0.0F, 180.0F, 360.0F};
	source.ship_classes[0].half_angle_cosines = {1.0F, 0.0F, -1.0F};
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& record = provisioned.slot.staged_candidate().class_records[0];
	EXPECT_FLOAT_EQ(20.0F, record.mass);
	EXPECT_EQ((std::array<float, 3>{1.0F, 2.0F, 4.0F}), record.inertia);
	EXPECT_FLOAT_EQ(0.0F, record.half_angles_rad[0]);
	EXPECT_FLOAT_EQ(static_cast<float>(std::acos(0.0)), record.half_angles_rad[1]);
	EXPECT_FLOAT_EQ(static_cast<float>(std::acos(-1.0)), record.half_angles_rad[2]);

	source.ship_classes[0].half_angle_cosines[0] = 1.01F;
	EXPECT_EQ(Phase2ManifestError::InvalidCosine, provisioned.slot.rebuild(source));
	source.ship_classes[0].half_angle_cosines[0] = std::numeric_limits<float>::quiet_NaN();
	EXPECT_EQ(Phase2ManifestError::NonFiniteDescriptor, provisioned.slot.rebuild(source));
}

TEST(Phase2Manifest, P2TST047AuxiliaryRegistriesUseCanonicalKeyOrderAndCountermeasureSemantics)
{
	ProvisionedSlot provisioned;
	auto source_storage = test::wp03::make_source(test::wp03::SourceCase::AllAuxiliaryRegistries);
	auto& source = *source_storage;
	source.ship_classes[0].countermeasure_capacity = 10.0F;
	source.ship_classes[0].countermeasure_cargo_size = 4.0F;
	source.ship_classes[0].countermeasure_uses_capacity = true;
	source.ship_classes[0].countermeasure_firewait_ms = 250;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& candidate = provisioned.slot.staged_candidate();
	const auto class_end = candidate.class_records.begin() + candidate.class_record_count;
	const auto record = std::find_if(candidate.class_records.begin(),
		class_end,
		[](const auto& value) { return value.name == "Ulysses"; });
	ASSERT_NE(class_end, record);
	EXPECT_EQ(2u, record->species_id);
	EXPECT_EQ(2u, record->ship_type_id);
	EXPECT_EQ(2u, record->iff_id);
	EXPECT_EQ(2u, record->wing_id);
	EXPECT_EQ(2u, record->subsystems[0].armor_id);
	const auto weapon_end = candidate.weapon_records.begin() + candidate.weapon_record_count;
	const auto weapon = std::find_if(candidate.weapon_records.begin(),
		weapon_end,
		[](const auto& value) { return value.name == "Countermeasure"; });
	ASSERT_NE(weapon_end, weapon);
	EXPECT_EQ(2u, weapon->damage_type_id);
	EXPECT_NE(0u, record->countermeasure_weapon_class_id);
	EXPECT_EQ(2u, record->countermeasure_initial_count);
	EXPECT_EQ(250000u, record->countermeasure_firewait_us);

	ProvisionedSlot count_mode;
	source.ship_classes[0].countermeasure_uses_capacity = false;
	ASSERT_EQ(Phase2ManifestError::None, count_mode.slot.rebuild(source));
	const auto& count_candidate = count_mode.slot.staged_candidate();
	const auto count_record = std::find_if(count_candidate.class_records.begin(),
		count_candidate.class_records.begin() + count_candidate.class_record_count,
		[](const auto& value) { return value.name == "Ulysses"; });
	ASSERT_NE(count_candidate.class_records.begin() + count_candidate.class_record_count, count_record);
	EXPECT_EQ(10u, count_record->countermeasure_initial_count);
}

TEST(Phase2Manifest, P2TST026KeepsActiveAndStagedUntilDependentSnapshotApplied)
{
	ProvisionedSlot provisioned;
	auto source_storage = minimal_source();
	auto& source = *source_storage;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_manifest_applied(1));
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_dependent_snapshot_applied(100, 1));
	EXPECT_EQ(1u, provisioned.slot.active_manifest_id());

	source.ship_classes[0].effective_mass = 101.0F;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	ASSERT_EQ(2u, provisioned.slot.staged_manifest_id());
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_manifest_applied(2));
	EXPECT_EQ(1u, provisioned.slot.active_manifest_id());
	EXPECT_EQ(2u, provisioned.slot.staged_manifest_id());
	EXPECT_TRUE(provisioned.slot.keyframe_allowed_for(2));
	EXPECT_TRUE(provisioned.slot.retains_generation(1));
	EXPECT_TRUE(provisioned.slot.retains_generation(2));

	source.ship_classes[0].effective_mass = 102.0F;
	EXPECT_EQ(Phase2ManifestError::RebuildCoalesced, provisioned.slot.rebuild(source));
	EXPECT_TRUE(provisioned.slot.has_rebuild_intent());
	EXPECT_EQ(2u, provisioned.slot.resident_generation_count());

	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_dependent_snapshot_applied(101, 2));
	EXPECT_EQ(2u, provisioned.slot.active_manifest_id());
	EXPECT_EQ(0u, provisioned.slot.staged_manifest_id());
	EXPECT_TRUE(provisioned.slot.retains_generation(1)); // reliable references still hold N
	provisioned.slot.release_reliable_references(1);
	EXPECT_FALSE(provisioned.slot.retains_generation(1));
	EXPECT_TRUE(provisioned.slot.rebuild_intent_scheduled());
}

TEST(Phase2Manifest, FingerprintsSeparateTopologyCatalogAndExcludeIdsAndGenerations)
{
	ProvisionedSlot provisioned;
	auto source_storage = minimal_source();
	auto& source = *source_storage;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto original_fingerprint = provisioned.slot.staged_candidate().catalog_fingerprint;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_manifest_applied(1));
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_dependent_snapshot_applied(1, 1));

	auto excluded_storage = std::make_unique<Phase2ManifestSource>(source);
	auto& excluded = *excluded_storage;
	excluded.engine_index += 100;
	excluded.manifest_generation += 100;
	EXPECT_EQ(Phase2ManifestError::NoCatalogChange, provisioned.slot.rebuild(excluded));
	EXPECT_EQ(original_fingerprint, provisioned.slot.catalog_fingerprint());

	auto topology_storage = std::make_unique<Phase2ManifestSource>(source);
	auto& topology = *topology_storage;
	topology.player_instance_signature += 1;
	EXPECT_EQ(Phase2ManifestError::TopologyOnly, provisioned.slot.rebuild(topology));
	EXPECT_EQ(1u, provisioned.slot.active_manifest_id());
	EXPECT_TRUE(provisioned.slot.keyframe_required_for(1));
	EXPECT_EQ(original_fingerprint, provisioned.slot.catalog_fingerprint());

	auto catalog_storage = std::make_unique<Phase2ManifestSource>(source);
	auto& catalog = *catalog_storage;
	catalog.ship_classes[0].effective_mass += 1.0F;
	EXPECT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(catalog));
	EXPECT_GT(provisioned.slot.staged_manifest_id(), 1u);
	EXPECT_NE(original_fingerprint, provisioned.slot.staged_candidate().catalog_fingerprint);
}

TEST(Phase2Manifest, CatalogFingerprintUsesCompleteSemanticDescriptorsAndNotRawSourceKeys)
{
	auto source_storage = test::wp03::make_source(test::wp03::SourceCase::AllAuxiliaryRegistries);
	auto& source = *source_storage;
	ProvisionedSlot baseline;
	ASSERT_EQ(Phase2ManifestError::None, baseline.slot.rebuild(source));
	const auto fingerprint = baseline.slot.staged_candidate().catalog_fingerprint;

	auto raw_changed_storage = std::make_unique<Phase2ManifestSource>(source);
	auto& raw_changed = *raw_changed_storage;
	const auto old_class_key = raw_changed.ship_classes[0].source_key;
	raw_changed.ship_classes[0].source_key += 5000;
	for (std::uint32_t i = 0; i < raw_changed.referenced_ship_class_count; ++i) {
		if (raw_changed.referenced_ship_class_keys[i] == old_class_key) {
			raw_changed.referenced_ship_class_keys[i] = raw_changed.ship_classes[0].source_key;
		}
	}
	raw_changed.engine_index += 99;
	test::wp03::reverse_engine_order(raw_changed);
	ProvisionedSlot raw_slot;
	ASSERT_EQ(Phase2ManifestError::None, raw_slot.slot.rebuild(raw_changed));
	EXPECT_EQ(fingerprint, raw_slot.slot.staged_candidate().catalog_fingerprint);

	for (const auto mutation : {0, 1, 2, 3}) {
		auto semantic_storage = std::make_unique<Phase2ManifestSource>(source);
		auto& semantic = *semantic_storage;
		switch (mutation) {
		case 0: semantic.ship_classes[0].name = "Semantic rename"; break;
		case 1: semantic.weapons[0].title = "Semantic weapon title"; break;
		case 2: semantic.auxiliary_entries[0].name = "Semantic auxiliary"; break;
		case 3: semantic.ship_classes[0].subsystems[0].max_hits += 1.0F; break;
		}
		ProvisionedSlot changed;
		ASSERT_EQ(Phase2ManifestError::None, changed.slot.rebuild(semantic));
		EXPECT_NE(fingerprint, changed.slot.staged_candidate().catalog_fingerprint) << mutation;
	}
}

TEST(Phase2Manifest, D2006NoAllocationAfterReadyUsesCallerOwnedTwoGenerationArenaAndFailsAtomically)
{
	static_assert(64u * 64u == 4096u);
	static_assert(8u <= 192u && 4u <= 64u && 64u <= 256u);
	static_assert(4096u <= 4096u);
	ProvisionedSlot provisioned;
	auto source_storage = test::wp03::make_source(test::wp03::SourceCase::NearMaxTransaction);
	auto& source = *source_storage;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source)); // warm/provision before Ready
	ASSERT_EQ(Phase2ManifestError::None,
		provisioned.slot.on_manifest_applied(provisioned.slot.staged_manifest_id()));
	ASSERT_EQ(Phase2ManifestError::None,
		provisioned.slot.on_dependent_snapshot_applied(1, provisioned.slot.staged_manifest_id()));
	const auto sentinel_id = provisioned.slot.active_manifest_id();
	const auto allocations_at_ready = provisioned.fallback.allocations;
	EXPECT_EQ(ManifestKind::FullRequired, provisioned.slot.active_candidate().kind);
	EXPECT_EQ(64u, provisioned.slot.active_candidate().class_record_count);
	EXPECT_EQ(4096u, provisioned.slot.active_candidate().weapon_record_count);
	EXPECT_EQ(4096u, provisioned.slot.active_candidate().aggregate_subsystem_count);
	EXPECT_GE(provisioned.slot.active_candidate().encoded_size, NearMaxLegalLowerBound);

	source.ship_classes[0].effective_mass += 1.0F;
	test::wp03::GlobalAllocationScope allocation_scope;
	const auto result = provisioned.slot.rebuild(source);
	const auto global_allocations = allocation_scope.finish();
	EXPECT_EQ(Phase2ManifestError::None, result);
	EXPECT_EQ(0u, global_allocations);
	EXPECT_EQ(allocations_at_ready, provisioned.fallback.allocations);
	EXPECT_EQ(64u, provisioned.slot.staged_candidate().class_record_count);
	EXPECT_EQ(4096u, provisioned.slot.staged_candidate().weapon_record_count);
	EXPECT_EQ(4096u, provisioned.slot.staged_candidate().aggregate_subsystem_count);
	EXPECT_GE(provisioned.slot.staged_candidate().encoded_size, NearMaxLegalLowerBound);
	EXPECT_GE(provisioned.slot.active_candidate().encoded_size + provisioned.slot.staged_candidate().encoded_size,
		2 * NearMaxLegalLowerBound);

	auto overflow_storage = minimal_source();
	auto& overflow = *overflow_storage;
	overflow.ship_classes[0].name = std::string("\xc3\x28", 2);
	const auto staged_hash = provisioned.slot.staged_candidate().transaction_sha256;
	EXPECT_EQ(Phase2ManifestError::InvalidString, provisioned.slot.rebuild(overflow));
	EXPECT_EQ(allocations_at_ready, provisioned.fallback.allocations);
	EXPECT_EQ(sentinel_id + 1, provisioned.slot.staged_manifest_id());
	EXPECT_EQ(staged_hash, provisioned.slot.staged_candidate().transaction_sha256);

	ASSERT_EQ(Phase2ManifestError::None,
		provisioned.slot.on_manifest_applied(provisioned.slot.staged_manifest_id()));
	ASSERT_EQ(Phase2ManifestError::None,
		provisioned.slot.on_dependent_snapshot_applied(2, provisioned.slot.staged_manifest_id()));
	provisioned.slot.release_reliable_references(sentinel_id);
	source.ship_classes[0].effective_mass += 1.0F;
	test::wp03::GlobalAllocationScope reuse_scope;
	const auto reuse_result = provisioned.slot.rebuild(source);
	const auto reuse_allocations = reuse_scope.finish();
	EXPECT_EQ(Phase2ManifestError::None, reuse_result);
	EXPECT_EQ(0u, reuse_allocations);
	EXPECT_EQ(allocations_at_ready, provisioned.fallback.allocations);
}

TEST(Phase2Manifest, D2006InsufficientCallerArenaAndThrowingFallbackFailAtomically)
{
	CountingFailResource fallback;
	std::vector<std::uint8_t> active_arena(1024);
	std::vector<std::uint8_t> staged_arena(1024);
	Phase2ManifestStorage storage(MutableByteView{active_arena.data(), active_arena.size()},
		MutableByteView{staged_arena.data(), staged_arena.size()},
		&fallback);
	auto slot = std::make_unique<Phase2ManifestSlot>(storage);
	auto baseline = minimal_source();
	ASSERT_EQ(Phase2ManifestError::None, slot->rebuild(*baseline));
	const auto sentinel = slot->staged_manifest_id();
	const auto sentinel_hash = slot->staged_candidate().transaction_sha256;
	fallback.fail = true;
	auto multipart = test::wp03::make_source(test::wp03::SourceCase::MultipartFullRequired);
	const auto result = slot->rebuild(*multipart);
	EXPECT_EQ(Phase2ManifestError::AllocationFailed, result);
	EXPECT_EQ(sentinel, slot->staged_manifest_id());
	EXPECT_EQ(sentinel_hash, slot->staged_candidate().transaction_sha256);
}

} // namespace
