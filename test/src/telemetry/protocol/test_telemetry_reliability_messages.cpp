#include "telemetry/protocol/telemetry_reliability_messages.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

template <typename Container>
MutableByteView mutable_byte_view(Container& bytes)
{
	return MutableByteView{static_cast<std::uint8_t*>(static_cast<void*>(bytes.data())), bytes.size()};
}

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
	ASSERT_LE(offset + 2U, bytes.size());
	bytes[offset] = static_cast<std::uint8_t>(value);
	bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

AckPayload valid_ack()
{
	AckPayload payload;
	payload.target_message_id = 0x11223344U;
	payload.target_message_type = MessageType::FullSnapshot;
	payload.ack_flags = KnownAckFlags;
	payload.target_fragment_count = 7U;
	payload.target_message_crc32 = 0xa1b2c3d4U;
	return payload;
}

NackPayload valid_nack(ByteView bitmap = ByteView{})
{
	NackPayload payload;
	payload.target_message_id = 0x11223344U;
	payload.target_message_type = MessageType::TargetVideoFrame;
	payload.reason = NackReason::MissingFragments;
	payload.target_fragment_count = 9U;
	payload.target_message_crc32 = 0xa1b2c3d4U;
	payload.needed_before_producer_time_us = 0x0102030405060708ULL;
	payload.missing_bitmap = bitmap;
	return payload;
}

ResyncRequestPayload valid_resync()
{
	ResyncRequestPayload payload;
	payload.request_id = 0x11223344U;
	payload.reason = ResyncReason::SessionStale;
	payload.request_flags = KnownResyncRequestFlags;
	payload.last_applied_snapshot_id = 0x55667788U;
	payload.last_applied_delta_sequence = 0x99aabbccU;
	payload.client_send_time_us = 0x0102030405060708ULL;
	return payload;
}

SessionEndPayload valid_session_end()
{
	SessionEndPayload payload;
	payload.reason = SessionEndReason::Restart;
	payload.end_flags = SessionEndFlagReconnectAllowed;
	payload.last_snapshot_id = 0x11223344U;
	payload.producer_sample_time_us = 0x0102030405060708ULL;
	return payload;
}

TEST(TelemetryProtocolReliabilityMessages, AckGoldenBytesRoundTripAndExactLength)
{
	const auto original = valid_ack();
	std::array<std::uint8_t, AckPayloadSize> encoded{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_ack_payload(original, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);
	const std::array<std::uint8_t, AckPayloadSize> golden{{
		0x44U,
		0x33U,
		0x22U,
		0x11U,
		0x06U,
		0x03U,
		0x07U,
		0x00U,
		0xd4U,
		0xc3U,
		0xb2U,
		0xa1U,
	}};
	EXPECT_EQ(golden, encoded);

	AckPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_ack_payload(byte_view(golden), decoded));
	EXPECT_EQ(original.target_message_id, decoded.target_message_id);
	EXPECT_EQ(original.target_message_type, decoded.target_message_type);
	EXPECT_EQ(original.ack_flags, decoded.ack_flags);
	EXPECT_EQ(original.target_fragment_count, decoded.target_fragment_count);
	EXPECT_EQ(original.target_message_crc32, decoded.target_message_crc32);

	for (std::size_t size = 0; size < golden.size(); ++size) {
		decoded = original;
		EXPECT_EQ(ValidationError::TruncatedPayload, decode_ack_payload(ByteView{golden.data(), size}, decoded));
		EXPECT_EQ(0U, decoded.target_message_id);
		EXPECT_EQ(MessageType::Invalid, decoded.target_message_type);
	}
	auto trailing = std::vector<std::uint8_t>(golden.begin(), golden.end());
	trailing.push_back(0U);
	decoded = original;
	EXPECT_EQ(ValidationError::TrailingBytes, decode_ack_payload(byte_view(trailing), decoded));
	EXPECT_EQ(0U, decoded.target_message_id);
	EXPECT_EQ(ValidationError::TruncatedPayload, decode_ack_payload(ByteView{nullptr, AckPayloadSize}, decoded));
}

