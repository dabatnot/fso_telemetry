#include "telemetry/phase4_trusted_image.h"

#include "telemetry/phase4_ship_state_scope.h"

#include <algorithm>
#include <iterator>
#include <new>
#include <utility>

namespace telemetry::detail {

Phase4TrustedImageStatus build_phase4_trusted_image_from_inventory(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	const std::vector<Phase4DockingRelation>& docking_relations,
	const std::vector<protocol::StateAtom>& inherited_ship_records,
	std::uint64_t sample_time_us,
	protocol::StateImage& output)
{
	std::vector<Phase4EntityProjectionInput> entities;
	try {
		entities.reserve(inventory.size());
	} catch (const std::bad_alloc&) {
		return Phase4TrustedImageStatus::AllocationFailure;
	}
	const auto bindings = bind_phase4_catalogs(inventory, manifest,
		sample_time_us, entities);
	if (bindings == Phase4CatalogBindingStatus::AllocationFailure)
		return Phase4TrustedImageStatus::AllocationFailure;
	if (bindings != Phase4CatalogBindingStatus::Created)
		return Phase4TrustedImageStatus::InvalidEntities;
	return build_phase4_trusted_image(entities, docking_relations, inherited_ship_records,
		sample_time_us, output);
}

Phase4TrustedImageStatus build_phase4_trusted_image_from_inventory(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	const std::vector<Phase4DockingRelation>& docking_relations,
	std::uint64_t sample_time_us,
	protocol::StateImage& output)
{
	static const std::vector<protocol::StateAtom> EmptyShipRecords;
	return build_phase4_trusted_image_from_inventory(inventory, manifest,
		docking_relations, EmptyShipRecords, sample_time_us, output);
}

Phase4TrustedImageStatus build_phase4_trusted_image_from_inventory_preallocated(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	std::uint64_t sample_time_us,
	std::vector<Phase4EntityProjectionInput>& projections,
	Phase4StateImagePool& pool,
	protocol::StateImage& output) noexcept
{
	const auto bindings = bind_phase4_catalogs(inventory, manifest,
		sample_time_us, projections);
	if (bindings == Phase4CatalogBindingStatus::AllocationFailure)
		return Phase4TrustedImageStatus::AllocationFailure;
	if (bindings != Phase4CatalogBindingStatus::Created)
		return Phase4TrustedImageStatus::InvalidEntities;
	const auto image = build_phase4_entity_image_preallocated(
		projections, pool, output);
	if (image == Phase4EntityImageStatus::AllocationFailure ||
		image == Phase4EntityImageStatus::TooManyEntities)
		return Phase4TrustedImageStatus::AllocationFailure;
	return image == Phase4EntityImageStatus::Created
		? Phase4TrustedImageStatus::Created
		: Phase4TrustedImageStatus::InvalidEntities;
}

Phase4TrustedImageStatus build_phase4_trusted_image(
	const std::vector<Phase4EntityProjectionInput>& entities,
	const std::vector<Phase4DockingRelation>& docking_relations,
	const std::vector<protocol::StateAtom>& inherited_ship_records,
	std::uint64_t sample_time_us,
	protocol::StateImage& output)
{
	protocol::StateImage entity_image;
	const auto entity_status = build_phase4_entity_image(entities, entity_image);
	if (entity_status != Phase4EntityImageStatus::Created) {
		return entity_status == Phase4EntityImageStatus::AllocationFailure
			? Phase4TrustedImageStatus::AllocationFailure
			: Phase4TrustedImageStatus::InvalidEntities;
	}
	try {
		std::vector<Phase4EntityTypeBinding> bindings;
		bindings.reserve(entities.size());
		for (const auto& entity : entities)
			bindings.push_back({entity.entity_id, entity.object_type});
		if (validate_phase4_ship_state_scope(inherited_ship_records, bindings) !=
			Phase4ShipStateScopeStatus::Valid)
			return Phase4TrustedImageStatus::InvalidEntities;
		std::vector<std::uint64_t> ships;
		ships.reserve(entities.size());
		for (const auto& entity : entities) {
			if (entity.object_type == protocol::ObjectType::Ship) ships.push_back(entity.entity_id);
		}
		std::vector<protocol::StateAtom> docking;
		if (!ships.empty()) {
			std::sort(ships.begin(), ships.end());
			if (project_phase4_docking_states(ships, docking_relations, sample_time_us, docking) !=
				Phase4DockingProjectionStatus::Created) return Phase4TrustedImageStatus::InvalidDocking;
		} else if (!docking_relations.empty()) {
			return Phase4TrustedImageStatus::InvalidDocking;
		}
		std::vector<protocol::StateAtom> atoms = entity_image.records();
		atoms.insert(atoms.end(), inherited_ship_records.begin(), inherited_ship_records.end());
		atoms.insert(atoms.end(), std::make_move_iterator(docking.begin()), std::make_move_iterator(docking.end()));
		protocol::StateImage candidate;
		protocol::StateImageInvalidRecordReason reason;
		const auto result = protocol::StateImage::create(std::move(atoms), candidate, reason);
		if (result == protocol::StateImageResult::AllocationFailed) return Phase4TrustedImageStatus::AllocationFailure;
		if (result == protocol::StateImageResult::SizeLimitExceeded) return Phase4TrustedImageStatus::SizeLimitExceeded;
		if (result != protocol::StateImageResult::Created) {
			return reason == protocol::StateImageInvalidRecordReason::None
				? Phase4TrustedImageStatus::InvalidEntities
				: Phase4TrustedImageStatus::InvalidCompositeRecord;
		}
		output = std::move(candidate);
		return Phase4TrustedImageStatus::Created;
	} catch (const std::bad_alloc&) {
		return Phase4TrustedImageStatus::AllocationFailure;
	}
}

Phase4TrustedImageStatus build_phase4_trusted_image(
	const std::vector<Phase4EntityProjectionInput>& entities,
	const std::vector<Phase4DockingRelation>& docking_relations,
	std::uint64_t sample_time_us,
	protocol::StateImage& output)
{
	static const std::vector<protocol::StateAtom> EmptyShipRecords;
	return build_phase4_trusted_image(entities, docking_relations,
		EmptyShipRecords, sample_time_us, output);
}

} // namespace telemetry::detail
