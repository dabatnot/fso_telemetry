#include "telemetry/phase4_catalog_dependencies.h"

#include <algorithm>

namespace telemetry::detail {
namespace {

bool append_unique(std::uint32_t key, std::uint32_t* values,
	std::uint32_t& count, std::uint32_t capacity) noexcept
{
	if (key == 0U) return false;
	for (std::uint32_t index = 0U; index < count; ++index)
		if (values[index] == key) return true;
	if (count == capacity) return false;
	values[count++] = key;
	return true;
}

} // namespace

Phase4CatalogDependencyStatus discover_phase4_catalog_dependencies(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase4CatalogDependencies& output) noexcept
{
	Phase4CatalogDependencies candidate;
	for (const auto& entry : inventory) {
		if (entry.entity_id == 0U) return Phase4CatalogDependencyStatus::InvalidInventory;
		switch (entry.identity.object_type) {
		case protocol::ObjectType::Ship:
			if (!append_unique(entry.source_class_key,
				candidate.ship_class_source_keys.data(), candidate.ship_class_count,
				Phase2ManifestLimits::MaxClasses))
				return entry.source_class_key == 0U
					? Phase4CatalogDependencyStatus::InvalidInventory
					: Phase4CatalogDependencyStatus::SourceLimitExceeded;
			break;
		case protocol::ObjectType::Weapon:
			if (!append_unique(entry.source_class_key,
				candidate.weapon_source_keys.data(), candidate.weapon_count,
				Phase2ManifestLimits::MaxWeapons))
				return entry.source_class_key == 0U
					? Phase4CatalogDependencyStatus::InvalidInventory
					: Phase4CatalogDependencyStatus::SourceLimitExceeded;
			break;
		case protocol::ObjectType::Asteroid:
		case protocol::ObjectType::Debris:
		case protocol::ObjectType::JumpNode:
		case protocol::ObjectType::Waypoint:
		case protocol::ObjectType::Fireball:
		case protocol::ObjectType::Other:
			if (entry.source_class_key != 0U)
				return Phase4CatalogDependencyStatus::InvalidInventory;
			break;
		default:
			return Phase4CatalogDependencyStatus::InvalidInventory;
		}
	}
	std::sort(candidate.ship_class_source_keys.begin(),
		candidate.ship_class_source_keys.begin() + candidate.ship_class_count);
	std::sort(candidate.weapon_source_keys.begin(),
		candidate.weapon_source_keys.begin() + candidate.weapon_count);
	output = candidate;
	return Phase4CatalogDependencyStatus::Collected;
}

} // namespace telemetry::detail
