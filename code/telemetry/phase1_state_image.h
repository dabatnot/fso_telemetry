#pragma once

#include "telemetry/engine_adapter.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <array>
#include <cstdint>
#include <memory>

namespace telemetry::detail {

struct MissionObservationDto {
	std::uint64_t producer_sample_time_us = 0U;
	std::uint32_t mission_generation = 0U;
	protocol::MissionPhase phase = protocol::MissionPhase::None;
	bool paused = false;
	float time_compression = 0.0F;
};

struct Phase1StateImageInput {
	std::uint64_t producer_id = 0U;
	std::uint32_t negotiated_capability_generation = 0U;
	protocol::SessionPhase session_phase = protocol::SessionPhase::Synchronizing;
	MissionObservationDto mission{};
	CaptureResult player_capture{};
	PlayerKinematicsSample player{};
};

// Optional, test-only timing sink. Normal capture passes nullptr and pays no
// timing cost; the P9.3 runner uses it to separate canonical record filling
// from immutable-image publication and semantic validation.
struct Phase1StateImageBuildTiming {
	std::uint64_t fill_records_ns = 0U;
	std::uint64_t publish_validate_ns = 0U;
	std::uint64_t adopt_preallocated_ns = 0U;
	std::uint64_t semantic_validate_ns = 0U;
};

enum class Phase1StateImageBuildStatus : std::uint8_t {
	Created = 0,
	InvalidInput,
	AllocationFailed,
	Count,
};

// P8 keeps the mutable construction storage separate from published images.
// A slot is only reused when the pool is its sole owner; the published
// StateImage interface remains immutable while current/active/candidate
// baselines retain their backing.
class Phase1StateImagePool final {
  public:
	static constexpr std::size_t SlotsPerRecordSet = 4U;
	// The provisioner creates four 2-record and four 4-record immutable
	// backings. This prices their explicit vector capacities, not the inline
	// Phase1StateImagePool object or allocator bookkeeping.
	static constexpr std::size_t BackingBytesPerClient =
		SlotsPerRecordSet *
			((sizeof(protocol::StateAtom) * 2U + 72U + 28U) +
				(sizeof(protocol::StateAtom) * 4U + 72U + 28U + 30U + 84U + sizeof(std::uint64_t) * 3U));

	Phase1StateImagePool() = default;
	Phase1StateImagePool(const Phase1StateImagePool&) = delete;
	Phase1StateImagePool& operator=(const Phase1StateImagePool&) = delete;

	// May allocate only during startup, before bind/Ready.
	bool provision() noexcept;
	void reset() noexcept;
	bool ready() const noexcept { return m_ready; }
	std::uint64_t successful_allocation_count() const noexcept { return m_successful_allocation_count; }
	// This is the sum of the explicitly reserved record and byte capacities.
	// Allocator bookkeeping is intentionally not represented as owned protocol
	// capacity, but every buffer that can grow on the capture path is.
	std::size_t owned_backing_bytes() const noexcept;

  private:
	friend Phase1StateImageBuildStatus build_phase1_state_image_preallocated(const Phase1StateImageInput& input,
		Phase1StateImagePool& pool,
		protocol::StateImage& image,
		Phase1StateImageBuildTiming* timing) noexcept;

	struct Slot {
		std::shared_ptr<std::vector<protocol::StateAtom>> records;
	};

	static bool provision_slot(Slot& slot, bool has_player, std::uint64_t& allocation_count) noexcept;
	Slot* acquire(bool has_player) noexcept;

	std::array<Slot, SlotsPerRecordSet> m_without_player{};
	std::array<Slot, SlotsPerRecordSet> m_with_player{};
	std::uint64_t m_successful_allocation_count = 0U;
	bool m_ready = false;
};

Phase1StateImageBuildStatus build_phase1_state_image(const Phase1StateImageInput& input,
	protocol::StateImage& image) noexcept;
// Uses startup-owned backing storage and performs no allocation on a
// successful or rejected capture. Exhausted in-use slots fail closed with
// AllocationFailed, preserving every immutable published image.
Phase1StateImageBuildStatus build_phase1_state_image_preallocated(const Phase1StateImageInput& input,
	Phase1StateImagePool& pool,
	protocol::StateImage& image,
	Phase1StateImageBuildTiming* timing = nullptr) noexcept;

} // namespace telemetry::detail
