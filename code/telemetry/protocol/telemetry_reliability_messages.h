#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::size_t AckPayloadSize = 12;
constexpr std::size_t NackPayloadPrefixSize = 24;
constexpr std::size_t MaxNackBitmapBytes = 256;
constexpr std::size_t ResyncRequestPayloadSize = 24;
constexpr std::size_t SessionEndPayloadSize = 16;

struct AckPayload {
	std::uint32_t target_message_id = 0;
	MessageType target_message_type = MessageType::Invalid;
	std::uint8_t ack_flags = 0;
	std::uint16_t target_fragment_count = 0;
	std::uint32_t target_message_crc32 = 0;
};

struct NackPayload {
	std::uint32_t target_message_id = 0;
	MessageType target_message_type = MessageType::Invalid;
	NackReason reason = NackReason::Invalid;
	std::uint16_t target_fragment_count = 0;
	std::uint32_t target_message_crc32 = 0;
	std::uint64_t needed_before_producer_time_us = 0;
	ByteView missing_bitmap;
};

struct ResyncRequestPayload {
	std::uint32_t request_id = 0;
	ResyncReason reason = ResyncReason::Invalid;
	std::uint8_t request_flags = ResyncRequestFlagNone;
	std::uint32_t last_applied_snapshot_id = 0;
	std::uint32_t last_applied_delta_sequence = 0;
	std::uint64_t client_send_time_us = 0;
};

struct SessionEndPayload {
	SessionEndReason reason = static_cast<SessionEndReason>(0);
	std::uint8_t end_flags = SessionEndFlagNone;
	std::uint32_t last_snapshot_id = 0;
	std::uint64_t producer_sample_time_us = 0;
};

// The tuple repeated by ACK/NACK payloads. session_id and endpoint belong to
// the validated outer datagram/session context and must be checked before this
// payload-only tuple. deadline_bearing is true only for a retained target whose
// logical class permits a producer-clock deadline (an IDR video frame in v1.0).
struct ReliabilityTargetTuple {
	std::uint32_t message_id = 0;
	MessageType message_type = MessageType::Invalid;
	std::uint16_t fragment_count = 0;
	std::uint32_t message_crc32 = 0;
	bool deadline_bearing = false;
};

constexpr std::size_t missing_fragment_bitmap_size(std::uint16_t fragment_count) noexcept
{
	return (static_cast<std::size_t>(fragment_count) + 7U) / 8U;
}

ValidationError validate_missing_fragment_bitmap(ByteView bitmap, std::uint16_t fragment_count) noexcept;

// Initializes a caller-owned bitmap to "no fragments missing". On failure,
// output is unchanged and written is zero.
ValidationError
initialize_missing_fragment_bitmap(std::uint16_t fragment_count, MutableByteView output, std::size_t& written) noexcept;

// Mutates one bit in an already initialized, exactly sized bitmap.
ValidationError set_fragment_missing(MutableByteView bitmap,
	std::uint16_t fragment_count,
	std::uint16_t fragment_index,
	bool missing = true) noexcept;

ValidationError is_fragment_missing(ByteView bitmap,
	std::uint16_t fragment_count,
	std::uint16_t fragment_index,
	bool& missing) noexcept;

// Allocation-free iterator over the set bits of a validated bitmap.
class MissingFragmentIterator {
  public:
	MissingFragmentIterator(ByteView bitmap, std::uint16_t fragment_count) noexcept;

	// On success, has_value is true for an index and false at the exact end.
	ValidationError next(std::uint16_t& fragment_index, bool& has_value) noexcept;
	ValidationError error() const noexcept
	{
		return m_error;
	}

  private:
	ByteView m_bitmap;
	std::uint16_t m_fragment_count = 0;
	std::uint16_t m_next_index = 0;
	ValidationError m_error = ValidationError::None;
};

ValidationError validate_ack_payload(const AckPayload& payload) noexcept;
ValidationError validate_nack_payload(const NackPayload& payload) noexcept;
ValidationError validate_resync_request_payload(const ResyncRequestPayload& payload) noexcept;
ValidationError validate_session_end_payload(const SessionEndPayload& payload) noexcept;

// These helpers validate the payload and then require an exact retained-target
// match. A mismatch is InvalidStateTransition so callers can count and ignore a
// forged, late or incoherent response without mutating their send window.
ValidationError validate_ack_target(const AckPayload& payload, const ReliabilityTargetTuple& target) noexcept;
ValidationError validate_nack_target(const NackPayload& payload, const ReliabilityTargetTuple& target) noexcept;

ValidationError encode_ack_payload(const AckPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_ack_payload(ByteView input, AckPayload& payload) noexcept;

ValidationError encode_nack_payload(const NackPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_nack_payload(ByteView input, NackPayload& payload) noexcept;

ValidationError encode_resync_request_payload(const ResyncRequestPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_resync_request_payload(ByteView input, ResyncRequestPayload& payload) noexcept;

ValidationError
encode_session_end_payload(const SessionEndPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_session_end_payload(ByteView input, SessionEndPayload& payload) noexcept;

} // namespace telemetry::protocol
