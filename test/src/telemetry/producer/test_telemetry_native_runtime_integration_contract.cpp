#include "telemetry/runtime.h"

#if __has_include("telemetry/native_session_runtime.h")
#include "telemetry/native_session_runtime.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/session_controller.h"
#include "telemetry/startup_budget.h"
#include "telemetry/transport.h"
#define FSO_HAS_NATIVE_SESSION_RUNTIME 1
#else
#define FSO_HAS_NATIVE_SESSION_RUNTIME 0
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr const char* MissingNativeComposition =
	"R2 requires telemetry/native_session_runtime.h; production must remain gated until the composition exists.";

#if !FSO_HAS_NATIVE_SESSION_RUNTIME

#define R2_MISSING_TEST(name, contract) \
	TEST(TelemetryNativeRuntimeIntegrationContract, name) \
	{ \
		FAIL() << MissingNativeComposition << " Contract: " << contract; \
	}

R2_MISSING_TEST(NativeCompositionHeaderAndNoexceptContractExist,
	"internal injectable ownership and noexcept start/tick/purge/shutdown API")
R2_MISSING_TEST(BudgetGateConstructsNativeStackOnlyInsideSuccessfulStartTransport,
	"real deferred mask 0x00f0 constructs nothing; only fake RuntimeStartupServices may inject completeness")
R2_MISSING_TEST(EngineUpdateReadsOneClockThenAppliesLifecycleBeforeTick,
	"Clock once -> lifecycle -> service_tick with the post-lifecycle context")
R2_MISSING_TEST(FourBehavioralInversionsLockLifecycleTimeoutReliablePeriodicAndIoOrder,
	"lifecycle before timeout, timeout before REL, REL before periodic, periodic before I/O")
R2_MISSING_TEST(SharedBudgetAlternatesAcrossTicksAndWouldBlockStopsOnlyOneDirection,
	"one shared 1..256 attempt budget, persistent alternation and direction-local WouldBlock")
R2_MISSING_TEST(AllowlistAndVersionNegotiationComposeWithoutDurableRejectedState,
	"nonallowlisted zero response; 1.1 accepted; 1.0 unsupported without durable slot")
R2_MISSING_TEST(PermanentReceiveClosedOrErrorPurgesAllAndNeverReopens,
	"receive Closed/Error is a global permanent fault with complete purge")
R2_MISSING_TEST(PermanentSendClosedOrErrorCompletesOnceThenPurgesGlobally,
	"one send completion followed by global teardown and no retry/reopen")
R2_MISSING_TEST(ShutdownAndWrongThreadNeverClockServiceOrDrain,
	"wrong thread and late updates perform no clock/I/O; shutdown is ordered and idempotent")

#undef R2_MISSING_TEST

#else

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

template <typename Container>
protocol::ByteView byte_view(const Container& bytes) noexcept
{
	return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

template <typename Container>
protocol::MutableByteView mutable_byte_view(Container& bytes) noexcept
{
	return {reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()};
}

protocol::EndpointKey peer(std::uint8_t last = 1U, std::uint16_t port = 43000U) noexcept
{
	return protocol::EndpointKey::from_ipv4({127U, 0U, 0U, last}, port);
}

telemetry::NumericIpAddress loopback4() noexcept
{
	return telemetry::NumericIpAddress::from_ipv4({127U, 0U, 0U, 1U});
}

struct Packet {
	std::vector<std::uint8_t> bytes;
	protocol::EndpointKey endpoint;
};

Packet hello(std::uint8_t minor, std::uint64_t nonce, protocol::EndpointKey endpoint = peer())
{
	protocol::HelloPayload payload;
	payload.client_nonce = nonce;
	payload.client_send_t0_us = 100U;
	payload.min_major = protocol::VersionMajor;
	payload.max_major = protocol::VersionMajor;
	payload.min_minor = minor;
	payload.max_minor = minor;
	payload.requested_visibility_mode = protocol::VisibilityMode::Cockpit;
	payload.requested_heartbeat_ms = 1000U;
	std::array<std::uint8_t, protocol::HelloPayloadPrefixSize> encoded_payload{};
	std::size_t payload_size = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_hello_payload(payload, mutable_byte_view(encoded_payload), payload_size));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = minor;
	header.message_type = protocol::MessageType::Hello;
	header.packet_sequence = 1U;
	header.sent_time_us = 100U;
	header.message_id = 1U;
	header.message_size = static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc({encoded_payload.data(), payload_size});
	Packet result;
	result.endpoint = endpoint;
	result.bytes.resize(protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{minor, minor},
			{encoded_payload.data(), payload_size},
			mutable_byte_view(result.bytes),
			written));
	return result;
}

Packet ack_for(const std::vector<std::uint8_t>& target,
	protocol::EndpointKey endpoint,
	std::uint32_t packet_sequence)
{
	protocol::DatagramView decoded;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(byte_view(target),
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1},
			decoded));
	protocol::AckPayload ack;
	ack.target_message_id = decoded.header.message_id;
	ack.target_message_type = decoded.header.message_type;
	ack.ack_flags = protocol::KnownAckFlags;
	ack.target_fragment_count = decoded.header.fragment_count;
	ack.target_message_crc32 = decoded.header.message_crc32;
	std::array<std::uint8_t, protocol::AckPayloadSize> payload{};
	std::size_t payload_size = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_ack_payload(ack, mutable_byte_view(payload), payload_size));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = decoded.header.version_minor;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = decoded.header.session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = 200U + packet_sequence;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), payload_size});
	Packet result;
	result.endpoint = endpoint;
	result.bytes.resize(protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{header.version_minor, header.version_minor},
			{payload.data(), payload_size},
			mutable_byte_view(result.bytes),
			written));
	return result;
}

