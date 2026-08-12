#pragma once

#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase4_catalog_dependencies.h"

#include <cstdint>

namespace telemetry::detail {

enum class Phase4ManifestPlan : std::uint8_t {
	ManifestRequired = 0,
	ReadyForKeyframe,
	InvalidDependencies,
	InvalidManifest,
	Count,
};

enum class Phase4ManifestSourceStatus : std::uint8_t {
	Created = 0,
	InvalidDependencies,
	MissingDefinition,
	DuplicateDefinition,
	Count,
};

// The caller owns the unpublished workspace, so references
// can be committed in place after a complete validation pass. No allocation
// is attempted and an error never yields a publishable source.
Phase4ManifestSourceStatus prepare_phase4_manifest_source_in_place(
	Phase2ManifestSource& source,
	const Phase4CatalogDependencies& dependencies) noexcept;

// Compares the exact active inventory dependencies with the installed public
// manifest. A mismatch is deliberately not a partial-image condition: the
// caller must stage a manifest and then a keyframe before publishing state.
Phase4ManifestPlan plan_phase4_manifest(
	const Phase4CatalogDependencies& dependencies,
	const Phase2ManifestCandidate* installed_manifest) noexcept;

} // namespace telemetry::detail
