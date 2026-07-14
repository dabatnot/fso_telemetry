#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_reliable_receive.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using namespace telemetry::protocol;

constexpr std::uint8_t FragmentedFlag = static_cast<std::uint8_t>(MessageFlagFragmented);
constexpr std::uint8_t AckRequiredFlag = static_cast<std::uint8_t>(MessageFlagAckRequired);
constexpr std::uint8_t KeyframeFlag = static_cast<std::uint8_t>(MessageFlagKeyframe);
constexpr std::uint8_t VideoIdrFlag = static_cast<std::uint8_t>(MessageFlagVideoIdr);
constexpr std::uint8_t RetransmissionFlag = static_cast<std::uint8_t>(MessageFlagRetransmission);
constexpr std::uint8_t FragmentedAckFlags = FragmentedFlag | AckRequiredFlag;

EndpointKey endpoint(std::uint8_t last_octet = 1U, std::uint16_t port = 7808U)
{
	return EndpointKey::from_ipv4({{127U, 0U, 0U, last_octet}}, port);
}

ReliableReceiveKey receive_key(std::uint32_t message_id = 1U,
	MessageType message_type = MessageType::SessionBegin,
	std::uint64_t session_id = 0x1122334455667788ULL,
	EndpointKey peer = endpoint(),
	std::uint32_t message_crc32 = 0xa1b2c3d4U,
	std::uint16_t fragment_count = 1U)
{
	ReliableReceiveKey key;
	key.session_id = session_id;
	key.endpoint = peer;
	key.message_type = message_type;
	key.message_id = message_id;
	key.message_crc32 = message_crc32;
	key.fragment_count = fragment_count;
	return key;
}

const std::array<std::uint8_t, MaxFragmentPayload>& fragment_payload_storage()
{
	static const std::array<std::uint8_t, MaxFragmentPayload> payload{};
	return payload;
}

DatagramView fragment(MessageType message_type,
	std::uint16_t fragment_index,
	std::uint16_t fragment_count,
	std::uint32_t message_id,
	std::uint32_t message_crc32,
	std::uint64_t session_id,
	std::uint8_t flags,
	std::uint16_t final_fragment_size = 37U)
{
	TelemetryDatagramHeader header;
	header.message_type = message_type;
	header.flags = flags;
	header.session_id = session_id;
	header.packet_sequence = static_cast<std::uint32_t>(1000U + fragment_index);
	header.message_id = message_id;
	header.fragment_index = fragment_index;
	header.fragment_count = fragment_count;
	header.message_size = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(fragment_count - 1U) * MaxFragmentPayload + final_fragment_size);
	header.fragment_offset =
		static_cast<std::uint32_t>(static_cast<std::uint64_t>(fragment_index) * MaxFragmentPayload);
	header.payload_size =
		fragment_index + 1U == fragment_count ? final_fragment_size : static_cast<std::uint16_t>(MaxFragmentPayload);
	header.message_crc32 = message_crc32;
	const auto& payload = fragment_payload_storage();
	return DatagramView{header, ByteView{payload.data(), header.payload_size}};
}

DatagramView state_fragment(std::uint16_t fragment_index,
	std::uint16_t fragment_count = 3U,
	std::uint32_t message_id = 1U,
	std::uint32_t message_crc32 = 0x12345678U,
	std::uint64_t session_id = 42U,
	EndpointKey = endpoint())
{
	return fragment(MessageType::Manifest,
		fragment_index,
		fragment_count,
		message_id,
		message_crc32,
		session_id,
		FragmentedAckFlags);
}

DatagramView video_idr_fragment(std::uint16_t fragment_index,
	std::uint16_t fragment_count = 3U,
	std::uint32_t message_id = 1U,
	std::uint32_t message_crc32 = 0x87654321U,
	std::uint64_t session_id = 42U)
{
	return fragment(MessageType::TargetVideoFrame,
		fragment_index,
		fragment_count,
		message_id,
		message_crc32,
		session_id,
		static_cast<std::uint8_t>(FragmentedFlag | VideoIdrFlag));
}

DatagramView delta_fragment(std::uint16_t fragment_index,
	std::uint16_t fragment_count = 3U,
	std::uint32_t message_id = 1U,
	std::uint32_t message_crc32 = 0x10293847U,
	std::uint64_t session_id = 42U)
{
	return fragment(MessageType::Delta,
		fragment_index,
		fragment_count,
		message_id,
		message_crc32,
		session_id,
		FragmentedFlag);
}

DatagramView video_interframe_fragment(std::uint16_t fragment_index,
	std::uint16_t fragment_count = 3U,
	std::uint32_t message_id = 1U,
	std::uint32_t message_crc32 = 0x56473829U,
	std::uint64_t session_id = 42U)
{
	return fragment(MessageType::TargetVideoFrame,
		fragment_index,
		fragment_count,
		message_id,
		message_crc32,
		session_id,
		FragmentedFlag);
}

std::uint32_t zero_message_crc(std::uint16_t fragment_count, std::uint16_t final_fragment_size = 37U)
{
	const auto message_size = static_cast<std::size_t>(fragment_count - 1U) * MaxFragmentPayload + final_fragment_size;
	const std::vector<std::uint8_t> payload(message_size, 0U);
	return crc32_iso_hdlc(ByteView{payload.data(), payload.size()});
}

NackPayload sentinel_nack()
{
	static const std::array<std::uint8_t, 1> bitmap{{0x01U}};
	NackPayload nack;
	nack.target_message_id = 0xfeedbeefU;
	nack.target_message_type = MessageType::Delta;
	nack.reason = NackReason::StaleBaseline;
	nack.target_fragment_count = 77U;
	nack.target_message_crc32 = 0x0badcafeU;
	nack.needed_before_producer_time_us = 0x0102030405060708ULL;
	nack.missing_bitmap = ByteView{bitmap.data(), bitmap.size()};
	return nack;
}

void expect_sentinel_nack(const NackPayload& nack)
{
	EXPECT_EQ(0xfeedbeefU, nack.target_message_id);
	EXPECT_EQ(MessageType::Delta, nack.target_message_type);
	EXPECT_EQ(NackReason::StaleBaseline, nack.reason);
	EXPECT_EQ(77U, nack.target_fragment_count);
	EXPECT_EQ(0x0badcafeU, nack.target_message_crc32);
	EXPECT_EQ(0x0102030405060708ULL, nack.needed_before_producer_time_us);
	ASSERT_EQ(1U, nack.missing_bitmap.size);
	ASSERT_NE(nullptr, nack.missing_bitmap.data);
	EXPECT_EQ(0x01U, nack.missing_bitmap.data[0]);
}

SelectiveNackAction sentinel_action()
{
	SelectiveNackAction action;
	action.kind = SelectiveNackActionKind::RequestKeyframe;
	action.target.session_id = 0x8877665544332211ULL;
	action.target.endpoint = endpoint(9U, 9999U);
	action.target.message_type = MessageType::Delta;
	action.target.message_id = 0xdecafbadU;
	action.target.fragment_count = 91U;
	action.target.message_crc32 = 0x10203040U;
	action.nack = sentinel_nack();
	return action;
}

void expect_sentinel_action(const SelectiveNackAction& action)
{
	EXPECT_EQ(SelectiveNackActionKind::RequestKeyframe, action.kind);
	EXPECT_EQ(0x8877665544332211ULL, action.target.session_id);
	EXPECT_EQ(endpoint(9U, 9999U), action.target.endpoint);
	EXPECT_EQ(MessageType::Delta, action.target.message_type);
	EXPECT_EQ(0xdecafbadU, action.target.message_id);
	EXPECT_EQ(91U, action.target.fragment_count);
	EXPECT_EQ(0x10203040U, action.target.message_crc32);
	expect_sentinel_nack(action.nack);
}

TelemetrySessionContext client_context(std::uint64_t session_id = 42U, EndpointKey peer = endpoint())
{
	TelemetrySessionContext context;
	context.local_role = LocalEndpointRole::Client;
	context.peer_endpoint = peer;
	context.active_session_id = session_id;
	return context;
}

TEST(TelemetryProtocolReliableReceiveCache, ClassTypeMatrixAndRetentionDurationsAreExact)
{
	struct RetentionCase {
		ReliableMessageClass message_class;
		MessageType message_type;
		std::uint64_t retention_us;
	};
	const std::array<RetentionCase, 9> cases{{
		{ReliableMessageClass::ControlDrop,
			MessageType::TargetVideoSubscribe,
			ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs},
		{ReliableMessageClass::ControlRequestKeyframe,
			MessageType::TargetVideoKeyframeRequest,
			ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs},
		{ReliableMessageClass::ControlRequestResync,
			MessageType::ResyncRequest,
			ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs},
		{ReliableMessageClass::SessionCritical,
			MessageType::SessionBegin,
			ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs},
		{ReliableMessageClass::SessionClosing,
			MessageType::SessionEnd,
			ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs},
		{ReliableMessageClass::Transaction,
			MessageType::Manifest,
			ReliableTransactionRetentionUs + ReliableReceiveDeduplicationGraceUs},
		{ReliableMessageClass::ReliableEvent,
			MessageType::EventBatch,
			ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs},
		{ReliableMessageClass::HandshakeCritical,
			MessageType::Welcome,
			ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs},
		{ReliableMessageClass::HelloNegotiation, MessageType::Hello, 0U},
	}};

	for (const auto& test_case : cases) {
		SCOPED_TRACE(static_cast<unsigned>(test_case.message_class));
		EXPECT_EQ(test_case.retention_us, reliable_receive_retention_us(test_case.message_class));
		EXPECT_EQ(test_case.message_class != ReliableMessageClass::HelloNegotiation,
			reliable_receive_class_matches_type(test_case.message_class, test_case.message_type));
		EXPECT_FALSE(reliable_receive_class_matches_type(test_case.message_class, MessageType::Delta));
	}

	EXPECT_TRUE(reliable_receive_class_matches_type(ReliableMessageClass::ControlDrop, MessageType::TargetVideoConfig));
	EXPECT_TRUE(reliable_receive_class_matches_type(ReliableMessageClass::ControlDrop, MessageType::TargetVideoStop));
	EXPECT_TRUE(
		reliable_receive_class_matches_type(ReliableMessageClass::SessionCritical, MessageType::CapabilityUpdate));
	EXPECT_TRUE(reliable_receive_class_matches_type(ReliableMessageClass::Transaction, MessageType::FullSnapshot));
	EXPECT_EQ(0U, reliable_receive_retention_us(static_cast<ReliableMessageClass>(0xffU)));
	EXPECT_FALSE(
		reliable_receive_class_matches_type(static_cast<ReliableMessageClass>(0xffU), MessageType::SessionBegin));
	EXPECT_EQ(0U, reliable_receive_retention_us(ReliableMessageClass::VideoIdr));
	EXPECT_FALSE(reliable_receive_class_matches_type(ReliableMessageClass::VideoIdr, MessageType::TargetVideoFrame));
}

