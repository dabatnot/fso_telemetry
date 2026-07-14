#include "telemetry/protocol/telemetry_business_records.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace telemetry::protocol;

void u8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
{
	bytes.push_back(value);
}

void u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (unsigned int shift = 0; shift < 32; shift += 8) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void u64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for (unsigned int shift = 0; shift < 64; shift += 8) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

std::vector<std::uint8_t> event_item(std::uint64_t event_id,
	EventKind kind,
	std::uint16_t flags,
	std::uint32_t presence = 0,
	std::uint64_t subject = 7)
{
	std::vector<std::uint8_t> bytes;
	u32(bytes, presence);
	u64(bytes, event_id);
	u64(bytes, 1000 + event_id);
	u16(bytes, static_cast<std::uint16_t>(kind));
	u16(bytes, flags);
	u64(bytes, subject);
	return bytes;
}

void append_item(std::vector<std::uint8_t>& payload, const std::vector<std::uint8_t>& item)
{
	u8(payload, 1);
	u16(payload, static_cast<std::uint16_t>(item.size()));
	payload.insert(payload.end(), item.begin(), item.end());
}

ValidationError validate(const std::vector<std::uint8_t>& payload, BusinessRecordContainer container)
{
	const RecordEnvelopeView record{static_cast<std::uint16_t>(RecordType::Events),
		1,
		RecordFlagCreate,
		ByteView{payload.data(), payload.size()}};
	BusinessRecordMetadata metadata;
	return validate_business_record(record, container, metadata);
}

TEST(TelemetryProtocolBusinessRecords28, ReplaceableAndReliableDeliveryClassesHaveDistinctFlags)
{
	std::vector<std::uint8_t> replaceable;
	u16(replaceable, 1);
	append_item(replaceable,
		event_item(1, EventKind::EntityAppeared, static_cast<std::uint16_t>(EventFlagReconstructible)));
	EXPECT_EQ(ValidationError::None,
		validate(replaceable, BusinessRecordContainer::EventBatchReplaceable));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate(replaceable, BusinessRecordContainer::EventBatchReliable));

	std::vector<std::uint8_t> reliable;
	u16(reliable, 1);
	append_item(reliable, event_item(1, EventKind::EntityAppeared, static_cast<std::uint16_t>(EventFlagReliable)));
	EXPECT_EQ(ValidationError::None, validate(reliable, BusinessRecordContainer::EventBatchReliable));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate(reliable, BusinessRecordContainer::EventBatchReplaceable));
}

TEST(TelemetryProtocolBusinessRecords28, EventIdsAreStrictlyIncreasingAndItemsConsumeTheirExactSize)
{
	std::vector<std::uint8_t> payload;
	u16(payload, 2);
	append_item(payload, event_item(2, EventKind::EntityAppeared, EventFlagReliable));
	append_item(payload, event_item(1, EventKind::EntityAppeared, EventFlagReliable));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate(payload, BusinessRecordContainer::EventBatchReliable));

	payload.clear();
	u16(payload, 1);
	auto item = event_item(1, EventKind::EntityAppeared, EventFlagReliable);
	item.push_back(0);
	append_item(payload, item);
	EXPECT_EQ(ValidationError::TrailingBytes, validate(payload, BusinessRecordContainer::EventBatchReliable));
}

TEST(TelemetryProtocolBusinessRecords28, KindMatrixRequiresAndForbidsFieldsExactly)
{
	std::vector<std::uint8_t> missing;
	u16(missing, 1);
	append_item(missing, event_item(1, EventKind::WeaponFired, EventFlagReliable));
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(missing, BusinessRecordContainer::EventBatchReliable));

	std::vector<std::uint8_t> forbidden;
	u16(forbidden, 1);
	append_item(forbidden,
		event_item(1,
			EventKind::AfterburnerStarted,
			EventFlagReliable,
			EventItemPresenceFlagPosition));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate(forbidden, BusinessRecordContainer::EventBatchReliable));
}

TEST(TelemetryProtocolBusinessRecords28, MissionEventsRequireZeroSubjectAndTransientEventsRequireReliableDelivery)
{
	std::vector<std::uint8_t> mission;
	u16(mission, 1);
	append_item(mission, event_item(1, EventKind::MissionStarted, EventFlagReliable, 0, 7));
	EXPECT_EQ(ValidationError::OutOfRange, validate(mission, BusinessRecordContainer::EventBatchReliable));

	mission.clear();
	u16(mission, 1);
	append_item(mission, event_item(1, EventKind::MissionStarted, EventFlagReliable, 0, 0));
	EXPECT_EQ(ValidationError::None, validate(mission, BusinessRecordContainer::EventBatchReliable));

	std::vector<std::uint8_t> fired;
	u16(fired, 1);
	append_item(fired,
		event_item(1,
			EventKind::WeaponFired,
			EventFlagReconstructible,
			EventItemPresenceFlagWeaponClass | EventItemPresenceFlagBank | EventItemPresenceFlagFirePoint));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate(fired, BusinessRecordContainer::EventBatchReplaceable));
}

} // namespace
