#pragma once

#include "telemetry/phase4_entity_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry::detail {

// The engine owns at most MAX_OBJECTS live object slots. Keep this ABI-free
// public-to-telemetry bound independent from the cumulative session identity
// registry, which deliberately has a larger lifetime capacity.
constexpr std::size_t Phase4MaximumLiveEntities = 5'000U;

// Engine-derived, but ABI-free, inventory row.  The source class key is an
// internal catalogue lookup key, not a public class_id and is never serialized.
struct Phase4EngineInventoryEntry {
	Phase4EntityIdentityKey identity{};
	std::uint64_t entity_id = 0U;
	std::uint32_t source_class_key = 0U;
	std::array<float, 3U> position_world{};
	std::array<float, 4U> orientation_local_to_world{{0.0F, 0.0F, 0.0F, 1.0F}};
	std::array<float, 3U> velocity_world{};
	std::array<float, 3U> rotational_velocity_local{};
	float radius = 0.0F;
	std::uint32_t physics_mode_flags = 0U;
	bool static_marker = false;
};

enum class Phase4EngineInventoryStatus : std::uint8_t {
	Collected = 0,
	NotReady,
	SourceLimitExceeded,
	InsufficientStorage,
	InvalidSource,
	IdentityFailure,
	Count,
};

// Reads the engine's authoritative used-object list on the main thread.  The
// caller owns a pre-reserved output workspace; this function never grows it.
// It intentionally stops before public class IDs and StateAtoms: the following
// catalogue stage must install those definitions before an image can publish.
Phase4EngineInventoryStatus collect_phase4_engine_inventory(
	Phase4EntityRegistry& identities,
	std::vector<Phase4EngineInventoryEntry>& output) noexcept;

} // namespace telemetry::detail