TEST(TelemetryProtocolReliabilityMessages, AckRejectsInvalidIdentityCountsAndFlags)
{
	auto payload = valid_ack();
	payload.target_message_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_ack_payload(payload));
	payload = valid_ack();
	payload.target_message_type = MessageType::Invalid;
	EXPECT_EQ(ValidationError::UnknownMessageType, validate_ack_payload(payload));
	payload.target_message_type = static_cast<MessageType>(FirstReservedMessageType);
	EXPECT_EQ(ValidationError::UnknownMessageType, validate_ack_payload(payload));

	payload = valid_ack();
	payload.target_fragment_count = 0U;
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_ack_payload(payload));
	payload.target_fragment_count = static_cast<std::uint16_t>(MaxStateFragments + 1U);
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_ack_payload(payload));
	payload.target_message_type = MessageType::TargetVideoFrame;
	payload.target_fragment_count = static_cast<std::uint16_t>(MaxVideoFragments);
	EXPECT_EQ(ValidationError::None, validate_ack_payload(payload));
	payload.target_fragment_count = static_cast<std::uint16_t>(MaxVideoFragments + 1U);
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_ack_payload(payload));

	payload = valid_ack();
	payload.ack_flags = static_cast<std::uint8_t>(AckFlag::Validated);
	EXPECT_EQ(ValidationError::None, validate_ack_payload(payload));
	payload.ack_flags = KnownAckFlags;
	EXPECT_EQ(ValidationError::None, validate_ack_payload(payload));
	payload.ack_flags = static_cast<std::uint8_t>(AckFlag::Applied);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_ack_payload(payload));
	payload.ack_flags = 0U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_ack_payload(payload));
	payload.ack_flags = 0x04U;
	EXPECT_EQ(ValidationError::ReservedFlag, validate_ack_payload(payload));
}

