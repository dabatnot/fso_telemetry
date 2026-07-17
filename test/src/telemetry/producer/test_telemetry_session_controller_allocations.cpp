#include "telemetry/session_controller.h"

#include "telemetry/native_session_runtime.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/transport.h"
#include "telemetry/runtime.h"
#include "telemetry/startup_budget.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<bool> allocation_probe_armed{false};
std::atomic<std::uint64_t> allocation_probe_attempts{0U};

void observe_allocation() noexcept
{
	if (allocation_probe_armed.load(std::memory_order_relaxed)) {
		allocation_probe_attempts.fetch_add(1U, std::memory_order_relaxed);
	}
}

void* allocate_unaligned(std::size_t size)
{
	observe_allocation();
	if (auto* allocation = std::malloc(size == 0U ? 1U : size)) {
		return allocation;
	}
	throw std::bad_alloc{};
}

void* allocate_aligned(std::size_t size, std::size_t alignment)
{
	observe_allocation();
	void* allocation = nullptr;
#if defined(_MSC_VER)
	allocation = _aligned_malloc(size == 0U ? 1U : size, alignment);
#else
	if (posix_memalign(&allocation, alignment, size == 0U ? 1U : size) != 0) {
		allocation = nullptr;
	}
#endif
	if (allocation == nullptr) {
		throw std::bad_alloc{};
	}
	return allocation;
}

void free_aligned(void* allocation) noexcept
{
#if defined(_MSC_VER)
	_aligned_free(allocation);
#else
	std::free(allocation);
#endif
}

void arm_allocation_probe() noexcept
{
	allocation_probe_attempts.store(0U, std::memory_order_relaxed);
	allocation_probe_armed.store(true, std::memory_order_relaxed);
}

std::uint64_t disarm_allocation_probe() noexcept
{
	allocation_probe_armed.store(false, std::memory_order_relaxed);
	return allocation_probe_attempts.load(std::memory_order_relaxed);
}

} // namespace

void* operator new(std::size_t size)
{
	return allocate_unaligned(size);
}

void* operator new[](std::size_t size)
{
	return allocate_unaligned(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	try {
		return allocate_unaligned(size);
	} catch (...) {
		return nullptr;
	}
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
	try {
		return allocate_unaligned(size);
	} catch (...) {
		return nullptr;
	}
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
	return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
	return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	try {
		return allocate_aligned(size, static_cast<std::size_t>(alignment));
	} catch (...) {
		return nullptr;
	}
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	try {
		return allocate_aligned(size, static_cast<std::size_t>(alignment));
	} catch (...) {
		return nullptr;
	}
}

void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete(void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::align_val_t) noexcept { free_aligned(allocation); }
void operator delete[](void* allocation, std::align_val_t) noexcept { free_aligned(allocation); }
void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept { free_aligned(allocation); }
void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept { free_aligned(allocation); }
void operator delete(void* allocation, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(allocation); }
void operator delete[](void* allocation, std::align_val_t, const std::nothrow_t&) noexcept
{
	free_aligned(allocation);
}

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

struct FixedRandom final : detail::RandomSource {
	std::array<std::uint64_t, 4U> draws{};
	std::size_t count = 0U;
	std::size_t index = 0U;
	bool next_u64(std::uint64_t& output) noexcept override
	{
		if (index >= count) {
			return false;
		}
		output = draws[index++];
		return true;
	}
};

struct Datagram {
	std::array<std::uint8_t, protocol::MaxDatagramSize> bytes{};
	std::size_t size = 0U;
};

protocol::EndpointKey test_endpoint()
{
	return protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 2U}, 42043U);
}

Datagram encode_message(protocol::TelemetryDatagramHeader header, protocol::ByteView payload)
{
	Datagram result;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{header.version_minor, header.version_minor},
			payload,
			{result.bytes.data(), result.bytes.size()},
			result.size));
	return result;
}

