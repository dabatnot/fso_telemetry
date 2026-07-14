#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/telemetry_reliable_window.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {

using namespace telemetry::protocol;

constexpr std::uint64_t TestSessionId = 0x1122334455667788ULL;
constexpr std::uint64_t TestFirstSendUs = 10'000ULL;

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{bytes.empty() ? nullptr : bytes.data(), bytes.size()};
}

EndpointKey endpoint(std::uint8_t host = 1U, std::uint16_t port = 7808U)
{
	return EndpointKey::from_ipv4({{192U, 0U, 2U, host}}, port);
}

std::vector<std::uint8_t> payload(std::size_t size = 8U, std::uint8_t seed = 0x31U)
{
	std::vector<std::uint8_t> bytes(size);
	for (std::size_t index = 0; index < size; ++index) {
		bytes[index] = static_cast<std::uint8_t>(seed + index * 17U);
	}
	return bytes;
}

MessageType type_for(ReliableMessageClass message_class)
{
	switch (message_class) {
	case ReliableMessageClass::ControlDrop:
		return MessageType::TargetVideoSubscribe;
	case ReliableMessageClass::ControlRequestKeyframe:
		return MessageType::TargetVideoKeyframeRequest;
	case ReliableMessageClass::ControlRequestResync:
		return MessageType::ResyncRequest;
	case ReliableMessageClass::SessionCritical:
		return MessageType::SessionBegin;
	case ReliableMessageClass::SessionClosing:
		return MessageType::SessionEnd;
	case ReliableMessageClass::Transaction:
		return MessageType::Manifest;
	case ReliableMessageClass::ReliableEvent:
		return MessageType::EventBatch;
	case ReliableMessageClass::VideoIdr:
		return MessageType::TargetVideoFrame;
	case ReliableMessageClass::HandshakeCritical:
		return MessageType::Welcome;
	case ReliableMessageClass::HelloNegotiation:
		return MessageType::Hello;
	default:
		return MessageType::Invalid;
	}
}

RequiredAckLevel ack_for(ReliableMessageClass message_class)
{
	switch (message_class) {
	case ReliableMessageClass::ControlRequestKeyframe:
	case ReliableMessageClass::ControlRequestResync:
		return RequiredAckLevel::Validated;
	case ReliableMessageClass::VideoIdr:
	case ReliableMessageClass::HelloNegotiation:
		return RequiredAckLevel::None;
	default:
		return RequiredAckLevel::Applied;
	}
}

ReliableMessageToRetain message(ReliableMessageClass message_class,
	std::uint32_t message_id,
	const std::vector<std::uint8_t>& bytes,
	std::uint64_t session_id = TestSessionId,
	EndpointKey peer = endpoint())
{
	ReliableMessageToRetain result;
	result.session_id = message_class == ReliableMessageClass::HelloNegotiation ? 0U : session_id;
	result.endpoint = peer;
	result.message_type = type_for(message_class);
	result.message_id = message_id;
	result.logical_payload = byte_view(bytes);
	result.required_ack = ack_for(message_class);
	result.message_class = message_class;
	if (message_class == ReliableMessageClass::Transaction) {
		result.transaction_id = message_id;
		result.transaction_sha256.fill(static_cast<std::uint8_t>(message_id));
	}
	if (result.required_ack != RequiredAckLevel::None) {
		result.base_flags |= MessageFlagAckRequired;
	}
	if (message_class == ReliableMessageClass::VideoIdr) {
		result.base_flags |= MessageFlagVideoIdr;
	}
	if (result.message_type == MessageType::FullSnapshot) {
		result.base_flags |= MessageFlagKeyframe;
	}
	if (result.message_type == MessageType::FullSnapshot || result.message_type == MessageType::EventBatch) {
		result.frame_id = 9U;
		result.mission_time_us = 123'456;
	}

	TelemetryFragmenter fragmenter;
	if (TelemetryFragmenter::create(result.logical_payload, message_size_class(result.message_type), fragmenter)) {
		result.fragment_count = fragmenter.fragment_count();
		result.message_crc32 = fragmenter.message_crc32();
		if (result.fragment_count > 1U) {
			result.base_flags |= MessageFlagFragmented;
		}
	}
	return result;
}

AckPayload ack_for(const ReliableMessageToRetain& retained,
	std::uint8_t flags = static_cast<std::uint8_t>(AckFlag::Validated))
{
	AckPayload result;
	result.target_message_id = retained.message_id;
	result.target_message_type = retained.message_type;
	result.ack_flags = flags;
	result.target_fragment_count = retained.fragment_count;
	result.target_message_crc32 = retained.message_crc32;
	return result;
}

NackPayload nack_for(const ReliableMessageToRetain& retained,
	NackReason reason,
	ByteView bitmap = {},
	std::uint64_t needed_before_us = 0U)
{
	NackPayload result;
	result.target_message_id = retained.message_id;
	result.target_message_type = retained.message_type;
	result.reason = reason;
	result.target_fragment_count = retained.fragment_count;
	result.target_message_crc32 = retained.message_crc32;
	result.needed_before_producer_time_us = needed_before_us;
	result.missing_bitmap = bitmap;
	return result;
}

void expect_key(const ReliableMessageKey& key, const ReliableMessageToRetain& retained)
{
	EXPECT_EQ(retained.session_id, key.session_id);
	EXPECT_EQ(retained.endpoint, key.endpoint);
	EXPECT_EQ(retained.message_type, key.message_type);
	EXPECT_EQ(retained.message_id, key.message_id);
	EXPECT_EQ(retained.fragment_count, key.fragment_count);
	EXPECT_EQ(retained.message_crc32, key.message_crc32);
	EXPECT_EQ(retained.transaction_id, key.transaction_id);
	EXPECT_EQ(retained.transaction_sha256, key.transaction_sha256);
}

void seed_decision_canary(ReliableNackDecision& decision)
{
	decision.kind = ReliableNackDecisionKind::FullRetransmissionScheduled;
	decision.terminal_policy = ReliableTerminalPolicy::CloseSession;
	decision.target.session_id = 0xfedcba9876543210ULL;
	decision.target.endpoint = endpoint(99U, 9999U);
	decision.target.message_type = MessageType::CapabilityUpdate;
	decision.target.message_id = 0xabcdef01U;
	decision.target.fragment_count = 77U;
	decision.target.message_crc32 = 0xdecafbadU;
}

void expect_decision_canary(const ReliableNackDecision& decision)
{
	EXPECT_EQ(ReliableNackDecisionKind::FullRetransmissionScheduled, decision.kind);
	EXPECT_EQ(ReliableTerminalPolicy::CloseSession, decision.terminal_policy);
	EXPECT_EQ(0xfedcba9876543210ULL, decision.target.session_id);
	EXPECT_EQ(endpoint(99U, 9999U), decision.target.endpoint);
	EXPECT_EQ(MessageType::CapabilityUpdate, decision.target.message_type);
	EXPECT_EQ(0xabcdef01U, decision.target.message_id);
	EXPECT_EQ(77U, decision.target.fragment_count);
	EXPECT_EQ(0xdecafbadU, decision.target.message_crc32);
}

void seed_action_canary(ReliableWindowAction& action)
{
	action.kind = ReliableWindowActionKind::TerminalPolicy;
	action.terminal_policy = ReliableTerminalPolicy::CloseSession;
	action.target.session_id = 0xfedcba9876543210ULL;
	action.target.message_id = 0xabcdef01U;
	action.retransmission.retry_number = 0x12345678U;
	action.retransmission.absolute_deadline_us = 0x8877665544332211ULL;
}

