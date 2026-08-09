#include "telemetry/session_controller.h"

#include "telemetry/protocol/telemetry_clock.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_rate_limiter.h"
#include "telemetry/transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

template <typename Container>
protocol::ByteView view(const Container& bytes)
{
	return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

template <typename Container>
protocol::MutableByteView mutable_view(Container& bytes)
{
	return {reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()};
}

protocol::EndpointKey endpoint(std::uint8_t last_octet = 2U, std::uint16_t port = 42043U)
{
	return protocol::EndpointKey::from_ipv4({127U, 0U, 0U, last_octet}, port);
}

struct ScriptedRandom final : detail::RandomSource {
	std::vector<std::uint64_t> values;
	std::size_t index = 0U;
	ScriptedRandom(std::initializer_list<std::uint64_t> script) : values(script) {}
	bool next_u64(std::uint64_t& output) noexcept override
	{
		if (index >= values.size()) {
			return false;
		}
		output = values[index++];
		return true;
	}
};

struct IdentityHarness {
	ScriptedRandom random;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator allocator;
	explicit IdentityHarness(std::initializer_list<std::uint64_t> values)
		: random(values), allocator(random, registry)
	{
		EXPECT_TRUE(registry.allocate_storage());
	}
};

struct Datagram {
	std::vector<std::uint8_t> bytes;
};

Datagram encode_datagram(protocol::TelemetryDatagramHeader header, protocol::ByteView payload)
{
	Datagram result;
	result.bytes.resize(protocol::HeaderSizeV1 + payload.size);
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{header.version_minor, header.version_minor},
			payload,
			mutable_view(result.bytes),
			written));
	EXPECT_EQ(result.bytes.size(), written);
	return result;
}

Datagram make_hello(std::uint64_t nonce,
	std::uint32_t packet_sequence = 1U,
	std::uint8_t min_minor = protocol::VersionMinorV1_1,
	std::uint8_t max_minor = protocol::VersionMinorV1_1)
{
	protocol::HelloPayload hello;
	hello.client_nonce = nonce;
	hello.client_send_t0_us = 100U;
	hello.min_major = protocol::VersionMajor;
	hello.max_major = protocol::VersionMajor;
	hello.min_minor = min_minor;
	hello.max_minor = max_minor;
	hello.requested_visibility_mode = protocol::VisibilityMode::Cockpit;
	hello.requested_heartbeat_ms = 1000U;
	std::array<std::uint8_t, protocol::HelloPayloadPrefixSize> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_hello_payload(hello, mutable_view(payload), written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = max_minor;
	header.message_type = protocol::MessageType::Hello;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = hello.client_send_t0_us;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	return encode_datagram(header, {payload.data(), written});
}

struct DecodedOutput {
	std::vector<std::uint8_t> storage;
	protocol::DatagramView datagram;
};

DecodedOutput pop_output(detail::SessionController& controller)
{
	detail::SessionControllerOutput output;
	EXPECT_TRUE(controller.pop_output(output));
	DecodedOutput decoded;
	decoded.storage.assign(output.bytes.begin(), output.bytes.begin() + static_cast<std::ptrdiff_t>(output.size));
	if (!decoded.storage.empty()) {
		EXPECT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram(view(decoded.storage),
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				decoded.datagram));
	}
	return decoded;
}

Datagram make_ack(const DecodedOutput& target, std::uint32_t packet_sequence)
{
	protocol::AckPayload ack;
	ack.target_message_id = target.datagram.header.message_id;
	ack.target_message_type = target.datagram.header.message_type;
	ack.ack_flags = protocol::KnownAckFlags;
	ack.target_fragment_count = target.datagram.header.fragment_count;
	ack.target_message_crc32 = target.datagram.header.message_crc32;
	std::array<std::uint8_t, protocol::AckPayloadSize> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_ack_payload(ack, mutable_view(payload), written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = target.datagram.header.session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = 200U + packet_sequence;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	return encode_datagram(header, {payload.data(), written});
}

Datagram make_heartbeat(std::uint64_t session_id,
	const protocol::HeartbeatPayload& heartbeat,
	std::uint32_t packet_sequence,
	std::uint64_t sent_time_us)
{
	std::array<std::uint8_t, protocol::HeartbeatPayloadSize> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_heartbeat_payload(heartbeat, mutable_view(payload), written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Heartbeat;
	header.session_id = session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = sent_time_us;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	return encode_datagram(header, {payload.data(), written});
}

void write_u32_le(std::array<std::uint8_t, protocol::HeartbeatPayloadSize>& payload,
	std::size_t offset,
	std::uint32_t value)
{
	for (std::size_t index = 0U; index < 4U; ++index) {
		payload[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
	}
}

void write_u64_le(std::array<std::uint8_t, protocol::HeartbeatPayloadSize>& payload,
	std::size_t offset,
	std::uint64_t value)
{
	for (std::size_t index = 0U; index < 8U; ++index) {
		payload[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
	}
}

Datagram make_raw_heartbeat(std::uint64_t session_id,
	std::uint32_t probe_id,
	protocol::HeartbeatKind kind,
	std::uint64_t origin_t0_us,
	std::uint64_t receive_t1_us,
	std::uint64_t transmit_t2_us,
	std::uint32_t packet_sequence,
	std::uint64_t sent_time_us)
{
	std::array<std::uint8_t, protocol::HeartbeatPayloadSize> payload{};
	write_u32_le(payload, 0U, probe_id);
	payload[4U] = static_cast<std::uint8_t>(kind);
	write_u64_le(payload, 8U, origin_t0_us);
	write_u64_le(payload, 16U, receive_t1_us);
	write_u64_le(payload, 24U, transmit_t2_us);
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Heartbeat;
	header.session_id = session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = sent_time_us;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(payload.size());
	header.message_crc32 = protocol::crc32_iso_hdlc(view(payload));
	return encode_datagram(header, view(payload));
}

detail::SessionControllerConfig config(std::size_t max_clients = 1U)
{
	detail::SessionControllerConfig value;
	value.max_clients = max_clients;
	value.producer_id = 0x1020304050607080ULL;
	value.mission_heartbeat_ms = 500U;
	value.idle_heartbeat_ms = 1000U;
	value.security.enabled = true;
	value.security.port = 42042U;
	value.security.bind_mode = protocol::NetworkBindMode::LoopbackOnly;
	value.security.resources.max_clients = max_clients;
	value.security.resources.global_state_reassembly_bytes =
		max_clients * protocol::MaxStateReassemblyBytesPerClient;
	return value;
}

detail::SessionController make_controller(detail::SessionIdAllocator& ids, std::size_t max_clients = 1U)
{
	struct PacketSequences final : detail::RandomSource {
		std::uint64_t next = 0x50607080U;
		bool next_u64(std::uint64_t& output) noexcept override
		{
			output = next++;
			return true;
		}
	};
	static PacketSequences sequences;
	detail::SessionController controller;
	EXPECT_EQ(detail::SessionControllerConfigureResult::Ready,
		detail::SessionController::configure(config(max_clients), ids, sequences, 0U, nullptr, controller));
	return controller;
}

DecodedOutput establish_ready(detail::SessionController& controller,
	const protocol::EndpointKey& peer,
	std::uint64_t nonce,
	std::uint64_t hello_time_us,
	std::uint64_t proof_time_us,
	std::uint32_t packet_base,
	bool mission_active = false,
	std::uint16_t expected_heartbeat_interval_ms = 1000U)
{
	const auto mission_generation = mission_active ? 7U : 0U;
	const auto request = make_hello(nonce, packet_base);
	EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(request.bytes), hello_time_us, mission_generation, mission_active).disposition);
	const auto welcome = pop_output(controller);
	protocol::WelcomePayload welcome_payload;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_welcome_payload(welcome.datagram.payload, welcome_payload));
	EXPECT_EQ(expected_heartbeat_interval_ms, welcome_payload.heartbeat_interval_ms);
	const auto proof = make_ack(welcome, packet_base + 1U);
	EXPECT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(peer, view(proof.bytes), proof_time_us, mission_generation, mission_active).disposition);
	const auto begin = pop_output(controller);
	const auto applied = make_ack(begin, packet_base + 2U);
	EXPECT_EQ(detail::SessionIngressDropReason::None,
		controller.ingest(peer,
			view(applied.bytes),
			proof_time_us + 1U,
			mission_generation,
			mission_active)
			.drop_reason);
	return begin;
}

protocol::HeartbeatPayload decode_heartbeat(const DecodedOutput& output)
{
	protocol::HeartbeatPayload heartbeat;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_heartbeat_payload(output.datagram.payload, heartbeat));
	return heartbeat;
}

