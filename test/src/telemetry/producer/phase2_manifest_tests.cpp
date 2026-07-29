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

struct WireRecordStats {
	std::uint32_t count = 0;
	std::size_t maximum_payload = 0;
};

WireRecordStats wire_record_stats(const Phase2ManifestCandidate& candidate)
{
	WireRecordStats stats{};
	for (std::uint16_t part_index = 0; part_index < candidate.part_count; ++part_index) {
		const auto& part = candidate.parts[part_index];
		RecordEnvelopeIterator iterator(part.records, part.record_count, RecordFlagPolicy::RequireNone);
		for (;;) {
			RecordEnvelopeView envelope{};
			bool has_value = false;
			EXPECT_EQ(ValidationError::None, iterator.next(envelope, has_value));
			if (!has_value) break;
			++stats.count;
			stats.maximum_payload = std::max(stats.maximum_payload, envelope.payload.size);
		}
	}
	return stats;
}

std::unique_ptr<Phase2ManifestSource> exact_max_class_record_source(bool plus_one)
{
	auto source = minimal_source();
	auto& ship_class = source->ship_classes[0];
	ship_class.name.assign(255, 'C');
	ship_class.subsystem_count = 81;
	for (std::uint32_t index = 0; index < 80; ++index) {
		auto& subsystem = ship_class.subsystems[index];
		subsystem.source_key = 1000 + index;
		subsystem.system_info_key = 2000 + index;
		subsystem.name.assign(255, 'N');
		subsystem.alt_name.assign(255, 'A');
		subsystem.hud_name.assign(255, 'H');
	}
	auto& tail = ship_class.subsystems[80];
	tail.source_key = 1080;
	tail.system_info_key = 2080;
	tail.name.assign(255, 'T');
	tail.alt_name.assign(plus_one ? 185 : 184, 'A');
	return source;
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

TEST(Phase2Manifest, P2TST021CanonicalOrderingUsesCompleteDescriptorsWhenNamesAndShallowKeysCollide)
{
	auto first = test::wp03::make_source(test::wp03::SourceCase::TwoClassesThreeWeaponsWithDecoys);
	first->ship_classes[0].name = "Colliding class";
	first->ship_classes[1].name = "Colliding class";
	first->ship_classes[0].effective_mass = 100.0F;
	first->ship_classes[1].effective_mass = 100.0F;
	first->ship_classes[0].subsystem_count = 1;
	first->ship_classes[1].subsystem_count = 1;
	first->ship_classes[0].subsystems[0].system_info_key = 10;
	first->ship_classes[1].subsystems[0].system_info_key = 20;
	first->ship_classes[0].subsystems[0].max_hits = 10.0F;
	first->ship_classes[1].subsystems[0].max_hits = 20.0F;
	first->weapons[0].name = "Colliding weapon";
	first->weapons[1].name = "Colliding weapon";
	first->weapons[0].title = "Same title";
	first->weapons[1].title = "Same title";
	first->weapons[0].has_damage = true;
	first->weapons[1].has_damage = true;
	first->weapons[0].damage = 10.0F;
	first->weapons[1].damage = 20.0F;

	auto reversed = std::make_unique<Phase2ManifestSource>(*first);
	test::wp03::reverse_engine_order(*reversed);
	ProvisionedSlot a;
	ProvisionedSlot b;
	ASSERT_EQ(Phase2ManifestError::None, a.slot.rebuild(*first));
	ASSERT_EQ(Phase2ManifestError::None, b.slot.rebuild(*reversed));
	const auto& left = a.slot.staged_candidate();
	const auto& right = b.slot.staged_candidate();
	EXPECT_EQ(left.catalog_fingerprint, right.catalog_fingerprint)
		<< "P2-TST-021 requires complete canonical descriptors, not source order, to break collisions.";
	EXPECT_EQ(left.topology_fingerprint, right.topology_fingerprint);
	ASSERT_EQ(left.encoded_size, right.encoded_size);
	EXPECT_TRUE(std::equal(left.encoded_bytes.begin(), left.encoded_bytes.end(), right.encoded_bytes.begin()))
		<< "P2-REQ-017 requires identical IDs and bytes for semantically identical catalogs.";
	EXPECT_EQ(static_ids(left), static_ids(right));
}

TEST(Phase2Manifest, P2TST021BankDescriptorsUseTheFullLexicographicTopology)
{
	auto first = minimal_source();
	auto& ship_class = first->ship_classes[0];
	ship_class.subsystem_count = 2;
	for (std::uint32_t index = 0; index < ship_class.subsystem_count; ++index) {
		ship_class.subsystems[index].source_key = 10 + index;
		ship_class.subsystems[index].system_info_key = 20 + index;
		ship_class.subsystems[index].name = "Turret " + std::to_string(index);
	}
	ship_class.bank_count = 2;
	for (std::uint32_t index = 0; index < ship_class.bank_count; ++index) {
		auto& bank = ship_class.banks[index];
		bank.family = WeaponFamily::Tertiary;
		bank.source_family = WeaponFamily::Tertiary;
		bank.bank_index = 0;
		bank.owner_subsystem_canonical_index = static_cast<std::uint16_t>(1 - index);
	}
	auto reversed = std::make_unique<Phase2ManifestSource>(*first);
	std::swap(reversed->ship_classes[0].banks[0], reversed->ship_classes[0].banks[1]);

	ProvisionedSlot a;
	ProvisionedSlot b;
	ASSERT_EQ(Phase2ManifestError::None, a.slot.rebuild(*first));
	ASSERT_EQ(Phase2ManifestError::None, b.slot.rebuild(*reversed));
	const auto& left = a.slot.staged_candidate();
	const auto& right = b.slot.staged_candidate();
	EXPECT_EQ(left.catalog_fingerprint, right.catalog_fingerprint);
	EXPECT_EQ(left.topology_fingerprint, right.topology_fingerprint);
	ASSERT_EQ(left.encoded_size, right.encoded_size);
	EXPECT_TRUE(std::equal(left.encoded_bytes.begin(), left.encoded_bytes.end(), right.encoded_bytes.begin()));
	ASSERT_EQ(2u, left.class_records[0].bank_count);
	EXPECT_EQ(1u, left.class_records[0].banks[0].owner_subsystem_id);
	EXPECT_EQ(2u, left.class_records[0].banks[1].owner_subsystem_id);
	EXPECT_EQ(WeaponFamily::Tertiary, left.class_records[0].banks[0].source_family);
	EXPECT_EQ(WeaponFamily::Tertiary, left.class_records[0].banks[1].source_family);
	EXPECT_EQ(1u, left.class_records[0].banks[0].bank_id);
	EXPECT_EQ(2u, left.class_records[0].banks[1].bank_id);
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
	source.ship_classes[0].species_index = 11;
	source.ship_classes[1].species_index = 12;
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

TEST(Phase2Manifest, P2REQ017BankIdsAreNonZeroAndDistinctAcrossTheWholeManifestGeneration)
{
	auto source_storage = minimal_source();
	auto& source = *source_storage;
	source.ship_classes[1] = source.ship_classes[0];
	source.ship_classes[1].source_key = 42;
	source.ship_classes[1].name = "Second class";
	source.ship_class_count = 2;
	source.referenced_ship_class_keys[1] = 42;
	source.referenced_ship_class_count = 2;
	for (std::uint32_t class_index = 0; class_index < 2; ++class_index) {
		auto& ship_class = source.ship_classes[class_index];
		ship_class.bank_count = 1;
		ship_class.banks[0].family = telemetry::WeaponFamily::Tertiary;
		ship_class.banks[0].source_family = telemetry::WeaponFamily::Tertiary;
		ship_class.banks[0].bank_index = 0;
	}

	ProvisionedSlot provisioned;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	const auto& candidate = provisioned.slot.staged_candidate();
	std::vector<std::uint32_t> bank_ids;
	for (std::uint32_t class_index = 0; class_index < candidate.class_record_count; ++class_index) {
		const auto& ship_class = candidate.class_records[class_index];
		for (std::uint32_t bank_index = 0; bank_index < ship_class.bank_count; ++bank_index) {
			bank_ids.push_back(ship_class.banks[bank_index].bank_id);
		}
	}
	ASSERT_EQ(2u, bank_ids.size());
	EXPECT_TRUE(nonzero_unique(bank_ids))
		<< "P2-REQ-017/D2-008 require one canonical non-zero bank ID per (class, family, owner, source, index) tuple.";
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
	for (const auto exact_case : {test::wp03::BoundaryCase::ClassBanks192}) {
		ProvisionedSlot accepted;
		const auto exact = test::wp03::make_boundary_source(exact_case);
		ASSERT_EQ(Phase2ManifestError::None, accepted.slot.rebuild(*exact));
	}

	for (const auto overflow_case : {test::wp03::BoundaryCase::ClassBanks193}) {
		ProvisionedSlot rejected;
		auto baseline = minimal_source();
		ASSERT_EQ(Phase2ManifestError::None, rejected.slot.rebuild(*baseline));
		const auto sentinel_id = rejected.slot.staged_manifest_id();
		const auto overflow = test::wp03::make_boundary_source(overflow_case);
		EXPECT_EQ(Phase2ManifestError::SourceLimitExceeded, rejected.slot.rebuild(*overflow));
		EXPECT_EQ(sentinel_id, rejected.slot.staged_manifest_id());
	}
}

TEST(Phase2Manifest, P2TST023ActualCatalogFieldsAcceptExactLimitsAndRejectEachPlusOne)
{
	{
		ProvisionedSlot provisioned;
		auto source = minimal_source();
		source->ship_classes[0].name.assign(255, 'C');
		ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(*source));
		const auto exact_stats = wire_record_stats(provisioned.slot.staged_candidate());
		EXPECT_EQ(1u, exact_stats.count);
		EXPECT_EQ(provisioned.slot.staged_candidate().encoded_size - RecordEnvelopeHeaderSize,
			exact_stats.maximum_payload);
		EXPECT_LE(exact_stats.maximum_payload, 65535u);
		const auto sentinel = provisioned.slot.staged_manifest_id();
		source->ship_classes[0].name.push_back('X');
		EXPECT_EQ(Phase2ManifestError::InvalidString, provisioned.slot.rebuild(*source));
		EXPECT_EQ(sentinel, provisioned.slot.staged_manifest_id());
	}

	{
		ProvisionedSlot provisioned;
		auto source = minimal_source();
		source->ship_class_count = Phase2ManifestLimits::MaxClasses;
		source->referenced_ship_class_count = Phase2ManifestLimits::MaxClasses;
		for (std::uint32_t index = 1; index < Phase2ManifestLimits::MaxClasses; ++index) {
			source->ship_classes[index] = source->ship_classes[0];
			source->ship_classes[index].source_key = 1000 + index;
			source->ship_classes[index].name = "Class " + std::to_string(index);
			source->referenced_ship_class_keys[index] = source->ship_classes[index].source_key;
		}
		ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(*source));
		EXPECT_EQ(Phase2ManifestLimits::MaxClasses,
			wire_record_stats(provisioned.slot.staged_candidate()).count);
		const auto sentinel = provisioned.slot.staged_manifest_id();
		source->ship_class_count = Phase2ManifestLimits::MaxClasses + 1;
		source->referenced_ship_class_count = Phase2ManifestLimits::MaxClasses + 1;
		EXPECT_EQ(Phase2ManifestError::SourceLimitExceeded, provisioned.slot.rebuild(*source));
		EXPECT_EQ(sentinel, provisioned.slot.staged_manifest_id());
	}

	{
		ProvisionedSlot provisioned;
		auto source = test::wp03::make_source(test::wp03::SourceCase::MultipartFullRequired);
		ASSERT_EQ(Phase2ManifestLimits::MaxWeapons, source->weapon_count);
		ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(*source));
		EXPECT_EQ(1u + Phase2ManifestLimits::MaxWeapons,
			wire_record_stats(provisioned.slot.staged_candidate()).count);
		const auto sentinel = provisioned.slot.staged_manifest_id();
		source->weapon_count = Phase2ManifestLimits::MaxWeapons + 1;
		source->referenced_weapon_count = Phase2ManifestLimits::MaxWeapons + 1;
		EXPECT_EQ(Phase2ManifestError::SourceLimitExceeded, provisioned.slot.rebuild(*source));
		EXPECT_EQ(sentinel, provisioned.slot.staged_manifest_id());
	}
}