void expect_action_canary(const ReliableWindowAction& action)
{
	EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
	EXPECT_EQ(ReliableTerminalPolicy::CloseSession, action.terminal_policy);
	EXPECT_EQ(0xfedcba9876543210ULL, action.target.session_id);
	EXPECT_EQ(0xabcdef01U, action.target.message_id);
	EXPECT_EQ(0x12345678U, action.retransmission.retry_number);
	EXPECT_EQ(0x8877665544332211ULL, action.retransmission.absolute_deadline_us);
}

TEST(TelemetryProtocolReliableWindow, RtoClampBackoffAndJitterAreDeterministicAndOverflowSafe)
{
	EXPECT_EQ(ReliableDefaultRtoUs, reliable_base_rto_us(false, 0U));
	EXPECT_EQ(ReliableDefaultRtoUs, reliable_base_rto_us(false, std::numeric_limits<std::uint64_t>::max()));
	EXPECT_EQ(ReliableMinimumRtoUs, reliable_base_rto_us(true, 0U));
	EXPECT_EQ(ReliableMinimumRtoUs, reliable_base_rto_us(true, 50'000U));
	EXPECT_EQ(100'002U, reliable_base_rto_us(true, 50'001U));
	EXPECT_EQ(999'998U, reliable_base_rto_us(true, 499'999U));
	EXPECT_EQ(ReliableMaximumRtoUs, reliable_base_rto_us(true, 500'000U));
	EXPECT_EQ(ReliableMaximumRtoUs, reliable_base_rto_us(true, std::numeric_limits<std::uint64_t>::max()));

	constexpr auto seed = 0x4653544c5f52544fULL;
	EXPECT_EQ(227'425U, reliable_retry_delay_us(250'000U, seed, TestSessionId, 7U, 0U));
	EXPECT_EQ(546'550U, reliable_retry_delay_us(250'000U, seed, TestSessionId, 7U, 1U));
	EXPECT_EQ(1'052'300U, reliable_retry_delay_us(250'000U, seed, TestSessionId, 7U, 2U));
	EXPECT_EQ(1'052'300U, reliable_retry_delay_us(250'000U, seed, TestSessionId, 7U, 2U));
	EXPECT_EQ(94'940U, reliable_retry_delay_us(1U, 0U, 0U, 1U, 0U));
	EXPECT_EQ(1'097'000U,
		reliable_retry_delay_us(std::numeric_limits<std::uint64_t>::max(),
			std::numeric_limits<std::uint64_t>::max(),
			std::numeric_limits<std::uint64_t>::max(),
			std::numeric_limits<std::uint32_t>::max(),
			std::numeric_limits<std::uint32_t>::max()));

	for (std::uint32_t retry = 0U; retry < 64U; ++retry) {
		const auto delay = reliable_retry_delay_us(250'000U, seed, TestSessionId, 99U, retry);
		const auto capped_base = retry == 0U ? 250'000ULL : retry == 1U ? 500'000ULL : 1'000'000ULL;
		EXPECT_GE(delay, capped_base * 9U / 10U) << retry;
		EXPECT_LE(delay, capped_base * 11U / 10U) << retry;
	}
}

TEST(TelemetryProtocolReliableWindow, ConfigureBoundsAreClosedAndFailureLeavesTheWindowUnchanged)
{
	ReliableSendWindow window;
	ReliableWindowLimits limits;
	limits.max_entries = 1U;
	limits.max_retained_bytes = 8U;
	limits.jitter_seed = 17U;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(limits, window));
	const auto bytes = payload(8U);
	const auto retained = message(ReliableMessageClass::SessionCritical, 1U, bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));

	auto invalid = limits;
	invalid.max_entries = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, ReliableSendWindow::configure(invalid, window));
	invalid = limits;
	invalid.max_entries = ReliableWindowMaximumEntries + 1U;
	EXPECT_EQ(ValidationError::OutOfRange, ReliableSendWindow::configure(invalid, window));
	invalid = limits;
	invalid.max_retained_bytes = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, ReliableSendWindow::configure(invalid, window));
	invalid = limits;
	invalid.max_retained_bytes = ReliableWindowMaximumRetainedBytes + 1U;
	EXPECT_EQ(ValidationError::OutOfRange, ReliableSendWindow::configure(invalid, window));
	EXPECT_EQ(1U, window.entry_count());
	EXPECT_EQ(bytes.size(), window.retained_bytes());
	EXPECT_EQ(1U, window.counters().retained);
	EXPECT_EQ(limits.max_entries, window.limits().max_entries);
	EXPECT_EQ(limits.max_retained_bytes, window.limits().max_retained_bytes);
	EXPECT_EQ(limits.jitter_seed, window.limits().jitter_seed);

	ReliableWindowLimits maximums;
	maximums.max_entries = ReliableWindowMaximumEntries;
	maximums.max_retained_bytes = ReliableWindowMaximumRetainedBytes;
	maximums.jitter_seed = 99U;
	EXPECT_EQ(ValidationError::None, ReliableSendWindow::configure(maximums, window));
	EXPECT_EQ(0U, window.entry_count());
	EXPECT_EQ(0U, window.retained_bytes());
	EXPECT_EQ(0U, window.counters().retained);
	EXPECT_EQ(ReliableDefaultRtoUs, window.base_rto_us());
}

TEST(TelemetryProtocolReliableWindow, EmptyWindowAndInvalidFragmentSelectionsPreserveOutputs)
{
	ReliableSendWindow window;
	ReliableWindowAction action;
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None, window.pull_next_action(0U, action));
	expect_action_canary(action);

	ReliableFragmentSelection selection;
	selection.all_fragments = true;
	selection.fragment_count = 2U;
	bool selected = false;
	EXPECT_EQ(ValidationError::None, selection.is_selected(0U, selected));
	EXPECT_TRUE(selected);
	selected = false;
	EXPECT_EQ(ValidationError::None, selection.is_selected(1U, selected));
	EXPECT_TRUE(selected);
	selected = true;
	EXPECT_EQ(ValidationError::BadFragmentIndex, selection.is_selected(2U, selected));
	EXPECT_TRUE(selected);

	selection.fragment_count = 0U;
	selected = true;
	EXPECT_EQ(ValidationError::BadFragmentCount, selection.is_selected(0U, selected));
	EXPECT_TRUE(selected);
	selection.fragment_count = 1U;
	selection.bitmap_bytes = 1U;
	EXPECT_EQ(ValidationError::BadFragmentSlice, selection.is_selected(0U, selected));
	EXPECT_TRUE(selected);
	selection.all_fragments = false;
	selection.bitmap_bytes = 0U;
	EXPECT_EQ(ValidationError::BadFragmentSlice, selection.is_selected(0U, selected));
	EXPECT_TRUE(selected);
	selection.bitmap_bytes = 1U;
	selection.bitmap[0U] = 0x02U;
	EXPECT_EQ(ValidationError::BadFragmentSlice, selection.is_selected(0U, selected));
	EXPECT_TRUE(selected);
}

TEST(TelemetryProtocolReliableWindow, RetainRejectsInvalidIdentityFlagsContextAndFragmentMetadataAtomically)
{
	ReliableSendWindow window;
	const auto bytes = payload();
	const auto valid = message(ReliableMessageClass::SessionCritical, 5U, bytes);
	std::vector<ReliableMessageToRetain> invalid;

	auto candidate = valid;
	candidate.session_id = 0U;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.endpoint = EndpointKey{};
	invalid.push_back(candidate);
	candidate = valid;
	candidate.message_id = 0U;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.logical_payload = ByteView{nullptr, 1U};
	invalid.push_back(candidate);
	candidate = valid;
	candidate.message_type = MessageType::Invalid;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.message_type = MessageType::SessionEnd;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.required_ack = RequiredAckLevel::None;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.base_flags = MessageFlagNone;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.base_flags |= MessageFlagRetransmission;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.base_flags |= 0x80U;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.base_flags |= MessageFlagFragmented;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.base_flags |= MessageFlagKeyframe;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.frame_id = 1U;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.mission_time_us = 1;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.fragment_count += 1U;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.message_crc32 ^= 1U;
	invalid.push_back(candidate);
	candidate = valid;
	candidate.message_class = static_cast<ReliableMessageClass>(255U);
	invalid.push_back(candidate);

	std::uint8_t one = 1U;
	candidate = valid;
	candidate.logical_payload = ByteView{&one, std::numeric_limits<std::size_t>::max()};
	invalid.push_back(candidate);

	for (std::size_t index = 0; index < invalid.size(); ++index) {
		SCOPED_TRACE(index);
		EXPECT_EQ(ReliableRetainResult::InvalidMessage, window.retain(invalid[index], TestFirstSendUs));
		EXPECT_EQ(0U, window.entry_count());
		EXPECT_EQ(0U, window.retained_bytes());
	}
	EXPECT_EQ(invalid.size(), window.counters().invalid_rejected);
	EXPECT_EQ(0U, window.counters().retained);
}

TEST(TelemetryProtocolReliableWindow, FixedTypeCannotBeRetainedAsFragmentedEvenWithCanonicalSlices)
{
	const auto bytes = payload(MaxFragmentPayload + 1U);
	const auto fixed = message(ReliableMessageClass::SessionCritical, 6U, bytes);
	ASSERT_EQ(MessageType::SessionBegin, fixed.message_type);
	ASSERT_FALSE(message_type_allows_fragmentation(fixed.message_type));
	ASSERT_EQ(2U, fixed.fragment_count);
	ASSERT_TRUE((fixed.base_flags & MessageFlagFragmented) != 0);

	ReliableSendWindow window;
	EXPECT_EQ(ReliableRetainResult::InvalidMessage, window.retain(fixed, TestFirstSendUs));
	EXPECT_EQ(0U, window.entry_count());
	EXPECT_EQ(0U, window.retained_bytes());
}

TEST(TelemetryProtocolReliableWindow, EveryReliableClassRetainsWithItsExactWireIdentity)
{
	const std::array<ReliableMessageClass, 10> classes{{
		ReliableMessageClass::ControlDrop,
		ReliableMessageClass::ControlRequestKeyframe,
		ReliableMessageClass::ControlRequestResync,
		ReliableMessageClass::SessionCritical,
		ReliableMessageClass::SessionClosing,
		ReliableMessageClass::Transaction,
		ReliableMessageClass::ReliableEvent,
		ReliableMessageClass::VideoIdr,
		ReliableMessageClass::HandshakeCritical,
		ReliableMessageClass::HelloNegotiation,
	}};

	for (std::size_t index = 0; index < classes.size(); ++index) {
		SCOPED_TRACE(index);
		ReliableSendWindow window;
		const auto bytes = payload(9U, static_cast<std::uint8_t>(index));
		const auto retained = message(classes[index], static_cast<std::uint32_t>(index + 1U), bytes);
		ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));
		EXPECT_EQ(1U, window.entry_count());
		EXPECT_EQ(bytes.size(), window.retained_bytes());
		EXPECT_EQ(1U, window.counters().retained);
	}
}