protocol::HeartbeatPayload decode_heartbeat(const detail::SessionControllerOutput& output)
{
	protocol::DatagramView datagram;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram({output.bytes.data(), output.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			datagram));
	protocol::HeartbeatPayload heartbeat;
	EXPECT_EQ(protocol::ValidationError::None, protocol::decode_heartbeat_payload(datagram.payload, heartbeat));
	return heartbeat;
}

template <typename Controller, typename = void>
struct has_wp06_heartbeat_contract : std::false_type {};

template <typename Controller>
struct has_wp06_heartbeat_contract<Controller,
	std::void_t<decltype(std::declval<Controller&>().service_session_maintenance(
			std::declval<std::uint64_t>())),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.probes.in_flight_count()),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.clock_filter.sample_count()),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.negotiated_interval_ms),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.stale_timeout_us),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.disconnect_timeout_us),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.next_periodic_due_us),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.last_valid_network_activity_us),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.last_valid_clock_response_us),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.clock_stale)>> : std::true_type {};

template <typename Controller, typename = void>
struct has_wp06_ordered_tick_phases : std::false_type {};

template <typename Controller>
struct has_wp06_ordered_tick_phases<Controller,
	std::void_t<decltype(std::declval<Controller&>().service_timeouts(std::declval<std::uint64_t>())),
		decltype(std::declval<Controller&>().service_periodic(std::declval<std::uint64_t>())),
		decltype(std::declval<Controller&>().purge_all(std::declval<detail::SessionCloseReason>()))>>
	: std::bool_constant<noexcept(
		  std::declval<Controller&>().service_timeouts(std::declval<std::uint64_t>())) &&
		  noexcept(std::declval<Controller&>().service_periodic(std::declval<std::uint64_t>())) &&
		  noexcept(std::declval<Controller&>().purge_all(std::declval<detail::SessionCloseReason>()))> {};

template <typename Controller>
void expect_request_refreshes_network_activity(Controller& controller, std::uint64_t expected_now_us)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "The fixed heartbeat activity timestamp is absent.";
	} else {
		EXPECT_EQ(expected_now_us, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "A valid incoming Request refreshes network activity.";
	}
}

TEST(TelemetryWp06HeartbeatContract, CanonicalRequestQueuesImmediateResponseWithExactFourTimestampPrefix)
{
	IdentityHarness ids{0x1111U};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	const auto begin = establish_ready(controller, peer, 1U, 100U, 200U, 10U);
	protocol::HeartbeatPayload request;
	request.probe_id = 7U;
	request.kind = protocol::HeartbeatKind::Request;
	request.origin_t0_us = 900U;
	const auto datagram = make_heartbeat(begin.datagram.header.session_id, request, 20U, 900U);
	const auto result = controller.ingest(peer, view(datagram.bytes), 1'000U, 0U, false);
	ASSERT_EQ(detail::SessionIngressDropReason::None, result.drop_reason);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued, result.disposition);
	const auto response_output = pop_output(controller);
	ASSERT_EQ(protocol::MessageType::Heartbeat, response_output.datagram.header.message_type);
	EXPECT_EQ(protocol::MessageFlagNone, response_output.datagram.header.flags);
	const auto response = decode_heartbeat(response_output);
	EXPECT_EQ(protocol::HeartbeatKind::Response, response.kind);
	EXPECT_EQ(request.probe_id, response.probe_id);
	EXPECT_EQ(request.origin_t0_us, response.origin_t0_us);
	EXPECT_EQ(1'000U, response.receive_t1_us);
	EXPECT_EQ(1'000U, response.transmit_t2_us);
	expect_request_refreshes_network_activity(controller, 1'000U);
}

template <typename Controller>
void expect_probe_correlation_and_filter(Controller& controller, const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "WP06-HB service and fixed per-slot heartbeat state are absent.";
	} else {
		controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
		const auto request_output = pop_output(controller);
		const auto request = decode_heartbeat(request_output);
		ASSERT_EQ(protocol::HeartbeatKind::Request, request.kind);
		ASSERT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		protocol::HeartbeatPayload forged = request;
		forged.kind = protocol::HeartbeatKind::Response;
		forged.receive_t1_us = request.origin_t0_us + 10U;
		forged.transmit_t2_us = request.origin_t0_us + 20U;
		const auto response_time = request.origin_t0_us + 100U;
		++forged.origin_t0_us;
		const auto forged_datagram =
			make_heartbeat(controller.slot(0U).session_id, forged, 30U, response_time);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(forged_datagram.bytes), response_time, 0U, false).drop_reason);
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		--forged.origin_t0_us;
		const auto valid = make_heartbeat(controller.slot(0U).session_id, forged, 31U, response_time);
		EXPECT_EQ(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(valid.bytes), response_time, 0U, false).drop_reason);
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
		std::uint64_t minimum_rtt_us = 0U;
		std::int64_t smoothed_offset_us = 0;
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.minimum_round_trip_time_us(minimum_rtt_us));
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.smoothed_offset_us(smoothed_offset_us));
		EXPECT_EQ(90U, minimum_rtt_us);
		EXPECT_EQ(-35, smoothed_offset_us);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(valid.bytes), response_time + 1U, 0U, false).drop_reason)
			<< "An exact response is consumed once; its duplicate is uncorrelated.";
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
	}
}

TEST(TelemetryWp06HeartbeatContract, ResponseCorrelationRejectsForgedTupleAndFeedsPhase0ClockFilter)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x2222U};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 2U, 100U, 200U, 40U);
	expect_probe_correlation_and_filter(controller, peer);
}

template <typename Controller>
void expect_probe_and_sample_capacities(Controller& controller, const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "The missing HB seam prevents fixed probe/sample capacity proofs.";
	} else {
		std::array<protocol::HeartbeatPayload, protocol::MaxInFlightProbes> requests{};
		for (std::size_t index = 0U; index < protocol::MaxInFlightProbes; ++index) {
			controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
			ASSERT_TRUE(controller.has_output());
			requests[index] = decode_heartbeat(pop_output(controller));
			ASSERT_EQ(protocol::HeartbeatKind::Request, requests[index].kind);
		}
		ASSERT_EQ(protocol::MaxInFlightProbes, controller.slot(0U).heartbeat.probes.in_flight_count());
		const auto sequence = controller.slot(0U).next_packet_sequence;
		const auto saturated_due = controller.slot(0U).heartbeat.next_periodic_due_us;
		controller.service_session_maintenance(saturated_due);
		EXPECT_FALSE(controller.has_output());
		EXPECT_EQ(protocol::MaxInFlightProbes, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(sequence, controller.slot(0U).next_packet_sequence)
			<< "CapacityReached must not evict a probe or consume a packet sequence.";

		for (std::size_t reverse = protocol::MaxInFlightProbes; reverse > 0U; --reverse) {
			const auto index = reverse - 1U;
			auto response = requests[index];
			response.kind = protocol::HeartbeatKind::Response;
			response.receive_t1_us = response.origin_t0_us + 10U;
			response.transmit_t2_us = response.origin_t0_us + 20U;
			const auto receive_time = saturated_due + 100U + (protocol::MaxInFlightProbes - reverse);
			const auto datagram = make_heartbeat(
				controller.slot(0U).session_id, response, 600U + static_cast<std::uint32_t>(index), receive_time);
			ASSERT_EQ(detail::SessionIngressDropReason::None,
				controller.ingest(peer, view(datagram.bytes), receive_time, 0U, false).drop_reason);
		}
		ASSERT_EQ(protocol::ClockFilter::Capacity,
			controller.slot(0U).heartbeat.clock_filter.sample_count());
		std::uint64_t minimum_before_eviction = 0U;
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.minimum_round_trip_time_us(
			minimum_before_eviction));

		controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
		auto ninth_response = decode_heartbeat(pop_output(controller));
		ninth_response.kind = protocol::HeartbeatKind::Response;
		ninth_response.receive_t1_us = ninth_response.origin_t0_us + 10U;
		ninth_response.transmit_t2_us = ninth_response.origin_t0_us + 20U;
		const auto ninth_receive_time = ninth_response.origin_t0_us + 10'000'000U;
		const auto ninth_datagram =
			make_heartbeat(controller.slot(0U).session_id, ninth_response, 700U, ninth_receive_time);
		ASSERT_EQ(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(ninth_datagram.bytes), ninth_receive_time, 0U, false).drop_reason);
		EXPECT_EQ(protocol::ClockFilter::Capacity,
			controller.slot(0U).heartbeat.clock_filter.sample_count());
		std::uint64_t minimum_after_eviction = 0U;
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.minimum_round_trip_time_us(
			minimum_after_eviction));
		EXPECT_GT(minimum_after_eviction, minimum_before_eviction)
			<< "The ninth valid sample evicts the oldest sample, not an in-flight probe.";
	}
}

