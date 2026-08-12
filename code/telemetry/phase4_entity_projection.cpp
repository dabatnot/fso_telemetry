#include "telemetry/phase4_entity_projection.h"

#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_records.h"

#include <array>
#include <cmath>

namespace telemetry::detail {
namespace {

using protocol::MutableByteView;
using protocol::PacketWriter;
using protocol::RecordType;
using protocol::StateAtom;

constexpr std::size_t LifecyclePayloadCapacity = 64U;
constexpr std::size_t FlightPayloadCapacity = 96U;

bool valid_exportable_type(protocol::ObjectType type) noexcept
{
	return type >= protocol::ObjectType::Ship && type <= protocol::ObjectType::Other;
}

bool has_public_class(protocol::ObjectType type) noexcept
{
	return type == protocol::ObjectType::Ship || type == protocol::ObjectType::Weapon;
}

bool finite(const std::array<float, 3U>& values) noexcept
{
	return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}

bool finite(const std::array<float, 4U>& values) noexcept
{
	return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]) &&
		std::isfinite(values[3]);
}

bool is_identity(const std::array<float, 4U>& value) noexcept
{
	return value[0] == 0.0F && value[1] == 0.0F && value[2] == 0.0F && value[3] == 1.0F;
}

bool is_zero(const std::array<float, 3U>& value) noexcept
{
	return value[0] == 0.0F && value[1] == 0.0F && value[2] == 0.0F;
}

bool set_entity_key(StateAtom& atom, RecordType type, std::uint64_t entity_id) noexcept
{
	std::array<std::uint8_t, 8U> bytes{};
	PacketWriter writer({bytes.data(), bytes.size()});
	if (!writer.write_u64(entity_id)) return false;
	atom.key.record_type = static_cast<std::uint16_t>(type);
	atom.key.identity.assign(bytes.begin(), bytes.end());
	atom.has_cascade_owner = type != RecordType::EntityLifecycle;
	if (atom.has_cascade_owner) {
		atom.cascade_owner.record_type = static_cast<std::uint16_t>(RecordType::EntityLifecycle);
		atom.cascade_owner.identity = atom.key.identity;
	}
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

void reset_atom_preserving_storage(StateAtom& atom) noexcept
{
	atom.key.record_type = 0U;
	atom.key.identity.clear();
	atom.record_version = 1U;
	atom.lifecycle = protocol::StateRecordLifecycle::UpsertOnly;
	atom.has_cascade_owner = false;
	atom.cascade_owner.record_type = 0U;
	atom.cascade_owner.identity.clear();
	atom.value.clear();
}

} // namespace

Phase4EntityProjectionStatus project_phase4_entity(const Phase4EntityProjectionInput& input,
	StateAtom& lifecycle,
	StateAtom& flight)
{
	if (input.entity_id == 0U || !valid_exportable_type(input.object_type) ||
		input.lifecycle_phase > protocol::LifecyclePhase::Removed ||
		(input.lifecycle_flags & ~protocol::KnownEntityLifecycleFlags) != 0U ||
		((input.lifecycle_flags & protocol::EntityLifecycleFlagBomb) != 0U &&
			input.object_type != protocol::ObjectType::Weapon) ||
		(has_public_class(input.object_type) && input.class_id == 0U) ||
		(!has_public_class(input.object_type) && input.class_id != 0U)) {
		return Phase4EntityProjectionStatus::InvalidLifecycle;
	}
	if (!finite(input.position_world) || !finite(input.orientation_local_to_world) ||
		!finite(input.velocity_world) || !finite(input.rotational_velocity_local) ||
		!std::isfinite(input.radius) || input.radius < 0.0F ||
		(input.static_marker && (!is_zero(input.velocity_world) ||
			!is_zero(input.rotational_velocity_local) || !is_identity(input.orientation_local_to_world)))) {
		return Phase4EntityProjectionStatus::InvalidPose;
	}

	reset_atom_preserving_storage(lifecycle);
	reset_atom_preserving_storage(flight);
	const auto presence = (has_public_class(input.object_type)
		? protocol::EntityLifecyclePresenceFlagClassReference : 0U) |
		(input.parent_entity_id != 0U ? protocol::EntityLifecyclePresenceFlagParent : 0U);
	std::array<std::uint8_t, LifecyclePayloadCapacity> lifecycle_bytes{};
	PacketWriter lifecycle_writer({lifecycle_bytes.data(), lifecycle_bytes.size()});
	if (!lifecycle_writer.write_u64(input.entity_id) || !lifecycle_writer.write_u64(presence) ||
		!lifecycle_writer.write_u64(input.sample_time_us) ||
		!lifecycle_writer.write_u8(static_cast<std::uint8_t>(input.object_type)) ||
		!lifecycle_writer.write_u8(static_cast<std::uint8_t>(input.lifecycle_phase)) ||
		!lifecycle_writer.write_u32(input.lifecycle_flags) ||
		(has_public_class(input.object_type) && !lifecycle_writer.write_u32(input.class_id)) ||
		(input.parent_entity_id != 0U && !lifecycle_writer.write_u64(input.parent_entity_id)) ||
		!set_entity_key(lifecycle, RecordType::EntityLifecycle, input.entity_id)) {
		return Phase4EntityProjectionStatus::EncodingFailure;
	}
	lifecycle.lifecycle = protocol::StateRecordLifecycle::ExplicitCreateDelete;
	if (!assign_and_validate(lifecycle_writer, lifecycle)) return Phase4EntityProjectionStatus::EncodingFailure;

	std::array<std::uint8_t, FlightPayloadCapacity> flight_bytes{};
	PacketWriter flight_writer({flight_bytes.data(), flight_bytes.size()});
	if (!flight_writer.write_u64(input.entity_id) ||
		!flight_writer.write_u64(protocol::FlightStatePresenceFlagNone) ||
		!flight_writer.write_u64(input.sample_time_us)) return Phase4EntityProjectionStatus::EncodingFailure;
	for (const auto value : input.position_world) if (!flight_writer.write_f32(value)) return Phase4EntityProjectionStatus::EncodingFailure;
	for (const auto value : input.orientation_local_to_world) if (!flight_writer.write_f32(value)) return Phase4EntityProjectionStatus::EncodingFailure;
	for (const auto value : input.velocity_world) if (!flight_writer.write_f32(value)) return Phase4EntityProjectionStatus::EncodingFailure;
	for (const auto value : input.rotational_velocity_local) if (!flight_writer.write_f32(value)) return Phase4EntityProjectionStatus::EncodingFailure;
	if (!flight_writer.write_f32(input.radius) || !flight_writer.write_u32(input.physics_mode_flags) ||
		!set_entity_key(flight, RecordType::FlightState, input.entity_id) ||
		!assign_and_validate(flight_writer, flight)) return Phase4EntityProjectionStatus::EncodingFailure;
	return Phase4EntityProjectionStatus::Created;
}

} // namespace telemetry::detail