TEST(TelemetryProtocolReliableWindow, HelloNegotiationIsTheOnlyPreSessionEntryAndUsesBackoffUntilDiscard)
{
	const auto bytes = payload(12U);
	const auto hello = message(ReliableMessageClass::HelloNegotiation, 17U, bytes);
	ASSERT_EQ(0U, hello.session_id);
	ASSERT_EQ(RequiredAckLevel::None, hello.required_ack);
	ASSERT_EQ(MessageFlagNone, hello.base_flags);

	ReliableSendWindow window;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(hello, TestFirstSendUs));
	EXPECT_EQ(ReliableRetainResult::Duplicate, window.retain(hello, TestFirstSendUs + 123U));
	auto illegal_hello = hello;
	illegal_hello.session_id = TestSessionId;
	EXPECT_EQ(ReliableRetainResult::InvalidMessage, ReliableSendWindow{}.retain(illegal_hello, TestFirstSendUs));
	auto illegal_session_message = message(ReliableMessageClass::SessionCritical, 18U, bytes);
	illegal_session_message.session_id = 0U;
	EXPECT_EQ(ReliableRetainResult::InvalidMessage,
		ReliableSendWindow{}.retain(illegal_session_message, TestFirstSendUs));

	const auto due =
		TestFirstSendUs +
		reliable_retry_delay_us(window.base_rto_us(), window.limits().jitter_seed, 0U, hello.message_id, 0U);
	ReliableWindowAction action;
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None, window.pull_next_action(due - 1U, action));
	expect_action_canary(action);
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(due, action));
	EXPECT_EQ(ReliableWindowActionKind::Retransmit, action.kind);
	expect_key(action.retransmission.key, hello);
	EXPECT_EQ(0U, action.retransmission.retry_number);
	EXPECT_EQ(TestFirstSendUs + ReliableOrdinaryRetentionUs, action.retransmission.absolute_deadline_us);
	EXPECT_EQ(bytes,
		std::vector<std::uint8_t>(action.retransmission.logical_payload.begin(),
			action.retransmission.logical_payload.end()));

	EXPECT_TRUE(window.discard(0U, hello.endpoint, hello.message_id));
	EXPECT_FALSE(window.discard(0U, hello.endpoint, hello.message_id));
	EXPECT_EQ(0U, window.entry_count());

	ReliableSendWindow expiring;
	ASSERT_EQ(ReliableRetainResult::Retained, expiring.retain(hello, TestFirstSendUs));
	ASSERT_EQ(ReliablePullResult::Action,
		expiring.pull_next_action(TestFirstSendUs + ReliableOrdinaryRetentionUs, action));
	EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
	EXPECT_EQ(ReliableTerminalPolicy::CloseSession, action.terminal_policy);
	expect_key(action.target, hello);
}

TEST(TelemetryProtocolReliableWindow, HelloNeverAcceptsAckOrNackCompletion)
{
	const auto bytes = payload(12U);
	const auto hello = message(ReliableMessageClass::HelloNegotiation, 19U, bytes);
	ReliableSendWindow window;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(hello, TestFirstSendUs));

	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.acknowledge(hello.session_id, hello.endpoint, ack_for(hello), TestFirstSendUs + 1U));
	ReliableNackDecision decision;
	seed_decision_canary(decision);
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.reject(hello.session_id,
			hello.endpoint,
			nack_for(hello, NackReason::BadMessageCrc),
			TestFirstSendUs + 2U,
			decision));
	expect_decision_canary(decision);
	EXPECT_EQ(1U, window.entry_count());
	EXPECT_EQ(1U, window.counters().ack_ignored_incoherent);
	EXPECT_EQ(1U, window.counters().nack_ignored_incoherent);
}