TEST(TelemetryWp06HeartbeatContract, EightConcurrentProbesSaturateWithoutEvictionAndNinthSampleEvictsOldest)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x3333U};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 3U, 100U, 200U, 60U);
	expect_probe_and_sample_capacities(controller, peer);
}

template <typename Controller>
void expect_cadence_and_backpressure(Controller& controller)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "WP06-HB periodic cadence service is absent.";
	} else {
		ASSERT_EQ(1000U, controller.slot(0U).heartbeat.negotiated_interval_ms);
		ASSERT_EQ(3'000'000U, controller.slot(0U).heartbeat.stale_timeout_us);
		ASSERT_EQ(10'000'000U, controller.slot(0U).heartbeat.disconnect_timeout_us);
		const std::uint64_t due = 1'000'200U;
		ASSERT_EQ(due, controller.slot(0U).heartbeat.next_periodic_due_us)
			<< "Idle cadence is anchored when WELCOME becomes applied, using immutable H=1000 ms.";
		controller.service_session_maintenance(due - 1U);
		EXPECT_FALSE(controller.has_output());
		controller.service_session_maintenance(due);
		ASSERT_TRUE(controller.has_output());
		const auto probes_before = controller.slot(0U).heartbeat.probes.in_flight_count();
		ASSERT_EQ(1U, probes_before);
		controller.complete_output(detail::IoStatus::WouldBlock);
		EXPECT_FALSE(controller.has_output()) << "An unreliable heartbeat is abandoned on WouldBlock.";
		EXPECT_EQ(probes_before - 1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(due + 1'000'000U, controller.slot(0U).heartbeat.next_periodic_due_us);

		const std::uint64_t hitch_now = 9'000'000U;
		ASSERT_LT(hitch_now,
			controller.slot(0U).heartbeat.last_valid_network_activity_us + 10'000'000U)
			<< "The multi-H hitch must remain strictly before the normative disconnect deadline.";
		controller.service_session_maintenance(hitch_now);
		ASSERT_TRUE(controller.has_output()) << "A multi-H hitch coalesces to one recent heartbeat.";
		EXPECT_EQ(protocol::HeartbeatKind::Request, decode_heartbeat(pop_output(controller)).kind);
		controller.service_session_maintenance(hitch_now);
		EXPECT_FALSE(controller.has_output()) << "No same-now catch-up burst is permitted.";
	}
}

TEST(TelemetryWp06HeartbeatContract, PeriodicCadenceUsesExactBoundariesAndHitchesNeverBurst)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x4444U};
	auto controller = make_controller(ids.allocator);
	(void)establish_ready(controller, endpoint(), 4U, 100U, 200U, 80U);
	expect_cadence_and_backpressure(controller);
}

template <typename Controller>
void expect_welcome_freezes_mission_cadence(Controller& controller)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "WP06-HB cannot prove the WELCOME-selected immutable session cadence.";
	} else {
		EXPECT_EQ(500U, controller.slot(0U).heartbeat.negotiated_interval_ms);
		EXPECT_EQ(3'000'000U, controller.slot(0U).heartbeat.stale_timeout_us)
			<< "Mission H=500 ms still uses the normative 3-second minimum.";
		EXPECT_EQ(10'000'000U, controller.slot(0U).heartbeat.disconnect_timeout_us)
			<< "Mission H=500 ms still uses the normative 10-second minimum.";
		const std::uint64_t due = 500'200U;
		ASSERT_EQ(due, controller.slot(0U).heartbeat.next_periodic_due_us)
			<< "Mission cadence is anchored when WELCOME becomes applied, using immutable H=500 ms.";
		controller.service_session_maintenance(due - 1U);
		EXPECT_FALSE(controller.has_output());
		controller.service_session_maintenance(due);
		ASSERT_TRUE(controller.has_output());
		const auto request = decode_heartbeat(pop_output(controller));
		EXPECT_EQ(protocol::HeartbeatKind::Request, request.kind);
		EXPECT_EQ(500U, controller.slot(0U).heartbeat.negotiated_interval_ms)
			<< "The interval announced in WELCOME is immutable for this session.";
		EXPECT_EQ(due + 500'000U, controller.slot(0U).heartbeat.next_periodic_due_us);
	}
}

TEST(TelemetryWp06HeartbeatContract, WelcomeFreezesMissionSelectedHeartbeatIntervalForSessionLifetime)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x4545U};
	auto controller = make_controller(ids.allocator);
	(void)establish_ready(controller, endpoint(), 45U, 100U, 200U, 90U, true, 500U);
	expect_welcome_freezes_mission_cadence(controller);
}

template <typename Controller>
void expect_timeout_boundaries(Controller& controller, const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "WP06-HB stale/long timeout state is absent.";
	} else {
		controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
		const auto request = decode_heartbeat(pop_output(controller));
		ASSERT_EQ(protocol::HeartbeatKind::Request, request.kind);
		ASSERT_EQ(1000U, controller.slot(0U).heartbeat.negotiated_interval_ms);
		ASSERT_EQ(3'000'000U, controller.slot(0U).heartbeat.stale_timeout_us);
		ASSERT_EQ(10'000'000U, controller.slot(0U).heartbeat.disconnect_timeout_us);
		const auto base = controller.slot(0U).heartbeat.last_valid_clock_response_us;
		const auto stale = controller.slot(0U).heartbeat.stale_timeout_us;
		controller.service_session_maintenance(base + stale - 1U);
		EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress);
		controller.service_session_maintenance(base + stale);
		EXPECT_EQ(detail::ProducerSessionProgress::Stale, controller.slot(0U).progress);
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_stale);
		EXPECT_FALSE(controller.slot(0U).heartbeat.clock_filter.valid());

		// A valid response may rebuild clock state, but WP08/new-session logic owns
		// any semantic promotion out of Stale.
		protocol::HeartbeatPayload response = request;
		response.kind = protocol::HeartbeatKind::Response;
		response.receive_t1_us = request.origin_t0_us + 10U;
		response.transmit_t2_us = request.origin_t0_us + 20U;
		const auto response_time = base + stale + 1U;
		const auto datagram = make_heartbeat(controller.slot(0U).session_id, response, 140U, response_time);
		EXPECT_EQ(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(datagram.bytes), response_time, 0U, false).drop_reason);
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_EQ(detail::ProducerSessionProgress::Stale, controller.slot(0U).progress);
		const auto network = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto disconnect = controller.slot(0U).heartbeat.disconnect_timeout_us;
		controller.service_session_maintenance(network + disconnect - 1U);
		EXPECT_EQ(1U, controller.active_slots());
		controller.service_session_maintenance(network + disconnect);
		EXPECT_EQ(0U, controller.active_slots());
		(void)peer;
	}
}

TEST(TelemetryWp06HeartbeatContract, StaleAndDisconnectTimeoutsAreExactAndNeverPromoteStaleToReady)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x5555U};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 5U, 100U, 200U, 100U);
	expect_timeout_boundaries(controller, peer);
}

