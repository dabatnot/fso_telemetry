#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes) {
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

template <typename Container>
MutableByteView mutable_byte_view(Container& bytes) {
	return MutableByteView{static_cast<std::uint8_t*>(static_cast<void*>(bytes.data())), bytes.size()};
}

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
	ASSERT_LE(offset + 2U, bytes.size());
	bytes[offset] = static_cast<std::uint8_t>(value);
	bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
	ASSERT_LE(offset + 4U, bytes.size());
	for (std::size_t index = 0; index < 4U; ++index) {
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
	}
}

void reseal_datagram(std::vector<std::uint8_t>& bytes) {
	ASSERT_GE(bytes.size(), HeaderSizeV1);
	put_u32(bytes, 64U, 0U);
	put_u32(bytes, 64U, crc32_iso_hdlc(byte_view(bytes)));
}

TelemetryDatagramHeader base_header(MessageType type, ByteView payload, std::uint32_t message_id = 0xa1b2c3d4U) {
	TelemetryDatagramHeader header;
	header.message_type = type;
	header.session_id = 0x0102030405060708ULL;
	header.packet_sequence = 0x11223344U;
	header.sent_time_us = 0x1112131415161718ULL;
	header.message_id = message_id;
	header.message_size = static_cast<std::uint32_t>(payload.size);
	header.message_crc32 = crc32_iso_hdlc(payload);
	return header;
}

std::vector<std::uint8_t> encode(TelemetryDatagramHeader header, ByteView payload) {
	std::vector<std::uint8_t> bytes(HeaderSizeV1 + payload.size, 0xa5U);
	std::size_t written = std::numeric_limits<std::size_t>::max();
	EXPECT_EQ(ValidationError::None, encode_datagram(header, payload, mutable_byte_view(bytes), written));
	EXPECT_EQ(bytes.size(), written);
	return bytes;
}

TEST(TelemetryProtocolDatagram, GoldenEmptyHeaderFreezesAllSixtyEightWireBytesAndOffsets) {
	const auto header = base_header(MessageType::Heartbeat, ByteView{});
	const auto encoded = encode(header, ByteView{});

	// This vector was calculated independently from the field table in section
	// 4.2. The final four bytes are CRC-32/ISO-HDLC over the header with those
	// bytes zeroed. Keeping the whole vector literal freezes every field offset.
	const std::array<std::uint8_t, HeaderSizeV1> golden{
		0x46, 0x53, 0x54, 0x4c, 0x01, 0x00, 0x09, 0x00, 0x44, 0x00, 0x00, 0x00,
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x44, 0x33, 0x22, 0x11,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0xd4, 0xc3, 0xb2, 0xa1,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x76, 0xd6, 0x18, 0x52,
	};

	ASSERT_EQ(golden.size(), encoded.size());
	for (std::size_t offset = 0; offset < golden.size(); ++offset) {
		EXPECT_EQ(golden[offset], encoded[offset]) << "wire offset " << offset;
	}

	DatagramView decoded;
	ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(golden), decoded));
	EXPECT_EQ(Magic, decoded.header.magic);
	EXPECT_EQ(VersionMajor, decoded.header.version_major);
	EXPECT_EQ(VersionMinor, decoded.header.version_minor);
	EXPECT_EQ(MessageType::Heartbeat, decoded.header.message_type);
	EXPECT_EQ(HeaderSizeV1, decoded.header.header_size);
	EXPECT_EQ(0U, decoded.header.payload_size);
	EXPECT_EQ(0x0102030405060708ULL, decoded.header.session_id);
	EXPECT_EQ(0x11223344U, decoded.header.packet_sequence);
	EXPECT_EQ(0U, decoded.header.frame_id);
	EXPECT_EQ(0, decoded.header.mission_time_us);
	EXPECT_EQ(0x1112131415161718ULL, decoded.header.sent_time_us);
	EXPECT_EQ(0xa1b2c3d4U, decoded.header.message_id);
	EXPECT_EQ(0U, decoded.header.fragment_index);
	EXPECT_EQ(1U, decoded.header.fragment_count);
	EXPECT_EQ(0U, decoded.header.message_size);
	EXPECT_EQ(0U, decoded.header.fragment_offset);
	EXPECT_EQ(0U, decoded.header.message_crc32);
	EXPECT_EQ(0x5218d676U, decoded.header.crc32);
	EXPECT_TRUE(decoded.payload.empty());
}