Datagram make_hello()
{
	protocol::HelloPayload hello;
	hello.client_nonce = 0x1234U;
	hello.client_send_t0_us = 1'000U;
	hello.min_major = protocol::VersionMajor;
	hello.max_major = protocol::VersionMajor;
	hello.min_minor = protocol::VersionMinorV1_1;
	hello.max_minor = protocol::VersionMinorV1_1;
	hello.requested_visibility_mode = protocol::VisibilityMode::Cockpit;
	hello.requested_heartbeat_ms = 1000U;
	std::array<std::uint8_t, protocol::HelloPayloadPrefixSize> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_hello_payload(hello, {payload.data(), payload.size()}, written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Hello;
	header.packet_sequence = 7U;
	header.sent_time_us = 1'000U;
	header.message_id = 1U;
	header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	return encode_message(header, {payload.data(), written});
}

protocol::DatagramView decode_output(const detail::SessionControllerOutput& output)
{
	protocol::DatagramView view;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram({output.bytes.data(), output.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			view));
	return view;
}

Datagram make_ack(const protocol::DatagramView& target, std::uint32_t packet_sequence = 8U)
{
	protocol::AckPayload ack;
	ack.target_message_id = target.header.message_id;
	ack.target_message_type = target.header.message_type;
	ack.ack_flags = protocol::KnownAckFlags;
	ack.target_fragment_count = target.header.fragment_count;
	ack.target_message_crc32 = target.header.message_crc32;
	std::array<std::uint8_t, protocol::AckPayloadSize> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_ack_payload(ack, {payload.data(), payload.size()}, written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = target.header.session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = 2'000U;
	header.message_id = 2U;
	header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	return encode_message(header, {payload.data(), written});
}

Datagram make_heartbeat(std::uint64_t session_id,
	const protocol::HeartbeatPayload& heartbeat,
	std::uint32_t packet_sequence,
	std::uint64_t sent_time_us)
{
	std::array<std::uint8_t, protocol::HeartbeatPayloadSize> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_heartbeat_payload(heartbeat, {payload.data(), payload.size()}, written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Heartbeat;
	header.session_id = session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = sent_time_us;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	return encode_message(header, {payload.data(), written});
}

Datagram make_nack(const protocol::DatagramView& target)
{
	std::array<std::uint8_t, 1U> bitmap{{1U}};
	protocol::NackPayload nack;
	nack.target_message_id = target.header.message_id;
	nack.target_message_type = target.header.message_type;
	nack.reason = protocol::NackReason::MissingFragments;
	nack.target_fragment_count = target.header.fragment_count;
	nack.target_message_crc32 = target.header.message_crc32;
	nack.missing_bitmap = {bitmap.data(), bitmap.size()};
	std::array<std::uint8_t, protocol::NackPayloadPrefixSize + 1U> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_nack_payload(nack, {payload.data(), payload.size()}, written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Nack;
	header.session_id = target.header.session_id;
	header.packet_sequence = 9U;
	header.sent_time_us = 3'000U;
	header.message_id = 3U;
	header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	return encode_message(header, {payload.data(), written});
}

template <typename Controller, typename = void>
struct has_wp06_reliability_service_and_transactional_egress : std::false_type {};

template <typename Controller>
struct has_wp06_reliability_service_and_transactional_egress<Controller,
	std::void_t<decltype(std::declval<Controller&>().service_reliability(std::declval<std::uint64_t>())),
		decltype(std::declval<const Controller&>().peek_output(std::declval<detail::SessionControllerOutput&>())),
		decltype(std::declval<Controller&>().complete_output(std::declval<detail::IoStatus>()))>> : std::true_type {};

template <typename Controller, typename = void>
struct has_wp06_heartbeat_service_and_fixed_state : std::false_type {};

template <typename Controller>
struct has_wp06_heartbeat_service_and_fixed_state<Controller,
	std::void_t<decltype(std::declval<Controller&>().service_session_maintenance(
			std::declval<std::uint64_t>())),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.probes.in_flight_count()),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.clock_filter.sample_count()),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.negotiated_interval_ms),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.next_periodic_due_us),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.stale_timeout_us),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.disconnect_timeout_us),
		decltype(std::declval<const Controller&>().slot(0U).heartbeat.last_valid_network_activity_us)>>
	: std::true_type {};

template <typename Controller, typename = void>
struct has_wp06_ordered_phases_and_purge : std::false_type {};

template <typename Controller>
struct has_wp06_ordered_phases_and_purge<Controller,
	std::void_t<decltype(std::declval<Controller&>().service_timeouts(std::declval<std::uint64_t>())),
		decltype(std::declval<Controller&>().service_periodic(std::declval<std::uint64_t>())),
		decltype(std::declval<Controller&>().purge_all(std::declval<detail::SessionCloseReason>()))>>
	: std::true_type {};

TEST(TelemetryWp06AllocationContract, HelloAckNackAndRetransmissionAllocateNothingAfterReady)
{
	FixedRandom id_random;
	id_random.draws[0] = 0x1111U;
	id_random.count = 1U;
	detail::SessionIdRegistry registry;
	ASSERT_TRUE(registry.allocate_storage());
	detail::SessionIdAllocator ids(id_random, registry);
	FixedRandom sequences;
	sequences.draws[0] = 0x10203040U;
	sequences.count = 1U;
	detail::SessionControllerConfig config;
	config.max_clients = 1U;
	config.producer_id = 0x12345678U;
	config.security.enabled = true;
	config.security.port = 42042U;
	config.security.bind_mode = protocol::NetworkBindMode::LoopbackOnly;
	config.security.resources.max_clients = 1U;
	config.security.resources.global_state_reassembly_bytes = protocol::MaxStateReassemblyBytesPerClient;
	detail::SessionController controller;
	ASSERT_EQ(detail::SessionControllerConfigureResult::Ready,
		detail::SessionController::configure(config, ids, sequences, 0U, nullptr, controller));
	ASSERT_EQ(0U, controller.owned_capacity().dynamic_allocations_after_ready);

	const auto hello = make_hello();
	detail::SessionControllerOutput welcome_output;
	arm_allocation_probe();
	const auto hello_result = controller.ingest(
		test_endpoint(), {hello.bytes.data(), hello.size}, 1'000U, 0U, false);
	const auto popped_welcome = controller.pop_output(welcome_output);
	const auto hello_allocations = disarm_allocation_probe();
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued, hello_result.disposition);
	ASSERT_TRUE(popped_welcome);
	EXPECT_EQ(0U, hello_allocations) << "HELLO/WELCOME retained state allocated after Ready.";

	const auto welcome = decode_output(welcome_output);
	const auto ack = make_ack(welcome);
	detail::SessionControllerOutput begin_output;
	arm_allocation_probe();
	const auto ack_result = controller.ingest(test_endpoint(), {ack.bytes.data(), ack.size}, 2'000U, 0U, false);
	const auto popped_begin = controller.pop_output(begin_output);
	const auto ack_allocations = disarm_allocation_probe();
	ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied, ack_result.disposition);
	ASSERT_TRUE(popped_begin);
	EXPECT_EQ(0U, ack_allocations) << "ACK/SESSION_BEGIN retained state allocated after Ready.";

	const auto begin = decode_output(begin_output);
	const auto nack = make_nack(begin);
	detail::SessionControllerOutput retransmission_output;
	arm_allocation_probe();
	const auto nack_result = controller.ingest(test_endpoint(), {nack.bytes.data(), nack.size}, 3'000U, 0U, false);
	const auto popped_retransmission = controller.pop_output(retransmission_output);
	const auto nack_allocations = disarm_allocation_probe();
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued, nack_result.disposition);
	ASSERT_TRUE(popped_retransmission);
	EXPECT_EQ(0U, nack_allocations) << "NACK/retransmission allocated after Ready.";
}