struct FixedRandom final : detail::RandomSource {
	std::uint64_t next = 0x1000U;
	bool next_u64(std::uint64_t& output) noexcept override
	{
		output = next++;
		return true;
	}
};

struct ScriptedBackend final : detail::UdpSocketBackend {
	struct Rx { detail::IoStatus status; Packet packet; bool truncated = false; };
	std::vector<detail::SocketOpenStatus> open_statuses;
	std::vector<Rx> receives;
	std::vector<detail::IoStatus> sends;
	std::vector<std::vector<std::uint8_t>> sent;
	std::vector<protocol::EndpointKey> send_endpoints;
	std::vector<detail::SocketHandle> closed;
	std::vector<char> io_trace;
	std::size_t open_calls = 0U;
	std::size_t receive_calls = 0U;
	std::size_t send_calls = 0U;
	std::size_t receive_script_index = 0U;
	std::size_t send_script_index = 0U;

	detail::SocketOpenResult open_socket(const detail::SocketOpenRequest& request) noexcept override
	{
		io_trace.push_back('O');
		const auto index = open_calls++;
		const auto status = index < open_statuses.size() ? open_statuses[index] : detail::SocketOpenStatus::Complete;
		return {status,
			status == detail::SocketOpenStatus::Complete ? 10U + index : detail::InvalidSocketHandle,
			request.bind_address.family() == protocol::IpAddressFamily::Ipv4
				? protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, request.port)
				: protocol::EndpointKey::from_ipv6(request.bind_address.bytes(), request.port)};
	}

	detail::SocketReceiveResult try_receive(detail::SocketHandle, protocol::MutableByteView output) noexcept override
	{
		io_trace.push_back('R');
		++receive_calls;
		if (receive_script_index >= receives.size()) {
			return {detail::IoStatus::WouldBlock, {}, 0U, false};
		}
		const auto& item = receives[receive_script_index++];
		if (item.status == detail::IoStatus::Complete && item.packet.bytes.size() <= output.size) {
			std::copy(item.packet.bytes.begin(), item.packet.bytes.end(), output.data);
		}
		return {item.status, item.packet.endpoint, item.packet.bytes.size(), item.truncated};
	}

	detail::SocketSendResult try_send(detail::SocketHandle,
		const protocol::EndpointKey& endpoint,
		protocol::ByteView bytes) noexcept override
	{
		io_trace.push_back('S');
		++send_calls;
		send_endpoints.push_back(endpoint);
		sent.emplace_back(bytes.data, bytes.data + bytes.size);
		const auto status = send_script_index < sends.size() ? sends[send_script_index++] : detail::IoStatus::Complete;
		return {status, status == detail::IoStatus::Complete ? bytes.size : 0U};
	}

	void close_socket(detail::SocketHandle handle) noexcept override
	{
		io_trace.push_back('C');
		closed.push_back(handle);
	}
};

struct CountingOutputCompletion final : detail::NativeOutputCompletionPort {
	void complete(detail::SessionController& controller, detail::IoStatus status) noexcept override
	{
		++calls;
		last_status = status;
		usage_before = controller.owned_usage();
		forwarder.complete(controller, status);
		usage_after = controller.owned_usage();
	}

	detail::NativeOutputCompletionForwarder forwarder;
	std::size_t calls = 0U;
	detail::IoStatus last_status = detail::IoStatus::WouldBlock;
	detail::SessionControllerOwnedUsage usage_before{};
	detail::SessionControllerOwnedUsage usage_after{};
};

telemetry::TelemetryConfig enabled_config(std::uint16_t attempts = 64U)
{
	telemetry::TelemetryConfig config;
	config.enabled = true;
	config.bind_addresses.clear();
	config.bind_addresses.add(loopback4());
	config.max_datagrams_per_tick = attempts;
	return config;
}

protocol::MessageType sent_type(const ScriptedBackend& backend, std::size_t index);
protocol::DatagramView decode_sent(const ScriptedBackend& backend,
	std::size_t index,
	std::vector<std::uint8_t>& stable_storage);

struct NativeFixture {
	ScriptedBackend backend;
	CountingOutputCompletion completion;
	FixedRandom ids_random;
	FixedRandom packet_random;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator ids;
	detail::NativeSessionRuntime runtime;

	NativeFixture() : ids(ids_random, registry), runtime(backend, completion)
	{
		EXPECT_TRUE(registry.allocate_storage());
	}

	detail::NativeSessionStartStatus start(telemetry::TelemetryConfig& config)
	{
		detail::NativeSessionStartRequest request{&config, 0x1020304050607080ULL, &ids, &packet_random};
		return runtime.start(request);
	}
};

