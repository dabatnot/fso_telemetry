#pragma once

#include "telemetry/phase1_state_image.h"
#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase2_observation.h"
#include "telemetry/phase2_profile_gate.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace telemetry {

constexpr std::uint64_t Phase2CoreGateCoverage =
	protocol::StateDomainCoverageBitPlayerKinematics | protocol::StateDomainCoverageBitCoreShip;

constexpr std::size_t phase2_core_gate_record_count(std::size_t subsystem_count) noexcept
{
	return subsystem_count > static_cast<std::size_t>(-1) - 9U
		? static_cast<std::size_t>(-1)
		: 9U + subsystem_count;
}

struct Phase2CoreGateStateImageInput {
	std::uint64_t producer_id = 0U;
	std::uint32_t negotiated_capability_generation = 0U;
	protocol::SessionPhase session_phase = protocol::SessionPhase::Synchronizing;
	detail::MissionObservationDto mission{};
	std::uint64_t player_entity_id = 0U;
	const detail::Phase2ObservationDto* observation = nullptr;
	const Phase2ManifestCandidate* installed_manifest = nullptr;
	std::uint32_t required_manifest_id = 0U;
	bool manifest_applied = false;
};

enum class Phase2StateImageBuildStatus : std::uint8_t {
	Created = 0,
	InvalidInput,
	ManifestUnavailable,
	SourceMappingMissing,
	CapacityExceeded,
	AllocationFailed,
	Count,
};

enum class Phase2StateImageBuildStage : std::uint8_t {
	None = 0,
	Prepare,
	Resolve,
	FillBusiness,
	Adopt,
	Count,
};

struct Phase2StateImageBuildDiagnostic {
	Phase2StateImageBuildStage stage = Phase2StateImageBuildStage::None;
	protocol::ValidationError business_error =
		protocol::ValidationError::None;
	protocol::StateImageInvalidRecordReason structural_reason =
		protocol::StateImageInvalidRecordReason::None;
	std::uint16_t record_type = 0U;
	std::size_t record_index = static_cast<std::size_t>(-1);
};

class Phase2StateImagePool final {
  public:
	static constexpr std::size_t SlotCount = 4U;

	Phase2StateImagePool() = default;
	Phase2StateImagePool(const Phase2StateImagePool&) = delete;
	Phase2StateImagePool& operator=(const Phase2StateImagePool&) = delete;

	bool provision(std::size_t maximum_subsystems) noexcept;
	void reset() noexcept;
	bool ready() const noexcept { return m_ready; }
	std::size_t maximum_subsystems() const noexcept { return m_maximum_subsystems; }
	std::size_t owned_backing_bytes() const noexcept;

  private:
	friend Phase2StateImageBuildStatus build_phase2_core_gate_state_image_preallocated(
		const Phase2CoreGateStateImageInput&, Phase2StateImagePool&, protocol::StateImage&) noexcept;

	struct Slot {
		std::shared_ptr<std::vector<protocol::StateAtom>> records;
		std::vector<protocol::StateAtom> spares;
		std::size_t spare_count = 0U;
	};

	Slot* acquire() noexcept;
	bool prepare(Slot& slot, std::size_t record_count) noexcept;
	std::array<Slot, SlotCount> m_slots{};
	std::size_t m_maximum_subsystems = 0U;
	bool m_ready = false;
};

Phase2StateImageBuildStatus build_phase2_core_gate_state_image(
	const Phase2CoreGateStateImageInput& input, protocol::StateImage& image) noexcept;

Phase2StateImageBuildStatus build_phase2_core_gate_state_image_preallocated(
	const Phase2CoreGateStateImageInput& input,
	Phase2StateImagePool& pool,
	protocol::StateImage& image) noexcept;

enum class Phase2ProjectionCaptureKind : std::uint8_t {
	Keyframe = 0,
	Delta,
	Count,
};

struct Phase2Wp05SubjectBinding {
	detail::Phase2CaptureLocalKey capture_key;
	std::uint64_t entity_id = 0U;
};

struct Phase2Wp05ProjectionInput {
	std::uint64_t player_entity_id = 0U;
	const detail::Phase2ObservationDto* observation = nullptr;
	const Phase2ManifestCandidate* installed_manifest = nullptr;
	// When empty, the explicitly identified player is the sole subject.
	// Otherwise every projected ship must have an exact capture-key/public-ID
	// binding, including the player.
	const Phase2Wp05SubjectBinding* subjects = nullptr;
	std::size_t subject_count = 0U;
	Phase2ProjectionCaptureKind capture_kind =
		Phase2ProjectionCaptureKind::Keyframe;
};

class Phase2Wp05ProjectionPool final {
  public:
	static constexpr std::size_t SlotCount = 4U;

	bool provision(std::size_t maximum_subsystems) noexcept;
	void reset() noexcept;
	bool ready() const noexcept { return m_ready; }
	std::size_t maximum_subsystems() const noexcept {
		return m_maximum_subsystems;
	}
	std::size_t owned_backing_bytes() const noexcept;

