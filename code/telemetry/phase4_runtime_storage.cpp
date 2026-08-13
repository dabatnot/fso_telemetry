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
		m_inherited_ship_records.reserve(Phase4MaximumLiveEntities * 8U);
	} catch (const std::bad_alloc&) {
		reset();
		return false;
	}
	if (!m_catalog.provision()) {
		reset();
		return false;
	}
	constexpr auto manifest_bytes = protocol::MaxTransactionSize * 2U;
	m_manifest_backing.reset(new (std::nothrow) std::uint8_t[manifest_bytes]);
	if (!m_manifest_backing) { reset(); return false; }
	m_manifest_storage.reset(new (std::nothrow) Phase2ManifestStorage(
		protocol::MutableByteView{m_manifest_backing.get(), manifest_bytes}));
	m_manifest_slot.reset(new (std::nothrow) Phase2ManifestSlot(*m_manifest_storage));
	if (!m_manifest_storage || !m_manifest_slot) { reset(); return false; }
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
	std::vector<protocol::StateAtom>().swap(m_inherited_ship_records);
	m_manifest_slot.reset();
	m_manifest_storage.reset();
	m_manifest_backing.reset();
	m_current_manifest = nullptr;
	m_candidate_image = {};
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
	m_inherited_ship_records.clear();
	m_current_manifest = nullptr;
	m_candidate_image = {};
	m_dependencies = {};
	m_catalog.reset_cycle();
}

std::size_t Phase4RuntimeStorage::owned_backing_bytes() const noexcept
{
	std::size_t total = shared_owned_backing_bytes();
	for (std::size_t slot = 0U; slot < m_client_count; ++slot)
		total += client_owned_backing_bytes(slot);
	return total;
}

std::size_t Phase4RuntimeStorage::shared_owned_backing_bytes() const noexcept
{
	return m_identities.provisioned_bytes() +
		m_inventory.capacity() * sizeof(Phase4EngineInventoryEntry) +
		m_projections.capacity() * sizeof(Phase4EntityProjectionInput) +
		m_docking_relations.capacity() * sizeof(Phase4DockingRelation) +
		m_inherited_ship_records.capacity() * sizeof(protocol::StateAtom) +
		(m_manifest_backing ? protocol::MaxTransactionSize * 2U : 0U) +
		(m_manifest_storage ? sizeof(*m_manifest_storage) : 0U) +
		(m_manifest_slot ? sizeof(*m_manifest_slot) : 0U) +
		m_catalog.owned_backing_bytes();
}

const Phase2ManifestCandidate* Phase4RuntimeStorage::rebuild_local_manifest(
	const Phase2ManifestSource& source) noexcept
{
	if (!m_ready || !m_manifest_slot) return nullptr;
	const auto result = m_manifest_slot->rebuild(source);
	if (result == Phase2ManifestError::None ||
		result == Phase2ManifestError::RebuildCoalesced ||
		result == Phase2ManifestError::NoCatalogChange ||
		result == Phase2ManifestError::TopologyOnly) {
		if (m_manifest_slot->staged_manifest_id() != 0U) {
			m_current_manifest = &m_manifest_slot->staged_candidate();
			return m_current_manifest;
		}
		if (m_manifest_slot->active_manifest_id() != 0U) {
			m_current_manifest = &m_manifest_slot->active_candidate();
			return m_current_manifest;
		}
	}
	m_current_manifest = nullptr;
	return nullptr;
}

std::size_t Phase4RuntimeStorage::client_owned_backing_bytes(
	std::size_t client_slot) const noexcept
{
	return m_ready && client_slot < m_client_count
		? m_image_pools[client_slot].owned_backing_bytes() : 0U;
}

Phase4StateImagePool* Phase4RuntimeStorage::image_pool(
	std::size_t client_slot) noexcept
{
	return m_ready && client_slot < m_client_count
		? &m_image_pools[client_slot] : nullptr;
}

} // namespace telemetry::detail
