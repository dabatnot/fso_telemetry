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

bool hash_u8(protocol::Sha256& hash, std::uint8_t value) noexcept
{
	return hash.update(protocol::ByteView{&value, sizeof(value)});
}

bool hash_u32(protocol::Sha256& hash, std::uint32_t value) noexcept
{
	std::array<std::uint8_t, 4> bytes{{
		static_cast<std::uint8_t>(value),
		static_cast<std::uint8_t>(value >> 8U),
		static_cast<std::uint8_t>(value >> 16U),
		static_cast<std::uint8_t>(value >> 24U)}};
	return hash.update(protocol::ByteView{bytes.data(), bytes.size()});
}

bool hash_float(protocol::Sha256& hash, float value) noexcept
{
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "float32 is required");
	std::memcpy(&bits, &value, sizeof(bits));
	return hash_u32(hash, bits);
}

struct TopologyMember {
	std::uint32_t signature = 0;
};

struct TopologyEdge {
	std::uint8_t kind = 0;
	std::uint32_t owner_signature = 0;
	std::uint32_t target_signature = 0;

	bool operator<(const TopologyEdge& other) const noexcept
	{
		if (kind != other.kind) return kind < other.kind;
		if (owner_signature != other.owner_signature) return owner_signature < other.owner_signature;
		return target_signature < other.target_signature;
	}
};

bool append_topology_edge(std::array<TopologyEdge,
		Phase2ClosureLimits::MaxShips * (Phase2ClosureLimits::MaxShips + 2)>& edges,
	std::uint32_t& count,
	std::uint8_t kind,
	const Phase2ClosureInput& input,
	const Phase2ClosureNode& owner,
	std::uint64_t target_key) noexcept
{
	const auto* target = find_node(input, target_key);
	if (target == nullptr || count == edges.size()) return false;
	edges[count++] = {kind, owner.instance_signature, target->instance_signature};
	return true;
}

bool build_topology_fingerprint(const Phase2ClosureInput& input,
	const Phase2Closure& closure,
	protocol::Sha256Digest& digest) noexcept
{
	std::array<TopologyMember, Phase2ClosureLimits::MaxShips> members{};
	std::array<TopologyEdge,
		Phase2ClosureLimits::MaxShips * (Phase2ClosureLimits::MaxShips + 2)> edges{};
	std::uint32_t edge_count = 0;
	for (std::uint32_t index = 0; index < closure.ship_entity_count; ++index) {
		const auto* node = find_node(input, closure.ship_entities[index]);
		if (node == nullptr) return false;
		members[index].signature = node->instance_signature;
		if (node->support_source_key != 0 && has_key(closure, node->support_source_key) &&
			!append_topology_edge(edges, edge_count, 1, input, *node, node->support_source_key))
			return false;
		if (node->group_leader_source_key != 0 && has_key(closure, node->group_leader_source_key) &&
			!append_topology_edge(edges, edge_count, 2, input, *node, node->group_leader_source_key))
			return false;
		for (std::uint32_t dock = 0; dock < node->dock_source_count; ++dock)
			if (has_key(closure, node->dock_source_keys[dock]) &&
				!append_topology_edge(edges, edge_count, 3, input, *node, node->dock_source_keys[dock]))
				return false;
	}
	std::sort(members.begin(), members.begin() + closure.ship_entity_count,
		[](const auto& left, const auto& right) { return left.signature < right.signature; });
	std::sort(edges.begin(), edges.begin() + edge_count);

	protocol::Sha256 hash;
	if (!hash_u32(hash, closure.ship_entity_count) || !hash_u32(hash, edge_count)) return false;
	for (std::uint32_t index = 0; index < closure.ship_entity_count; ++index)
		if (!hash_u32(hash, members[index].signature)) return false;
	for (std::uint32_t index = 0; index < edge_count; ++index)
		if (!hash_u8(hash, edges[index].kind) ||
			!hash_u32(hash, edges[index].owner_signature) ||
			!hash_u32(hash, edges[index].target_signature))
			return false;
	return hash.finalize(digest);
}

bool build_catalog_fingerprint(const Phase2ClosureInput& input,
	const Phase2Closure& closure,
	protocol::Sha256Digest& digest) noexcept
{
	std::array<protocol::Sha256Digest, Phase2ClosureLimits::MaxShips> descriptors{};
	for (std::uint32_t index = 0; index < closure.ship_entity_count; ++index) {
		const auto* node = find_node(input, closure.ship_entities[index]);
		if (node == nullptr) return false;
		std::array<std::uint32_t, Phase2ClosureLimits::MaxSubsystemsPerShip> subsystem_keys{};
		std::copy(node->subsystem_keys.begin(),
			node->subsystem_keys.begin() + node->subsystem_key_count,
			subsystem_keys.begin());
		std::sort(subsystem_keys.begin(), subsystem_keys.begin() + node->subsystem_key_count);
		protocol::Sha256 descriptor;
		if (!hash_u32(descriptor, node->ship_class_key) ||
			!hash_float(descriptor, node->effective_mass) ||
			!hash_u32(descriptor, node->subsystem_key_count))
			return false;
		for (std::uint32_t subsystem = 0; subsystem < node->subsystem_key_count; ++subsystem)
			if (!hash_u32(descriptor, subsystem_keys[subsystem])) return false;
		if (!descriptor.finalize(descriptors[index])) return false;
	}
	std::sort(descriptors.begin(), descriptors.begin() + closure.ship_entity_count);
	const auto unique_end = std::unique(
		descriptors.begin(), descriptors.begin() + closure.ship_entity_count);
	const auto unique_count = static_cast<std::uint32_t>(
		std::distance(descriptors.begin(), unique_end));
	protocol::Sha256 catalog;
	if (!hash_u32(catalog, unique_count)) return false;
	for (std::uint32_t index = 0; index < unique_count; ++index)
		if (!catalog.update(protocol::ByteView{descriptors[index].data(), descriptors[index].size()}))
			return false;
	return catalog.finalize(digest);
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
	if (!build_topology_fingerprint(input, candidate, candidate.topology_fingerprint) ||
		!build_catalog_fingerprint(input, candidate, candidate.catalog_fingerprint))
		return Phase2ClosureError::InvalidSource;
	output = candidate;
	return Phase2ClosureError::None;
}

} // namespace telemetry
