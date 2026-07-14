#include "telemetry/protocol/telemetry_reliability_messages.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_datagram.h"

#include <array>
#include <cstring>

namespace telemetry::protocol {

namespace {

ValidationError validate_input_size(ByteView input, std::size_t expected_size) noexcept
{
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < expected_size) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > expected_size) {
		return ValidationError::TrailingBytes;
	}
	return ValidationError::None;
}

template <std::size_t Size>
ValidationError publish_fixed_payload(const std::array<std::uint8_t, Size>& encoded,
	MutableByteView output,
	std::size_t& written) noexcept
{
	if (output.data == nullptr || output.size < encoded.size()) {
		return ValidationError::InternalSerializationError;
	}
	std::memcpy(output.data, encoded.data(), encoded.size());
	written = encoded.size();
	return ValidationError::None;
}

template <std::size_t PrefixSize>
ValidationError publish_payload_with_suffix(const std::array<std::uint8_t, PrefixSize>& prefix,
	ByteView suffix,
	MutableByteView output,
	std::size_t& written) noexcept
{
	const auto total_size = PrefixSize + suffix.size;
	if (output.data == nullptr || output.size < total_size) {
		return ValidationError::InternalSerializationError;
	}

	// Copy the suffix first so encoding is defined even when it aliases output.
	if (suffix.size != 0) {
		std::memmove(output.data + PrefixSize, suffix.data, suffix.size);
	}
	std::memcpy(output.data, prefix.data(), prefix.size());
	written = total_size;
	return ValidationError::None;
}

template <std::size_t Size>
bool is_complete_prefix(const PacketWriter& writer) noexcept
{
	return writer.ok() && writer.size() == Size;
}

bool is_known_nack_reason(NackReason reason) noexcept
{
	switch (reason) {
	case NackReason::MissingFragments:
	case NackReason::BadMessageCrc:
	case NackReason::StaleBaseline:
	case NackReason::BadFragmentLayout:
	case NackReason::ResourceLimit:
	case NackReason::UnsupportedMessage:
	case NackReason::SemanticValidationFailed:
	case NackReason::DeadlineExpired:
		return true;
	case NackReason::Invalid:
		return false;
	}
	return false;
}

bool is_known_resync_reason(ResyncReason reason) noexcept
{
	switch (reason) {
	case ResyncReason::UnknownBaseline:
	case ResyncReason::ValidationFailed:
	case ResyncReason::ReassemblyTimeout:
	case ResyncReason::SessionStale:
	case ResyncReason::Manual:
		return true;
	case ResyncReason::Invalid:
		return false;
	}
	return false;
}

bool is_known_session_end_reason(SessionEndReason reason) noexcept
{
	switch (reason) {
	case SessionEndReason::Normal:
	case SessionEndReason::ProducerShutdown:
	case SessionEndReason::MissionEnded:
	case SessionEndReason::Restart:
	case SessionEndReason::ProtocolError:
	case SessionEndReason::Timeout:
		return true;
	}
	return false;
}

ValidationError validate_target_identity(std::uint32_t message_id,
	MessageType message_type,
	std::uint16_t fragment_count,
	bool allow_unknown_type) noexcept
{
	if (message_id == 0) {
		return ValidationError::OutOfRange;
	}
	if (message_type == MessageType::Invalid) {
		return ValidationError::UnknownMessageType;
	}
	if (!is_known_message_type(message_type) && !allow_unknown_type) {
		return ValidationError::UnknownMessageType;
	}
	if (fragment_count == 0) {
		return ValidationError::BadFragmentCount;
	}
	if (is_known_message_type(message_type) && fragment_count > max_fragment_count(message_type)) {
		return ValidationError::BadFragmentCount;
	}
	if (is_known_message_type(message_type) && fragment_count > 1 && !message_type_allows_fragmentation(message_type)) {
		return ValidationError::BadFragmentCount;
	}
	return ValidationError::None;
}

ValidationError validate_reliability_target(const ReliabilityTargetTuple& target) noexcept
{
	if (const auto error =
			validate_target_identity(target.message_id, target.message_type, target.fragment_count, true);
		error != ValidationError::None) {
		return error;
	}
	if (target.deadline_bearing && target.message_type != MessageType::TargetVideoFrame) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

bool tuple_matches(const AckPayload& payload, const ReliabilityTargetTuple& target) noexcept
{
	return payload.target_message_id == target.message_id && payload.target_message_type == target.message_type &&
		   payload.target_fragment_count == target.fragment_count &&
		   payload.target_message_crc32 == target.message_crc32;
}

bool tuple_matches(const NackPayload& payload, const ReliabilityTargetTuple& target) noexcept
{
	return payload.target_message_id == target.message_id && payload.target_message_type == target.message_type &&
		   payload.target_fragment_count == target.fragment_count &&
		   payload.target_message_crc32 == target.message_crc32;
}

} // namespace