TEST(TelemetryProtocolDatagram, DatagramCrcAlwaysTreatsTheWireCrcFieldAsZero) {
	auto header = base_header(MessageType::Heartbeat, ByteView{});
	header.crc32 = 0xffffffffU;
	std::uint32_t calculated = 0;
	ASSERT_EQ(ValidationError::None, calculate_datagram_crc(header, ByteView{}, calculated));
	EXPECT_EQ(0x5218d676U, calculated);

	header.crc32 = calculated;
	std::array<std::uint8_t, HeaderSizeV1> raw{};
	ASSERT_EQ(ValidationError::None, encode_datagram_header(header, mutable_byte_view(raw)));
	std::fill(raw.begin() + 64, raw.end(), std::uint8_t{0});
	EXPECT_EQ(calculated, crc32_iso_hdlc(byte_view(raw)));
}

TEST(TelemetryProtocolDatagram, EncodeHonorsExactCapacityAndRejectsShortNullOrOversizedViews) {
	const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
	const auto header = base_header(MessageType::Heartbeat, byte_view(payload));
	std::array<std::uint8_t, HeaderSizeV1 + payload.size()> exact{};
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::None, encode_datagram(header, byte_view(payload), mutable_byte_view(exact), written));
	EXPECT_EQ(exact.size(), written);

	std::array<std::uint8_t, HeaderSizeV1 + payload.size() - 1U> short_buffer{};
	short_buffer.fill(0xa5U);
	const auto canary = short_buffer;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
	          encode_datagram(header, byte_view(payload), mutable_byte_view(short_buffer), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, short_buffer);

	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
	          encode_datagram(header, byte_view(payload), MutableByteView{nullptr, exact.size()}, written));
	EXPECT_EQ(0U, written);
	const std::uint8_t byte = 0U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
	          encode_datagram(header, ByteView{nullptr, 1U}, mutable_byte_view(exact), written));
	EXPECT_EQ(ValidationError::DatagramTooLarge,
	          encode_datagram(header,
	                          ByteView{&byte, MaxFragmentPayload + 1U},
	                          mutable_byte_view(exact),
	                          written));

	std::array<std::uint8_t, HeaderSizeV1 - 1U> short_header{};
	EXPECT_EQ(ValidationError::InternalSerializationError,
	          encode_datagram_header(header, mutable_byte_view(short_header)));
}

TEST(TelemetryProtocolDatagram, KeyframeAndVideoIdrFlagsAreTypeSpecificAndMutuallyExclusive) {
	TelemetryDatagramHeader header;
	header.message_id = 1U;

	header.message_type = MessageType::FullSnapshot;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_fragment_layout(header));
	header.flags = static_cast<std::uint8_t>(MessageFlagKeyframe | MessageFlagAckRequired);
	EXPECT_EQ(ValidationError::None, validate_fragment_layout(header));
	header.flags = static_cast<std::uint8_t>(MessageFlagKeyframe | MessageFlagVideoIdr | MessageFlagAckRequired);
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_fragment_layout(header));

	header.message_type = MessageType::TargetVideoFrame;
	header.flags = MessageFlagVideoIdr;
	EXPECT_EQ(ValidationError::None, validate_fragment_layout(header));
	header.flags = MessageFlagKeyframe;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_fragment_layout(header));

	header.message_type = MessageType::Delta;
	header.flags = MessageFlagVideoIdr;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_fragment_layout(header));
	header.flags = MessageFlagKeyframe;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_fragment_layout(header));
	header.flags = MessageFlagRetransmission;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_fragment_layout(header));

	header.message_type = MessageType::Manifest;
	header.flags = MessageFlagNone;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_fragment_layout(header));
	header.flags = MessageFlagAckRequired;
	EXPECT_EQ(ValidationError::None, validate_fragment_layout(header));
	header.flags = static_cast<std::uint8_t>(MessageFlagAckRequired | MessageFlagRetransmission);
	EXPECT_EQ(ValidationError::None, validate_fragment_layout(header));

	header.message_type = MessageType::TargetVideoFrame;
	header.flags = MessageFlagRetransmission;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_fragment_layout(header));
	header.flags = static_cast<std::uint8_t>(MessageFlagVideoIdr | MessageFlagRetransmission);
	EXPECT_EQ(ValidationError::None, validate_fragment_layout(header));

	std::uint16_t count = 0U;
	EXPECT_EQ(ValidationError::BadFragmentCount,
	          expected_fragment_count(MessageType::Heartbeat,
	                                  static_cast<std::uint32_t>(MaxFragmentPayload + 1U),
	                                  count));
}

