#pragma once

// Test-only fixture surface for WP03. Production headers expose none of these
// boundary factories, mutation helpers, or oracle data.

#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>

namespace telemetry::test::phase2test {

enum class SourceCase : std::uint8_t {
	TwoClassesThreeWeaponsWithDecoys,
	CanonicalIdsWithHullTurretAndTertiaryBanks,
	AllAuxiliaryRegistries,
	MultipartFullRequired,
	NearMaxTransaction,
};

enum class BoundaryCase : std::uint8_t {
	String65535,
	String65536,
	RecordCount65535,
	RecordCount65536,
	RecordLength65535,
	RecordLength65536,
	ClassBanks192,
	ClassBanks193,
};

enum class CatalogMutation : std::uint8_t {
	MissingClass,
	MissingWeapon,
	DuplicateClass,
	DuplicateWeapon,
	UnauthorizedExtraClass,
	UnauthorizedExtraWeapon,
};

std::unique_ptr<Phase2ManifestSource> make_source(SourceCase source_case);
std::unique_ptr<Phase2ManifestSource> make_boundary_source(BoundaryCase boundary_case);
std::unique_ptr<Phase2ManifestSource> make_subsystem_source(std::initializer_list<std::uint32_t> counts);
void reverse_engine_order(Phase2ManifestSource& source);
void erase_weapon_definition(Phase2ManifestSource& source, std::uint32_t source_key);
void duplicate_weapon_definition(Phase2ManifestSource& source, std::uint32_t source_key);
protocol::CompletedTransaction
mutate_completed_catalog(const Phase2ManifestCandidate& candidate, CatalogMutation mutation);

} // namespace telemetry::test::phase2test
