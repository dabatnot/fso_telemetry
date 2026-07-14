#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::size_t EventBatchPayloadPrefixSize = 24;

struct EventBatchPayload {
	std::uint32_t batch_id = 0;
	std::uint64_t first_event_id = 0;
	std::uint64_t producer_sample_time_us = 0;
	EventDeliveryClass delivery_class = EventDeliveryClass::Replaceable;
	std::uint16_t record_count = 0;
	ByteView records;
};

// ACK_REQUIRED is a header-level property whose required value is determined
// by delivery_class. Pass it explicitly so a payload can never be accepted in
// isolation with contradictory reliability semantics.
ValidationError validate_event_batch_payload(const EventBatchPayload& payload, bool ack_required) noexcept;

ValidationError encode_event_batch_payload(const EventBatchPayload& payload,
	bool ack_required,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_event_batch_payload(ByteView input,
	bool ack_required,
	EventBatchPayload& payload) noexcept;

} // namespace telemetry::protocol
