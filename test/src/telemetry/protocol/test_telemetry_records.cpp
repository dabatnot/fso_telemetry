#include "telemetry/protocol/telemetry_records.h"

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

std::vector<std::uint8_t>
record(std::uint16_t raw_type, std::uint8_t version, std::uint8_t flags, const std::vector<std::uint8_t>& payload = {})
{
	EXPECT_LE(payload.size(), std::numeric_limits<std::uint16_t>::max());
	const auto payload_size = static_cast<std::uint16_t>(payload.size());
	std::vector<std::uint8_t> encoded;
	encoded.reserve(RecordEnvelopeHeaderSize + payload.size());
	encoded.push_back(static_cast<std::uint8_t>(raw_type & 0xffU));
	encoded.push_back(static_cast<std::uint8_t>(raw_type >> 8U));
	encoded.push_back(version);
	encoded.push_back(flags);
	encoded.push_back(static_cast<std::uint8_t>(payload_size & 0xffU));
	encoded.push_back(static_cast<std::uint8_t>(payload_size >> 8U));
	encoded.insert(encoded.end(), payload.begin(), payload.end());
	return encoded;
}

void append(std::vector<std::uint8_t>& destination, const std::vector<std::uint8_t>& source)
{
	destination.insert(destination.end(), source.begin(), source.end());
}

TEST(TelemetryProtocolRecords, IteratesKnownAndUnknownEnvelopesWithoutCopyingPayloads)
{
	const auto first = record(static_cast<std::uint16_t>(RecordType::SessionState), 1U, RecordFlagNone, {0x10U, 0x11U});
	const auto second = record(FirstReservedRecordType, 9U, RecordFlagCreate, {0x20U});
	const auto third = record(std::numeric_limits<std::uint16_t>::max(), 0U, RecordFlagDelete);
	std::vector<std::uint8_t> region;
	append(region, first);
	append(region, second);
	append(region, third);

	ASSERT_EQ(ValidationError::None, validate_record_region(byte_view(region), 3U, RecordFlagPolicy::AllowV1Mutations));
	RecordEnvelopeIterator iterator(byte_view(region), 3U, RecordFlagPolicy::AllowV1Mutations);
	RecordEnvelopeView decoded;
	bool has_value = false;

	ASSERT_EQ(ValidationError::None, iterator.next(decoded, has_value));
	ASSERT_TRUE(has_value);
	EXPECT_EQ(static_cast<std::uint16_t>(RecordType::SessionState), decoded.raw_record_type);
	EXPECT_EQ(1U, decoded.record_version);
	EXPECT_EQ(RecordFlagNone, decoded.record_flags);
	EXPECT_EQ(region.data() + RecordEnvelopeHeaderSize, decoded.payload.data);
	EXPECT_EQ(2U, decoded.payload.size);

	ASSERT_EQ(ValidationError::None, iterator.next(decoded, has_value));
	ASSERT_TRUE(has_value);
	EXPECT_EQ(FirstReservedRecordType, decoded.raw_record_type);
	EXPECT_EQ(9U, decoded.record_version);
	EXPECT_EQ(RecordFlagCreate, decoded.record_flags);
	EXPECT_EQ(region.data() + first.size() + RecordEnvelopeHeaderSize, decoded.payload.data);
	ASSERT_EQ(1U, decoded.payload.size);
	EXPECT_EQ(0x20U, decoded.payload.data[0]);

	ASSERT_EQ(ValidationError::None, iterator.next(decoded, has_value));
	ASSERT_TRUE(has_value);
	EXPECT_EQ(std::numeric_limits<std::uint16_t>::max(), decoded.raw_record_type);
	EXPECT_EQ(0U, decoded.record_version);
	EXPECT_EQ(RecordFlagDelete, decoded.record_flags);
	EXPECT_TRUE(decoded.payload.empty());
	EXPECT_EQ(nullptr, decoded.payload.data);
	EXPECT_EQ(3U, iterator.consumed_count());
	EXPECT_EQ(region.size(), iterator.consumed_bytes());

	decoded.raw_record_type = 0xfeedU;
	has_value = true;
	EXPECT_EQ(ValidationError::None, iterator.next(decoded, has_value));
	EXPECT_FALSE(has_value);
	EXPECT_EQ(0U, decoded.raw_record_type);
	EXPECT_EQ(3U, iterator.consumed_count());
	EXPECT_EQ(region.size(), iterator.consumed_bytes());
}

