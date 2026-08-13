#pragma once

#include "telemetry/phase4_docking_projection.h"
#include "telemetry/phase4_engine_inventory.h"

#include <cstdint>
#include <vector>

namespace telemetry::detail {

enum class Phase4DockingCollectorStatus : std::uint8_t {
	Collected = 0, NotReady, InvalidSource, UnknownEndpoint, CapacityExceeded, Count,
};

// Reads the authoritative, reciprocal object dock lists on the main thread.
// The output is replaced only after the entire graph has been validated.
Phase4DockingCollectorStatus collect_phase4_docking_relations(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	std::vector<Phase4DockingRelation>& output) noexcept;

} // namespace telemetry::detail