TEST(TelemetryProtocolReliableReceiveCache, ExactIdentityControlsDuplicateAndCrcContradictionResults)
{
	ReliableReceiveCache cache(8U);
	ReliableReceiveOutcome outcome{ReliableReceiveOutcomeKind::Applied, ValidationError::None};
	const auto key = receive_key();
	EXPECT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(key, ReliableMessageClass::SessionCritical, 100U, outcome));
	// Non-duplicate reserve results leave the caller's output untouched.
	EXPECT_EQ(ReliableReceiveOutcomeKind::Applied, outcome.kind);
	EXPECT_EQ(1U, cache.entry_count());

	outcome = {ReliableReceiveOutcomeKind::Error, ValidationError::BadMessageCrc};
	EXPECT_EQ(ReliableReceiveReserveResult::Duplicate,
		cache.reserve(key, ReliableMessageClass::SessionCritical, 101U, outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Pending, outcome.kind);
	EXPECT_EQ(ValidationError::None, outcome.error);

	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, cache.record_validated(key));
	outcome = {};
	ASSERT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(key, 102U, outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Validated, outcome.kind);
	EXPECT_EQ(ValidationError::None, outcome.error);

	auto conflicting_crc = key;
	conflicting_crc.message_crc32++;
	outcome = {ReliableReceiveOutcomeKind::Applied, ValidationError::None};
	EXPECT_EQ(ReliableReceiveReserveResult::IdentityConflict,
		cache.reserve(conflicting_crc, ReliableMessageClass::SessionCritical, 103U, outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Applied, outcome.kind);
	EXPECT_EQ(ReliableReceiveLookupResult::IdentityConflict, cache.lookup(conflicting_crc, 104U, outcome));
	EXPECT_EQ(ReliableReceiveUpdateResult::IdentityConflict, cache.record_applied(conflicting_crc));
	EXPECT_EQ(1U, cache.entry_count());

	EXPECT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(receive_key(1U, MessageType::CapabilityUpdate),
			ReliableMessageClass::SessionCritical,
			105U,
			outcome));
	EXPECT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(receive_key(2U), ReliableMessageClass::SessionCritical, 105U, outcome));
	EXPECT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(receive_key(1U, MessageType::SessionBegin, 99U),
			ReliableMessageClass::SessionCritical,
			105U,
			outcome));
	EXPECT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(receive_key(1U, MessageType::SessionBegin, key.session_id, endpoint(2U)),
			ReliableMessageClass::SessionCritical,
			105U,
			outcome));
	EXPECT_EQ(5U, cache.entry_count());
}

TEST(TelemetryProtocolReliableReceiveCache, OutcomeTransitionsNeverRegressOrReopenTerminalResults)
{
	ReliableReceiveCache cache(8U);
	ReliableReceiveOutcome outcome;
	const auto applied_key = receive_key(1U);
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(applied_key, ReliableMessageClass::SessionCritical, 0U, outcome));
	EXPECT_EQ(ReliableReceiveUpdateResult::Updated, cache.record_validated(applied_key));
	EXPECT_EQ(ReliableReceiveUpdateResult::Unchanged, cache.record_validated(applied_key));
	EXPECT_EQ(ReliableReceiveUpdateResult::Updated, cache.record_applied(applied_key));
	EXPECT_EQ(ReliableReceiveUpdateResult::Unchanged, cache.record_validated(applied_key));
	EXPECT_EQ(ReliableReceiveUpdateResult::Unchanged,
		cache.record_error(applied_key, ValidationError::VisibilityViolation));
	EXPECT_EQ(ReliableReceiveUpdateResult::Unchanged, cache.record_applied(applied_key));
	outcome = {};
	ASSERT_EQ(ReliableReceiveReserveResult::Duplicate,
		cache.reserve(applied_key, ReliableMessageClass::SessionCritical, 1U, outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Applied, outcome.kind);
	EXPECT_EQ(ValidationError::None, outcome.error);

	const auto error_key = receive_key(2U);
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(error_key, ReliableMessageClass::SessionCritical, 0U, outcome));
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, cache.record_validated(error_key));
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated,
		cache.record_error(error_key, ValidationError::InvalidStateTransition));
	EXPECT_EQ(ReliableReceiveUpdateResult::Unchanged, cache.record_applied(error_key));
	EXPECT_EQ(ReliableReceiveUpdateResult::Unchanged, cache.record_validated(error_key));
	EXPECT_EQ(ReliableReceiveUpdateResult::Unchanged, cache.record_error(error_key, ValidationError::BadMessageCrc));
	outcome = {};
	ASSERT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(error_key, 1U, outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Error, outcome.kind);
	EXPECT_EQ(ValidationError::InvalidStateTransition, outcome.error);

	const auto parse_error_key = receive_key(3U);
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(parse_error_key, ReliableMessageClass::SessionCritical, 0U, outcome));
	EXPECT_EQ(ReliableReceiveUpdateResult::Updated,
		cache.record_error(parse_error_key, ValidationError::TruncatedPayload));
	EXPECT_EQ(ReliableReceiveUpdateResult::InvalidOutcome, cache.record_error(parse_error_key, ValidationError::None));
	EXPECT_EQ(ReliableReceiveUpdateResult::NotFound, cache.record_applied(receive_key(999U)));
	auto conflict = parse_error_key;
	conflict.message_crc32++;
	EXPECT_EQ(ReliableReceiveUpdateResult::IdentityConflict, cache.record_validated(conflict));
	ReliableReceiveKey invalid;
	EXPECT_EQ(ReliableReceiveUpdateResult::InvalidOutcome, cache.record_validated(invalid));
}

TEST(TelemetryProtocolReliableReceiveCache, CachedAckReplaysExactStoredFragmentTuple)
{
	ReliableReceiveCache cache(2U);
	ReliableReceiveOutcome outcome;
	const auto key = receive_key(77U, MessageType::Manifest, 42U, endpoint(), 0xaabbccddU, 3U);
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(key, ReliableMessageClass::Transaction, 100U, outcome));
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, cache.record_validated(key));

	AckPayload ack;
	ack.target_message_id = 0xfeedbeefU;
	ack.target_message_type = MessageType::Delta;
	ack.ack_flags = KnownAckFlags;
	ack.target_fragment_count = 99U;
	ack.target_message_crc32 = 0x01020304U;
	ASSERT_EQ(ValidationError::None, cache.cached_ack(key, 101U, ack));
	EXPECT_EQ(key.message_id, ack.target_message_id);
	EXPECT_EQ(key.message_type, ack.target_message_type);
	EXPECT_EQ(static_cast<std::uint8_t>(AckFlag::Validated), ack.ack_flags);
	EXPECT_EQ(key.fragment_count, ack.target_fragment_count);
	EXPECT_EQ(key.message_crc32, ack.target_message_crc32);

	auto conflicting_count = key;
	++conflicting_count.fragment_count;
	ack.target_message_id = 0xfeedbeefU;
	ack.target_message_type = MessageType::Delta;
	ack.ack_flags = KnownAckFlags;
	ack.target_fragment_count = 99U;
	ack.target_message_crc32 = 0x01020304U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, cache.cached_ack(conflicting_count, 102U, ack));
	EXPECT_EQ(0xfeedbeefU, ack.target_message_id);
	EXPECT_EQ(MessageType::Delta, ack.target_message_type);
	EXPECT_EQ(KnownAckFlags, ack.ack_flags);
	EXPECT_EQ(99U, ack.target_fragment_count);
	EXPECT_EQ(0x01020304U, ack.target_message_crc32);

	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, cache.record_applied(key));
	ASSERT_EQ(ValidationError::None, cache.cached_ack(key, 103U, ack));
	EXPECT_EQ(KnownAckFlags, ack.ack_flags);
	EXPECT_EQ(3U, ack.target_fragment_count);
}

