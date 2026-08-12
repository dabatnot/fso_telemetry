#pragma once

#include "telemetry/phase4_entity_projection.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <cstddef>
#include <array>
#include <memory>
#include <vector>

namespace telemetry::detail {

enum class Phase4EntityImageStatus : std::uint8_t {
	Created = 0,
	EmptyInventory,
	TooManyEntities,
	DuplicateEntityId,
	UnknownParent,
	ParentCycle,
	InvalidEntity,
	AllocationFailure,
	Count,
};

class Phase4StateImagePool final {
  public:
	static constexpr std::size_t SlotCount = 4U;

	Phase4StateImagePool() = default;
	Phase4StateImagePool(const Phase4StateImagePool&) = delete;
	Phase4StateImagePool& operator=(const Phase4StateImagePool&) = delete;

	bool provision(std::size_t maximum_entities) noexcept;
	void reset() noexcept;
	bool ready() const noexcept { return m_ready; }
	std::size_t maximum_entities() const noexcept { return m_maximum_entities; }
	std::size_t owned_backing_bytes() const noexcept;

  private:
	friend Phase4EntityImageStatus build_phase4_entity_image_preallocated(
		const std::vector<Phase4EntityProjectionInput>&,
		Phase4StateImagePool&, protocol::StateImage&) noexcept;

	struct Slot {
		std::shared_ptr<std::vector<protocol::StateAtom>> records;
		std::vector<protocol::StateAtom> spares;
		std::vector<std::size_t> order;
		std::size_t spare_count = 0U;
	};

	Slot* acquire() noexcept;
	bool prepare(Slot&, std::size_t) noexcept;
	std::array<Slot, SlotCount> m_slots{};
	std::size_t m_maximum_entities = 0U;
	bool m_ready = false;
};

// Builds an unpublished, complete entity image. On failure, output is left
// unchanged; callers can therefore retain the last ACKed baseline.
Phase4EntityImageStatus build_phase4_entity_image(
	const std::vector<Phase4EntityProjectionInput>& entities,
	protocol::StateImage& output);

// Runtime path: all backing is provisioned before capture. Capacity or slot
// exhaustion fails closed and leaves output unchanged.
Phase4EntityImageStatus build_phase4_entity_image_preallocated(
	const std::vector<Phase4EntityProjectionInput>& entities,
	Phase4StateImagePool& pool,
	protocol::StateImage& output) noexcept;

} // namespace telemetry::detail