TEST(TelemetryWp06HeartbeatContract, HeartbeatRequestBucketIsTwentyThenRefillsAtExactlyTenPerSecond)
{
	IdentityHarness ids{0x6666U};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	const auto begin = establish_ready(controller, peer, 6U, 100U, 200U, 120U);
	for (std::uint32_t index = 0U; index < protocol::HeartbeatRequestRateLimit.burst_tokens; ++index) {
		protocol::HeartbeatPayload request;
		request.probe_id = index + 1U;
		request.kind = protocol::HeartbeatKind::Request;
		request.origin_t0_us = 1'000U + index;
		const auto datagram = make_heartbeat(begin.datagram.header.session_id, request, 200U + index, 1'000U);
		ASSERT_EQ(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(datagram.bytes), 1'000U, 0U, false).drop_reason);
		(void)pop_output(controller);
	}
	protocol::HeartbeatPayload request;
	request.probe_id = 99U;
	request.kind = protocol::HeartbeatKind::Request;
	request.origin_t0_us = 2'000U;
	const auto limited = make_heartbeat(begin.datagram.header.session_id, request, 299U, 2'000U);
	EXPECT_NE(detail::SessionIngressDropReason::None,
		controller.ingest(peer, view(limited.bytes), 1'000U, 0U, false).drop_reason);
	EXPECT_NE(detail::SessionIngressDropReason::None,
		controller.ingest(peer, view(limited.bytes), 100'999U, 0U, false).drop_reason);
	EXPECT_EQ(detail::SessionIngressDropReason::None,
		controller.ingest(peer, view(limited.bytes), 101'000U, 0U, false).drop_reason);
}

template <typename Controller>
void expect_rate_limited_request_is_mutation_free(Controller& controller,
	const protocol::EndpointKey& peer,
	std::uint64_t session_id)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Fixed heartbeat state is required for the P1-REQ-013 mutation audit.";
	} else {
		for (std::uint32_t index = 0U; index < protocol::HeartbeatRequestRateLimit.burst_tokens; ++index) {
			protocol::HeartbeatPayload request;
			request.probe_id = index + 1U;
			request.kind = protocol::HeartbeatKind::Request;
			request.origin_t0_us = 1'000U + index;
			const auto datagram = make_heartbeat(session_id, request, 1'000U + index, 1'000U + index);
			ASSERT_EQ(detail::SessionIngressDropReason::None,
				controller.ingest(peer, view(datagram.bytes), 1'000U + index, 0U, false).drop_reason);
			detail::SessionControllerOutput discarded;
			ASSERT_TRUE(controller.pop_output(discarded));
		}
		const auto activity_before = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto clock_before = controller.slot(0U).heartbeat.last_valid_clock_response_us;
		const auto probes_before = controller.slot(0U).heartbeat.probes.in_flight_count();
		const auto samples_before = controller.slot(0U).heartbeat.clock_filter.sample_count();
		protocol::HeartbeatPayload limited;
		limited.probe_id = 99U;
		limited.kind = protocol::HeartbeatKind::Request;
		limited.origin_t0_us = 2'000U;
		const auto datagram = make_heartbeat(session_id, limited, 2'000U, 2'000U);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(datagram.bytes), 2'000U, 0U, false).drop_reason);
		EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(clock_before, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(probes_before, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(samples_before, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06HeartbeatContract, RateLimitedRequestCannotRefreshTimeoutOrMutateHeartbeatState)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x6A6AU};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	const auto begin = establish_ready(controller, peer, 61U, 100U, 200U, 1'100U);
	expect_rate_limited_request_is_mutation_free(controller, peer, begin.datagram.header.session_id);
}

template <typename Controller>
void expect_response_rejections_are_mutation_free(Controller& controller, const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Fixed heartbeat state is required for the P1-REQ-013 correlation audit.";
	} else {
		controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
		const auto request = decode_heartbeat(pop_output(controller));
		ASSERT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		const auto activity_before = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto clock_before = controller.slot(0U).heartbeat.last_valid_clock_response_us;

		auto response = request;
		response.kind = protocol::HeartbeatKind::Response;
		response.receive_t1_us = request.origin_t0_us + 10U;
		response.transmit_t2_us = request.origin_t0_us + 20U;
		++response.probe_id;
		const auto unknown = make_heartbeat(
			controller.slot(0U).session_id, response, 1'200U, request.origin_t0_us + 100U);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(unknown.bytes), request.origin_t0_us + 100U, 0U, false).drop_reason);
		EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(clock_before, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_FALSE(controller.slot(0U).heartbeat.clock_filter.valid());
		EXPECT_FALSE(controller.has_output());

		--response.probe_id;
		++response.origin_t0_us;
		const auto wrong_t0 = make_heartbeat(
			controller.slot(0U).session_id, response, 1'201U, request.origin_t0_us + 200U);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(wrong_t0.bytes), request.origin_t0_us + 200U, 0U, false).drop_reason);
		EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(clock_before, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_FALSE(controller.slot(0U).heartbeat.clock_filter.valid());
		EXPECT_FALSE(controller.has_output());

		--response.origin_t0_us;
		const auto exact_time = request.origin_t0_us + 300U;
		const auto exact = make_heartbeat(controller.slot(0U).session_id, response, 1'202U, exact_time);
		ASSERT_EQ(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(exact.bytes), exact_time, 0U, false).drop_reason);
		EXPECT_EQ(exact_time, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(exact_time, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
		std::uint64_t minimum_rtt_us = 0U;
		std::int64_t smoothed_offset_us = 0;
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.minimum_round_trip_time_us(minimum_rtt_us));
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.smoothed_offset_us(smoothed_offset_us));
		EXPECT_EQ(290U, minimum_rtt_us);
		EXPECT_EQ(-135, smoothed_offset_us);
		EXPECT_FALSE(controller.has_output());

		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(exact.bytes), exact_time + 1U, 0U, false).drop_reason);
		EXPECT_EQ(exact_time, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(exact_time, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
		minimum_rtt_us = 0U;
		smoothed_offset_us = 0;
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.minimum_round_trip_time_us(minimum_rtt_us));
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.smoothed_offset_us(smoothed_offset_us));
		EXPECT_EQ(290U, minimum_rtt_us);
		EXPECT_EQ(-135, smoothed_offset_us);
		EXPECT_FALSE(controller.has_output());

		controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
		detail::SessionControllerOutput pending;
		ASSERT_TRUE(controller.peek_output(pending));
		auto discarded = decode_heartbeat(pending);
		controller.complete_output(detail::IoStatus::WouldBlock);
		ASSERT_EQ(0U, controller.slot(0U).heartbeat.probes.in_flight_count());
		discarded.kind = protocol::HeartbeatKind::Response;
		discarded.receive_t1_us = discarded.origin_t0_us + 10U;
		discarded.transmit_t2_us = discarded.origin_t0_us + 20U;
		const auto late_time = discarded.origin_t0_us + 100U;
		const auto late = make_heartbeat(controller.slot(0U).session_id, discarded, 1'203U, late_time);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(late.bytes), late_time, 0U, false).drop_reason);
		EXPECT_EQ(exact_time, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(exact_time, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
		minimum_rtt_us = 0U;
		smoothed_offset_us = 0;
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.minimum_round_trip_time_us(minimum_rtt_us));
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.smoothed_offset_us(smoothed_offset_us));
		EXPECT_EQ(290U, minimum_rtt_us);
		EXPECT_EQ(-135, smoothed_offset_us);
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06HeartbeatContract, OnlyExactlyCorrelatedResponseMayRefreshNetworkAndClockActivity)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x6B6BU};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 62U, 100U, 200U, 1'300U);
	expect_response_rejections_are_mutation_free(controller, peer);
}

template <typename Controller>
void expect_admitted_request_refreshes_before_output_backpressure(Controller& controller,
	const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Fixed heartbeat state is required for the admitted Request audit.";
	} else {
		controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
		detail::SessionControllerOutput before;
		ASSERT_TRUE(controller.peek_output(before));
		const auto probes_before = controller.slot(0U).heartbeat.probes.in_flight_count();
		const auto clock_before = controller.slot(0U).heartbeat.last_valid_clock_response_us;
		protocol::HeartbeatPayload request;
		request.probe_id = 77U;
		request.kind = protocol::HeartbeatKind::Request;
		request.origin_t0_us = before.size + 5'000U;
		const auto now_us = controller.slot(0U).heartbeat.last_valid_network_activity_us + 1'000U;
		const auto datagram = make_heartbeat(controller.slot(0U).session_id, request, 1'400U, now_us);
		const auto result = controller.ingest(peer, view(datagram.bytes), now_us, 0U, false);
		EXPECT_EQ(detail::SessionIngressDropReason::OutputBusy, result.drop_reason);
		EXPECT_EQ(now_us, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "The valid admitted Request refreshes activity before its Response encounters backpressure.";
		EXPECT_EQ(clock_before, controller.slot(0U).heartbeat.last_valid_clock_response_us)
			<< "A Request never claims a correlated clock response.";
		EXPECT_EQ(probes_before, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		detail::SessionControllerOutput after;
		ASSERT_TRUE(controller.peek_output(after));
		EXPECT_EQ(before.endpoint, after.endpoint);
		EXPECT_EQ(before.size, after.size);
		EXPECT_TRUE(std::equal(before.bytes.begin(),
			before.bytes.begin() + static_cast<std::ptrdiff_t>(before.size),
			after.bytes.begin()));
		controller.complete_output(detail::IoStatus::WouldBlock);
		EXPECT_FALSE(controller.has_output());
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(now_us, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(clock_before, controller.slot(0U).heartbeat.last_valid_clock_response_us);
	}
}

TEST(TelemetryWp06HeartbeatContract, AdmittedRequestRefreshesActivityBeforeBusyResponseAndWouldBlock)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x6C6CU};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 63U, 100U, 200U, 1'500U);
	expect_admitted_request_refreshes_before_output_backpressure(controller, peer);
}

template <typename Controller>
protocol::HeartbeatPayload issue_periodic_probe(Controller& controller)
{
	controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
	auto request = decode_heartbeat(pop_output(controller));
	EXPECT_EQ(protocol::HeartbeatKind::Request, request.kind);
	return request;
}

TEST(TelemetryWp06HeartbeatContract, CorrelatedResponsePreservesPriorityOutputAndForeignResponseIsMutationFree)
{
	IdentityHarness ids{0x6C7CU};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 64U, 100U, 200U, 1'600U);
	const auto request = issue_periodic_probe(controller);
	// A second periodic request is a real priority output while the first probe
	// remains outstanding.
	controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
	detail::SessionControllerOutput before{};
	ASSERT_TRUE(controller.peek_output(before));
	const std::vector<std::uint8_t> bytes(before.bytes.begin(), before.bytes.begin() + before.size);
	auto response = request;
	response.kind = protocol::HeartbeatKind::Response;
	response.receive_t1_us = request.origin_t0_us + 10U;
	response.transmit_t2_us = request.origin_t0_us + 20U;
	// The local receive time must follow both remote response timestamps; using
	// the prior activity timestamp would make this otherwise valid sample fail
	// clock-sample validation before the correlation result can be observed.
	const auto now = request.origin_t0_us + 100U;
	const auto activity_before = controller.slot(0U).heartbeat.last_valid_network_activity_us;
	// A tuple-correlated response with impossible timestamps is still untrusted:
	// it must not consume the outstanding probe or disturb the queued priority
	// output before the later valid response is admitted.
	auto impossible = response;
	impossible.receive_t1_us = request.origin_t0_us + 10U;
	impossible.transmit_t2_us = request.origin_t0_us + 210U;
	const auto malformed = make_heartbeat(controller.slot(0U).session_id, impossible, 899U, now);
	EXPECT_NE(detail::SessionIngressDropReason::None,
		controller.ingest(peer, view(malformed.bytes), now, 0U, false).drop_reason);
	EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
	detail::SessionControllerOutput malformed_after{};
	ASSERT_TRUE(controller.peek_output(malformed_after));
	EXPECT_EQ(bytes, std::vector<std::uint8_t>(malformed_after.bytes.begin(), malformed_after.bytes.begin() + malformed_after.size));
	const auto valid = make_heartbeat(controller.slot(0U).session_id, response, 900U, now);
	EXPECT_EQ(detail::SessionIngressDropReason::None,
		controller.ingest(peer, view(valid.bytes), now, 0U, false).drop_reason);
	detail::SessionControllerOutput after{};
	ASSERT_TRUE(controller.peek_output(after));
	EXPECT_EQ(bytes, std::vector<std::uint8_t>(after.bytes.begin(), after.bytes.begin() + after.size));
	const auto activity = controller.slot(0U).heartbeat.last_valid_network_activity_us;
	const auto forged = make_heartbeat(controller.slot(0U).session_id, response, 901U, now + 1U);
	EXPECT_NE(detail::SessionIngressDropReason::None,
		controller.ingest(endpoint(3U), view(forged.bytes), now + 1U, 0U, false).drop_reason);
	EXPECT_EQ(activity, controller.slot(0U).heartbeat.last_valid_network_activity_us);
	ASSERT_TRUE(controller.peek_output(after));
	EXPECT_EQ(bytes, std::vector<std::uint8_t>(after.bytes.begin(), after.bytes.begin() + after.size));
}

template <typename Controller>
std::uint64_t add_reference_clock_sample(Controller& controller,
	const protocol::EndpointKey& peer,
	std::uint32_t packet_sequence)
{
	auto response = issue_periodic_probe(controller);
	response.kind = protocol::HeartbeatKind::Response;
	response.receive_t1_us = response.origin_t0_us + 10U;
	response.transmit_t2_us = response.origin_t0_us + 20U;
	const auto response_time = response.origin_t0_us + 100U;
	const auto datagram =
		make_heartbeat(controller.slot(0U).session_id, response, packet_sequence, response_time);
	EXPECT_EQ(detail::SessionIngressDropReason::None,
		controller.ingest(peer, view(datagram.bytes), response_time, 0U, false).drop_reason);
	return response_time;
}

template <typename Controller>
void expect_payload_invalid_response_preserves_probe(Controller& controller, const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Fixed heartbeat state is required for hostile Response validation.";
	} else {
		const auto request = issue_periodic_probe(controller);
		const auto activity_before = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto clock_before = controller.slot(0U).heartbeat.last_valid_clock_response_us;
		const auto malformed = make_raw_heartbeat(controller.slot(0U).session_id,
			request.probe_id,
			protocol::HeartbeatKind::Response,
			request.origin_t0_us,
			request.origin_t0_us + 20U,
			request.origin_t0_us + 10U,
			1'600U,
			request.origin_t0_us + 100U);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(malformed.bytes), request.origin_t0_us + 100U, 0U, false).drop_reason);
		EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(clock_before, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_FALSE(controller.slot(0U).heartbeat.clock_filter.valid());
	}
}

TEST(TelemetryWp06HeartbeatContract, PayloadInvalidResponseIsRejectedBeforeProbeCorrelation)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x6D6DU};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 64U, 100U, 200U, 1'700U);
	expect_payload_invalid_response_preserves_probe(controller, peer);
}

template <typename Controller>
void expect_negative_round_trip_preserves_probe_and_clock_state(Controller& controller,
	const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Fixed heartbeat state is required for hostile Response validation.";
	} else {
		const auto reference_time = add_reference_clock_sample(controller, peer, 1'800U);
		const auto activity_before = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto request = issue_periodic_probe(controller);
		protocol::HeartbeatPayload response = request;
		response.kind = protocol::HeartbeatKind::Response;
		response.receive_t1_us = request.origin_t0_us + 10U;
		response.transmit_t2_us = request.origin_t0_us + 210U;
		const auto response_time = request.origin_t0_us + 100U;
		const auto datagram =
			make_heartbeat(controller.slot(0U).session_id, response, 1'801U, response_time);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(datagram.bytes), response_time, 0U, false).drop_reason);
		EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(reference_time, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
		std::uint64_t minimum_rtt_us = 0U;
		std::int64_t offset_us = 0;
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.minimum_round_trip_time_us(minimum_rtt_us));
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.smoothed_offset_us(offset_us));
		EXPECT_EQ(90U, minimum_rtt_us);
		EXPECT_EQ(-35, offset_us);
	}
}

TEST(TelemetryWp06HeartbeatContract, NegativeRoundTripPreservesProbeAndPriorClockEvidence)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x6E6EU};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 65U, 100U, 200U, 1'900U);
	expect_negative_round_trip_preserves_probe_and_clock_state(controller, peer);
}

template <typename Controller>
void expect_offset_overflow_invalidates_filter(Controller& controller, const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Fixed heartbeat state is required for hostile Response validation.";
	} else {
		const auto reference_time = add_reference_clock_sample(controller, peer, 2'000U);
		const auto activity_before = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto request = issue_periodic_probe(controller);
		const auto response_time = request.origin_t0_us + 100U;
		const auto overflow = make_raw_heartbeat(controller.slot(0U).session_id,
			request.probe_id,
			protocol::HeartbeatKind::Response,
			request.origin_t0_us,
			std::numeric_limits<std::uint64_t>::max(),
			std::numeric_limits<std::uint64_t>::max(),
			2'001U,
			response_time);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(overflow.bytes), response_time, 0U, false).drop_reason);
		EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(reference_time, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
	}
}

TEST(TelemetryWp06HeartbeatContract, OffsetOverflowPreservesProbeAndPriorClockFilter)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x6F6FU};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 66U, 100U, 200U, 2'100U);
	expect_offset_overflow_invalidates_filter(controller, peer);
}

template <typename Controller>
void expect_time_reversal_marks_stale_without_underflow(Controller& controller,
	const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Fixed heartbeat state is required for hostile Response validation.";
	} else {
		const auto reference_time = add_reference_clock_sample(controller, peer, 2'200U);
		const auto activity_before = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto request = issue_periodic_probe(controller);
		protocol::HeartbeatPayload response = request;
		response.kind = protocol::HeartbeatKind::Response;
		response.receive_t1_us = request.origin_t0_us + 10U;
		response.transmit_t2_us = request.origin_t0_us + 20U;
		const auto reversed_receive_time = request.origin_t0_us - 1U;
		const auto reversed = make_heartbeat(
			controller.slot(0U).session_id, response, 2'201U, reversed_receive_time);
		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(reversed.bytes), reversed_receive_time, 0U, false).drop_reason);
		EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(reference_time, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
		EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress);
		EXPECT_FALSE(controller.slot(0U).heartbeat.clock_stale);

		EXPECT_NE(detail::SessionIngressDropReason::None,
			controller.ingest(peer, view(reversed.bytes), reversed_receive_time + 1U, 0U, false).drop_reason);
		EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		EXPECT_EQ(reference_time, controller.slot(0U).heartbeat.last_valid_clock_response_us);
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());
		EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress)
			<< "A duplicate hostile Response cannot alter semantic readiness.";
		EXPECT_FALSE(controller.slot(0U).heartbeat.clock_stale)
			<< "A duplicate hostile Response cannot alter the clock stale marker.";
	}
}

TEST(TelemetryWp06HeartbeatContract, InitiatorTimeReversalPreservesProbeClockAndReadiness)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x7070U};
	auto controller = make_controller(ids.allocator);
	const auto peer = endpoint();
	(void)establish_ready(controller, peer, 67U, 100U, 200U, 2'300U);
	expect_time_reversal_marks_stale_without_underflow(controller, peer);
}

template <typename Controller>
void expect_multi_client_fairness(Controller& controller)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "The missing HB scheduler prevents a multi-client fairness proof.";
	} else {
		const auto both_due = std::max(controller.slot(0U).heartbeat.next_periodic_due_us,
			controller.slot(1U).heartbeat.next_periodic_due_us);
		controller.service_session_maintenance(both_due);
		detail::SessionControllerOutput first;
		ASSERT_TRUE(controller.peek_output(first));
		controller.complete_output(detail::IoStatus::Complete);
		controller.service_session_maintenance(both_due);
		detail::SessionControllerOutput second;
		ASSERT_TRUE(controller.peek_output(second));
		EXPECT_NE(first.endpoint, second.endpoint);
		controller.complete_output(detail::IoStatus::Complete);
		controller.service_session_maintenance(both_due);
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06HeartbeatContract, SimultaneouslyDueClientsAdvanceOneDatagramPerCallWithoutStarvation)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x7777U, 0x8888U};
	auto controller = make_controller(ids.allocator, 2U);
	(void)establish_ready(controller, endpoint(2U, 42043U), 7U, 100U, 200U, 300U);
	(void)establish_ready(controller, endpoint(3U, 42044U), 8U, 300U, 400U, 400U);
	expect_multi_client_fairness(controller);
}

