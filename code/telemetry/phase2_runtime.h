#pragma once

#include "telemetry/phase2_observation.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

enum class Phase2RuntimeResult : std::uint8_t {
	Applied = 0,
	NoChange,
	ManifestRequired,
	SnapshotRequired,
	CandidateBusy,
	Stale,
	InvalidInput,
	CapacityExceeded,
	CounterExhausted,
	Count,
};

enum class Phase2RuntimeSnapshotCause : std::uint8_t {
	None = 0,
	Initial,
	Periodic,
	Topology,
	Catalog,
	Lifecycle,
	SupportTerminal,
	DeltaCapacity,
	Resync,
	Count,
};

struct Phase2RuntimeSnapshotPlan {
	std::uint32_t snapshot_id = 0U;
	std::uint32_t required_manifest_id = 0U;
	std::uint16_t flags = protocol::SnapshotFlagNone;
	Phase2RuntimeSnapshotCause cause =
		Phase2RuntimeSnapshotCause::None;
};

struct Phase2RuntimeManifestState {
	std::uint32_t active_id = 0U;
	std::uint32_t staged_id = 0U;
	bool staged_applied = false;
	bool rebuild_intent = false;
};

enum class Phase2RuntimeLifecycleEventKind : std::uint8_t {
	Appeared = 0,
	Disabled,
	DyingStarted,
	Destroyed,
	Disappeared,
	Count,
};

struct Phase2RuntimeLifecycleFact {
	std::uint64_t event_id = 0U;
	std::uint64_t dependency_snapshot_id = 0U;
	std::uint64_t entity_id = 0U;
	std::uint64_t sample_time_us = 0U;
	std::uint32_t source_signature = 0U;
	std::uint32_t generation = 0U;
	Phase2RuntimeLifecycleEventKind kind =
		Phase2RuntimeLifecycleEventKind::Appeared;
};

class Phase2RuntimeSlot final {
  public:
	static constexpr std::size_t ClosureCapacity =
		MaximumPhase2ObservationShips;
	static constexpr std::size_t LifecycleCapacity =
		ClosureCapacity * 5U;
	static constexpr std::size_t BlockCount = 8U;

	bool configure(std::size_t session_slot) noexcept;
	void reset() noexcept;

	Phase2RuntimeResult reconcile_closure(
		const Phase2CaptureLocalKey* signatures,
		std::size_t count,
		Phase2Wp05SubjectBinding* bindings,
		std::size_t binding_capacity) noexcept;
	Phase2RuntimeResult reconcile_closure_with_public_ids(
		const Phase2CaptureLocalKey* identity_signatures,
		const Phase2CaptureLocalKey* binding_keys,
		const std::uint64_t* public_entity_ids,
		std::size_t count,
		Phase2Wp05SubjectBinding* bindings,
		std::size_t binding_capacity) noexcept;
	Phase2RuntimeResult stage_manifest(
		std::uint32_t manifest_id,
		const protocol::Sha256Digest& catalog_fingerprint) noexcept;
	Phase2RuntimeResult preview_stage_manifest(
		std::uint32_t manifest_id,
		const protocol::Sha256Digest& catalog_fingerprint) const noexcept;
	Phase2RuntimeResult observe_topology(
		const protocol::Sha256Digest& topology_fingerprint) noexcept;
	Phase2RuntimeResult observe_lifecycle(
		const Phase2ObservationDto& observation,
		const Phase2Wp05SubjectBinding* bindings,
		std::size_t binding_count) noexcept;
	bool set_current_block_samples(
		const std::array<std::uint64_t, BlockCount>& samples) noexcept;
	Phase2RuntimeResult on_manifest_applied(
		std::uint32_t manifest_id) noexcept;
	Phase2RuntimeResult request_snapshot(
		Phase2RuntimeSnapshotCause cause) noexcept;
	Phase2RuntimeResult apply_global_events(
		const Phase2Wp07GlobalEventBatch& batch) noexcept;
	Phase2RuntimeResult preview_global_events(
		const Phase2Wp07GlobalEventBatch& batch) const noexcept;
	Phase2RuntimeResult next_snapshot_plan(
		Phase2RuntimeSnapshotPlan& plan) noexcept;
	Phase2RuntimeResult on_snapshot_started(
		const Phase2RuntimeSnapshotPlan& plan) noexcept;
	Phase2RuntimeResult on_snapshot_applied(
		std::uint32_t snapshot_id,
		std::uint32_t manifest_id) noexcept;
	Phase2RuntimeResult on_snapshot_abandoned(
		std::uint32_t snapshot_id) noexcept;

	Phase2RuntimeResult classify_delta_size(
		std::size_t encoded_size) noexcept;
	bool can_emit_delta() const noexcept;
	bool consume_rebuild_intent() noexcept;