std::uint64_t establish_ready(NativeFixture& fixture,
	protocol::EndpointKey endpoint,
	std::uint64_t nonce,
	std::uint64_t now_us,
	std::uint32_t packet_base,
	std::uint32_t mission_generation = 0U,
	bool mission_active = false)
{
	const auto sessions_before = fixture.runtime.active_sessions();
	auto drive_new_send = [&](std::size_t previous, std::uint64_t first_tick) {
		for (std::uint64_t offset = 0U; offset < 8U && fixture.backend.sent.size() == previous; ++offset) {
			fixture.runtime.service_tick(
				{first_tick + offset, mission_generation, mission_active});
		}
	};
	fixture.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, nonce, endpoint)});
	drive_new_send(fixture.backend.sent.size(), now_us);
	EXPECT_FALSE(fixture.backend.sent.empty());
	if (fixture.backend.sent.empty()) return 0U;
	const auto welcome_index = fixture.backend.sent.size() - 1U;
	EXPECT_EQ(protocol::MessageType::Welcome, sent_type(fixture.backend, welcome_index));
	fixture.backend.receives.push_back(
		{detail::IoStatus::Complete,
			ack_for(fixture.backend.sent[welcome_index], endpoint, packet_base + 1U)});
	drive_new_send(fixture.backend.sent.size(), now_us + 10U);
	EXPECT_GT(fixture.backend.sent.size(), welcome_index + 1U);
	if (fixture.backend.sent.size() <= welcome_index + 1U) return 0U;
	const auto begin_index = fixture.backend.sent.size() - 1U;
	EXPECT_EQ(protocol::MessageType::SessionBegin, sent_type(fixture.backend, begin_index));
	std::vector<std::uint8_t> stable;
	const auto begin = decode_sent(fixture.backend, begin_index, stable);
	fixture.backend.receives.push_back(
		{detail::IoStatus::Complete,
			ack_for(fixture.backend.sent[begin_index], endpoint, packet_base + 2U)});
	const auto final_ack_script_index = fixture.backend.receives.size() - 1U;
	for (std::uint64_t offset = 0U;
		offset < 8U && fixture.backend.receive_script_index <= final_ack_script_index;
		++offset) {
		fixture.runtime.service_tick({now_us + 20U + offset, mission_generation, mission_active});
	}
	EXPECT_GT(fixture.backend.receive_script_index, final_ack_script_index)
		<< "The final SESSION_BEGIN ACK must be consumed, not merely queued.";
	EXPECT_EQ(sessions_before + 1U, fixture.runtime.active_sessions())
		<< "establish_ready adds exactly one session without assuming an empty fixture.";
	return begin.header.session_id;
}

// This fake is deliberately the only place where a synthetic complete startup
// budget may reach a NativeSessionRuntime.  Construction is performed inside
// start_transport(), never in the fake's constructor or in the direct native
// fixture above.  Runtime remains the real owner of startup/lifecycle ordering.
struct RuntimeCompositionServices final : detail::RuntimeStartupServices {
	RuntimeCompositionServices()
		: ids(ids_random, registry)
	{
		config.enabled = true;
		config.bind_addresses.clear();
		config.bind_addresses.add(loopback4());
		config.max_clients = 2U;
		config.max_datagrams_per_tick = 8U;
		budget = detail::calculate_wp06_startup_budget(
			detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(2U)), 2U);
	}

	void capture_main_thread() noexcept override { captured = true; }
	bool is_on_captured_main_thread() noexcept override
	{
		++thread_checks;
		return captured && thread_matches;
	}
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
	detail::Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t) noexcept override
	{
		++budget_calls;
		return budget;
	}
	bool allocate_session_registry() noexcept override
	{
		++registry_allocations;
		return registry.allocate_storage();
	}
	detail::SessionIdRegistrationStatus register_session_candidate(std::uint64_t candidate) noexcept override
	{
		return registry.register_candidate(candidate);
	}
	detail::RuntimeTransportStatus start_transport() noexcept override
	{
		++start_transport_calls;
		++native_constructions;
		native = std::make_unique<detail::NativeSessionRuntime>(backend, output_completion);
		detail::NativeSessionStartRequest request{&config,
			0x1020304050607080ULL,
			&ids,
			&packet_random};
		return native->start(request) == detail::NativeSessionStartStatus::Started
			? detail::RuntimeTransportStatus::Started
			: detail::RuntimeTransportStatus::Unavailable;
	}

	// These are the R2 Runtime service seams.  Omitting override is intentional:
	// a partially introduced base API still compiles far enough for the API test
	// to report a focused failure instead of producing an override cascade.
	std::uint64_t monotonic_now_us() noexcept
	{
		++clock_calls;
		runtime_trace.push_back('C');
		return clock_now_us;
	}
	detail::RuntimeTickStatus service_tick(const detail::RuntimeTickContext& context) noexcept
	{
		++service_calls;
		runtime_trace.push_back('T');
		last_tick = context;
		if (native == nullptr) return detail::RuntimeTickStatus::Unavailable;
		const auto status = native->service_tick(
			{context.now_us, context.mission_generation, context.mission_active});
		return status == detail::NativeSessionTickStatus::Complete ? detail::RuntimeTickStatus::Complete
															  : detail::RuntimeTickStatus::PermanentTransportFailure;
	}

	void stop_collection() noexcept override { teardown.push_back('1'); }
	void invalidate_mission_state_and_entities() noexcept override
	{
		teardown.push_back('2');
		runtime_trace.push_back('L');
		if (native != nullptr) native->purge_all(detail::SessionCloseReason::MissionDiscontinuity);
	}
	void cancel_replication() noexcept override { teardown.push_back('3'); }
	void close_sessions_and_stores() noexcept override
	{
		teardown.push_back('4');
		if (native != nullptr) native->purge_all(detail::SessionCloseReason::Shutdown);
	}
	void reset_mission_scope() noexcept override { teardown.push_back('5'); }
	void stop_transport() noexcept override
	{
		teardown.push_back('6');
		if (native != nullptr) native->shutdown();
	}
	void emit_runtime_summary() noexcept override { teardown.push_back('7'); }
	void release_runtime_allocations() noexcept override
	{
		teardown.push_back('8');
		native.reset();
	}
	void release_session_registry() noexcept override { teardown.push_back('9'); }
	void emit_startup_diagnostic(detail::RuntimeTerminalReason) noexcept override { ++diagnostics; }

	ScriptedBackend backend;
	detail::NativeOutputCompletionForwarder output_completion;
	FixedRandom ids_random;
	FixedRandom packet_random;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator ids;
	telemetry::TelemetryConfig config;
	detail::Wp03KnownBudgetSubtotal budget;
	std::unique_ptr<detail::NativeSessionRuntime> native;
	detail::RuntimeTickContext last_tick{};
	std::vector<char> teardown;
	std::vector<char> runtime_trace;
	std::uint64_t clock_now_us = 9'000U;
	std::size_t clock_calls = 0U;
	std::size_t service_calls = 0U;
	std::size_t thread_checks = 0U;
	std::size_t budget_calls = 0U;
	std::size_t registry_allocations = 0U;
	std::size_t start_transport_calls = 0U;
	std::size_t native_constructions = 0U;
	std::size_t diagnostics = 0U;
	bool captured = false;
	bool thread_matches = true;
};

