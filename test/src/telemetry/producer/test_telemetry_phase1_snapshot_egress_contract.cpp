#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#if __has_include("telemetry/phase1_snapshot_egress.h")
#include "telemetry/phase1_snapshot_egress.h"
#else
namespace telemetry::detail {

enum class Phase1SnapshotEgressResult : std::uint8_t {
	Queued = 0,
	InvalidArgument,
	ReliableCapacity,
	OutputCapacity,
	AllocationFailure,
};

struct Phase1SnapshotEgressLimits {
	std::size_t reliable_item_capacity = 8U;
	std::size_t output_datagram_capacity = 8U;
};

struct Phase1SnapshotDatagram {
	protocol::EndpointKey endpoint;
	std::array<std::uint8_t, protocol::MaxDatagramSize> bytes{};
	std::size_t size = 0U;
};

class Phase1SnapshotEgress final {
  public:
	bool configure(const Phase1SnapshotEgressLimits&) noexcept { return false; }
	Phase1SnapshotEgressResult queue_initial_snapshot(std::uint64_t,
		const protocol::EndpointKey&,
		std::uint32_t,
		std::uint64_t,
		const protocol::StateImage&,
		std::uint64_t) noexcept
	{
		return Phase1SnapshotEgressResult::InvalidArgument;
	}
	std::size_t service(std::size_t) noexcept { return 0U; }
	bool peek_output(Phase1SnapshotDatagram&) const noexcept { return false; }
	void complete_output() noexcept {}
	bool has_candidate() const noexcept { return false; }
	std::size_t retained_item_count() const noexcept { return 0U; }
	std::size_t queued_datagram_count() const noexcept { return 0U; }
	const std::vector<protocol::SnapshotCandidatePart>& candidate_parts() const noexcept
	{
		return m_parts;
	}

  private:
	std::vector<protocol::SnapshotCandidatePart> m_parts;
};

} // namespace telemetry::detail
#endif

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

// The egress owns publication only; the per-slot tracker owns baseline commit.

protocol::EndpointKey endpoint()
{
	return protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 9U}, 7808U);
}

protocol::StateImage extension_image(std::size_t value_size)
{
	protocol::StateAtom atom;
	atom.key.record_type = 0x8001U;
	atom.value.resize(value_size, 0x5aU);
	protocol::StateImage result;
	EXPECT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::create(std::vector<protocol::StateAtom>{std::move(atom)}, result));
	return result;
}

protocol::DatagramView decode(const detail::Phase1SnapshotDatagram& output)
{
	protocol::DatagramView result;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram({output.bytes.data(), output.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			result));
	return result;
}

TEST(TelemetryPhase1SnapshotEgressContract, StateImageBecomesARealRetainedFullSnapshotPart)
{
	detail::Phase1SnapshotEgress egress;
	ASSERT_TRUE(egress.configure({4U, 4U}));
	ASSERT_EQ(detail::Phase1SnapshotEgressResult::Queued,
		egress.queue_initial_snapshot(0x1234U, endpoint(), 17U, 555'000U, extension_image(16U), 1'000U));
	ASSERT_TRUE(egress.has_candidate());
	ASSERT_EQ(1U, egress.retained_item_count());
	ASSERT_EQ(1U, egress.candidate_parts().size());
	EXPECT_EQ(protocol::MessageType::FullSnapshot, egress.candidate_parts()[0].target.message_type);

	ASSERT_EQ(1U, egress.service(4U));
	detail::Phase1SnapshotDatagram output;
	ASSERT_TRUE(egress.peek_output(output));
	const auto datagram = decode(output);
	EXPECT_EQ(protocol::MessageType::FullSnapshot, datagram.header.message_type);
	EXPECT_EQ(0x1234U, datagram.header.session_id);
	EXPECT_NE(0U, static_cast<std::uint8_t>(datagram.header.flags & protocol::MessageFlagAckRequired));
	EXPECT_NE(0U, static_cast<std::uint8_t>(datagram.header.flags & protocol::MessageFlagKeyframe));
	EXPECT_EQ(egress.candidate_parts()[0].target.message_id, datagram.header.message_id);
	EXPECT_EQ(egress.candidate_parts()[0].target.fragment_count, datagram.header.fragment_count);
	EXPECT_EQ(egress.candidate_parts()[0].target.message_crc32, datagram.header.message_crc32);

	protocol::FullSnapshotPartPayload payload;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_full_snapshot_part_payload(datagram.payload, payload));
	EXPECT_EQ(17U, payload.snapshot_id);
	EXPECT_EQ(0U, payload.required_manifest_id);
	EXPECT_EQ(1U, payload.part_count);
	EXPECT_EQ(1U, payload.record_count);
	EXPECT_GT(payload.records.size, 0U) << "The StateImage must be serialized, not replaced by an empty marker.";
}

TEST(TelemetryPhase1SnapshotEgressContract, FragmentPublicationIsCappedByTheExplicitServiceBudget)
{
	detail::Phase1SnapshotEgress egress;
	ASSERT_TRUE(egress.configure({4U, 2U}));
	ASSERT_EQ(detail::Phase1SnapshotEgressResult::Queued,
		egress.queue_initial_snapshot(0x5678U, endpoint(), 18U, 666'000U, extension_image(3'000U), 2'000U));
	ASSERT_EQ(1U, egress.candidate_parts().size());
	ASSERT_GT(egress.candidate_parts()[0].target.fragment_count, 1U);

	std::uint16_t observed = 0U;
	while (observed < egress.candidate_parts()[0].target.fragment_count) {
		EXPECT_EQ(1U, egress.service(1U));
		EXPECT_EQ(1U, egress.queued_datagram_count());
		detail::Phase1SnapshotDatagram output;
		ASSERT_TRUE(egress.peek_output(output));
		const auto datagram = decode(output);
		EXPECT_EQ(observed, datagram.header.fragment_index);
		EXPECT_EQ(egress.candidate_parts()[0].target.fragment_count, datagram.header.fragment_count);
		EXPECT_EQ(egress.candidate_parts()[0].target.message_crc32, datagram.header.message_crc32);
		egress.complete_output();
		++observed;
	}
	EXPECT_EQ(0U, egress.service(1U));
	EXPECT_EQ(0U, egress.queued_datagram_count());
}

TEST(TelemetryPhase1SnapshotEgressContract, FailedReliableReservationPublishesNoPartialTransaction)
{
	detail::Phase1SnapshotEgress egress;
	ASSERT_TRUE(egress.configure({0U, 8U}));
	EXPECT_EQ(detail::Phase1SnapshotEgressResult::ReliableCapacity,
		egress.queue_initial_snapshot(0x9abcU, endpoint(), 19U, 777'000U, extension_image(3'000U), 3'000U));
	EXPECT_FALSE(egress.has_candidate());
	EXPECT_EQ(0U, egress.retained_item_count());
	EXPECT_TRUE(egress.candidate_parts().empty());
	EXPECT_EQ(0U, egress.service(8U));
	EXPECT_EQ(0U, egress.queued_datagram_count());
	detail::Phase1SnapshotDatagram output;
	EXPECT_FALSE(egress.peek_output(output));
}

} // namespace