TEST(TelemetryProtocolReliableReceiveCache, InvalidKeysAndClassMismatchesNeverMutateCacheOrOutputs)
{
	ReliableReceiveCache cache(4U);
	ReliableReceiveOutcome outcome{ReliableReceiveOutcomeKind::Applied, ValidationError::None};
	const auto original = outcome;
	auto key = receive_key();

	key.session_id = 0U;
	EXPECT_EQ(ReliableReceiveReserveResult::InvalidKey,
		cache.reserve(key, ReliableMessageClass::SessionCritical, 0U, outcome));
	key = receive_key();
	key.endpoint = EndpointKey{};
	EXPECT_EQ(ReliableReceiveReserveResult::InvalidKey,
		cache.reserve(key, ReliableMessageClass::SessionCritical, 0U, outcome));
	key = receive_key();
	key.message_type = MessageType::Invalid;
	EXPECT_EQ(ReliableReceiveReserveResult::InvalidKey,
		cache.reserve(key, ReliableMessageClass::SessionCritical, 0U, outcome));
	key = receive_key();
	key.message_id = 0U;
	EXPECT_EQ(ReliableReceiveReserveResult::InvalidKey,
		cache.reserve(key, ReliableMessageClass::SessionCritical, 0U, outcome));
	key = receive_key();
	EXPECT_EQ(ReliableReceiveReserveResult::InvalidKey,
		cache.reserve(key, ReliableMessageClass::Transaction, 0U, outcome));
	EXPECT_EQ(ReliableReceiveReserveResult::InvalidKey,
		cache.reserve(receive_key(1U, MessageType::Hello), ReliableMessageClass::HelloNegotiation, 0U, outcome));
	EXPECT_EQ(ReliableReceiveReserveResult::InvalidKey,
		cache.reserve(receive_key(2U, MessageType::TargetVideoFrame, 42U, endpoint(), 0x12345678U, 3U),
			ReliableMessageClass::VideoIdr,
			0U,
			outcome));
	EXPECT_EQ(original.kind, outcome.kind);
	EXPECT_EQ(original.error, outcome.error);
	EXPECT_EQ(0U, cache.entry_count());

	ReliableReceiveKey invalid;
	EXPECT_EQ(ReliableReceiveLookupResult::InvalidKey, cache.lookup(invalid, 0U, outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::NotFound, cache.lookup(receive_key(), 0U, outcome));
	EXPECT_EQ(0U, cache.entry_count());
}

TEST(TelemetryProtocolReliableReceiveCache, Full4096QuotaNeverEvictsLiveEntries)
{
	ReliableReceiveCache cache;
	EXPECT_EQ(ReliableReceiveMaximumEntries, cache.maximum_entries());
	ReliableReceiveOutcome outcome;
	constexpr std::uint64_t start_us = 123U;
	for (std::uint32_t index = 1U; index <= ReliableReceiveMaximumEntries; ++index) {
		SCOPED_TRACE(index);
		ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
			cache.reserve(receive_key(index), ReliableMessageClass::SessionCritical, start_us, outcome));
	}
	EXPECT_EQ(ReliableReceiveMaximumEntries, cache.entry_count());
	EXPECT_EQ(ReliableReceiveReserveResult::ResourceLimit,
		cache.reserve(receive_key(static_cast<std::uint32_t>(ReliableReceiveMaximumEntries + 1U)),
			ReliableMessageClass::SessionCritical,
			start_us,
			outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(receive_key(1U), start_us, outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::Found,
		cache.lookup(receive_key(static_cast<std::uint32_t>(ReliableReceiveMaximumEntries)), start_us, outcome));

	auto conflict = receive_key(1U);
	conflict.message_crc32++;
	EXPECT_EQ(ReliableReceiveReserveResult::IdentityConflict,
		cache.reserve(conflict, ReliableMessageClass::SessionCritical, start_us, outcome));
	EXPECT_EQ(ReliableReceiveMaximumEntries, cache.entry_count());

	const auto retention = ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs;
	EXPECT_EQ(0U, cache.expire(start_us + retention - 1U));
	EXPECT_EQ(ReliableReceiveMaximumEntries, cache.entry_count());
	EXPECT_EQ(ReliableReceiveMaximumEntries, cache.expire(start_us + retention));
	EXPECT_EQ(0U, cache.entry_count());
	EXPECT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(receive_key(5000U), ReliableMessageClass::SessionCritical, start_us + retention, outcome));

	ReliableReceiveCache capped(ReliableReceiveMaximumEntries + 100U);
	EXPECT_EQ(ReliableReceiveMaximumEntries, capped.maximum_entries());
	ReliableReceiveCache zero(0U);
	EXPECT_EQ(0U, zero.maximum_entries());
	EXPECT_EQ(ReliableReceiveReserveResult::ResourceLimit,
		zero.reserve(receive_key(), ReliableMessageClass::SessionCritical, 0U, outcome));
}

TEST(TelemetryProtocolReliableReceiveCache, EveryRetentionExpiresAtExactBoundaryAndClockRegressionCannotExtendIt)
{
	struct ExpirationCase {
		ReliableMessageClass message_class;
		MessageType message_type;
	};
	const std::array<ExpirationCase, 8> cases{{
		{ReliableMessageClass::ControlDrop, MessageType::TargetVideoSubscribe},
		{ReliableMessageClass::ControlRequestKeyframe, MessageType::TargetVideoKeyframeRequest},
		{ReliableMessageClass::ControlRequestResync, MessageType::ResyncRequest},
		{ReliableMessageClass::SessionCritical, MessageType::SessionBegin},
		{ReliableMessageClass::SessionClosing, MessageType::SessionEnd},
		{ReliableMessageClass::Transaction, MessageType::Manifest},
		{ReliableMessageClass::ReliableEvent, MessageType::EventBatch},
		{ReliableMessageClass::HandshakeCritical, MessageType::Welcome},
	}};

	for (std::size_t index = 0; index < cases.size(); ++index) {
		SCOPED_TRACE(index);
		ReliableReceiveCache cache(1U);
		ReliableReceiveOutcome outcome;
		const auto key = receive_key(1U, cases[index].message_type);
		constexpr std::uint64_t start_us = 1000U;
		ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
			cache.reserve(key, cases[index].message_class, start_us, outcome));
		const auto retention = reliable_receive_retention_us(cases[index].message_class);
		EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(key, start_us + retention - 1U, outcome));
		EXPECT_EQ(start_us + retention - 1U, cache.monotonic_time_us());
		EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(key, 0U, outcome));
		EXPECT_EQ(start_us + retention - 1U, cache.monotonic_time_us());
		EXPECT_EQ(ReliableReceiveLookupResult::NotFound, cache.lookup(key, start_us + retention, outcome));
		EXPECT_EQ(0U, cache.entry_count());
		EXPECT_EQ(0U, cache.expire(0U));
		EXPECT_EQ(start_us + retention, cache.monotonic_time_us());
	}

	ReliableReceiveCache saturated(1U);
	ReliableReceiveOutcome outcome;
	const auto key = receive_key();
	const auto almost_max = std::numeric_limits<std::uint64_t>::max() - 1U;
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		saturated.reserve(key, ReliableMessageClass::SessionCritical, almost_max, outcome));
	EXPECT_EQ(0U, saturated.expire(almost_max));
	EXPECT_EQ(1U, saturated.entry_count());
	EXPECT_EQ(1U, saturated.expire(std::numeric_limits<std::uint64_t>::max()));
	EXPECT_EQ(0U, saturated.entry_count());
	EXPECT_EQ(0U, saturated.expire(0U));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), saturated.monotonic_time_us());
}

TEST(TelemetryProtocolReliableReceiveCache, PurgeScopesAreExactAndClearRemovesEverything)
{
	ReliableReceiveCache cache(16U);
	ReliableReceiveOutcome outcome;
	const auto peer_a = endpoint(1U);
	const auto peer_b = endpoint(2U);
	const auto a1 = receive_key(1U, MessageType::SessionBegin, 10U, peer_a);
	const auto a2 = receive_key(2U, MessageType::SessionBegin, 10U, peer_a);
	const auto b1 = receive_key(3U, MessageType::SessionBegin, 10U, peer_b);
	const auto other = receive_key(4U, MessageType::SessionBegin, 11U, peer_a);
	for (const auto& key : {a1, a2, b1, other}) {
		ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
			cache.reserve(key, ReliableMessageClass::SessionCritical, 0U, outcome));
	}
	EXPECT_EQ(2U, cache.purge_session(10U, peer_a));
	EXPECT_EQ(ReliableReceiveLookupResult::NotFound, cache.lookup(a1, 0U, outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(b1, 0U, outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(other, 0U, outcome));
	EXPECT_EQ(1U, cache.purge_session(10U));
	EXPECT_EQ(1U, cache.entry_count());
	cache.clear();
	EXPECT_EQ(0U, cache.entry_count());
	EXPECT_EQ(ReliableReceiveLookupResult::NotFound, cache.lookup(other, 0U, outcome));
}

TEST(TelemetryProtocolReliableReceiveCache, AppliedSessionEndTombstoneSurvivesTeardownForExactlySevenSeconds)
{
	ReliableReceiveCache cache(12U);
	ReliableReceiveOutcome outcome;
	const auto peer_a = endpoint(1U);
	const auto peer_b = endpoint(2U);
	const auto end = receive_key(1U, MessageType::SessionEnd, 42U, peer_a);
	const auto pending_end = receive_key(2U, MessageType::SessionEnd, 42U, peer_a);
	const auto ordinary = receive_key(3U, MessageType::SessionBegin, 42U, peer_a);
	const auto other_endpoint = receive_key(4U, MessageType::SessionBegin, 42U, peer_b);
	const auto other_session = receive_key(5U, MessageType::Manifest, 43U, peer_a);
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(end, ReliableMessageClass::SessionClosing, 0U, outcome));
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, cache.record_validated(end));
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, cache.record_applied(end));
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(pending_end, ReliableMessageClass::SessionClosing, 0U, outcome));
	for (const auto& key : {ordinary, other_endpoint}) {
		ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
			cache.reserve(key, ReliableMessageClass::SessionCritical, 0U, outcome));
	}
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		cache.reserve(other_session, ReliableMessageClass::Transaction, 0U, outcome));

	EXPECT_EQ(2U, cache.purge_session_preserving_end_tombstone(42U, peer_a));
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(end, 0U, outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Applied, outcome.kind);
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(other_endpoint, 0U, outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(other_session, 0U, outcome));
	EXPECT_EQ(1U, cache.purge_session_preserving_end_tombstone(42U));
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(end, 0U, outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::NotFound, cache.lookup(other_endpoint, 0U, outcome));

	constexpr std::uint64_t tombstone_retention = ReliableOrdinaryRetentionUs + ReliableReceiveDeduplicationGraceUs;
	EXPECT_EQ(7'000'000U, tombstone_retention);
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(end, tombstone_retention - 1U, outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::NotFound, cache.lookup(end, tombstone_retention, outcome));
	EXPECT_EQ(ReliableReceiveLookupResult::Found, cache.lookup(other_session, tombstone_retention, outcome));
	EXPECT_EQ(1U, cache.purge_session(43U));
	EXPECT_EQ(0U, cache.entry_count());

	ReliableReceiveCache administrative(2U);
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		administrative.reserve(end, ReliableMessageClass::SessionClosing, 0U, outcome));
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, administrative.record_applied(end));
	EXPECT_EQ(1U, administrative.purge_session(42U));
	EXPECT_EQ(0U, administrative.entry_count());
}

