#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_reliable_window.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace {

using namespace telemetry::protocol;

constexpr std::uint64_t Session = 0x1122334455667788ULL;
constexpr std::uint64_t FirstSendUs = 10'000U;

EndpointKey peer()
{
	return EndpointKey::from_ipv4({192U, 0U, 2U, 1U}, 7808U);
}

struct Target {
	std::array<std::uint8_t, 16U> bytes{};
	ReliableMessageToRetain retained;
};

Target target(ReliableMessageClass message_class, std::uint32_t message_id)
{
	Target value;
	for (std::size_t i = 0U; i < value.bytes.size(); ++i) {
		value.bytes[i] = static_cast<std::uint8_t>(0x31U + i * 7U);
	}
	value.retained.session_id = Session;
	value.retained.endpoint = peer();
	value.retained.message_type = message_class == ReliableMessageClass::HandshakeCritical
		? MessageType::Welcome
		: MessageType::SessionBegin;
	value.retained.base_flags = MessageFlagAckRequired;
	value.retained.message_id = message_id;
	value.retained.fragment_count = 1U;
	value.retained.message_crc32 = crc32_iso_hdlc({value.bytes.data(), value.bytes.size()});
	value.retained.logical_payload = {value.bytes.data(), value.bytes.size()};
	value.retained.required_ack = RequiredAckLevel::Applied;
	value.retained.message_class = message_class;
	return value;
}

AckPayload ack(const ReliableMessageToRetain& retained, std::uint8_t flags)
{
	AckPayload value;
	value.target_message_id = retained.message_id;
	value.target_message_type = retained.message_type;
	value.ack_flags = flags;
	value.target_fragment_count = retained.fragment_count;
	value.target_message_crc32 = retained.message_crc32;
	return value;
}

NackPayload nack(const ReliableMessageToRetain& retained, NackReason reason, ByteView bitmap = {})
{
	NackPayload value;
	value.target_message_id = retained.message_id;
	value.target_message_type = retained.message_type;
	value.reason = reason;
	value.target_fragment_count = retained.fragment_count;
	value.target_message_crc32 = retained.message_crc32;
	value.missing_bitmap = bitmap;
	return value;
}

void expect_key_equal(const ReliableMessageKey& fixed, const ReliableMessageKey& general)
{
	EXPECT_EQ(general.session_id, fixed.session_id);
	EXPECT_EQ(general.endpoint, fixed.endpoint);
	EXPECT_EQ(general.message_type, fixed.message_type);
	EXPECT_EQ(general.message_id, fixed.message_id);
	EXPECT_EQ(general.fragment_count, fixed.fragment_count);
	EXPECT_EQ(general.message_crc32, fixed.message_crc32);
	EXPECT_EQ(general.transaction_id, fixed.transaction_id);
	EXPECT_EQ(general.transaction_sha256, fixed.transaction_sha256);
}

void expect_decision_equal(const ReliableNackDecision& fixed, const ReliableNackDecision& general)
{
	EXPECT_EQ(general.kind, fixed.kind);
	EXPECT_EQ(general.terminal_policy, fixed.terminal_policy);
	expect_key_equal(fixed.target, general.target);
}

void expect_action_equal(const ReliableWindowAction& fixed, const ReliableWindowAction& general)
{
	EXPECT_EQ(general.kind, fixed.kind);
	EXPECT_EQ(general.terminal_policy, fixed.terminal_policy);
	expect_key_equal(fixed.target, general.target);
	expect_key_equal(fixed.retransmission.key, general.retransmission.key);
	EXPECT_EQ(general.retransmission.base_flags, fixed.retransmission.base_flags);
	EXPECT_EQ(general.retransmission.frame_id, fixed.retransmission.frame_id);
	EXPECT_EQ(general.retransmission.mission_time_us, fixed.retransmission.mission_time_us);
	EXPECT_EQ(general.retransmission.fragments.all_fragments, fixed.retransmission.fragments.all_fragments);
	EXPECT_EQ(general.retransmission.fragments.fragment_count, fixed.retransmission.fragments.fragment_count);
	EXPECT_EQ(general.retransmission.fragments.bitmap_bytes, fixed.retransmission.fragments.bitmap_bytes);
	EXPECT_EQ(general.retransmission.fragments.bitmap, fixed.retransmission.fragments.bitmap);
	EXPECT_EQ(general.retransmission.retry_number, fixed.retransmission.retry_number);
	EXPECT_EQ(general.retransmission.absolute_deadline_us, fixed.retransmission.absolute_deadline_us);
	ASSERT_EQ(general.retransmission.logical_payload.size, fixed.retransmission.logical_payload.size);
	EXPECT_TRUE(std::equal(fixed.retransmission.logical_payload.begin(),
		fixed.retransmission.logical_payload.end(),
		general.retransmission.logical_payload.begin()));
}

