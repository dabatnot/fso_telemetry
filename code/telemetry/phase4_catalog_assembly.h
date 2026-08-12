#pragma once

#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase4_engine_inventory.h"

#include <cstdint>
#include <memory>
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

class Phase4CatalogAssemblyWorkspace final {
  public:
	Phase4CatalogAssemblyWorkspace() = default;
	Phase4CatalogAssemblyWorkspace(const Phase4CatalogAssemblyWorkspace&) = delete;
	Phase4CatalogAssemblyWorkspace& operator=(const Phase4CatalogAssemblyWorkspace&) = delete;

	bool provision() noexcept;
	void reset() noexcept;
	void reset_cycle() noexcept;
	bool ready() const noexcept;
	std::size_t owned_backing_bytes() const noexcept;

  private:
	friend Phase4CatalogAssemblyStatus
	assemble_phase4_catalog_definitions_preallocated(
		const Phase4CatalogDefinitionReadView&,
		const std::vector<Phase4EngineInventoryEntry>&,
		Phase4CatalogAssemblyWorkspace&,
		const Phase2ManifestSource*&) noexcept;
	std::unique_ptr<Phase2ManifestSource> m_available;
	std::unique_ptr<Phase2ManifestSource> m_fragment;
};

// Runtime path. All storage must be provisioned before the capture tick.
// output is null on failure and otherwise borrows workspace until the next
// reset_cycle/assembly.
Phase4CatalogAssemblyStatus assemble_phase4_catalog_definitions_preallocated(
	const Phase4CatalogDefinitionReadView& reader,
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase4CatalogAssemblyWorkspace& workspace,
	const Phase2ManifestSource*& output) noexcept;

} // namespace telemetry::detail