  private:
	friend Phase2StateImageBuildStatus build_phase2_wp05_projection_preallocated(
		const Phase2Wp05ProjectionInput&,
		Phase2Wp05ProjectionPool&,
		protocol::StateImage&) noexcept;

	struct Slot {
		std::shared_ptr<std::vector<protocol::StateAtom>> records;
		std::vector<protocol::StateAtom> spares;
		std::size_t spare_count = 0U;
	};

	Slot* acquire() noexcept;
	bool prepare(Slot& slot, std::size_t record_count) noexcept;
	std::array<Slot, SlotCount> m_slots{};
	std::size_t m_maximum_subsystems = 0U;
	bool m_ready = false;
};

Phase2StateImageBuildStatus build_phase2_wp05_projection(
	const Phase2Wp05ProjectionInput& input,
	protocol::StateImage& image) noexcept;

Phase2StateImageBuildStatus build_phase2_wp05_projection_preallocated(
	const Phase2Wp05ProjectionInput& input,
	Phase2Wp05ProjectionPool& pool,
	protocol::StateImage& image) noexcept;

struct Phase2Wp06WeaponProjectionInput {
	const detail::Phase2ObservationDto* observation = nullptr;
	const Phase2ManifestCandidate* installed_manifest = nullptr;
	const Phase2Wp05SubjectBinding* subjects = nullptr;
	std::size_t subject_count = 0U;
};

class Phase2Wp06WeaponProjectionPool final {
  public:
	static constexpr std::size_t SlotCount = 4U;

	bool provision(std::size_t maximum_subjects,
		std::size_t maximum_primary_banks,
		std::size_t maximum_secondary_banks) noexcept;
	void reset() noexcept;
	bool ready() const noexcept { return m_ready; }
	std::size_t owned_backing_bytes() const noexcept;

  private:
	friend Phase2StateImageBuildStatus
	build_phase2_wp06_weapon_projection_preallocated(
		const Phase2Wp06WeaponProjectionInput&,
		Phase2Wp06WeaponProjectionPool&,
		protocol::StateImage&) noexcept;

	struct Slot {
		std::shared_ptr<std::vector<protocol::StateAtom>> records;
		std::vector<protocol::StateAtom> spares;
		std::size_t spare_count = 0U;
	};
	Slot* acquire() noexcept;
	bool prepare(Slot&, std::size_t) noexcept;
	std::array<Slot, SlotCount> m_slots{};
	std::size_t m_maximum_subjects = 0U;
	std::size_t m_maximum_primary_banks = 0U;
	std::size_t m_maximum_secondary_banks = 0U;
	bool m_ready = false;
};

Phase2StateImageBuildStatus build_phase2_wp06_weapon_projection(
	const Phase2Wp06WeaponProjectionInput& input,
	protocol::StateImage& image) noexcept;

Phase2StateImageBuildStatus
build_phase2_wp06_weapon_projection_preallocated(
	const Phase2Wp06WeaponProjectionInput& input,
	Phase2Wp06WeaponProjectionPool& pool,
	protocol::StateImage& image) noexcept;

struct Phase2CompleteDomainInput {
	std::uint64_t producer_id = 1U;
	std::uint32_t negotiated_capability_generation = 1U;
	protocol::SessionPhase session_phase = protocol::SessionPhase::Synchronizing;
	detail::MissionObservationDto mission{};
	const detail::Phase2ObservationDto* observation = nullptr;
	// Ordinary cadence ticks retain the latest atom values for groups that
	// were not sampled on this tick. A keyframe sets both refresh flags.
	const protocol::StateImage* retained_state = nullptr;
	bool refresh_flight_controls = true;
	bool refresh_systems = true;
	const Phase2ManifestCandidate* installed_manifest = nullptr;
	const Phase2Wp05SubjectBinding* subjects = nullptr;
	std::size_t subject_count = 0U;
	std::uint64_t player_entity_id = 0U;
	detail::Phase2Wp07CleanupRing* cleanup_ring = nullptr;
	detail::Phase2Wp07SupportTerminalRing* support_terminal_ring = nullptr;
	detail::Phase2Wp07EpisodeLatches* episode_latches = nullptr;
	const detail::Phase2Wp07CleanupBatch* cleanup_batch = nullptr;
	std::size_t session_slot = 0U;
	std::uint32_t cargo_authority_generation = 0U;
	std::uint32_t expected_cargo_authority_generation = 0U;
	protocol::DockingPhase docking_phase_override = protocol::DockingPhase::None;
	std::optional<protocol::RecordType> omit_record_for_test;

	std::uint32_t cargo_authority_consume_count() const noexcept {
		return m_cargo_authority_consume_count;
	}
	std::uint64_t canonical_hash(const protocol::StateImage& image) const noexcept;

