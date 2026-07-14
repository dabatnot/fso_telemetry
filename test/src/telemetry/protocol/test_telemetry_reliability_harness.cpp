#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliable_receive.h"
#include "telemetry/protocol/telemetry_reliable_window.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

EndpointKey harness_endpoint()
{
	return EndpointKey::from_ipv4({127U, 0U, 0U, 42U}, 7808U);
}

std::vector<std::uint8_t> harness_payload(std::size_t size)
{
	std::vector<std::uint8_t> payload(size);
	for (std::size_t index = 0; index < payload.size(); ++index) {
		payload[index] = static_cast<std::uint8_t>(0x31U + index * 29U);
	}
	return payload;
}

struct HarnessFragment {
	TelemetryDatagramHeader header;
	std::vector<std::uint8_t> payload;

	DatagramView view() const noexcept
	{
		return DatagramView{header, byte_view(payload)};
	}
};

std::vector<HarnessFragment>
make_snapshot_fragments(const std::vector<std::uint8_t>& payload, std::uint64_t session_id, std::uint32_t message_id)
{
	TelemetryFragmenter fragmenter;
	EXPECT_TRUE(TelemetryFragmenter::create(byte_view(payload), MessageSizeClass::State, fragmenter));

	std::vector<HarnessFragment> fragments;
	for (std::uint16_t index = 0; index < fragmenter.fragment_count(); ++index) {
		FragmentSlice slice;
		EXPECT_TRUE(fragmenter.fragment(index, slice));
		HarnessFragment fragment;
		fragment.header.message_type = MessageType::FullSnapshot;
		fragment.header.flags =
			static_cast<std::uint8_t>(MessageFlagAckRequired | MessageFlagKeyframe |
									  (slice.fragment_count > 1 ? MessageFlagFragmented : MessageFlagNone));
		fragment.header.session_id = session_id;
		fragment.header.packet_sequence = 1000U + index;
		fragment.header.frame_id = 77U;
		fragment.header.mission_time_us = 500'000;
		fragment.header.sent_time_us = 1'000'000U + index;
		fragment.header.message_id = message_id;
		fragment.header.fragment_index = slice.fragment_index;
		fragment.header.fragment_count = slice.fragment_count;
		fragment.header.message_size = slice.message_size;
		fragment.header.fragment_offset = slice.fragment_offset;
		fragment.header.message_crc32 = slice.message_crc32;
		fragment.header.payload_size = static_cast<std::uint16_t>(slice.payload.size);
		fragment.payload.assign(slice.payload.begin(), slice.payload.end());
		fragments.emplace_back(std::move(fragment));
	}
	return fragments;
}

std::vector<std::uint8_t> encode_retransmitted_fragment(const ReliableRetransmissionView& retransmission,
	std::uint16_t fragment_index,
	std::uint32_t packet_sequence,
	std::uint64_t sent_time_us)
{
	TelemetryFragmenter fragmenter;
	EXPECT_TRUE(TelemetryFragmenter::create(retransmission.logical_payload,
		message_size_class(retransmission.key.message_type),
		fragmenter));
	FragmentSlice slice;
	EXPECT_TRUE(fragmenter.fragment(fragment_index, slice));

	TelemetryDatagramHeader header;
	header.message_type = retransmission.key.message_type;
	header.flags =
		static_cast<std::uint8_t>(retransmission.base_flags | static_cast<std::uint8_t>(MessageFlagRetransmission));
	header.session_id = retransmission.key.session_id;
	header.packet_sequence = packet_sequence;
	header.frame_id = retransmission.frame_id;
	header.mission_time_us = retransmission.mission_time_us;
	header.sent_time_us = sent_time_us;
	header.message_id = retransmission.key.message_id;
	header.fragment_index = slice.fragment_index;
	header.fragment_count = slice.fragment_count;
	header.message_size = slice.message_size;
	header.fragment_offset = slice.fragment_offset;
	header.message_crc32 = slice.message_crc32;

	std::vector<std::uint8_t> wire(MaxDatagramSize);
	std::size_t written = 0;
	EXPECT_EQ(ValidationError::None,
		encode_datagram(header, slice.payload, MutableByteView{wire.data(), wire.size()}, written));
	wire.resize(written);
	return wire;
}

