#include "telemetry/phase4_catalog_assembly.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace {

using telemetry::Phase2ManifestSource;
using telemetry::detail::Phase4CatalogAssemblyStatus;
using telemetry::detail::Phase4CatalogDefinitionReadStatus;
using telemetry::detail::Phase4CatalogDefinitionReadView;
using telemetry::detail::Phase4EngineInventoryEntry;
using telemetry::detail::assemble_phase4_catalog_definitions;
using telemetry::protocol::ObjectType;

Phase4EngineInventoryEntry inventory_entry(std::uint64_t identity,
	std::uint64_t entity_id, ObjectType type, std::uint32_t source_key)
{
	Phase4EngineInventoryEntry entry;
	entry.identity = {identity, type};
	entry.entity_id = entity_id;
	entry.source_class_key = source_key;
	entry.orientation_local_to_world = {0.0F, 0.0F, 0.0F, 1.0F};
	return entry;
}

std::unique_ptr<Phase2ManifestSource> ship_fragment(std::uint32_t class_key,
	std::initializer_list<std::uint32_t> weapon_keys)
{
	auto source = std::make_unique<Phase2ManifestSource>();
	source->ship_class_count = 1U;
	source->ship_classes[0].source_key = class_key;
	source->ship_classes[0].name = "ship-" + std::to_string(class_key);
	for (const auto weapon_key : weapon_keys) {
		auto& weapon = source->weapons[source->weapon_count++];
		weapon.source_key = weapon_key;
		weapon.name = "weapon-" + std::to_string(weapon_key);
		auto& bank = source->ship_classes[0]
			.banks[source->ship_classes[0].bank_count++];
		bank.weapon_source_key = weapon_key;
	}
	return source;
}

std::unique_ptr<Phase2ManifestSource> weapon_fragment(std::uint32_t weapon_key)
{
	auto source = std::make_unique<Phase2ManifestSource>();
	source->weapon_count = 1U;
	source->weapons[0].source_key = weapon_key;
	source->weapons[0].name = "weapon-" + std::to_string(weapon_key);
	return source;
}

class FakeCatalogReader final : public Phase4CatalogDefinitionReadView {
  public:
	struct Definition {
		std::uint32_t key = 0U;
		std::unique_ptr<Phase2ManifestSource> source;
	};

	Phase4CatalogDefinitionReadStatus read_ship_definition(
		const Phase4EngineInventoryEntry& entry,
		Phase2ManifestSource& output) const noexcept override
	{
		++ship_reads;
		return read(ship_definitions, entry.source_class_key, output);
	}

	Phase4CatalogDefinitionReadStatus read_weapon_definition(
		std::uint32_t key, Phase2ManifestSource& output) const noexcept override
	{
		++weapon_reads;
		return read(weapon_definitions, key, output);
	}

	std::vector<Definition> ship_definitions;
	std::vector<Definition> weapon_definitions;
	mutable std::uint32_t ship_reads = 0U;
	mutable std::uint32_t weapon_reads = 0U;

  private:
	static Phase4CatalogDefinitionReadStatus read(
		const std::vector<Definition>& definitions, std::uint32_t key,
		Phase2ManifestSource& output) noexcept
	{
		for (const auto& definition : definitions)
			if (definition.key == key) {
				output = *definition.source;
				return Phase4CatalogDefinitionReadStatus::Read;
			}
		return Phase4CatalogDefinitionReadStatus::MissingDefinition;
	}
};

TEST(TelemetryPhase4CatalogAssembly,
	CollectsUniqueShipClassesTheirWeaponsAndDirectProjectileExactly)
{
	FakeCatalogReader reader;
	auto class_two = ship_fragment(2U, {5U, 6U});
	class_two->ship_classes[0].countermeasure_weapon_source_key = 8U;
	class_two->weapons[class_two->weapon_count].source_key = 8U;
	class_two->weapons[class_two->weapon_count++].name = "weapon-8";
	reader.ship_definitions.push_back({2U, std::move(class_two)});
	reader.ship_definitions.push_back({9U, ship_fragment(9U, {7U})});
	reader.weapon_definitions.push_back({17U, weapon_fragment(17U)});
	const std::vector<Phase4EngineInventoryEntry> inventory{
		inventory_entry(101U, 1U, ObjectType::Ship, 9U),
		inventory_entry(102U, 2U, ObjectType::Ship, 2U),
		inventory_entry(103U, 3U, ObjectType::Ship, 2U),
		inventory_entry(104U, 4U, ObjectType::Weapon, 17U),
	};
	auto output = std::make_unique<Phase2ManifestSource>();

	ASSERT_EQ(Phase4CatalogAssemblyStatus::Created,
		assemble_phase4_catalog_definitions(reader, inventory, *output));
	EXPECT_EQ(3U, reader.ship_reads);
	EXPECT_EQ(1U, reader.weapon_reads);
	ASSERT_EQ(2U, output->ship_class_count);
	EXPECT_EQ(2U, output->ship_classes[0].source_key);
	EXPECT_EQ(9U, output->ship_classes[1].source_key);
	ASSERT_EQ(5U, output->weapon_count);
	EXPECT_EQ(5U, output->weapons[0].source_key);
	EXPECT_EQ(6U, output->weapons[1].source_key);
	EXPECT_EQ(8U, output->weapons[2].source_key);
	EXPECT_EQ(7U, output->weapons[3].source_key);
	EXPECT_EQ(17U, output->weapons[4].source_key);
	ASSERT_EQ(2U, output->referenced_ship_class_count);
	EXPECT_EQ(2U, output->referenced_ship_class_keys[0]);
	EXPECT_EQ(9U, output->referenced_ship_class_keys[1]);
	ASSERT_EQ(5U, output->referenced_weapon_count);
}

