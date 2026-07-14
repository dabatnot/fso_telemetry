#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/telemetry_reassembler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

std::vector<std::uint8_t> message_bytes(std::size_t size, std::uint8_t seed = 0x31U)
{
	std::vector<std::uint8_t> result(size);
	for (std::size_t index = 0; index < size; ++index) {
		result[index] = static_cast<std::uint8_t>(seed + index * 37U);
	}
	return result;
}

struct OwnedFragment {
	TelemetryDatagramHeader header;
	std::vector<std::uint8_t> payload;

	DatagramView view() const
	{
		return DatagramView{header, byte_view(payload)};
	}
};

std::vector<OwnedFragment> make_fragments(const std::vector<std::uint8_t>& message,
	MessageType type = MessageType::Delta,
	std::uint32_t message_id = 100U,
	std::uint64_t session_id = 42U)
{
	TelemetryFragmenter fragmenter;
	EXPECT_TRUE(TelemetryFragmenter::create(byte_view(message), message_size_class(type), fragmenter));
	std::vector<OwnedFragment> result;
	for (std::size_t index = 0; index < fragmenter.fragment_count(); ++index) {
		FragmentSlice slice;
		EXPECT_TRUE(fragmenter.fragment(index, slice));
		OwnedFragment owned;
		owned.header.message_type = type;
		owned.header.flags = slice.fragment_count > 1U ? MessageFlagFragmented : MessageFlagNone;
		if (type == MessageType::Manifest || type == MessageType::FullSnapshot) {
			owned.header.flags |= MessageFlagAckRequired;
		}
		if (type == MessageType::FullSnapshot) {
			owned.header.flags |= MessageFlagKeyframe;
		}
		owned.header.session_id = session_id;
		owned.header.packet_sequence = static_cast<std::uint32_t>(1000U + index);
		owned.header.frame_id = type == MessageType::Manifest ? 0U : 77U;
		owned.header.mission_time_us = type == MessageType::Manifest ? 0 : -123456;
		owned.header.sent_time_us = 900000U + index;
		owned.header.message_id = message_id;
		owned.header.fragment_index = slice.fragment_index;
		owned.header.fragment_count = slice.fragment_count;
		owned.header.message_size = slice.message_size;
		owned.header.fragment_offset = slice.fragment_offset;
		owned.header.message_crc32 = slice.message_crc32;
		owned.header.payload_size = static_cast<std::uint16_t>(slice.payload.size);
		owned.payload.assign(slice.payload.begin(), slice.payload.end());
		result.emplace_back(std::move(owned));
	}
	return result;
}

OwnedFragment make_first_fragment(MessageType type,
	std::uint32_t message_size,
	std::uint32_t message_id,
	std::uint64_t session_id = 42U)
{
	OwnedFragment result;
	result.header.message_type = type;
	result.header.session_id = session_id;
	result.header.message_id = message_id;
	result.header.message_size = message_size;
	result.header.message_crc32 = 0x12345678U;
	EXPECT_EQ(ValidationError::None, expected_fragment_count(type, message_size, result.header.fragment_count));
	result.header.flags = result.header.fragment_count > 1U ? MessageFlagFragmented : MessageFlagNone;
	result.payload.assign(std::min<std::size_t>(message_size, MaxFragmentPayload), 0x5aU);
	result.header.payload_size = static_cast<std::uint16_t>(result.payload.size());
	return result;
}

ReassembledMessage sentinel_message()
{
	ReassembledMessage result;
	result.header.message_id = 0xfeedbeefU;
	result.message_class = MessageSizeClass::Video;
	result.payload = {0x5aU};
	return result;
}

void expect_sentinel(const ReassembledMessage& message)
{
	EXPECT_EQ(0xfeedbeefU, message.header.message_id);
	EXPECT_EQ(MessageSizeClass::Video, message.message_class);
	EXPECT_EQ((std::vector<std::uint8_t>{0x5aU}), message.payload);
}

