#pragma once

#include "telemetry/phase4_catalog_collector.h"
#include "telemetry/phase4_catalog_dependencies.h"
#include "telemetry/phase4_docking_projection.h"
#include "telemetry/phase4_entity_image.h"
#include "telemetry/phase4_engine_inventory.h"

#include <array>
#include <cstddef>
#include <vector>

namespace telemetry::detail {

constexpr std::size_t Phase4MaximumClients = 4U;
constexpr std::size_t Phase4MaximumDockingRelations = 64U;

// Owns every allocation needed by the first TrustedFullState runtime slice.
// It is provisioned before transport publication and only exposes bounded
// vectors to the main-thread capture path.
class Phase4RuntimeStorage final {
  public:
	bool provision(std::size_t client_count) noexcept;
	void reset() noexcept;
	void reset_mission() noexcept;
	bool ready() const noexcept { return m_ready; }
	std::size_t client_count() const noexcept { return m_client_count; }
	std::size_t owned_backing_bytes() const noexcept;

	Phase4EntityRegistry& identities() noexcept { return m_identities; }
	std::vector<Phase4EngineInventoryEntry>& inventory() noexcept {
		return m_inventory;
	}
	Phase4CatalogDependencies& dependencies() noexcept {
		return m_dependencies;
	}
	Phase4CatalogCollectorWorkspace& catalog() noexcept { return m_catalog; }
	std::vector<Phase4EntityProjectionInput>& projections() noexcept {
		return m_projections;
	}
	std::vector<Phase4DockingRelation>& docking_relations() noexcept {
		return m_docking_relations;
	}
	Phase4StateImagePool* image_pool(std::size_t client_slot) noexcept;

  private:
	Phase4EntityRegistry m_identities;
	std::vector<Phase4EngineInventoryEntry> m_inventory;
	Phase4CatalogDependencies m_dependencies{};
	Phase4CatalogCollectorWorkspace m_catalog;
	std::vector<Phase4EntityProjectionInput> m_projections;
	std::vector<Phase4DockingRelation> m_docking_relations;
	std::array<Phase4StateImagePool, Phase4MaximumClients> m_image_pools{};
	std::size_t m_client_count = 0U;
	bool m_ready = false;
};

} // namespace telemetry::detail