TEST(TelemetryProtocolPreallocatedReliableParity, ValidatedAckIsRememberedAsDuplicateAndStopsRetriesUntilApplied)
{
	for (const auto message_class :
		{ReliableMessageClass::HandshakeCritical, ReliableMessageClass::SessionCritical}) {
		SCOPED_TRACE(static_cast<int>(message_class));
		auto value = target(message_class, static_cast<std::uint32_t>(message_class));
		value.retained.logical_payload = {value.bytes.data(), value.bytes.size()};
		PreallocatedReliableControlWindow fixed;
		fixed.configure();
		ReliableSendWindow general;
		ASSERT_EQ(ReliableRetainResult::Retained, fixed.retain(value.retained, FirstSendUs));
		ASSERT_EQ(ReliableRetainResult::Retained, general.retain(value.retained, FirstSendUs));
		const auto validated = ack(value.retained, static_cast<std::uint8_t>(AckFlag::Validated));
		EXPECT_EQ(ReliableResponseResult::ValidatedRetained,
			fixed.acknowledge(Session, peer(), validated, FirstSendUs + 1U));
		EXPECT_EQ(ReliableResponseResult::ValidatedRetained,
			general.acknowledge(Session, peer(), validated, FirstSendUs + 1U));
		EXPECT_EQ(ReliableResponseResult::Duplicate,
			fixed.acknowledge(Session, peer(), validated, FirstSendUs + 2U));
		EXPECT_EQ(ReliableResponseResult::Duplicate,
			general.acknowledge(Session, peer(), validated, FirstSendUs + 2U));
		ReliableWindowAction fixed_action;
		ReliableWindowAction general_action;
		EXPECT_EQ(ReliablePullResult::None,
			fixed.pull_next_action(FirstSendUs + ReliableOrdinaryRetentionUs - 1U, fixed_action));
		EXPECT_EQ(ReliablePullResult::None,
			general.pull_next_action(FirstSendUs + ReliableOrdinaryRetentionUs - 1U, general_action));
		EXPECT_EQ(ReliableResponseResult::Released,
			fixed.acknowledge(Session, peer(), ack(value.retained, KnownAckFlags), FirstSendUs + 3U));
		EXPECT_EQ(0U, fixed.entry_count());
	}
}

TEST(TelemetryProtocolPreallocatedReliableParity, RecoverableNacksSelectMissingFullOrOriginalScheduledRetry)
{
	for (const auto reason :
		{NackReason::MissingFragments, NackReason::BadMessageCrc, NackReason::BadFragmentLayout, NackReason::ResourceLimit}) {
		SCOPED_TRACE(static_cast<int>(reason));
		auto value = target(ReliableMessageClass::SessionCritical, 21U + static_cast<std::uint32_t>(reason));
		value.retained.logical_payload = {value.bytes.data(), value.bytes.size()};
		PreallocatedReliableControlWindow fixed;
		fixed.configure();
		ASSERT_EQ(ReliableRetainResult::Retained, fixed.retain(value.retained, FirstSendUs));
		const std::array<std::uint8_t, 1U> bitmap{{1U}};
		ReliableNackDecision decision;
		const auto response = fixed.reject(Session,
			peer(),
			nack(value.retained,
				reason,
				reason == NackReason::MissingFragments ? ByteView{bitmap.data(), bitmap.size()} : ByteView{}),
			FirstSendUs + 1U,
			decision);
		ASSERT_EQ(ReliableResponseResult::ValidatedRetained, response);
		const auto expected = reason == NackReason::MissingFragments
			? ReliableNackDecisionKind::SelectiveRetransmissionScheduled
			: (reason == NackReason::ResourceLimit ? ReliableNackDecisionKind::WaitForScheduledRetry
										 : ReliableNackDecisionKind::FullRetransmissionScheduled);
		EXPECT_EQ(expected, decision.kind);
		ReliableWindowAction action;
		if (reason == NackReason::ResourceLimit) {
			EXPECT_EQ(ReliablePullResult::None, fixed.pull_next_action(FirstSendUs + 1U, action));
			const auto original_due = FirstSendUs + reliable_retry_delay_us(ReliableDefaultRtoUs,
				0x4653544c5f52544fULL,
				Session,
				value.retained.message_id,
				0U);
			EXPECT_EQ(ReliablePullResult::Action, fixed.pull_next_action(original_due, action));
		} else {
			ASSERT_EQ(ReliablePullResult::Action, fixed.pull_next_action(FirstSendUs + 1U, action));
			EXPECT_EQ(reason != NackReason::MissingFragments, action.retransmission.fragments.all_fragments);
		}
	}
}

