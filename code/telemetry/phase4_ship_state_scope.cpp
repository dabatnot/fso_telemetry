#include "telemetry/phase4_ship_state_scope.h"

namespace telemetry::detail {
namespace {

bool ship_detailed_record(protocol::RecordType type) noexcept
{
	return type >= protocol::RecordType::ShipIdentity &&
		type <= protocol::RecordType::EffectState &&
		type != protocol::RecordType::FlightState;
}

bool entity_id(const protocol::StateAtom& atom, std::uint64_t& output) noexcept
{
	if (atom.key.identity.size() < 8U) return false;
	output = 0U;
	for (std::size_t index = 0U; index < 8U; ++index)
		output |= static_cast<std::uint64_t>(atom.key.identity[index]) << (index * 8U);
	return output != 0U;
}

const Phase4EntityTypeBinding* find_entity(
	const std::vector<Phase4EntityTypeBinding>& entities, std::uint64_t entity_id) noexcept
{
	for (const auto& entity : entities)
		if (entity.entity_id == entity_id) return &entity;
	return nullptr;
}

} // namespace

Phase4ShipStateScopeStatus validate_phase4_ship_state_scope(
	const std::vector<protocol::StateAtom>& records,
	const std::vector<Phase4EntityTypeBinding>& entities) noexcept
{
	for (std::size_t left = 0U; left < entities.size(); ++left) {
		if (entities[left].entity_id == 0U ||
			entities[left].object_type < protocol::ObjectType::Ship ||
			entities[left].object_type > protocol::ObjectType::Other)
			return Phase4ShipStateScopeStatus::InvalidBinding;
		for (std::size_t right = 0U; right < left; ++right)
			if (entities[left].entity_id == entities[right].entity_id)
				return Phase4ShipStateScopeStatus::InvalidBinding;
	}
	for (const auto& atom : records) {
		const auto type = static_cast<protocol::RecordType>(atom.key.record_type);
		if (!ship_detailed_record(type)) continue;
		std::uint64_t id = 0U;
		if (!entity_id(atom, id)) return Phase4ShipStateScopeStatus::MalformedIdentity;
		const auto* entity = find_entity(entities, id);
		if (entity == nullptr) return Phase4ShipStateScopeStatus::UnknownEntity;
		if (entity->object_type != protocol::ObjectType::Ship)
			return Phase4ShipStateScopeStatus::NonShipDetailedState;
	}
	return Phase4ShipStateScopeStatus::Valid;
}

} // namespace telemetry::detail