template <typename T, typename = void>
struct has_native_contract : std::false_type {};

template <typename T>
struct has_native_contract<T,
	std::void_t<decltype(std::declval<T&>().start(std::declval<const detail::NativeSessionStartRequest&>())),
		decltype(std::declval<T&>().service_tick(std::declval<const detail::NativeSessionTickContext&>())),
		decltype(std::declval<T&>().purge_all(std::declval<detail::SessionCloseReason>())),
		decltype(std::declval<T&>().shutdown()),
		decltype(std::declval<const T&>().socket_count()),
		decltype(std::declval<const T&>().active_sessions()),
		decltype(std::declval<const T&>().owned_usage())>>
	: std::bool_constant<noexcept(std::declval<T&>().start(
		  std::declval<const detail::NativeSessionStartRequest&>())) &&
		  noexcept(std::declval<T&>().service_tick(
			  std::declval<const detail::NativeSessionTickContext&>())) &&
		  noexcept(std::declval<T&>().purge_all(std::declval<detail::SessionCloseReason>())) &&
		  noexcept(std::declval<T&>().shutdown())> {};

template <typename NativeRuntime>
void expect_native_api_contract()
{
	if constexpr (!has_native_contract<NativeRuntime>::value) {
		FAIL() << "NativeSessionRuntime exists only partially: start/tick/purge/shutdown and state queries "
			   << "must all exist and the four mutating seams must be noexcept.";
	} else {
		EXPECT_TRUE((std::is_nothrow_constructible_v<NativeRuntime,
			detail::UdpSocketBackend&,
			detail::NativeOutputCompletionPort&>));
		EXPECT_FALSE((std::is_constructible_v<NativeRuntime,
			detail::UdpSocketBackend&,
			detail::NativeOutputCompletionPort*>))
			<< "The completion port is a mandatory non-null reference.";
		EXPECT_FALSE((std::is_constructible_v<NativeRuntime,
			detail::UdpSocketBackend&,
			detail::NativeOutputCompletionPort&,
			const detail::Wp03KnownBudgetSubtotal&>))
			<< "A direct native budget/completeness bypass is forbidden.";
		EXPECT_TRUE(std::is_nothrow_default_constructible_v<detail::NativeOutputCompletionForwarder>);
		EXPECT_TRUE(noexcept(std::declval<detail::NativeOutputCompletionForwarder&>().complete(
			std::declval<detail::SessionController&>(), std::declval<detail::IoStatus>())));
	}
}

protocol::MessageType sent_type(const ScriptedBackend& backend, std::size_t index)
{
	protocol::DatagramView datagram;
	EXPECT_LT(index, backend.sent.size());
	if (index >= backend.sent.size()) return static_cast<protocol::MessageType>(0xffU);
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(byte_view(backend.sent[index]),
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1},
			datagram));
	return datagram.header.message_type;
}

protocol::DatagramView decode_sent(const ScriptedBackend& backend,
	std::size_t index,
	std::vector<std::uint8_t>& stable_storage)
{
	protocol::DatagramView datagram;
	EXPECT_LT(index, backend.sent.size());
	if (index >= backend.sent.size()) return datagram;
	stable_storage = backend.sent[index];
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(byte_view(stable_storage),
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1},
			datagram));
	return datagram;
}

TEST(TelemetryNativeRuntimeIntegrationContract, NativeCompositionHeaderAndNoexceptContractExist)
{
	expect_native_api_contract<detail::NativeSessionRuntime>();
}

TEST(TelemetryNativeRuntimeIntegrationContract, BudgetGateConstructsNativeStackOnlyInsideSuccessfulStartTransport)
{
	RuntimeCompositionServices real_services;
	ASSERT_EQ(detail::StartupBudgetError::None, real_services.budget.error);
	ASSERT_FALSE(real_services.budget.is_complete);
	ASSERT_EQ(0x00f0U, real_services.budget.deferred_categories);
	detail::Runtime real_runtime(real_services);
	real_runtime.capture_main_thread();
	EXPECT_EQ(0U, real_services.native_constructions);
	EXPECT_EQ(0U, real_services.backend.open_calls);
	real_runtime.on_engine_update();
	EXPECT_EQ(detail::RuntimeState::Faulted, real_runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::BudgetFailure, real_runtime.terminal_reason());
	EXPECT_EQ(0U, real_services.start_transport_calls);
	EXPECT_EQ(0U, real_services.native_constructions);
	EXPECT_EQ(0U, real_services.backend.open_calls);

	RuntimeCompositionServices synthetic_services;
	synthetic_services.budget.is_complete = true;
	synthetic_services.budget.deferred_categories = 0U;
	detail::Runtime synthetic_runtime(synthetic_services);
	synthetic_runtime.capture_main_thread();
	EXPECT_EQ(0U, synthetic_services.native_constructions)
		<< "The native graph must not be eagerly constructed by the fake.";
	synthetic_runtime.on_engine_update();
	EXPECT_EQ(detail::RuntimeState::Ready, synthetic_runtime.state());
	EXPECT_EQ(1U, synthetic_services.start_transport_calls);
	EXPECT_EQ(1U, synthetic_services.native_constructions)
		<< "Synthetic completeness is consumed only at Runtime::start_transport().";
	EXPECT_EQ(1U, synthetic_services.backend.open_calls);
}

