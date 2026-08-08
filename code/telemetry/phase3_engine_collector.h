#pragma once

#include "telemetry/phase2_observation.h"
#include "telemetry/phase2_state_image.h"
#include "telemetry/phase3_identity_registry.h"
#include "telemetry/phase3_state_image.h"

#include <cstddef>
#include <cstdint>
#include <array>

namespace telemetry::detail {

enum class Phase3EngineCollectStatus : std::uint8_t {
	Collected = 0,
	NoPlayer,
	NotMainThread,
	InvalidSource,
	SourceLimitExceeded,
	IdentityFailure,
	AllocationFailure,
	Count,
};

// Closed diagnostic dimensions for the exact Phase 3 projection stage that
// rejected a sample. They are local observability values, never wire fields.
enum class Phase3EngineCollectBlock : std::uint8_t {
	None = 0,
	Precondition,
	TargetLocks,
	Radar,
	Threat,
	Cargo,
	Navigation,
	StateImage,
	Count,
};

struct Phase3EngineCollectDiagnostic {
	Phase3EngineCollectBlock block = Phase3EngineCollectBlock::None;
	Phase3EngineCollectStatus status = Phase3EngineCollectStatus::Collected;
};

struct Phase3EngineCollectInput {
	std::uint64_t producer_sample_time_us = 0U;
	std::uint64_t player_entity_id = 0U;
	const Phase2ObservationDto* phase2_observation = nullptr;
	const Phase2Wp05SubjectBinding* phase2_bindings = nullptr;
	std::size_t phase2_binding_count = 0U;
	const Phase2ManifestCandidate* installed_manifest = nullptr;
	bool refresh_flight_controls = false;
	bool refresh_systems = false;
};

// Private, pre-ID discovery used to select the next observer-specific
// manifest.  It deliberately contains source keys only and is never retained
// in a Phase3Projection or serialized state image.
struct Phase3CatalogDependencies {
	std::array<std::uint32_t, 64U> ship_class_source_keys{};
	std::uint32_t ship_class_count = 0U;
	std::array<std::uint32_t, 4096U> weapon_source_keys{};
	std::uint32_t weapon_count = 0U;
};

Phase3EngineCollectStatus discover_phase3_catalog_dependencies(
	const Phase2ObservationDto& observation,
	Phase3CatalogDependencies& output) noexcept;

// Reads only main-thread engine authorities. The Phase 2 closure is reconciled
// before any sensor-only identity is allocated.
Phase3EngineCollectStatus collect_phase3_engine_projection(
	const Phase3EngineCollectInput& input,
	Phase3IdentityRegistry& identities,
	Phase3Projection& output,
	Phase3Projection& scratch,
	Phase3EngineCollectDiagnostic* diagnostic = nullptr) noexcept;

} // namespace telemetry::detail
