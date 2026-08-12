#include "telemetry/phase4_entity_image.h"

#include "telemetry/phase4_entity_registry.h"

#include <algorithm>
#include <new>
#include <utility>

namespace telemetry::detail {
namespace {

const Phase4EntityProjectionInput* find_entity(
	const std::vector<Phase4EntityProjectionInput>& entities,
	const std::vector<std::size_t>& order,
	std::uint64_t entity_id) noexcept
{
	const auto found = std::lower_bound(order.begin(), order.end(), entity_id,
		[&entities](std::size_t index, std::uint64_t value) {
			return entities[index].entity_id < value;
		});
	return found != order.end() && entities[*found].entity_id == entity_id
		? &entities[*found] : nullptr;
}

} // namespace

Phase4EntityImageStatus build_phase4_entity_image(
	const std::vector<Phase4EntityProjectionInput>& entities,
	protocol::StateImage& output)
{
	if (entities.empty()) return Phase4EntityImageStatus::EmptyInventory;
	if (entities.size() > Phase4EntityRegistry::Capacity) return Phase4EntityImageStatus::TooManyEntities;
	try {
		std::vector<std::size_t> order;
		order.reserve(entities.size());
		for (std::size_t index = 0U; index < entities.size(); ++index) order.push_back(index);
		std::sort(order.begin(), order.end(), [&entities](std::size_t left, std::size_t right) {
			return entities[left].entity_id < entities[right].entity_id;
		});
		for (std::size_t index = 0U; index < order.size(); ++index) {
			const auto& entity = entities[order[index]];
			if (entity.entity_id == 0U ||
				(index != 0U && entities[order[index - 1U]].entity_id == entity.entity_id)) {
				return Phase4EntityImageStatus::DuplicateEntityId;
			}
			if (entity.parent_entity_id != 0U &&
				find_entity(entities, order, entity.parent_entity_id) == nullptr) {
				return Phase4EntityImageStatus::UnknownParent;
			}
		}
		for (const auto index : order) {
			const auto* current = &entities[index];
			for (std::size_t depth = 0U; current->parent_entity_id != 0U; ++depth) {
				if (depth == entities.size()) return Phase4EntityImageStatus::ParentCycle;
				current = find_entity(entities, order, current->parent_entity_id);
				if (current == nullptr) return Phase4EntityImageStatus::UnknownParent;
			}
		}

		std::vector<protocol::StateAtom> atoms;
		atoms.reserve(entities.size() * 2U);
		for (const auto index : order) {
			protocol::StateAtom lifecycle;
			protocol::StateAtom flight;
			if (project_phase4_entity(entities[index], lifecycle, flight) !=
				Phase4EntityProjectionStatus::Created) {
				return Phase4EntityImageStatus::InvalidEntity;
			}
			atoms.push_back(std::move(lifecycle));
			atoms.push_back(std::move(flight));
		}
		protocol::StateImage candidate;
		const auto result = protocol::StateImage::create(std::move(atoms), candidate);
		if (result == protocol::StateImageResult::AllocationFailed) return Phase4EntityImageStatus::AllocationFailure;
		if (result != protocol::StateImageResult::Created) return Phase4EntityImageStatus::InvalidEntity;
		output = std::move(candidate);
		return Phase4EntityImageStatus::Created;
	} catch (const std::bad_alloc&) {
		return Phase4EntityImageStatus::AllocationFailure;
	}
}

} // namespace telemetry::detail
