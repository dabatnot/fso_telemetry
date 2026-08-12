#pragma once

#include "telemetry/engine_adapter.h"
#include "telemetry/phase4_catalog_assembly.h"

#include <memory>

namespace telemetry::detail {

class Phase4CatalogCollectorWorkspace final {
  public:
	bool provision() noexcept;
	void reset() noexcept;
	void reset_cycle() noexcept;
	bool ready() const noexcept;
	std::size_t owned_backing_bytes() const noexcept;

  private:
	friend Phase4CatalogAssemblyStatus
	collect_phase4_catalog_definitions_preallocated(
		const FsoEngineReadView&,
		const std::vector<Phase4EngineInventoryEntry>&,
		Phase4CatalogCollectorWorkspace&,
		const Phase2ManifestSource*&) noexcept;
	Phase4CatalogAssemblyWorkspace m_assembly;
	std::unique_ptr<Phase2ShipSource> m_ship_source;
	std::unique_ptr<Phase2ObservationDto> m_observation;
};

Phase4CatalogAssemblyStatus collect_phase4_catalog_definitions_preallocated(
	const FsoEngineReadView& engine,
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase4CatalogCollectorWorkspace& workspace,
	const Phase2ManifestSource*& output) noexcept;

} // namespace telemetry::detail
