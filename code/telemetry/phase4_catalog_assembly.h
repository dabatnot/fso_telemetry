#pragma once

#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase4_engine_inventory.h"

#include <cstdint>
#include <vector>

namespace telemetry::detail {

enum class Phase4CatalogDefinitionReadStatus : std::uint8_t {
	Read = 0,
	NotReady,
	MissingDefinition,
	InvalidDefinition,
	SourceLimitExceeded,
	Count,
};

class Phase4CatalogDefinitionReadView {
  public:
	virtual ~Phase4CatalogDefinitionReadView() noexcept = default;
	virtual Phase4CatalogDefinitionReadStatus read_ship_definition(
		const Phase4EngineInventoryEntry& inventory_entry,
		Phase2ManifestSource& output) const noexcept = 0;
	virtual Phase4CatalogDefinitionReadStatus read_weapon_definition(
		std::uint32_t source_key,
		Phase2ManifestSource& output) const noexcept = 0;
};

enum class Phase4CatalogAssemblyStatus : std::uint8_t {
	Created = 0,
	NotReady,
	InvalidInventory,
	MissingDefinition,
	InvalidDefinition,
	AmbiguousDefinition,
	SourceLimitExceeded,
	AllocationFailure,
	Count,
};

// Builds the exact manifest source required by inventory. The reader may
// return all weapons referenced by a ship class; direct WEAPON rows are read
// separately. Output is replaced only after every definition, duplicate and
// FSTL bound has been validated.
Phase4CatalogAssemblyStatus assemble_phase4_catalog_definitions(
	const Phase4CatalogDefinitionReadView& reader,
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase2ManifestSource& output) noexcept;

} // namespace telemetry::detail
