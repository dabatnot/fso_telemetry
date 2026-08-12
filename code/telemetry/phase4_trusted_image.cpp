#include "telemetry/phase4_trusted_image.h"

#include <algorithm>
#include <iterator>
#include <new>
#include <utility>

namespace telemetry::detail {

Phase4TrustedImageStatus build_phase4_trusted_image_from_inventory(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	const Phase2ManifestCandidate* manifest,
	const std::vector<Phase4DockingRelation>& docking_relations,
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
	return build_phase4_trusted_image(entities, docking_relations,
		sample_time_us, output);
}

Phase4TrustedImageStatus build_phase4_trusted_image(
	const std::vector<Phase4EntityProjectionInput>& entities,
	const std::vector<Phase4DockingRelation>& docking_relations,
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
		atoms.insert(atoms.end(), std::make_move_iterator(docking.begin()), std::make_move_iterator(docking.end()));
		protocol::StateImage candidate;
		protocol::StateImageInvalidRecordReason reason;
		const auto result = protocol::StateImage::create(std::move(atoms), candidate, reason);
		if (result == protocol::StateImageResult::AllocationFailed) return Phase4TrustedImageStatus::AllocationFailure;
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

} // namespace telemetry::detail
