#include "telemetry/phase4_entity_image.h"

#include "telemetry/phase4_entity_registry.h"

#include <algorithm>
#include <limits>
#include <memory>
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

void swap_atom_backing(protocol::StateAtom& left,
	protocol::StateAtom& right) noexcept
{
	using std::swap;
	swap(left.key, right.key);
	swap(left.record_version, right.record_version);
	swap(left.lifecycle, right.lifecycle);
	swap(left.has_cascade_owner, right.has_cascade_owner);
	swap(left.cascade_owner, right.cascade_owner);
	swap(left.value, right.value);
}

std::size_t atom_backing_bytes(const protocol::StateAtom& atom) noexcept
{
	return atom.key.identity.capacity() +
		atom.cascade_owner.identity.capacity() + atom.value.capacity();
}

} // namespace

bool Phase4StateImagePool::provision(std::size_t maximum_entities) noexcept
{
	reset();
	if (maximum_entities == 0U ||
		maximum_entities > Phase4EntityRegistry::Capacity ||
		maximum_entities > std::numeric_limits<std::size_t>::max() / 2U)
		return false;
	const auto maximum_records = maximum_entities * 2U;
	try {
		for (auto& slot : m_slots) {
			slot.records = std::make_shared<std::vector<protocol::StateAtom>>();
			slot.records->reserve(maximum_records);
			slot.records->resize(maximum_records);
			slot.spares.reserve(maximum_records);
			slot.spares.resize(maximum_records);
			slot.order.reserve(maximum_entities);
			for (auto& atom : *slot.records) {
				atom.key.identity.reserve(8U);
				atom.cascade_owner.identity.reserve(8U);
				atom.value.reserve(96U);
			}
			slot.spare_count = 0U;
		}
	} catch (const std::bad_alloc&) {
		reset();
		return false;
	}
	m_maximum_entities = maximum_entities;
	m_ready = true;
	return true;
}

void Phase4StateImagePool::reset() noexcept
{
	for (auto& slot : m_slots) {
		slot.records.reset();
		std::vector<protocol::StateAtom>().swap(slot.spares);
		std::vector<std::size_t>().swap(slot.order);
		slot.spare_count = 0U;
	}
	m_maximum_entities = 0U;
	m_ready = false;
}

std::size_t Phase4StateImagePool::owned_backing_bytes() const noexcept
{
	std::size_t total = 0U;
	for (const auto& slot : m_slots) {
		if (slot.records) {
			total += sizeof(*slot.records) +
				slot.records->capacity() * sizeof(protocol::StateAtom);
			for (const auto& atom : *slot.records)
				total += atom_backing_bytes(atom);
		}
		total += slot.spares.capacity() * sizeof(protocol::StateAtom);
		for (const auto& atom : slot.spares)
			total += atom_backing_bytes(atom);
		total += slot.order.capacity() * sizeof(std::size_t);
	}
	return total;
}

Phase4StateImagePool::Slot* Phase4StateImagePool::acquire() noexcept
{
	if (!m_ready) return nullptr;
	for (auto& slot : m_slots)
		if (slot.records && slot.records.use_count() == 1L) return &slot;
	return nullptr;
}

bool Phase4StateImagePool::prepare(Slot& slot,
	std::size_t record_count) noexcept
{
	if (!slot.records || record_count > m_maximum_entities * 2U ||
		slot.records->size() + slot.spare_count < record_count)
		return false;
	while (slot.records->size() > record_count) {
		if (slot.spare_count >= slot.spares.size()) return false;
		swap_atom_backing(slot.spares[slot.spare_count++],
			slot.records->back());
		slot.records->pop_back();
	}
	while (slot.records->size() < record_count) {
		if (slot.spare_count == 0U) return false;
		slot.records->emplace_back();
		swap_atom_backing(slot.records->back(),
			slot.spares[--slot.spare_count]);
	}
	return true;
}

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

Phase4EntityImageStatus build_phase4_entity_image_preallocated(
	const std::vector<Phase4EntityProjectionInput>& entities,
	Phase4StateImagePool& pool,
	protocol::StateImage& output) noexcept
{
	if (entities.empty()) return Phase4EntityImageStatus::EmptyInventory;
	if (!pool.ready() || entities.size() > pool.maximum_entities())
		return Phase4EntityImageStatus::TooManyEntities;
	auto* slot = pool.acquire();
	if (slot == nullptr)
		return Phase4EntityImageStatus::AllocationFailure;
	if (!pool.prepare(*slot, entities.size() * 2U) ||
		slot->order.capacity() < entities.size())
		return Phase4EntityImageStatus::AllocationFailure;

	slot->order.clear();
	for (std::size_t index = 0U; index < entities.size(); ++index)
		slot->order.push_back(index);
	std::sort(slot->order.begin(), slot->order.end(),
		[&entities](std::size_t left, std::size_t right) noexcept {
			return entities[left].entity_id < entities[right].entity_id;
		});
	for (std::size_t index = 0U; index < slot->order.size(); ++index) {
		const auto& entity = entities[slot->order[index]];
		if (entity.entity_id == 0U ||
			(index != 0U && entities[slot->order[index - 1U]].entity_id ==
				entity.entity_id))
			return Phase4EntityImageStatus::DuplicateEntityId;
		if (entity.parent_entity_id != 0U &&
			find_entity(entities, slot->order,
				entity.parent_entity_id) == nullptr)
			return Phase4EntityImageStatus::UnknownParent;
	}
	for (const auto index : slot->order) {
		const auto* current = &entities[index];
		for (std::size_t depth = 0U;
			 current->parent_entity_id != 0U; ++depth) {
			if (depth == entities.size())
				return Phase4EntityImageStatus::ParentCycle;
			current = find_entity(entities, slot->order,
				current->parent_entity_id);
			if (current == nullptr)
				return Phase4EntityImageStatus::UnknownParent;
		}
	}

	const auto count = entities.size();
	for (std::size_t ordinal = 0U; ordinal < count; ++ordinal) {
		auto& lifecycle = (*slot->records)[ordinal];
		auto& flight = (*slot->records)[count + ordinal];
		if (project_phase4_entity(entities[slot->order[ordinal]],
				lifecycle, flight) != Phase4EntityProjectionStatus::Created)
			return Phase4EntityImageStatus::InvalidEntity;
	}
	protocol::StateImage candidate;
	protocol::StateImageInvalidRecordReason reason =
		protocol::StateImageInvalidRecordReason::None;
	std::shared_ptr<const std::vector<protocol::StateAtom>> records =
		slot->records;
	const auto adopted = protocol::StateImage::adopt_preallocated(
		records, candidate, reason);
	if (adopted == protocol::StateImageResult::AllocationFailed)
		return Phase4EntityImageStatus::AllocationFailure;
	if (adopted != protocol::StateImageResult::Created)
		return Phase4EntityImageStatus::InvalidEntity;
	output = std::move(candidate);
	return Phase4EntityImageStatus::Created;
}

} // namespace telemetry::detail