TEST(TelemetryProtocolSelectiveNack, EligibilityCoversReliableStateAndVideoIdrOnly)
{
	const auto manifest = state_fragment(0U, 2U);
	EXPECT_TRUE(is_selective_nack_candidate(manifest.header));

	const auto full_snapshot = fragment(MessageType::FullSnapshot,
		0U,
		2U,
		2U,
		3U,
		42U,
		static_cast<std::uint8_t>(FragmentedAckFlags | KeyframeFlag));
	EXPECT_TRUE(is_selective_nack_candidate(full_snapshot.header));
	const auto reliable_event = fragment(MessageType::EventBatch, 0U, 2U, 3U, 4U, 42U, FragmentedAckFlags);
	EXPECT_TRUE(is_selective_nack_candidate(reliable_event.header));
	const auto idr = video_idr_fragment(0U, 2U);
	EXPECT_TRUE(is_selective_nack_candidate(idr.header));

	auto unfragmented = fragment(MessageType::Manifest, 0U, 1U, 4U, 5U, 42U, AckRequiredFlag);
	EXPECT_FALSE(is_selective_nack_candidate(unfragmented.header));
	auto delta = fragment(MessageType::Delta, 0U, 2U, 5U, 6U, 42U, FragmentedFlag);
	EXPECT_FALSE(is_selective_nack_candidate(delta.header));
	auto replaceable_event = fragment(MessageType::EventBatch, 0U, 2U, 6U, 7U, 42U, FragmentedFlag);
	EXPECT_FALSE(is_selective_nack_candidate(replaceable_event.header));
	auto non_idr = fragment(MessageType::TargetVideoFrame, 0U, 2U, 7U, 8U, 42U, FragmentedFlag);
	EXPECT_FALSE(is_selective_nack_candidate(non_idr.header));
	auto malformed = manifest;
	malformed.header.fragment_offset = 1U;
	EXPECT_FALSE(is_selective_nack_candidate(malformed.header));
}

TEST(TelemetryProtocolSelectiveNack, HoleDelayCurrentBitmapTailAndFiftyMillisecondCadenceAreExact)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	constexpr std::uint64_t base_rto_us = ReliableMinimumRtoUs;
	static_assert(ReliableMinimumRtoUs / 4U == SelectiveNackMaximumDelayUs,
		"minimum RTO quarter must exercise the normative 25 ms boundary");
	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(0U, 10U, 101U), peer, 0U, base_rto_us));
	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(9U, 10U, 101U), peer, 1000U, base_rto_us));
	EXPECT_EQ(1U, tracker.active_entries(MessageSizeClass::State));

	auto action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(25'999U, 0U, action));
	expect_sentinel_action(action);
	ASSERT_EQ(SelectiveNackPollResult::Ready, tracker.poll(26'000U, 0U, action));
	ASSERT_EQ(SelectiveNackActionKind::SendNack, action.kind);
	const auto& nack = action.nack;
	EXPECT_EQ(101U, nack.target_message_id);
	EXPECT_EQ(MessageType::Manifest, nack.target_message_type);
	EXPECT_EQ(NackReason::MissingFragments, nack.reason);
	EXPECT_EQ(10U, nack.target_fragment_count);
	EXPECT_EQ(0x12345678U, nack.target_message_crc32);
	EXPECT_EQ(0U, nack.needed_before_producer_time_us);
	ASSERT_EQ(2U, nack.missing_bitmap.size);
	EXPECT_EQ(0xfeU, nack.missing_bitmap.data[0]);
	EXPECT_EQ(0x01U, nack.missing_bitmap.data[1]);
	// The six unused tail bits are canonical zeroes.
	EXPECT_EQ(0U, static_cast<std::uint8_t>(nack.missing_bitmap.data[1] & 0xfcU));

	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(1U, 10U, 101U), peer, 26'001U, base_rto_us));
	action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(75'999U, 0U, action));
	expect_sentinel_action(action);
	ASSERT_EQ(SelectiveNackPollResult::Ready, tracker.poll(76'000U, 0U, action));
	ASSERT_EQ(SelectiveNackActionKind::SendNack, action.kind);
	ASSERT_EQ(2U, action.nack.missing_bitmap.size);
	EXPECT_EQ(0xfcU, action.nack.missing_bitmap.data[0]);
	EXPECT_EQ(0x01U, action.nack.missing_bitmap.data[1]);

	for (std::uint16_t index = 2U; index < 8U; ++index) {
		EXPECT_EQ(SelectiveNackObserveResult::Accepted,
			tracker.observe_validated_fragment(state_fragment(index, 10U, 101U), peer, 76'001U + index, base_rto_us));
	}
	EXPECT_EQ(SelectiveNackObserveResult::Completed,
		tracker.observe_validated_fragment(state_fragment(8U, 10U, 101U), peer, 76'010U, base_rto_us));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));
	action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(200'000U, 0U, action));
	expect_sentinel_action(action);
}

TEST(TelemetryProtocolSelectiveNack, DuplicateRetransmissionAndCompletionDoNotReopenMessage)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(2U, 3U, 55U), peer, 100U, ReliableDefaultRtoUs));
	auto retransmission = state_fragment(2U, 3U, 55U);
	retransmission.header.flags = static_cast<std::uint8_t>(retransmission.header.flags | RetransmissionFlag);
	EXPECT_EQ(SelectiveNackObserveResult::Duplicate,
		tracker.observe_validated_fragment(retransmission, peer, 101U, ReliableDefaultRtoUs));
	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(0U, 3U, 55U), peer, 102U, ReliableDefaultRtoUs));
	EXPECT_EQ(SelectiveNackObserveResult::Duplicate,
		tracker.observe_validated_fragment(state_fragment(0U, 3U, 55U), peer, 103U, ReliableDefaultRtoUs));
	EXPECT_EQ(SelectiveNackObserveResult::Completed,
		tracker.observe_validated_fragment(state_fragment(1U, 3U, 55U), peer, 104U, ReliableDefaultRtoUs));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));
	// Completion discards tracker state; a late fragment starts a fresh bounded candidate.
	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(1U, 3U, 55U), peer, 105U, ReliableDefaultRtoUs));
	EXPECT_EQ(1U, tracker.active_entries(MessageSizeClass::State));
}

TEST(TelemetryProtocolSelectiveNack, FourStateSlotsAndLatestVideoIdrSlotAreIndependent)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	for (std::uint32_t id = 1U; id <= MaxStateReassembliesPerClient; ++id) {
		ASSERT_EQ(SelectiveNackObserveResult::Accepted,
			tracker.observe_validated_fragment(state_fragment(0U, 2U, id), peer, 0U, ReliableDefaultRtoUs));
	}
	EXPECT_EQ(MaxStateReassembliesPerClient, tracker.active_entries(MessageSizeClass::State));
	EXPECT_EQ(SelectiveNackObserveResult::ResourceLimit,
		tracker.observe_validated_fragment(state_fragment(0U, 2U, 99U), peer, 0U, ReliableDefaultRtoUs));
	EXPECT_EQ(MaxStateReassembliesPerClient, tracker.active_entries(MessageSizeClass::State));

	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(video_idr_fragment(0U, 2U, 101U), peer, 0U, ReliableDefaultRtoUs, 9000U));
	EXPECT_EQ(SelectiveNackMaximumVideoEntries, tracker.active_entries(MessageSizeClass::Video));
	// A newer IDR replaces recovery of the previous one without consuming state quota.
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(video_idr_fragment(0U, 2U, 102U), peer, 0U, ReliableDefaultRtoUs, 9000U));
	EXPECT_EQ(SelectiveNackMaximumVideoEntries, tracker.active_entries(MessageSizeClass::Video));
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible,
		tracker.observe_validated_fragment(video_idr_fragment(0U, 2U, 100U), peer, 0U, ReliableDefaultRtoUs, 9000U));
	EXPECT_EQ(SelectiveNackMaximumEntries,
		tracker.active_entries(MessageSizeClass::State) + tracker.active_entries(MessageSizeClass::Video));

	EXPECT_TRUE(tracker.discard(42U, peer, MessageType::Manifest, 2U));
	EXPECT_FALSE(tracker.discard(42U, peer, MessageType::Manifest, 2U));
	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(0U, 2U, 99U), peer, 1U, ReliableDefaultRtoUs));
	EXPECT_EQ(MaxStateReassembliesPerClient, tracker.active_entries(MessageSizeClass::State));
	tracker.clear();
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::Video));
}

TEST(TelemetryProtocolSelectiveNack, ContradictoryIdentityIsRejectedAndPurgedInsteadOfMerged)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	const auto original = state_fragment(0U, 2U, 12U, 0x11111111U);
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(original, peer, 0U, ReliableDefaultRtoUs));
	auto changed_crc = state_fragment(1U, 2U, 12U, 0x22222222U);
	EXPECT_EQ(SelectiveNackObserveResult::IdentityConflict,
		tracker.observe_validated_fragment(changed_crc, peer, 1U, ReliableDefaultRtoUs));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));

	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(original, peer, 2U, ReliableDefaultRtoUs));
	auto changed_count = state_fragment(1U, 3U, 12U, 0x11111111U);
	EXPECT_EQ(SelectiveNackObserveResult::IdentityConflict,
		tracker.observe_validated_fragment(changed_count, peer, 3U, ReliableDefaultRtoUs));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));

	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(original, peer, 4U, ReliableDefaultRtoUs));
	auto changed_size = fragment(MessageType::Manifest, 1U, 2U, 12U, 0x11111111U, 42U, FragmentedAckFlags, 38U);
	EXPECT_EQ(SelectiveNackObserveResult::IdentityConflict,
		tracker.observe_validated_fragment(changed_size, peer, 5U, ReliableDefaultRtoUs));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));

	const auto video = video_idr_fragment(0U, 2U, 77U);
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(video, peer, 6U, ReliableDefaultRtoUs, 1000U));
	EXPECT_EQ(SelectiveNackObserveResult::IdentityConflict,
		tracker.observe_validated_fragment(video_idr_fragment(1U, 2U, 77U), peer, 7U, ReliableDefaultRtoUs, 1001U));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::Video));

	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(original, peer, 8U, ReliableDefaultRtoUs));
	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(original, endpoint(2U), 8U, ReliableDefaultRtoUs));
	auto another_session = original;
	another_session.header.session_id = 43U;
	EXPECT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(another_session, peer, 8U, ReliableDefaultRtoUs));
	EXPECT_EQ(3U, tracker.active_entries(MessageSizeClass::State));
}