TEST(TelemetryProtocolDatagram, EmptyAndUnfragmentedMessagesRoundTripWithoutPadding) {
	const std::array<std::uint8_t, 4> payload{0xde, 0xad, 0xbe, 0xef};
	const std::array<ByteView, 2> messages{ByteView{}, byte_view(payload)};

	for (const auto message : messages) {
		SCOPED_TRACE(testing::Message() << "payload size " << message.size);
		const auto header = base_header(MessageType::Heartbeat, message);
		const auto encoded = encode(header, message);
		DatagramView decoded;
		ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(encoded), decoded));
		EXPECT_EQ(HeaderSizeV1 + message.size, encoded.size());
		EXPECT_EQ(message.size, decoded.payload.size);
		EXPECT_EQ(ValidationError::None, validate_message_crc(decoded.header, decoded.payload));
		for (std::size_t index = 0; index < message.size; ++index) {
			EXPECT_EQ(message.data[index], decoded.payload.data[index]);
		}
	}
}

TEST(TelemetryProtocolDatagram, RejectsSizeMagicVersionHeaderFlagsTypeAndMessageIdBeforePublishing) {
	const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
	const auto valid = encode(base_header(MessageType::Heartbeat, byte_view(payload)), byte_view(payload));

	struct Mutation {
		const char* name;
		std::size_t offset;
		std::uint8_t value;
		ValidationError expected;
	};
	const std::array<Mutation, 7> mutations{{
		{"magic", 0U, 0x00U, ValidationError::BadMagic},
		{"major", 4U, 0x02U, ValidationError::UnsupportedMajor},
		{"minor", 5U, 0x01U, ValidationError::UnsupportedMinor},
		{"header size", 8U, 0x43U, ValidationError::BadHeaderSize},
		{"reserved flags", 7U, 0x80U, ValidationError::ReservedHeaderFlag},
		{"unknown type", 6U, 0xffU, ValidationError::UnknownMessageType},
		{"zero message id", 44U, 0x00U, ValidationError::OutOfRange},
	}};

	for (const auto& mutation : mutations) {
		SCOPED_TRACE(mutation.name);
		auto bytes = valid;
		bytes[mutation.offset] = mutation.value;
		if (mutation.offset == 44U) {
			put_u32(bytes, 44U, 0U);
		}
		reseal_datagram(bytes);
		DatagramView sentinel;
		sentinel.header.message_id = 0xfeedbeefU;
		const std::uint8_t sentinel_byte = 0x5aU;
		sentinel.payload = ByteView{&sentinel_byte, 1U};
		EXPECT_EQ(mutation.expected, decode_and_validate_datagram(byte_view(bytes), sentinel));
		EXPECT_EQ(0U, sentinel.header.message_id);
		EXPECT_EQ(nullptr, sentinel.payload.data);
		EXPECT_EQ(0U, sentinel.payload.size);
	}

	DatagramView decoded;
	EXPECT_EQ(ValidationError::DatagramTooShort,
	          decode_and_validate_datagram(ByteView{valid.data(), HeaderSizeV1 - 1U}, decoded));
	std::vector<std::uint8_t> too_large(MaxDatagramSize + 1U, 0U);
	EXPECT_EQ(ValidationError::DatagramTooLarge, decode_and_validate_datagram(byte_view(too_large), decoded));

	auto trailing = valid;
	trailing.push_back(0U);
	EXPECT_EQ(ValidationError::BadDatagramLength, decode_and_validate_datagram(byte_view(trailing), decoded));
	auto truncated = valid;
	truncated.pop_back();
	EXPECT_EQ(ValidationError::BadDatagramLength, decode_and_validate_datagram(byte_view(truncated), decoded));

	// Fixed-header validation order is normative: a reserved bit and an
	// oversized declared payload win before length mismatch or CRC work.
	auto reserved_and_corrupt = valid;
	reserved_and_corrupt[7U] = 0x80U;
	reserved_and_corrupt.back() ^= 0x80U;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag,
		decode_and_validate_datagram(byte_view(reserved_and_corrupt), decoded));
	auto oversized_declared_payload = valid;
	put_u16(oversized_declared_payload, 10U, static_cast<std::uint16_t>(MaxFragmentPayload + 1U));
	EXPECT_EQ(ValidationError::DatagramTooLarge,
		decode_and_validate_datagram(byte_view(oversized_declared_payload), decoded));
}

TEST(TelemetryProtocolDatagram, DetectsDatagramCrcBeforeFragmentLayoutValidation) {
	const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
	auto corrupted = encode(base_header(MessageType::Heartbeat, byte_view(payload)), byte_view(payload));
	corrupted.back() ^= 0x80U;
	// A bad fragment offset is also present, but CRC validation has precedence.
	put_u32(corrupted, 56U, 1U);
	DatagramView decoded;
	EXPECT_EQ(ValidationError::BadDatagramCrc, decode_and_validate_datagram(byte_view(corrupted), decoded));
}