template <typename Controller>
void expect_reliability_service_allocates_nothing(Controller& controller,
	const detail::SessionControllerOutput& retained_welcome)
{
	if constexpr (!has_wp06_reliability_service_and_transactional_egress<Controller>::value) {
		FAIL() << "The WP06 reliability service/transactional egress seams are absent.";
	} else {
		const auto welcome = decode_output(retained_welcome);
		const auto due = 1'000U + protocol::reliable_retry_delay_us(protocol::ReliableDefaultRtoUs,
			0x4653544c5f52544fULL,
			welcome.header.session_id,
			welcome.header.message_id,
			0U);
		detail::SessionControllerOutput peeked;
		arm_allocation_probe();
		controller.service_reliability(due);
		const auto exposed = controller.peek_output(peeked);
		controller.complete_output(detail::IoStatus::WouldBlock);
		const auto retained = controller.peek_output(peeked);
		controller.complete_output(detail::IoStatus::Complete);
		const auto allocations = disarm_allocation_probe();
		EXPECT_TRUE(exposed);
		EXPECT_TRUE(retained);
		EXPECT_EQ(0U, allocations) << "RTO service or WouldBlock completion allocated after Ready.";
	}
}

TEST(TelemetryWp06AllocationContract, ReliabilityServiceAndWouldBlockAllocateNothingAfterReady)
{
	EXPECT_TRUE(has_wp06_reliability_service_and_transactional_egress<detail::SessionController>::value);
	FixedRandom id_random;
	id_random.draws[0] = 0x7777U;
	id_random.count = 1U;
	detail::SessionIdRegistry registry;
	ASSERT_TRUE(registry.allocate_storage());
	detail::SessionIdAllocator ids(id_random, registry);
	FixedRandom sequences;
	sequences.draws[0] = 0x20304050U;
	sequences.count = 1U;
	detail::SessionControllerConfig config;
	config.max_clients = 1U;
	config.producer_id = 0x12345678U;
	config.security.enabled = true;
	config.security.port = 42042U;
	config.security.bind_mode = protocol::NetworkBindMode::LoopbackOnly;
	config.security.resources.max_clients = 1U;
	config.security.resources.global_state_reassembly_bytes = protocol::MaxStateReassemblyBytesPerClient;
	detail::SessionController controller;
	ASSERT_EQ(detail::SessionControllerConfigureResult::Ready,
		detail::SessionController::configure(config, ids, sequences, 0U, nullptr, controller));
	const auto hello = make_hello();
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(test_endpoint(), {hello.bytes.data(), hello.size}, 1'000U, 0U, false).disposition);
	detail::SessionControllerOutput welcome;
	ASSERT_TRUE(controller.pop_output(welcome));
	expect_reliability_service_allocates_nothing(controller, welcome);
}

template <typename Controller>
void expect_heartbeat_paths_allocate_nothing(Controller& controller, const protocol::EndpointKey& peer)
{
	if constexpr (!has_wp06_heartbeat_service_and_fixed_state<Controller>::value) {
		FAIL() << "The WP06 heartbeat service/fixed slot state seams are absent.";
	} else {
		ASSERT_EQ(1000U, controller.slot(0U).heartbeat.negotiated_interval_ms);
		ASSERT_EQ(3'000'000U, controller.slot(0U).heartbeat.stale_timeout_us);
		ASSERT_EQ(10'000'000U, controller.slot(0U).heartbeat.disconnect_timeout_us);
		ASSERT_EQ(1'002'000U, controller.slot(0U).heartbeat.next_periodic_due_us);
		const auto due = controller.slot(0U).heartbeat.next_periodic_due_us;
		detail::SessionControllerOutput request_output;
		arm_allocation_probe();
		controller.service_session_maintenance(due);
		const auto exposed = controller.peek_output(request_output);
		controller.complete_output(detail::IoStatus::Complete);
		const auto cadence_allocations = disarm_allocation_probe();
		ASSERT_TRUE(exposed);
		EXPECT_EQ(0U, cadence_allocations) << "Periodic heartbeat cadence allocated after Ready.";

		const auto request_datagram = decode_output(request_output);
		protocol::HeartbeatPayload request;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_heartbeat_payload(request_datagram.payload, request));
		ASSERT_EQ(protocol::HeartbeatKind::Request, request.kind);
		protocol::HeartbeatPayload response = request;
		response.kind = protocol::HeartbeatKind::Response;
		response.receive_t1_us = request.origin_t0_us + 10U;
		response.transmit_t2_us = request.origin_t0_us + 20U;
		const auto response_time = request.origin_t0_us + 100U;
		const auto response_datagram = make_heartbeat(
			controller.slot(0U).session_id, response, 30U, response_time);

		arm_allocation_probe();
		const auto response_result = controller.ingest(peer,
			{response_datagram.bytes.data(), response_datagram.size}, response_time, 0U, false);
		const auto response_allocations = disarm_allocation_probe();
		EXPECT_EQ(detail::SessionIngressDropReason::None, response_result.drop_reason);
		EXPECT_EQ(0U, response_allocations) << "Heartbeat correlation/filter update allocated after Ready.";

		const auto next_due = controller.slot(0U).heartbeat.next_periodic_due_us;
		arm_allocation_probe();
		controller.service_session_maintenance(next_due);
		const auto would_block_exposed = controller.peek_output(request_output);
		controller.complete_output(detail::IoStatus::WouldBlock);
		const auto backpressure_allocations = disarm_allocation_probe();
		EXPECT_TRUE(would_block_exposed);
		EXPECT_EQ(0U, backpressure_allocations) << "Heartbeat WouldBlock cleanup allocated after Ready.";

		const auto stale_at = response_time + controller.slot(0U).heartbeat.stale_timeout_us;
		const auto disconnect_at = response_time + controller.slot(0U).heartbeat.disconnect_timeout_us;
		arm_allocation_probe();
		controller.service_session_maintenance(stale_at);
		controller.service_session_maintenance(disconnect_at);
		const auto timeout_allocations = disarm_allocation_probe();
		EXPECT_EQ(0U, timeout_allocations) << "Heartbeat stale/disconnect cleanup allocated after Ready.";
	}
}