TEST(TelemetryPhase4CatalogAssembly,
	RejectsTwoPublicVariantsSharingOneInventoryClassKey)
{
	class VariantReader final : public Phase4CatalogDefinitionReadView {
	  public:
		Phase4CatalogDefinitionReadStatus read_ship_definition(
			const Phase4EngineInventoryEntry& entry,
			Phase2ManifestSource& output) const noexcept override
		{
			output = *ship_fragment(2U, {5U});
			output.ship_classes[0].iff_index =
				entry.identity.stable_identity == 101U ? 1 : 2;
			return Phase4CatalogDefinitionReadStatus::Read;
		}
		Phase4CatalogDefinitionReadStatus read_weapon_definition(
			std::uint32_t, Phase2ManifestSource&) const noexcept override
		{
			return Phase4CatalogDefinitionReadStatus::MissingDefinition;
		}
	} reader;
	const std::vector<Phase4EngineInventoryEntry> inventory{
		inventory_entry(101U, 1U, ObjectType::Ship, 2U),
		inventory_entry(102U, 2U, ObjectType::Ship, 2U),
	};
	auto output = std::make_unique<Phase2ManifestSource>();
	output->ship_class_count = 1U;
	output->ship_classes[0].source_key = 700U;

	EXPECT_EQ(Phase4CatalogAssemblyStatus::AmbiguousDefinition,
		assemble_phase4_catalog_definitions(reader, inventory, *output));
	EXPECT_EQ(700U, output->ship_classes[0].source_key);
}

TEST(TelemetryPhase4CatalogAssembly,
	RejectsConflictingWeaponDefinitionWithoutPublishing)
{
	FakeCatalogReader reader;
	auto first = ship_fragment(2U, {5U});
	auto second = ship_fragment(9U, {5U});
	second->weapons[0].damage = 99.0F;
	reader.ship_definitions.push_back({2U, std::move(first)});
	reader.ship_definitions.push_back({9U, std::move(second)});
	const std::vector<Phase4EngineInventoryEntry> inventory{
		inventory_entry(101U, 1U, ObjectType::Ship, 2U),
		inventory_entry(102U, 2U, ObjectType::Ship, 9U),
	};
	auto output = std::make_unique<Phase2ManifestSource>();
	output->ship_class_count = 1U;
	output->ship_classes[0].source_key = 777U;

	EXPECT_EQ(Phase4CatalogAssemblyStatus::AmbiguousDefinition,
		assemble_phase4_catalog_definitions(reader, inventory, *output));
	ASSERT_EQ(1U, output->ship_class_count);
	EXPECT_EQ(777U, output->ship_classes[0].source_key);
}

TEST(TelemetryPhase4CatalogAssembly,
	RejectsMissingDirectProjectileDefinitionWithoutPublishing)
{
	FakeCatalogReader reader;
	const std::vector<Phase4EngineInventoryEntry> inventory{
		inventory_entry(104U, 4U, ObjectType::Weapon, 17U),
	};
	auto output = std::make_unique<Phase2ManifestSource>();
	output->weapon_count = 1U;
	output->weapons[0].source_key = 888U;

	EXPECT_EQ(Phase4CatalogAssemblyStatus::MissingDefinition,
		assemble_phase4_catalog_definitions(reader, inventory, *output));
	ASSERT_EQ(1U, output->weapon_count);
	EXPECT_EQ(888U, output->weapons[0].source_key);
}

TEST(TelemetryPhase4CatalogAssembly,
	RejectsMismatchedClassDefinitionWithoutPublishing)
{
	FakeCatalogReader reader;
	reader.ship_definitions.push_back({2U, ship_fragment(3U, {5U})});
	const std::vector<Phase4EngineInventoryEntry> inventory{
		inventory_entry(101U, 1U, ObjectType::Ship, 2U),
	};
	auto output = std::make_unique<Phase2ManifestSource>();
	output->ship_class_count = 1U;
	output->ship_classes[0].source_key = 889U;

	EXPECT_EQ(Phase4CatalogAssemblyStatus::InvalidDefinition,
		assemble_phase4_catalog_definitions(reader, inventory, *output));
	ASSERT_EQ(1U, output->ship_class_count);
	EXPECT_EQ(889U, output->ship_classes[0].source_key);
}

TEST(TelemetryPhase4CatalogAssembly,
	RejectsTheSixtyFifthShipClassBeforeReadingOrPublishing)
{
	FakeCatalogReader reader;
	std::vector<Phase4EngineInventoryEntry> inventory;
	for (std::uint32_t key = 1U; key <= 65U; ++key)
		inventory.push_back(inventory_entry(100U + key, key,
			ObjectType::Ship, key));
	auto output = std::make_unique<Phase2ManifestSource>();
	output->ship_class_count = 1U;
	output->ship_classes[0].source_key = 999U;

	EXPECT_EQ(Phase4CatalogAssemblyStatus::SourceLimitExceeded,
		assemble_phase4_catalog_definitions(reader, inventory, *output));
	EXPECT_EQ(0U, reader.ship_reads);
	ASSERT_EQ(1U, output->ship_class_count);
	EXPECT_EQ(999U, output->ship_classes[0].source_key);
}

} // namespace
