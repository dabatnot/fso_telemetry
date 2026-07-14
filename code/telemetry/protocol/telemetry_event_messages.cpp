#include "telemetry/protocol/telemetry_event_messages.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_specialized_views.h"

#include <array>
#include <cstring>

namespace telemetry::protocol {

namespace {

struct EventRange {
	std::uint64_t first_event_id = 0;
	std::uint64_t last_event_id = 0;
	std::uint64_t maximum_sample_time_us = 0;
};

ValidationError comm_view_event_range(ByteView payload, EventRange& range) noexcept
{
	CommViewEventPayload event;
	if (const auto error = decode_comm_view_event_payload(payload, event); error != ValidationError::None) {
		return error;
	}
	range.first_event_id = event.event_id;
	range.last_event_id = event.event_id;
	range.maximum_sample_time_us = event.producer_sample_time_us;
	return ValidationError::None;
}

ValidationError events_record_range(ByteView payload, EventRange& range) noexcept
{
	range = EventRange{};
	PacketReader reader(payload);
	std::uint16_t item_count = 0;
	if (!reader.read_u16(item_count)) {
		return ValidationError::TruncatedPayload;
	}
	if (item_count == 0) {
		return ValidationError::OutOfRange;
	}
	for (std::uint16_t index = 0; index < item_count; ++index) {
		std::uint8_t item_version = 0;
		std::uint16_t item_size = 0;
		ByteView item_bytes;
		if (!reader.read_u8(item_version) || !reader.read_u16(item_size) ||
			!reader.read_bytes(item_size, item_bytes)) {
			return ValidationError::TruncatedPayload;
		}
		if (item_version != 1U) {
			return ValidationError::UnsupportedRecordVersion;
		}
		PacketReader item(item_bytes);
		std::uint32_t presence = 0;
		std::uint64_t event_id = 0;
		std::uint64_t sample_time_us = 0;
		if (!item.read_u32(presence) || !item.read_u64(event_id) || !item.read_u64(sample_time_us)) {
			return ValidationError::TruncatedPayload;
		}
		static_cast<void>(presence);
		if (index == 0) {
			range.first_event_id = event_id;
		} else if (event_id <= range.last_event_id) {
			return ValidationError::InvalidStateTransition;
		}
		range.last_event_id = event_id;
		if (sample_time_us > range.maximum_sample_time_us) {
			range.maximum_sample_time_us = sample_time_us;
		}
	}
	return reader.at_end() ? ValidationError::None : ValidationError::TrailingBytes;
}

ValidationError validate_event_records(const EventBatchPayload& payload) noexcept
{
	const auto container = payload.delivery_class == EventDeliveryClass::Reliable
							   ? BusinessRecordContainer::EventBatchReliable
							   : BusinessRecordContainer::EventBatchReplaceable;
	RecordEnvelopeIterator iterator(payload.records, payload.record_count, RecordFlagPolicy::AllowV1Mutations);
	RecordType batch_record_type = RecordType::Invalid;
	std::uint64_t first_event_id = 0;
	std::uint64_t previous_event_id = 0;
	bool found_known_event = false;
	for (;;) {
		RecordEnvelopeView record;
		bool has_value = false;
		if (const auto error = iterator.next(record, has_value); error != ValidationError::None) {
			return error;
		}
		if (!has_value) {
			break;
		}

		BusinessRecordMetadata metadata;
		if (const auto error = validate_business_record(record, container, metadata);
			error != ValidationError::None) {
			return error;
		}
		if (metadata.type == RecordType::Invalid) {
			// Unknown extension record: structurally bounded and intentionally
			// ignored without weakening the ordering of known v1 events.
			continue;
		}
		if (metadata.type != RecordType::CommViewEvent && metadata.type != RecordType::Events) {
			return ValidationError::InvalidStateTransition;
		}
		if (batch_record_type != RecordType::Invalid && metadata.type != batch_record_type) {
			// A v1 batch never mixes COMM_VIEW_EVENT and EVENTS records.
			return ValidationError::InvalidStateTransition;
		}
		batch_record_type = metadata.type;

		EventRange range;
		const auto range_error = metadata.type == RecordType::CommViewEvent
							 ? comm_view_event_range(record.payload, range)
							 : events_record_range(record.payload, range);
		if (range_error != ValidationError::None) {
			return range_error;
		}
		if (!found_known_event) {
			first_event_id = range.first_event_id;
			found_known_event = true;
		} else if (range.first_event_id <= previous_event_id) {
			return ValidationError::InvalidStateTransition;
		}
		if (range.maximum_sample_time_us > payload.producer_sample_time_us) {
			return ValidationError::OutOfRange;
		}
		previous_event_id = range.last_event_id;
	}
	if (!found_known_event) {
		return ValidationError::UnknownRequiredRecord;
	}
	return payload.first_event_id == first_event_id ? ValidationError::None
										   : ValidationError::InvalidStateTransition;
}

template <std::size_t PrefixSize>
ValidationError publish_payload(const std::array<std::uint8_t, PrefixSize>& prefix,
	ByteView records,
	MutableByteView output,
	std::size_t& written) noexcept
{
	const auto total_size = PrefixSize + records.size;
	if (output.data == nullptr || output.size < total_size) {
		return ValidationError::InternalSerializationError;
	}
	if (!records.empty()) {
		std::memmove(output.data + PrefixSize, records.data, records.size);
	}
	std::memcpy(output.data, prefix.data(), prefix.size());
	written = total_size;
	return ValidationError::None;
}

} // namespace

ValidationError validate_event_batch_payload(const EventBatchPayload& payload, bool ack_required) noexcept
{
	if (payload.batch_id == 0 || payload.first_event_id == 0 || payload.record_count == 0) {
		return ValidationError::OutOfRange;
	}
	if (payload.records.size > MaxStateMessageSize - EventBatchPayloadPrefixSize) {
		return ValidationError::MessageTooLarge;
	}
	switch (payload.delivery_class) {
	case EventDeliveryClass::Replaceable:
		if (ack_required) {
			return ValidationError::InvalidStateTransition;
		}
		break;
	case EventDeliveryClass::Reliable:
		if (!ack_required) {
			return ValidationError::InvalidStateTransition;
		}
		break;
	default:
		return ValidationError::UnknownEnum;
	}
	return validate_event_records(payload);
}

ValidationError encode_event_batch_payload(const EventBatchPayload& payload,
	bool ack_required,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_event_batch_payload(payload, ack_required); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, EventBatchPayloadPrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u32(payload.batch_id) && writer.write_u64(payload.first_event_id) &&
					writer.write_u64(payload.producer_sample_time_us) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.delivery_class)) && writer.write_u8(0) &&
					writer.write_u16(payload.record_count);
	if (!ok || !writer.ok() || writer.size() != EventBatchPayloadPrefixSize) {
		return ValidationError::InternalSerializationError;
	}
	return publish_payload(prefix, payload.records, output, written);
}

ValidationError decode_event_batch_payload(ByteView input,
	bool ack_required,
	EventBatchPayload& payload) noexcept
{
	payload = EventBatchPayload{};
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < EventBatchPayloadPrefixSize) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > MaxStateMessageSize) {
		return ValidationError::MessageTooLarge;
	}

	EventBatchPayload candidate;
	std::uint8_t raw_delivery_class = 0;
	std::uint8_t reserved = 0;
	PacketReader reader(ByteView{input.data, EventBatchPayloadPrefixSize});
	const bool ok = reader.read_u32(candidate.batch_id) && reader.read_u64(candidate.first_event_id) &&
					reader.read_u64(candidate.producer_sample_time_us) && reader.read_u8(raw_delivery_class) &&
					reader.read_u8(reserved) && reader.read_u16(candidate.record_count);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	candidate.delivery_class = static_cast<EventDeliveryClass>(raw_delivery_class);
	candidate.records = ByteView{input.data + EventBatchPayloadPrefixSize, input.size - EventBatchPayloadPrefixSize};
	if (const auto error = validate_event_batch_payload(candidate, ack_required); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

} // namespace telemetry::protocol