ValidationError validate_missing_fragment_bitmap(ByteView bitmap, std::uint16_t fragment_count) noexcept
{
	if (fragment_count == 0) {
		return ValidationError::BadFragmentCount;
	}
	const auto expected_size = missing_fragment_bitmap_size(fragment_count);
	if (expected_size > MaxNackBitmapBytes) {
		return ValidationError::BadFragmentCount;
	}
	if (bitmap.size != 0 && bitmap.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (bitmap.size < expected_size) {
		return ValidationError::TruncatedPayload;
	}
	if (bitmap.size > expected_size) {
		return ValidationError::TrailingBytes;
	}

	const auto used_bits_in_tail = static_cast<std::uint8_t>(fragment_count & 7U);
	if (used_bits_in_tail != 0) {
		const auto reserved_mask = static_cast<std::uint8_t>(0xffU << used_bits_in_tail);
		if ((bitmap.data[bitmap.size - 1] & reserved_mask) != 0) {
			return ValidationError::ReservedFlag;
		}
	}
	return ValidationError::None;
}

ValidationError
initialize_missing_fragment_bitmap(std::uint16_t fragment_count, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (fragment_count == 0) {
		return ValidationError::BadFragmentCount;
	}
	const auto required_size = missing_fragment_bitmap_size(fragment_count);
	if (required_size > MaxNackBitmapBytes) {
		return ValidationError::BadFragmentCount;
	}
	if (output.data == nullptr || output.size < required_size) {
		return ValidationError::InternalSerializationError;
	}
	std::memset(output.data, 0, required_size);
	written = required_size;
	return ValidationError::None;
}

ValidationError set_fragment_missing(MutableByteView bitmap,
	std::uint16_t fragment_count,
	std::uint16_t fragment_index,
	bool missing) noexcept
{
	if (fragment_index >= fragment_count) {
		return ValidationError::BadFragmentIndex;
	}
	if (const auto error = validate_missing_fragment_bitmap(bitmap, fragment_count); error != ValidationError::None) {
		return error;
	}
	const auto byte_index = static_cast<std::size_t>(fragment_index / 8U);
	const auto bit_mask = static_cast<std::uint8_t>(1U << (fragment_index & 7U));
	if (missing) {
		bitmap.data[byte_index] = static_cast<std::uint8_t>(bitmap.data[byte_index] | bit_mask);
	} else {
		bitmap.data[byte_index] = static_cast<std::uint8_t>(bitmap.data[byte_index] & ~bit_mask);
	}
	return ValidationError::None;
}

ValidationError
is_fragment_missing(ByteView bitmap, std::uint16_t fragment_count, std::uint16_t fragment_index, bool& missing) noexcept
{
	missing = false;
	if (fragment_index >= fragment_count) {
		return ValidationError::BadFragmentIndex;
	}
	if (const auto error = validate_missing_fragment_bitmap(bitmap, fragment_count); error != ValidationError::None) {
		return error;
	}
	const auto byte_index = static_cast<std::size_t>(fragment_index / 8U);
	const auto bit_mask = static_cast<std::uint8_t>(1U << (fragment_index & 7U));
	missing = (bitmap.data[byte_index] & bit_mask) != 0;
	return ValidationError::None;
}

MissingFragmentIterator::MissingFragmentIterator(ByteView bitmap, std::uint16_t fragment_count) noexcept
	: m_bitmap(bitmap), m_fragment_count(fragment_count)
{
	m_error = validate_missing_fragment_bitmap(bitmap, fragment_count);
}

ValidationError MissingFragmentIterator::next(std::uint16_t& fragment_index, bool& has_value) noexcept
{
	fragment_index = 0;
	has_value = false;
	if (m_error != ValidationError::None) {
		return m_error;
	}
	while (m_next_index < m_fragment_count) {
		const auto candidate = m_next_index++;
		const auto byte_index = static_cast<std::size_t>(candidate / 8U);
		const auto bit_mask = static_cast<std::uint8_t>(1U << (candidate & 7U));
		if ((m_bitmap.data[byte_index] & bit_mask) != 0) {
			fragment_index = candidate;
			has_value = true;
			return ValidationError::None;
		}
	}
	return ValidationError::None;
}

ValidationError validate_ack_payload(const AckPayload& payload) noexcept
{
	if (const auto error = validate_target_identity(payload.target_message_id,
			payload.target_message_type,
			payload.target_fragment_count,
			false);
		error != ValidationError::None) {
		return error;
	}
	if ((payload.ack_flags & ~KnownAckFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	if (payload.ack_flags != static_cast<std::uint8_t>(AckFlag::Validated) && payload.ack_flags != KnownAckFlags) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError validate_nack_payload(const NackPayload& payload) noexcept
{
	if (!is_known_nack_reason(payload.reason)) {
		return ValidationError::UnknownEnum;
	}
	const auto is_unsupported = payload.reason == NackReason::UnsupportedMessage;
	if (const auto error = validate_target_identity(payload.target_message_id,
			payload.target_message_type,
			payload.target_fragment_count,
			is_unsupported);
		error != ValidationError::None) {
		return error;
	}
	if (is_unsupported == is_known_message_type(payload.target_message_type)) {
		return ValidationError::InvalidStateTransition;
	}

	if (payload.reason == NackReason::MissingFragments) {
		if (const auto error = validate_missing_fragment_bitmap(payload.missing_bitmap, payload.target_fragment_count);
			error != ValidationError::None) {
			return error;
		}
	} else if (!payload.missing_bitmap.empty()) {
		return ValidationError::InvalidStateTransition;
	}

	// In v1.0, the only logical message class carrying a producer-time
	// retransmission deadline is an IDR TargetVideoFrame. IDR itself is outer
	// message metadata, so validate_nack_target performs the final check.
	if (payload.needed_before_producer_time_us != 0 && payload.target_message_type != MessageType::TargetVideoFrame) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError validate_resync_request_payload(const ResyncRequestPayload& payload) noexcept
{
	if (payload.request_id == 0) {
		return ValidationError::OutOfRange;
	}
	if (!is_known_resync_reason(payload.reason)) {
		return ValidationError::UnknownEnum;
	}
	if ((payload.request_flags & ~KnownResyncRequestFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	if ((payload.request_flags & ResyncRequestFlagRequireFullSnapshot) == 0) {
		return ValidationError::InvalidStateTransition;
	}
	if (payload.last_applied_snapshot_id == 0 && payload.last_applied_delta_sequence != 0) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError validate_session_end_payload(const SessionEndPayload& payload) noexcept
{
	if (!is_known_session_end_reason(payload.reason)) {
		return ValidationError::UnknownEnum;
	}
	if ((payload.end_flags & ~KnownSessionEndFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	return ValidationError::None;
}

ValidationError validate_ack_target(const AckPayload& payload, const ReliabilityTargetTuple& target) noexcept
{
	if (const auto error = validate_ack_payload(payload); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_reliability_target(target); error != ValidationError::None) {
		return error;
	}
	return tuple_matches(payload, target) ? ValidationError::None : ValidationError::InvalidStateTransition;
}

ValidationError validate_nack_target(const NackPayload& payload, const ReliabilityTargetTuple& target) noexcept
{
	if (const auto error = validate_nack_payload(payload); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_reliability_target(target); error != ValidationError::None) {
		return error;
	}
	if (!tuple_matches(payload, target)) {
		return ValidationError::InvalidStateTransition;
	}
	if (payload.needed_before_producer_time_us != 0 && !target.deadline_bearing) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError encode_ack_payload(const AckPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_ack_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, AckPayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u32(payload.target_message_id) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.target_message_type)) &&
					writer.write_u8(payload.ack_flags) && writer.write_u16(payload.target_fragment_count) &&
					writer.write_u32(payload.target_message_crc32);
	if (!ok || !is_complete_prefix<AckPayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed_payload(encoded, output, written);
}

ValidationError decode_ack_payload(ByteView input, AckPayload& payload) noexcept
{
	payload = AckPayload{};
	if (const auto error = validate_input_size(input, AckPayloadSize); error != ValidationError::None) {
		return error;
	}

	AckPayload candidate;
	std::uint8_t raw_message_type = 0;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.target_message_id) && reader.read_u8(raw_message_type) &&
					reader.read_u8(candidate.ack_flags) && reader.read_u16(candidate.target_fragment_count) &&
					reader.read_u32(candidate.target_message_crc32);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	candidate.target_message_type = static_cast<MessageType>(raw_message_type);
	if (const auto error = validate_ack_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_nack_payload(const NackPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_nack_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, NackPayloadPrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u32(payload.target_message_id) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.target_message_type)) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.reason)) &&
					writer.write_u16(payload.target_fragment_count) && writer.write_u32(payload.target_message_crc32) &&
					writer.write_u64(payload.needed_before_producer_time_us) &&
					writer.write_u16(static_cast<std::uint16_t>(payload.missing_bitmap.size)) && writer.write_u16(0);
	if (!ok || !is_complete_prefix<NackPayloadPrefixSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_payload_with_suffix(prefix, payload.missing_bitmap, output, written);
}

ValidationError decode_nack_payload(ByteView input, NackPayload& payload) noexcept
{
	payload = NackPayload{};
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < NackPayloadPrefixSize) {
		return ValidationError::TruncatedPayload;
	}

	NackPayload candidate;
	std::uint8_t raw_message_type = 0;
	std::uint8_t raw_reason = 0;
	std::uint16_t bitmap_bytes = 0;
	std::uint16_t reserved = 0;
	PacketReader reader(ByteView{input.data, NackPayloadPrefixSize});
	const bool ok = reader.read_u32(candidate.target_message_id) && reader.read_u8(raw_message_type) &&
					reader.read_u8(raw_reason) && reader.read_u16(candidate.target_fragment_count) &&
					reader.read_u32(candidate.target_message_crc32) &&
					reader.read_u64(candidate.needed_before_producer_time_us) && reader.read_u16(bitmap_bytes) &&
					reader.read_u16(reserved);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (bitmap_bytes > MaxNackBitmapBytes) {
		return ValidationError::OutOfRange;
	}
	const auto expected_size = NackPayloadPrefixSize + static_cast<std::size_t>(bitmap_bytes);
	if (input.size < expected_size) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > expected_size) {
		return ValidationError::TrailingBytes;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}

	candidate.target_message_type = static_cast<MessageType>(raw_message_type);
	candidate.reason = static_cast<NackReason>(raw_reason);
	candidate.missing_bitmap =
		bitmap_bytes == 0 ? ByteView{} : ByteView{input.data + NackPayloadPrefixSize, bitmap_bytes};
	if (const auto error = validate_nack_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_resync_request_payload(const ResyncRequestPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_resync_request_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, ResyncRequestPayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok =
		writer.write_u32(payload.request_id) && writer.write_u8(static_cast<std::uint8_t>(payload.reason)) &&
		writer.write_u8(payload.request_flags) && writer.write_u16(0) &&
		writer.write_u32(payload.last_applied_snapshot_id) && writer.write_u32(payload.last_applied_delta_sequence) &&
		writer.write_u64(payload.client_send_time_us);
	if (!ok || !is_complete_prefix<ResyncRequestPayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed_payload(encoded, output, written);
}

ValidationError decode_resync_request_payload(ByteView input, ResyncRequestPayload& payload) noexcept
{
	payload = ResyncRequestPayload{};
	if (const auto error = validate_input_size(input, ResyncRequestPayloadSize); error != ValidationError::None) {
		return error;
	}

	ResyncRequestPayload candidate;
	std::uint8_t raw_reason = 0;
	std::uint16_t reserved = 0;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.request_id) && reader.read_u8(raw_reason) &&
					reader.read_u8(candidate.request_flags) && reader.read_u16(reserved) &&
					reader.read_u32(candidate.last_applied_snapshot_id) &&
					reader.read_u32(candidate.last_applied_delta_sequence) &&
					reader.read_u64(candidate.client_send_time_us);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	candidate.reason = static_cast<ResyncReason>(raw_reason);
	if (const auto error = validate_resync_request_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError
encode_session_end_payload(const SessionEndPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_session_end_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, SessionEndPayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u8(static_cast<std::uint8_t>(payload.reason)) && writer.write_u8(payload.end_flags) &&
					writer.write_u16(0) && writer.write_u32(payload.last_snapshot_id) &&
					writer.write_u64(payload.producer_sample_time_us);
	if (!ok || !is_complete_prefix<SessionEndPayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed_payload(encoded, output, written);
}

ValidationError decode_session_end_payload(ByteView input, SessionEndPayload& payload) noexcept
{
	payload = SessionEndPayload{};
	if (const auto error = validate_input_size(input, SessionEndPayloadSize); error != ValidationError::None) {
		return error;
	}

	SessionEndPayload candidate;
	std::uint8_t raw_reason = 0;
	std::uint16_t reserved = 0;
	PacketReader reader(input);
	const bool ok = reader.read_u8(raw_reason) && reader.read_u8(candidate.end_flags) && reader.read_u16(reserved) &&
					reader.read_u32(candidate.last_snapshot_id) && reader.read_u64(candidate.producer_sample_time_us);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	candidate.reason = static_cast<SessionEndReason>(raw_reason);
	if (const auto error = validate_session_end_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

} // namespace telemetry::protocol
