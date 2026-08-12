#pragma once

#include "telemetry/engine_adapter.h"
#include "telemetry/phase4_catalog_assembly.h"

namespace telemetry::detail {

// Production main-thread collector. It resolves inventory identities back to
// the live engine object only while reading, and never stores the object index
// or a pointer in the ABI-free inventory or resulting catalogue.
Phase4CatalogAssemblyStatus collect_phase4_catalog_definitions(
	const FsoEngineReadView& engine,
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase2ManifestSource& output) noexcept;

} // namespace telemetry::detail