TEST(TelemetryProtocolPreallocatedReliableParity, TerminalNacksReleaseWithClassSpecificPolicy)
{
	struct Case {
		ReliableMessageClass message_class;
		ReliableTerminalPolicy policy;
	};
	for (const auto& item : {Case{ReliableMessageClass::HandshakeCritical, ReliableTerminalPolicy::CloseSession},
			 Case{ReliableMessageClass::SessionCritical, ReliableTerminalPolicy::MarkSessionStale}}) {
		for (const auto reason :
			{NackReason::StaleBaseline, NackReason::SemanticValidationFailed, NackReason::DeadlineExpired}) {
			SCOPED_TRACE(static_cast<int>(item.message_class));
			SCOPED_TRACE(static_cast<int>(reason));
			auto value = target(item.message_class, 40U + static_cast<std::uint32_t>(reason));
			value.retained.logical_payload = {value.bytes.data(), value.bytes.size()};
			PreallocatedReliableControlWindow fixed;
			fixed.configure();
			ASSERT_EQ(ReliableRetainResult::Retained, fixed.retain(value.retained, FirstSendUs));
			ReliableNackDecision decision;
			EXPECT_EQ(ReliableResponseResult::Released,
				fixed.reject(Session, peer(), nack(value.retained, reason), FirstSendUs + 1U, decision));
			EXPECT_EQ(ReliableNackDecisionKind::TerminalPolicy, decision.kind);
			EXPECT_EQ(item.policy, decision.terminal_policy);
			EXPECT_EQ(0U, fixed.entry_count());
		}
	}
}

TEST(TelemetryProtocolPreallocatedReliableParity, EveryNackReasonMatchesGeneralWindowIncludingKnownTupleUnsupported)
{
	const std::array<NackReason, 9U> reasons{{NackReason::Invalid,
		NackReason::MissingFragments,
		NackReason::BadMessageCrc,
		NackReason::StaleBaseline,
		NackReason::BadFragmentLayout,
		NackReason::ResourceLimit,
		NackReason::UnsupportedMessage,
		NackReason::SemanticValidationFailed,
		NackReason::DeadlineExpired}};
	for (const auto message_class :
		{ReliableMessageClass::HandshakeCritical, ReliableMessageClass::SessionCritical}) {
		for (const auto reason : reasons) {
			SCOPED_TRACE(static_cast<int>(message_class));
			SCOPED_TRACE(static_cast<int>(reason));
			auto value = target(message_class,
				100U + static_cast<std::uint32_t>(message_class) * 16U + static_cast<std::uint32_t>(reason));
			value.retained.logical_payload = {value.bytes.data(), value.bytes.size()};
			PreallocatedReliableControlWindow fixed;
			fixed.configure();
			ReliableSendWindow general;
			ASSERT_EQ(ReliableRetainResult::Retained, fixed.retain(value.retained, FirstSendUs));
			ASSERT_EQ(ReliableRetainResult::Retained, general.retain(value.retained, FirstSendUs));
			const std::array<std::uint8_t, 1U> bitmap{{1U}};
			const auto response = nack(value.retained,
				reason,
				reason == NackReason::MissingFragments ? ByteView{bitmap.data(), bitmap.size()} : ByteView{});
			ReliableNackDecision fixed_decision;
			ReliableNackDecision general_decision;
			const auto fixed_response =
				fixed.reject(Session, peer(), response, FirstSendUs + 1U, fixed_decision);
			const auto general_response =
				general.reject(Session, peer(), response, FirstSendUs + 1U, general_decision);
			EXPECT_EQ(general_response, fixed_response);
			expect_decision_equal(fixed_decision, general_decision);
			EXPECT_EQ(general.entry_count(), fixed.entry_count());
			ReliableWindowAction fixed_action;
			ReliableWindowAction general_action;
			const auto fixed_pull = fixed.pull_next_action(FirstSendUs + 1U, fixed_action);
			const auto general_pull = general.pull_next_action(FirstSendUs + 1U, general_action);
			EXPECT_EQ(general_pull, fixed_pull);
			expect_action_equal(fixed_action, general_action);
			EXPECT_EQ(general.entry_count(), fixed.entry_count());
			if (reason == NackReason::UnsupportedMessage) {
				EXPECT_EQ(ReliableResponseResult::IgnoredIncoherent, general_response);
				EXPECT_EQ(1U, general.entry_count());
				EXPECT_EQ(ReliablePullResult::None, general_pull);
			}
		}
	}
}