TEST(TelemetryProtocolSelectiveNack, InvalidAndIneligibleFragmentsNeverConsumeQuotaOrAdvanceTime)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	const auto valid = state_fragment(0U, 2U);
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(valid, EndpointKey{}, 100U, ReliableDefaultRtoUs));
	auto no_session = valid;
	no_session.header.session_id = 0U;
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(no_session, peer, 100U, ReliableDefaultRtoUs));
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(valid, peer, 100U, ReliableMinimumRtoUs - 1U));
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(valid, peer, 100U, ReliableMaximumRtoUs + 1U));
	auto wrong_size = valid;
	wrong_size.payload.size--;
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(wrong_size, peer, 100U, ReliableDefaultRtoUs));
	auto null_payload = valid;
	null_payload.payload.data = nullptr;
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(null_payload, peer, 100U, ReliableDefaultRtoUs));
	auto bad_offset = valid;
	bad_offset.header.fragment_offset++;
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(bad_offset, peer, 100U, ReliableDefaultRtoUs));
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(valid, peer, 100U, ReliableDefaultRtoUs, 1U));
	EXPECT_EQ(0U, tracker.monotonic_time_us());
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));

	const auto unfragmented = fragment(MessageType::Manifest, 0U, 1U, 2U, 3U, 42U, AckRequiredFlag);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible,
		tracker.observe_validated_fragment(unfragmented, peer, 101U, ReliableDefaultRtoUs));
	const auto delta = fragment(MessageType::Delta, 0U, 2U, 3U, 4U, 42U, FragmentedFlag);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible,
		tracker.observe_validated_fragment(delta, peer, 102U, ReliableDefaultRtoUs));
	const auto replaceable_event = fragment(MessageType::EventBatch, 0U, 2U, 4U, 5U, 42U, FragmentedFlag);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible,
		tracker.observe_validated_fragment(replaceable_event, peer, 103U, ReliableDefaultRtoUs));
	const auto non_idr = fragment(MessageType::TargetVideoFrame, 0U, 2U, 5U, 6U, 42U, FragmentedFlag);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible,
		tracker.observe_validated_fragment(non_idr, peer, 104U, ReliableDefaultRtoUs));
	const auto video_with_ack = fragment(MessageType::TargetVideoFrame,
		0U,
		2U,
		6U,
		7U,
		42U,
		static_cast<std::uint8_t>(FragmentedFlag | VideoIdrFlag | AckRequiredFlag));
	EXPECT_EQ(SelectiveNackObserveResult::InvalidFragment,
		tracker.observe_validated_fragment(video_with_ack, peer, 105U, ReliableDefaultRtoUs));
	// Eligibility rejection occurs before peer time is accepted.
	EXPECT_EQ(0U, tracker.monotonic_time_us());
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::Video));
}

TEST(TelemetryProtocolSelectiveNack, ClockRegressionIsClampedAndCannotAccelerateCadence)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(2U, 3U, 88U), peer, 100'000U, ReliableMaximumRtoUs));
	EXPECT_EQ(100'000U, tracker.monotonic_time_us());
	auto action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(1U, 0U, action));
	expect_sentinel_action(action);
	EXPECT_EQ(100'000U, tracker.monotonic_time_us());
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(124'999U, 0U, action));
	EXPECT_EQ(SelectiveNackPollResult::Ready, tracker.poll(125'000U, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::SendNack, action.kind);
	action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(0U, 0U, action));
	expect_sentinel_action(action);
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(174'999U, 0U, action));
	EXPECT_EQ(SelectiveNackPollResult::Ready, tracker.poll(175'000U, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::SendNack, action.kind);
	EXPECT_EQ(175'000U, tracker.monotonic_time_us());
}

TEST(TelemetryProtocolSelectiveNack, PollReturnsOnlyEarliestOneAndPreservesOutputWhenNone)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(2U, 3U, 10U), peer, 0U, ReliableDefaultRtoUs));
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(state_fragment(2U, 3U, 20U), peer, 1000U, ReliableDefaultRtoUs));
	auto action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(24'999U, 0U, action));
	expect_sentinel_action(action);
	ASSERT_EQ(SelectiveNackPollResult::Ready, tracker.poll(25'000U, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::SendNack, action.kind);
	EXPECT_EQ(10U, action.nack.target_message_id);
	ASSERT_EQ(SelectiveNackPollResult::Ready, tracker.poll(26'000U, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::SendNack, action.kind);
	EXPECT_EQ(20U, action.nack.target_message_id);
	action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(26'000U, 0U, action));
	expect_sentinel_action(action);
}

TEST(TelemetryProtocolSelectiveNack, VideoIdrCarriesDeadlineAndCurrentMissingFragments)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	constexpr std::uint64_t deadline = 9'876'543U;
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker
			.observe_validated_fragment(video_idr_fragment(0U, 4U, 300U), peer, 100U, ReliableDefaultRtoUs, deadline));
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker
			.observe_validated_fragment(video_idr_fragment(3U, 4U, 300U), peer, 200U, ReliableDefaultRtoUs, deadline));
	auto action = sentinel_action();
	ASSERT_EQ(SelectiveNackPollResult::Ready, tracker.poll(25'200U, 0U, action));
	ASSERT_EQ(SelectiveNackActionKind::SendNack, action.kind);
	const auto& nack = action.nack;
	EXPECT_EQ(300U, nack.target_message_id);
	EXPECT_EQ(MessageType::TargetVideoFrame, nack.target_message_type);
	EXPECT_EQ(deadline, nack.needed_before_producer_time_us);
	ASSERT_EQ(1U, nack.missing_bitmap.size);
	EXPECT_EQ(0x06U, nack.missing_bitmap.data[0]);
	EXPECT_EQ(0U, static_cast<std::uint8_t>(nack.missing_bitmap.data[0] & 0xf0U));
}

TEST(TelemetryProtocolSelectiveNack, ReliableReassemblyExpiresAtTwoSecondsAndRequestsResync)
{
	SelectiveNackTracker tracker;
	const auto peer = endpoint();
	constexpr std::uint64_t start_us = 123U;
	const auto first = state_fragment(0U, 3U, 700U, 0x10203040U);
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(first, peer, start_us, ReliableDefaultRtoUs));
	ASSERT_EQ(1U, tracker.active_entries(MessageSizeClass::State));

	auto action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None,
		tracker.poll(start_us + SelectiveNackReliableRetentionUs - 1U, 0U, action));
	expect_sentinel_action(action);
	ASSERT_EQ(SelectiveNackPollResult::Ready, tracker.poll(start_us + SelectiveNackReliableRetentionUs, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestResync, action.kind);
	EXPECT_EQ(first.header.session_id, action.target.session_id);
	EXPECT_EQ(peer, action.target.endpoint);
	EXPECT_EQ(first.header.message_type, action.target.message_type);
	EXPECT_EQ(first.header.message_id, action.target.message_id);
	EXPECT_EQ(first.header.fragment_count, action.target.fragment_count);
	EXPECT_EQ(first.header.message_crc32, action.target.message_crc32);
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));
}

TEST(TelemetryProtocolSelectiveNack, VideoIdrExpiryRequestsKeyframeAtLocalOrProducerDeadline)
{
	const auto peer = endpoint();
	constexpr std::uint64_t start_us = 200U;
	{
		SelectiveNackTracker tracker;
		const auto first = video_idr_fragment(0U, 3U, 800U, 0x50607080U);
		ASSERT_EQ(SelectiveNackObserveResult::Accepted,
			tracker.observe_validated_fragment(first, peer, start_us, ReliableDefaultRtoUs));
		auto action = sentinel_action();
		EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(start_us + ReliableVideoIdrRetentionUs - 1U, 0U, action));
		expect_sentinel_action(action);
		ASSERT_EQ(SelectiveNackPollResult::Ready, tracker.poll(start_us + ReliableVideoIdrRetentionUs, 0U, action));
		EXPECT_EQ(SelectiveNackActionKind::RequestKeyframe, action.kind);
		EXPECT_EQ(first.header.message_id, action.target.message_id);
		EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::Video));
	}

	{
		SelectiveNackTracker tracker;
		constexpr std::uint64_t producer_deadline_us = 9'000U;
		const auto first = video_idr_fragment(0U, 3U, 801U, 0x90a0b0c0U);
		ASSERT_EQ(SelectiveNackObserveResult::Accepted,
			tracker.observe_validated_fragment(first, peer, start_us, ReliableDefaultRtoUs, producer_deadline_us));
		auto action = sentinel_action();
		EXPECT_EQ(SelectiveNackPollResult::None, tracker.poll(start_us + 1U, producer_deadline_us - 1U, action));
		expect_sentinel_action(action);
		ASSERT_EQ(SelectiveNackPollResult::Ready, tracker.poll(start_us + 2U, producer_deadline_us, action));
		EXPECT_EQ(SelectiveNackActionKind::RequestKeyframe, action.kind);
		EXPECT_EQ(first.header.message_id, action.target.message_id);
		EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::Video));
	}
}

TEST(TelemetryProtocolSelectiveNack, DiscardPurgeAndClearUseExactSessionEndpointIdentity)
{
	SelectiveNackTracker tracker;
	const auto peer_a = endpoint(1U);
	const auto peer_b = endpoint(2U);
	const auto a = state_fragment(0U, 2U, 1U, 10U, 42U);
	const auto b = state_fragment(0U, 2U, 2U, 20U, 42U);
	auto other_session = state_fragment(0U, 2U, 3U, 30U, 43U);
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(a, peer_a, 0U, ReliableDefaultRtoUs));
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(b, peer_b, 0U, ReliableDefaultRtoUs));
	ASSERT_EQ(SelectiveNackObserveResult::Accepted,
		tracker.observe_validated_fragment(other_session, peer_a, 0U, ReliableDefaultRtoUs));
	EXPECT_FALSE(tracker.discard(42U, peer_b, MessageType::Manifest, 1U));
	EXPECT_FALSE(tracker.discard(42U, peer_a, MessageType::Manifest, 99U));
	EXPECT_EQ(1U, tracker.purge_session(42U, peer_a));
	EXPECT_EQ(2U, tracker.active_entries(MessageSizeClass::State));
	EXPECT_EQ(1U, tracker.purge_session(42U));
	EXPECT_EQ(1U, tracker.active_entries(MessageSizeClass::State));
	tracker.clear();
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::State));
	EXPECT_EQ(0U, tracker.active_entries(MessageSizeClass::Video));
}