TEST(Phase2Manifest, P2TST023ClassManifestRecordLength65535IsRealAndPlusOneFailsBeforeEgress)
{
	ProvisionedSlot provisioned;
	auto exact = exact_max_class_record_source(false);
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(*exact));
	const auto& candidate = provisioned.slot.staged_candidate();
	const auto stats = wire_record_stats(candidate);
	ASSERT_EQ(1u, stats.count);
	EXPECT_EQ(65535u, stats.maximum_payload);
	ASSERT_GE(candidate.encoded_bytes.size, RecordEnvelopeHeaderSize);
	EXPECT_EQ(65535u,
		static_cast<std::uint32_t>(candidate.encoded_bytes.data[4]) |
			(static_cast<std::uint32_t>(candidate.encoded_bytes.data[5]) << 8));
	EXPECT_EQ(65535u + RecordEnvelopeHeaderSize, candidate.encoded_size);
	const auto sentinel_id = candidate.manifest_id;
	const auto sentinel_hash = candidate.transaction_sha256;
	const auto sentinel_size = candidate.encoded_size;

	auto plus_one = exact_max_class_record_source(true);
	EXPECT_EQ(Phase2ManifestError::AllocationFailed, provisioned.slot.rebuild(*plus_one));
	EXPECT_EQ(sentinel_id, provisioned.slot.staged_manifest_id());
	EXPECT_EQ(sentinel_hash, provisioned.slot.staged_candidate().transaction_sha256);
	EXPECT_EQ(sentinel_size, provisioned.slot.staged_candidate().encoded_size);
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

