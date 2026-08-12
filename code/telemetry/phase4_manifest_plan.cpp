#include "telemetry/phase4_manifest_plan.h"

#include <memory>
#include <new>

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

template <typename Definitions>
bool exactly_one_definition(const Definitions& definitions, std::uint32_t count,
	std::uint32_t source_key) noexcept
{
	std::uint32_t matches = 0U;
	for (std::uint32_t index = 0U; index < count; ++index)
		if (definitions[index].source_key == source_key) ++matches;
	return matches == 1U;
}

bool append_unique(std::uint32_t source_key, std::uint32_t* values,
	std::uint32_t& count, std::uint32_t capacity) noexcept
{
	if (source_key == 0U || count > capacity) return false;
	for (std::uint32_t index = 0U; index < count; ++index)
		if (values[index] == source_key) return true;
	if (count == capacity) return false;
	values[count++] = source_key;
	return true;
}

const Phase2ClassSource* find_ship_class(const Phase2ManifestSource& source,
	std::uint32_t source_key) noexcept
{
	for (std::uint32_t index = 0U; index < source.ship_class_count; ++index)
		if (source.ship_classes[index].source_key == source_key)
			return &source.ship_classes[index];
	return nullptr;
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

Phase4ManifestSourceStatus build_phase4_manifest_source(
	const Phase2ManifestSource& available_definitions,
	const Phase4CatalogDependencies& dependencies,
	Phase2ManifestSource& output) noexcept
{
	if (!sorted_unique_nonzero(dependencies.ship_class_source_keys.data(),
		dependencies.ship_class_count, Phase2ManifestLimits::MaxClasses) ||
		!sorted_unique_nonzero(dependencies.weapon_source_keys.data(),
		dependencies.weapon_count, Phase2ManifestLimits::MaxWeapons) ||
		available_definitions.ship_class_count > Phase2ManifestLimits::MaxClasses ||
		available_definitions.weapon_count > Phase2ManifestLimits::MaxWeapons)
		return Phase4ManifestSourceStatus::InvalidDependencies;
	for (std::uint32_t index = 0U; index < available_definitions.ship_class_count; ++index)
		if (!exactly_one_definition(available_definitions.ship_classes,
			available_definitions.ship_class_count,
			available_definitions.ship_classes[index].source_key))
			return Phase4ManifestSourceStatus::DuplicateDefinition;
	for (std::uint32_t index = 0U; index < available_definitions.weapon_count; ++index)
		if (!exactly_one_definition(available_definitions.weapons,
			available_definitions.weapon_count,
			available_definitions.weapons[index].source_key))
			return Phase4ManifestSourceStatus::DuplicateDefinition;

	auto candidate = std::unique_ptr<Phase2ManifestSource>(
		new (std::nothrow) Phase2ManifestSource(available_definitions));
	if (candidate == nullptr) return Phase4ManifestSourceStatus::AllocationFailure;
	candidate->referenced_ship_class_count = 0U;
	candidate->referenced_weapon_count = 0U;
	candidate->referenced_ship_class_keys.fill(0U);
	candidate->referenced_weapon_keys.fill(0U);
	for (std::uint32_t index = 0U; index < dependencies.ship_class_count; ++index) {
		const auto key = dependencies.ship_class_source_keys[index];
		const auto* ship_class = find_ship_class(available_definitions, key);
		if (ship_class == nullptr) return Phase4ManifestSourceStatus::MissingDefinition;
		if (!append_unique(key, candidate->referenced_ship_class_keys.data(),
			candidate->referenced_ship_class_count,
			Phase2ManifestLimits::MaxClasses))
			return Phase4ManifestSourceStatus::InvalidDependencies;
		if (ship_class->bank_count > Phase2ManifestLimits::MaxBanksPerClass)
			return Phase4ManifestSourceStatus::InvalidDependencies;
		for (std::uint32_t bank = 0U; bank < ship_class->bank_count; ++bank)
			if (!append_unique(ship_class->banks[bank].weapon_source_key,
				candidate->referenced_weapon_keys.data(),
				candidate->referenced_weapon_count,
				Phase2ManifestLimits::MaxWeapons))
				return Phase4ManifestSourceStatus::InvalidDependencies;
		if (ship_class->countermeasure_weapon_source_key != 0U &&
			!append_unique(ship_class->countermeasure_weapon_source_key,
				candidate->referenced_weapon_keys.data(),
				candidate->referenced_weapon_count,
				Phase2ManifestLimits::MaxWeapons))
			return Phase4ManifestSourceStatus::InvalidDependencies;
	}
	for (std::uint32_t index = 0U; index < dependencies.weapon_count; ++index)
		if (!append_unique(dependencies.weapon_source_keys[index],
			candidate->referenced_weapon_keys.data(),
			candidate->referenced_weapon_count,
			Phase2ManifestLimits::MaxWeapons))
			return Phase4ManifestSourceStatus::InvalidDependencies;
	for (std::uint32_t index = 0U; index < candidate->referenced_weapon_count; ++index)
		if (!exactly_one_definition(available_definitions.weapons,
			available_definitions.weapon_count,
			candidate->referenced_weapon_keys[index]))
			return Phase4ManifestSourceStatus::MissingDefinition;
	output = std::move(*candidate);
	return Phase4ManifestSourceStatus::Created;
}

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