TEST(TelemetryProtocolReliableWindow, DuplicateConflictQuotaAndPayloadOwnershipNeverEvictAnEntry)
{
	ReliableWindowLimits limits;
	limits.max_entries = 3U;
	limits.max_retained_bytes = 12U;
	limits.jitter_seed = 41U;
	ReliableSendWindow window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(limits, window));

	auto source = payload(4U, 1U);
	const auto first = message(ReliableMessageClass::SessionCritical, 1U, source);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(first, TestFirstSendUs));
	const auto duplicate_bytes = payload(4U, 1U);
	const auto duplicate = message(ReliableMessageClass::SessionCritical, 1U, duplicate_bytes);
	EXPECT_EQ(ReliableRetainResult::Duplicate, window.retain(duplicate, TestFirstSendUs + 999U));
	const auto conflicting_bytes = payload(4U, 2U);
	const auto conflict = message(ReliableMessageClass::SessionCritical, 1U, conflicting_bytes);
	EXPECT_EQ(ReliableRetainResult::IdentityConflict, window.retain(conflict, TestFirstSendUs));
	auto different_context = first;
	different_context.frame_id = 0U;
	different_context.mission_time_us = 0;
	different_context.message_type = MessageType::CapabilityUpdate;
	EXPECT_EQ(ReliableRetainResult::IdentityConflict, window.retain(different_context, TestFirstSendUs));

	const auto same_id_other_endpoint =
		message(ReliableMessageClass::SessionCritical, 1U, source, TestSessionId, endpoint(2U));
	const auto same_id_other_session =
		message(ReliableMessageClass::SessionCritical, 1U, source, TestSessionId + 1U, endpoint());
	EXPECT_EQ(ReliableRetainResult::Retained, window.retain(same_id_other_endpoint, TestFirstSendUs));
	EXPECT_EQ(ReliableRetainResult::Retained, window.retain(same_id_other_session, TestFirstSendUs));
	EXPECT_EQ(3U, window.entry_count());
	EXPECT_EQ(12U, window.retained_bytes());

	const auto over_entry_and_bytes =
		message(ReliableMessageClass::SessionCritical, 2U, source, TestSessionId, endpoint());
	EXPECT_EQ(ReliableRetainResult::QuotaExceeded, window.retain(over_entry_and_bytes, TestFirstSendUs));
	EXPECT_EQ(3U, window.entry_count());
	EXPECT_EQ(12U, window.retained_bytes());
	EXPECT_EQ(1U, window.counters().quota_rejected);
	EXPECT_EQ(3U, window.counters().retained);

	// The window owns the retained bytes; mutating the caller's buffer cannot
	// alter the eventual retransmission payload.
	ASSERT_TRUE(window.discard(same_id_other_endpoint.session_id,
		same_id_other_endpoint.endpoint,
		same_id_other_endpoint.message_id));
	ASSERT_TRUE(window.discard(same_id_other_session.session_id,
		same_id_other_session.endpoint,
		same_id_other_session.message_id));
	source.assign(source.size(), 0xffU);
	const auto due =
		TestFirstSendUs +
		reliable_retry_delay_us(window.base_rto_us(), limits.jitter_seed, TestSessionId, first.message_id, 0U);
	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(due, action));
	expect_key(action.retransmission.key, first);
	EXPECT_EQ(duplicate_bytes,
		std::vector<std::uint8_t>(action.retransmission.logical_payload.begin(),
			action.retransmission.logical_payload.end()));
}

TEST(TelemetryProtocolReliableWindow, ByteAndEntryQuotasAreIndependentAndExact)
{
	const auto four_bytes = payload(4U);
	const auto one_byte = payload(1U);

	ReliableWindowLimits byte_limits;
	byte_limits.max_entries = 4U;
	byte_limits.max_retained_bytes = 4U;
	ReliableSendWindow byte_window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(byte_limits, byte_window));
	ASSERT_EQ(ReliableRetainResult::Retained,
		byte_window.retain(message(ReliableMessageClass::SessionCritical, 1U, four_bytes), 0U));
	EXPECT_EQ(ReliableRetainResult::QuotaExceeded,
		byte_window.retain(message(ReliableMessageClass::SessionCritical, 2U, one_byte), 0U));
	EXPECT_EQ(1U, byte_window.entry_count());
	EXPECT_EQ(4U, byte_window.retained_bytes());

	ReliableWindowLimits entry_limits;
	entry_limits.max_entries = 1U;
	entry_limits.max_retained_bytes = 8U;
	ReliableSendWindow entry_window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(entry_limits, entry_window));
	ASSERT_EQ(ReliableRetainResult::Retained,
		entry_window.retain(message(ReliableMessageClass::SessionCritical, 1U, one_byte), 0U));
	EXPECT_EQ(ReliableRetainResult::QuotaExceeded,
		entry_window.retain(message(ReliableMessageClass::SessionCritical, 2U, one_byte), 0U));
	EXPECT_EQ(1U, entry_window.entry_count());
	EXPECT_EQ(1U, entry_window.retained_bytes());
}

TEST(TelemetryProtocolReliableWindow, DedicatedLastIdrQuotaCannotConsumeOrEvictReliableCapacity)
{
	ReliableWindowLimits limits;
	limits.max_entries = 1U;
	limits.max_retained_bytes = 8U;
	ReliableSendWindow window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(limits, window));

	const auto state_bytes = payload(8U, 1U);
	const auto state = message(ReliableMessageClass::SessionCritical, 1U, state_bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(state, 0U));
	const auto video_bytes = payload(MaxFragmentPayload + 1U, 2U);
	const auto first_idr = message(ReliableMessageClass::VideoIdr, 2U, video_bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(first_idr, 0U));
	EXPECT_EQ(1U, window.reliable_entry_count());
	EXPECT_EQ(1U, window.video_entry_count());
	EXPECT_EQ(state_bytes.size(), window.reliable_retained_bytes());
	EXPECT_EQ(video_bytes.size(), window.video_retained_bytes());

	EXPECT_EQ(ReliableRetainResult::QuotaExceeded,
		window.retain(message(ReliableMessageClass::SessionCritical, 3U, payload(1U)), 0U));
	const auto replacement_bytes = payload(MaxFragmentPayload + 2U, 3U);
	const auto replacement = message(ReliableMessageClass::VideoIdr, 4U, replacement_bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(replacement, 1U));
	EXPECT_FALSE(window.discard(first_idr.session_id, first_idr.endpoint, first_idr.message_id));
	EXPECT_EQ(1U, window.reliable_entry_count());
	EXPECT_EQ(1U, window.video_entry_count());
	EXPECT_EQ(state_bytes.size(), window.reliable_retained_bytes());
	EXPECT_EQ(replacement_bytes.size(), window.video_retained_bytes());
	EXPECT_EQ(state_bytes.size() + replacement_bytes.size(), window.retained_bytes());
}

TEST(TelemetryProtocolReliableWindow, MaximumLogicalPayloadsAndClockBoundaryAreHandledWithoutWrap)
{
	ReliableWindowLimits limits;
	limits.max_entries = 2U;
	limits.max_retained_bytes = MaxStateMessageSize + MaxVideoMessageSize;
	ReliableSendWindow window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(limits, window));

	const auto maximum_state = payload(MaxStateMessageSize, 3U);
	const auto state = message(ReliableMessageClass::Transaction, 1U, maximum_state);
	ASSERT_GT(state.fragment_count, 1U);
	EXPECT_EQ(ReliableRetainResult::Retained, window.retain(state, 0U));
	const auto maximum_video = payload(MaxVideoMessageSize, 4U);
	const auto video = message(ReliableMessageClass::VideoIdr, 2U, maximum_video);
	ASSERT_GT(video.fragment_count, 1U);
	EXPECT_EQ(ReliableRetainResult::Retained, window.retain(video, 0U));
	EXPECT_EQ(MaxStateMessageSize + MaxVideoMessageSize, window.retained_bytes());

	window.clear();
	const auto too_large = payload(MaxStateMessageSize + 1U, 5U);
	EXPECT_EQ(ReliableRetainResult::InvalidMessage,
		window.retain(message(ReliableMessageClass::Transaction, 3U, too_large), 0U));
	EXPECT_EQ(0U, window.entry_count());

	const auto small = payload(1U);
	const auto ordinary = message(ReliableMessageClass::SessionCritical, 4U, small);
	const auto last_valid_start = std::numeric_limits<std::uint64_t>::max() - ReliableOrdinaryRetentionUs;
	EXPECT_EQ(ReliableRetainResult::Retained, window.retain(ordinary, last_valid_start));
	window.clear();
	EXPECT_EQ(ReliableRetainResult::ClockOverflow, window.retain(ordinary, last_valid_start + 1U));
	EXPECT_EQ(0U, window.entry_count());
	EXPECT_EQ(0U, window.retained_bytes());
}