TEST(Phase2Manifest, P2TST047BankOwnerAndAmmunitionSemanticsAreProjectedAndValidated)
{
	auto source = test::wp03::make_source(test::wp03::SourceCase::TwoClassesThreeWeaponsWithDecoys);
	auto& ship_class = source->ship_classes[0];
	ship_class.subsystem_count = 1;
	ship_class.subsystems[0].source_key = 700;
	ship_class.subsystems[0].system_info_key = 701;
	ship_class.subsystems[0].name = "Missile turret";
	ship_class.bank_count = 2;
	ship_class.banks[0].family = WeaponFamily::Primary;
	ship_class.banks[0].source_family = WeaponFamily::Primary;
	ship_class.banks[0].bank_index = 0;
	ship_class.banks[0].weapon_source_key = 101;
	ship_class.banks[0].consumes_ammunition = false;
	ship_class.banks[0].capacity = 99.0F;
	ship_class.banks[1].family = WeaponFamily::Secondary;
	ship_class.banks[1].source_family = WeaponFamily::Secondary;
	ship_class.banks[1].bank_index = 0;
	ship_class.banks[1].owner_subsystem_canonical_index = 0;
	ship_class.banks[1].weapon_source_key = 102;
	ship_class.banks[1].consumes_ammunition = true;
	ship_class.banks[1].capacity = 12.0F;

	ProvisionedSlot provisioned;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(*source));
	const auto& candidate = provisioned.slot.staged_candidate();
	const auto class_end = candidate.class_records.begin() + candidate.class_record_count;
	const auto record = std::find_if(candidate.class_records.begin(), class_end, [&](const auto& value) {
		return value.name == ship_class.name;
	});
	ASSERT_NE(class_end, record);
	ASSERT_EQ(2u, record->bank_count);
	const auto primary = std::find_if(record->banks.begin(), record->banks.end(), [](const auto& bank) {
		return bank.family == WeaponFamily::Primary;
	});
	const auto secondary = std::find_if(record->banks.begin(), record->banks.end(), [](const auto& bank) {
		return bank.family == WeaponFamily::Secondary;
	});
	ASSERT_NE(record->banks.end(), primary);
	ASSERT_NE(record->banks.end(), secondary);
	EXPECT_EQ(0u, primary->owner_subsystem_id);
	EXPECT_EQ(WeaponFamily::Primary, primary->source_family);
	EXPECT_EQ(1u, secondary->owner_subsystem_id);
	EXPECT_EQ(WeaponFamily::Secondary, secondary->source_family);

	auto invalid = std::make_unique<Phase2ManifestSource>(*source);
	invalid->ship_classes[0].banks[1].owner_subsystem_canonical_index = 1;
	EXPECT_EQ(Phase2ManifestError::InvalidSource, provisioned.slot.rebuild(*invalid));
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
	EXPECT_EQ(Phase2ManifestError::InvalidSource,
		provisioned.slot.on_dependent_snapshot_applied(101, 2));
	EXPECT_EQ(1u, provisioned.slot.active_manifest_id());
	EXPECT_EQ(2u, provisioned.slot.staged_manifest_id());
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
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	EXPECT_EQ(3u, provisioned.slot.staged_manifest_id());
	EXPECT_EQ(2u, provisioned.slot.resident_generation_count());
	EXPECT_FALSE(provisioned.slot.has_rebuild_intent());
	EXPECT_FALSE(provisioned.slot.rebuild_intent_scheduled());
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_manifest_applied(3));
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.on_dependent_snapshot_applied(102, 3));
	EXPECT_FALSE(provisioned.slot.has_rebuild_intent());
	EXPECT_FALSE(provisioned.slot.rebuild_intent_scheduled());
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