template <typename Controller>
void expect_cleanup_purges_heartbeat_state(Controller& controller)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Heartbeat cleanup cannot be proved before fixed HB state exists.";
	} else {
		controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
		auto response = decode_heartbeat(pop_output(controller));
		ASSERT_EQ(protocol::HeartbeatKind::Request, response.kind);
		ASSERT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		response.kind = protocol::HeartbeatKind::Response;
		response.receive_t1_us = response.origin_t0_us + 10U;
		response.transmit_t2_us = response.origin_t0_us + 20U;
		const auto response_time = response.origin_t0_us + 100U;
		const auto response_datagram =
			make_heartbeat(controller.slot(0U).session_id, response, 900U, response_time);
		ASSERT_EQ(detail::SessionIngressDropReason::None,
			controller.ingest(endpoint(), view(response_datagram.bytes), response_time, 0U, false).drop_reason);
		ASSERT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		ASSERT_TRUE(controller.slot(0U).heartbeat.clock_filter.valid());

		controller.service_session_maintenance(controller.slot(0U).heartbeat.next_periodic_due_us);
		detail::SessionControllerOutput pending;
		ASSERT_TRUE(controller.peek_output(pending));
		protocol::DatagramView pending_datagram;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram({pending.bytes.data(), pending.size},
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				pending_datagram));
		ASSERT_EQ(protocol::MessageType::Heartbeat, pending_datagram.header.message_type);
		ASSERT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		ASSERT_TRUE(controller.close_slot(0U, detail::SessionCloseReason::MissionDiscontinuity));
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.probes.in_flight_count());
		EXPECT_EQ(0U, controller.slot(0U).heartbeat.clock_filter.sample_count());
		EXPECT_FALSE(controller.slot(0U).heartbeat.clock_filter.valid());
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06HeartbeatContract, ClosingSessionPurgesProbesSamplesAndOwnedHeartbeatOutput)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0x9999U};
	auto controller = make_controller(ids.allocator);
	(void)establish_ready(controller, endpoint(), 9U, 100U, 200U, 500U);
	expect_cleanup_purges_heartbeat_state(controller);
}

