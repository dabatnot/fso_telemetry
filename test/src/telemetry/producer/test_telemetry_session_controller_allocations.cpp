#include "telemetry/session_controller.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/transport.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
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

Datagram make_ack(const protocol::DatagramView& target)
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
	header.packet_sequence = 8U;
	header.sent_time_us = 2'000U;
	header.message_id = 2U;
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

} // namespace
