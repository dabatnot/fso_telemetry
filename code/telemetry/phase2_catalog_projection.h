#pragma once

#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase2_observation.h"

#include <cstdint>

namespace telemetry::detail {

// Closed result of the production engine-state to normalized pre-ID catalog
// projection. Callers may retain their last coherent catalog only for
// SourceTemporarilyUnavailable; every other failure is structural.
enum class Phase2CatalogProjectionStatus : std::uint8_t {
	Success = 0,
	SourceTemporarilyUnavailable,
	SourceLimitExceeded,
	InvalidSource,
};

Phase2CatalogProjectionStatus project_phase2_catalog(
	const Phase2ObservationDto& observation,
	Phase2ManifestSource& output) noexcept;

} // namespace telemetry::detail