TEST(TelemetryProtocolReliabilityMessages, AckEncodingIsAtomicAndDecodeRejectsWireFlagsAndTypes)
{
	std::array<std::uint8_t, AckPayloadSize - 1U> short_output{};
	short_output.fill(0xa5U);
	const auto short_canary = short_output;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_ack_payload(valid_ack(), mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(short_canary, short_output);

	std::array<std::uint8_t, AckPayloadSize> output{};
	output.fill(0xa5U);
	const auto canary = output;
	auto invalid = valid_ack();
	invalid.target_message_id = 0U;
	written = 99U;
	EXPECT_EQ(ValidationError::OutOfRange, encode_ack_payload(invalid, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, output);
	EXPECT_EQ(ValidationError::InternalSerializationError, encode_ack_payload(valid_ack(), MutableByteView{}, written));
	EXPECT_EQ(0U, written);

	std::vector<std::uint8_t> bytes(AckPayloadSize, 0U);
	ASSERT_EQ(ValidationError::None, encode_ack_payload(valid_ack(), mutable_byte_view(bytes), written));
	AckPayload decoded;
	bytes[4U] = FirstReservedMessageType;
	EXPECT_EQ(ValidationError::UnknownMessageType, decode_ack_payload(byte_view(bytes), decoded));
	bytes[4U] = static_cast<std::uint8_t>(MessageType::FullSnapshot);
	bytes[5U] = 0x04U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_ack_payload(byte_view(bytes), decoded));
	bytes[5U] = static_cast<std::uint8_t>(AckFlag::Applied);
	EXPECT_EQ(ValidationError::InvalidStateTransition, decode_ack_payload(byte_view(bytes), decoded));
}

TEST(TelemetryProtocolReliabilityMessages, NackGoldenBytesRoundTripAndAliasedEncoding)
{
	std::array<std::uint8_t, 2> bitmap{{0x81U, 0x01U}};
	const auto original = valid_nack(byte_view(bitmap));
	std::array<std::uint8_t, NackPayloadPrefixSize + bitmap.size()> encoded{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_nack_payload(original, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);
	const std::array<std::uint8_t, NackPayloadPrefixSize + bitmap.size()> golden{{
		0x44U,
		0x33U,
		0x22U,
		0x11U,
		0x10U,
		0x01U,
		0x09U,
		0x00U,
		0xd4U,
		0xc3U,
		0xb2U,
		0xa1U,
		0x08U,
		0x07U,
		0x06U,
		0x05U,
		0x04U,
		0x03U,
		0x02U,
		0x01U,
		0x02U,
		0x00U,
		0x00U,
		0x00U,
		0x81U,
		0x01U,
	}};
	EXPECT_EQ(golden, encoded);

	NackPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_nack_payload(byte_view(golden), decoded));
	EXPECT_EQ(original.target_message_id, decoded.target_message_id);
	EXPECT_EQ(original.target_message_type, decoded.target_message_type);
	EXPECT_EQ(original.reason, decoded.reason);
	EXPECT_EQ(original.target_fragment_count, decoded.target_fragment_count);
	EXPECT_EQ(original.target_message_crc32, decoded.target_message_crc32);
	EXPECT_EQ(original.needed_before_producer_time_us, decoded.needed_before_producer_time_us);
	ASSERT_EQ(bitmap.size(), decoded.missing_bitmap.size);
	EXPECT_EQ(bitmap[0], decoded.missing_bitmap.data[0]);
	EXPECT_EQ(bitmap[1], decoded.missing_bitmap.data[1]);

	// The bitmap source may overlap the output prefix. The encoder must first
	// preserve/move the suffix and only then publish the fixed prefix.
	std::array<std::uint8_t, NackPayloadPrefixSize + bitmap.size()> aliased{};
	aliased[0U] = bitmap[0U];
	aliased[1U] = bitmap[1U];
	auto alias_payload = valid_nack(ByteView{aliased.data(), bitmap.size()});
	written = 99U;
	ASSERT_EQ(ValidationError::None, encode_nack_payload(alias_payload, mutable_byte_view(aliased), written));
	EXPECT_EQ(golden, aliased);
}

TEST(TelemetryProtocolReliabilityMessages, NackRejectsAllShortTrailingReservedAndOversizedBitmapEncodings)
{
	const std::array<std::uint8_t, 2> bitmap{{0x81U, 0x01U}};
	std::vector<std::uint8_t> golden(NackPayloadPrefixSize + bitmap.size(), 0U);
	std::size_t written = 0U;
	ASSERT_EQ(ValidationError::None,
		encode_nack_payload(valid_nack(byte_view(bitmap)), mutable_byte_view(golden), written));

	NackPayload decoded;
	for (std::size_t size = 0; size < golden.size(); ++size) {
		decoded = valid_nack(byte_view(bitmap));
		EXPECT_EQ(ValidationError::TruncatedPayload, decode_nack_payload(ByteView{golden.data(), size}, decoded));
		EXPECT_EQ(0U, decoded.target_message_id);
		EXPECT_TRUE(decoded.missing_bitmap.empty());
	}
	auto trailing = golden;
	trailing.push_back(0U);
	decoded = valid_nack(byte_view(bitmap));
	EXPECT_EQ(ValidationError::TrailingBytes, decode_nack_payload(byte_view(trailing), decoded));
	EXPECT_EQ(0U, decoded.target_message_id);
	EXPECT_EQ(ValidationError::TruncatedPayload,
		decode_nack_payload(ByteView{nullptr, NackPayloadPrefixSize}, decoded));

	auto reserved = golden;
	reserved[22U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_nack_payload(byte_view(reserved), decoded));
	reserved = golden;
	reserved[23U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_nack_payload(byte_view(reserved), decoded));

	auto oversized = std::vector<std::uint8_t>(NackPayloadPrefixSize, 0U);
	put_u16(oversized, 20U, static_cast<std::uint16_t>(MaxNackBitmapBytes + 1U));
	EXPECT_EQ(ValidationError::OutOfRange, decode_nack_payload(byte_view(oversized), decoded));
}

TEST(TelemetryProtocolReliabilityMessages, MissingBitmapBoundariesTailBitsAndMaxAreExact)
{
	struct BitmapCase {
		std::uint16_t fragment_count;
		std::size_t bytes;
		std::uint8_t valid_tail;
		std::uint8_t first_reserved_tail_bit;
	};
	const std::array<BitmapCase, 5> cases{{
		{1U, 1U, 0x01U, 0x02U},
		{7U, 1U, 0x7fU, 0x80U},
		{8U, 1U, 0xffU, 0x00U},
		{9U, 2U, 0x01U, 0x02U},
		{2048U, MaxNackBitmapBytes, 0xffU, 0x00U},
	}};

	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.fragment_count);
		EXPECT_EQ(test_case.bytes, missing_fragment_bitmap_size(test_case.fragment_count));
		std::vector<std::uint8_t> bitmap(test_case.bytes, 0xffU);
		bitmap.back() = test_case.valid_tail;
		EXPECT_EQ(ValidationError::None, validate_missing_fragment_bitmap(byte_view(bitmap), test_case.fragment_count));
		if (test_case.first_reserved_tail_bit != 0U) {
			bitmap.back() = static_cast<std::uint8_t>(test_case.valid_tail | test_case.first_reserved_tail_bit);
			EXPECT_EQ(ValidationError::ReservedFlag,
				validate_missing_fragment_bitmap(byte_view(bitmap), test_case.fragment_count));
		}
	}

	EXPECT_EQ(ValidationError::BadFragmentCount, validate_missing_fragment_bitmap(ByteView{}, 0U));
	std::vector<std::uint8_t> too_large(MaxNackBitmapBytes + 1U, 0U);
	EXPECT_EQ(ValidationError::BadFragmentCount,
		validate_missing_fragment_bitmap(byte_view(too_large), static_cast<std::uint16_t>(MaxVideoFragments + 1U)));
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_missing_fragment_bitmap(ByteView{nullptr, 1U}, 1U));
	std::array<std::uint8_t, 2> two_bytes{};
	EXPECT_EQ(ValidationError::TrailingBytes, validate_missing_fragment_bitmap(byte_view(two_bytes), 8U));
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_missing_fragment_bitmap(ByteView{two_bytes.data(), 1U}, 9U));
}