TEST(TelemetryProtocolReliableReceivePipeline, MultipleReliableExpirationsReleaseEveryBufferBeforeOneCoalescedResync)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	ReassembledMessage completed;
	constexpr std::uint64_t start_us = 100U;
	std::size_t expected_bytes = 0;
	for (std::uint32_t message_id = 100U; message_id < 104U; ++message_id) {
		const auto first = state_fragment(0U, 3U, message_id, 0x10000000U + message_id);
		const auto result = pipeline.ingest_validated_fragment(first, peer, start_us, ReliableDefaultRtoUs, completed);
		ASSERT_EQ(SelectiveNackObserveResult::Accepted, result.tracking);
		ASSERT_TRUE(result.reassembly_attempted);
		ASSERT_EQ(ReassemblyResult::Accepted, result.reassembly);
		expected_bytes += first.header.message_size;
	}
	ASSERT_EQ(MaxStateReassembliesPerClient, pipeline.active_reassemblies(MessageSizeClass::State));
	ASSERT_EQ(expected_bytes, pipeline.reserved_bytes(MessageSizeClass::State));

	auto action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None,
		pipeline.poll(start_us + SelectiveNackReliableRetentionUs - 1U, 0U, action));
	ASSERT_EQ(SelectiveNackPollResult::Ready, pipeline.poll(start_us + SelectiveNackReliableRetentionUs, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestResync, action.kind);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, pipeline.reserved_bytes(MessageSizeClass::State));
	EXPECT_EQ(0U, pipeline.tracked_entries(MessageSizeClass::State));
	EXPECT_EQ(SelectiveNackPollResult::None, pipeline.poll(start_us + SelectiveNackReliableRetentionUs, 0U, action));
}

TEST(TelemetryProtocolReliableReceivePipeline, IngestAtReliableDeadlineEvictsOldOwnerBeforeAdmittingReplacement)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	ReassembledMessage completed;
	const auto old_first = state_fragment(0U, 3U, 200U, 0x20000001U);
	const auto replacement = state_fragment(0U, 3U, 201U, 0x20000002U);
	ASSERT_EQ(ReassemblyResult::Accepted,
		pipeline.ingest_validated_fragment(old_first, peer, 0U, ReliableDefaultRtoUs, completed).reassembly);

	const auto result = pipeline.ingest_validated_fragment(replacement,
		peer,
		SelectiveNackReliableRetentionUs,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::Accepted, result.tracking);
	EXPECT_EQ(ReassemblyResult::Accepted, result.reassembly);
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(replacement.header.message_size, pipeline.reserved_bytes(MessageSizeClass::State));
	auto action = sentinel_action();
	ASSERT_EQ(SelectiveNackPollResult::Ready, pipeline.poll(SelectiveNackReliableRetentionUs, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestResync, action.kind);
}

TEST(TelemetryProtocolReliableReceivePipeline, CompletionAtExactDeadlineCannotPublishTheExpiredAssembly)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	const auto crc = zero_message_crc(2U) ^ 1U;
	const auto first = state_fragment(0U, 2U, 300U, crc);
	const auto final = state_fragment(1U, 2U, 300U, crc);
	ReassembledMessage completed;
	completed.header.message_id = 0xfeedU;
	completed.payload = {0xaaU};
	ASSERT_EQ(ReassemblyResult::Accepted,
		pipeline.ingest_validated_fragment(first, peer, 0U, ReliableDefaultRtoUs, completed).reassembly);

	const auto result = pipeline.ingest_validated_fragment(final,
		peer,
		SelectiveNackReliableRetentionUs,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::Accepted, result.tracking);
	EXPECT_EQ(ReassemblyResult::Accepted, result.reassembly);
	EXPECT_EQ(0xfeedU, completed.header.message_id);
	ASSERT_EQ(1U, completed.payload.size());
	EXPECT_EQ(0xaaU, completed.payload[0]);
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::State));
	const auto bad_completion = pipeline.ingest_validated_fragment(first,
		peer,
		SelectiveNackReliableRetentionUs,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::Completed, bad_completion.tracking);
	EXPECT_EQ(ReassemblyResult::MessageCrcMismatch, bad_completion.reassembly);
	EXPECT_EQ(0xfeedU, completed.header.message_id);
	ASSERT_EQ(1U, completed.payload.size());
	EXPECT_EQ(0xaaU, completed.payload[0]);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::State));
	auto action = sentinel_action();
	ASSERT_EQ(SelectiveNackPollResult::Ready, pipeline.poll(SelectiveNackReliableRetentionUs, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestResync, action.kind);
}

TEST(TelemetryProtocolReliableReceivePipeline, CompletionAtExactProducerDeadlineCannotPublishTheExpiredIdr)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	constexpr std::uint64_t producer_deadline_us = 9'000U;
	const auto crc = zero_message_crc(2U);
	const auto first = video_idr_fragment(0U, 2U, 350U, crc);
	const auto final = video_idr_fragment(1U, 2U, 350U, crc);
	ReassembledMessage completed;
	completed.header.message_id = 0xfeedU;
	completed.payload = {0xaaU};
	const auto accepted = pipeline.ingest_validated_fragment(first,
		peer,
		0U,
		ReliableDefaultRtoUs,
		completed,
		producer_deadline_us,
		producer_deadline_us - 1U);
	ASSERT_EQ(SelectiveNackObserveResult::Accepted, accepted.tracking);
	ASSERT_EQ(ReassemblyResult::Accepted, accepted.reassembly);

	const auto expired = pipeline.ingest_validated_fragment(final,
		peer,
		1U,
		ReliableDefaultRtoUs,
		completed,
		producer_deadline_us,
		producer_deadline_us);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, expired.tracking);
	EXPECT_FALSE(expired.reassembly_attempted);
	EXPECT_EQ(0xfeedU, completed.header.message_id);
	ASSERT_EQ(1U, completed.payload.size());
	EXPECT_EQ(0xaaU, completed.payload[0]);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
	auto action = sentinel_action();
	ASSERT_EQ(SelectiveNackPollResult::Ready, pipeline.poll(1U, producer_deadline_us, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestKeyframe, action.kind);
	EXPECT_EQ(350U, action.target.message_id);
}

TEST(TelemetryProtocolReliableReceivePipeline, UnfragmentedIdrPublishesOnceAndExpiredSuccessorNeverAllocates)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	constexpr std::uint64_t producer_deadline_us = 10'000U;
	const auto idr = fragment(MessageType::TargetVideoFrame, 0U, 1U, 351U, zero_message_crc(1U), 42U, VideoIdrFlag);
	ReassembledMessage completed;
	const auto first = pipeline.ingest_validated_fragment(idr,
		peer,
		0U,
		ReliableDefaultRtoUs,
		completed,
		producer_deadline_us,
		producer_deadline_us - 1U);
	ASSERT_EQ(SelectiveNackObserveResult::Completed, first.tracking);
	ASSERT_EQ(ReassemblyResult::Completed, first.reassembly);
	ASSERT_TRUE(first.reassembly_attempted);
	EXPECT_EQ(351U, completed.header.message_id);

	completed.header.message_id = 0xfeedU;
	completed.payload = {0xaaU};
	const auto duplicate = pipeline.ingest_validated_fragment(idr,
		peer,
		1U,
		ReliableDefaultRtoUs,
		completed,
		producer_deadline_us,
		producer_deadline_us - 1U);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, duplicate.tracking);
	EXPECT_FALSE(duplicate.reassembly_attempted);
	EXPECT_EQ(0xfeedU, completed.header.message_id);
	ASSERT_EQ(1U, completed.payload.size());
	EXPECT_EQ(0xaaU, completed.payload[0]);

	const auto expired_idr =
		fragment(MessageType::TargetVideoFrame, 0U, 1U, 352U, zero_message_crc(1U), 42U, VideoIdrFlag);
	const auto expired = pipeline.ingest_validated_fragment(expired_idr,
		peer,
		2U,
		ReliableDefaultRtoUs,
		completed,
		producer_deadline_us,
		producer_deadline_us);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, expired.tracking);
	EXPECT_FALSE(expired.reassembly_attempted);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
	auto action = sentinel_action();
	ASSERT_EQ(SelectiveNackPollResult::Ready, pipeline.poll(2U, producer_deadline_us, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestKeyframe, action.kind);
	EXPECT_EQ(352U, action.target.message_id);
}

TEST(TelemetryProtocolReliableReceivePipeline, NewIdrAtOldDeadlineCancelsStaleRecoveryAndOwnsTheOnlyIdrBuffer)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	ReassembledMessage completed;
	const auto old_idr = video_idr_fragment(0U, 3U, 400U, 0x40000001U);
	const auto new_idr = video_idr_fragment(0U, 3U, 401U, 0x40000002U);
	ASSERT_EQ(ReassemblyResult::Accepted,
		pipeline.ingest_validated_fragment(old_idr, peer, 0U, ReliableDefaultRtoUs, completed).reassembly);

	const auto result =
		pipeline.ingest_validated_fragment(new_idr, peer, ReliableVideoIdrRetentionUs, ReliableDefaultRtoUs, completed);
	EXPECT_EQ(SelectiveNackObserveResult::Accepted, result.tracking);
	EXPECT_EQ(ReassemblyResult::Accepted, result.reassembly);
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(new_idr.header.message_size, pipeline.reserved_bytes(MessageSizeClass::Video));
	auto action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, pipeline.poll(ReliableVideoIdrRetentionUs, 0U, action));
	EXPECT_EQ(SelectiveNackPollResult::None, pipeline.poll(2U * ReliableVideoIdrRetentionUs - 1U, 0U, action));
	ASSERT_EQ(SelectiveNackPollResult::Ready, pipeline.poll(2U * ReliableVideoIdrRetentionUs, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestKeyframe, action.kind);
	EXPECT_EQ(401U, action.target.message_id);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(0U, pipeline.reserved_bytes(MessageSizeClass::Video));
}

