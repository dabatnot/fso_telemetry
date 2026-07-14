#include "telemetry/protocol/telemetry_event_messages.h"

#include "telemetry/protocol/telemetry_specialized_views.h"

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
ByteView byte_view(const Container& bytes)
{
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

template <typename Container>
MutableByteView mutable_byte_view(Container& bytes)
{
	return MutableByteView{static_cast<std::uint8_t*>(static_cast<void*>(bytes.data())), bytes.size()};
}

void append_u8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
{
	bytes.push_back(value);
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (unsigned int shift = 0; shift < 32U; shift += 8U) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for (unsigned int shift = 0; shift < 64U; shift += 8U) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

std::vector<std::uint8_t> record(std::uint16_t type,
	std::uint8_t flags,
	const std::vector<std::uint8_t>& payload,
	std::uint8_t version = 1U)
{
	std::vector<std::uint8_t> bytes;
	append_u16(bytes, type);
	append_u8(bytes, version);
	append_u8(bytes, flags);
	append_u16(bytes, static_cast<std::uint16_t>(payload.size()));
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	return bytes;
}

std::vector<std::uint8_t> events_record(std::uint64_t event_id,
	std::uint64_t sample_time_us,
	bool reliable)
{
	std::vector<std::uint8_t> item;
	append_u32(item, 0U);
	append_u64(item, event_id);
	append_u64(item, sample_time_us);
	append_u16(item, static_cast<std::uint16_t>(EventKind::EntityAppeared));
	append_u16(item,
		reliable ? static_cast<std::uint16_t>(EventFlagReliable)
				 : static_cast<std::uint16_t>(EventFlagReconstructible));
	append_u64(item, 7U);

	std::vector<std::uint8_t> payload;
	append_u16(payload, 1U);
	append_u8(payload, 1U);
	append_u16(payload, static_cast<std::uint16_t>(item.size()));
	payload.insert(payload.end(), item.begin(), item.end());
	return record(static_cast<std::uint16_t>(RecordType::Events), RecordFlagCreate, payload);
}

std::vector<std::uint8_t> comm_view_event_record(std::uint64_t event_id, std::uint64_t sample_time_us)
{
	CommViewEventPayload event;
	event.event_id = event_id;
	event.event_kind = CommEventKind::Stop;
	event.stop_reason = CommStopReason::Completed;
	event.playback_id = 1U;
	event.producer_sample_time_us = sample_time_us;
	std::array<std::uint8_t, CommViewEventPayloadSize> encoded{};
	std::size_t written = 0;
	EXPECT_EQ(ValidationError::None,
		encode_comm_view_event_payload(event, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);
	return record(static_cast<std::uint16_t>(RecordType::CommViewEvent),
		RecordFlagCreate,
		std::vector<std::uint8_t>(encoded.begin(), encoded.end()));
}

EventBatchPayload valid_batch(ByteView records,
	std::uint16_t record_count = 1U,
	EventDeliveryClass delivery_class = EventDeliveryClass::Reliable)
{
	EventBatchPayload payload;
	payload.batch_id = 0x11223344U;
	payload.first_event_id = 1U;
	payload.producer_sample_time_us = 1001U;
	payload.delivery_class = delivery_class;
	payload.record_count = record_count;
	payload.records = records;
	return payload;
}

TEST(TelemetryProtocolEventMessages, ReliableEventsGoldenPrefixRoundTripsAndEncodingIsAtomic)
{
	const auto records = events_record(1U, 1001U, true);
	const auto original = valid_batch(byte_view(records));
	std::vector<std::uint8_t> encoded(EventBatchPayloadPrefixSize + records.size(), 0xa5U);
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None,
		encode_event_batch_payload(original, true, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);
	const std::array<std::uint8_t, EventBatchPayloadPrefixSize> golden_prefix{{
		0x44U, 0x33U, 0x22U, 0x11U,
		0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
		0xe9U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
		0x02U, 0x00U, 0x01U, 0x00U,
	}};
	EXPECT_TRUE(std::equal(golden_prefix.begin(), golden_prefix.end(), encoded.begin()));
	EXPECT_TRUE(std::equal(records.begin(), records.end(), encoded.begin() + EventBatchPayloadPrefixSize));

	EventBatchPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_event_batch_payload(byte_view(encoded), true, decoded));
	EXPECT_EQ(original.batch_id, decoded.batch_id);
	EXPECT_EQ(original.first_event_id, decoded.first_event_id);
	EXPECT_EQ(original.producer_sample_time_us, decoded.producer_sample_time_us);
	EXPECT_EQ(original.delivery_class, decoded.delivery_class);
	EXPECT_EQ(original.record_count, decoded.record_count);
	EXPECT_EQ(records.size(), decoded.records.size);

	std::vector<std::uint8_t> short_output(encoded.size() - 1U, 0xa5U);
	const auto canary = short_output;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_event_batch_payload(original, true, mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, short_output);

	std::vector<std::uint8_t> aliased(encoded.size(), 0U);
	std::copy(records.begin(), records.end(), aliased.begin());
	auto alias_payload = valid_batch(ByteView{aliased.data(), records.size()});
	written = 99U;
	ASSERT_EQ(ValidationError::None,
		encode_event_batch_payload(alias_payload, true, mutable_byte_view(aliased), written));
	EXPECT_EQ(encoded, aliased);
}

TEST(TelemetryProtocolEventMessages, DeliveryClassAndAckRequiredMustAgree)
{
	const auto reliable_records = events_record(1U, 1001U, true);
	auto payload = valid_batch(byte_view(reliable_records));
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_event_batch_payload(payload, false));

	const auto replaceable_records = events_record(1U, 1001U, false);
	payload = valid_batch(byte_view(replaceable_records), 1U, EventDeliveryClass::Replaceable);
	EXPECT_EQ(ValidationError::None, validate_event_batch_payload(payload, false));
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_event_batch_payload(payload, true));
	payload.delivery_class = static_cast<EventDeliveryClass>(99U);
	EXPECT_EQ(ValidationError::UnknownEnum, validate_event_batch_payload(payload, false));
}