TEST(TelemetryProtocolFragmenter, CanonicalSlicesCoverEmptyExactAndFinalRemainderBoundaries)
{
	struct Case {
		std::size_t size;
		std::uint16_t count;
	};
	const std::array<Case, 6> cases{{{0U, 1U}, {1U, 1U}, {1132U, 1U}, {1133U, 2U}, {2264U, 2U}, {2265U, 3U}}};

	for (const auto value : cases) {
		SCOPED_TRACE(testing::Message() << "message size " << value.size);
		const auto message = message_bytes(value.size);
		TelemetryFragmenter fragmenter;
		ASSERT_TRUE(TelemetryFragmenter::create(byte_view(message), MessageSizeClass::State, fragmenter));
		EXPECT_TRUE(fragmenter.ok());
		EXPECT_EQ(value.size, fragmenter.message_size());
		EXPECT_EQ(value.count, fragmenter.fragment_count());
		if (value.size == 0U) {
			EXPECT_EQ(0U, fragmenter.message_crc32());
		}

		std::vector<std::uint8_t> reconstructed;
		for (std::size_t index = 0; index < value.count; ++index) {
			FragmentSlice slice;
			ASSERT_TRUE(fragmenter.fragment(index, slice));
			EXPECT_EQ(index, slice.fragment_index);
			EXPECT_EQ(value.count, slice.fragment_count);
			EXPECT_EQ(index * MaxFragmentPayload, slice.fragment_offset);
			const auto expected_size =
				value.size == 0U ? 0U : std::min(MaxFragmentPayload, value.size - index * MaxFragmentPayload);
			EXPECT_EQ(expected_size, slice.payload.size);
			if (index + 1U < value.count) {
				EXPECT_EQ(MaxFragmentPayload, slice.payload.size);
			} else if (value.size != 0U) {
				EXPECT_GT(slice.payload.size, 0U);
			}
			reconstructed.insert(reconstructed.end(), slice.payload.begin(), slice.payload.end());
		}
		EXPECT_EQ(message, reconstructed);
	}
}

TEST(TelemetryProtocolFragmenter, UsesLogicalCrcAndRejectsInvalidClassBoundsOverflowAndIndex)
{
	const std::array<std::uint8_t, 9> check{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	TelemetryFragmenter fragmenter;
	ASSERT_TRUE(TelemetryFragmenter::create(byte_view(check), MessageSizeClass::State, fragmenter));
	EXPECT_EQ(0xcbf43926U, fragmenter.message_crc32());

	FragmentSlice sentinel;
	sentinel.fragment_index = 99U;
	sentinel.payload = byte_view(check);
	EXPECT_FALSE(fragmenter.fragment(fragmenter.fragment_count(), sentinel));
	EXPECT_EQ(0U, sentinel.fragment_index);
	EXPECT_EQ(nullptr, sentinel.payload.data);

	const std::uint8_t byte = 0U;
	EXPECT_FALSE(TelemetryFragmenter::create(ByteView{nullptr, 1U}, MessageSizeClass::State, fragmenter));
	EXPECT_FALSE(fragmenter.ok());
	EXPECT_FALSE(
		TelemetryFragmenter::create(ByteView{&byte, MaxStateMessageSize + 1U}, MessageSizeClass::State, fragmenter));
	EXPECT_FALSE(
		TelemetryFragmenter::create(ByteView{&byte, MaxVideoMessageSize + 1U}, MessageSizeClass::Video, fragmenter));
	// A 32-bit size_t cannot represent a value above u32; constructing that
	// case would wrap the test input to zero before create() sees it.
	if (std::numeric_limits<std::size_t>::max() > std::numeric_limits<std::uint32_t>::max()) {
		EXPECT_FALSE(TelemetryFragmenter::create(
			ByteView{&byte, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U},
			MessageSizeClass::Video,
			fragmenter));
	}
	EXPECT_FALSE(TelemetryFragmenter::create(ByteView{}, MessageSizeClass::Invalid, fragmenter));
}

TEST(TelemetryProtocolReassembler, PublishesOnlyAfterAllOutOfOrderFragmentsValidateAtomically)
{
	const auto message = message_bytes(2265U);
	auto fragments = make_fragments(message);
	ASSERT_EQ(3U, fragments.size());
	TelemetryReassembler reassembler;
	auto completed = sentinel_message();

	EXPECT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragments[2].view(), completed));
	expect_sentinel(completed);
	EXPECT_EQ(1U, reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(message.size(), reassembler.reserved_bytes(MessageSizeClass::State));
	EXPECT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragments[0].view(), completed));
	expect_sentinel(completed);
	EXPECT_EQ(ReassemblyResult::Completed, reassembler.ingest(fragments[1].view(), completed));
	EXPECT_EQ(MessageSizeClass::State, completed.message_class);
	EXPECT_EQ(message, completed.payload);
	EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));
}

