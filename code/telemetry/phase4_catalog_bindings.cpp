#include "telemetry/phase4_catalog_bindings.h"

namespace telemetry::detail {
namespace {

bool has_duplicate_definitions(const Phase2ManifestCandidate& manifest) noexcept
{
	for (std::uint32_t left = 0U; left < manifest.class_record_count; ++left)
		for (std::uint32_t right = left + 1U; right < manifest.class_record_count; ++right)
			if (manifest.class_records[left].source_key == manifest.class_records[right].source_key)
				return true;
	for (std::uint32_t left = 0U; left < manifest.weapon_record_count; ++left)
		for (std::uint32_t right = left + 1U; right < manifest.weapon_record_count; ++right)
			if (manifest.weapon_records[left].source_key == manifest.weapon_records[right].source_key)
				return true;
	return false;
}

std::uint32_t public_class_id(const Phase2ManifestCandidate& manifest,
	protocol::ObjectType type, std::uint32_t source_key) noexcept
{
	if (source_key == 0U) return 0U;
	if (type == protocol::ObjectType::Ship) {
		for (std::uint32_t index = 0U; index < manifest.class_record_count; ++index)
			if (manifest.class_records[index].source_key == source_key)
				return manifest.class_records[index].class_id;
	} else if (type == protocol::ObjectType::Weapon) {
		for (std::uint32_t index = 0U; index < manifest.weapon_record_count; ++index)
			if (manifest.weapon_records[index].source_key == source_key)
				return manifest.weapon_records[index].weapon_class_id;
	}
	return 0U;
}

bool public_class_type(protocol::ObjectType type) noexcept
{
	return type == protocol::ObjectType::Ship || type == protocol::ObjectType::Weapon;
}

} // namespace

Phase4CatalogBindingStatus bind_phase4_catalogs(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	std::uint64_t sample_time_us,
	std::vector<Phase4EntityProjectionInput>& output) noexcept
{
	if (manifest == nullptr) return Phase4CatalogBindingStatus::MissingManifest;
	if (manifest->class_record_count > Phase2ManifestLimits::MaxClasses ||
		manifest->weapon_record_count > Phase2ManifestLimits::MaxWeapons ||
		has_duplicate_definitions(*manifest))
		return Phase4CatalogBindingStatus::DuplicateDefinition;
	if (output.capacity() < inventory.size())
		return Phase4CatalogBindingStatus::AllocationFailure;

	// Validate completely before mutating the caller-owned, pre-reserved
	// workspace.  A second pass then materializes the projection without any
	// allocation in the capture tick.
	for (const auto& source : inventory) {
		const auto type = source.identity.object_type;
		if (source.entity_id == 0U || type < protocol::ObjectType::Ship ||
			type > protocol::ObjectType::Other)
			return Phase4CatalogBindingStatus::InvalidInventory;
		const auto class_id = public_class_id(*manifest, type, source.source_class_key);
		if (public_class_type(type) && class_id == 0U)
			return Phase4CatalogBindingStatus::MissingClass;
		if (!public_class_type(type) && source.source_class_key != 0U)
			return Phase4CatalogBindingStatus::InvalidInventory;
	}
	output.clear();
	for (const auto& source : inventory) {
		const auto type = source.identity.object_type;
		Phase4EntityProjectionInput entry;
		entry.entity_id = source.entity_id;
		entry.object_type = type;
		entry.sample_time_us = sample_time_us;
		entry.class_id = public_class_id(*manifest, type, source.source_class_key);
		entry.position_world = source.position_world;
		entry.orientation_local_to_world = source.orientation_local_to_world;
		entry.velocity_world = source.velocity_world;
		entry.rotational_velocity_local = source.rotational_velocity_local;
		entry.radius = source.radius;
		entry.physics_mode_flags = source.physics_mode_flags;
		entry.static_marker = source.static_marker;
		output.push_back(entry);
	}
	return Phase4CatalogBindingStatus::Created;
}

} // namespace telemetry::detail