TEST(TelemetryProtocolReliableWindow, ValidatedAndAppliedAcknowledgementsReleaseAtTheRequiredLevel)
{
	ReliableSendWindow window;
	const auto bytes = payload();
	const auto validated_target = message(ReliableMessageClass::ControlRequestResync, 1U, bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(validated_target, TestFirstSendUs));
	EXPECT_EQ(ReliableResponseResult::Released,
		window.acknowledge(validated_target.session_id,
			validated_target.endpoint,
			ack_for(validated_target),
			TestFirstSendUs + 1U));
	EXPECT_EQ(0U, window.entry_count());

	const auto applied_target = message(ReliableMessageClass::SessionCritical, 2U, bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(applied_target, TestFirstSendUs));
	EXPECT_EQ(ReliableResponseResult::ValidatedRetained,
		window.acknowledge(applied_target.session_id,
			applied_target.endpoint,
			ack_for(applied_target),
			TestFirstSendUs + 1U));
	EXPECT_EQ(ReliableResponseResult::Duplicate,
		window.acknowledge(applied_target.session_id,
			applied_target.endpoint,
			ack_for(applied_target),
			TestFirstSendUs + 2U));
	ReliableWindowAction action;
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None,
		window.pull_next_action(TestFirstSendUs + ReliableOrdinaryRetentionUs - 1U, action));
	expect_action_canary(action);
	EXPECT_EQ(ReliableResponseResult::Released,
		window.acknowledge(applied_target.session_id,
			applied_target.endpoint,
			ack_for(applied_target, KnownAckFlags),
			TestFirstSendUs + ReliableOrdinaryRetentionUs - 1U));
	EXPECT_EQ(0U, window.entry_count());

	const auto direct_applied = message(ReliableMessageClass::ReliableEvent, 3U, bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(direct_applied, TestFirstSendUs));
	EXPECT_EQ(ReliableResponseResult::Released,
		window.acknowledge(direct_applied.session_id,
			direct_applied.endpoint,
			ack_for(direct_applied, KnownAckFlags),
			TestFirstSendUs + 1U));
	// Every structurally valid ACK datagram is counted, including the duplicate
	// VALIDATED response above; the state transition itself remains idempotent.
	EXPECT_EQ(5U, window.counters().ack_validated);
	EXPECT_EQ(2U, window.counters().ack_applied);
}

TEST(TelemetryProtocolReliableWindow, LostAckRetransmitsButLateUnknownAndIncoherentAcksNeverMutateIdentity)
{
	ReliableSendWindow window;
	const auto bytes = payload();
	const auto retained = message(ReliableMessageClass::SessionCritical, 7U, bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));
	const auto due = TestFirstSendUs + reliable_retry_delay_us(window.base_rto_us(),
										   window.limits().jitter_seed,
										   retained.session_id,
										   retained.message_id,
										   0U);
	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(due, action));
	EXPECT_EQ(ReliableWindowActionKind::Retransmit, action.kind);
	EXPECT_EQ(0U, action.retransmission.retry_number);
	expect_key(action.retransmission.key, retained);

	auto incoherent = ack_for(retained);
	incoherent.target_message_crc32 ^= 1U;
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.acknowledge(retained.session_id, retained.endpoint, incoherent, due + 1U));
	incoherent = ack_for(retained);
	incoherent.target_message_type = MessageType::CapabilityUpdate;
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.acknowledge(retained.session_id, retained.endpoint, incoherent, due + 1U));
	incoherent = ack_for(retained);
	incoherent.target_fragment_count += 1U;
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.acknowledge(retained.session_id, retained.endpoint, incoherent, due + 1U));
	incoherent = ack_for(retained);
	incoherent.ack_flags = 0U;
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.acknowledge(retained.session_id, retained.endpoint, incoherent, due + 1U));
	EXPECT_EQ(1U, window.entry_count());

	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		window.acknowledge(retained.session_id + 1U, retained.endpoint, ack_for(retained), due + 1U));
	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		window.acknowledge(retained.session_id, endpoint(2U), ack_for(retained), due + 1U));
	auto unknown = ack_for(retained);
	unknown.target_message_id += 1U;
	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		window.acknowledge(retained.session_id, retained.endpoint, unknown, due + 1U));
	EXPECT_EQ(1U, window.entry_count());

	const auto deadline = TestFirstSendUs + ReliableOrdinaryRetentionUs;
	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		window.acknowledge(retained.session_id, retained.endpoint, ack_for(retained, KnownAckFlags), deadline));
	EXPECT_EQ(1U, window.entry_count());
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(deadline, action));
	EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
	EXPECT_EQ(ReliableTerminalPolicy::MarkSessionStale, action.terminal_policy);
	EXPECT_EQ(4U, window.counters().ack_ignored_incoherent);
	EXPECT_EQ(4U, window.counters().ack_ignored_unknown_or_late);
}

TEST(TelemetryProtocolReliableWindow, ValidatedAckCancelsPendingNackAndAllFutureRetriesUntilApplied)
{
	ReliableSendWindow window;
	const auto bytes = payload(MaxFragmentPayload + 1U);
	const auto retained = message(ReliableMessageClass::Transaction, 9U, bytes);
	ASSERT_EQ(2U, retained.fragment_count);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));
	const std::array<std::uint8_t, 1> bitmap{{0x02U}};
	ReliableNackDecision decision;
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.reject(retained.session_id,
			retained.endpoint,
			nack_for(retained, NackReason::MissingFragments, byte_view(bitmap)),
			TestFirstSendUs + 1U,
			decision));
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.acknowledge(retained.session_id, retained.endpoint, ack_for(retained), TestFirstSendUs + 2U));
	ReliableWindowAction action;
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None,
		window.pull_next_action(TestFirstSendUs + ReliableTransactionRetentionUs - 1U, action));
	expect_action_canary(action);
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.reject(retained.session_id,
			retained.endpoint,
			nack_for(retained, NackReason::MissingFragments, byte_view(bitmap)),
			TestFirstSendUs + 3U,
			decision));
	EXPECT_EQ(ReliableResponseResult::Released,
		window.acknowledge(retained.session_id,
			retained.endpoint,
			ack_for(retained, KnownAckFlags),
			TestFirstSendUs + 4U));
}

TEST(TelemetryProtocolReliableWindow, TransactionPartsShareFirstDeadlineAndRejectConcurrentCandidate)
{
	const auto bytes = payload(32U);
	auto first = message(ReliableMessageClass::Transaction, 101U, bytes);
	auto second = message(ReliableMessageClass::Transaction, 102U, bytes);
	second.transaction_id = first.transaction_id;
	second.transaction_sha256 = first.transaction_sha256;

	ReliableSendWindow window;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(first, TestFirstSendUs));
	const auto second_send_us = TestFirstSendUs + ReliableTransactionRetentionUs / 2U;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(second, second_send_us));
	EXPECT_EQ(2U, window.entry_count());

	const auto concurrent = message(ReliableMessageClass::Transaction, 103U, bytes);
	EXPECT_EQ(ReliableRetainResult::TransactionBusy, window.retain(concurrent, second_send_us + 1U));
	auto contradictory_hash = message(ReliableMessageClass::Transaction, 104U, bytes);
	contradictory_hash.transaction_id = first.transaction_id;
	contradictory_hash.transaction_sha256 = first.transaction_sha256;
	contradictory_hash.transaction_sha256[0U] ^= 0xffU;
	EXPECT_EQ(ReliableRetainResult::IdentityConflict, window.retain(contradictory_hash, second_send_us + 1U));

	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action,
		window.pull_next_action(TestFirstSendUs + ReliableTransactionRetentionUs, action));
	EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
	EXPECT_EQ(ReliableTerminalPolicy::RequestResync, action.terminal_policy);
	EXPECT_EQ(first.transaction_id, action.target.transaction_id);
	EXPECT_EQ(first.transaction_sha256, action.target.transaction_sha256);
	EXPECT_EQ(0U, window.entry_count());
	EXPECT_EQ(0U, window.retained_bytes());
	EXPECT_EQ(2U, window.counters().expirations);
}

