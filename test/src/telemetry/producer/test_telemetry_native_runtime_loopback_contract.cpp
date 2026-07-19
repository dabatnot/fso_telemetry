#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "telemetry/native_session_runtime.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/runtime.h"
#include "telemetry/startup_budget.h"
#include "telemetry/transport.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

constexpr std::size_t MaximumLoopbackAttempts = 16U;
constexpr std::size_t MaximumPumpIterations = 4096U;
constexpr auto MaximumPumpDuration = std::chrono::seconds(2);

bool native_loopback_opted_in() noexcept
{
	const auto* value = std::getenv("FSO_TELEMETRY_RUN_NATIVE_LOOPBACK");
	return value != nullptr && std::strcmp(value, "1") == 0;
}

class NativeSocketApiScope final {
  public:
	NativeSocketApiScope() noexcept
	{
#if defined(_WIN32)
		WSADATA data{};
		m_ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
	}

	~NativeSocketApiScope() noexcept
	{
#if defined(_WIN32)
		if (m_ready) WSACleanup();
#endif
	}

	bool ready() const noexcept { return m_ready; }

  private:
	bool m_ready = true;
};

#if defined(_WIN32)
using ProbeSocket = SOCKET;
constexpr ProbeSocket InvalidProbeSocket = INVALID_SOCKET;
void close_probe_socket(ProbeSocket socket) noexcept { closesocket(socket); }
int last_socket_error() noexcept { return WSAGetLastError(); }
#else
using ProbeSocket = int;
constexpr ProbeSocket InvalidProbeSocket = -1;
void close_probe_socket(ProbeSocket socket) noexcept { close(socket); }
int last_socket_error() noexcept { return errno; }
#endif

class ProbeSocketOwner final {
  public:
	explicit ProbeSocketOwner(ProbeSocket socket) noexcept : m_socket(socket) {}
	~ProbeSocketOwner() noexcept
	{
		if (m_socket != InvalidProbeSocket) close_probe_socket(m_socket);
	}
	ProbeSocket get() const noexcept { return m_socket; }
	void close() noexcept
	{
		if (m_socket != InvalidProbeSocket) {
			close_probe_socket(m_socket);
			m_socket = InvalidProbeSocket;
		}
	}

  private:
	ProbeSocket m_socket = InvalidProbeSocket;
};

enum class ProbeFailureStage : std::uint8_t { None = 0U, Socket, Bind, GetSockName };

const char* probe_failure_stage_name(ProbeFailureStage stage) noexcept
{
	switch (stage) {
	case ProbeFailureStage::None:
		return "none";
	case ProbeFailureStage::Socket:
		return "socket";
	case ProbeFailureStage::Bind:
		return "bind";
	case ProbeFailureStage::GetSockName:
		return "getsockname";
	}
	return "unknown";
}

struct PortProbeResult {
	bool family_available = false;
	std::uint16_t port = 0U;
	ProbeFailureStage failure_stage = ProbeFailureStage::None;
	int error = 0;
};

bool ipv6_family_unavailable(const PortProbeResult& probe) noexcept
{
	if (probe.family_available) return false;
#if defined(_WIN32)
	if (probe.failure_stage == ProbeFailureStage::Socket) {
		return probe.error == WSAEAFNOSUPPORT || probe.error == WSAEPROTONOSUPPORT;
	}
	return probe.failure_stage == ProbeFailureStage::Bind && probe.error == WSAEADDRNOTAVAIL;
#else
	if (probe.failure_stage == ProbeFailureStage::Socket) {
		return probe.error == EAFNOSUPPORT || probe.error == EPROTONOSUPPORT;
	}
	return probe.failure_stage == ProbeFailureStage::Bind && probe.error == EADDRNOTAVAIL;
#endif
}