TEST(TelemetryProtocolReliableReceivePipeline, CompletedIdrHighWatermarkRejectsEveryOlderLateFragment)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	const auto crc = zero_message_crc(3U);
	ReassembledMessage completed;
	for (std::uint16_t index = 0; index < 3U; ++index) {
		const auto result = pipeline.ingest_validated_fragment(video_idr_fragment(index, 3U, 502U, crc),
			peer,
			index,
			ReliableDefaultRtoUs,
			completed);
		EXPECT_EQ(index == 2U ? ReassemblyResult::Completed : ReassemblyResult::Accepted, result.reassembly);
	}
	ASSERT_EQ(502U, completed.header.message_id);
	ASSERT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));

	const auto stale = pipeline.ingest_validated_fragment(video_idr_fragment(0U, 3U, 501U, crc),
		peer,
		3U,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, stale.tracking);
	EXPECT_FALSE(stale.reassembly_attempted);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(0U, pipeline.reserved_bytes(MessageSizeClass::Video));
	auto action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None, pipeline.poll(600'000U, 0U, action));
}

TEST(TelemetryProtocolReliableReceivePipeline, FailedIdrOwnerAdmissionRestoresKeyframeRecoveryAndTombstonesThatIdr)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	ReassembledMessage completed;
	for (std::uint32_t message_id = 800U; message_id < 803U; ++message_id) {
		const auto result = pipeline.ingest_validated_fragment(video_interframe_fragment(0U, 3U, message_id),
			peer,
			0U,
			ReliableDefaultRtoUs,
			completed);
		ASSERT_EQ(ReassemblyResult::Accepted, result.reassembly);
	}
	ASSERT_EQ(MaxVideoReassembliesPerClient, pipeline.active_reassemblies(MessageSizeClass::Video));

	const auto idr = video_idr_fragment(0U, 3U, 803U, 0x80300001U);
	const auto failed = pipeline.ingest_validated_fragment(idr, peer, 1U, ReliableDefaultRtoUs, completed);
	EXPECT_EQ(SelectiveNackObserveResult::Accepted, failed.tracking);
	EXPECT_EQ(ReassemblyResult::QuotaExceeded, failed.reassembly);
	// The terminal newer IDR makes all older partial interframes obsolete even
	// though those buffers caused its initial quota failure.
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
	const auto late_interframe = pipeline.ingest_validated_fragment(video_interframe_fragment(1U, 3U, 802U),
		peer,
		1U,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, late_interframe.tracking);
	EXPECT_FALSE(late_interframe.reassembly_attempted);
	auto action = sentinel_action();
	ASSERT_EQ(SelectiveNackPollResult::Ready, pipeline.poll(1U, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestKeyframe, action.kind);
	EXPECT_EQ(803U, action.target.message_id);

	const auto retry = pipeline.ingest_validated_fragment(idr, peer, 2U, ReliableDefaultRtoUs, completed);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, retry.tracking);
	EXPECT_FALSE(retry.reassembly_attempted);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
}

TEST(TelemetryProtocolReliableReceivePipeline, ContradictoryIdrIdentityPurgesBytesAndRequestsANewKeyframe)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	ReassembledMessage completed;
	const auto first = video_idr_fragment(0U, 3U, 900U, 0x90000001U);
	ASSERT_EQ(ReassemblyResult::Accepted,
		pipeline.ingest_validated_fragment(first, peer, 0U, ReliableDefaultRtoUs, completed).reassembly);
	const auto contradictory = video_idr_fragment(1U, 3U, 900U, 0x90000002U);
	const auto conflict = pipeline.ingest_validated_fragment(contradictory, peer, 1U, ReliableDefaultRtoUs, completed);
	EXPECT_EQ(SelectiveNackObserveResult::IdentityConflict, conflict.tracking);
	EXPECT_FALSE(conflict.reassembly_attempted);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(0U, pipeline.reserved_bytes(MessageSizeClass::Video));
	auto action = sentinel_action();
	ASSERT_EQ(SelectiveNackPollResult::Ready, pipeline.poll(1U, 0U, action));
	EXPECT_EQ(SelectiveNackActionKind::RequestKeyframe, action.kind);
	EXPECT_EQ(900U, action.target.message_id);
	const auto retry = pipeline.ingest_validated_fragment(first, peer, 2U, ReliableDefaultRtoUs, completed);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, retry.tracking);
	EXPECT_FALSE(retry.reassembly_attempted);
}

TEST(TelemetryProtocolReliableReceivePipeline, DeltaAndInterframeUseSharedQuotaAndTheirExactSilentTimeouts)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	ReassembledMessage completed;
	const auto delta = delta_fragment(0U, 3U, 600U);
	const auto delta_result = pipeline.ingest_validated_fragment(delta, peer, 10U, ReliableDefaultRtoUs, completed);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, delta_result.tracking);
	EXPECT_EQ(ReassemblyResult::Accepted, delta_result.reassembly);
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::State));
	auto action = sentinel_action();
	EXPECT_EQ(SelectiveNackPollResult::None,
		pipeline.poll(10U + ReplaceableStateReassemblyRetentionUs - 1U, 0U, action));
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(SelectiveNackPollResult::None, pipeline.poll(10U + ReplaceableStateReassemblyRetentionUs, 0U, action));
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::State));
	const auto late_delta = pipeline.ingest_validated_fragment(delta_fragment(2U, 3U, 600U),
		peer,
		10U + ReplaceableStateReassemblyRetentionUs,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, late_delta.tracking);
	EXPECT_FALSE(late_delta.reassembly_attempted);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::State));

	const auto interframe = video_interframe_fragment(0U, 3U, 601U);
	const auto video_result =
		pipeline.ingest_validated_fragment(interframe, peer, 600'000U, ReliableDefaultRtoUs, completed);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, video_result.tracking);
	EXPECT_EQ(ReassemblyResult::Accepted, video_result.reassembly);
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(SelectiveNackPollResult::None,
		pipeline.poll(600'000U + VideoInterframeReassemblyRetentionUs - 1U, 0U, action));
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(SelectiveNackPollResult::None,
		pipeline.poll(600'000U + VideoInterframeReassemblyRetentionUs, 0U, action));
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
	const auto late_interframe = pipeline.ingest_validated_fragment(video_interframe_fragment(2U, 3U, 601U),
		peer,
		600'000U + VideoInterframeReassemblyRetentionUs,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::IgnoredIneligible, late_interframe.tracking);
	EXPECT_FALSE(late_interframe.reassembly_attempted);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
}

TEST(TelemetryProtocolReliableReceivePipeline, SilentReplaceableTerminalWatermarksDeduplicateAndPurgeOnlyTheirClass)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint();
	ReassembledMessage completed;

	const auto old_delta = delta_fragment(0U, 2U, 610U, zero_message_crc(2U));
	ASSERT_EQ(ReassemblyResult::Accepted,
		pipeline.ingest_validated_fragment(old_delta, peer, 0U, ReliableDefaultRtoUs, completed).reassembly);
	const auto new_delta = fragment(MessageType::Delta, 0U, 1U, 611U, zero_message_crc(1U), 42U, 0U);
	ASSERT_EQ(ReassemblyResult::Completed,
		pipeline.ingest_validated_fragment(new_delta, peer, 1U, ReliableDefaultRtoUs, completed).reassembly);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::State));
	const auto old_delta_late = pipeline.ingest_validated_fragment(delta_fragment(1U, 2U, 610U, zero_message_crc(2U)),
		peer,
		2U,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_FALSE(old_delta_late.reassembly_attempted);
	const auto delta_duplicate =
		pipeline.ingest_validated_fragment(new_delta, peer, 3U, ReliableDefaultRtoUs, completed);
	EXPECT_FALSE(delta_duplicate.reassembly_attempted);

	const auto replaceable_event = fragment(MessageType::EventBatch, 0U, 1U, 612U, zero_message_crc(1U), 42U, 0U);
	ASSERT_EQ(ReassemblyResult::Completed,
		pipeline.ingest_validated_fragment(replaceable_event, peer, 4U, ReliableDefaultRtoUs, completed).reassembly);
	const auto event_duplicate =
		pipeline.ingest_validated_fragment(replaceable_event, peer, 5U, ReliableDefaultRtoUs, completed);
	EXPECT_FALSE(event_duplicate.reassembly_attempted);

	const auto old_interframe = video_interframe_fragment(0U, 2U, 613U, zero_message_crc(2U));
	ASSERT_EQ(ReassemblyResult::Accepted,
		pipeline.ingest_validated_fragment(old_interframe, peer, 6U, ReliableDefaultRtoUs, completed).reassembly);
	const auto new_interframe = fragment(MessageType::TargetVideoFrame, 0U, 1U, 614U, zero_message_crc(1U), 42U, 0U);
	ASSERT_EQ(ReassemblyResult::Completed,
		pipeline.ingest_validated_fragment(new_interframe, peer, 7U, ReliableDefaultRtoUs, completed).reassembly);
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::Video));
	const auto old_interframe_late =
		pipeline.ingest_validated_fragment(video_interframe_fragment(1U, 2U, 613U, zero_message_crc(2U)),
			peer,
			8U,
			ReliableDefaultRtoUs,
			completed);
	EXPECT_FALSE(old_interframe_late.reassembly_attempted);

	const auto reliable_event_first =
		fragment(MessageType::EventBatch, 0U, 2U, 615U, zero_message_crc(2U), 42U, FragmentedAckFlags);
	ASSERT_EQ(ReassemblyResult::Accepted,
		pipeline.ingest_validated_fragment(reliable_event_first, peer, 9U, ReliableDefaultRtoUs, completed).reassembly);
	const auto newer_replaceable_event = fragment(MessageType::EventBatch, 0U, 1U, 616U, zero_message_crc(1U), 42U, 0U);
	ASSERT_EQ(ReassemblyResult::Completed,
		pipeline.ingest_validated_fragment(newer_replaceable_event, peer, 10U, ReliableDefaultRtoUs, completed)
			.reassembly);
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::State));
	const auto reliable_event_final =
		fragment(MessageType::EventBatch, 1U, 2U, 615U, zero_message_crc(2U), 42U, FragmentedAckFlags);
	const auto reliable_result =
		pipeline.ingest_validated_fragment(reliable_event_final, peer, 11U, ReliableDefaultRtoUs, completed);
	EXPECT_EQ(SelectiveNackObserveResult::Completed, reliable_result.tracking);
	EXPECT_EQ(ReassemblyResult::Completed, reliable_result.reassembly);
}