TEST(TelemetryWp06AllocationContract, HeartbeatCadenceResponseFilterAndTimeoutAllocateNothingAfterReady)
{
	EXPECT_TRUE(has_wp06_heartbeat_service_and_fixed_state<detail::SessionController>::value);
	FixedRandom id_random;
	id_random.draws[0] = 0x8888U;
	id_random.count = 1U;
	detail::SessionIdRegistry registry;
	ASSERT_TRUE(registry.allocate_storage());
	detail::SessionIdAllocator ids(id_random, registry);
	FixedRandom sequences;
	sequences.draws[0] = 0x30405060U;
	sequences.count = 1U;
	detail::SessionControllerConfig config;
	config.max_clients = 1U;
	config.producer_id = 0x12345678U;
	config.security.enabled = true;
	config.security.port = 42042U;
	config.security.bind_mode = protocol::NetworkBindMode::LoopbackOnly;
	config.security.resources.max_clients = 1U;
	config.security.resources.global_state_reassembly_bytes = protocol::MaxStateReassemblyBytesPerClient;
	detail::SessionController controller;
	ASSERT_EQ(detail::SessionControllerConfigureResult::Ready,
		detail::SessionController::configure(config, ids, sequences, 0U, nullptr, controller));
	const auto peer = test_endpoint();
	const auto hello = make_hello();
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, {hello.bytes.data(), hello.size}, 1'000U, 0U, false).disposition);
	detail::SessionControllerOutput welcome_output;
	ASSERT_TRUE(controller.pop_output(welcome_output));
	const auto proof = make_ack(decode_output(welcome_output), 8U);
	ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(peer, {proof.bytes.data(), proof.size}, 2'000U, 0U, false).disposition);
	detail::SessionControllerOutput begin_output;
	ASSERT_TRUE(controller.pop_output(begin_output));
	const auto applied = make_ack(decode_output(begin_output), 10U);
	ASSERT_EQ(detail::SessionIngressDropReason::None,
		controller.ingest(peer, {applied.bytes.data(), applied.size}, 2'001U, 0U, false).drop_reason);
	expect_heartbeat_paths_allocate_nothing(controller, peer);
}

template <typename Controller>
void expect_ordered_phases_and_purge_allocate_nothing(Controller& controller, std::uint64_t now_us)
{
	if constexpr (!has_wp06_ordered_phases_and_purge<Controller>::value) {
		FAIL() << "The WP06 timeout/periodic/purge_all seams are absent.";
	} else {
		arm_allocation_probe();
		controller.service_timeouts(now_us);
		controller.service_reliability(now_us);
		controller.service_periodic(now_us);
		controller.purge_all(detail::SessionCloseReason::MissionDiscontinuity);
		const auto allocations = disarm_allocation_probe();
		EXPECT_EQ(0U, allocations) << "Ordered maintenance or purge allocated after Ready.";
		EXPECT_EQ(detail::SessionControllerOwnedUsage{}, controller.owned_usage());
	}
}

TEST(TelemetryWp06AllocationContract, OrderedTimeoutReliablePeriodicAndPurgeAllocateNothingAfterReady)
{
	EXPECT_TRUE(has_wp06_ordered_phases_and_purge<detail::SessionController>::value);
	FixedRandom id_random;
	id_random.draws[0] = 0x9999U;
	id_random.count = 1U;
	detail::SessionIdRegistry registry;
	ASSERT_TRUE(registry.allocate_storage());
	detail::SessionIdAllocator ids(id_random, registry);
	FixedRandom sequences;
	sequences.draws[0] = 0x40506070U;
	sequences.count = 1U;
	detail::SessionControllerConfig config;
	config.max_clients = 1U;
	config.producer_id = 0x12345678U;
	config.security.enabled = true;
	config.security.port = 42042U;
	config.security.bind_mode = protocol::NetworkBindMode::LoopbackOnly;
	config.security.resources.max_clients = 1U;
	config.security.resources.global_state_reassembly_bytes = protocol::MaxStateReassemblyBytesPerClient;
	detail::SessionController controller;
	ASSERT_EQ(detail::SessionControllerConfigureResult::Ready,
		detail::SessionController::configure(config, ids, sequences, 0U, nullptr, controller));
	const auto hello = make_hello();
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(test_endpoint(), {hello.bytes.data(), hello.size}, 1'000U, 0U, false).disposition);
	ASSERT_GT(controller.owned_usage().reliable_items, 0U);
	ASSERT_TRUE(controller.has_output());
	expect_ordered_phases_and_purge_allocate_nothing(controller, 2'000U);
}

struct FixedNativeBackend final : detail::UdpSocketBackend {
	struct ReceiveStep {
		detail::IoStatus status = detail::IoStatus::WouldBlock;
		Datagram datagram{};
		protocol::EndpointKey endpoint{};
	};

	static constexpr std::size_t MaximumSteps = 32U;
	std::array<ReceiveStep, MaximumSteps> receives{};
	std::array<detail::IoStatus, MaximumSteps> send_statuses{};
	std::array<Datagram, MaximumSteps> sent{};
	std::array<protocol::EndpointKey, MaximumSteps> sent_endpoints{};
	std::size_t receive_count = 0U;
	std::size_t receive_index = 0U;
	std::size_t send_status_count = 0U;
	std::size_t send_status_index = 0U;
	std::size_t sent_count = 0U;
	std::size_t open_calls = 0U;
	std::size_t receive_calls = 0U;
	std::size_t send_calls = 0U;
	std::size_t close_calls = 0U;
	bool socket_open = false;

	bool queue_receive(detail::IoStatus status,
		const Datagram& datagram = {},
		protocol::EndpointKey endpoint = {}) noexcept
	{
		if (receive_count >= receives.size()) {
			return false;
		}
		receives[receive_count++] = {status, datagram, endpoint};
		return true;
	}

	bool queue_send_status(detail::IoStatus status) noexcept
	{
		if (send_status_count >= send_statuses.size()) {
			return false;
		}
		send_statuses[send_status_count++] = status;
		return true;
	}

	detail::SocketOpenResult open_socket(const detail::SocketOpenRequest& request) noexcept override
	{
		++open_calls;
		socket_open = true;
		return {detail::SocketOpenStatus::Complete,
			91U,
			protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, request.port)};
	}

	detail::SocketReceiveResult try_receive(detail::SocketHandle,
		protocol::MutableByteView output) noexcept override
	{
		++receive_calls;
		if (receive_index >= receive_count) {
			return {detail::IoStatus::WouldBlock, {}, 0U, false};
		}
		const auto& step = receives[receive_index++];
		if (step.status == detail::IoStatus::Complete) {
			if (step.datagram.size > output.size) {
				return {detail::IoStatus::Complete, step.endpoint, step.datagram.size, true};
			}
			for (std::size_t index = 0U; index < step.datagram.size; ++index) {
				output.data[index] = step.datagram.bytes[index];
			}
		}
		return {step.status, step.endpoint, step.datagram.size, false};
	}

	detail::SocketSendResult try_send(detail::SocketHandle,
		const protocol::EndpointKey& endpoint,
		protocol::ByteView bytes) noexcept override
	{
		++send_calls;
		if (sent_count >= sent.size() || bytes.size > sent[sent_count].bytes.size()) {
			return {detail::IoStatus::Error, 0U};
		}
		auto& captured = sent[sent_count];
		captured.size = bytes.size;
		for (std::size_t index = 0U; index < bytes.size; ++index) {
			captured.bytes[index] = bytes.data[index];
		}
		sent_endpoints[sent_count] = endpoint;
		++sent_count;
		const auto status = send_status_index < send_status_count
			? send_statuses[send_status_index++]
			: detail::IoStatus::Complete;
		return {status, status == detail::IoStatus::Complete ? bytes.size : 0U};
	}

	void close_socket(detail::SocketHandle) noexcept override
	{
		++close_calls;
		socket_open = false;
	}
};

protocol::DatagramView decode_fixed_datagram(const Datagram& datagram)
{
	protocol::DatagramView view;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram({datagram.bytes.data(), datagram.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1},
			view));
	return view;
}

struct NativeAllocationServices final : detail::RuntimeStartupServices {
	explicit NativeAllocationServices(bool complete_budget) : ids(id_random, registry)
	{
		config.enabled = true;
		config.bind_addresses.clear();
		EXPECT_EQ(telemetry::BindAddressAddResult::Added,
			config.bind_addresses.add(telemetry::NumericIpAddress::from_ipv4({127U, 0U, 0U, 1U})));
		config.bind_port = 42042U;
		config.max_clients = 1U;
		config.max_datagrams_per_tick = 8U;
		id_random.draws = {{0x2222U, 0x3333U, 0x4444U, 0x5555U}};
		id_random.count = id_random.draws.size();
		packet_random.draws = {{0x10203040U, 0x20304050U, 0x30405060U, 0x40506070U}};
		packet_random.count = packet_random.draws.size();
		budget = detail::calculate_wp06_startup_budget(
			detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(1U)), 1U);
		if (complete_budget) {
			budget.is_complete = true;
			budget.deferred_categories = 0U;
		}
	}

	void capture_main_thread() noexcept override { captured = true; }
	bool is_on_captured_main_thread() noexcept override { return captured; }
	detail::RuntimeConfigResult load_config() noexcept override
	{
		return {detail::RuntimeConfigStatus::Enabled, config.max_clients};
	}
	detail::IdentityResult load_producer_identity() noexcept override
	{
		return {0x1020304050607080ULL, detail::IdentityError::None};
	}
	detail::SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		return {detail::SessionIdCandidateStatus::Ready, 0x1111U};
	}
	detail::Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t) noexcept override { return budget; }
	bool allocate_session_registry() noexcept override { return registry.allocate_storage(); }
	detail::SessionIdRegistrationStatus register_session_candidate(std::uint64_t candidate) noexcept override
	{
		return registry.register_candidate(candidate);
	}
	detail::RuntimeTransportStatus start_transport() noexcept override
	{
		++start_transport_calls;
		native.emplace(backend, completion);
		++native_constructions;
		const detail::NativeSessionStartRequest request{&config,
			0x1020304050607080ULL,
			&ids,
			&packet_random};
		return native->start(request) == detail::NativeSessionStartStatus::Started
			? detail::RuntimeTransportStatus::Started
			: detail::RuntimeTransportStatus::Unavailable;
	}
	std::uint64_t monotonic_now_us() noexcept override { return now_us; }
	detail::RuntimeTickStatus service_tick(const detail::RuntimeTickContext& context) noexcept override
	{
		++service_calls;
		if (!native.has_value()) {
			return detail::RuntimeTickStatus::Unavailable;
		}
		return native->service_tick({context.now_us, context.mission_generation, context.mission_active}) ==
				detail::NativeSessionTickStatus::Complete
			? detail::RuntimeTickStatus::Complete
			: detail::RuntimeTickStatus::PermanentTransportFailure;
	}
	void stop_collection() noexcept override {}
	void invalidate_mission_state_and_entities() noexcept override
	{
		if (native.has_value()) native->purge_all(detail::SessionCloseReason::MissionDiscontinuity);
	}
	void cancel_replication() noexcept override {}
	void close_sessions_and_stores() noexcept override
	{
		if (native.has_value()) {
			native->purge_all(detail::SessionCloseReason::Shutdown);
			usage_after_close = native->owned_usage();
		}
	}
	void reset_mission_scope() noexcept override {}
	void stop_transport() noexcept override
	{
		if (native.has_value()) {
			native->shutdown();
			sockets_after_stop = native->socket_count();
		}
	}
	void emit_runtime_summary() noexcept override {}
	void release_runtime_allocations() noexcept override { native.reset(); }
	void release_session_registry() noexcept override { ++registry_releases; }
	void emit_startup_diagnostic(detail::RuntimeTerminalReason) noexcept override { ++diagnostics; }

	FixedNativeBackend backend;
	detail::NativeOutputCompletionForwarder completion;
	FixedRandom id_random;
	FixedRandom packet_random;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator ids;
	telemetry::TelemetryConfig config;
	detail::Wp03KnownBudgetSubtotal budget{};
	std::optional<detail::NativeSessionRuntime> native;
	std::uint64_t now_us = 10'000U;
	std::size_t service_calls = 0U;
	std::size_t start_transport_calls = 0U;
	std::size_t native_constructions = 0U;
	std::size_t registry_releases = 0U;
	std::size_t diagnostics = 0U;
	std::size_t sockets_after_stop = 0U;
	detail::SessionControllerOwnedUsage usage_after_close{};
	bool captured = false;
};

