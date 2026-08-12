#include "telemetry/phase4_publication_gate.h"

namespace telemetry::detail {

Phase4PublicationResult Phase4PublicationGate::require_manifest(std::uint32_t manifest_id) noexcept
{
	if (m_state == Phase4PublicationState::Faulted) return Phase4PublicationResult::Faulted;
	if (manifest_id == 0U) return Phase4PublicationResult::InvalidManifest;
	m_required_manifest_id = manifest_id;
	m_pending_snapshot_id = 0U;
	m_state = Phase4PublicationState::AwaitingManifest;
	return Phase4PublicationResult::Accepted;
}

Phase4PublicationResult Phase4PublicationGate::on_manifest_applied(std::uint32_t manifest_id) noexcept
{
	if (m_state == Phase4PublicationState::Faulted) return Phase4PublicationResult::Faulted;
	if (m_state != Phase4PublicationState::AwaitingManifest) return Phase4PublicationResult::NotReady;
	if (manifest_id == 0U || manifest_id != m_required_manifest_id) return Phase4PublicationResult::InvalidManifest;
	m_state = Phase4PublicationState::ReadyForSnapshot;
	return Phase4PublicationResult::Accepted;
}

Phase4PublicationResult Phase4PublicationGate::begin_snapshot(std::uint64_t snapshot_id) noexcept
{
	if (m_state == Phase4PublicationState::Faulted) return Phase4PublicationResult::Faulted;
	if (m_state != Phase4PublicationState::ReadyForSnapshot) return Phase4PublicationResult::NotReady;
	if (snapshot_id == 0U) return Phase4PublicationResult::InvalidSnapshot;
	m_pending_snapshot_id = snapshot_id;
	m_state = Phase4PublicationState::AwaitingSnapshotApplied;
	return Phase4PublicationResult::Accepted;
}

Phase4PublicationResult Phase4PublicationGate::on_snapshot_applied(std::uint64_t snapshot_id,
	std::uint32_t manifest_id) noexcept
{
	if (m_state == Phase4PublicationState::Faulted) return Phase4PublicationResult::Faulted;
	if (m_state != Phase4PublicationState::AwaitingSnapshotApplied) return Phase4PublicationResult::NotReady;
	if (manifest_id != m_required_manifest_id) return Phase4PublicationResult::InvalidManifest;
	if (snapshot_id == 0U || snapshot_id != m_pending_snapshot_id) return Phase4PublicationResult::InvalidSnapshot;
	m_pending_snapshot_id = 0U;
	m_state = Phase4PublicationState::ReadyForDeltas;
	return Phase4PublicationResult::Accepted;
}

Phase4PublicationResult Phase4PublicationGate::permit_delta() const noexcept
{
	return m_state == Phase4PublicationState::ReadyForDeltas
		? Phase4PublicationResult::Accepted
		: m_state == Phase4PublicationState::Faulted
			? Phase4PublicationResult::Faulted
			: Phase4PublicationResult::NotReady;
}

void Phase4PublicationGate::fault() noexcept
{
	m_pending_snapshot_id = 0U;
	m_state = Phase4PublicationState::Faulted;
}

void Phase4PublicationGate::reset() noexcept
{
	m_required_manifest_id = 0U;
	m_pending_snapshot_id = 0U;
	m_state = Phase4PublicationState::AwaitingManifest;
}

} // namespace telemetry::detail