PortProbeResult probe_server_port(protocol::IpAddressFamily family) noexcept
{
	const auto native_family = family == protocol::IpAddressFamily::Ipv4 ? AF_INET : AF_INET6;
	ProbeSocketOwner socket(::socket(native_family, SOCK_DGRAM, IPPROTO_UDP));
	if (socket.get() == InvalidProbeSocket) {
		return {false, 0U, ProbeFailureStage::Socket, last_socket_error()};
	}

	if (family == protocol::IpAddressFamily::Ipv4) {
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(0U);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
			return {false, 0U, ProbeFailureStage::Bind, last_socket_error()};
		}
#if defined(_WIN32)
		int length = static_cast<int>(sizeof(address));
#else
		socklen_t length = static_cast<socklen_t>(sizeof(address));
#endif
		if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
			return {false, 0U, ProbeFailureStage::GetSockName, last_socket_error()};
		}
		const auto port = ntohs(address.sin_port);
		socket.close();
		return {true, port, ProbeFailureStage::None, 0};
	}

	sockaddr_in6 address{};
	address.sin6_family = AF_INET6;
	address.sin6_port = htons(0U);
	address.sin6_addr = in6addr_loopback;
	if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
		return {false, 0U, ProbeFailureStage::Bind, last_socket_error()};
	}
#if defined(_WIN32)
	int length = static_cast<int>(sizeof(address));
#else
	socklen_t length = static_cast<socklen_t>(sizeof(address));
#endif
	if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
		return {false, 0U, ProbeFailureStage::GetSockName, last_socket_error()};
	}
	const auto port = ntohs(address.sin6_port);
	socket.close();
	return {true, port, ProbeFailureStage::None, 0};
}

telemetry::NumericIpAddress loopback_address(protocol::IpAddressFamily family) noexcept
{
	if (family == protocol::IpAddressFamily::Ipv4) {
		return telemetry::NumericIpAddress::from_ipv4({127U, 0U, 0U, 1U});
	}
	std::array<std::uint8_t, 16U> address{};
	address.back() = 1U;
	return telemetry::NumericIpAddress::from_ipv6(address);
}

protocol::EndpointKey endpoint_for(protocol::IpAddressFamily family, std::uint16_t port) noexcept
{
	if (family == protocol::IpAddressFamily::Ipv4) {
		return protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, port);
	}
	std::array<std::uint8_t, 16U> address{};
	address.back() = 1U;
	return protocol::EndpointKey::from_ipv6(address, port);
}

struct Packet {
	std::array<std::uint8_t, protocol::MaxDatagramSize> bytes{};
	std::size_t size = 0U;
};

bool encode_packet(protocol::TelemetryDatagramHeader header,
	protocol::ByteView payload,
	Packet& packet) noexcept
{
	header.message_size = static_cast<std::uint32_t>(payload.size);
	header.message_crc32 = protocol::crc32_iso_hdlc(payload);
	return protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{header.version_minor, header.version_minor},
			payload,
			{packet.bytes.data(), packet.bytes.size()},
			packet.size) == protocol::ValidationError::None;
}

