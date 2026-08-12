#include "telemetry/phase4_docking_projection.h"

#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_records.h"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace telemetry::detail {
namespace {

using protocol::MutableByteView;
using protocol::PacketWriter;
using protocol::RecordType;
using protocol::StateAtom;

constexpr std::size_t MaximumRelations = 64U;
constexpr std::size_t DockingPayloadCapacity = 18U + MaximumRelations * 288U;

bool contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) noexcept
{
	return std::binary_search(ids.begin(), ids.end(), id);
}

bool reciprocal(const Phase4DockingRelation& left, const Phase4DockingRelation& right) noexcept
{
	return left.local_entity_id == right.remote_entity_id &&
		left.remote_entity_id == right.local_entity_id &&
		left.local_dockpoint == right.remote_dockpoint &&
		left.remote_dockpoint == right.local_dockpoint &&
		left.local_dock_bay_name == right.remote_dock_bay_name &&
		left.remote_dock_bay_name == right.local_dock_bay_name;
}

bool set_entity_key(StateAtom& atom, std::uint64_t entity_id) noexcept
{
	std::array<std::uint8_t, 8U> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(entity_id)) return false;
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::DockingState);
	atom.key.identity.assign(bytes.begin(), bytes.end());
	atom.has_cascade_owner = true;
	atom.cascade_owner.record_type = static_cast<std::uint16_t>(RecordType::EntityLifecycle);
	atom.cascade_owner.identity = atom.key.identity;
	return true;
}

bool assign_and_validate(PacketWriter& writer, StateAtom& atom) noexcept
{
	if (!writer.ok()) return false;
	const auto bytes = writer.written();
	atom.value.assign(bytes.data, bytes.data + bytes.size);
	protocol::BusinessRecordMetadata metadata;
	protocol::RecordEnvelopeView envelope;
	envelope.raw_record_type = atom.key.record_type;
	envelope.record_version = atom.record_version;
	envelope.record_flags = protocol::RecordFlagNone;
	envelope.payload = {atom.value.data(), atom.value.size()};
	return protocol::validate_business_record(envelope,
		protocol::BusinessRecordContainer::FullSnapshot,
		protocol::VersionMinorV1_1, metadata) == protocol::ValidationError::None;
}

} // namespace

Phase4DockingProjectionStatus project_phase4_docking_states(
	const std::vector<std::uint64_t>& ship_entity_ids,
	const std::vector<Phase4DockingRelation>& relations,
	std::uint64_t sample_time_us,
	std::vector<StateAtom>& output)
{
	if (ship_entity_ids.empty() || !std::is_sorted(ship_entity_ids.begin(), ship_entity_ids.end()) ||
		ship_entity_ids.front() == 0U || std::adjacent_find(ship_entity_ids.begin(), ship_entity_ids.end()) != ship_entity_ids.end()) {
		return Phase4DockingProjectionStatus::InvalidShipInventory;
	}
	for (const auto entity_id : ship_entity_ids) {
		std::size_t outgoing_count = 0U;
		for (const auto& relation : relations) if (relation.local_entity_id == entity_id) ++outgoing_count;
		if (outgoing_count > MaximumRelations) return Phase4DockingProjectionStatus::TooManyRelations;
	}
	for (const auto& relation : relations) {
		if (!contains(ship_entity_ids, relation.local_entity_id) ||
			!contains(ship_entity_ids, relation.remote_entity_id)) return Phase4DockingProjectionStatus::UnknownEndpoint;
		if (relation.local_entity_id == relation.remote_entity_id) return Phase4DockingProjectionStatus::SelfRelation;
		if (relation.local_dockpoint > 4095U || relation.remote_dockpoint > 4095U) return Phase4DockingProjectionStatus::InvalidDockpoint;
		std::size_t reciprocal_count = 0U;
		for (const auto& candidate : relations) if (reciprocal(relation, candidate)) ++reciprocal_count;
		if (reciprocal_count == 0U) return Phase4DockingProjectionStatus::MissingReciprocal;
		if (reciprocal_count != 1U) return Phase4DockingProjectionStatus::DuplicateRelation;
		std::size_t same_key_count = 0U;
		for (const auto& candidate : relations) {
			if (candidate.local_entity_id == relation.local_entity_id &&
				candidate.remote_entity_id == relation.remote_entity_id &&
				candidate.local_dockpoint == relation.local_dockpoint &&
				candidate.remote_dockpoint == relation.remote_dockpoint) ++same_key_count;
		}
		if (same_key_count != 1U) return Phase4DockingProjectionStatus::DuplicateRelation;
	}
	try {
		std::vector<StateAtom> candidate;
		candidate.reserve(ship_entity_ids.size());
		for (const auto entity_id : ship_entity_ids) {
			std::vector<const Phase4DockingRelation*> outgoing;
			for (const auto& relation : relations) if (relation.local_entity_id == entity_id) outgoing.push_back(&relation);
			std::sort(outgoing.begin(), outgoing.end(), [](const auto* left, const auto* right) {
				return left->remote_entity_id != right->remote_entity_id
					? left->remote_entity_id < right->remote_entity_id
					: left->local_dockpoint != right->local_dockpoint
						? left->local_dockpoint < right->local_dockpoint
						: left->remote_dockpoint < right->remote_dockpoint;
			});
			std::array<std::uint8_t, DockingPayloadCapacity> bytes{};
			PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
			StateAtom atom;
			if (!writer.write_u64(entity_id) || !writer.write_u64(protocol::DockingStatePresenceFlagNone) ||
				!writer.write_u64(sample_time_us) || !writer.write_u8(static_cast<std::uint8_t>(
					outgoing.empty() ? protocol::DockingPhase::None : protocol::DockingPhase::Docked)) ||
				!writer.write_u64(0U) || !writer.write_u16(static_cast<std::uint16_t>(outgoing.size()))) {
				return Phase4DockingProjectionStatus::EncodingFailure;
			}
			for (const auto* relation : outgoing) {
				std::array<std::uint8_t, 288U> item_bytes{};
				PacketWriter item({item_bytes.data(), item_bytes.size()});
				if (!item.write_u64(relation->remote_entity_id) || !item.write_u16(relation->local_dockpoint) ||
					!item.write_u16(relation->remote_dockpoint) ||
					!item.write_utf8(relation->local_dock_bay_name, 127U) ||
					!item.write_utf8(relation->remote_dock_bay_name, 127U) ||
					!writer.write_u8(1U) || !writer.write_u16(static_cast<std::uint16_t>(item.size())) ||
					!writer.write_bytes(item.written())) return Phase4DockingProjectionStatus::EncodingFailure;
			}
			if (!set_entity_key(atom, entity_id) || !assign_and_validate(writer, atom)) return Phase4DockingProjectionStatus::EncodingFailure;
			candidate.push_back(std::move(atom));
		}
		output = std::move(candidate);
		return Phase4DockingProjectionStatus::Created;
	} catch (const std::bad_alloc&) {
		return Phase4DockingProjectionStatus::AllocationFailure;
	}
}

} // namespace telemetry::detail
