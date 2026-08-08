#include "telemetry/protocol/telemetry_business_records.h"

#include "telemetry/protocol/telemetry_business_records_internal.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_specialized_views.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace telemetry::protocol {

namespace {

constexpr bool is_known_container(BusinessRecordContainer container) noexcept
{
	return container == BusinessRecordContainer::Manifest || container == BusinessRecordContainer::FullSnapshot ||
		   container == BusinessRecordContainer::Delta ||
		   container == BusinessRecordContainer::EventBatchReplaceable ||
		   container == BusinessRecordContainer::EventBatchReliable;
}

constexpr bool is_manifest_record(RecordType type) noexcept
{
	return type == RecordType::ClassManifest || type == RecordType::WeaponManifest ||
		   type == RecordType::CommAssetManifest;
}

constexpr bool is_state_record(RecordType type) noexcept
{
	const auto value = static_cast<std::uint16_t>(type);
	return type == RecordType::SessionState || type == RecordType::MissionState ||
		   (value >= static_cast<std::uint16_t>(RecordType::EntityLifecycle) &&
			   value <= static_cast<std::uint16_t>(RecordType::EffectState)) ||
		   type == RecordType::CommViewState;
}

constexpr bool is_explicit_lifecycle_record(RecordType type) noexcept
{
	return type == RecordType::EntityLifecycle || type == RecordType::SubsystemState ||
		   type == RecordType::RadarContacts;
}

constexpr std::size_t key_size_for(RecordType type) noexcept
{
	switch (type) {
	case RecordType::SessionState:
	case RecordType::MissionState:
	case RecordType::CommViewState:
		return 0;
	case RecordType::ClassManifest:
	case RecordType::WeaponManifest:
		return 8;
	case RecordType::SubsystemState:
		return 12;
	case RecordType::RadarContacts:
		return 16;
	case RecordType::EntityLifecycle:
	case RecordType::ShipIdentity:
	case RecordType::FlightState:
	case RecordType::ControlState:
	case RecordType::DamageState:
	case RecordType::ShieldState:
	case RecordType::EnergyState:
	case RecordType::PropulsionState:
	case RecordType::WeaponState:
	case RecordType::LockState:
	case RecordType::TargetState:
	case RecordType::RadarState:
	case RecordType::ThreatState:
	case RecordType::CargoScanState:
	case RecordType::DockingState:
	case RecordType::SupportState:
	case RecordType::NavigationState:
	case RecordType::EffectState:
		return 8;
	default:
		return 0;
	}
}

ValidationError validate_container_and_flags(RecordType type,
	std::uint8_t flags,
	BusinessRecordContainer container) noexcept
{
	if (!is_known_container(container)) {
		return ValidationError::InvalidStateTransition;
	}
	if (is_manifest_record(type)) {
		return container == BusinessRecordContainer::Manifest && flags == RecordFlagNone
			   ? ValidationError::None
			   : ValidationError::InvalidStateTransition;
	}
	if (is_state_record(type)) {
		if (container != BusinessRecordContainer::FullSnapshot && container != BusinessRecordContainer::Delta) {
			return ValidationError::InvalidStateTransition;
		}
		if (container == BusinessRecordContainer::FullSnapshot) {
			return flags == RecordFlagNone ? ValidationError::None : ValidationError::InvalidStateTransition;
		}
		if (!is_explicit_lifecycle_record(type) && flags != RecordFlagNone) {
			return ValidationError::InvalidStateTransition;
		}
		return ValidationError::None;
	}
	if (type == RecordType::CommViewEvent) {
		return container == BusinessRecordContainer::EventBatchReliable && flags == RecordFlagCreate
			   ? ValidationError::None
			   : ValidationError::InvalidStateTransition;
	}
	if (type == RecordType::Events) {
		return (container == BusinessRecordContainer::EventBatchReliable ||
				   container == BusinessRecordContainer::EventBatchReplaceable) &&
				  flags == RecordFlagCreate
			   ? ValidationError::None
			   : ValidationError::InvalidStateTransition;
	}
	return ValidationError::UnknownRequiredRecord;
}

ValidationError validate_payload(RecordType type,
	std::uint8_t record_version,
	ByteView payload,
	BusinessRecordContainer container,
	std::uint8_t protocol_minor) noexcept
{
	const auto value = static_cast<std::uint16_t>(type);
	if (value >= 1 && value <= 10) {
		return detail::validate_business_record_1_10(type, payload, protocol_minor);
	}
	if (value >= 11 && value <= 18) {
		return detail::validate_business_record_11_18(
			type, record_version, payload);
	}
	if (value >= 19 && value <= 24) {
		return detail::validate_business_record_19_24(type, payload);
	}
	if (type == RecordType::CommAssetManifest) {
		return validate_comm_asset_manifest_record_payload(payload);
	}
	if (type == RecordType::CommViewState) {
		return validate_comm_view_state_record_payload(payload);
	}
	if (type == RecordType::CommViewEvent) {
		return validate_comm_view_event_record_payload(payload);
	}
	if (type == RecordType::Events) {
		return detail::validate_business_record_28(payload, container == BusinessRecordContainer::EventBatchReliable);
	}
	return ValidationError::UnknownRequiredRecord;
}

} // namespace