TEST(TelemetryProtocolReliableWindow, TerminalNackAfterValidatedPurgesWholeTransactionGroup)
{
	const auto bytes = payload(24U);
	auto first = message(ReliableMessageClass::Transaction, 111U, bytes);
	auto second = message(ReliableMessageClass::Transaction, 112U, bytes);
	second.transaction_id = first.transaction_id;
	second.transaction_sha256 = first.transaction_sha256;

	ReliableSendWindow window;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(first, TestFirstSendUs));
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(second, TestFirstSendUs + 1U));
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.acknowledge(first.session_id, first.endpoint, ack_for(first), TestFirstSendUs + 2U));

	ReliableNackDecision decision;
	ASSERT_EQ(ReliableResponseResult::Released,
		window.reject(first.session_id,
			first.endpoint,
			nack_for(first, NackReason::SemanticValidationFailed),
			TestFirstSendUs + 3U,
			decision));
	EXPECT_EQ(ReliableNackDecisionKind::TerminalPolicy, decision.kind);
	EXPECT_EQ(ReliableTerminalPolicy::RequestResync, decision.terminal_policy);
	expect_key(decision.target, first);
	EXPECT_EQ(0U, window.entry_count());
	EXPECT_EQ(0U, window.retained_bytes());
	EXPECT_EQ(1U, window.counters().nack_accepted);
}

TEST(TelemetryProtocolReliableWindow, SelectiveNackRetransmitsExactBitmapWithoutMovingRtoOrBackoff)
{
	ReliableWindowLimits limits;
	limits.max_entries = 4U;
	limits.max_retained_bytes = MaxCandidateTransactionBytesPerClient;
	limits.jitter_seed = 0x123456789abcdef0ULL;
	ReliableSendWindow window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(limits, window));
	const auto bytes = payload(MaxFragmentPayload + 10U);
	const auto retained = message(ReliableMessageClass::Transaction, 11U, bytes);
	ASSERT_EQ(2U, retained.fragment_count);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));
	const std::array<std::uint8_t, 1> bitmap{{0x02U}};
	ReliableNackDecision decision;
	seed_decision_canary(decision);
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.reject(retained.session_id,
			retained.endpoint,
			nack_for(retained, NackReason::MissingFragments, byte_view(bitmap)),
			TestFirstSendUs + 1U,
			decision));
	EXPECT_EQ(ReliableNackDecisionKind::SelectiveRetransmissionScheduled, decision.kind);
	expect_key(decision.target, retained);

	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(TestFirstSendUs + 1U, action));
	ASSERT_EQ(ReliableWindowActionKind::Retransmit, action.kind);
	expect_key(action.retransmission.key, retained);
	EXPECT_EQ(retained.base_flags, action.retransmission.base_flags);
	EXPECT_EQ(retained.frame_id, action.retransmission.frame_id);
	EXPECT_EQ(retained.mission_time_us, action.retransmission.mission_time_us);
	EXPECT_EQ(0U, action.retransmission.retry_number);
	EXPECT_FALSE(action.retransmission.fragments.all_fragments);
	EXPECT_EQ(1U, action.retransmission.fragments.bitmap_bytes);
	bool selected = true;
	ASSERT_EQ(ValidationError::None, action.retransmission.fragments.is_selected(0U, selected));
	EXPECT_FALSE(selected);
	ASSERT_EQ(ValidationError::None, action.retransmission.fragments.is_selected(1U, selected));
	EXPECT_TRUE(selected);

	const auto original_rto =
		TestFirstSendUs +
		reliable_retry_delay_us(window.base_rto_us(), limits.jitter_seed, retained.session_id, retained.message_id, 0U);
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None, window.pull_next_action(original_rto - 1U, action));
	expect_action_canary(action);
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(original_rto, action));
	EXPECT_TRUE(action.retransmission.fragments.all_fragments);
	EXPECT_EQ(0U, action.retransmission.retry_number);
	const auto second_rto =
		original_rto +
		reliable_retry_delay_us(window.base_rto_us(), limits.jitter_seed, retained.session_id, retained.message_id, 1U);
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None, window.pull_next_action(second_rto - 1U, action));
	expect_action_canary(action);
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(second_rto, action));
	EXPECT_EQ(1U, action.retransmission.retry_number);
	EXPECT_EQ(3U, window.counters().retransmissions);
}

TEST(TelemetryProtocolReliableWindow, NackValidationAndLatenessPreserveDecisionAndRetainedEntry)
{
	ReliableSendWindow window;
	const auto bytes = payload();
	const auto retained = message(ReliableMessageClass::SessionCritical, 12U, bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));
	ReliableNackDecision decision;

	auto malformed = nack_for(retained, NackReason::Invalid);
	seed_decision_canary(decision);
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.reject(retained.session_id, retained.endpoint, malformed, TestFirstSendUs + 1U, decision));
	expect_decision_canary(decision);
	auto mismatched = nack_for(retained, NackReason::BadMessageCrc);
	mismatched.target_message_crc32 ^= 1U;
	seed_decision_canary(decision);
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.reject(retained.session_id, retained.endpoint, mismatched, TestFirstSendUs + 1U, decision));
	expect_decision_canary(decision);
	seed_decision_canary(decision);
	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		window.reject(retained.session_id + 1U,
			retained.endpoint,
			nack_for(retained, NackReason::BadMessageCrc),
			TestFirstSendUs + 1U,
			decision));
	expect_decision_canary(decision);
	seed_decision_canary(decision);
	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		window.reject(retained.session_id,
			endpoint(2U),
			nack_for(retained, NackReason::BadMessageCrc),
			TestFirstSendUs + 1U,
			decision));
	expect_decision_canary(decision);
	seed_decision_canary(decision);
	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		window.reject(retained.session_id,
			retained.endpoint,
			nack_for(retained, NackReason::BadMessageCrc),
			TestFirstSendUs + ReliableOrdinaryRetentionUs,
			decision));
	expect_decision_canary(decision);
	EXPECT_EQ(1U, window.entry_count());
	EXPECT_EQ(2U, window.counters().nack_ignored_incoherent);
	EXPECT_EQ(3U, window.counters().nack_ignored_unknown_or_late);
}