TEST(TelemetryProtocolReliabilityMessages, BitmapInitializeSetClearQueryAndIteratorPreserveOutputsOnErrors)
{
	std::array<std::uint8_t, 2> bitmap{{0xa5U, 0xa5U}};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, initialize_missing_fragment_bitmap(9U, mutable_byte_view(bitmap), written));
	EXPECT_EQ(bitmap.size(), written);
	EXPECT_EQ(0U, bitmap[0U]);
	EXPECT_EQ(0U, bitmap[1U]);

	for (const auto index : {std::uint16_t{0}, std::uint16_t{7}, std::uint16_t{8}}) {
		ASSERT_EQ(ValidationError::None, set_fragment_missing(mutable_byte_view(bitmap), 9U, index));
		bool missing = false;
		ASSERT_EQ(ValidationError::None, is_fragment_missing(byte_view(bitmap), 9U, index, missing));
		EXPECT_TRUE(missing);
	}
	ASSERT_EQ(ValidationError::None, set_fragment_missing(mutable_byte_view(bitmap), 9U, 7U, false));
	bool missing = true;
	ASSERT_EQ(ValidationError::None, is_fragment_missing(byte_view(bitmap), 9U, 7U, missing));
	EXPECT_FALSE(missing);

	MissingFragmentIterator iterator(byte_view(bitmap), 9U);
	std::uint16_t index = 999U;
	bool has_value = false;
	ASSERT_EQ(ValidationError::None, iterator.next(index, has_value));
	ASSERT_TRUE(has_value);
	EXPECT_EQ(0U, index);
	ASSERT_EQ(ValidationError::None, iterator.next(index, has_value));
	ASSERT_TRUE(has_value);
	EXPECT_EQ(8U, index);
	ASSERT_EQ(ValidationError::None, iterator.next(index, has_value));
	EXPECT_FALSE(has_value);
	EXPECT_EQ(0U, index);
	ASSERT_EQ(ValidationError::None, iterator.next(index, has_value));
	EXPECT_FALSE(has_value);
	EXPECT_EQ(0U, index);

	const auto canary = bitmap;
	EXPECT_EQ(ValidationError::BadFragmentIndex, set_fragment_missing(mutable_byte_view(bitmap), 9U, 9U));
	EXPECT_EQ(canary, bitmap);
	missing = true;
	EXPECT_EQ(ValidationError::BadFragmentIndex, is_fragment_missing(byte_view(bitmap), 9U, 9U, missing));
	EXPECT_FALSE(missing);

	std::array<std::uint8_t, 1> bad_tail{{0x02U}};
	const auto bad_tail_canary = bad_tail;
	EXPECT_EQ(ValidationError::ReservedFlag, set_fragment_missing(mutable_byte_view(bad_tail), 1U, 0U));
	EXPECT_EQ(bad_tail_canary, bad_tail);
	MissingFragmentIterator invalid_iterator(byte_view(bad_tail), 1U);
	index = 999U;
	has_value = true;
	EXPECT_EQ(ValidationError::ReservedFlag, invalid_iterator.next(index, has_value));
	EXPECT_EQ(ValidationError::ReservedFlag, invalid_iterator.error());
	EXPECT_EQ(0U, index);
	EXPECT_FALSE(has_value);

	std::array<std::uint8_t, 1> short_output{{0xa5U}};
	const auto short_canary = short_output;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		initialize_missing_fragment_bitmap(9U, mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(short_canary, short_output);
	written = 99U;
	EXPECT_EQ(ValidationError::BadFragmentCount,
		initialize_missing_fragment_bitmap(0U, mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(short_canary, short_output);
}

TEST(TelemetryProtocolReliabilityMessages, BitmapIteratorCoversThe2048FragmentMaximum)
{
	std::vector<std::uint8_t> bitmap(MaxNackBitmapBytes, 0U);
	for (const auto index :
		{std::uint16_t{0}, std::uint16_t{7}, std::uint16_t{8}, std::uint16_t{1023}, std::uint16_t{2047}}) {
		ASSERT_EQ(ValidationError::None, set_fragment_missing(mutable_byte_view(bitmap), 2048U, index));
	}
	MissingFragmentIterator iterator(byte_view(bitmap), 2048U);
	for (const auto expected :
		{std::uint16_t{0}, std::uint16_t{7}, std::uint16_t{8}, std::uint16_t{1023}, std::uint16_t{2047}}) {
		std::uint16_t actual = 0U;
		bool has_value = false;
		ASSERT_EQ(ValidationError::None, iterator.next(actual, has_value));
		ASSERT_TRUE(has_value);
		EXPECT_EQ(expected, actual);
	}
	std::uint16_t actual = 99U;
	bool has_value = true;
	ASSERT_EQ(ValidationError::None, iterator.next(actual, has_value));
	EXPECT_FALSE(has_value);
	EXPECT_EQ(0U, actual);
}

TEST(TelemetryProtocolReliabilityMessages, NackReasonIdentityBitmapAndDeadlineRulesAreClosed)
{
	std::array<std::uint8_t, 2> bitmap{{0x81U, 0x01U}};
	auto payload = valid_nack(byte_view(bitmap));
	EXPECT_EQ(ValidationError::None, validate_nack_payload(payload));

	for (std::uint8_t raw_reason = 1U; raw_reason <= 8U; ++raw_reason) {
		payload = valid_nack();
		payload.target_message_type = MessageType::FullSnapshot;
		payload.target_fragment_count = 1U;
		payload.needed_before_producer_time_us = 0U;
		payload.reason = static_cast<NackReason>(raw_reason);
		if (payload.reason == NackReason::MissingFragments) {
			static const std::array<std::uint8_t, 1> one_missing{{0x01U}};
			payload.missing_bitmap = byte_view(one_missing);
		} else if (payload.reason == NackReason::UnsupportedMessage) {
			payload.target_message_type = static_cast<MessageType>(FirstReservedMessageType);
		}
		SCOPED_TRACE(raw_reason);
		EXPECT_EQ(ValidationError::None, validate_nack_payload(payload));
	}

	payload = valid_nack(byte_view(bitmap));
	payload.reason = NackReason::Invalid;
	EXPECT_EQ(ValidationError::UnknownEnum, validate_nack_payload(payload));
	payload.reason = static_cast<NackReason>(9U);
	EXPECT_EQ(ValidationError::UnknownEnum, validate_nack_payload(payload));

	payload = valid_nack();
	payload.reason = NackReason::UnsupportedMessage;
	payload.target_message_type = MessageType::FullSnapshot;
	payload.target_fragment_count = 1U;
	payload.needed_before_producer_time_us = 0U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_nack_payload(payload));
	payload.target_message_type = static_cast<MessageType>(FirstReservedMessageType);
	EXPECT_EQ(ValidationError::None, validate_nack_payload(payload));
	payload.reason = NackReason::BadMessageCrc;
	EXPECT_EQ(ValidationError::UnknownMessageType, validate_nack_payload(payload));
	payload.reason = NackReason::UnsupportedMessage;
	payload.target_message_type = MessageType::Invalid;
	EXPECT_EQ(ValidationError::UnknownMessageType, validate_nack_payload(payload));

	payload = valid_nack(byte_view(bitmap));
	payload.target_message_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_nack_payload(payload));
	payload = valid_nack(byte_view(bitmap));
	payload.target_fragment_count = 0U;
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_nack_payload(payload));
	payload.target_fragment_count = static_cast<std::uint16_t>(MaxVideoFragments + 1U);
	EXPECT_EQ(ValidationError::BadFragmentCount, validate_nack_payload(payload));

	payload = valid_nack();
	payload.reason = NackReason::BadMessageCrc;
	payload.target_message_type = MessageType::FullSnapshot;
	payload.target_fragment_count = 1U;
	payload.needed_before_producer_time_us = 1U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_nack_payload(payload));
	const std::array<std::uint8_t, 1> unexpected_bitmap{{0x01U}};
	payload.needed_before_producer_time_us = 0U;
	payload.missing_bitmap = byte_view(unexpected_bitmap);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_nack_payload(payload));
}