bool business_record_metadata(std::uint16_t raw_record_type, BusinessRecordMetadata& metadata) noexcept
{
	metadata = BusinessRecordMetadata{};
	if (raw_record_type == 0 || raw_record_type >= FirstReservedRecordType) {
		return false;
	}
	const auto type = static_cast<RecordType>(raw_record_type);
	metadata.type = type;
	metadata.key_size = key_size_for(type);
	metadata.state_atom = is_state_record(type);
	metadata.lifecycle = is_explicit_lifecycle_record(type) ? StateRecordLifecycle::ExplicitCreateDelete
												 : StateRecordLifecycle::UpsertOnly;
	const auto value = static_cast<std::uint16_t>(type);
	metadata.cascades_with_entity = value >= static_cast<std::uint16_t>(RecordType::ShipIdentity) &&
									 value <= static_cast<std::uint16_t>(RecordType::EffectState);
	return true;
}

ValidationError validate_business_record(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	BusinessRecordMetadata& metadata) noexcept
{
	return validate_business_record(record, container, VersionMinor, metadata);
}

ValidationError validate_business_record(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	std::uint8_t protocol_minor,
	BusinessRecordMetadata& metadata) noexcept
{
	metadata = BusinessRecordMetadata{};
	if (!is_supported_version_minor(protocol_minor)) {
		return ValidationError::UnsupportedMinor;
	}
	if (record.payload.size > std::numeric_limits<std::uint16_t>::max() ||
		(record.payload.size != 0 && record.payload.data == nullptr)) {
		return ValidationError::BadRecordLength;
	}
	BusinessRecordMetadata candidate;
	if (!business_record_metadata(record.raw_record_type, candidate)) {
		return record.raw_record_type == 0 ? ValidationError::OutOfRange : ValidationError::None;
	}
	const auto phase3_v2_or_v3 =
		(candidate.type == RecordType::RadarContacts ||
		 candidate.type == RecordType::TargetState) &&
		(record.record_version == 2U ||
		 record.record_version == 3U) &&
		protocol_minor >= VersionMinorV1_1;
	if (record.record_version != 1U && !phase3_v2_or_v3) {
		return ValidationError::UnsupportedRecordVersion;
	}
	if (const auto error = validate_record_flags_v1(record.record_flags, RecordFlagPolicy::AllowV1Mutations);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_container_and_flags(candidate.type, record.record_flags, container);
		error != ValidationError::None) {
		return error;
	}
	if ((record.record_flags & RecordFlagDelete) != 0) {
		if (record.payload.size != candidate.key_size || (record.payload.size != 0 && record.payload.data == nullptr)) {
			return ValidationError::BadRecordLength;
		}
	} else if (const auto error = validate_payload(candidate.type,
				   record.record_version, record.payload, container,
				   protocol_minor);
			   error != ValidationError::None) {
		return error;
	}
	metadata = candidate;
	return ValidationError::None;
}

ValidationError encode_business_record(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	MutableByteView output,
	std::size_t& written) noexcept
{
	return encode_business_record(record, container, VersionMinor, output, written);
}

