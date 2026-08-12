#pragma once

#include <cstdint>

namespace telemetry::detail {

enum class Phase4PublicationState : std::uint8_t {
	AwaitingManifest = 0,
	ReadyForSnapshot,
	AwaitingSnapshotApplied,
	ReadyForDeltas,
	Faulted,
};

enum class Phase4PublicationResult : std::uint8_t {
	Accepted = 0,
	NotReady,
	InvalidManifest,
	InvalidSnapshot,
	Faulted,
};

// Enforces the TrustedFullState ordering at the producer boundary. It has no
// transport side effects: the runtime owns the actual manifest/snapshot I/O.
class Phase4PublicationGate final {
  public:
	Phase4PublicationResult require_manifest(std::uint32_t manifest_id) noexcept;
	Phase4PublicationResult on_manifest_applied(std::uint32_t manifest_id) noexcept;
	Phase4PublicationResult begin_snapshot(std::uint64_t snapshot_id) noexcept;
	Phase4PublicationResult on_snapshot_applied(std::uint64_t snapshot_id,
		std::uint32_t manifest_id) noexcept;
	Phase4PublicationResult permit_delta() const noexcept;
	void fault() noexcept;
	void reset() noexcept;

	Phase4PublicationState state() const noexcept { return m_state; }
	std::uint32_t required_manifest_id() const noexcept { return m_required_manifest_id; }
	std::uint64_t pending_snapshot_id() const noexcept { return m_pending_snapshot_id; }

  private:
	Phase4PublicationState m_state = Phase4PublicationState::AwaitingManifest;
	std::uint32_t m_required_manifest_id = 0U;
	std::uint64_t m_pending_snapshot_id = 0U;
};

} // namespace telemetry::detail