TEST(TelemetryProtocolReassembler, EveryThreeFragmentPermutationPublishesTheSameMessage)
{
	const auto message = message_bytes(2265U);
	const auto fragments = make_fragments(message);
	ASSERT_EQ(3U, fragments.size());

	std::array<std::size_t, 3> order{{0U, 1U, 2U}};
	std::size_t permutation_count = 0U;
	do {
		SCOPED_TRACE(testing::Message() << "order " << order[0] << order[1] << order[2]);
		TelemetryReassembler reassembler;
		auto completed = sentinel_message();
		for (std::size_t position = 0; position < order.size(); ++position) {
			const auto expected =
				position + 1U == order.size() ? ReassemblyResult::Completed : ReassemblyResult::Accepted;
			ASSERT_EQ(expected, reassembler.ingest(fragments[order[position]].view(), completed));
			if (expected != ReassemblyResult::Completed) {
				expect_sentinel(completed);
			}
		}
		EXPECT_EQ(message, completed.payload);
		EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
		EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));
		++permutation_count;
	} while (std::next_permutation(order.begin(), order.end()));
	EXPECT_EQ(6U, permutation_count);
}

TEST(TelemetryProtocolReassembler, IdenticalDuplicatesAreIdempotentAndContradictionsPurgeTheWholeMessage)
{
	const auto message = message_bytes(1133U);
	auto fragments = make_fragments(message, MessageType::Manifest);
	TelemetryReassembler reassembler;
	auto completed = sentinel_message();
	ASSERT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragments[0].view(), completed));

	auto retransmission = fragments[0];
	retransmission.header.flags |= MessageFlagRetransmission;
	retransmission.header.packet_sequence += 100U;
	retransmission.header.sent_time_us += 100U;
	EXPECT_EQ(ReassemblyResult::Duplicate, reassembler.ingest(retransmission.view(), completed));
	expect_sentinel(completed);
	EXPECT_EQ(1U, reassembler.active_reassemblies(MessageSizeClass::State));

	auto contradiction = fragments[0];
	contradiction.payload[0] ^= 0x80U;
	EXPECT_EQ(ReassemblyResult::InconsistentFragment, reassembler.ingest(contradiction.view(), completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));

	// The second fragment now starts a fresh candidate, proving the old entry
	// was purged rather than left partially publishable.
	EXPECT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragments[1].view(), completed));
	EXPECT_EQ(ReassemblyResult::Completed, reassembler.ingest(fragments[0].view(), completed));
	EXPECT_EQ(message, completed.payload);
}

TEST(TelemetryProtocolReassembler, EnforcesEveryCommonFieldButAllowsDatagramLocalRetransmissionFields)
{
	const auto message = message_bytes(2264U);
	auto original = make_fragments(message, MessageType::EventBatch);
	for (auto& fragment : original) {
		fragment.header.flags |= MessageFlagAckRequired;
	}
	ASSERT_EQ(2U, original.size());

	using Mutation = std::function<void(OwnedFragment&)>;
	const std::array<std::pair<const char*, Mutation>, 9> mutations{{
		{"major version", [](OwnedFragment& value) { value.header.version_major++; }},
		{"minor version", [](OwnedFragment& value) { value.header.version_minor++; }},
		{"type", [](OwnedFragment& value) { value.header.message_type = MessageType::Manifest; }},
		{"flags",
			[](OwnedFragment& value) { value.header.flags &= static_cast<std::uint8_t>(~MessageFlagAckRequired); }},
		{"frame", [](OwnedFragment& value) { value.header.frame_id++; }},
		{"mission time", [](OwnedFragment& value) { value.header.mission_time_us++; }},
		{"message size",
			[](OwnedFragment& value) {
				value.header.message_size--;
				value.payload.pop_back();
				value.header.payload_size--;
			}},
		{"fragment count",
			[](OwnedFragment& value) {
				value.header.message_size++;
				value.header.fragment_count++;
			}},
		{"message CRC", [](OwnedFragment& value) { value.header.message_crc32 ^= 1U; }},
	}};
	for (const auto& item : mutations) {
		SCOPED_TRACE(item.first);
		TelemetryReassembler reassembler;
		auto completed = sentinel_message();
		ASSERT_EQ(ReassemblyResult::Accepted, reassembler.ingest(original[0].view(), completed));
		auto changed = original[1];
		item.second(changed);
		EXPECT_EQ(ReassemblyResult::InconsistentFragment, reassembler.ingest(changed.view(), completed));
		expect_sentinel(completed);
		EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
	}

	TelemetryReassembler allowed;
	auto completed = sentinel_message();
	ASSERT_EQ(ReassemblyResult::Accepted, allowed.ingest(original[0].view(), completed));
	auto retransmitted = original[1];
	retransmitted.header.flags |= MessageFlagRetransmission;
	retransmitted.header.packet_sequence += 99U;
	retransmitted.header.sent_time_us += 99U;
	EXPECT_EQ(ReassemblyResult::Completed, allowed.ingest(retransmitted.view(), completed));
	EXPECT_EQ(message, completed.payload);
}