ValidationError encode_business_record(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	std::uint8_t protocol_minor,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	BusinessRecordMetadata metadata;
	if (const auto error = validate_business_record(record, container, protocol_minor, metadata);
		error != ValidationError::None) {
		return error;
	}
	if (metadata.type == RecordType::Invalid) {
		return ValidationError::UnknownRequiredRecord;
	}

	const auto total_size = RecordEnvelopeHeaderSize + record.payload.size;
	if (output.data == nullptr || output.size < total_size) {
		return ValidationError::InternalSerializationError;
	}

	std::array<std::uint8_t, RecordEnvelopeHeaderSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u16(record.raw_record_type) && writer.write_u8(record.record_version) &&
					writer.write_u8(record.record_flags) &&
					writer.write_u16(static_cast<std::uint16_t>(record.payload.size));
	if (!ok || !writer.ok() || writer.size() != RecordEnvelopeHeaderSize) {
		return ValidationError::InternalSerializationError;
	}

	// Move the payload first so overlapping/in-place encodes are deterministic.
	if (record.payload.size != 0) {
		std::memmove(output.data + RecordEnvelopeHeaderSize, record.payload.data, record.payload.size);
	}
	std::memcpy(output.data, prefix.data(), prefix.size());
	written = total_size;
	return ValidationError::None;
}

ValidationError decode_business_state_atom(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	StateAtom& output) noexcept
{
	return decode_business_state_atom(record, container, VersionMinor, output);
}

ValidationError decode_business_state_atom(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	std::uint8_t protocol_minor,
	StateAtom& output) noexcept
{
	BusinessRecordMetadata metadata;
	if (const auto error = validate_business_record(record, container, protocol_minor, metadata);
		error != ValidationError::None) {
		return error;
	}
	if (!metadata.state_atom || (container != BusinessRecordContainer::FullSnapshot &&
							   container != BusinessRecordContainer::Delta)) {
		return ValidationError::InvalidStateTransition;
	}
	if (record.payload.size < metadata.key_size || (record.payload.size != 0 && record.payload.data == nullptr)) {
		return ValidationError::BadRecordLength;
	}

	try {
		StateAtom candidate;
		candidate.key.record_type = record.raw_record_type;
		candidate.key.identity.assign(record.payload.data, record.payload.data + metadata.key_size);
		candidate.record_version = record.record_version;
		candidate.lifecycle = metadata.lifecycle;
		if ((record.record_flags & RecordFlagDelete) == 0) {
			candidate.value.assign(record.payload.data, record.payload.data + record.payload.size);
			if (metadata.cascades_with_entity) {
				candidate.has_cascade_owner = true;
				candidate.cascade_owner.record_type = static_cast<std::uint16_t>(RecordType::EntityLifecycle);
				candidate.cascade_owner.identity.assign(record.payload.data, record.payload.data + 8U);
			}
		}
		output = std::move(candidate);
	} catch (const std::bad_alloc&) {
		return ValidationError::ResourceLimit;
	}
	return ValidationError::None;
}

namespace {

ValidationError decode_business_snapshot_region_impl(ByteView records,
	std::uint16_t record_count,
	const StateImageValidator* validator,
	StateImage& output) noexcept
{
	const auto protocol_minor = validator == nullptr ? VersionMinor : validator->protocol_minor();
	if (!is_supported_version_minor(protocol_minor)) {
		return ValidationError::UnsupportedMinor;
	}
	if (const auto error = validate_record_region(records, record_count, RecordFlagPolicy::RequireNone);
		error != ValidationError::None) {
		return error;
	}
	try {
		std::vector<StateAtom> atoms;
		atoms.reserve(record_count);
		RecordEnvelopeIterator iterator(records, record_count, RecordFlagPolicy::RequireNone);
		for (;;) {
			RecordEnvelopeView envelope;
			bool has_value = false;
			if (const auto error = iterator.next(envelope, has_value); error != ValidationError::None) {
				return error;
			}
			if (!has_value) {
				break;
			}
			BusinessRecordMetadata metadata;
			if (!business_record_metadata(envelope.raw_record_type, metadata)) {
				continue;
			}
			if (!metadata.state_atom) {
				return ValidationError::InvalidStateTransition;
			}
			StateAtom atom;
			if (const auto error = decode_business_state_atom(
					envelope, BusinessRecordContainer::FullSnapshot, protocol_minor, atom);
				error != ValidationError::None) {
				return error;
			}
			atoms.push_back(std::move(atom));
		}
		StateImage candidate;
		StateImageInvalidRecordReason invalid_record_reason = StateImageInvalidRecordReason::None;
		switch (StateImage::create(std::move(atoms), candidate, invalid_record_reason)) {
		case StateImageResult::Created:
			if (validator != nullptr) {
				if (const auto error = validator->validate(candidate); error != ValidationError::None) {
					return error;
				}
			}
			output = std::move(candidate);
			return ValidationError::None;
		case StateImageResult::DuplicateKey:
			return ValidationError::DuplicateRecord;
		case StateImageResult::SizeLimitExceeded:
			return ValidationError::ResourceLimit;
		case StateImageResult::AllocationFailed:
			return ValidationError::ResourceLimit;
		case StateImageResult::InvalidRecord:
			if (validator != nullptr && protocol_minor == VersionMinorV1_1 &&
				invalid_record_reason == StateImageInvalidRecordReason::MissingCascadeOwner) {
				return ValidationError::InvalidAbsence;
			}
			return ValidationError::BadRecordLength;
		default:
			return ValidationError::BadRecordLength;
		}
	} catch (const std::bad_alloc&) {
		return ValidationError::ResourceLimit;
	}
}

} // namespace