template <typename Controller>
void expect_unrelated_close_preserves_heartbeat_output(Controller& controller)
{
	if constexpr (!has_wp06_heartbeat_contract<Controller>::value) {
		FAIL() << "Heartbeat ownership cannot be proved before fixed HB state exists.";
	} else {
		const auto both_due = std::max(controller.slot(0U).heartbeat.next_periodic_due_us,
			controller.slot(1U).heartbeat.next_periodic_due_us);
		controller.service_session_maintenance(both_due);
		detail::SessionControllerOutput before;
		ASSERT_TRUE(controller.peek_output(before));
		const auto owner = before.endpoint == controller.slot(0U).endpoint ? 0U : 1U;
		const auto unrelated = owner == 0U ? 1U : 0U;
		protocol::DatagramView pending_datagram;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram({before.bytes.data(), before.size},
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				pending_datagram));
		ASSERT_EQ(protocol::MessageType::Heartbeat, pending_datagram.header.message_type);
		ASSERT_TRUE(controller.close_slot(unrelated, detail::SessionCloseReason::MissionDiscontinuity));
		detail::SessionControllerOutput after;
		ASSERT_TRUE(controller.peek_output(after));
		EXPECT_EQ(before.endpoint, after.endpoint);
		EXPECT_EQ(before.size, after.size);
		EXPECT_TRUE(std::equal(before.bytes.begin(),
			before.bytes.begin() + static_cast<std::ptrdiff_t>(before.size),
			after.bytes.begin()))
			<< "Closing an unrelated slot preserves the pending heartbeat owner's bytes.";
	}
}

TEST(TelemetryWp06HeartbeatContract, ClosingUnrelatedSessionPreservesPendingHeartbeatOwner)
{
	EXPECT_TRUE(has_wp06_heartbeat_contract<detail::SessionController>::value);
	IdentityHarness ids{0xAAAAU, 0xBBBBU};
	auto controller = make_controller(ids.allocator, 2U);
	(void)establish_ready(controller, endpoint(2U, 42043U), 10U, 100U, 200U, 800U);
	(void)establish_ready(controller, endpoint(3U, 42044U), 11U, 300U, 400U, 900U);
	expect_unrelated_close_preserves_heartbeat_output(controller);
}

template <typename Controller>
void expect_due_reliable_precedes_periodic(Controller& controller,
	const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_ordered_tick_phases<Controller>::value) {
		FAIL() << "SessionController lacks the noexcept timeout and periodic phase seams required to order timeout -> REL -> periodic.";
	} else {
		const auto request = make_hello(7'001U, 101U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(peer, view(request.bytes), 1'000U, 0U, false).disposition);
		const auto welcome = pop_output(controller);
		const auto proof = make_ack(welcome, 102U);
		ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
			controller.ingest(peer, view(proof.bytes), 2'000U, 0U, false).disposition);
		const auto begin = pop_output(controller);
		ASSERT_EQ(protocol::MessageType::SessionBegin, begin.datagram.header.message_type);
		const auto both_due = controller.slot(0U).heartbeat.next_periodic_due_us;

		controller.service_timeouts(both_due);
		controller.service_reliability(both_due);
		detail::SessionControllerOutput selected;
		ASSERT_TRUE(controller.peek_output(selected));
		protocol::DatagramView selected_datagram;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram({selected.bytes.data(), selected.size},
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				selected_datagram));
		EXPECT_EQ(protocol::MessageType::SessionBegin, selected_datagram.header.message_type)
			<< "A due reliable control message must win over a periodic heartbeat.";

		controller.service_periodic(both_due);
		detail::SessionControllerOutput still_selected;
		ASSERT_TRUE(controller.peek_output(still_selected));
		EXPECT_EQ(selected.size, still_selected.size);
		EXPECT_TRUE(std::equal(selected.bytes.begin(),
			selected.bytes.begin() + static_cast<std::ptrdiff_t>(selected.size),
			still_selected.bytes.begin()));

		controller.complete_output(detail::IoStatus::Complete);
		controller.service_periodic(both_due);
		detail::SessionControllerOutput periodic;
		ASSERT_TRUE(controller.peek_output(periodic));
		protocol::DatagramView periodic_datagram;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram({periodic.bytes.data(), periodic.size},
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				periodic_datagram));
		EXPECT_EQ(protocol::MessageType::Heartbeat, periodic_datagram.header.message_type)
			<< "Periodic work becomes eligible only after the higher-priority output completes.";
	}
}