TEST(TelemetryProtocolReassembler, InvalidLayoutAndMessageCrcFailurePurgeWithoutPublishing)
{
	const auto message = message_bytes(1133U);
	auto fragments = make_fragments(message);
	auto completed = sentinel_message();
	TelemetryReassembler reassembler;
	ASSERT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragments[0].view(), completed));
	auto invalid = fragments[1];
	invalid.header.fragment_offset++;
	EXPECT_EQ(ReassemblyResult::InvalidLayout, reassembler.ingest(invalid.view(), completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));

	fragments = make_fragments(message);
	for (auto& fragment : fragments) {
		fragment.header.message_crc32 ^= 1U;
	}
	ASSERT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragments[0].view(), completed));
	EXPECT_EQ(ReassemblyResult::MessageCrcMismatch, reassembler.ingest(fragments[1].view(), completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));
}

TEST(TelemetryProtocolReassembler, ValidatesLayoutAndQuotasBeforeAnyObservableReservation)
{
	TelemetryReassembler reassembler;
	auto completed = sentinel_message();
	auto invalid = make_first_fragment(MessageType::Delta, 1133U, 1U);
	invalid.header.fragment_offset = 1U;
	EXPECT_EQ(ReassemblyResult::InvalidLayout, reassembler.ingest(invalid.view(), completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));

	for (std::uint32_t index = 0; index < MaxStateReassembliesPerClient; ++index) {
		auto fragment =
			make_first_fragment(MessageType::Delta, static_cast<std::uint32_t>(MaxStateMessageSize), 100U + index);
		EXPECT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragment.view(), completed));
	}
	EXPECT_EQ(MaxStateReassembliesPerClient, reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(MaxStateReassemblyBytesPerClient, reassembler.reserved_bytes(MessageSizeClass::State));
	auto excess_state = make_first_fragment(MessageType::Delta, static_cast<std::uint32_t>(MaxStateMessageSize), 999U);
	EXPECT_EQ(ReassemblyResult::QuotaExceeded, reassembler.ingest(excess_state.view(), completed));
	EXPECT_EQ(MaxStateReassembliesPerClient, reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(MaxStateReassemblyBytesPerClient, reassembler.reserved_bytes(MessageSizeClass::State));

	for (std::uint32_t index = 0; index < MaxVideoReassembliesPerClient; ++index) {
		auto fragment = make_first_fragment(MessageType::TargetVideoFrame,
			static_cast<std::uint32_t>(MaxVideoMessageSize),
			2000U + index);
		EXPECT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragment.view(), completed));
	}
	EXPECT_EQ(MaxVideoReassembliesPerClient, reassembler.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(MaxVideoReassemblyBytesPerClient, reassembler.reserved_bytes(MessageSizeClass::Video));
	auto excess_video =
		make_first_fragment(MessageType::TargetVideoFrame, static_cast<std::uint32_t>(MaxVideoMessageSize), 2999U);
	EXPECT_EQ(ReassemblyResult::QuotaExceeded, reassembler.ingest(excess_video.view(), completed));
	EXPECT_EQ(MaxVideoReassembliesPerClient, reassembler.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(MaxStateReassembliesPerClient, reassembler.active_reassemblies(MessageSizeClass::State));
	expect_sentinel(completed);

	EXPECT_TRUE(reassembler.discard(42U, 100U));
	EXPECT_FALSE(reassembler.discard(42U, 100U));
	EXPECT_EQ(MaxStateReassembliesPerClient - 1U, reassembler.active_reassemblies(MessageSizeClass::State));
	reassembler.clear();
	EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::Video));
	EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));
	EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::Video));
}

TEST(TelemetryProtocolReassembler, SessionAndMessageIdFormIndependentCandidateKeys)
{
	const auto message = message_bytes(1133U);
	auto first = make_fragments(message, MessageType::Delta, 7U, 11U);
	auto other_session = make_fragments(message, MessageType::Delta, 7U, 12U);
	auto other_message = make_fragments(message, MessageType::Delta, 8U, 11U);
	TelemetryReassembler reassembler;
	auto completed = sentinel_message();
	EXPECT_EQ(ReassemblyResult::Accepted, reassembler.ingest(first[0].view(), completed));
	EXPECT_EQ(ReassemblyResult::Accepted, reassembler.ingest(other_session[0].view(), completed));
	EXPECT_EQ(ReassemblyResult::Accepted, reassembler.ingest(other_message[0].view(), completed));
	EXPECT_EQ(3U, reassembler.active_reassemblies(MessageSizeClass::State));
}

} // namespace