  private:
	friend Phase2StateImageBuildStatus build_phase2_complete_domain(
		const Phase2CompleteDomainInput&, protocol::StateImage&) noexcept;
	friend Phase2StateImageBuildStatus build_phase2_complete_domain_preallocated(
		const Phase2CompleteDomainInput&, class Phase2CompleteDomainPool&,
		protocol::StateImage&, Phase2StateImageBuildDiagnostic*,
		struct Phase2StateImageRebuildSet*) noexcept;
	friend Phase2StateImageBuildStatus
	build_phase2_complete_domain_patch_preallocated(
		const Phase2CompleteDomainInput&,
		class Phase2CompleteDomainPool&,
		protocol::StateImage&,
		struct Phase2StateImageRebuildSet&,
		Phase2StateImageBuildDiagnostic*) noexcept;
	mutable std::uint32_t m_last_consumed_cargo_generation = 0U;
	mutable std::uint32_t m_cargo_authority_consume_count = 0U;
};

constexpr std::size_t Phase2CompleteDomainMaximumRecords =
	4U + 10U * detail::MaximumPhase2ObservationShips +
	Phase2ManifestLimits::MaxAggregateSubsystems;

struct Phase2StateImageRebuildSet {
	std::array<std::uint16_t,
		Phase2CompleteDomainMaximumRecords> canonical_indices;
	std::size_t count = 0U;
	std::uint8_t patch_pool_slot = 0xffU;
	std::uint8_t patch_layout_mask = 0U;
	bool patch_applied = false;
	bool exhaustive = false;
};

class Phase2CompleteDomainPool final {
  public:
	static constexpr std::size_t SlotCount = 4U;
	bool provision(std::size_t maximum_subjects,
		std::size_t maximum_subsystems,
		std::size_t maximum_dock_relations,
		std::size_t maximum_support_latches) noexcept;
	void reset() noexcept;
	bool ready() const noexcept { return m_ready; }
	std::size_t owned_backing_bytes() const noexcept;

  private:
	friend Phase2StateImageBuildStatus build_phase2_complete_domain_preallocated(
		const Phase2CompleteDomainInput&, Phase2CompleteDomainPool&,
		protocol::StateImage&, Phase2StateImageBuildDiagnostic*,
		Phase2StateImageRebuildSet*) noexcept;
	friend Phase2StateImageBuildStatus
	build_phase2_complete_domain_patch_preallocated(
		const Phase2CompleteDomainInput&,
		Phase2CompleteDomainPool&,
		protocol::StateImage&,
		Phase2StateImageRebuildSet&,
		Phase2StateImageBuildDiagnostic*) noexcept;
	friend bool rollback_phase2_complete_domain_patch_preallocated(
		Phase2CompleteDomainPool&, protocol::StateImage&,
		Phase2StateImageRebuildSet&) noexcept;
	struct Slot {
		std::shared_ptr<std::vector<protocol::StateAtom>> records;
		std::vector<protocol::StateAtom> spares;
		std::size_t spare_count = 0U;
		std::size_t patch_layout_record_count = 0U;
		std::size_t patch_mapping_count = 0U;
		std::array<std::uint16_t,
			Phase2CompleteDomainMaximumRecords>
			patch_canonical_indices{};
		std::array<std::uint16_t,
			Phase2CompleteDomainMaximumRecords>
			patch_source_indices{};
		std::uint8_t patch_layout_mask = 0U;
		bool patch_layout_ready = false;
	};
	std::array<Slot, SlotCount> m_slots{};
	std::size_t m_maximum_subjects = 0U;
	std::size_t m_maximum_subsystems = 0U;
	std::size_t m_maximum_dock_relations = 0U;
	std::size_t m_maximum_support_latches = 0U;
	std::size_t m_owned_backing_bytes = 0U;
	bool m_ready = false;
};

Phase2StateImageBuildStatus build_phase2_complete_domain(
	const Phase2CompleteDomainInput& input,
	protocol::StateImage& image) noexcept;
Phase2StateImageBuildStatus build_phase2_complete_domain_preallocated(
	const Phase2CompleteDomainInput& input,
	Phase2CompleteDomainPool& pool,
	protocol::StateImage& image,
	Phase2StateImageBuildDiagnostic* diagnostic = nullptr,
	Phase2StateImageRebuildSet* rebuilt = nullptr) noexcept;
Phase2StateImageBuildStatus
build_phase2_complete_domain_patch_preallocated(
	const Phase2CompleteDomainInput& input,
	Phase2CompleteDomainPool& pool,
	protocol::StateImage& image,
	Phase2StateImageRebuildSet& rebuilt,
	Phase2StateImageBuildDiagnostic* diagnostic = nullptr) noexcept;
bool rollback_phase2_complete_domain_patch_preallocated(
	Phase2CompleteDomainPool& pool,
	protocol::StateImage& image,
	Phase2StateImageRebuildSet& rebuilt) noexcept;

} // namespace telemetry