TEST(Phase2Manifest, P2REQ018CatalogFingerprintIncludesAuxiliaryAssociationsAtConstantDefinitionSet)
{
	auto baseline = test::wp03::make_source(test::wp03::SourceCase::AllAuxiliaryRegistries);
	auto reassigned = std::make_unique<Phase2ManifestSource>(*baseline);
	std::swap(reassigned->ship_classes[0].species_index, reassigned->ship_classes[1].species_index);
	std::swap(reassigned->ship_classes[0].subsystems[0].armor_index,
		reassigned->ship_classes[1].subsystems[0].armor_index);
	std::swap(reassigned->weapons[0].damage_type_index, reassigned->weapons[1].damage_type_index);

	ProvisionedSlot lifecycle;
	ASSERT_EQ(Phase2ManifestError::None, lifecycle.slot.rebuild(*baseline));
	const auto original_catalog = lifecycle.slot.staged_candidate().catalog_fingerprint;
	const auto original_topology = lifecycle.slot.staged_candidate().topology_fingerprint;
	ASSERT_EQ(Phase2ManifestError::None, lifecycle.slot.on_manifest_applied(1));
	ASSERT_EQ(Phase2ManifestError::None, lifecycle.slot.on_dependent_snapshot_applied(1, 1));
	ASSERT_EQ(Phase2ManifestError::None, lifecycle.slot.rebuild(*reassigned))
		<< "Auxiliary associations are catalog semantics even when the auxiliary definition set is constant.";
	EXPECT_EQ(2u, lifecycle.slot.staged_manifest_id());
	EXPECT_NE(original_catalog, lifecycle.slot.staged_candidate().catalog_fingerprint);
	EXPECT_EQ(original_topology, lifecycle.slot.staged_candidate().topology_fingerprint)
		<< "Closure evidence is topological; the manifest catalog fingerprint owns descriptor associations.";

	auto reordered = std::make_unique<Phase2ManifestSource>(*reassigned);
	test::wp03::reverse_engine_order(*reordered);
	std::reverse(reordered->auxiliary_entries.begin(),
		reordered->auxiliary_entries.begin() + reordered->auxiliary_entry_count);
	ProvisionedSlot canonical;
	ProvisionedSlot reversed;
	ASSERT_EQ(Phase2ManifestError::None, canonical.slot.rebuild(*reassigned));
	ASSERT_EQ(Phase2ManifestError::None, reversed.slot.rebuild(*reordered));
	EXPECT_EQ(canonical.slot.staged_candidate().catalog_fingerprint,
		reversed.slot.staged_candidate().catalog_fingerprint);
	EXPECT_EQ(canonical.slot.staged_candidate().topology_fingerprint,
		reversed.slot.staged_candidate().topology_fingerprint);
	ASSERT_EQ(canonical.slot.staged_candidate().encoded_size,
		reversed.slot.staged_candidate().encoded_size);
	EXPECT_TRUE(std::equal(canonical.slot.staged_candidate().encoded_bytes.begin(),
		canonical.slot.staged_candidate().encoded_bytes.end(),
		reversed.slot.staged_candidate().encoded_bytes.begin()));
}