bool make_hello(Packet& packet) noexcept
{
	protocol::HelloPayload hello;
	hello.client_nonce = 0x1122334455667788ULL;
	hello.client_send_t0_us = 10'000U;
	hello.min_major = protocol::VersionMajor;
	hello.max_major = protocol::VersionMajor;
	hello.min_minor = protocol::VersionMinorV1_1;
	hello.max_minor = protocol::VersionMinorV1_1;
	hello.requested_visibility_mode = protocol::VisibilityMode::Cockpit;
	hello.requested_heartbeat_ms = 1000U;
	std::array<std::uint8_t, protocol::HelloPayloadPrefixSize> payload{};
	std::size_t written = 0U;
	if (protocol::encode_hello_payload(hello, {payload.data(), payload.size()}, written) !=
		protocol::ValidationError::None) {
		return false;
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Hello;
	header.packet_sequence = 1U;
	header.sent_time_us = 10'000U;
	header.message_id = 1U;
	return encode_packet(header, {payload.data(), written}, packet);
}

bool decode_packet(const Packet& packet, protocol::DatagramView& decoded) noexcept
{
	return protocol::decode_and_validate_datagram({packet.bytes.data(), packet.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1},
			decoded) == protocol::ValidationError::None;
}

bool make_ack(const Packet& target, std::uint32_t packet_sequence, Packet& packet) noexcept
{
	protocol::DatagramView decoded;
	if (!decode_packet(target, decoded)) return false;
	protocol::AckPayload ack;
	ack.target_message_id = decoded.header.message_id;
	ack.target_message_type = decoded.header.message_type;
	ack.ack_flags = protocol::KnownAckFlags;
	ack.target_fragment_count = decoded.header.fragment_count;
	ack.target_message_crc32 = decoded.header.message_crc32;
	std::array<std::uint8_t, protocol::AckPayloadSize> payload{};
	std::size_t written = 0U;
	if (protocol::encode_ack_payload(ack, {payload.data(), payload.size()}, written) !=
		protocol::ValidationError::None) {
		return false;
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = decoded.header.version_minor;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = decoded.header.session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = 20'000U + packet_sequence;
	header.message_id = packet_sequence;
	return encode_packet(header, {payload.data(), written}, packet);
}

bool make_heartbeat_response(const protocol::DatagramView& request,
	const protocol::HeartbeatPayload& request_payload,
	Packet& packet) noexcept
{
	protocol::HeartbeatPayload response = request_payload;
	response.kind = protocol::HeartbeatKind::Response;
	response.receive_t1_us = request_payload.origin_t0_us + 10U;
	response.transmit_t2_us = request_payload.origin_t0_us + 20U;
	std::array<std::uint8_t, protocol::HeartbeatPayloadSize> payload{};
	std::size_t written = 0U;
	if (protocol::encode_heartbeat_payload(response, {payload.data(), payload.size()}, written) !=
		protocol::ValidationError::None) {
		return false;
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Heartbeat;
	header.session_id = request.header.session_id;
	header.packet_sequence = 40U;
	header.sent_time_us = request_payload.origin_t0_us + 30U;
	header.message_id = 40U;
	return encode_packet(header, {payload.data(), written}, packet);
}

struct IncrementingRandom final : detail::RandomSource {
	std::uint64_t next = 0x2222U;
	bool next_u64(std::uint64_t& output) noexcept override
	{
		output = next++;
		return true;
	}
};

struct CountingNativeBackend final : detail::UdpSocketBackend {
	detail::SocketOpenResult open_socket(const detail::SocketOpenRequest& request) noexcept override
	{
		++open_calls;
		auto result = native.open_socket(request);
		if (result.status == detail::SocketOpenStatus::Complete) ++successful_opens;
		return result;
	}
	detail::SocketReceiveResult try_receive(detail::SocketHandle handle,
		protocol::MutableByteView output) noexcept override
	{
		++receive_calls;
		auto result = native.try_receive(handle, output);
		if (result.status == detail::IoStatus::Complete) ++complete_receives;
		return result;
	}
	detail::SocketSendResult try_send(detail::SocketHandle handle,
		const protocol::EndpointKey& endpoint,
		protocol::ByteView bytes) noexcept override
	{
		++send_calls;
		return native.try_send(handle, endpoint, bytes);
	}
	void close_socket(detail::SocketHandle handle) noexcept override
	{
		++close_calls;
		native.close_socket(handle);
	}

	detail::NativeUdpSocketBackend native;
	std::size_t open_calls = 0U;
	std::size_t successful_opens = 0U;
	std::size_t receive_calls = 0U;
	std::size_t complete_receives = 0U;
	std::size_t send_calls = 0U;
	std::size_t close_calls = 0U;
};

struct LoopbackRuntimeServices final : detail::RuntimeStartupServices {
	LoopbackRuntimeServices(protocol::IpAddressFamily family, std::uint16_t port, bool complete_budget)
		: ids(id_random, registry)
	{
		config.enabled = true;
		config.bind_addresses.clear();
		config.bind_addresses.add(loopback_address(family));
		config.bind_port = port;
		config.max_clients = 1U;
		config.max_datagrams_per_tick = 8U;
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
		last_tick = context;
		if (!native.has_value()) return detail::RuntimeTickStatus::Unavailable;
		auto engine_view = detail::make_fso_engine_read_view();
		return native->service_tick(
			{context.now_us, context.mission_generation, context.mission_active}, engine_view) ==
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
		if (native.has_value()) native->purge_all(detail::SessionCloseReason::Shutdown);
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
	void release_session_registry() noexcept override {}
	void emit_startup_diagnostic(detail::RuntimeTerminalReason) noexcept override {}

	CountingNativeBackend backend;
	detail::NativeOutputCompletionForwarder completion;
	IncrementingRandom id_random;
	IncrementingRandom packet_random;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator ids;
	telemetry::TelemetryConfig config;
	detail::Wp03KnownBudgetSubtotal budget{};
	std::optional<detail::NativeSessionRuntime> native;
	detail::RuntimeTickContext last_tick{};
	std::uint64_t now_us = 30'000U;
	std::size_t start_transport_calls = 0U;
	std::size_t native_constructions = 0U;
	std::size_t sockets_after_stop = 0U;
	bool captured = false;
};

struct ServerFixture {
	ServerFixture(protocol::IpAddressFamily family, std::uint16_t port, bool complete_budget)
		: services(family, port, complete_budget), runtime(services)
	{
	}
	~ServerFixture() noexcept { runtime.on_engine_shutdown(); }
	void start()
	{
		runtime.capture_main_thread();
		runtime.on_engine_update();
	}

	LoopbackRuntimeServices services;
	detail::Runtime runtime;
};

class ClientSocketOwner final {
  public:
	ClientSocketOwner(detail::NativeUdpSocketBackend& backend, detail::SocketHandle handle) noexcept
		: m_backend(backend), m_handle(handle)
	{
	}
	~ClientSocketOwner() noexcept { close(); }
	void close() noexcept
	{
		if (m_handle != detail::InvalidSocketHandle) {
			m_backend.close_socket(m_handle);
			m_handle = detail::InvalidSocketHandle;
		}
	}
	bool closed() const noexcept { return m_handle == detail::InvalidSocketHandle; }

  private:
	detail::NativeUdpSocketBackend& m_backend;
	detail::SocketHandle m_handle = detail::InvalidSocketHandle;
};

struct PumpBudget {
	std::size_t iterations = 0U;
	std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + MaximumPumpDuration;

	bool available() const noexcept
	{
		return iterations < MaximumPumpIterations && std::chrono::steady_clock::now() < deadline;
	}
};

bool pump_once(ServerFixture& server, PumpBudget& budget) noexcept
{
	if (!budget.available()) return false;
	server.runtime.on_engine_update();
	++server.services.now_us;
	++budget.iterations;
	std::this_thread::yield();
	return true;
}

bool pump_until_packet(ServerFixture& server,
	detail::NativeUdpSocketBackend& client,
	detail::SocketHandle client_handle,
	protocol::MessageType wanted,
	PumpBudget& budget,
	Packet& packet) noexcept
{
	while (budget.available()) {
		if (!pump_once(server, budget)) return false;
		auto received = client.try_receive(client_handle, {packet.bytes.data(), packet.bytes.size()});
		if (received.status == detail::IoStatus::WouldBlock) continue;
		if (received.status != detail::IoStatus::Complete || received.truncated) return false;
		packet.size = received.bytes_received;
		protocol::DatagramView decoded;
		if (!decode_packet(packet, decoded)) return false;
		if (decoded.header.message_type == wanted) return true;
	}
	return false;
}

bool pump_until_server_receive(ServerFixture& server,
	std::size_t target_receives,
	PumpBudget& budget) noexcept
{
	while (server.services.backend.complete_receives < target_receives && budget.available()) {
		if (!pump_once(server, budget)) return false;
	}
	return server.services.backend.complete_receives >= target_receives;
}

testing::AssertionResult exercise_native_runtime_loopback(protocol::IpAddressFamily family,
	std::uint16_t first_port)
{
	{
		ServerFixture real_budget(family, first_port, false);
		if (real_budget.services.budget.deferred_categories != 0x0080U) {
			return testing::AssertionFailure() << "real startup mask changed from 0x0080";
		}
		real_budget.start();
		if (real_budget.runtime.state() != detail::RuntimeState::Faulted ||
			real_budget.runtime.terminal_reason() != detail::RuntimeTerminalReason::BudgetFailure ||
			real_budget.services.native_constructions != 0U || real_budget.services.backend.open_calls != 0U) {
			return testing::AssertionFailure()
				<< "real incomplete budget crossed native construction or bind boundary";
		}
	}

	std::unique_ptr<ServerFixture> server;
	std::uint16_t port = first_port;
	for (std::size_t attempt = 0U; attempt < MaximumLoopbackAttempts; ++attempt) {
		if (attempt != 0U) {
			const auto retry_probe = probe_server_port(family);
			if (!retry_probe.family_available) {
				return testing::AssertionFailure() << "ephemeral retry probe failed at "
					<< probe_failure_stage_name(retry_probe.failure_stage) << " with OS error "
					<< retry_probe.error;
			}
			port = retry_probe.port;
		}
		if (port < 1024U) continue;
		auto candidate = std::make_unique<ServerFixture>(family, port, true);
		candidate->start();
		if (candidate->runtime.state() == detail::RuntimeState::Ready) {
			server = std::move(candidate);
			break;
		}
	}
	if (!server) {
		return testing::AssertionFailure() << "server could not bind a probed ephemeral port in 16 attempts";
	}
	if (!server->services.native.has_value() || server->services.native->socket_count() != 1U ||
		server->services.backend.successful_opens != 1U) {
		return testing::AssertionFailure() << "synthetic Runtime did not own exactly one native server socket";
	}

	detail::NativeUdpSocketBackend client;
	const detail::SocketOpenRequest client_request{
		loopback_address(family), 0U, true, family == protocol::IpAddressFamily::Ipv6};
	const auto opened_client = client.open_socket(client_request);
	if (opened_client.status != detail::SocketOpenStatus::Complete ||
		opened_client.local_endpoint.port() == 0U) {
		return testing::AssertionFailure() << "native client failed to bind port zero";
	}
	ClientSocketOwner client_owner(client, opened_client.handle);
	const auto server_endpoint = endpoint_for(family, port);
	PumpBudget pump;

	Packet hello;
	if (!make_hello(hello) ||
		client.try_send(opened_client.handle, server_endpoint, {hello.bytes.data(), hello.size}).status !=
			detail::IoStatus::Complete) {
		return testing::AssertionFailure() << "native client could not send HELLO 1.1";
	}
	Packet welcome;
	if (!pump_until_packet(*server,
			client,
			opened_client.handle,
			protocol::MessageType::Welcome,
			pump,
			welcome)) {
		return testing::AssertionFailure() << "bounded pump did not receive WELCOME";
	}
	protocol::DatagramView welcome_view;
	protocol::WelcomePayload welcome_payload;
	if (!decode_packet(welcome, welcome_view) || welcome_view.header.version_minor != protocol::VersionMinorV1_1 ||
		welcome_view.header.session_id == 0U ||
		protocol::decode_welcome_payload(welcome_view.payload, welcome_payload) != protocol::ValidationError::None ||
		welcome_payload.status != protocol::WelcomeStatus::Accepted ||
		welcome_payload.selected_minor != protocol::VersionMinorV1_1) {
		return testing::AssertionFailure() << "WELCOME was not an accepted FSTL 1.1 negotiation";
	}

	Packet welcome_ack;
	if (!make_ack(welcome, 2U, welcome_ack) ||
		client.try_send(opened_client.handle,
			server_endpoint,
			{welcome_ack.bytes.data(), welcome_ack.size}).status != detail::IoStatus::Complete) {
		return testing::AssertionFailure() << "native client could not ACK WELCOME";
	}
	Packet session_begin;
	if (!pump_until_packet(*server,
			client,
			opened_client.handle,
			protocol::MessageType::SessionBegin,
			pump,
			session_begin)) {
		return testing::AssertionFailure() << "bounded pump did not receive SESSION_BEGIN";
	}
	const auto welcome_proof_time = server->services.last_tick.now_us;
	protocol::DatagramView begin_view;
	if (!decode_packet(session_begin, begin_view) || begin_view.header.session_id != welcome_view.header.session_id) {
		return testing::AssertionFailure() << "SESSION_BEGIN did not preserve the negotiated session";
	}

	Packet begin_ack;
	const auto receives_before_begin_ack = server->services.backend.complete_receives;
	if (!make_ack(session_begin, 3U, begin_ack) ||
		client.try_send(opened_client.handle, server_endpoint, {begin_ack.bytes.data(), begin_ack.size}).status !=
			detail::IoStatus::Complete ||
		!pump_until_server_receive(*server, receives_before_begin_ack + 1U, pump)) {
		return testing::AssertionFailure() << "bounded pump did not apply SESSION_BEGIN ACK";
	}
	if (!server->services.native.has_value() || server->services.native->active_sessions() != 1U) {
		return testing::AssertionFailure() << "server did not retain exactly one active session";
	}
	// The allowlist is address/CIDR based, but a live FSTL session is bound to
	// the exact UDP endpoint.  A second native loopback socket therefore has
	// an allowed address and a different source port: its attributed control
	// datagrams must be rejected without mutating the established session.
	detail::NativeUdpSocketBackend changed_endpoint_client;
	const auto opened_changed_endpoint = changed_endpoint_client.open_socket(client_request);
	if (opened_changed_endpoint.status != detail::SocketOpenStatus::Complete ||
		opened_changed_endpoint.local_endpoint.port() == 0U ||
		opened_changed_endpoint.local_endpoint.port() == opened_client.local_endpoint.port()) {
		return testing::AssertionFailure() << "native endpoint-change client failed to bind a distinct source port";
	}
	ClientSocketOwner changed_endpoint_owner(changed_endpoint_client, opened_changed_endpoint.handle);
	const auto sends_before_wrong_port_ack = server->services.backend.send_calls;
	const auto receives_before_wrong_port_ack = server->services.backend.complete_receives;
	if (changed_endpoint_client.try_send(opened_changed_endpoint.handle,
			server_endpoint,
			{begin_ack.bytes.data(), begin_ack.size}).status != detail::IoStatus::Complete ||
		!pump_until_server_receive(*server, receives_before_wrong_port_ack + 1U, pump) ||
		!server->services.native.has_value() || server->services.native->active_sessions() != 1U ||
		server->services.backend.send_calls != sends_before_wrong_port_ack) {
		return testing::AssertionFailure() << "wrong source port ACK changed the live session";
	}
	const auto activity_before_heartbeat = server->services.last_tick.now_us;

	server->services.now_us = welcome_proof_time + 1'000'000U;
	Packet heartbeat_request;
	if (!pump_until_packet(*server,
			client,
			opened_client.handle,
			protocol::MessageType::Heartbeat,
			pump,
			heartbeat_request)) {
		return testing::AssertionFailure() << "bounded pump did not receive periodic HEARTBEAT request";
	}
	protocol::DatagramView heartbeat_view;
	protocol::HeartbeatPayload heartbeat_payload;
	if (!decode_packet(heartbeat_request, heartbeat_view) ||
		protocol::decode_heartbeat_payload(heartbeat_view.payload, heartbeat_payload) !=
			protocol::ValidationError::None ||
		heartbeat_payload.kind != protocol::HeartbeatKind::Request) {
		return testing::AssertionFailure() << "periodic packet was not a canonical HEARTBEAT request";
	}

	Packet heartbeat_response;
	const auto receives_before_heartbeat = server->services.backend.complete_receives;
	// The response's causal receive/transmit timestamps must not be in the future
	// relative to the synthetic server clock that consumes it.
	server->services.now_us = heartbeat_payload.origin_t0_us + 20U;
	if (!make_heartbeat_response(heartbeat_view, heartbeat_payload, heartbeat_response)) {
		return testing::AssertionFailure() << "could not encode canonical HEARTBEAT response";
	}
	const auto sends_before_endpoint_change = server->services.backend.send_calls;
	if (changed_endpoint_client.try_send(opened_changed_endpoint.handle,
			server_endpoint,
			{heartbeat_response.bytes.data(), heartbeat_response.size}).status != detail::IoStatus::Complete ||
		!pump_until_server_receive(*server, receives_before_heartbeat + 1U, pump) ||
		!server->services.native.has_value() || server->services.native->active_sessions() != 1U ||
		server->services.backend.send_calls != sends_before_endpoint_change) {
		return testing::AssertionFailure() << "endpoint-change HEARTBEAT response changed the live session";
	}
	const auto receives_before_expected_heartbeat = server->services.backend.complete_receives;
	if (
		client.try_send(opened_client.handle,
			server_endpoint,
			{heartbeat_response.bytes.data(), heartbeat_response.size}).status != detail::IoStatus::Complete ||
		!pump_until_server_receive(*server, receives_before_expected_heartbeat + 1U, pump)) {
		return testing::AssertionFailure() << "bounded pump did not apply HEARTBEAT response";
	}
	if (!server->services.native.has_value() || server->services.native->active_sessions() != 1U) {
		return testing::AssertionFailure() << "HEARTBEAT response did not preserve the active session";
	}
	server->services.now_us = activity_before_heartbeat + 10'000'000U;
	if (!pump_once(*server, pump) || !server->services.native.has_value() ||
		server->services.native->active_sessions() != 1U) {
		return testing::AssertionFailure()
			<< "HEARTBEAT response was not accepted as activity beyond the prior disconnect boundary";
	}

	server->runtime.on_engine_shutdown();
	const auto closes_after_shutdown = server->services.backend.close_calls;
	server->runtime.on_engine_shutdown();
	if (server->runtime.state() != detail::RuntimeState::Stopped || server->services.sockets_after_stop != 0U ||
		closes_after_shutdown != 1U || server->services.backend.close_calls != closes_after_shutdown) {
		return testing::AssertionFailure() << "server shutdown was not socket-zero and idempotent";
	}
	client_owner.close();
	changed_endpoint_owner.close();
	if (!client_owner.closed() || !changed_endpoint_owner.closed()) return testing::AssertionFailure() << "client socket did not close";
	return testing::AssertionSuccess();
}

TEST(TelemetryNativeRuntimeLoopbackContract, IPv4NegotiationHeartbeatAndShutdown)
{
	if (!native_loopback_opted_in()) GTEST_SKIP() << "set FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1 to run native loopback";
	NativeSocketApiScope socket_api;
	ASSERT_TRUE(socket_api.ready()) << "IPv4 loopback requires an available native socket API";
	const auto probe = probe_server_port(protocol::IpAddressFamily::Ipv4);
	ASSERT_TRUE(probe.family_available) << "IPv4 loopback preflight failed at "
		<< probe_failure_stage_name(probe.failure_stage) << " with OS error " << probe.error;
	ASSERT_TRUE(exercise_native_runtime_loopback(protocol::IpAddressFamily::Ipv4, probe.port));
}

TEST(TelemetryNativeRuntimeLoopbackContract, IPv6NegotiationHeartbeatAndShutdown)
{
	if (!native_loopback_opted_in()) GTEST_SKIP() << "set FSO_TELEMETRY_RUN_NATIVE_LOOPBACK=1 to run native loopback";
	NativeSocketApiScope socket_api;
	ASSERT_TRUE(socket_api.ready()) << "IPv6 WinSock bootstrap failed before family preflight";
	const auto probe = probe_server_port(protocol::IpAddressFamily::Ipv6);
	if (ipv6_family_unavailable(probe)) {
		GTEST_SKIP() << "IPv6 loopback unavailable at " << probe_failure_stage_name(probe.failure_stage)
					 << " with explicit family/address OS error " << probe.error;
	}
	ASSERT_TRUE(probe.family_available) << "IPv6 loopback preflight failed at "
		<< probe_failure_stage_name(probe.failure_stage) << " with unexpected OS error " << probe.error;
	ASSERT_TRUE(exercise_native_runtime_loopback(protocol::IpAddressFamily::Ipv6, probe.port));
}

} // namespace