TEST(TelemetryProtocolDatagram, EnvelopeValidationHandsUnknownTypeToTheContextPipelineAfterCrc) {
	const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
	auto unknown = encode(base_header(MessageType::Heartbeat, byte_view(payload)), byte_view(payload));
	unknown[6] = 0xffU;
	reseal_datagram(unknown);

	DatagramView envelope;
	ASSERT_EQ(ValidationError::None,
	          decode_and_validate_datagram_envelope(byte_view(unknown), envelope));
	EXPECT_EQ(static_cast<MessageType>(0xffU), envelope.header.message_type);
	EXPECT_EQ(payload.size(), envelope.payload.size);

	DatagramView fully_validated;
	fully_validated.header.message_id = 0xfeedbeefU;
	EXPECT_EQ(ValidationError::UnknownMessageType,
	          decode_and_validate_datagram(byte_view(unknown), fully_validated));
	EXPECT_EQ(0U, fully_validated.header.message_id);
	EXPECT_EQ(nullptr, fully_validated.payload.data);
}

TEST(TelemetryProtocolDatagram, ExpectedFragmentCountsAreCanonicalAtEveryBoundary) {
	struct Case {
		MessageType type;
		std::uint32_t size;
		std::uint16_t count;
	};
	const std::array<Case, 9> cases{{
		{MessageType::Delta, 0U, 1U},
		{MessageType::Delta, 1U, 1U},
		{MessageType::Delta, 1132U, 1U},
		{MessageType::Delta, 1133U, 2U},
		{MessageType::Delta, 2264U, 2U},
		{MessageType::Delta, 2265U, 3U},
		{MessageType::Delta, static_cast<std::uint32_t>(MaxStateMessageSize), 927U},
		{MessageType::TargetVideoFrame, static_cast<std::uint32_t>(MaxVideoMessageSize), 1853U},
		{MessageType::Heartbeat, 1132U, 1U},
	}};

	for (const auto& value : cases) {
		SCOPED_TRACE(testing::Message() << "type " << static_cast<unsigned>(value.type) << ", size " << value.size);
		std::uint16_t count = 0;
		EXPECT_EQ(ValidationError::None, expected_fragment_count(value.type, value.size, count));
		EXPECT_EQ(value.count, count);
	}

	std::uint16_t unchanged = 0xbeefU;
	EXPECT_EQ(ValidationError::MessageTooLarge,
	          expected_fragment_count(MessageType::Delta,
	                                  static_cast<std::uint32_t>(MaxStateMessageSize + 1U),
	                                  unchanged));
	EXPECT_EQ(0U, unchanged);
	EXPECT_EQ(ValidationError::MessageTooLarge,
	          expected_fragment_count(MessageType::TargetVideoFrame,
	                                  static_cast<std::uint32_t>(MaxVideoMessageSize + 1U),
	                                  unchanged));
	EXPECT_EQ(ValidationError::MessageTooLarge,
	          expected_fragment_count(MessageType::Delta, std::numeric_limits<std::uint32_t>::max(), unchanged));
	EXPECT_EQ(ValidationError::UnknownMessageType,
	          expected_fragment_count(static_cast<MessageType>(0xffU), 1U, unchanged));
}

TEST(TelemetryProtocolDatagram, FragmentLayoutRejectsCountIndexOffsetSliceFlagAndOverflowErrors) {
	const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
	auto valid = base_header(MessageType::Delta, byte_view(payload));
	valid.payload_size = static_cast<std::uint16_t>(payload.size());
	ASSERT_EQ(ValidationError::None, validate_fragment_layout(valid));

	auto invalid = valid;
	invalid.fragment_count = 0U;
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_fragment_layout(invalid));
	invalid = valid;
	invalid.fragment_index = 1U;
	EXPECT_EQ(ValidationError::BadFragmentIndex, validate_fragment_layout(invalid));
	invalid = valid;
	invalid.fragment_offset = 1U;
	EXPECT_EQ(ValidationError::BadFragmentOffset, validate_fragment_layout(invalid));
	invalid = valid;
	invalid.message_size = 5U;
	EXPECT_EQ(ValidationError::BadFragmentSlice, validate_fragment_layout(invalid));
	invalid = valid;
	invalid.flags = MessageFlagFragmented;
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_fragment_layout(invalid));

	invalid = valid;
	invalid.message_size = 1133U;
	invalid.payload_size = static_cast<std::uint16_t>(MaxFragmentPayload);
	invalid.fragment_count = 2U;
	invalid.flags = MessageFlagNone;
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_fragment_layout(invalid));
	invalid.flags = MessageFlagFragmented;
	EXPECT_EQ(ValidationError::None, validate_fragment_layout(invalid));

	invalid.message_type = MessageType::Heartbeat;
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_fragment_layout(invalid));
	invalid = valid;
	invalid.message_size = std::numeric_limits<std::uint32_t>::max();
	EXPECT_EQ(ValidationError::MessageTooLarge, validate_fragment_layout(invalid));
	invalid = valid;
	invalid.message_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_fragment_layout(invalid));
}