TEST(TelemetryNativeRuntimeIntegrationContract, EngineUpdateReadsOneClockThenAppliesLifecycleBeforeTick)
{
	RuntimeCompositionServices services;
	services.budget.is_complete = true;
	services.budget.deferred_categories = 0U;
	detail::Runtime runtime(services);
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	services.clock_calls = 0U;
	services.service_calls = 0U;
	services.teardown.clear();
	services.runtime_trace.clear();
	services.clock_now_us = 12'345U;
	runtime.on_game_mission_load();
	runtime.on_engine_update();
	EXPECT_EQ(1U, services.clock_calls);
	EXPECT_EQ(1U, services.service_calls);
	EXPECT_EQ(12'345U, services.last_tick.now_us);
	EXPECT_EQ(runtime.mission_generation(), services.last_tick.mission_generation);
	EXPECT_FALSE(services.last_tick.mission_active);
	EXPECT_EQ((std::vector<char>{'C', 'L', 'T'}), services.runtime_trace)
		<< "The one clock read precedes lifecycle consumption, which precedes tick service.";
	EXPECT_EQ((std::vector<char>{'1', '2', '3', '4', '5'}), services.teardown)
		<< "Mission discontinuity uses the exact bounded Runtime purge sequence.";
}

TEST(TelemetryNativeRuntimeIntegrationContract, FourBehavioralInversionsLockLifecycleTimeoutReliablePeriodicAndIoOrder)
{
	// 1. Lifecycle > service: a queued old-generation session is purged by the
	// real Runtime marker before the same EngineUpdate services new network work.
	RuntimeCompositionServices lifecycle;
	lifecycle.budget.is_complete = true;
	lifecycle.budget.deferred_categories = 0U;
	detail::Runtime runtime(lifecycle);
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_NE(nullptr, lifecycle.native.get());
	lifecycle.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 100U, peer(2U))});
	runtime.on_engine_update();
	ASSERT_FALSE(lifecycle.backend.sent.empty());
	std::vector<std::uint8_t> old_storage;
	const auto old_welcome = decode_sent(
		lifecycle.backend, lifecycle.backend.sent.size() - 1U, old_storage);
	ASSERT_EQ(peer(2U), lifecycle.backend.send_endpoints.back());
	ASSERT_NE(0U, old_welcome.header.session_id);
	ASSERT_EQ(1U, lifecycle.native->active_sessions());
	const auto sent_before_lifecycle = lifecycle.backend.sent.size();
	lifecycle.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 101U, peer(3U))});
	runtime.on_game_mission_load();
	runtime.on_engine_update();
	EXPECT_EQ(runtime.mission_generation(), lifecycle.last_tick.mission_generation);
	ASSERT_GT(lifecycle.backend.sent.size(), sent_before_lifecycle);
	std::vector<std::uint8_t> new_storage;
	const auto new_welcome = decode_sent(
		lifecycle.backend, lifecycle.backend.sent.size() - 1U, new_storage);
	EXPECT_EQ(peer(3U), lifecycle.backend.send_endpoints.back());
	EXPECT_NE(old_welcome.header.session_id, new_welcome.header.session_id);
	EXPECT_EQ(1U, lifecycle.native->active_sessions());
	EXPECT_EQ(1U, lifecycle.native->owned_usage().active_slots)
		<< "The old peer is absent while exactly the new post-lifecycle peer survives.";

	// 2. Timeout > REL: WELCOME proof creates a retained SESSION_BEGIN.  At the
	// exact disconnect boundary, closure removes the slot/window before a due
	// retry can reach the socket.
	NativeFixture timeout;
	auto broad = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, timeout.start(broad));
	timeout.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 110U, peer(4U))});
	timeout.runtime.service_tick({1'000U, 0U, false});
	ASSERT_FALSE(timeout.backend.sent.empty());
	const auto welcome = timeout.backend.sent.back();
	timeout.backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(welcome, peer(4U), 111U)});
	timeout.runtime.service_tick({2'000U, 0U, false});
	ASSERT_GT(timeout.runtime.owned_usage().reliable_items, 0U);
	const auto sends_before_timeout = timeout.backend.send_calls;
	timeout.runtime.service_tick({2'000U + 10'000'000U, 0U, false});
	EXPECT_EQ(sends_before_timeout, timeout.backend.send_calls);
	EXPECT_EQ(detail::SessionControllerOwnedUsage{}, timeout.runtime.owned_usage());

	// 3. REL > periodic: client A is Ready with heartbeat due while client B has
	// a SESSION_BEGIN retry due.  The next emitted datagram must be B's byte-
	// identical reliable control, not A's heartbeat.
	NativeFixture priority;
	auto two_clients = enabled_config(1U);
	two_clients.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, priority.start(two_clients));
	(void)establish_ready(priority, peer(5U), 120U, 20'000U, 120U);
	const auto sent_before_second = priority.backend.sent.size();
	priority.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 121U, peer(6U))});
	for (std::uint64_t tick = 0U; tick < 8U && priority.backend.sent.size() == sent_before_second; ++tick) {
		priority.runtime.service_tick({21'000U + tick, 0U, false});
	}
	ASSERT_GT(priority.backend.sent.size(), sent_before_second);
	const auto second_welcome = priority.backend.sent[sent_before_second];
	const auto sent_before_begin = priority.backend.sent.size();
	priority.backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(second_welcome, peer(6U), 122U)});
	for (std::uint64_t tick = 0U; tick < 8U && priority.backend.sent.size() == sent_before_begin; ++tick) {
		priority.runtime.service_tick({22'000U + tick, 0U, false});
	}
	ASSERT_GT(priority.backend.sent.size(), sent_before_begin);
	ASSERT_EQ(protocol::MessageType::SessionBegin, sent_type(priority.backend, sent_before_begin));
	const auto simultaneous_due = 1'020'100U;
	const auto sent_before_due = priority.backend.sent.size();
	for (std::uint64_t offset = 0U; offset < 4U && priority.backend.sent.size() == sent_before_due; ++offset) {
		priority.runtime.service_tick({simultaneous_due + offset, 0U, false});
	}
	ASSERT_GT(priority.backend.sent.size(), sent_before_due);
	EXPECT_EQ(protocol::MessageType::SessionBegin,
		sent_type(priority.backend, sent_before_due));
	const auto sent_after_rel = priority.backend.sent.size();
	for (std::uint64_t offset = 4U; offset < 8U && priority.backend.sent.size() == sent_after_rel; ++offset) {
		priority.runtime.service_tick({simultaneous_due + offset, 0U, false});
	}
	ASSERT_GT(priority.backend.sent.size(), sent_after_rel);
	EXPECT_EQ(protocol::MessageType::Heartbeat,
		sent_type(priority.backend, sent_after_rel))
		<< "After the first due REL completes, the already-due heartbeat follows without waiting for another RTO.";

	// 4. Periodic > I/O: after persistent alternation selects egress first and
	// N=1, a due heartbeat consumes the sole syscall; an already queued HELLO
	// remains untouched until a later tick.
	NativeFixture periodic;
	auto one = enabled_config(1U);
	one.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, periodic.start(one));
	(void)establish_ready(periodic, peer(7U), 130U, 30'000U, 130U);
	periodic.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 131U, peer(8U))});
	const auto rx_before = periodic.backend.receive_script_index;
	const auto sent_before_periodic = periodic.backend.sent.size();
	// WELCOME proof is applied at 30'010.  The SESSION_BEGIN ACK consumed at
	// 30'020 makes the session Ready but does not re-anchor the periodic clock.
	constexpr std::uint64_t ExactPeriodicDueUs = 1'030'010U;
	periodic.runtime.service_tick({ExactPeriodicDueUs, 0U, false});
	ASSERT_GT(periodic.backend.sent.size(), sent_before_periodic);
	EXPECT_EQ(protocol::MessageType::Heartbeat,
		sent_type(periodic.backend, periodic.backend.sent.size() - 1U));
	EXPECT_EQ(rx_before, periodic.backend.receive_script_index)
		<< "Periodic scheduling precedes and consumes the N=1 I/O opportunity.";
}