TEST(TelemetryProtocolEventMessages, FirstIdOrderingAndSampleTimeCoverEveryRecord)
{
	auto records = events_record(1U, 1001U, true);
	const auto second = events_record(2U, 1002U, true);
	records.insert(records.end(), second.begin(), second.end());
	auto payload = valid_batch(byte_view(records), 2U);
	payload.producer_sample_time_us = 1002U;
	EXPECT_EQ(ValidationError::None, validate_event_batch_payload(payload, true));
	payload.first_event_id = 2U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_event_batch_payload(payload, true));
	payload.first_event_id = 1U;
	payload.producer_sample_time_us = 1001U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_event_batch_payload(payload, true));

	records = events_record(2U, 1002U, true);
	const auto older = events_record(1U, 1001U, true);
	records.insert(records.end(), older.begin(), older.end());
	payload = valid_batch(byte_view(records), 2U);
	payload.first_event_id = 2U;
	payload.producer_sample_time_us = 1002U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_event_batch_payload(payload, true));
}

TEST(TelemetryProtocolEventMessages, CommViewEventsAreReliableOrderedAndNeverMixedWithEvents)
{
	auto records = comm_view_event_record(1U, 1001U);
	auto payload = valid_batch(byte_view(records));
	EXPECT_EQ(ValidationError::None, validate_event_batch_payload(payload, true));
	payload.delivery_class = EventDeliveryClass::Replaceable;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_event_batch_payload(payload, false));

	records = comm_view_event_record(1U, 1001U);
	const auto events = events_record(2U, 1002U, true);
	records.insert(records.end(), events.begin(), events.end());
	payload = valid_batch(byte_view(records), 2U);
	payload.producer_sample_time_us = 1002U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_event_batch_payload(payload, true));
}

TEST(TelemetryProtocolEventMessages, UnknownExtensionsAreSkippedButCannotFormAnEventBatchAlone)
{
	const auto extension = record(FirstReservedRecordType, RecordFlagNone, {}, 7U);
	auto payload = valid_batch(byte_view(extension));
	EXPECT_EQ(ValidationError::UnknownRequiredRecord, validate_event_batch_payload(payload, true));

	auto records = extension;
	const auto events = events_record(1U, 1001U, true);
	records.insert(records.end(), events.begin(), events.end());
	payload = valid_batch(byte_view(records), 2U);
	EXPECT_EQ(ValidationError::None, validate_event_batch_payload(payload, true));
}

TEST(TelemetryProtocolEventMessages, DecodeRejectsReservedTruncatedAndTrailingRecordBytesWithoutPublishing)
{
	const auto records = events_record(1U, 1001U, true);
	const auto original = valid_batch(byte_view(records));
	std::vector<std::uint8_t> encoded(EventBatchPayloadPrefixSize + records.size(), 0U);
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None,
		encode_event_batch_payload(original, true, mutable_byte_view(encoded), written));

	EventBatchPayload decoded;
	auto malformed = encoded;
	malformed[21U] = 1U;
	decoded.batch_id = 99U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_event_batch_payload(byte_view(malformed), true, decoded));
	EXPECT_EQ(0U, decoded.batch_id);
	malformed = encoded;
	malformed.pop_back();
	EXPECT_EQ(ValidationError::BadRecordLength, decode_event_batch_payload(byte_view(malformed), true, decoded));
	malformed = encoded;
	malformed.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_event_batch_payload(byte_view(malformed), true, decoded));
	EXPECT_EQ(ValidationError::TruncatedPayload,
		decode_event_batch_payload(ByteView{nullptr, 1U}, true, decoded));
}

} // namespace
