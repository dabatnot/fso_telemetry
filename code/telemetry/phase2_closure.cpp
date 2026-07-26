#include "telemetry/phase2_closure.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace telemetry {
namespace {

const Phase2ClosureNode* find_node(const Phase2ClosureInput& input, std::uint64_t key) noexcept
{
	for (std::uint32_t i = 0; i < input.node_count && i < input.nodes.size(); ++i)
		if (input.nodes[i].source_key == key) return &input.nodes[i];
	return nullptr;
}

bool has_key(const Phase2Closure& closure, std::uint64_t key) noexcept
{
	return std::find(closure.ship_entities.begin(),
		       closure.ship_entities.begin() + closure.ship_entity_count,
		       key) != closure.ship_entities.begin() + closure.ship_entity_count;
}

bool append_key(Phase2Closure& closure, std::uint64_t key) noexcept
{
	if (key == 0 || has_key(closure, key)) return true;
	if (closure.ship_entity_count == Phase2ClosureLimits::MaxShips) return false;
	closure.ship_entities[closure.ship_entity_count++] = key;
	return true;
}

template <typename T>
void hash_values(const T* values, std::size_t count, protocol::Sha256Digest& digest) noexcept
{
	protocol::sha256(protocol::ByteView{
		reinterpret_cast<const std::uint8_t*>(values), count * sizeof(T)}, digest);
}

} // namespace

bool Phase2Closure::references_cargo_target(std::uint64_t owner, std::uint64_t target) const noexcept
{
	for (std::uint32_t i = 0; i < cargo_reference_count; ++i)
		if (cargo_references[i][0] == owner && cargo_references[i][1] == target) return true;
	return false;
}

Phase2ClosureError build_phase2_closure(
	const Phase2ClosureInput& input, Phase2Profile profile, Phase2Closure& output) noexcept
{
	if (input.node_count > input.nodes.size()) return Phase2ClosureError::TooManyShips;
	const auto* player = find_node(input, input.player_source_key);
	if (input.player_source_key == 0 || player == nullptr || !player->valid)
		return Phase2ClosureError::InvalidSource;

	Phase2Closure candidate{};
	candidate.ship_entities[candidate.ship_entity_count++] = input.player_source_key;
	if (profile == Phase2Profile::CompleteShip) {
		for (std::uint32_t cursor = 0; cursor < candidate.ship_entity_count; ++cursor) {
			const auto* node = find_node(input, candidate.ship_entities[cursor]);
			if (node == nullptr || !node->valid || node->source_key == UINT64_MAX ||
				node->ship_class_key == 0 || node->subsystem_key_count > Phase2ClosureLimits::MaxSubsystemsPerShip ||
				node->dock_source_count > Phase2ClosureLimits::MaxShips)
				return node != nullptr && node->subsystem_key_count > Phase2ClosureLimits::MaxSubsystemsPerShip
					? Phase2ClosureError::TooManySubsystemsPerShip
					: Phase2ClosureError::InvalidSource;
			if (node->support_source_key != 0) {
				if (node->support_source_key == UINT64_MAX) return Phase2ClosureError::InvalidSource;
				if (!node->support_signature_valid || find_node(input, node->support_source_key) == nullptr)
					return Phase2ClosureError::InvalidAuthorizedEdge;
				if (!append_key(candidate, node->support_source_key)) return Phase2ClosureError::TooManyShips;
			}
			for (std::uint32_t d = 0; d < node->dock_source_count; ++d) {
				const auto key = node->dock_source_keys[d];
				const auto* peer = find_node(input, key);
				if (key == 0 || peer == nullptr) return Phase2ClosureError::InvalidAuthorizedEdge;
				bool reciprocal = false;
				for (std::uint32_t p = 0; p < peer->dock_source_count; ++p)
					reciprocal = reciprocal || peer->dock_source_keys[p] == node->source_key;
				if (!reciprocal) return Phase2ClosureError::InvalidAuthorizedEdge;
				if (!append_key(candidate, key)) return Phase2ClosureError::TooManyShips;
			}
			if (node->group_leader_source_key != 0) {
				if (!node->group_leader_component_valid ||
					find_node(input, node->group_leader_source_key) == nullptr)
					return Phase2ClosureError::InvalidAuthorizedEdge;
				if (!append_key(candidate, node->group_leader_source_key))
					return Phase2ClosureError::TooManyShips;
			}
		}
	}

	std::sort(candidate.ship_entities.begin(),
		candidate.ship_entities.begin() + candidate.ship_entity_count);
	for (std::uint32_t i = 0; i < candidate.ship_entity_count; ++i) {
		const auto* node = find_node(input, candidate.ship_entities[i]);
		if (node == nullptr || !node->valid) return Phase2ClosureError::InvalidSource;
		if (node->subsystem_key_count > Phase2ClosureLimits::MaxSubsystemsPerShip)
			return Phase2ClosureError::TooManySubsystemsPerShip;
		if (candidate.subsystem_count + node->subsystem_key_count >
			Phase2ClosureLimits::MaxAggregateSubsystems)
			return Phase2ClosureError::TooManyAggregateSubsystems;
		for (std::uint32_t s = 0; s < node->subsystem_key_count; ++s)
			candidate.subsystem_keys[candidate.subsystem_count++] = node->subsystem_keys[s];
		candidate.class_descriptors[candidate.class_descriptor_count++] =
			{node->instance_signature, node->ship_class_key, node->effective_mass};
		if (node->cargo_target_source_key != 0) {
			if (!has_key(candidate, node->cargo_target_source_key))
				return Phase2ClosureError::UnauthorizedReference;
			candidate.cargo_references[candidate.cargo_reference_count++] =
				{node->source_key, node->cargo_target_source_key};
		}
	}
	std::sort(candidate.class_descriptors.begin(),
		candidate.class_descriptors.begin() + candidate.class_descriptor_count,
		[](const auto& a, const auto& b) {
			if (a.instance_signature != b.instance_signature)
				return a.instance_signature < b.instance_signature;
			return a.ship_class_key < b.ship_class_key;
		});
	std::array<std::uint32_t, Phase2ClosureLimits::MaxShips> topology{};
	for (std::uint32_t i = 0; i < candidate.class_descriptor_count; ++i)
		topology[i] = candidate.class_descriptors[i].instance_signature;
	std::sort(topology.begin(), topology.begin() + candidate.class_descriptor_count);
	hash_values(topology.data(), candidate.class_descriptor_count, candidate.topology_fingerprint);
	std::array<float, Phase2ClosureLimits::MaxShips> catalog{};
	for (std::uint32_t i = 0; i < candidate.class_descriptor_count; ++i)
		catalog[i] = candidate.class_descriptors[i].effective_mass;
	std::sort(catalog.begin(), catalog.begin() + candidate.class_descriptor_count,
		[](float a, float b) { return a < b; });
	hash_values(catalog.data(), candidate.class_descriptor_count, candidate.catalog_fingerprint);
	output = candidate;
	return Phase2ClosureError::None;
}

} // namespace telemetry