TEST(TelemetryProtocolReliableReceivePipeline, WrongEndpointDiscardCannotReleaseOwnedBytes)
{
	ReliableReceivePipeline pipeline;
	const auto peer = endpoint(1U);
	const auto wrong_peer = endpoint(2U);
	ReassembledMessage completed;
	const auto first = state_fragment(0U, 3U, 700U, 0x70000001U);
	ASSERT_EQ(ReassemblyResult::Accepted,
		pipeline.ingest_validated_fragment(first, peer, 0U, ReliableDefaultRtoUs, completed).reassembly);
	EXPECT_FALSE(
		pipeline.discard(first.header.session_id, wrong_peer, first.header.message_type, first.header.message_id));
	EXPECT_EQ(1U, pipeline.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(first.header.message_size, pipeline.reserved_bytes(MessageSizeClass::State));
	EXPECT_TRUE(pipeline.discard(first.header.session_id, peer, first.header.message_type, first.header.message_id));
	EXPECT_EQ(0U, pipeline.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, pipeline.reserved_bytes(MessageSizeClass::State));
}

TEST(TelemetryProtocolReliableReceiveNackHelpers, ErrorNackRequiresExactContextEligibilityAndReason)
{
	const auto peer = endpoint();
	const auto context = client_context(42U, peer);
	auto target = state_fragment(0U, 2U, 0x1234U, 0xabcdef01U, 42U).header;
	const std::array<NackReason, 6> accepted{{
		NackReason::BadMessageCrc,
		NackReason::StaleBaseline,
		NackReason::BadFragmentLayout,
		NackReason::ResourceLimit,
		NackReason::SemanticValidationFailed,
		NackReason::DeadlineExpired,
	}};
	for (const auto reason : accepted) {
		SCOPED_TRACE(static_cast<unsigned>(reason));
		auto nack = sentinel_nack();
		ASSERT_EQ(ValidationError::None, make_context_validated_error_nack(target, peer, context, reason, 0U, nack));
		EXPECT_EQ(target.message_id, nack.target_message_id);
		EXPECT_EQ(target.message_type, nack.target_message_type);
		EXPECT_EQ(reason, nack.reason);
		EXPECT_EQ(target.fragment_count, nack.target_fragment_count);
		EXPECT_EQ(target.message_crc32, nack.target_message_crc32);
		EXPECT_EQ(0U, nack.needed_before_producer_time_us);
		EXPECT_TRUE(nack.missing_bitmap.empty());
	}

	// Bad layout reporting needs contextual identity, not a successfully revalidated layout.
	auto bad_layout = target;
	bad_layout.fragment_offset++;
	auto nack = sentinel_nack();
	EXPECT_EQ(ValidationError::None,
		make_context_validated_error_nack(bad_layout, peer, context, NackReason::BadFragmentLayout, 0U, nack));

	const std::array<NackReason, 3> rejected{
		{NackReason::Invalid, NackReason::MissingFragments, NackReason::UnsupportedMessage}};
	for (const auto reason : rejected) {
		nack = sentinel_nack();
		EXPECT_EQ(ValidationError::InvalidStateTransition,
			make_context_validated_error_nack(target, peer, context, reason, 0U, nack));
		expect_sentinel_nack(nack);
	}
	nack = sentinel_nack();
	EXPECT_EQ(ValidationError::UnknownEnum,
		make_context_validated_error_nack(target, peer, context, static_cast<NackReason>(0xffU), 0U, nack));
	expect_sentinel_nack(nack);
}

TEST(TelemetryProtocolReliableReceiveNackHelpers, ErrorNackFailurePrecedenceLeavesOutputUntouched)
{
	const auto peer = endpoint();
	const auto context = client_context(42U, peer);
	const auto target = state_fragment(0U, 2U, 9U, 10U, 42U).header;
	auto nack = sentinel_nack();
	auto no_session = context;
	no_session.active_session_id = 0U;
	EXPECT_EQ(ValidationError::SessionMismatch,
		make_context_validated_error_nack(target, peer, no_session, NackReason::BadMessageCrc, 0U, nack));
	expect_sentinel_nack(nack);

	auto wrong_session = target;
	wrong_session.session_id++;
	EXPECT_EQ(ValidationError::SessionMismatch,
		make_context_validated_error_nack(wrong_session, endpoint(2U), context, NackReason::BadMessageCrc, 0U, nack));
	expect_sentinel_nack(nack);
	auto wrong_role = context;
	wrong_role.local_role = LocalEndpointRole::Producer;
	EXPECT_EQ(ValidationError::WrongDirection,
		make_context_validated_error_nack(target, endpoint(2U), wrong_role, NackReason::BadMessageCrc, 0U, nack));
	expect_sentinel_nack(nack);
	EXPECT_EQ(ValidationError::EndpointMismatch,
		make_context_validated_error_nack(target, endpoint(2U), context, NackReason::BadMessageCrc, 0U, nack));
	expect_sentinel_nack(nack);

	auto reserved = target;
	reserved.flags = static_cast<std::uint8_t>(reserved.flags | 0x80U);
	EXPECT_EQ(ValidationError::ReservedHeaderFlag,
		make_context_validated_error_nack(reserved, peer, context, NackReason::BadMessageCrc, 0U, nack));
	expect_sentinel_nack(nack);
	auto ineligible = target;
	ineligible.flags = FragmentedFlag;
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		make_context_validated_error_nack(ineligible, peer, context, NackReason::BadMessageCrc, 0U, nack));
	expect_sentinel_nack(nack);
	auto zero_id = target;
	zero_id.message_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange,
		make_context_validated_error_nack(zero_id, peer, context, NackReason::BadMessageCrc, 0U, nack));
	expect_sentinel_nack(nack);
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		make_context_validated_error_nack(target, peer, context, NackReason::BadMessageCrc, 1U, nack));
	expect_sentinel_nack(nack);
}

TEST(TelemetryProtocolReliableReceiveNackHelpers, VideoIdrRejectsEveryBitmapFreeErrorNack)
{
	const auto peer = endpoint();
	const auto context = client_context(42U, peer);
	const auto target = video_idr_fragment(0U, 3U, 99U, 0x99887766U, 42U).header;
	constexpr std::uint64_t deadline = 1'234'567U;
	const std::array<NackReason, 6> forbidden{{
		NackReason::BadMessageCrc,
		NackReason::StaleBaseline,
		NackReason::BadFragmentLayout,
		NackReason::ResourceLimit,
		NackReason::SemanticValidationFailed,
		NackReason::DeadlineExpired,
	}};
	for (const auto reason : forbidden) {
		SCOPED_TRACE(static_cast<unsigned>(reason));
		auto nack = sentinel_nack();
		EXPECT_EQ(ValidationError::InvalidStateTransition,
			make_context_validated_error_nack(target, peer, context, reason, deadline, nack));
		expect_sentinel_nack(nack);
	}
}

TEST(TelemetryProtocolReliableReceiveNackHelpers, UnsupportedMessageNeedsActivePeerAndAckRequiredIntent)
{
	const auto peer = endpoint();
	const auto context = client_context(42U, peer);
	TelemetryDatagramHeader target;
	target.message_type = static_cast<MessageType>(FirstReservedMessageType);
	target.flags = AckRequiredFlag;
	target.session_id = 42U;
	target.message_id = 0x11223344U;
	target.fragment_count = 7U;
	target.message_crc32 = 0xaabbccddU;

	auto nack = sentinel_nack();
	ASSERT_EQ(ValidationError::None, make_context_validated_unsupported_message_nack(target, peer, context, nack));
	EXPECT_EQ(target.message_id, nack.target_message_id);
	EXPECT_EQ(target.message_type, nack.target_message_type);
	EXPECT_EQ(NackReason::UnsupportedMessage, nack.reason);
	EXPECT_EQ(target.fragment_count, nack.target_fragment_count);
	EXPECT_EQ(target.message_crc32, nack.target_message_crc32);
	EXPECT_EQ(0U, nack.needed_before_producer_time_us);
	EXPECT_TRUE(nack.missing_bitmap.empty());

	auto no_session = context;
	no_session.active_session_id = 0U;
	nack = sentinel_nack();
	EXPECT_EQ(ValidationError::SessionMismatch,
		make_context_validated_unsupported_message_nack(target, peer, no_session, nack));
	expect_sentinel_nack(nack);
	auto wrong_session = target;
	wrong_session.session_id++;
	EXPECT_EQ(ValidationError::SessionMismatch,
		make_context_validated_unsupported_message_nack(wrong_session, endpoint(2U), context, nack));
	expect_sentinel_nack(nack);
	EXPECT_EQ(ValidationError::EndpointMismatch,
		make_context_validated_unsupported_message_nack(target, endpoint(2U), context, nack));
	expect_sentinel_nack(nack);

	auto no_ack = target;
	no_ack.flags = MessageFlagNone;
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		make_context_validated_unsupported_message_nack(no_ack, peer, context, nack));
	expect_sentinel_nack(nack);
	auto reserved = target;
	reserved.flags = static_cast<std::uint8_t>(AckRequiredFlag | 0x20U);
	EXPECT_EQ(ValidationError::ReservedHeaderFlag,
		make_context_validated_unsupported_message_nack(reserved, peer, context, nack));
	expect_sentinel_nack(nack);
	auto known = target;
	known.message_type = MessageType::Manifest;
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		make_context_validated_unsupported_message_nack(known, peer, context, nack));
	expect_sentinel_nack(nack);
	auto invalid = target;
	invalid.message_type = MessageType::Invalid;
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		make_context_validated_unsupported_message_nack(invalid, peer, context, nack));
	expect_sentinel_nack(nack);
	auto zero_id = target;
	zero_id.message_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange,
		make_context_validated_unsupported_message_nack(zero_id, peer, context, nack));
	expect_sentinel_nack(nack);
}

} // namespace