TEST(TelemetryNativeRuntimeIntegrationContract, SharedBudgetAlternatesAcrossTicksAndWouldBlockStopsOnlyOneDirection)
{
	for (const auto invalid_budget : {0U, 257U}) {
		NativeFixture invalid;
		auto config = enabled_config(static_cast<std::uint16_t>(invalid_budget));
		EXPECT_EQ(detail::NativeSessionStartStatus::InvalidConfiguration, invalid.start(config));
		EXPECT_EQ(0U, invalid.backend.open_calls);
	}

	// With N=1 and both directions repeatedly made ready, the persistent first
	// direction alternation is externally visible as R,S,R,S across four ticks.
	NativeFixture alternating;
	auto one = enabled_config(1U);
	one.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, alternating.start(one));
	alternating.backend.io_trace.clear();
	alternating.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 20U, peer(2U))});
	alternating.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 21U, peer(3U))});
	for (std::uint64_t tick = 0U; tick < 4U; ++tick) {
		alternating.runtime.service_tick({2'000U + tick, 0U, false});
	}
	EXPECT_EQ((std::vector<char>{'R', 'S', 'R', 'S'}), alternating.backend.io_trace);

	// Preload a real WELCOME without any send attempt: three rejected Complete
	// receives followed by the HELLO consume all N=4 units.  The next two ticks
	// then prove TX-WouldBlock -> RX progress and RX-WouldBlock -> TX progress.
	NativeFixture blocked;
	auto four = enabled_config(4U);
	four.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, blocked.start(four));
	for (std::uint8_t source = 20U; source < 23U; ++source) {
		blocked.backend.receives.push_back({detail::IoStatus::Complete, Packet{{}, peer(source)}});
	}
	blocked.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 30U, peer(4U))});
	const auto setup_rx = blocked.backend.receive_calls;
	const auto setup_tx = blocked.backend.send_calls;
	blocked.runtime.service_tick({2'100U, 0U, false});
	EXPECT_EQ(4U, blocked.backend.receive_calls - setup_rx);
	EXPECT_EQ(0U, blocked.backend.send_calls - setup_tx);
	ASSERT_TRUE(blocked.runtime.owned_usage().output_queued);

	blocked.backend.sends.push_back(detail::IoStatus::WouldBlock);
	blocked.backend.receives.push_back({detail::IoStatus::Complete, Packet{{}, peer(23U)}});
	blocked.backend.io_trace.clear();
	const auto tx_block_rx = blocked.backend.receive_calls;
	const auto tx_block_tx = blocked.backend.send_calls;
	blocked.runtime.service_tick({2'101U, 0U, false});
	const auto tx_block_rx_delta = blocked.backend.receive_calls - tx_block_rx;
	const auto tx_block_tx_delta = blocked.backend.send_calls - tx_block_tx;
	ASSERT_FALSE(blocked.backend.io_trace.empty());
	EXPECT_EQ('S', blocked.backend.io_trace.front());
	EXPECT_EQ(1U, tx_block_tx_delta);
	EXPECT_GE(tx_block_rx_delta, 1U);
	EXPECT_LE(tx_block_rx_delta + tx_block_tx_delta, 4U);
	ASSERT_TRUE(blocked.runtime.owned_usage().output_queued);

	blocked.backend.io_trace.clear();
	blocked.backend.receives.push_back({detail::IoStatus::WouldBlock, {}});
	const auto rx_block_rx = blocked.backend.receive_calls;
	const auto rx_block_tx = blocked.backend.send_calls;
	blocked.runtime.service_tick({2'102U, 0U, false});
	const auto rx_block_rx_delta = blocked.backend.receive_calls - rx_block_rx;
	const auto rx_block_tx_delta = blocked.backend.send_calls - rx_block_tx;
	ASSERT_FALSE(blocked.backend.io_trace.empty());
	EXPECT_EQ('R', blocked.backend.io_trace.front());
	EXPECT_EQ(1U, rx_block_rx_delta);
	EXPECT_EQ(1U, rx_block_tx_delta);
	EXPECT_LE(rx_block_rx_delta + rx_block_tx_delta, 4U);
	EXPECT_FALSE(blocked.runtime.owned_usage().output_queued);
}

TEST(TelemetryNativeRuntimeIntegrationContract, AllowlistAndVersionNegotiationComposeWithoutDurableRejectedState)
{
	NativeFixture accepted;
	auto accepted_config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, accepted.start(accepted_config));
	accepted.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 10U)});
	accepted.runtime.service_tick({3'000U, 0U, false});
	ASSERT_EQ(1U, accepted.backend.sent.size());
	EXPECT_EQ(protocol::MessageType::Welcome, sent_type(accepted.backend, 0U));
	EXPECT_EQ(1U, accepted.runtime.active_sessions());

	NativeFixture unsupported;
	auto unsupported_config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, unsupported.start(unsupported_config));
	unsupported.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_0, 11U)});
	unsupported.runtime.service_tick({3'050U, 0U, false});
	ASSERT_EQ(1U, unsupported.backend.sent.size());
	std::vector<std::uint8_t> storage;
	const auto datagram = decode_sent(unsupported.backend, 0U, storage);
	ASSERT_EQ(protocol::VersionMinorV1_0, datagram.header.version_minor);
	ASSERT_EQ(protocol::MessageType::Welcome, datagram.header.message_type);
	EXPECT_EQ(0U, datagram.header.session_id);
	protocol::WelcomePayload welcome;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_welcome_payload(datagram.payload, welcome));
	EXPECT_EQ(protocol::WelcomeStatus::UnsupportedVersion, welcome.status);
	EXPECT_EQ(0U, welcome.selected_major);
	EXPECT_EQ(0U, welcome.selected_minor);
	const auto unsupported_usage = unsupported.runtime.owned_usage();
	EXPECT_EQ(0U, unsupported_usage.active_slots);
	EXPECT_EQ(0U, unsupported_usage.reliable_items);
	EXPECT_EQ(1U, unsupported_usage.cache_entries)
		<< "The bounded rejection replay cache is not a durable client slot or heavy REL window.";

	NativeFixture rejected;
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, rejected.start(config));
	const auto usage_before = rejected.runtime.owned_usage();
	const auto id_random_before = rejected.ids_random.next;
	const auto packet_random_before = rejected.packet_random.next;
	rejected.backend.receives.push_back({detail::IoStatus::Complete,
		hello(protocol::VersionMinorV1_1, 99U, protocol::EndpointKey::from_ipv4({192U, 0U, 2U, 1U}, 43000U))});
	rejected.runtime.service_tick({3'100U, 0U, false});
	EXPECT_TRUE(rejected.backend.sent.empty());
	EXPECT_EQ(usage_before, rejected.runtime.owned_usage());
	EXPECT_EQ(id_random_before, rejected.ids_random.next);
	EXPECT_EQ(packet_random_before, rejected.packet_random.next);
}

TEST(TelemetryNativeRuntimeIntegrationContract, PermanentReceiveClosedOrErrorPurgesAllAndNeverReopens)
{
	for (const auto status : {detail::IoStatus::Closed, detail::IoStatus::Error}) {
		NativeFixture fixture;
		auto config = enabled_config(64U);
		config.max_clients = 4U;
		ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
		(void)establish_ready(fixture, peer(10U), 200U, 40'000U, 200U);
		(void)establish_ready(fixture, peer(11U), 201U, 41'000U, 210U);
		fixture.backend.receives.push_back(
			{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 202U, peer(12U))});
		fixture.backend.sends.push_back(detail::IoStatus::WouldBlock);
		fixture.runtime.service_tick({42'000U, 0U, false});
		const auto rich = fixture.runtime.owned_usage();
		ASSERT_GE(rich.active_slots, 3U);
		ASSERT_GT(rich.cache_entries, 0U);
		ASSERT_GT(rich.preproof_accounts, 0U);
		ASSERT_GT(rich.reliable_items, 0U);
		ASSERT_TRUE(rich.output_queued);
		const auto closes_before = fixture.backend.closed.size();
		fixture.backend.receives.push_back({status, {}});
		EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure,
			fixture.runtime.service_tick({42'001U, 0U, false}));
		EXPECT_EQ(0U, fixture.runtime.socket_count());
		EXPECT_EQ(detail::SessionControllerOwnedUsage{}, fixture.runtime.owned_usage());
		EXPECT_EQ(closes_before + 1U, fixture.backend.closed.size());
		const auto calls = fixture.backend.open_calls + fixture.backend.receive_calls + fixture.backend.send_calls;
		fixture.runtime.service_tick({42'002U, 0U, false});
		fixture.runtime.service_tick({42'003U, 0U, false});
		EXPECT_EQ(calls, fixture.backend.open_calls + fixture.backend.receive_calls + fixture.backend.send_calls);
		EXPECT_EQ(closes_before + 1U, fixture.backend.closed.size());
	}
}

TEST(TelemetryNativeRuntimeIntegrationContract, PermanentSendClosedOrErrorCompletesOnceThenPurgesGlobally)
{
	for (const auto status : {detail::IoStatus::Closed, detail::IoStatus::Error}) {
		NativeFixture fixture;
		auto config = enabled_config(64U);
		config.max_clients = 4U;
		ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
		(void)establish_ready(fixture, peer(13U), 210U, 50'000U, 220U);
		(void)establish_ready(fixture, peer(14U), 211U, 51'000U, 230U);
		fixture.backend.receives.push_back(
			{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 212U, peer(15U))});
		fixture.backend.sends.push_back(detail::IoStatus::WouldBlock);
		fixture.runtime.service_tick({52'000U, 0U, false});
		const auto rich = fixture.runtime.owned_usage();
		ASSERT_GE(rich.active_slots, 3U);
		ASSERT_GT(rich.cache_entries, 0U);
		ASSERT_GT(rich.preproof_accounts, 0U);
		ASSERT_GT(rich.reliable_items, 0U);
		ASSERT_TRUE(rich.output_queued);
		const auto sends_before = fixture.backend.send_calls;
		const auto closes_before = fixture.backend.closed.size();
		const auto completions_before = fixture.completion.calls;
		fixture.backend.sends.push_back(status);
		EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure,
			fixture.runtime.service_tick({52'001U, 0U, false}));
		EXPECT_EQ(sends_before + 1U, fixture.backend.send_calls)
			<< "The selected controller output is completed exactly once on fatal send.";
		EXPECT_EQ(completions_before + 1U, fixture.completion.calls);
		EXPECT_EQ(status, fixture.completion.last_status);
		EXPECT_EQ(rich, fixture.completion.usage_before);
		EXPECT_FALSE(fixture.completion.usage_after.output_queued);
		EXPECT_EQ(rich.active_slots - 1U, fixture.completion.usage_after.active_slots);
		EXPECT_GT(fixture.completion.usage_after.active_slots, 0U)
			<< "The focused completion closes only its owner; global purge must happen afterwards.";
		EXPECT_EQ(0U, fixture.runtime.socket_count());
		EXPECT_EQ(detail::SessionControllerOwnedUsage{}, fixture.runtime.owned_usage());
		EXPECT_EQ(closes_before + 1U, fixture.backend.closed.size());
		fixture.runtime.service_tick({52'002U, 0U, false});
		fixture.runtime.service_tick({52'003U, 0U, false});
		EXPECT_EQ(sends_before + 1U, fixture.backend.send_calls);
		EXPECT_EQ(completions_before + 1U, fixture.completion.calls);
		EXPECT_EQ(closes_before + 1U, fixture.backend.closed.size());
		EXPECT_EQ(1U, fixture.backend.open_calls);
	}
}

TEST(TelemetryNativeRuntimeIntegrationContract, ShutdownAndWrongThreadNeverClockServiceOrDrain)
{
	RuntimeCompositionServices worker_services;
	worker_services.budget.is_complete = true;
	worker_services.budget.deferred_categories = 0U;
	detail::Runtime worker_runtime(worker_services);
	worker_runtime.capture_main_thread();
	worker_runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, worker_runtime.state());
	worker_services.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 300U)});
	worker_services.clock_calls = 0U;
	worker_services.service_calls = 0U;
	const auto rx_before = worker_services.backend.receive_calls;
	const auto tx_before = worker_services.backend.send_calls;
	std::thread worker([&] { worker_runtime.on_engine_update(); });
	worker.join();
	EXPECT_EQ(0U, worker_services.clock_calls);
	EXPECT_EQ(0U, worker_services.service_calls);
	EXPECT_EQ(rx_before, worker_services.backend.receive_calls);
	EXPECT_EQ(tx_before, worker_services.backend.send_calls);
	worker_services.teardown.clear();
	worker_runtime.on_engine_update();
	EXPECT_EQ(detail::RuntimeState::Faulted, worker_runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::MainThreadViolation, worker_runtime.terminal_reason());
	EXPECT_EQ((std::vector<char>{'1', '4', '2', '6', '8', '9'}), worker_services.teardown);
	EXPECT_EQ(nullptr, worker_services.native.get());
	EXPECT_EQ(0U, worker_services.clock_calls);
	EXPECT_EQ(0U, worker_services.service_calls);
	EXPECT_EQ(rx_before, worker_services.backend.receive_calls);
	EXPECT_EQ(tx_before, worker_services.backend.send_calls);
	EXPECT_EQ(1U, worker_services.backend.closed.size());

	RuntimeCompositionServices shutdown_services;
	shutdown_services.budget.is_complete = true;
	shutdown_services.budget.deferred_categories = 0U;
	detail::Runtime shutdown_runtime(shutdown_services);
	shutdown_runtime.capture_main_thread();
	shutdown_runtime.on_engine_update();
	shutdown_services.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 301U)});
	shutdown_services.clock_calls = 0U;
	shutdown_services.service_calls = 0U;
	shutdown_services.teardown.clear();
	const auto rx_before_shutdown = shutdown_services.backend.receive_calls;
	const auto tx_before_shutdown = shutdown_services.backend.send_calls;
	shutdown_runtime.on_engine_shutdown();
	EXPECT_EQ(detail::RuntimeState::Stopped, shutdown_runtime.state());
	EXPECT_EQ((std::vector<char>{'1', '4', '2', '6', '7', '8', '9'}),
		shutdown_services.teardown);
	EXPECT_EQ(0U, shutdown_services.clock_calls);
	EXPECT_EQ(0U, shutdown_services.service_calls);
	EXPECT_EQ(rx_before_shutdown, shutdown_services.backend.receive_calls);
	EXPECT_EQ(tx_before_shutdown, shutdown_services.backend.send_calls)
		<< "Shutdown purges pending work; it never drains the socket.";
	const auto closes = shutdown_services.backend.closed.size();
	const auto teardown = shutdown_services.teardown;
	shutdown_runtime.on_engine_shutdown();
	shutdown_runtime.on_engine_update();
	EXPECT_EQ(teardown, shutdown_services.teardown);
	EXPECT_EQ(closes, shutdown_services.backend.closed.size());
	EXPECT_EQ(0U, shutdown_services.clock_calls);
	EXPECT_EQ(0U, shutdown_services.service_calls);
}

#endif

} // namespace