TEST(TelemetryProtocolRecords, EnforcesAllGenericV1FlagRulesWithoutRecordTypeSemantics)
{
	struct Case {
		std::uint8_t flags;
		RecordFlagPolicy policy;
		ValidationError expected;
	};
	const std::array<Case, 10> cases{{
		{RecordFlagNone, RecordFlagPolicy::AllowV1Mutations, ValidationError::None},
		{RecordFlagCreate, RecordFlagPolicy::AllowV1Mutations, ValidationError::None},
		{RecordFlagDelete, RecordFlagPolicy::AllowV1Mutations, ValidationError::None},
		{RecordFlagNone, RecordFlagPolicy::RequireNone, ValidationError::None},
		{RecordFlagCreate, RecordFlagPolicy::RequireNone, ValidationError::InvalidStateTransition},
		{RecordFlagDelete, RecordFlagPolicy::RequireNone, ValidationError::InvalidStateTransition},
		{RecordFlagCreate | RecordFlagDelete,
			RecordFlagPolicy::AllowV1Mutations,
			ValidationError::InvalidStateTransition},
		{RecordFlagPartial, RecordFlagPolicy::AllowV1Mutations, ValidationError::ReservedFlag},
		{RecordFlagCreate | RecordFlagPartial, RecordFlagPolicy::AllowV1Mutations, ValidationError::ReservedFlag},
		{RecordFlagDelete | RecordFlagPartial, RecordFlagPolicy::AllowV1Mutations, ValidationError::ReservedFlag},
	}};

	for (const auto& test_case : cases) {
		SCOPED_TRACE(testing::Message() << "flags=" << static_cast<unsigned int>(test_case.flags)
										<< " policy=" << static_cast<unsigned int>(test_case.policy));
		EXPECT_EQ(test_case.expected, validate_record_flags_v1(test_case.flags, test_case.policy));
		const auto encoded = record(1U, 1U, test_case.flags);
		EXPECT_EQ(test_case.expected, validate_record_region(byte_view(encoded), 1U, test_case.policy));
	}

	for (unsigned int bit = 3U; bit < 8U; ++bit) {
		SCOPED_TRACE(testing::Message() << "reserved bit " << bit);
		const auto flags = static_cast<std::uint8_t>(1U << bit);
		EXPECT_EQ(ValidationError::ReservedFlag, validate_record_flags_v1(flags, RecordFlagPolicy::AllowV1Mutations));
		const auto encoded = record(1U, 1U, flags);
		EXPECT_EQ(ValidationError::ReservedFlag,
			validate_record_region(byte_view(encoded), 1U, RecordFlagPolicy::AllowV1Mutations));
	}

	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_record_flags_v1(RecordFlagNone, static_cast<RecordFlagPolicy>(0xffU)));
}

TEST(TelemetryProtocolRecords, RejectsInvalidTypeButSkipsUnknownTypesAndVersionsStructurally)
{
	const auto invalid = record(0U, 1U, RecordFlagNone);
	EXPECT_EQ(ValidationError::OutOfRange, validate_record_region(byte_view(invalid), 1U));

	const auto unknown_type = record(FirstReservedRecordType, 1U, RecordFlagNone, {0xaaU});
	EXPECT_EQ(ValidationError::None, validate_record_region(byte_view(unknown_type), 1U));
	const auto unknown_version = record(static_cast<std::uint16_t>(RecordType::SessionState),
		std::numeric_limits<std::uint8_t>::max(),
		RecordFlagNone,
		{0xbbU});
	EXPECT_EQ(ValidationError::None, validate_record_region(byte_view(unknown_version), 1U));
}

