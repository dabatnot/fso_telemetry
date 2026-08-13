#pragma once

#include "telemetry/engine_adapter.h"
#include "telemetry/phase4_engine_inventory.h"
#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <cstdint>
#include <vector>

namespace telemetry::detail {

enum class Phase4ShipCollectorStatus : std::uint8_t {
	Collected = 0, NotReady, MissingManifest, InvalidInventory, ReadFailure,
	ProjectionFailure, CapacityExceeded, Count,
};

// Unlike the Phase 2 player closure, this iterates every SHIP in the Phase 4
// inventory. Output changes only after every source ship has been projected.
Phase4ShipCollectorStatus collect_phase4_ship_records(
	const FsoEngineReadView& engine,
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	std::uint64_t sample_time_us,
	std::vector<protocol::StateAtom>& output) noexcept;

} // namespace telemetry::detail
