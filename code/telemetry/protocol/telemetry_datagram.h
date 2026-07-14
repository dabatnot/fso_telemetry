#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

// Logical representation only. The wire header is always serialized field by
// field; this type is deliberately not packed or copied as raw storage.
struct TelemetryDatagramHeader {
	std::uint32_t magic = Magic;
	std::uint8_t version_major = VersionMajor;
	std::uint8_t version_minor = VersionMinor;
	MessageType message_type = MessageType::Invalid;
	std::uint8_t flags = MessageFlagNone;
	std::uint16_t header_size = static_cast<std::uint16_t>(HeaderSizeV1);
	std::uint16_t payload_size = 0;
	std::uint64_t session_id = 0;
	std::uint32_t packet_sequence = 0;
	std::uint32_t frame_id = 0;
	std::int64_t mission_time_us = 0;
	std::uint64_t sent_time_us = 0;
	std::uint32_t message_id = 0;
	std::uint16_t fragment_index = 0;
	std::uint16_t fragment_count = 1;
	std::uint32_t message_size = 0;
	std::uint32_t fragment_offset = 0;
	std::uint32_t message_crc32 = 0;
	std::uint32_t crc32 = 0;
};

struct DatagramView {
	TelemetryDatagramHeader header;
	ByteView payload;
};

enum class MessageSizeClass : std::uint8_t {
	Invalid = 0,
	State = 1,
	Video = 2,
};

MessageSizeClass message_size_class(MessageType type) noexcept;
bool message_type_allows_fragmentation(MessageType type) noexcept;

ValidationError expected_fragment_count(MessageType type,
	                                     std::uint32_t message_size,
	                                     std::uint16_t& count) noexcept;
ValidationError validate_fragment_layout(const TelemetryDatagramHeader& header) noexcept;

// These two routines only transform the fixed 68-byte logical header. Full
// wire validation, including CRC and the fragment slice, is performed by
// decode_and_validate_datagram().
ValidationError encode_datagram_header(const TelemetryDatagramHeader& header, MutableByteView output) noexcept;
ValidationError decode_datagram_header(ByteView input, TelemetryDatagramHeader& header) noexcept;

ValidationError calculate_datagram_crc(const TelemetryDatagramHeader& header,
	                                    ByteView payload,
	                                    std::uint32_t& crc) noexcept;

// Serializes one datagram without allocation. payload_size and crc32 are
// canonicalized from payload; all other header fields must already describe a
// valid canonical fragment.
ValidationError encode_datagram(TelemetryDatagramHeader header,
	                             ByteView payload,
	                             MutableByteView output,
	                             std::size_t& written) noexcept;

// Validates only the fixed transport envelope through datagram CRC and returns
// a bounded view even when the message type or fragment layout is not valid.
// This is the hand-off point for session, endpoint and direction checks, which
// normatively precede class/layout validation. No bytes are retained and no
// allocation is performed.
ValidationError decode_and_validate_datagram_envelope(ByteView datagram, DatagramView& decoded) noexcept;

// Convenience validator for callers which do not need to interpose contextual
// session checks. It validates the envelope and the static fragment layout.
ValidationError decode_and_validate_datagram(ByteView datagram, DatagramView& decoded) noexcept;

// Intended for a complete logical payload after reassembly (or an
// unfragmented payload). It validates length before computing message_crc32.
ValidationError validate_message_crc(const TelemetryDatagramHeader& header, ByteView logical_payload) noexcept;

} // namespace telemetry::protocol