struct NativeAllocationFixture {
	explicit NativeAllocationFixture(bool complete_budget = true) : services(complete_budget), runtime(services) {}

	void start()
	{
		runtime.capture_main_thread();
		runtime.on_engine_update();
	}

	std::uint64_t tick_and_count_allocations(std::uint64_t now_us)
	{
		services.now_us = now_us;
		arm_allocation_probe();
		runtime.on_engine_update();
		return disarm_allocation_probe();
	}

	NativeAllocationServices services;
	detail::Runtime runtime;
};

TEST(TelemetryNativeAllocationContract, RealDeferredBudgetMaskConstructsNoNativeStackOrSocket)
{
	NativeAllocationFixture fixture(false);
	EXPECT_EQ(0x00f0U, fixture.services.budget.deferred_categories);
	fixture.start();
	EXPECT_EQ(detail::RuntimeState::Faulted, fixture.runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::BudgetFailure, fixture.runtime.terminal_reason());
	EXPECT_EQ(0U, fixture.services.start_transport_calls);
	EXPECT_EQ(0U, fixture.services.native_constructions);
	EXPECT_EQ(0U, fixture.services.backend.open_calls);
	EXPECT_FALSE(fixture.services.backend.socket_open);
}

TEST(TelemetryNativeAllocationContract, ReadyRuntimeLifecycleAndBackpressureAllocateNothing)
{
	NativeAllocationFixture fixture;
	fixture.start();
	ASSERT_EQ(detail::RuntimeState::Ready, fixture.runtime.state());
	ASSERT_TRUE(fixture.services.native.has_value());
	ASSERT_EQ(1U, fixture.services.backend.open_calls);
	ASSERT_EQ(1U, fixture.services.native->socket_count());

	EXPECT_EQ(0U, fixture.tick_and_count_allocations(20'000U)) << "idle native tick allocated after Ready";

	const auto endpoint = test_endpoint();
	const auto hello = make_hello();
	ASSERT_TRUE(fixture.services.backend.queue_receive(detail::IoStatus::Complete, hello, endpoint));
	EXPECT_EQ(0U, fixture.tick_and_count_allocations(30'000U)) << "HELLO/WELCOME native tick allocated";
	ASSERT_GE(fixture.services.backend.sent_count, 1U);
	const auto welcome = decode_fixed_datagram(fixture.services.backend.sent[0U]);
	ASSERT_EQ(protocol::MessageType::Welcome, welcome.header.message_type);

	const auto welcome_ack = make_ack(welcome, 8U);
	ASSERT_TRUE(fixture.services.backend.queue_receive(detail::IoStatus::Complete, welcome_ack, endpoint));
	EXPECT_EQ(0U, fixture.tick_and_count_allocations(30'010U)) << "WELCOME ACK/SESSION_BEGIN native tick allocated";
	ASSERT_GE(fixture.services.backend.sent_count, 2U);
	const auto begin = decode_fixed_datagram(fixture.services.backend.sent[1U]);
	ASSERT_EQ(protocol::MessageType::SessionBegin, begin.header.message_type);

	const auto retry_due = 30'010U + protocol::reliable_retry_delay_us(protocol::ReliableDefaultRtoUs,
		0x4653544c5f52544fULL,
		begin.header.session_id,
		begin.header.message_id,
		0U);
	const auto sent_before_retry = fixture.services.backend.sent_count;
	EXPECT_EQ(0U, fixture.tick_and_count_allocations(retry_due)) << "REL retry native tick allocated";
	EXPECT_GT(fixture.services.backend.sent_count, sent_before_retry);

	const auto begin_ack = make_ack(begin, 10U);
	ASSERT_TRUE(fixture.services.backend.queue_receive(detail::IoStatus::Complete, begin_ack, endpoint));
	EXPECT_EQ(0U, fixture.tick_and_count_allocations(retry_due + 10U)) << "SESSION_BEGIN ACK native tick allocated";
	ASSERT_EQ(1U, fixture.services.native->active_sessions());

	const auto heartbeat_due = 1'030'010U;
	ASSERT_TRUE(fixture.services.backend.queue_send_status(detail::IoStatus::WouldBlock));
	const auto receive_before_would_block = fixture.services.backend.receive_calls;
	const auto send_before_would_block = fixture.services.backend.send_calls;
	EXPECT_EQ(0U, fixture.tick_and_count_allocations(heartbeat_due)) << "periodic HB and RX/TX WouldBlock allocated";
	EXPECT_GT(fixture.services.backend.receive_calls, receive_before_would_block);
	EXPECT_GT(fixture.services.backend.send_calls, send_before_would_block);
	ASSERT_GE(fixture.services.backend.sent_count, 4U);
	const auto heartbeat_request = decode_fixed_datagram(
		fixture.services.backend.sent[fixture.services.backend.sent_count - 1U]);
	ASSERT_EQ(protocol::MessageType::Heartbeat, heartbeat_request.header.message_type);
	protocol::HeartbeatPayload request_payload;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_heartbeat_payload(heartbeat_request.payload, request_payload));
	ASSERT_EQ(protocol::HeartbeatKind::Request, request_payload.kind);
	protocol::HeartbeatPayload response_payload = request_payload;
	response_payload.kind = protocol::HeartbeatKind::Response;
	response_payload.receive_t1_us = request_payload.origin_t0_us + 10U;
	response_payload.transmit_t2_us = request_payload.origin_t0_us + 20U;
	const auto response = make_heartbeat(
		begin.header.session_id, response_payload, 30U, heartbeat_due + 100U);
	ASSERT_TRUE(fixture.services.backend.queue_receive(detail::IoStatus::Complete, response, endpoint));
	EXPECT_EQ(0U, fixture.tick_and_count_allocations(heartbeat_due + 100U))
		<< "HB response and retained TX retry allocated";

	const auto timeout_at = heartbeat_due + 100U + 10'000'000U;
	EXPECT_EQ(0U, fixture.tick_and_count_allocations(timeout_at)) << "timeout cleanup allocated";
	EXPECT_EQ(0U, fixture.services.native->active_sessions());
	arm_allocation_probe();
	fixture.services.native->purge_all(detail::SessionCloseReason::MissionDiscontinuity);
	const auto purge_allocations = disarm_allocation_probe();
	EXPECT_EQ(0U, purge_allocations) << "native purge allocated";
	EXPECT_EQ(detail::SessionControllerOwnedUsage{}, fixture.services.native->owned_usage());

	arm_allocation_probe();
	fixture.runtime.on_engine_shutdown();
	fixture.runtime.on_engine_update();
	fixture.runtime.on_engine_shutdown();
	const auto shutdown_allocations = disarm_allocation_probe();
	EXPECT_EQ(0U, shutdown_allocations) << "native shutdown or late callbacks allocated";
	EXPECT_EQ(detail::RuntimeState::Stopped, fixture.runtime.state());
	EXPECT_EQ(1U, fixture.services.backend.close_calls);
	EXPECT_FALSE(fixture.services.backend.socket_open);
	EXPECT_EQ(0U, fixture.services.sockets_after_stop);
	EXPECT_EQ(detail::SessionControllerOwnedUsage{}, fixture.services.usage_after_close);
}

TEST(TelemetryNativeAllocationContract, PermanentReceiveAndSendFailuresAllocateNothingAndStayClosed)
{
	for (const auto receive_status : {detail::IoStatus::Closed, detail::IoStatus::Error}) {
		NativeAllocationFixture fixture;
		fixture.start();
		ASSERT_EQ(detail::RuntimeState::Ready, fixture.runtime.state());
		ASSERT_TRUE(fixture.services.backend.queue_receive(receive_status));
		EXPECT_EQ(0U, fixture.tick_and_count_allocations(40'000U)) << "fatal receive allocated";
		EXPECT_EQ(detail::RuntimeState::Faulted, fixture.runtime.state());
		EXPECT_EQ(detail::RuntimeTerminalReason::TransportUnavailable, fixture.runtime.terminal_reason());
		EXPECT_EQ(1U, fixture.services.backend.close_calls);
		EXPECT_FALSE(fixture.services.backend.socket_open);
		EXPECT_EQ(0U, fixture.services.sockets_after_stop);
		EXPECT_EQ(detail::SessionControllerOwnedUsage{}, fixture.services.usage_after_close);
		const auto receives = fixture.services.backend.receive_calls;
		const auto sends = fixture.services.backend.send_calls;
		EXPECT_EQ(0U, fixture.tick_and_count_allocations(40'001U)) << "late fatal receive tick allocated";
		EXPECT_EQ(receives, fixture.services.backend.receive_calls);
		EXPECT_EQ(sends, fixture.services.backend.send_calls);
		arm_allocation_probe();
		fixture.runtime.on_engine_shutdown();
		const auto shutdown_allocations = disarm_allocation_probe();
		EXPECT_EQ(0U, shutdown_allocations);
	}

	for (const auto send_status : {detail::IoStatus::Closed, detail::IoStatus::Error}) {
		NativeAllocationFixture fixture;
		fixture.start();
		ASSERT_EQ(detail::RuntimeState::Ready, fixture.runtime.state());
		ASSERT_TRUE(fixture.services.backend.queue_receive(
			detail::IoStatus::Complete, make_hello(), test_endpoint()));
		ASSERT_TRUE(fixture.services.backend.queue_send_status(send_status));
		EXPECT_EQ(0U, fixture.tick_and_count_allocations(50'000U)) << "fatal send completion allocated";
		EXPECT_EQ(detail::RuntimeState::Faulted, fixture.runtime.state());
		EXPECT_EQ(detail::RuntimeTerminalReason::TransportUnavailable, fixture.runtime.terminal_reason());
		EXPECT_EQ(1U, fixture.services.backend.send_calls);
		EXPECT_EQ(1U, fixture.services.backend.close_calls);
		EXPECT_FALSE(fixture.services.backend.socket_open);
		EXPECT_EQ(0U, fixture.services.sockets_after_stop);
		EXPECT_EQ(detail::SessionControllerOwnedUsage{}, fixture.services.usage_after_close);
		const auto receives = fixture.services.backend.receive_calls;
		const auto sends = fixture.services.backend.send_calls;
		EXPECT_EQ(0U, fixture.tick_and_count_allocations(50'001U)) << "late fatal send tick allocated";
		EXPECT_EQ(receives, fixture.services.backend.receive_calls);
		EXPECT_EQ(sends, fixture.services.backend.send_calls);
		arm_allocation_probe();
		fixture.runtime.on_engine_shutdown();
		const auto shutdown_allocations = disarm_allocation_probe();
		EXPECT_EQ(0U, shutdown_allocations);
	}
}

} // namespace