TEST(Phase2Manifest, P2REQ015UnreferencedAuxiliaryDefinitionsDoNotChangeTheLeastPrivilegeCatalog)
{
	auto source_storage = test::wp03::make_source(test::wp03::SourceCase::AllAuxiliaryRegistries);
	auto& source = *source_storage;
	ProvisionedSlot provisioned;
	ASSERT_EQ(Phase2ManifestError::None, provisioned.slot.rebuild(source));
	ASSERT_EQ(Phase2ManifestError::None,
		provisioned.slot.on_manifest_applied(provisioned.slot.staged_manifest_id()));
	ASSERT_EQ(Phase2ManifestError::None,
		provisioned.slot.on_dependent_snapshot_applied(1, provisioned.slot.staged_manifest_id()));
	const auto active_id = provisioned.slot.active_manifest_id();
	const auto active_hash = provisioned.slot.active_candidate().transaction_sha256;

	source.auxiliary_entries[source.auxiliary_entry_count++] =
		{AuxiliaryRegistry::Species, 999, "WP03-UNREFERENCED-AUXILIARY"};
	EXPECT_EQ(Phase2ManifestError::NoCatalogChange, provisioned.slot.rebuild(source))
		<< "P2-REQ-015/019 require the manifest and fingerprint to contain only definitions referenced by the closure.";
	EXPECT_EQ(active_id, provisioned.slot.active_manifest_id());
	EXPECT_EQ(0u, provisioned.slot.staged_manifest_id());
	EXPECT_EQ(active_hash, provisioned.slot.active_candidate().transaction_sha256);
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