TEST(TelemetryProtocolReliabilityMessages, NackMaximumBitmapAndEncodingOutputSemantics)
{
	std::vector<std::uint8_t> bitmap(MaxNackBitmapBytes, 0xffU);
	auto payload = valid_nack(byte_view(bitmap));
	payload.target_fragment_count = static_cast<std::uint16_t>(MaxVideoFragments);
	ASSERT_EQ(ValidationError::None, validate_nack_payload(payload));
	std::vector<std::uint8_t> encoded(NackPayloadPrefixSize + bitmap.size(), 0xa5U);
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_nack_payload(payload, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);
	NackPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_nack_payload(byte_view(encoded), decoded));
	EXPECT_EQ(MaxVideoFragments, decoded.target_fragment_count);
	EXPECT_EQ(MaxNackBitmapBytes, decoded.missing_bitmap.size);
	EXPECT_EQ(0xffU, decoded.missing_bitmap.data[MaxNackBitmapBytes - 1U]);

	std::vector<std::uint8_t> short_output(encoded.size() - 1U, 0xa5U);
	const auto short_canary = short_output;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_nack_payload(payload, mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(short_canary, short_output);

	const auto canary = encoded;
	payload.target_message_id = 0U;
	written = 99U;
	EXPECT_EQ(ValidationError::OutOfRange, encode_nack_payload(payload, mutable_byte_view(encoded), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, encoded);
}

TEST(TelemetryProtocolReliabilityMessages, ReliabilityTargetTupleRequiresExactIdentityAndDeadlineClass)
{
	const auto ack = valid_ack();
	ReliabilityTargetTuple target;
	target.message_id = ack.target_message_id;
	target.message_type = ack.target_message_type;
	target.fragment_count = ack.target_fragment_count;
	target.message_crc32 = ack.target_message_crc32;
	ASSERT_EQ(ValidationError::None, validate_ack_target(ack, target));

	auto mismatch = target;
	mismatch.message_id++;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_ack_target(ack, mismatch));
	mismatch = target;
	mismatch.message_type = MessageType::Delta;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_ack_target(ack, mismatch));
	mismatch = target;
	mismatch.fragment_count++;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_ack_target(ack, mismatch));
	mismatch = target;
	mismatch.message_crc32++;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_ack_target(ack, mismatch));
	mismatch = target;
	mismatch.deadline_bearing = true;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_ack_target(ack, mismatch));
	mismatch = target;
	mismatch.message_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_ack_target(ack, mismatch));

	std::array<std::uint8_t, 2> bitmap{{0x81U, 0x01U}};
	const auto nack = valid_nack(byte_view(bitmap));
	ReliabilityTargetTuple video_target;
	video_target.message_id = nack.target_message_id;
	video_target.message_type = nack.target_message_type;
	video_target.fragment_count = nack.target_fragment_count;
	video_target.message_crc32 = nack.target_message_crc32;
	video_target.deadline_bearing = true;
	EXPECT_EQ(ValidationError::None, validate_nack_target(nack, video_target));
	video_target.deadline_bearing = false;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_nack_target(nack, video_target));

	auto no_deadline = nack;
	no_deadline.needed_before_producer_time_us = 0U;
	EXPECT_EQ(ValidationError::None, validate_nack_target(no_deadline, video_target));
	video_target.message_crc32++;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_nack_target(no_deadline, video_target));

	NackPayload unsupported;
	unsupported.target_message_id = 7U;
	unsupported.target_message_type = static_cast<MessageType>(FirstReservedMessageType);
	unsupported.reason = NackReason::UnsupportedMessage;
	unsupported.target_fragment_count = 1U;
	unsupported.target_message_crc32 = 9U;
	ReliabilityTargetTuple unknown_target;
	unknown_target.message_id = 7U;
	unknown_target.message_type = static_cast<MessageType>(FirstReservedMessageType);
	unknown_target.fragment_count = 1U;
	unknown_target.message_crc32 = 9U;
	EXPECT_EQ(ValidationError::None, validate_nack_target(unsupported, unknown_target));
}

