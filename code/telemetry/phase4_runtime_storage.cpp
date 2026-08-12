#include "telemetry/phase4_runtime_storage.h"

#include <new>
#include <utility>

namespace telemetry::detail {

bool Phase4RuntimeStorage::provision(std::size_t client_count) noexcept
{
	reset();
	if (client_count == 0U || client_count > m_image_pools.size())
		return false;
	const auto identity_status = m_identities.provision();
	if (identity_status != Phase4EntityRegistryStatus::Allocated &&
		identity_status != Phase4EntityRegistryStatus::Existing)
		return false;
	try {
		m_inventory.reserve(Phase4MaximumLiveEntities);
		m_projections.reserve(Phase4MaximumLiveEntities);
		m_docking_relations.reserve(Phase4MaximumDockingRelations);
	} catch (const std::bad_alloc&) {
		reset();
		return false;
	}
	if (!m_catalog.provision()) {
		reset();
		return false;
	}
	for (std::size_t slot = 0U; slot < client_count; ++slot)
		if (!m_image_pools[slot].provision(Phase4MaximumLiveEntities)) {
			reset();
			return false;
		}
	m_client_count = client_count;
	m_ready = true;
	return true;
}

void Phase4RuntimeStorage::reset() noexcept
{
	m_identities.reset_session();
	std::vector<Phase4EngineInventoryEntry>().swap(m_inventory);
	std::vector<Phase4EntityProjectionInput>().swap(m_projections);
	std::vector<Phase4DockingRelation>().swap(m_docking_relations);
	m_dependencies = {};
	m_catalog.reset();
	for (auto& pool : m_image_pools) pool.reset();
	m_client_count = 0U;
	m_ready = false;
}

void Phase4RuntimeStorage::reset_mission() noexcept
{
	if (!m_ready) return;
	m_identities.reset_mission();
	m_inventory.clear();
	m_projections.clear();
	m_docking_relations.clear();
	m_dependencies = {};
	m_catalog.reset_cycle();
}

std::size_t Phase4RuntimeStorage::owned_backing_bytes() const noexcept
{
	std::size_t total = m_identities.provisioned_bytes() +
		m_inventory.capacity() * sizeof(Phase4EngineInventoryEntry) +
		m_projections.capacity() * sizeof(Phase4EntityProjectionInput) +
		m_docking_relations.capacity() * sizeof(Phase4DockingRelation) +
		m_catalog.owned_backing_bytes();
	for (std::size_t slot = 0U; slot < m_client_count; ++slot)
		total += m_image_pools[slot].owned_backing_bytes();
	return total;
}

Phase4StateImagePool* Phase4RuntimeStorage::image_pool(
	std::size_t client_slot) noexcept
{
	return m_ready && client_slot < m_client_count
		? &m_image_pools[client_slot] : nullptr;
}

} // namespace telemetry::detail