	const Phase2RuntimeManifestState& manifest_state() const noexcept {
		return m_manifest;
	}
	const Phase2Wp07EpisodeLatches& support_latches() const noexcept {
		return m_support_latches;
	}
	std::size_t pending_support_count() const noexcept {
		return m_pending_support_count;
	}
	Phase2Wp07EpisodeLatches& support_latches() noexcept {
		return m_support_latches;
	}
	const Phase2Wp07CleanupBatch& cleanup_batch() const noexcept {
		return m_cleanup_batch;
	}
	std::uint32_t active_snapshot_id() const noexcept {
		return m_active_snapshot_id;
	}
	Phase2RuntimeSnapshotCause pending_snapshot_cause() const noexcept {
		return m_pending_cause;
	}
	std::size_t closure_size() const noexcept { return m_entry_count; }
	std::uint64_t started_snapshot_sequence() const noexcept {
		return m_started_snapshot_sequence;
	}
	Phase2RuntimeSnapshotCause last_started_snapshot_cause() const noexcept {
		return m_last_started_snapshot_cause;
	}
	std::size_t pending_lifecycle_count() const noexcept {
		return m_pending_lifecycle_count;
	}
	const Phase2RuntimeLifecycleFact& pending_lifecycle(
		std::size_t index) const noexcept {
		return m_pending_lifecycle[index];
	}
	std::uint64_t active_block_sample(std::size_t block) const noexcept {
		return block < m_active_block_samples.size()
			? m_active_block_samples[block] : 0U;
	}

  private:
	struct EntityEntry {
		std::uint32_t signature = 0U;
		std::uint64_t entity_id = 0U;
		ShipLifecycleState lifecycle = ShipLifecycleState::Count;
		std::uint32_t lifecycle_flags = 0U;
		std::uint32_t generation = 0U;
		bool active = false;
	};

	static std::uint16_t snapshot_flags(
		Phase2RuntimeSnapshotCause cause) noexcept;
	bool has_catalog(
		const protocol::Sha256Digest& fingerprint) const noexcept;
	bool append_lifecycle(Phase2RuntimeLifecycleEventKind kind,
		const EntityEntry& entry, std::uint64_t sample_time_us) noexcept;

	std::array<EntityEntry, ClosureCapacity> m_entries{};
	std::size_t m_entry_count = 0U;
	std::uint64_t m_next_entity_id = 1U;
	std::uint32_t m_next_lifecycle_generation = 1U;
	std::uint32_t m_next_snapshot_id = 1U;
	std::uint32_t m_active_snapshot_id = 0U;
	std::uint32_t m_candidate_snapshot_id = 0U;
	std::uint32_t m_candidate_manifest_id = 0U;
	Phase2RuntimeSnapshotCause m_pending_cause =
		Phase2RuntimeSnapshotCause::None;
	Phase2RuntimeManifestState m_manifest{};
	protocol::Sha256Digest m_active_catalog{};
	protocol::Sha256Digest m_staged_catalog{};
	protocol::Sha256Digest m_topology{};
	Phase2Wp07EpisodeLatches m_support_latches{};
	Phase2Wp07EpisodeLatches m_candidate_support_latches{};
	std::array<SupportTransitionFact,
		Phase2Wp07SupportTerminalRing::Capacity> m_pending_support{};
	std::array<SupportTransitionFact,
		Phase2Wp07SupportTerminalRing::Capacity> m_candidate_support{};
	std::size_t m_pending_support_count = 0U;
	std::size_t m_candidate_support_count = 0U;
	Phase2Wp07CleanupBatch m_cleanup_batch{};
	Phase2Wp07CleanupBatch m_candidate_cleanup_batch{};
	std::array<Phase2RuntimeLifecycleFact, LifecycleCapacity>
		m_pending_lifecycle{};
	std::array<Phase2RuntimeLifecycleFact, LifecycleCapacity>
		m_candidate_lifecycle{};
	std::size_t m_pending_lifecycle_count = 0U;
	std::size_t m_candidate_lifecycle_count = 0U;
	std::uint64_t m_next_event_id = 1U;
	std::uint64_t m_started_snapshot_sequence = 0U;
	Phase2RuntimeSnapshotCause m_last_started_snapshot_cause =
		Phase2RuntimeSnapshotCause::None;
	std::array<std::uint64_t, BlockCount> m_current_block_samples{};
	std::array<std::uint64_t, BlockCount> m_candidate_block_samples{};
	std::array<std::uint64_t, BlockCount> m_active_block_samples{};
	std::size_t m_session_slot = 0U;
	bool m_configured = false;
};

} // namespace telemetry::detail