TEST(TelemetryWp06HeartbeatContract, OrderedPhasesGiveDueReliablePriorityOverPeriodicUntilCompletion)
{
	EXPECT_TRUE(has_wp06_ordered_tick_phases<detail::SessionController>::value);
	IdentityHarness ids{0x7101U};
	auto controller = make_controller(ids.allocator);
	expect_due_reliable_precedes_periodic(controller, endpoint());
}

template <typename Controller>
void expect_timeout_precedes_reliable_and_periodic(Controller& controller,
	const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_ordered_tick_phases<Controller>::value) {
		FAIL() << "SessionController lacks the separate timeout and periodic phase seams.";
	} else {
		const auto request = make_hello(7'002U, 111U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(peer, view(request.bytes), 1'000U, 0U, false).disposition);
		const auto welcome = pop_output(controller);
		const auto proof = make_ack(welcome, 112U);
		ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
			controller.ingest(peer, view(proof.bytes), 2'000U, 0U, false).disposition);
		detail::SessionControllerOutput discarded;
		ASSERT_TRUE(controller.pop_output(discarded));
		const auto disconnect_due = controller.slot(0U).heartbeat.last_valid_network_activity_us +
			controller.slot(0U).heartbeat.disconnect_timeout_us;

		controller.service_timeouts(disconnect_due);
		controller.service_reliability(disconnect_due);
		controller.service_periodic(disconnect_due);

		EXPECT_EQ(0U, controller.active_slots());
		EXPECT_FALSE(controller.has_output())
			<< "Timeout cleanup must close the slot before due REL or heartbeat can be selected.";
		EXPECT_EQ(0U, controller.owned_usage().reliable_items);
	}
}

TEST(TelemetryWp06HeartbeatContract, OrderedPhasesCloseTimedOutSessionBeforeReliableOrPeriodicSelection)
{
	IdentityHarness ids{0x7201U};
	auto controller = make_controller(ids.allocator);
	expect_timeout_precedes_reliable_and_periodic(controller, endpoint());
}

template <typename Controller>
void expect_periodic_hitch_coalesces_without_catchup(Controller& controller,
	const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_ordered_tick_phases<Controller>::value) {
		FAIL() << "SessionController lacks the separate periodic phase seam.";
	} else {
		(void)establish_ready(controller, peer, 7'003U, 1'000U, 2'000U, 121U);
		const auto interval_us =
			static_cast<std::uint64_t>(controller.slot(0U).heartbeat.negotiated_interval_ms) * 1000U;
		const std::uint64_t hitch_now = 9'000'000U;
		ASSERT_LT(hitch_now,
			controller.slot(0U).heartbeat.last_valid_network_activity_us +
				controller.slot(0U).heartbeat.disconnect_timeout_us);

		controller.service_periodic(hitch_now);
		ASSERT_TRUE(controller.has_output());
		EXPECT_EQ(hitch_now + interval_us, controller.slot(0U).heartbeat.next_periodic_due_us);
		controller.complete_output(detail::IoStatus::Complete);
		controller.service_periodic(hitch_now);
		EXPECT_FALSE(controller.has_output()) << "A hitch coalesces missed periods into one recent heartbeat.";
	}
}

TEST(TelemetryWp06HeartbeatContract, SeparatePeriodicPhaseAdvancesFromNowAndNeverCatchesUpInABurst)
{
	IdentityHarness ids{0x7301U};
	auto controller = make_controller(ids.allocator);
	expect_periodic_hitch_coalesces_without_catchup(controller, endpoint());
}

template <typename Controller>
void expect_purge_all_releases_every_session_scope(Controller& controller,
	IdentityHarness& ids)
{
	if constexpr (!has_wp06_ordered_tick_phases<Controller>::value) {
		FAIL() << "SessionController lacks purge_all(reason) and the ordered maintenance seams.";
	} else {
		const auto first_peer = endpoint(2U, 42043U);
		const auto begin = establish_ready(controller, first_peer, 8'001U, 1'000U, 2'000U, 201U);
		const auto first_session_id = begin.datagram.header.session_id;
		EXPECT_EQ(0x2222U, first_session_id)
			<< "The startup-reserved 0x1111 candidate must be skipped by the controller allocator.";

		const auto first_due = controller.slot(0U).heartbeat.next_periodic_due_us;
		controller.service_periodic(first_due);
		const auto probe_request_output = pop_output(controller);
		const auto probe_request = decode_heartbeat(probe_request_output);
		protocol::HeartbeatPayload probe_response = probe_request;
		probe_response.kind = protocol::HeartbeatKind::Response;
		probe_response.receive_t1_us = probe_request.origin_t0_us + 10U;
		probe_response.transmit_t2_us = probe_request.origin_t0_us + 20U;
		const auto response_time = probe_request.origin_t0_us + 100U;
		const auto response = make_heartbeat(first_session_id, probe_response, 204U, response_time);
		ASSERT_EQ(detail::SessionIngressDropReason::None,
			controller.ingest(first_peer, view(response.bytes), response_time, 0U, false).drop_reason);
		ASSERT_EQ(1U, controller.slot(0U).heartbeat.clock_filter.sample_count());

		for (std::uint64_t offset = 0U; offset < 3U; ++offset) {
			const auto request = make_hello(8'010U + offset, static_cast<std::uint32_t>(210U + offset));
			ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
				controller
					.ingest(endpoint(2U, static_cast<std::uint16_t>(42044U + offset)),
						view(request.bytes),
						response_time + 1'000U + offset,
						0U,
						false)
					.disposition);
			detail::SessionControllerOutput unsupported_welcome;
			ASSERT_TRUE(controller.pop_output(unsupported_welcome));
		}
		const auto limited_request = make_hello(8'020U, 220U);
		const auto limited =
			controller.ingest(endpoint(2U, 42050U), view(limited_request.bytes), response_time + 2'000U, 0U, false);
		ASSERT_EQ(detail::SessionIngressDropReason::NoClientSlot, limited.drop_reason);
		ASSERT_EQ(4U, controller.active_slots());
		ASSERT_EQ(4U, controller.handshake_cache_entries());
		ASSERT_EQ(3U, controller.preproof_account_count());
		ASSERT_GT(controller.owned_usage().reliable_items, 0U);

		const auto pending_due = controller.slot(0U).heartbeat.next_periodic_due_us;
		controller.service_periodic(pending_due);
		ASSERT_TRUE(controller.has_output());
		ASSERT_EQ(1U, controller.slot(0U).heartbeat.probes.in_flight_count());
		const auto process_ids_before_purge = ids.registry.used_count();
		ASSERT_EQ(5U, process_ids_before_purge);

		controller.purge_all(detail::SessionCloseReason::MissionDiscontinuity);

		EXPECT_EQ(0U, controller.active_slots());
		EXPECT_EQ(0U, controller.handshake_cache_entries());
		EXPECT_EQ(0U, controller.preproof_account_count());
		EXPECT_EQ(detail::SessionControllerOwnedUsage{}, controller.owned_usage());
		EXPECT_FALSE(controller.has_output());
		for (std::size_t slot = 0U; slot < 4U; ++slot) {
			EXPECT_EQ(0U, controller.slot(slot).heartbeat.probes.in_flight_count());
			EXPECT_EQ(0U, controller.slot(slot).heartbeat.clock_filter.sample_count());
		}
		EXPECT_EQ(process_ids_before_purge, ids.registry.used_count())
			<< "Mission purge must preserve the process-wide no-reuse registry.";

		const auto after_purge = make_hello(8'021U, 230U);
		const auto new_first_peer = endpoint(3U, 42043U);
		const auto admitted = controller.ingest(new_first_peer, view(after_purge.bytes), pending_due, 0U, false);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued, admitted.disposition)
			<< "A fresh source remains eligible after mission-scoped state is purged.";
		const auto new_welcome = pop_output(controller);
		EXPECT_EQ(0x6666U, new_welcome.datagram.header.session_id);
		EXPECT_NE(first_session_id, new_welcome.datagram.header.session_id);
		const auto first_proof = make_ack(new_welcome, 231U);
		ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
			controller.ingest(new_first_peer, view(first_proof.bytes), pending_due + 1U, 0U, false).disposition);
		(void)pop_output(controller);

		const auto second_peer = endpoint(4U, 42044U);
		const auto second_after_purge = make_hello(8'030U, 232U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(second_peer, view(second_after_purge.bytes), pending_due + 1U, 0U, false).disposition);
		const auto second_welcome = pop_output(controller);
		EXPECT_EQ(0x7777U, second_welcome.datagram.header.session_id);
		const auto second_proof = make_ack(second_welcome, 233U);
		ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
			controller.ingest(second_peer, view(second_proof.bytes), pending_due + 1U, 0U, false).disposition);
		(void)pop_output(controller);
		ASSERT_EQ(2U, controller.active_slots());
		EXPECT_EQ(process_ids_before_purge + 2U, ids.registry.used_count());

		const auto simultaneous_due = controller.slot(0U).heartbeat.next_periodic_due_us;
		ASSERT_EQ(simultaneous_due, controller.slot(1U).heartbeat.next_periodic_due_us);
		controller.service_reliability(simultaneous_due);
		detail::SessionControllerOutput reliable;
		ASSERT_TRUE(controller.peek_output(reliable));
		protocol::DatagramView reliable_datagram;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram({reliable.bytes.data(), reliable.size},
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				reliable_datagram));
		EXPECT_EQ(0x6666U, reliable_datagram.header.session_id)
			<< "purge_all must restart the reliable round-robin cursor at slot zero.";
		controller.complete_output(detail::IoStatus::Complete);

		controller.service_periodic(simultaneous_due);
		detail::SessionControllerOutput periodic;
		ASSERT_TRUE(controller.peek_output(periodic));
		protocol::DatagramView periodic_datagram;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram({periodic.bytes.data(), periodic.size},
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				periodic_datagram));
		EXPECT_EQ(0x6666U, periodic_datagram.header.session_id)
			<< "purge_all must restart the heartbeat round-robin cursor at slot zero.";
	}
}