TEST(TelemetryProtocolReliabilityMessages, ResyncGoldenBytesAndRequireFullSnapshotIdRelation)
{
	const auto original = valid_resync();
	std::array<std::uint8_t, ResyncRequestPayloadSize> encoded{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_resync_request_payload(original, mutable_byte_view(encoded), written));
	const std::array<std::uint8_t, ResyncRequestPayloadSize> golden{{
		0x44U,
		0x33U,
		0x22U,
		0x11U,
		0x04U,
		0x03U,
		0x00U,
		0x00U,
		0x88U,
		0x77U,
		0x66U,
		0x55U,
		0xccU,
		0xbbU,
		0xaaU,
		0x99U,
		0x08U,
		0x07U,
		0x06U,
		0x05U,
		0x04U,
		0x03U,
		0x02U,
		0x01U,
	}};
	EXPECT_EQ(golden, encoded);
	EXPECT_EQ(golden.size(), written);

	ResyncRequestPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_resync_request_payload(byte_view(golden), decoded));
	EXPECT_EQ(original.request_id, decoded.request_id);
	EXPECT_EQ(original.reason, decoded.reason);
	EXPECT_EQ(original.request_flags, decoded.request_flags);
	EXPECT_EQ(original.last_applied_snapshot_id, decoded.last_applied_snapshot_id);
	EXPECT_EQ(original.last_applied_delta_sequence, decoded.last_applied_delta_sequence);
	EXPECT_EQ(original.client_send_time_us, decoded.client_send_time_us);

	auto payload = valid_resync();
	payload.request_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_resync_request_payload(payload));
	payload = valid_resync();
	payload.reason = ResyncReason::Invalid;
	EXPECT_EQ(ValidationError::UnknownEnum, validate_resync_request_payload(payload));
	payload.reason = static_cast<ResyncReason>(6U);
	EXPECT_EQ(ValidationError::UnknownEnum, validate_resync_request_payload(payload));
	payload = valid_resync();
	payload.request_flags = ResyncRequestFlagNone;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_resync_request_payload(payload));
	payload.request_flags = ResyncRequestFlagRequireManifest;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_resync_request_payload(payload));
	payload.request_flags = ResyncRequestFlagRequireFullSnapshot;
	EXPECT_EQ(ValidationError::None, validate_resync_request_payload(payload));
	payload.request_flags = KnownResyncRequestFlags;
	EXPECT_EQ(ValidationError::None, validate_resync_request_payload(payload));
	payload.request_flags = 0x04U;
	EXPECT_EQ(ValidationError::ReservedFlag, validate_resync_request_payload(payload));
	payload = valid_resync();
	payload.last_applied_snapshot_id = 0U;
	payload.last_applied_delta_sequence = 1U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_resync_request_payload(payload));
	payload.last_applied_delta_sequence = 0U;
	EXPECT_EQ(ValidationError::None, validate_resync_request_payload(payload));
}