TEST(TelemetryProtocolRecords, DistinguishesTruncatedHeadersFromInvalidDeclaredLengths)
{
	const auto complete = record(1U, 1U, RecordFlagNone, {0x10U, 0x11U, 0x12U, 0x13U});
	ASSERT_EQ(10U, complete.size());
	for (std::size_t size = 0; size < complete.size(); ++size) {
		SCOPED_TRACE(testing::Message() << "size " << size);
		const auto expected =
			size < RecordEnvelopeHeaderSize ? ValidationError::TruncatedPayload : ValidationError::BadRecordLength;
		EXPECT_EQ(expected, validate_record_region(ByteView{complete.data(), size}, 1U));
	}
	EXPECT_EQ(ValidationError::None, validate_record_region(byte_view(complete), 1U));

	const std::uint8_t byte = 0U;
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_record_region(ByteView{nullptr, 1U}, 1U));
	EXPECT_EQ(ValidationError::TruncatedPayload,
		validate_record_region(ByteView{nullptr, std::numeric_limits<std::size_t>::max()}, 1U));
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_record_region(ByteView{&byte, 0U}, 1U));
}

TEST(TelemetryProtocolRecords, ConsumesExactlyTheDeclaredCountAndRejectsTrailingBytes)
{
	const auto first = record(1U, 1U, RecordFlagNone, {0x10U});
	const auto second = record(2U, 1U, RecordFlagNone, {0x20U, 0x21U});
	std::vector<std::uint8_t> region;
	append(region, first);
	append(region, second);

	EXPECT_EQ(ValidationError::None, validate_record_region(ByteView{}, 0U));
	EXPECT_EQ(ValidationError::TrailingBytes, validate_record_region(byte_view(first), 0U));
	EXPECT_EQ(ValidationError::TrailingBytes, validate_record_region(byte_view(region), 1U));
	EXPECT_EQ(ValidationError::None, validate_record_region(byte_view(region), 2U));
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_record_region(byte_view(region), 3U));

	region.push_back(0x5aU);
	EXPECT_EQ(ValidationError::TrailingBytes, validate_record_region(byte_view(region), 2U));
	region.pop_back();
	region.push_back(0x01U);
	region.push_back(0x00U);
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_record_region(byte_view(region), 3U));
}

TEST(TelemetryProtocolRecords, SupportsTheExactU16PayloadLimitAndRejectsOneMissingByte)
{
	std::vector<std::uint8_t> maximum_payload(std::numeric_limits<std::uint16_t>::max(), 0xa5U);
	auto maximum = record(std::numeric_limits<std::uint16_t>::max(), 1U, RecordFlagNone, maximum_payload);
	ASSERT_EQ(RecordEnvelopeHeaderSize + maximum_payload.size(), maximum.size());
	EXPECT_EQ(ValidationError::None, validate_record_region(byte_view(maximum), 1U));

	maximum.pop_back();
	EXPECT_EQ(ValidationError::BadRecordLength, validate_record_region(byte_view(maximum), 1U));
	EXPECT_EQ(ValidationError::TruncatedPayload,
		validate_record_region(ByteView{}, std::numeric_limits<std::uint16_t>::max()));
}

TEST(TelemetryProtocolRecords, IteratorErrorsAreStickyAndNeverPublishPartialEnvelopeMetadata)
{
	auto malformed = record(1U, 1U, RecordFlagNone, {0x10U});
	malformed[4U] = 2U;
	RecordEnvelopeIterator iterator(byte_view(malformed), 1U);
	RecordEnvelopeView output;
	output.raw_record_type = 0xfeedU;
	output.record_version = 0xffU;
	output.record_flags = 0xffU;
	const std::uint8_t sentinel = 0x5aU;
	output.payload = ByteView{&sentinel, 1U};
	bool has_value = true;

	EXPECT_EQ(ValidationError::BadRecordLength, iterator.next(output, has_value));
	EXPECT_FALSE(has_value);
	EXPECT_EQ(0U, output.raw_record_type);
	EXPECT_EQ(0U, output.record_version);
	EXPECT_EQ(RecordFlagNone, output.record_flags);
	EXPECT_TRUE(output.payload.empty());
	EXPECT_EQ(0U, iterator.consumed_count());
	EXPECT_EQ(0U, iterator.consumed_bytes());

	output.raw_record_type = 0xbeefU;
	has_value = true;
	EXPECT_EQ(ValidationError::BadRecordLength, iterator.next(output, has_value));
	EXPECT_FALSE(has_value);
	EXPECT_EQ(0U, output.raw_record_type);
	EXPECT_EQ(ValidationError::BadRecordLength, iterator.error());
}

} // namespace