TEST(TelemetryWp06HeartbeatContract, PurgeAllClearsSessionCachesPreproofOutputHeartbeatReliableAndCursorsButPreservesIds)
{
	IdentityHarness ids{0x1111U, 0x2222U, 0x3333U, 0x4444U, 0x5555U, 0x6666U, 0x7777U};
	ASSERT_EQ(detail::SessionIdRegistrationStatus::Registered, ids.registry.register_candidate(0x1111U));
	auto controller = make_controller(ids.allocator, 4U);
	expect_purge_all_releases_every_session_scope(controller, ids);
}

template <typename Controller>
void expect_mission_purge_preserves_presession_source_quota(Controller& controller,
	IdentityHarness& ids)
{
	if constexpr (!has_wp06_ordered_tick_phases<Controller>::value) {
		FAIL() << "SessionController lacks purge_all(reason).";
	} else {
		const auto source_octet = 9U;
		for (std::uint64_t nonce = 0U; nonce < protocol::HelloRateLimit.burst_tokens; ++nonce) {
			const auto request = make_hello(9'000U + nonce,
				static_cast<std::uint32_t>(300U + nonce),
				protocol::VersionMinorV1_0,
				protocol::VersionMinorV1_0);
			ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
				controller
					.ingest(endpoint(source_octet, static_cast<std::uint16_t>(43000U + nonce)),
						view(request.bytes),
						10'000U,
						0U,
						false)
					.disposition);
			detail::SessionControllerOutput unsupported_welcome;
			ASSERT_TRUE(controller.pop_output(unsupported_welcome));
		}
		ASSERT_EQ(protocol::HelloRateLimit.burst_tokens, controller.handshake_cache_entries());
		ASSERT_EQ(protocol::HelloRateLimit.burst_tokens, controller.preproof_account_count());
		ASSERT_EQ(0U, ids.random.index);

		controller.purge_all(detail::SessionCloseReason::MissionDiscontinuity);
		ASSERT_EQ(0U, controller.handshake_cache_entries());
		ASSERT_EQ(0U, controller.preproof_account_count());
		ASSERT_EQ(detail::SessionControllerOwnedUsage{}, controller.owned_usage());
		const auto before = controller.owned_usage();
		const auto used_ids_before = ids.registry.used_count();

		const auto still_limited = make_hello(9'999U,
			399U,
			protocol::VersionMinorV1_0,
			protocol::VersionMinorV1_0);
		const auto result = controller.ingest(
			endpoint(source_octet, 44000U), view(still_limited.bytes), 10'001U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
		EXPECT_EQ(detail::SessionIngressDropReason::HelloRateLimited, result.drop_reason)
			<< "Mission purge must preserve the process-scoped HELLO bucket for a source address.";
		EXPECT_EQ(before, controller.owned_usage());
		EXPECT_EQ(used_ids_before, ids.registry.used_count());
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06HeartbeatContract, MissionPurgePreservesSaturatedHelloSourceQuotaWhileClearingHandshakeState)
{
	IdentityHarness ids{0x9101U};
	auto controller = make_controller(ids.allocator);
	expect_mission_purge_preserves_presession_source_quota(controller, ids);
}

template <typename Controller>
void expect_mission_purge_preserves_global_monotonic_watermark(Controller& controller,
	IdentityHarness& ids)
{
	if constexpr (!has_wp06_ordered_tick_phases<Controller>::value) {
		FAIL() << "SessionController lacks purge_all(reason).";
	} else {
		const auto before_purge = make_hello(9'100U,
			410U,
			protocol::VersionMinorV1_0,
			protocol::VersionMinorV1_0);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(10U), view(before_purge.bytes), 20'000U, 0U, false).disposition);
		detail::SessionControllerOutput unsupported_welcome;
		ASSERT_TRUE(controller.pop_output(unsupported_welcome));
		controller.purge_all(detail::SessionCloseReason::MissionDiscontinuity);
		const auto before = controller.owned_usage();
		const auto used_ids_before = ids.registry.used_count();

		const auto regressed = make_hello(9'101U,
			411U,
			protocol::VersionMinorV1_0,
			protocol::VersionMinorV1_0);
		const auto result =
			controller.ingest(endpoint(11U), view(regressed.bytes), 19'999U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
		EXPECT_EQ(detail::SessionIngressDropReason::HelloRateLimited, result.drop_reason)
			<< "Mission purge must not reset the global limiter watermark to zero.";
		EXPECT_EQ(before, controller.owned_usage()) << "A regressed post-purge datagram is mutation-free.";
		EXPECT_EQ(used_ids_before, ids.registry.used_count());
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06HeartbeatContract, MissionPurgePreservesGlobalLimiterTimeAndRejectsRegressionWithoutMutation)
{
	IdentityHarness ids{0x9201U};
	auto controller = make_controller(ids.allocator);
	expect_mission_purge_preserves_global_monotonic_watermark(controller, ids);
}

template <typename Controller>
void expect_mission_purge_reclaims_session_and_target_admission(Controller& controller)
{
	if constexpr (!has_wp06_ordered_tick_phases<Controller>::value) {
		FAIL() << "SessionController lacks purge_all(reason).";
	} else {
		(void)establish_ready(controller, endpoint(20U), 9'200U, 30'000U, 30'001U, 420U);
		controller.purge_all(detail::SessionCloseReason::MissionDiscontinuity);
		ASSERT_EQ(0U, controller.active_slots());

		for (std::size_t slot = 0U; slot < 4U; ++slot) {
			SCOPED_TRACE(slot);
			const auto peer = endpoint(static_cast<std::uint8_t>(21U + slot));
			const auto request = make_hello(9'210U + slot, static_cast<std::uint32_t>(430U + slot * 3U));
			const auto hello_time = 31'000U + slot * 20U;
			ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
				controller.ingest(peer, view(request.bytes), hello_time, 0U, false).disposition);
			const auto welcome = pop_output(controller);
			const auto proof = make_ack(welcome, static_cast<std::uint32_t>(431U + slot * 3U));
			ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
				controller.ingest(peer, view(proof.bytes), hello_time + 10U, 0U, false).disposition)
				<< "Old session/target limiter ownership must not consume one of the four new admission slots.";
			(void)pop_output(controller);
		}
		EXPECT_EQ(4U, controller.active_slots());
	}
}

TEST(TelemetryWp06HeartbeatContract, MissionPurgeReclaimsOldSessionAndTargetLimiterAdmissionCapacity)
{
	IdentityHarness ids{0x9301U, 0x9302U, 0x9303U, 0x9304U, 0x9305U};
	auto controller = make_controller(ids.allocator, 4U);
	expect_mission_purge_reclaims_session_and_target_admission(controller);
}

} // namespace