TEST(TelemetryProtocolPreallocatedReliableParity, DuplicateRetainComparesTheCompleteIdentity)
{
	using Mutation = std::pair<const char*, std::function<void(ReliableMessageToRetain&)>>;
	const std::vector<Mutation> mutations{
		{"base_flags", [](auto& message) { message.base_flags ^= MessageFlagAckRequired; }},
		{"frame_id", [](auto& message) { message.frame_id = 1U; }},
		{"mission_time", [](auto& message) { message.mission_time_us = 1; }},
		{"fragment_count", [](auto& message) { ++message.fragment_count; }},
		{"transaction_metadata", [](auto& message) {
			 message.transaction_id = 1U;
			 message.transaction_sha256.fill(0x5aU);
		 }},
		{"required_ack", [](auto& message) { message.required_ack = RequiredAckLevel::Validated; }},
		{"message_class", [](auto& message) {
			 message.message_class = message.message_class == ReliableMessageClass::HandshakeCritical
				 ? ReliableMessageClass::SessionCritical
				 : ReliableMessageClass::HandshakeCritical;
		 }},
	};
	for (const auto message_class :
		{ReliableMessageClass::HandshakeCritical, ReliableMessageClass::SessionCritical}) {
		for (const auto& mutation : mutations) {
			SCOPED_TRACE(static_cast<int>(message_class));
			SCOPED_TRACE(mutation.first);
			auto value = target(message_class, 300U + static_cast<std::uint32_t>(message_class));
			value.retained.logical_payload = {value.bytes.data(), value.bytes.size()};
			PreallocatedReliableControlWindow fixed;
			fixed.configure();
			ReliableSendWindow general;
			ASSERT_EQ(ReliableRetainResult::Retained, fixed.retain(value.retained, FirstSendUs));
			ASSERT_EQ(ReliableRetainResult::Retained, general.retain(value.retained, FirstSendUs));
			EXPECT_EQ(ReliableRetainResult::Duplicate, fixed.retain(value.retained, FirstSendUs + 1U));
			EXPECT_EQ(ReliableRetainResult::Duplicate, general.retain(value.retained, FirstSendUs + 1U));
			auto conflict = value.retained;
			mutation.second(conflict);
			const auto general_result = general.retain(conflict, FirstSendUs + 2U);
			const auto fixed_result = fixed.retain(conflict, FirstSendUs + 2U);
			EXPECT_NE(ReliableRetainResult::Duplicate, general_result);
			EXPECT_EQ(general_result, fixed_result);
			EXPECT_EQ(1U, general.entry_count());
			EXPECT_EQ(general.entry_count(), fixed.entry_count());
		}
	}
}

TEST(TelemetryProtocolPreallocatedReliableParity, BackoffPreservesDeadlineAndExactExpiryEmitsTerminalThenReleases)
{
	for (const auto& item : {std::pair{ReliableMessageClass::HandshakeCritical, ReliableTerminalPolicy::CloseSession},
			 std::pair{ReliableMessageClass::SessionCritical, ReliableTerminalPolicy::MarkSessionStale}}) {
		auto value = target(item.first, 70U + static_cast<std::uint32_t>(item.first));
		value.retained.logical_payload = {value.bytes.data(), value.bytes.size()};
		PreallocatedReliableControlWindow fixed;
		fixed.configure();
		ASSERT_EQ(ReliableRetainResult::Retained, fixed.retain(value.retained, FirstSendUs));
		const auto first_due = FirstSendUs + reliable_retry_delay_us(ReliableDefaultRtoUs,
			0x4653544c5f52544fULL,
			Session,
			value.retained.message_id,
			0U);
		ReliableWindowAction action;
		ASSERT_EQ(ReliablePullResult::Action, fixed.pull_next_action(first_due, action));
		EXPECT_EQ(0U, action.retransmission.retry_number);
		EXPECT_EQ(FirstSendUs + ReliableOrdinaryRetentionUs, action.retransmission.absolute_deadline_us);
		ASSERT_EQ(ReliablePullResult::Action,
			fixed.pull_next_action(FirstSendUs + ReliableOrdinaryRetentionUs, action));
		EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
		EXPECT_EQ(item.second, action.terminal_policy);
		EXPECT_EQ(0U, fixed.entry_count());
		EXPECT_EQ(0U, fixed.retained_bytes());
	}
}

} // namespace