TEST(TelemetryProtocolReliabilityHarness, MissingFragmentAndLostAppliedAckConvergeWithoutRepublishing)
{
	constexpr std::uint64_t SessionId = 0x1122334455667788ULL;
	constexpr std::uint32_t MessageId = 91U;
	constexpr std::uint64_t FirstSendUs = 0U;
	constexpr std::uint64_t GapObservedUs = 1'000U;
	const auto endpoint = harness_endpoint();
	const auto payload = harness_payload(2'500U);
	const auto fragments = make_snapshot_fragments(payload, SessionId, MessageId);
	ASSERT_EQ(3U, fragments.size());

	ReliableWindowLimits limits;
	limits.jitter_seed = 0x0123456789abcdefULL;
	ReliableSendWindow send_window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(limits, send_window));

	ReliableMessageToRetain retained;
	retained.session_id = SessionId;
	retained.endpoint = endpoint;
	retained.message_type = MessageType::FullSnapshot;
	retained.base_flags = fragments.front().header.flags;
	retained.frame_id = fragments.front().header.frame_id;
	retained.mission_time_us = fragments.front().header.mission_time_us;
	retained.message_id = MessageId;
	retained.fragment_count = fragments.front().header.fragment_count;
	retained.message_crc32 = fragments.front().header.message_crc32;
	retained.logical_payload = byte_view(payload);
	retained.required_ack = RequiredAckLevel::Applied;
	retained.message_class = ReliableMessageClass::Transaction;
	retained.transaction_id = 0x31415926U;
	retained.transaction_sha256.fill(0x5aU);
	ASSERT_EQ(ReliableRetainResult::Retained, send_window.retain(retained, FirstSendUs));

	ReliableReceivePipeline receive_pipeline;
	ReassembledMessage completed;
	const auto first_result = receive_pipeline.ingest_validated_fragment(fragments[0].view(),
		endpoint,
		FirstSendUs,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::Accepted, first_result.tracking);
	EXPECT_EQ(ReassemblyResult::Accepted, first_result.reassembly);
	const auto gap_result = receive_pipeline.ingest_validated_fragment(fragments[2].view(),
		endpoint,
		GapObservedUs,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::Accepted, gap_result.tracking);
	EXPECT_EQ(ReassemblyResult::Accepted, gap_result.reassembly);

	SelectiveNackAction selective_action;
	EXPECT_EQ(SelectiveNackPollResult::None, receive_pipeline.poll(25'999U, 0U, selective_action));
	ASSERT_EQ(SelectiveNackPollResult::Ready, receive_pipeline.poll(26'000U, 0U, selective_action));
	ASSERT_EQ(SelectiveNackActionKind::SendNack, selective_action.kind);
	std::array<std::uint8_t, MaxNackBitmapBytes> nack_bitmap{};
	ASSERT_LE(selective_action.nack.missing_bitmap.size, nack_bitmap.size());
	std::copy(selective_action.nack.missing_bitmap.begin(),
		selective_action.nack.missing_bitmap.end(),
		nack_bitmap.begin());
	auto nack = selective_action.nack;
	nack.missing_bitmap = ByteView{nack_bitmap.data(), selective_action.nack.missing_bitmap.size};
	ASSERT_EQ(NackReason::MissingFragments, nack.reason);
	bool missing = false;
	ASSERT_EQ(ValidationError::None, is_fragment_missing(nack.missing_bitmap, nack.target_fragment_count, 1U, missing));
	EXPECT_TRUE(missing);
	ASSERT_EQ(ValidationError::None, is_fragment_missing(nack.missing_bitmap, nack.target_fragment_count, 0U, missing));
	EXPECT_FALSE(missing);
	ASSERT_EQ(ValidationError::None, is_fragment_missing(nack.missing_bitmap, nack.target_fragment_count, 2U, missing));
	EXPECT_FALSE(missing);

	ReliableNackDecision nack_decision;
	ASSERT_EQ(ReliableResponseResult::ValidatedRetained,
		send_window.reject(SessionId, endpoint, nack, 26'000U, nack_decision));
	EXPECT_EQ(ReliableNackDecisionKind::SelectiveRetransmissionScheduled, nack_decision.kind);

	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, send_window.pull_next_action(26'000U, action));
	ASSERT_EQ(ReliableWindowActionKind::Retransmit, action.kind);
	EXPECT_FALSE(action.retransmission.fragments.all_fragments);
	ASSERT_EQ(ValidationError::None, action.retransmission.fragments.is_selected(1U, missing));
	EXPECT_TRUE(missing);
	ASSERT_EQ(ValidationError::None, action.retransmission.fragments.is_selected(0U, missing));
	EXPECT_FALSE(missing);

	const auto retransmitted_wire = encode_retransmitted_fragment(action.retransmission, 1U, 2'001U, 26'001U);
	DatagramView retransmitted_fragment;
	ASSERT_EQ(ValidationError::None,
		decode_and_validate_datagram(byte_view(retransmitted_wire), retransmitted_fragment));
	EXPECT_NE(0U,
		static_cast<std::uint8_t>(
			retransmitted_fragment.header.flags & static_cast<std::uint8_t>(MessageFlagRetransmission)));
	EXPECT_EQ(fragments[1].payload.size(), retransmitted_fragment.payload.size);
	const auto completed_result = receive_pipeline.ingest_validated_fragment(retransmitted_fragment,
		endpoint,
		26'001U,
		ReliableDefaultRtoUs,
		completed);
	EXPECT_EQ(SelectiveNackObserveResult::Completed, completed_result.tracking);
	ASSERT_EQ(ReassemblyResult::Completed, completed_result.reassembly);
	ASSERT_EQ(payload, completed.payload);

	ReliableReceiveCache receive_cache(8U);
	const ReliableReceiveKey receive_key{SessionId,
		endpoint,
		MessageType::FullSnapshot,
		MessageId,
		retained.message_crc32,
		retained.fragment_count};
	std::uint32_t publish_count = 0U;
	const auto publish_completed = [&](const ReassembledMessage& message) {
		if (message.header.session_id != receive_key.session_id ||
			message.header.message_type != receive_key.message_type ||
			message.header.message_id != receive_key.message_id ||
			message.header.message_crc32 != receive_key.message_crc32 ||
			message.header.fragment_count != receive_key.fragment_count || message.payload != payload) {
			return false;
		}
		ReliableReceiveOutcome cached_outcome;
		if (receive_cache.reserve(receive_key, ReliableMessageClass::Transaction, 26'001U, cached_outcome) !=
				ReliableReceiveReserveResult::Reserved ||
			receive_cache.record_validated(receive_key) != ReliableReceiveUpdateResult::Updated ||
			receive_cache.record_applied(receive_key) != ReliableReceiveUpdateResult::Updated) {
			return false;
		}
		++publish_count;
		return true;
	};
	ASSERT_TRUE(publish_completed(completed));
	ASSERT_EQ(1U, publish_count);

	AckPayload applied_ack;
	ASSERT_EQ(ValidationError::None, receive_cache.cached_ack(receive_key, 26'001U, applied_ack));
	// The first APPLIED ACK is deliberately lost. The original RTO remains
	// anchored at FirstSendUs despite the earlier NACK-triggered retransmission.
	const auto original_rto =
		reliable_retry_delay_us(send_window.base_rto_us(), limits.jitter_seed, SessionId, MessageId, 0U);
	ASSERT_GT(original_rto, 26'000U);
	EXPECT_EQ(ReliablePullResult::None, send_window.pull_next_action(original_rto - 1U, action));
	ASSERT_EQ(ReliablePullResult::Action, send_window.pull_next_action(original_rto, action));
	ASSERT_EQ(ReliableWindowActionKind::Retransmit, action.kind);
	EXPECT_TRUE(action.retransmission.fragments.all_fragments);

	// The first retransmitted fragment carries the exact logical identity. The
	// receive cache intercepts it before a second reassembly or publication and
	// supplies the cached APPLIED ACK byte-for-byte.
	const auto duplicate_wire = encode_retransmitted_fragment(action.retransmission, 0U, 3'001U, original_rto);
	DatagramView duplicate_fragment;
	ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(duplicate_wire), duplicate_fragment));
	EXPECT_NE(0U,
		static_cast<std::uint8_t>(
			duplicate_fragment.header.flags & static_cast<std::uint8_t>(MessageFlagRetransmission)));
	const ReliableReceiveKey duplicate_key{duplicate_fragment.header.session_id,
		endpoint,
		duplicate_fragment.header.message_type,
		duplicate_fragment.header.message_id,
		duplicate_fragment.header.message_crc32,
		duplicate_fragment.header.fragment_count};
	ReliableReceiveOutcome duplicate_outcome;
	ASSERT_EQ(ReliableReceiveReserveResult::Duplicate,
		receive_cache.reserve(duplicate_key, ReliableMessageClass::Transaction, original_rto, duplicate_outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Applied, duplicate_outcome.kind);
	EXPECT_EQ(0U, receive_pipeline.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(1U, publish_count);
	AckPayload replayed_ack;
	ASSERT_EQ(ValidationError::None, receive_cache.cached_ack(duplicate_key, original_rto, replayed_ack));
	EXPECT_EQ(applied_ack.target_message_id, replayed_ack.target_message_id);
	EXPECT_EQ(applied_ack.target_message_type, replayed_ack.target_message_type);
	EXPECT_EQ(applied_ack.ack_flags, replayed_ack.ack_flags);
	EXPECT_EQ(applied_ack.target_fragment_count, replayed_ack.target_fragment_count);
	EXPECT_EQ(applied_ack.target_message_crc32, replayed_ack.target_message_crc32);
	ASSERT_EQ(ReliableResponseResult::Released,
		send_window.acknowledge(SessionId, endpoint, replayed_ack, original_rto));
	EXPECT_EQ(0U, send_window.entry_count());

	ReliableNackDecision late_decision;
	EXPECT_EQ(ReliableResponseResult::IgnoredUnknownOrLate,
		send_window.reject(SessionId, endpoint, nack, original_rto + 1U, late_decision));
	EXPECT_EQ(1U, publish_count);
}

TEST(TelemetryProtocolReliabilityHarness, ExpirationProducesRecoveryInsteadOfBlocking)
{
	constexpr std::uint64_t SessionId = 42U;
	constexpr std::uint32_t MessageId = 7U;
	const auto endpoint = harness_endpoint();
	const auto payload = harness_payload(64U);
	TelemetryFragmenter fragmenter;
	ASSERT_TRUE(TelemetryFragmenter::create(byte_view(payload), MessageSizeClass::State, fragmenter));

	ReliableSendWindow window;
	ReliableMessageToRetain retained;
	retained.session_id = SessionId;
	retained.endpoint = endpoint;
	retained.message_type = MessageType::EventBatch;
	retained.base_flags = MessageFlagAckRequired;
	retained.frame_id = 1U;
	retained.mission_time_us = 0;
	retained.message_id = MessageId;
	retained.fragment_count = fragmenter.fragment_count();
	retained.message_crc32 = fragmenter.message_crc32();
	retained.logical_payload = byte_view(payload);
	retained.required_ack = RequiredAckLevel::Applied;
	retained.message_class = ReliableMessageClass::ReliableEvent;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, 100U));

	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(100U + ReliableOrdinaryRetentionUs, action));
	EXPECT_EQ(ReliableWindowActionKind::TerminalPolicy, action.kind);
	EXPECT_EQ(ReliableTerminalPolicy::RequestResync, action.terminal_policy);
	EXPECT_EQ(MessageId, action.target.message_id);
	EXPECT_EQ(0U, window.entry_count());
	EXPECT_EQ(ReliablePullResult::None, window.pull_next_action(100U + ReliableOrdinaryRetentionUs + 1U, action));
}

} // namespace