TEST(TelemetryProtocolReliabilityMessages, ResyncExactWireReservedAndOutputSemantics)
{
	std::vector<std::uint8_t> bytes(ResyncRequestPayloadSize, 0U);
	std::size_t written = 0U;
	ASSERT_EQ(ValidationError::None, encode_resync_request_payload(valid_resync(), mutable_byte_view(bytes), written));
	ResyncRequestPayload decoded;
	for (std::size_t size = 0; size < bytes.size(); ++size) {
		decoded = valid_resync();
		EXPECT_EQ(ValidationError::TruncatedPayload,
			decode_resync_request_payload(ByteView{bytes.data(), size}, decoded));
		EXPECT_EQ(0U, decoded.request_id);
	}
	auto trailing = bytes;
	trailing.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_resync_request_payload(byte_view(trailing), decoded));
	auto reserved = bytes;
	reserved[6U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_resync_request_payload(byte_view(reserved), decoded));
	reserved = bytes;
	reserved[7U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_resync_request_payload(byte_view(reserved), decoded));

	std::array<std::uint8_t, ResyncRequestPayloadSize - 1U> short_output{};
	short_output.fill(0xa5U);
	const auto canary = short_output;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_resync_request_payload(valid_resync(), mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, short_output);
}

TEST(TelemetryProtocolReliabilityMessages, SessionEndGoldenBytesClosedEnumsFlagsAndExactWire)
{
	const auto original = valid_session_end();
	std::array<std::uint8_t, SessionEndPayloadSize> encoded{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_session_end_payload(original, mutable_byte_view(encoded), written));
	const std::array<std::uint8_t, SessionEndPayloadSize> golden{{
		0x04U,
		0x01U,
		0x00U,
		0x00U,
		0x44U,
		0x33U,
		0x22U,
		0x11U,
		0x08U,
		0x07U,
		0x06U,
		0x05U,
		0x04U,
		0x03U,
		0x02U,
		0x01U,
	}};
	EXPECT_EQ(golden, encoded);
	EXPECT_EQ(golden.size(), written);

	SessionEndPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_session_end_payload(byte_view(golden), decoded));
	EXPECT_EQ(original.reason, decoded.reason);
	EXPECT_EQ(original.end_flags, decoded.end_flags);
	EXPECT_EQ(original.last_snapshot_id, decoded.last_snapshot_id);
	EXPECT_EQ(original.producer_sample_time_us, decoded.producer_sample_time_us);

	for (std::uint8_t reason = 1U; reason <= 6U; ++reason) {
		auto payload = valid_session_end();
		payload.reason = static_cast<SessionEndReason>(reason);
		EXPECT_EQ(ValidationError::None, validate_session_end_payload(payload));
	}
	auto payload = valid_session_end();
	payload.reason = static_cast<SessionEndReason>(0U);
	EXPECT_EQ(ValidationError::UnknownEnum, validate_session_end_payload(payload));
	payload.reason = static_cast<SessionEndReason>(7U);
	EXPECT_EQ(ValidationError::UnknownEnum, validate_session_end_payload(payload));
	payload = valid_session_end();
	payload.end_flags = SessionEndFlagNone;
	EXPECT_EQ(ValidationError::None, validate_session_end_payload(payload));
	payload.end_flags = 0x02U;
	EXPECT_EQ(ValidationError::ReservedFlag, validate_session_end_payload(payload));

	for (std::size_t size = 0; size < golden.size(); ++size) {
		decoded = original;
		EXPECT_EQ(ValidationError::TruncatedPayload,
			decode_session_end_payload(ByteView{golden.data(), size}, decoded));
		EXPECT_EQ(0U, decoded.last_snapshot_id);
	}
	auto trailing = std::vector<std::uint8_t>(golden.begin(), golden.end());
	trailing.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_session_end_payload(byte_view(trailing), decoded));
	auto reserved = std::vector<std::uint8_t>(golden.begin(), golden.end());
	reserved[2U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_session_end_payload(byte_view(reserved), decoded));
	reserved[2U] = 0U;
	reserved[3U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_session_end_payload(byte_view(reserved), decoded));
}

TEST(TelemetryProtocolReliabilityMessages, SessionEndEncodingIsAtomicOnValidationAndCapacityFailures)
{
	std::array<std::uint8_t, SessionEndPayloadSize> output{};
	output.fill(0xa5U);
	const auto canary = output;
	auto invalid = valid_session_end();
	invalid.end_flags = 0x02U;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::ReservedFlag, encode_session_end_payload(invalid, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, output);

	std::array<std::uint8_t, SessionEndPayloadSize - 1U> short_output{};
	short_output.fill(0xa5U);
	const auto short_canary = short_output;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_session_end_payload(valid_session_end(), mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(short_canary, short_output);
}

} // namespace
