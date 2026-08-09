#include "telemetry/protocol/telemetry_datagram.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_crc32.h"

#include <array>
#include <cstring>
#include <limits>

namespace telemetry::protocol {

namespace {

constexpr bool has_flag(std::uint8_t flags, MessageFlag flag) noexcept {
	return (flags & static_cast<std::uint8_t>(flag)) != 0;
}

ValidationError validate_type_and_flags(const TelemetryDatagramHeader& header) noexcept {
	if ((header.flags & ReservedMessageFlags) != 0) {
		return ValidationError::ReservedHeaderFlag;
	}
	if (!is_known_message_type(header.message_type)) {
		return ValidationError::UnknownMessageType;
	}

	const auto fragmented = has_flag(header.flags, MessageFlagFragmented);
	const auto ack_required = has_flag(header.flags, MessageFlagAckRequired);
	const auto keyframe = has_flag(header.flags, MessageFlagKeyframe);
	const auto video_idr = has_flag(header.flags, MessageFlagVideoIdr);
	const auto retransmission = has_flag(header.flags, MessageFlagRetransmission);

	if (fragmented != (header.fragment_count > 1)) {
		return ValidationError::BadFragmentCount;
	}
	if (keyframe && video_idr) {
		return ValidationError::ReservedHeaderFlag;
	}
	if (keyframe != (header.message_type == MessageType::FullSnapshot)) {
		return ValidationError::ReservedHeaderFlag;
	}
	if (video_idr && header.message_type != MessageType::TargetVideoFrame) {
		return ValidationError::ReservedHeaderFlag;
	}
	if (fragmented && !message_type_allows_fragmentation(header.message_type)) {
		return ValidationError::BadFragmentCount;
	}

	// Welcome and EventBatch depend on their payload status/delivery class and
	// are completed by the corresponding logical-message validator. Every
	// other v1.0 class has a fixed ACK policy in the header matrix.
	switch (header.message_type) {
	case MessageType::SessionBegin:
	case MessageType::Manifest:
	case MessageType::FullSnapshot:
	case MessageType::ResyncRequest:
	case MessageType::SessionEnd:
	case MessageType::TargetVideoSubscribe:
	case MessageType::TargetVideoConfig:
	case MessageType::TargetVideoKeyframeRequest:
	case MessageType::TargetVideoStop:
	case MessageType::CapabilityUpdate:
		if (!ack_required) {
			return ValidationError::ReservedHeaderFlag;
		}
		break;
	case MessageType::Discovery:
	case MessageType::Hello:
	case MessageType::Delta:
	case MessageType::Heartbeat:
	case MessageType::Ack:
	case MessageType::Nack:
	case MessageType::TargetVideoFrame:
	case MessageType::TargetVideoStats:
		if (ack_required) {
			return ValidationError::ReservedHeaderFlag;
		}
		break;
	default:
		break;
	}

	// EventBatch delivery class and Welcome outcome are payload-dependent.
	// TargetVideoFrame retransmission is valid only for an IDR.
	if (retransmission) {
		switch (header.message_type) {
		case MessageType::Discovery:
		case MessageType::Delta:
		case MessageType::Heartbeat:
		case MessageType::Ack:
		case MessageType::Nack:
		case MessageType::TargetVideoStats:
			return ValidationError::ReservedHeaderFlag;
		case MessageType::TargetVideoFrame:
			if (!video_idr) {
				return ValidationError::ReservedHeaderFlag;
			}
			break;
		default:
			break;
		}
	}

	return ValidationError::None;
}

} // namespace

ValidationError validate_datagram_header_version(const TelemetryDatagramHeader& header,
	ProtocolMinorRange accepted_minors) noexcept
{
	if (header.magic != Magic) {
		return ValidationError::BadMagic;
	}
	if (header.version_major != VersionMajor) {
		return ValidationError::UnsupportedMajor;
	}
	if (!is_supported_version_minor(accepted_minors.minimum) ||
		!is_supported_version_minor(accepted_minors.maximum) ||
		accepted_minors.minimum > accepted_minors.maximum ||
		header.version_minor < accepted_minors.minimum || header.version_minor > accepted_minors.maximum) {
		return ValidationError::UnsupportedMinor;
	}
	if (header.header_size != HeaderSizeV1) {
		return ValidationError::BadHeaderSize;
	}
	return ValidationError::None;
}

MessageSizeClass message_size_class(MessageType type) noexcept {
	if (!is_known_message_type(type)) {
		return MessageSizeClass::Invalid;
	}
	return type == MessageType::TargetVideoFrame ? MessageSizeClass::Video : MessageSizeClass::State;
}

bool message_type_allows_fragmentation(MessageType type) noexcept {
	switch (type) {
	case MessageType::Manifest:
	case MessageType::FullSnapshot:
	case MessageType::Delta:
	case MessageType::EventBatch:
	case MessageType::TargetVideoFrame:
		return true;
	default:
		return false;
	}
}

ValidationError expected_fragment_count(MessageType type,
	                                     std::uint32_t message_size,
	                                     std::uint16_t& count) noexcept {
	count = 0;
	const auto size_class = message_size_class(type);
	if (size_class == MessageSizeClass::Invalid) {
		return ValidationError::UnknownMessageType;
	}

	const auto size_limit = max_message_size(type);
	if (message_size > size_limit) {
		return ValidationError::MessageTooLarge;
	}

	const std::uint64_t expected =
	    message_size == 0 ? 1 : (static_cast<std::uint64_t>(message_size) - 1U) / MaxFragmentPayload + 1U;
	if (expected > max_fragment_count(type) || expected > std::numeric_limits<std::uint16_t>::max()) {
		return ValidationError::BadFragmentCount;
	}
	if (expected > 1 && !message_type_allows_fragmentation(type)) {
		return ValidationError::BadFragmentCount;
	}

	count = static_cast<std::uint16_t>(expected);
	return ValidationError::None;
}

ValidationError validate_fragment_layout(const TelemetryDatagramHeader& header) noexcept {
	if (const auto error = validate_type_and_flags(header); error != ValidationError::None) {
		return error;
	}
	if (header.message_id == 0) {
		return ValidationError::OutOfRange;
	}
	if (header.payload_size > MaxFragmentPayload) {
		return ValidationError::BadFragmentSlice;
	}

	std::uint16_t expected_count = 0;
	if (const auto error = expected_fragment_count(header.message_type, header.message_size, expected_count);
	    error != ValidationError::None) {
		return error;
	}
	if (header.fragment_count != expected_count) {
		return ValidationError::BadFragmentCount;
	}
	if (header.fragment_index >= header.fragment_count) {
		return ValidationError::BadFragmentIndex;
	}

	const auto expected_offset = static_cast<std::uint64_t>(header.fragment_index) * MaxFragmentPayload;
	if (expected_offset > std::numeric_limits<std::uint32_t>::max() ||
	    header.fragment_offset != expected_offset) {
		return ValidationError::BadFragmentOffset;
	}

	std::uint32_t expected_payload_size = 0;
	if (header.message_size != 0) {
		if (expected_offset >= header.message_size) {
			return ValidationError::BadFragmentOffset;
		}
		const auto remaining = static_cast<std::uint64_t>(header.message_size) - expected_offset;
		expected_payload_size =
		    static_cast<std::uint32_t>(remaining < MaxFragmentPayload ? remaining : MaxFragmentPayload);
	}
	if (header.payload_size != expected_payload_size) {
		return ValidationError::BadFragmentSlice;
	}
	if (header.message_size == 0 && header.message_crc32 != 0) {
		return ValidationError::BadMessageCrc;
	}

	return ValidationError::None;
}

ValidationError encode_datagram_header(const TelemetryDatagramHeader& header, MutableByteView output) noexcept {
	if (output.data == nullptr || output.size < HeaderSizeV1) {
		return ValidationError::InternalSerializationError;
	}

	PacketWriter writer(MutableByteView{output.data, HeaderSizeV1});
	const bool ok = writer.write_u32(header.magic) && writer.write_u8(header.version_major) &&
	                writer.write_u8(header.version_minor) &&
	                writer.write_u8(static_cast<std::uint8_t>(header.message_type)) && writer.write_u8(header.flags) &&
	                writer.write_u16(header.header_size) && writer.write_u16(header.payload_size) &&
	                writer.write_u64(header.session_id) && writer.write_u32(header.packet_sequence) &&
	                writer.write_u32(header.frame_id) && writer.write_i64(header.mission_time_us) &&
	                writer.write_u64(header.sent_time_us) && writer.write_u32(header.message_id) &&
	                writer.write_u16(header.fragment_index) && writer.write_u16(header.fragment_count) &&
	                writer.write_u32(header.message_size) && writer.write_u32(header.fragment_offset) &&
	                writer.write_u32(header.message_crc32) && writer.write_u32(header.crc32);
	return ok && writer.size() == HeaderSizeV1 ? ValidationError::None
	                                           : ValidationError::InternalSerializationError;
}

ValidationError decode_datagram_header(ByteView input, TelemetryDatagramHeader& header) noexcept {
	if (input.data == nullptr || input.size < HeaderSizeV1) {
		return ValidationError::DatagramTooShort;
	}

	TelemetryDatagramHeader candidate;
	std::uint8_t raw_message_type = 0;
	PacketReader reader(ByteView{input.data, HeaderSizeV1});
	const bool ok = reader.read_u32(candidate.magic) && reader.read_u8(candidate.version_major) &&
	                reader.read_u8(candidate.version_minor) && reader.read_u8(raw_message_type) &&
	                reader.read_u8(candidate.flags) && reader.read_u16(candidate.header_size) &&
	                reader.read_u16(candidate.payload_size) && reader.read_u64(candidate.session_id) &&
	                reader.read_u32(candidate.packet_sequence) && reader.read_u32(candidate.frame_id) &&
	                reader.read_i64(candidate.mission_time_us) && reader.read_u64(candidate.sent_time_us) &&
	                reader.read_u32(candidate.message_id) && reader.read_u16(candidate.fragment_index) &&
	                reader.read_u16(candidate.fragment_count) && reader.read_u32(candidate.message_size) &&
	                reader.read_u32(candidate.fragment_offset) && reader.read_u32(candidate.message_crc32) &&
	                reader.read_u32(candidate.crc32);
	if (!ok || !reader.at_end()) {
		return ValidationError::DatagramTooShort;
	}

	candidate.message_type = static_cast<MessageType>(raw_message_type);
	header = candidate;
	return ValidationError::None;
}

ValidationError calculate_datagram_crc(const TelemetryDatagramHeader& header,
	                                    ByteView payload,
	                                    std::uint32_t& crc) noexcept {
	crc = 0;
	if (payload.size != 0 && payload.data == nullptr) {
		return ValidationError::InternalSerializationError;
	}
	if (header.header_size != HeaderSizeV1) {
		return ValidationError::BadHeaderSize;
	}
	if (payload.size != header.payload_size) {
		return ValidationError::BadDatagramLength;
	}
	if (payload.size > MaxFragmentPayload) {
		return ValidationError::DatagramTooLarge;
	}

	std::array<std::uint8_t, HeaderSizeV1> wire_header{};
	auto crc_header = header;
	crc_header.crc32 = 0;
	if (encode_datagram_header(crc_header, MutableByteView{wire_header.data(), wire_header.size()}) !=
	    ValidationError::None) {
		return ValidationError::InternalSerializationError;
	}

	Crc32IsoHdlc calculator;
	if (!calculator.update(ByteView{wire_header.data(), wire_header.size()}) || !calculator.update(payload)) {
		return ValidationError::InternalSerializationError;
	}
	crc = calculator.value();
	return ValidationError::None;
}

ValidationError encode_datagram(TelemetryDatagramHeader header,
	                             ByteView payload,
	                             MutableByteView output,
	                             std::size_t& written) noexcept
{
	return encode_datagram(header, FrozenV1_0MinorRange, payload, output, written);
}

ValidationError encode_datagram(TelemetryDatagramHeader header,
	ProtocolMinorRange accepted_minors,
	ByteView payload,
	MutableByteView output,
	std::size_t& written) noexcept {
	written = 0;
	if ((payload.size != 0 && payload.data == nullptr) || payload.size > MaxFragmentPayload) {
		return payload.size > MaxFragmentPayload ? ValidationError::DatagramTooLarge
		                                         : ValidationError::InternalSerializationError;
	}
	const auto datagram_size = HeaderSizeV1 + payload.size;
	if (output.data == nullptr || output.size < datagram_size) {
		return ValidationError::InternalSerializationError;
	}

	header.payload_size = static_cast<std::uint16_t>(payload.size);
	header.crc32 = 0;
	if (const auto error = validate_datagram_header_version(header, accepted_minors);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_fragment_layout(header); error != ValidationError::None) {
		return error;
	}
	if (const auto error = calculate_datagram_crc(header, payload, header.crc32); error != ValidationError::None) {
		return error;
	}

	// Copy first so even an overlapping input view is safe when the header is
	// subsequently written over the beginning of output.
	if (payload.size != 0) {
		std::memmove(output.data + HeaderSizeV1, payload.data, payload.size);
	}
	if (const auto error = encode_datagram_header(header, output); error != ValidationError::None) {
		return error;
	}
	written = datagram_size;
	return ValidationError::None;
}

ValidationError decode_and_validate_datagram_envelope(ByteView datagram, DatagramView& decoded) noexcept {
	return decode_and_validate_datagram_envelope(datagram, FrozenV1_0MinorRange, decoded);
}

ValidationError decode_and_validate_datagram_envelope(ByteView datagram,
	ProtocolMinorRange accepted_minors,
	DatagramView& decoded) noexcept {
	decoded = DatagramView{};
	if (datagram.data == nullptr || datagram.size < HeaderSizeV1) {
		return ValidationError::DatagramTooShort;
	}
	if (datagram.size > MaxDatagramSize) {
		return ValidationError::DatagramTooLarge;
	}

	TelemetryDatagramHeader header;
	if (const auto error = decode_datagram_header(datagram, header); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_datagram_header_version(header, accepted_minors);
		error != ValidationError::None) {
		return error;
	}
	// Reserved header bits are a fixed-envelope property and normatively
	// precede all length and CRC work. Type-dependent flag relations remain in
	// validate_fragment_layout(), after session/direction/endpoint validation.
	if ((header.flags & ReservedMessageFlags) != 0) {
		return ValidationError::ReservedHeaderFlag;
	}
	if (header.payload_size > MaxFragmentPayload) {
		return ValidationError::DatagramTooLarge;
	}

	const auto header_bytes = static_cast<std::size_t>(header.header_size);
	const auto payload_bytes = static_cast<std::size_t>(header.payload_size);
	if (payload_bytes > std::numeric_limits<std::size_t>::max() - header_bytes) {
		return ValidationError::BadDatagramLength;
	}
	const auto expected_datagram_size = header_bytes + payload_bytes;
	if (datagram.size != expected_datagram_size) {
		return ValidationError::BadDatagramLength;
	}

	const ByteView payload{datagram.data + HeaderSizeV1, header.payload_size};
	std::uint32_t calculated_crc = 0;
	if (const auto error = calculate_datagram_crc(header, payload, calculated_crc); error != ValidationError::None) {
		return error;
	}
	if (header.crc32 != calculated_crc) {
		return ValidationError::BadDatagramCrc;
	}

	decoded.header = header;
	decoded.payload = payload;
	return ValidationError::None;
}

ValidationError decode_and_validate_datagram(ByteView datagram, DatagramView& decoded) noexcept {
	return decode_and_validate_datagram(datagram, FrozenV1_0MinorRange, decoded);
}

ValidationError decode_and_validate_datagram(ByteView datagram,
	ProtocolMinorRange accepted_minors,
	DatagramView& decoded) noexcept {
	DatagramView envelope;
	if (const auto error = decode_and_validate_datagram_envelope(datagram, accepted_minors, envelope);
	    error != ValidationError::None) {
		decoded = DatagramView{};
		return error;
	}
	if (const auto error = validate_fragment_layout(envelope.header); error != ValidationError::None) {
		decoded = DatagramView{};
		return error;
	}

	decoded = envelope;
	return ValidationError::None;
}

ValidationError validate_message_crc(const TelemetryDatagramHeader& header, ByteView logical_payload) noexcept {
	if (!is_known_message_type(header.message_type)) {
		return ValidationError::UnknownMessageType;
	}
	if (header.message_size > max_message_size(header.message_type)) {
		return ValidationError::MessageTooLarge;
	}
	if (logical_payload.size != 0 && logical_payload.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (logical_payload.size < header.message_size) {
		return ValidationError::TruncatedPayload;
	}
	if (logical_payload.size > header.message_size) {
		return ValidationError::TrailingBytes;
	}

	std::uint32_t calculated_crc = 0;
	if (!crc32_iso_hdlc(logical_payload, calculated_crc)) {
		return ValidationError::InternalSerializationError;
	}
	return calculated_crc == header.message_crc32 ? ValidationError::None : ValidationError::BadMessageCrc;
}

} // namespace telemetry::protocol
