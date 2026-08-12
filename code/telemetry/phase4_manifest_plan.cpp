#include "telemetry/phase4_manifest_plan.h"

namespace telemetry::detail {
namespace {

bool sorted_unique_nonzero(const std::uint32_t* values,
	std::uint32_t count, std::uint32_t capacity) noexcept
{
	if (count > capacity) return false;
	for (std::uint32_t index = 0U; index < count; ++index)
		if (values[index] == 0U ||
			(index != 0U && values[index - 1U] >= values[index]))
			return false;
	return true;
}

template <typename Records>
bool manifest_matches(const std::uint32_t* required, std::uint32_t required_count,
	const Records& records, std::uint32_t record_count) noexcept
{
	if (required_count != record_count) return false;
	for (std::uint32_t index = 0U; index < required_count; ++index) {
		std::uint32_t matches = 0U;
		for (std::uint32_t record = 0U; record < record_count; ++record)
			if (records[record].source_key == required[index]) ++matches;
		if (matches != 1U) return false;
	}
	return true;
}

} // namespace

Phase4ManifestPlan plan_phase4_manifest(
	const Phase4CatalogDependencies& dependencies,
	const Phase2ManifestCandidate* installed_manifest) noexcept
{
	if (!sorted_unique_nonzero(dependencies.ship_class_source_keys.data(),
		dependencies.ship_class_count, Phase2ManifestLimits::MaxClasses) ||
		!sorted_unique_nonzero(dependencies.weapon_source_keys.data(),
		dependencies.weapon_count, Phase2ManifestLimits::MaxWeapons))
		return Phase4ManifestPlan::InvalidDependencies;
	if (installed_manifest == nullptr) return Phase4ManifestPlan::ManifestRequired;
	if (installed_manifest->class_record_count > Phase2ManifestLimits::MaxClasses ||
		installed_manifest->weapon_record_count > Phase2ManifestLimits::MaxWeapons)
		return Phase4ManifestPlan::InvalidManifest;
	return manifest_matches(dependencies.ship_class_source_keys.data(),
		dependencies.ship_class_count, installed_manifest->class_records,
		installed_manifest->class_record_count) &&
		manifest_matches(dependencies.weapon_source_keys.data(),
		dependencies.weapon_count, installed_manifest->weapon_records,
		installed_manifest->weapon_record_count)
		? Phase4ManifestPlan::ReadyForKeyframe
		: Phase4ManifestPlan::ManifestRequired;
}

} // namespace telemetry::detail