TEST(TelemetryProtocolDatagram, MultiFragmentWireSlicesAreExactIncludingAOneByteFinalSlice) {
	std::vector<std::uint8_t> first(MaxFragmentPayload);
	for (std::size_t index = 0; index < first.size(); ++index) {
		first[index] = static_cast<std::uint8_t>(index);
	}
	const std::array<std::uint8_t, 1> last{0x5aU};

	TelemetryDatagramHeader common = base_header(MessageType::Delta, ByteView{});
	common.flags = MessageFlagFragmented;
	common.message_size = static_cast<std::uint32_t>(MaxFragmentPayload + last.size());
	Crc32IsoHdlc message_crc;
	ASSERT_TRUE(message_crc.update(byte_view(first)));
	ASSERT_TRUE(message_crc.update(byte_view(last)));
	common.message_crc32 = message_crc.value();
	common.fragment_count = 2U;

	auto first_header = common;
	first_header.fragment_index = 0U;
	first_header.fragment_offset = 0U;
	const auto first_wire = encode(first_header, byte_view(first));
	auto last_header = common;
	last_header.packet_sequence++;
	last_header.fragment_index = 1U;
	last_header.fragment_offset = static_cast<std::uint32_t>(MaxFragmentPayload);
	const auto last_wire = encode(last_header, byte_view(last));

	DatagramView decoded_first;
	DatagramView decoded_last;
	ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(first_wire), decoded_first));
	ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(last_wire), decoded_last));
	EXPECT_EQ(MaxDatagramSize, first_wire.size());
	EXPECT_EQ(HeaderSizeV1 + 1U, last_wire.size());
	EXPECT_EQ(MaxFragmentPayload, decoded_first.payload.size);
	EXPECT_EQ(1U, decoded_last.payload.size);
	EXPECT_EQ(0U, decoded_first.header.fragment_offset);
	EXPECT_EQ(MaxFragmentPayload, decoded_last.header.fragment_offset);
}

TEST(TelemetryProtocolDatagram, MessageCrcRequiresExactLogicalLengthAndPublishesNoFalseSuccess) {
	const std::array<std::uint8_t, 9> payload{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	auto header = base_header(MessageType::Delta, byte_view(payload));
	header.message_crc32 = 0xcbf43926U;
	EXPECT_EQ(ValidationError::None, validate_message_crc(header, byte_view(payload)));

	auto corrupted = payload;
	corrupted[4] ^= 0x01U;
	EXPECT_EQ(ValidationError::BadMessageCrc, validate_message_crc(header, byte_view(corrupted)));
	EXPECT_EQ(ValidationError::TruncatedPayload,
	          validate_message_crc(header, ByteView{payload.data(), payload.size() - 1U}));

	header.message_size = 0U;
	header.message_crc32 = 0U;
	EXPECT_EQ(ValidationError::None, validate_message_crc(header, ByteView{}));
}

TEST(TelemetryProtocolDatagram, MinorOneRequiresExplicitVersionRangeAndBadMinorIsRejected) {
	auto header = base_header(MessageType::Heartbeat, ByteView{});
	header.version_minor = VersionMinorV1_1;
	std::vector<std::uint8_t> wire(HeaderSizeV1);
	std::size_t written = 0U;
	ASSERT_EQ(ValidationError::None,
		encode_datagram(header, Phase1ProducerMinorRange, ByteView{}, mutable_byte_view(wire), written));
	DatagramView decoded;
	EXPECT_EQ(ValidationError::UnsupportedMinor, decode_and_validate_datagram(byte_view(wire), decoded));
	EXPECT_EQ(ValidationError::None,
		decode_and_validate_datagram(byte_view(wire), Phase1ProducerMinorRange, decoded));
	wire[5] = 2U; reseal_datagram(wire);
	EXPECT_EQ(ValidationError::UnsupportedMinor,
		decode_and_validate_datagram(byte_view(wire), {VersionMinorV1_0, VersionMinorV1_1}, decoded));
}

} // namespace