TEST(TelemetryProtocolReliableWindow, NackErrorReasonsChooseFullWaitOrTerminalPolicyWithoutExtendingDeadline)
{
	const auto bytes = payload();
	const auto retained = message(ReliableMessageClass::SessionCritical, 13U, bytes);
	ReliableSendWindow window;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));
	ReliableNackDecision decision;
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.reject(retained.session_id,
			retained.endpoint,
			nack_for(retained, NackReason::BadMessageCrc),
			TestFirstSendUs + 1U,
			decision));
	EXPECT_EQ(ReliableNackDecisionKind::FullRetransmissionScheduled, decision.kind);
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.reject(retained.session_id,
			retained.endpoint,
			nack_for(retained, NackReason::ResourceLimit),
			TestFirstSendUs + 2U,
			decision));
	EXPECT_EQ(ReliableNackDecisionKind::WaitForScheduledRetry, decision.kind);
	ReliableWindowAction action;
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None, window.pull_next_action(TestFirstSendUs + 2U, action));
	expect_action_canary(action);
	const auto original_rto = TestFirstSendUs + reliable_retry_delay_us(window.base_rto_us(),
													window.limits().jitter_seed,
													retained.session_id,
													retained.message_id,
													0U);
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(original_rto, action));
	EXPECT_TRUE(action.retransmission.fragments.all_fragments);
	EXPECT_EQ(0U, action.retransmission.retry_number);
	EXPECT_EQ(TestFirstSendUs + ReliableOrdinaryRetentionUs, action.retransmission.absolute_deadline_us);

	const auto terminal = message(ReliableMessageClass::SessionCritical, 14U, bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(terminal, TestFirstSendUs));
	ASSERT_EQ(ReliableResponseResult::Released,
		window.reject(terminal.session_id,
			terminal.endpoint,
			nack_for(terminal, NackReason::SemanticValidationFailed),
			TestFirstSendUs + 1U,
			decision));
	EXPECT_EQ(ReliableNackDecisionKind::TerminalPolicy, decision.kind);
	EXPECT_EQ(ReliableTerminalPolicy::MarkSessionStale, decision.terminal_policy);
	expect_key(decision.target, terminal);
	EXPECT_EQ(1U, window.entry_count());
}

TEST(TelemetryProtocolReliableWindow, TerminalNacksUseEveryClassSpecificRecoveryPolicy)
{
	struct Case {
		ReliableMessageClass message_class;
		ReliableTerminalPolicy policy;
	};
	const std::array<Case, 8> cases{{
		{ReliableMessageClass::ControlDrop, ReliableTerminalPolicy::Drop},
		{ReliableMessageClass::ControlRequestKeyframe, ReliableTerminalPolicy::RequestKeyframe},
		{ReliableMessageClass::ControlRequestResync, ReliableTerminalPolicy::RequestResync},
		{ReliableMessageClass::SessionCritical, ReliableTerminalPolicy::MarkSessionStale},
		{ReliableMessageClass::SessionClosing, ReliableTerminalPolicy::CloseSession},
		{ReliableMessageClass::Transaction, ReliableTerminalPolicy::RequestResync},
		{ReliableMessageClass::ReliableEvent, ReliableTerminalPolicy::RequestResync},
		{ReliableMessageClass::HandshakeCritical, ReliableTerminalPolicy::CloseSession},
	}};
	const auto bytes = payload();
	for (std::size_t index = 0; index < cases.size(); ++index) {
		SCOPED_TRACE(index);
		ReliableSendWindow window;
		const auto retained = message(cases[index].message_class, static_cast<std::uint32_t>(index + 1U), bytes);
		ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));
		ReliableNackDecision decision;
		ASSERT_EQ(ReliableResponseResult::Released,
			window.reject(retained.session_id,
				retained.endpoint,
				nack_for(retained, NackReason::SemanticValidationFailed),
				TestFirstSendUs + 1U,
				decision));
		EXPECT_EQ(ReliableNackDecisionKind::TerminalPolicy, decision.kind);
		EXPECT_EQ(cases[index].policy, decision.terminal_policy);
		expect_key(decision.target, retained);
		EXPECT_EQ(0U, window.entry_count());
	}
}

TEST(TelemetryProtocolReliableWindow, VideoIdrHasNoAutonomousRetryAndHonorsBothDeadlines)
{
	ReliableSendWindow window;
	const auto bytes = payload(MaxFragmentPayload + 1U);
	const auto idr = message(ReliableMessageClass::VideoIdr, 21U, bytes);
	ASSERT_EQ(2U, idr.fragment_count);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(idr, TestFirstSendUs));
	ReliableWindowAction action;
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None,
		window.pull_next_action(TestFirstSendUs + ReliableVideoIdrRetentionUs - 1U, action));
	expect_action_canary(action);

	const std::array<std::uint8_t, 1> bitmap{{0x01U}};
	ReliableNackDecision decision;
	const auto obsolete_at = TestFirstSendUs + 200'000U;
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.reject(idr.session_id,
			idr.endpoint,
			nack_for(idr, NackReason::MissingFragments, byte_view(bitmap), obsolete_at),
			TestFirstSendUs + 1U,
			decision));
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None, window.pull_next_action(obsolete_at, action));
	expect_action_canary(action);
	EXPECT_EQ(1U, window.entry_count());

	seed_decision_canary(decision);
	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		window.reject(idr.session_id,
			idr.endpoint,
			nack_for(idr, NackReason::MissingFragments, byte_view(bitmap), obsolete_at),
			obsolete_at,
			decision));
	expect_decision_canary(decision);

	const auto attempted_extension = TestFirstSendUs + 400'000U;
	seed_decision_canary(decision);
	EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
		window.reject(idr.session_id,
			idr.endpoint,
			nack_for(idr, NackReason::MissingFragments, byte_view(bitmap), attempted_extension),
			obsolete_at + 1U,
			decision));
	expect_decision_canary(decision);
	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None, window.pull_next_action(obsolete_at + 1U, action));
	expect_action_canary(action);

	seed_action_canary(action);
	EXPECT_EQ(ReliablePullResult::None,
		window.pull_next_action(TestFirstSendUs + ReliableVideoIdrRetentionUs - 1U, action));
	expect_action_canary(action);
	ASSERT_EQ(ReliablePullResult::Action,
		window.pull_next_action(TestFirstSendUs + ReliableVideoIdrRetentionUs, action));
	EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
	EXPECT_EQ(ReliableTerminalPolicy::Drop, action.terminal_policy);
	expect_key(action.target, idr);
}

TEST(TelemetryProtocolReliableWindow, VideoIdrRejectsEveryNonSelectiveErrorNackWithoutMutation)
{
	ReliableSendWindow window;
	const auto bytes = payload(MaxFragmentPayload + 1U);
	const auto idr = message(ReliableMessageClass::VideoIdr, 22U, bytes);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(idr, TestFirstSendUs));
	const std::array<NackReason, 6> reasons{{
		NackReason::BadMessageCrc,
		NackReason::StaleBaseline,
		NackReason::BadFragmentLayout,
		NackReason::ResourceLimit,
		NackReason::SemanticValidationFailed,
		NackReason::DeadlineExpired,
	}};
	const auto poison_deadline = TestFirstSendUs + 100'000U;
	for (std::size_t index = 0; index < reasons.size(); ++index) {
		SCOPED_TRACE(index);
		ReliableNackDecision decision;
		seed_decision_canary(decision);
		EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent,
			window.reject(idr.session_id,
				idr.endpoint,
				nack_for(idr, reasons[index], {}, index == 0U ? poison_deadline : 0U),
				TestFirstSendUs + index + 1U,
				decision));
		expect_decision_canary(decision);
		EXPECT_EQ(1U, window.entry_count());
	}
	EXPECT_EQ(0U, window.counters().nack_accepted);
	EXPECT_EQ(reasons.size(), window.counters().nack_ignored_incoherent);

	const std::array<std::uint8_t, 1> bitmap{{0x01U}};
	const auto useful_deadline = TestFirstSendUs + 400'000U;
	ReliableNackDecision decision;
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.reject(idr.session_id,
			idr.endpoint,
			nack_for(idr, NackReason::MissingFragments, byte_view(bitmap), useful_deadline),
			TestFirstSendUs + reasons.size() + 1U,
			decision));
	EXPECT_EQ(ReliableNackDecisionKind::SelectiveRetransmissionScheduled, decision.kind);
	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(TestFirstSendUs + reasons.size() + 1U, action));
	EXPECT_EQ(ReliableWindowActionKind::Retransmit, action.kind);
	EXPECT_FALSE(action.retransmission.fragments.all_fragments);
	EXPECT_EQ(1U, window.counters().nack_accepted);
}