ValidationError decode_business_snapshot_region(ByteView records,
	std::uint16_t record_count,
	StateImage& output) noexcept
{
	return decode_business_snapshot_region_impl(records, record_count, nullptr, output);
}

ValidationError decode_business_snapshot_region_validated(ByteView records,
	std::uint16_t record_count,
	const StateImageValidator& validator,
	StateImage& output) noexcept
{
	return decode_business_snapshot_region_impl(records, record_count, &validator, output);
}

ValidationError decode_business_delta(const DeltaPayload& payload, CumulativeStateDelta& output) noexcept
{
	return decode_business_delta(payload, VersionMinor, output);
}

ValidationError decode_business_delta(const DeltaPayload& payload,
	std::uint8_t protocol_minor,
	CumulativeStateDelta& output) noexcept
{
	if (!is_supported_version_minor(protocol_minor)) {
		return ValidationError::UnsupportedMinor;
	}
	if (const auto error = validate_delta_payload(payload); error != ValidationError::None) {
		return error;
	}
	try {
		CumulativeStateDelta candidate;
		candidate.baseline_snapshot_id = payload.baseline_snapshot_id;
		candidate.delta_sequence = payload.delta_sequence;
		candidate.producer_sample_time_us = payload.producer_sample_time_us;
		candidate.mutations.reserve(payload.record_count);
		RecordEnvelopeIterator iterator(payload.records, payload.record_count, RecordFlagPolicy::AllowV1Mutations);
		for (;;) {
			RecordEnvelopeView envelope;
			bool has_value = false;
			if (const auto error = iterator.next(envelope, has_value); error != ValidationError::None) {
				return error;
			}
			if (!has_value) {
				break;
			}
			BusinessRecordMetadata metadata;
			if (!business_record_metadata(envelope.raw_record_type, metadata)) {
				continue;
			}
			if (!metadata.state_atom) {
				return ValidationError::InvalidStateTransition;
			}
			StateMutation mutation;
			mutation.kind = (envelope.record_flags & RecordFlagCreate) != 0
							? StateMutationKind::Create
							: ((envelope.record_flags & RecordFlagDelete) != 0 ? StateMutationKind::Delete
																	  : StateMutationKind::Upsert);
			if (const auto error = decode_business_state_atom(
					envelope, BusinessRecordContainer::Delta, protocol_minor, mutation.atom);
				error != ValidationError::None) {
				return error;
			}
			candidate.mutations.push_back(std::move(mutation));
		}
		std::sort(candidate.mutations.begin(), candidate.mutations.end(), [](const StateMutation& left,
																			  const StateMutation& right) {
			return left.atom.key < right.atom.key;
		});
		if (validate_cumulative_state_delta(candidate) != StateDeltaValidationResult::Valid) {
			return ValidationError::InvalidStateTransition;
		}
		output = std::move(candidate);
		return ValidationError::None;
	} catch (const std::bad_alloc&) {
		return ValidationError::ResourceLimit;
	}
}

} // namespace telemetry::protocol