TEST(TelemetryProtocolReliableWindow, SchedulerHonorsControlStateThenVideoPriorityEvenForImmediateNack)
{
	ReliableSendWindow window;
	const auto small = payload();
	const auto video_bytes = payload(MaxFragmentPayload + 1U);
	const auto video = message(ReliableMessageClass::VideoIdr, 1U, video_bytes);
	const auto event = message(ReliableMessageClass::ReliableEvent, 2U, small);
	const auto transaction = message(ReliableMessageClass::Transaction, 3U, small);
	const auto control = message(ReliableMessageClass::SessionCritical, 4U, small);
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(video, 0U));
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(event, 0U));
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(transaction, 0U));
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(control, 0U));
	const std::array<std::uint8_t, 1> bitmap{{0x01U}};
	ReliableNackDecision decision;
	constexpr std::uint64_t now = 300'000U;
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		window.reject(video.session_id,
			video.endpoint,
			nack_for(video, NackReason::MissingFragments, byte_view(bitmap)),
			now,
			decision));

	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(now, action));
	EXPECT_EQ(control.message_id, action.target.message_id);
	std::array<std::uint32_t, 2> state_ids{};
	for (auto& id : state_ids) {
		ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(now, action));
		id = action.target.message_id;
		EXPECT_TRUE(id == event.message_id || id == transaction.message_id);
	}
	EXPECT_NE(state_ids[0U], state_ids[1U]);
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(now, action));
	EXPECT_EQ(video.message_id, action.target.message_id);
	EXPECT_FALSE(action.retransmission.fragments.all_fragments);
}

TEST(TelemetryProtocolReliableWindow, ExpiredVideoNeverPreemptsReadyControlRetransmission)
{
	const auto video_bytes = payload(MaxFragmentPayload + 1U);
	const auto control_bytes = payload();
	const auto video = message(ReliableMessageClass::VideoIdr, 31U, video_bytes);
	const auto control = message(ReliableMessageClass::SessionCritical, 32U, control_bytes);
	ReliableSendWindow window;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(video, 0U));
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(control, 0U));

	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(ReliableVideoIdrRetentionUs, action));
	EXPECT_EQ(ReliableWindowActionKind::Retransmit, action.kind);
	expect_key(action.target, control);
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(ReliableVideoIdrRetentionUs, action));
	EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
	EXPECT_EQ(ReliableTerminalPolicy::Drop, action.terminal_policy);
	expect_key(action.target, video);
}

TEST(TelemetryProtocolReliableWindow, EveryClassExpiresAtItsImmutableDeadlineWithDeclaredPolicy)
{
	struct Case {
		ReliableMessageClass message_class;
		std::uint64_t retention_us;
		ReliableTerminalPolicy policy;
	};
	const std::array<Case, 10> cases{{
		{ReliableMessageClass::ControlDrop, ReliableOrdinaryRetentionUs, ReliableTerminalPolicy::Drop},
		{ReliableMessageClass::ControlRequestKeyframe,
			ReliableOrdinaryRetentionUs,
			ReliableTerminalPolicy::RequestKeyframe},
		{ReliableMessageClass::ControlRequestResync,
			ReliableOrdinaryRetentionUs,
			ReliableTerminalPolicy::RequestResync},
		{ReliableMessageClass::SessionCritical, ReliableOrdinaryRetentionUs, ReliableTerminalPolicy::MarkSessionStale},
		{ReliableMessageClass::SessionClosing, ReliableOrdinaryRetentionUs, ReliableTerminalPolicy::CloseSession},
		{ReliableMessageClass::Transaction, ReliableTransactionRetentionUs, ReliableTerminalPolicy::RequestResync},
		{ReliableMessageClass::ReliableEvent, ReliableOrdinaryRetentionUs, ReliableTerminalPolicy::RequestResync},
		{ReliableMessageClass::VideoIdr, ReliableVideoIdrRetentionUs, ReliableTerminalPolicy::Drop},
		{ReliableMessageClass::HandshakeCritical, ReliableOrdinaryRetentionUs, ReliableTerminalPolicy::CloseSession},
		{ReliableMessageClass::HelloNegotiation, ReliableOrdinaryRetentionUs, ReliableTerminalPolicy::CloseSession},
	}};
	const auto bytes = payload();
	for (std::size_t index = 0; index < cases.size(); ++index) {
		SCOPED_TRACE(index);
		ReliableSendWindow window;
		const auto retained = message(cases[index].message_class, static_cast<std::uint32_t>(index + 1U), bytes);
		ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, TestFirstSendUs));
		ReliableWindowAction action;
		ASSERT_EQ(ReliablePullResult::Action,
			window.pull_next_action(TestFirstSendUs + cases[index].retention_us, action));
		EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
		EXPECT_EQ(cases[index].policy, action.terminal_policy);
		expect_key(action.target, retained);
		EXPECT_EQ(0U, window.entry_count());
		EXPECT_EQ(0U, window.retained_bytes());
		EXPECT_EQ(1U, window.counters().expirations);
	}
}

TEST(TelemetryProtocolReliableWindow, DiscardSessionClearAndMovesReleaseExactResources)
{
	ReliableWindowLimits limits;
	limits.max_entries = 4U;
	limits.max_retained_bytes = 32U;
	limits.jitter_seed = 123U;
	ReliableSendWindow source;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(limits, source));
	const auto bytes = payload(4U);
	const auto first = message(ReliableMessageClass::SessionCritical, 1U, bytes);
	const auto second = message(ReliableMessageClass::SessionCritical, 2U, bytes, TestSessionId, endpoint(2U));
	const auto third = message(ReliableMessageClass::SessionCritical, 3U, bytes, TestSessionId + 1U, endpoint(3U));
	ASSERT_EQ(ReliableRetainResult::Retained, source.retain(first, 0U));
	ASSERT_EQ(ReliableRetainResult::Retained, source.retain(second, 0U));
	ASSERT_EQ(ReliableRetainResult::Retained, source.retain(third, 0U));
	EXPECT_FALSE(source.discard(first.session_id, endpoint(9U), first.message_id));
	EXPECT_FALSE(source.discard(first.session_id, first.endpoint, 99U));
	EXPECT_TRUE(source.discard(first.session_id, first.endpoint, first.message_id));
	EXPECT_EQ(2U, source.entry_count());
	EXPECT_EQ(8U, source.retained_bytes());

	ReliableSendWindow moved(std::move(source));
	EXPECT_EQ(2U, moved.entry_count());
	EXPECT_EQ(8U, moved.retained_bytes());
	EXPECT_EQ(limits.max_entries, moved.limits().max_entries);
	EXPECT_EQ(0U, source.entry_count());
	EXPECT_EQ(0U, source.retained_bytes());
	EXPECT_EQ(ReliableWindowDefaultEntries, source.limits().max_entries);
	EXPECT_EQ(1U, moved.discard_session(TestSessionId));
	EXPECT_EQ(1U, moved.entry_count());
	EXPECT_EQ(0U, moved.discard_session(TestSessionId));

	ReliableSendWindow assigned;
	assigned = std::move(moved);
	EXPECT_EQ(1U, assigned.entry_count());
	EXPECT_EQ(4U, assigned.retained_bytes());
	EXPECT_EQ(0U, moved.entry_count());
	assigned = std::move(assigned);
	EXPECT_EQ(1U, assigned.entry_count());
	assigned.clear();
	EXPECT_EQ(0U, assigned.entry_count());
	EXPECT_EQ(0U, assigned.retained_bytes());
	EXPECT_EQ(3U, assigned.counters().retained);
}

} // namespace
