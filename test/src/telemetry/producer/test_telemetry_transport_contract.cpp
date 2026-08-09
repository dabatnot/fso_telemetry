#if defined(_WIN32)
#include <winsock2.h>
#endif

#include "telemetry/transport.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

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
		if (m_ready) {
			WSACleanup();
		}
#endif
	}

	bool ready() const noexcept
	{
		return m_ready;
	}

  private:
	bool m_ready = true;
};

telemetry::NumericIpAddress ipv4_loopback_address() noexcept
{
	return telemetry::NumericIpAddress::from_ipv4({127U, 0U, 0U, 1U});
}

telemetry::NumericIpAddress ipv6_loopback_address() noexcept
{
	std::array<std::uint8_t, 16> address{};
	address.back() = 1U;
	return telemetry::NumericIpAddress::from_ipv6(address);
}

telemetry::NumericIpAddress ipv6_wildcard_address() noexcept
{
	return telemetry::NumericIpAddress::from_ipv6({});
}

telemetry::NumericIpAddress ipv4_documentation_address() noexcept
{
	return telemetry::NumericIpAddress::from_ipv4({192U, 0U, 2U, 10U});
}

protocol::EndpointKey endpoint_for(const telemetry::NumericIpAddress& address, std::uint16_t port) noexcept
{
	if (address.family() == protocol::IpAddressFamily::Ipv4) {
		std::array<std::uint8_t, 4> bytes{};
		std::copy_n(address.bytes().begin(), bytes.size(), bytes.begin());
		return protocol::EndpointKey::from_ipv4(bytes, port);
	}
	return protocol::EndpointKey::from_ipv6(address.bytes(), port);
}

protocol::EndpointKey ipv4_endpoint(std::uint16_t port = 42043U) noexcept
{
	return endpoint_for(ipv4_loopback_address(), port);
}

protocol::EndpointKey ipv6_endpoint(std::uint16_t port = 42043U) noexcept
{
	return endpoint_for(ipv6_loopback_address(), port);
}

detail::SocketOpenResult successful_open(const detail::SocketOpenRequest& request, detail::SocketHandle handle) noexcept
{
	detail::SocketOpenResult result;
	result.status = detail::SocketOpenStatus::Complete;
	result.handle = handle;
	result.local_endpoint = endpoint_for(request.bind_address, request.port == 0U ? 49152U : request.port);
	return result;
}

class ScriptedUdpSocketBackend final : public detail::UdpSocketBackend {
  public:
	detail::SocketOpenResult open_socket(const detail::SocketOpenRequest& request) noexcept override
	{
		open_requests.push_back(request);
		if (next_open_result < open_results.size()) {
			return open_results[next_open_result++];
		}
		return successful_open(request, next_handle++);
	}

	detail::SocketReceiveResult try_receive(detail::SocketHandle handle,
		protocol::MutableByteView output) noexcept override
	{
		receive_handles.push_back(handle);
		detail::SocketReceiveResult result;
		if (next_receive_result < receive_results.size()) {
			result = receive_results[next_receive_result++];
		}
		if (result.status == detail::IoStatus::Complete && result.bytes_received <= output.size &&
			result.bytes_received <= receive_payload.size()) {
			std::copy_n(receive_payload.begin(), result.bytes_received, output.data);
		}
		return result;
	}

	detail::SocketSendResult try_send(detail::SocketHandle handle,
		const protocol::EndpointKey& endpoint,
		protocol::ByteView bytes) noexcept override
	{
		send_handles.push_back(handle);
		send_endpoints.push_back(endpoint);
		send_data_pointers.push_back(bytes.data);
		sent_payloads.emplace_back(bytes.data, bytes.data + bytes.size);
		if (next_send_result < send_results.size()) {
			return send_results[next_send_result++];
		}
		return {detail::IoStatus::Complete, bytes.size};
	}

	void close_socket(detail::SocketHandle handle) noexcept override
	{
		closed_handles.push_back(handle);
		logical_socket_counts_during_close.push_back(observed_transport == nullptr ? 0U
																				   : observed_transport->socket_count());
	}

	std::vector<detail::SocketOpenResult> open_results;
	std::vector<detail::SocketReceiveResult> receive_results;
	std::vector<detail::SocketSendResult> send_results;
	std::vector<std::uint8_t> receive_payload;

	std::vector<detail::SocketOpenRequest> open_requests;
	std::vector<detail::SocketHandle> receive_handles;
	std::vector<detail::SocketHandle> send_handles;
	std::vector<protocol::EndpointKey> send_endpoints;
	std::vector<const std::uint8_t*> send_data_pointers;
	std::vector<std::vector<std::uint8_t>> sent_payloads;
	std::vector<detail::SocketHandle> closed_handles;
	std::vector<std::size_t> logical_socket_counts_during_close;
	const detail::DedicatedUdpTransport* observed_transport = nullptr;

  private:
	std::size_t next_open_result = 0U;
	std::size_t next_receive_result = 0U;
	std::size_t next_send_result = 0U;
	detail::SocketHandle next_handle = 10;
};

telemetry::TelemetryConfig enabled_default_config() noexcept
{
	telemetry::TelemetryConfig config;
	config.enabled = true;
	return config;
}

telemetry::TelemetryConfig enabled_dual_stack_wildcard_config() noexcept
{
	auto config = enabled_default_config();
	config.bind_addresses.clear();
	config.bind_addresses.add(ipv6_wildcard_address());

	config.allowed_clients.clear();
	protocol::IpCidr ipv4_loopback;
	protocol::IpCidr explicit_ipv6_client;
	std::array<std::uint8_t, 16> ipv6_client{};
	ipv6_client[0] = 0x20U;
	ipv6_client[1] = 0x01U;
	ipv6_client[2] = 0x0dU;
	ipv6_client[3] = 0xb8U;
	ipv6_client.back() = 1U;
	protocol::IpCidr::from_ipv4({127U, 0U, 0U, 1U}, 32U, ipv4_loopback);
	protocol::IpCidr::from_ipv6(ipv6_client, 128U, explicit_ipv6_client);
	config.allowed_clients.add(ipv4_loopback);
	config.allowed_clients.add(explicit_ipv6_client);
	return config;
}

telemetry::TelemetryConfig enabled_dual_stack_ipv4_allowlist_config() noexcept
{
	auto config = enabled_dual_stack_wildcard_config();
	config.allowed_clients.clear();
	protocol::IpCidr explicit_ipv4_client;
	protocol::IpCidr::from_ipv4({192U, 0U, 2U, 1U}, 32U, explicit_ipv4_client);
	config.allowed_clients.add(explicit_ipv4_client);
	return config;
}

protocol::IpCidr ipv4_catch_all() noexcept
{
	protocol::IpCidr catch_all;
	protocol::IpCidr::from_ipv4({0U, 0U, 0U, 0U}, 0U, catch_all);
	return catch_all;
}

protocol::IpCidr ipv6_catch_all() noexcept
{
	protocol::IpCidr catch_all;
	protocol::IpCidr::from_ipv6({}, 0U, catch_all);
	return catch_all;
}

static_assert(protocol::MaxDatagramSize == 1200U, "P1-REQ-012 freezes the direct UDP datagram ceiling.");
static_assert(std::is_polymorphic_v<detail::UdpSocketBackend>);
static_assert(static_cast<unsigned int>(detail::IoStatus::Complete) == 0U);
static_assert(static_cast<unsigned int>(detail::IoStatus::WouldBlock) == 1U);
static_assert(static_cast<unsigned int>(detail::IoStatus::Closed) == 2U);
static_assert(static_cast<unsigned int>(detail::IoStatus::Error) == 3U,
	"IoStatus remains the four-value syscall/transport status contract.");
static_assert(static_cast<unsigned int>(detail::TransportReceiveDisposition::None) == 0U);
static_assert(static_cast<unsigned int>(detail::TransportReceiveDisposition::Datagram) == 1U);
static_assert(static_cast<unsigned int>(detail::TransportReceiveDisposition::Rejected) == 2U,
	"A rejected datagram is distinct from a permanent socket error.");
static_assert(!std::is_copy_constructible_v<detail::NativeUdpSocketBackend>,
	"Copying the fixed native handle registry would create stale aliases and double-close risk.");
static_assert(!std::is_copy_assignable_v<detail::NativeUdpSocketBackend>,
	"The native handle registry has unique ownership and cannot be copy-assigned.");
static_assert(!std::is_move_constructible_v<detail::NativeUdpSocketBackend>,
	"Moving the native handle registry without an explicit ownership transfer would leave stale handles.");
static_assert(!std::is_move_assignable_v<detail::NativeUdpSocketBackend>,
	"The native handle registry has unique ownership and cannot be move-assigned.");
static_assert(sizeof(detail::NativeUdpSocketBackend) <= 512U,
	"The native handle registry is fixed embedded metadata, not variable/preallocated application-buffer budget.");
static_assert(std::is_nothrow_destructible_v<detail::DedicatedUdpTransport>);
static_assert(noexcept(std::declval<detail::DedicatedUdpTransport&>().open(
	std::declval<const telemetry::TelemetryConfig&>())));
static_assert(noexcept(std::declval<detail::DedicatedUdpTransport&>().try_receive()));
static_assert(noexcept(std::declval<detail::DedicatedUdpTransport&>().try_send(
	std::declval<const protocol::EndpointKey&>(), std::declval<protocol::ByteView>())));
static_assert(noexcept(std::declval<detail::DedicatedUdpTransport&>().close()));

TEST(TelemetryTransportContract, DisabledAndMalformedConfigurationsNeverReachTheSocketBackend)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);

	telemetry::TelemetryConfig disabled;
	EXPECT_EQ(detail::TransportOpenStatus::Disabled, transport.open(disabled));
	EXPECT_TRUE(backend.open_requests.empty());
	EXPECT_FALSE(transport.is_open());

	auto empty_bind = enabled_default_config();
	empty_bind.bind_addresses.clear();
	EXPECT_EQ(detail::TransportOpenStatus::InvalidConfiguration, transport.open(empty_bind));
	EXPECT_TRUE(backend.open_requests.empty());
	EXPECT_FALSE(transport.is_open());
}

TEST(TelemetryTransportContract, SpecificNonLoopbackBindAcceptsANonemptyCatchAllAllowlist)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	auto config = enabled_default_config();
	config.bind_addresses.clear();
	config.bind_addresses.add(ipv4_documentation_address());
	config.allowed_clients.clear();
	config.allowed_clients.add(ipv4_catch_all());

	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(config))
		<< "For a specific literal non-loopback bind, section 4.1 requires only an explicit nonempty allowlist; "
		   "the /0 prohibition is scoped to wildcard binds.";
	ASSERT_EQ(1U, backend.open_requests.size());
	EXPECT_EQ(ipv4_documentation_address(), backend.open_requests[0].bind_address);
}

TEST(TelemetryTransportContract, WildcardBindRejectsCatchAllAllowlistBeforeOpeningASocket)
{
	for (const auto family : {protocol::IpAddressFamily::Ipv4, protocol::IpAddressFamily::Ipv6}) {
		SCOPED_TRACE(static_cast<unsigned int>(family));
		ScriptedUdpSocketBackend backend;
		detail::DedicatedUdpTransport transport(backend);
		auto config = enabled_default_config();
		config.bind_addresses.clear();
		config.allowed_clients.clear();
		if (family == protocol::IpAddressFamily::Ipv4) {
			config.bind_addresses.add(telemetry::NumericIpAddress::from_ipv4({}));
			config.allowed_clients.add(ipv4_catch_all());
		} else {
			config.bind_addresses.add(ipv6_wildcard_address());
			config.allowed_clients.add(ipv6_catch_all());
		}

		EXPECT_EQ(detail::TransportOpenStatus::InvalidConfiguration, transport.open(config));
		EXPECT_TRUE(backend.open_requests.empty());
		EXPECT_FALSE(transport.is_open());
	}
}

TEST(TelemetryTransportContract, EnabledDefaultsBindOnlyBothLoopbacksAsPrivateNonBlockingSockets)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	backend.observed_transport = &transport;

	const auto config = enabled_default_config();
	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(config));
	ASSERT_EQ(2U, backend.open_requests.size());
	EXPECT_TRUE(transport.is_open());
	EXPECT_EQ(2U, transport.socket_count());

	const auto& ipv4 = backend.open_requests[0];
	EXPECT_EQ(protocol::IpAddressFamily::Ipv4, ipv4.bind_address.family());
	EXPECT_TRUE(ipv4.bind_address.is_loopback());
	EXPECT_FALSE(ipv4.bind_address.is_wildcard());
	EXPECT_EQ(protocol::DefaultTelemetryPort, ipv4.port);
	EXPECT_TRUE(ipv4.non_blocking);
	EXPECT_FALSE(ipv4.ipv6_only);

	const auto& ipv6 = backend.open_requests[1];
	EXPECT_EQ(protocol::IpAddressFamily::Ipv6, ipv6.bind_address.family());
	EXPECT_TRUE(ipv6.bind_address.is_loopback());
	EXPECT_FALSE(ipv6.bind_address.is_wildcard());
	EXPECT_EQ(protocol::DefaultTelemetryPort, ipv6.port);
	EXPECT_TRUE(ipv6.non_blocking);
	EXPECT_TRUE(ipv6.ipv6_only)
		<< "Two-socket mode must not let the IPv6 listener alias the dedicated IPv4 listener.";

	EXPECT_EQ(ipv4_endpoint(protocol::DefaultTelemetryPort),
		transport.local_endpoint(protocol::IpAddressFamily::Ipv4));
	EXPECT_EQ(ipv6_endpoint(protocol::DefaultTelemetryPort),
		transport.local_endpoint(protocol::IpAddressFamily::Ipv6));

	transport.close();
	transport.close();
	EXPECT_FALSE(transport.is_open());
	EXPECT_EQ(0U, transport.socket_count());
	EXPECT_EQ((std::vector<detail::SocketHandle>{10, 11}), backend.closed_handles);
	EXPECT_EQ((std::vector<std::size_t>{0U, 0U}), backend.logical_socket_counts_during_close)
		<< "Logical handles must be invalid before either physical close callback.";
}

TEST(TelemetryTransportContract, EveryOpenFailureRollsBackOnceAndPreservesTheClosedInvariant)
{
	struct FailureCase {
		detail::SocketOpenStatus socket_status;
		detail::TransportOpenStatus transport_status;
	};
	const std::array<FailureCase, 5> cases{{
		{detail::SocketOpenStatus::SocketCreationFailed, detail::TransportOpenStatus::SocketCreationFailed},
		{detail::SocketOpenStatus::NonBlockingFailed, detail::TransportOpenStatus::NonBlockingFailed},
		{detail::SocketOpenStatus::SocketOptionFailed, detail::TransportOpenStatus::SocketOptionFailed},
		{detail::SocketOpenStatus::BindFailed, detail::TransportOpenStatus::BindFailed},
		{detail::SocketOpenStatus::AddressVerificationFailed,
			detail::TransportOpenStatus::AddressVerificationFailed},
	}};

	for (const auto& item : cases) {
		SCOPED_TRACE(static_cast<unsigned int>(item.socket_status));
		ScriptedUdpSocketBackend backend;
		detail::DedicatedUdpTransport transport(backend);
		backend.observed_transport = &transport;
		auto config = enabled_default_config();
		const detail::SocketOpenRequest first_request{ipv4_loopback_address(), config.bind_port, true, false};
		backend.open_results.push_back(successful_open(first_request, 77));
		backend.open_results.push_back({item.socket_status, detail::InvalidSocketHandle, {}});

		EXPECT_EQ(item.transport_status, transport.open(config));
		EXPECT_FALSE(transport.is_open());
		EXPECT_EQ(0U, transport.socket_count());
		EXPECT_EQ((std::vector<detail::SocketHandle>{77}), backend.closed_handles);
		EXPECT_EQ((std::vector<std::size_t>{0U}), backend.logical_socket_counts_during_close);

		transport.close();
		EXPECT_EQ(1U, backend.closed_handles.size());
	}
}

TEST(TelemetryTransportContract, UnverifiableCurrentSocketRollsBackCurrentThenPriorInReverseOrder)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	backend.observed_transport = &transport;
	const auto config = enabled_default_config();
	const detail::SocketOpenRequest first_request{ipv4_loopback_address(), config.bind_port, true, false};
	backend.open_results.push_back(successful_open(first_request, 41U));
	backend.open_results.push_back(
		{detail::SocketOpenStatus::Complete, 42U, ipv4_endpoint(config.bind_port)});

	EXPECT_EQ(detail::TransportOpenStatus::AddressVerificationFailed, transport.open(config));
	EXPECT_FALSE(transport.is_open());
	EXPECT_EQ(0U, transport.socket_count());
	EXPECT_EQ((std::vector<detail::SocketHandle>{42U, 41U}), backend.closed_handles)
		<< "The uncommitted current socket must close before earlier committed sockets during reverse rollback.";
	EXPECT_EQ((std::vector<std::size_t>{0U, 0U}), backend.logical_socket_counts_during_close)
		<< "No physical close callback may observe a logically published transport socket.";
}

TEST(TelemetryTransportContract, EffectivePortMismatchOrZeroFailsAddressVerificationAndRollsBack)
{
	for (const auto effective_port : std::array<std::uint16_t, 2>{{0U, 42043U}}) {
		SCOPED_TRACE(effective_port);
		ScriptedUdpSocketBackend backend;
		detail::DedicatedUdpTransport transport(backend);
		backend.observed_transport = &transport;
		auto config = enabled_default_config();
		config.bind_addresses.clear();
		config.bind_addresses.add(ipv4_loopback_address());
		ASSERT_EQ(42042U, config.bind_port);
		backend.open_results.push_back(
			{detail::SocketOpenStatus::Complete, 77U, ipv4_endpoint(effective_port)});

		EXPECT_EQ(detail::TransportOpenStatus::AddressVerificationFailed, transport.open(config))
			<< "A nonzero requested port must equal the effective port returned by the socket backend.";
		EXPECT_FALSE(transport.is_open());
		EXPECT_EQ(0U, transport.socket_count());
		EXPECT_EQ((std::vector<detail::SocketHandle>{77U}), backend.closed_handles);
		EXPECT_EQ((std::vector<std::size_t>{0U}), backend.logical_socket_counts_during_close);
		if (transport.is_open()) {
			transport.close();
		}
	}
}

TEST(TelemetryTransportContract, ReceivePollsAtMostOneSocketAndMapsWouldBlockClosedErrorAndComplete)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(enabled_default_config()));

	backend.receive_payload = {0x10U, 0x20U, 0x30U, 0x40U};
	backend.receive_results = {
		{detail::IoStatus::WouldBlock, {}, 0U, false},
		{detail::IoStatus::Complete, ipv6_endpoint(), backend.receive_payload.size(), false},
		{detail::IoStatus::Closed, {}, 0U, false},
		{detail::IoStatus::Error, {}, 0U, false},
	};

	const auto blocked = transport.try_receive();
	EXPECT_EQ(detail::IoStatus::WouldBlock, blocked.status);
	EXPECT_EQ(detail::TransportReceiveDisposition::None, blocked.disposition);
	EXPECT_EQ(1U, backend.receive_handles.size());
	EXPECT_EQ(detail::SocketHandle{10U}, backend.receive_handles.back());

	const auto received = transport.try_receive();
	EXPECT_EQ(detail::IoStatus::Complete, received.status);
	EXPECT_EQ(detail::TransportReceiveDisposition::Datagram, received.disposition);
	EXPECT_EQ(ipv6_endpoint(), received.datagram.endpoint);
	ASSERT_EQ(backend.receive_payload.size(), received.datagram.bytes.size);
	EXPECT_TRUE(std::equal(backend.receive_payload.begin(),
		backend.receive_payload.end(),
		received.datagram.bytes.data));
	EXPECT_EQ(2U, backend.receive_handles.size());
	EXPECT_EQ(detail::SocketHandle{11U}, backend.receive_handles.back())
		<< "The receive cursor must rotate instead of polling both sockets in one call.";

	const auto closed = transport.try_receive();
	EXPECT_EQ(detail::IoStatus::Closed, closed.status);
	EXPECT_EQ(detail::TransportReceiveDisposition::None, closed.disposition);
	EXPECT_EQ(3U, backend.receive_handles.size());
	const auto error = transport.try_receive();
	EXPECT_EQ(detail::IoStatus::Error, error.status);
	EXPECT_EQ(detail::TransportReceiveDisposition::None, error.disposition);
	EXPECT_EQ(4U, backend.receive_handles.size());
}

TEST(TelemetryTransportContract, SendRoutesByFamilyMapsBackendStatusAndRejectsInvalidSizesWithoutASyscall)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(enabled_default_config()));

	const std::array<std::uint8_t, 4> payload{{1U, 2U, 3U, 4U}};
	backend.send_results = {
		{detail::IoStatus::Complete, payload.size()},
		{detail::IoStatus::WouldBlock, 0U},
		{detail::IoStatus::Closed, 0U},
		{detail::IoStatus::Error, 0U},
	};

	EXPECT_EQ(detail::IoStatus::Complete,
		transport.try_send(ipv4_endpoint(), {payload.data(), payload.size()}));
	EXPECT_EQ(detail::SocketHandle{10U}, backend.send_handles.back());
	EXPECT_EQ(detail::IoStatus::WouldBlock,
		transport.try_send(ipv6_endpoint(), {payload.data(), payload.size()}));
	EXPECT_EQ(detail::SocketHandle{11U}, backend.send_handles.back());
	EXPECT_EQ(detail::IoStatus::Closed,
		transport.try_send(ipv4_endpoint(), {payload.data(), payload.size()}));
	EXPECT_EQ(detail::IoStatus::Error,
		transport.try_send(ipv4_endpoint(), {payload.data(), payload.size()}));
	EXPECT_EQ(4U, backend.send_handles.size());

	std::array<std::uint8_t, protocol::MaxDatagramSize + 1U> oversized{};
	EXPECT_EQ(detail::IoStatus::Error,
		transport.try_send(ipv4_endpoint(), {oversized.data(), oversized.size()}));
	EXPECT_EQ(detail::IoStatus::Error, transport.try_send(ipv4_endpoint(), {}));
	EXPECT_EQ(detail::IoStatus::Error,
		transport.try_send(protocol::EndpointKey{}, {payload.data(), payload.size()}));
	EXPECT_EQ(4U, backend.send_handles.size())
		<< "Invalid or oversized datagrams must be rejected before the socket backend.";

	std::array<std::uint8_t, protocol::MaxDatagramSize> maximum{};
	EXPECT_EQ(detail::IoStatus::Complete,
		transport.try_send(ipv4_endpoint(), {maximum.data(), maximum.size()}));
	EXPECT_EQ(5U, backend.send_handles.size());
}

TEST(TelemetryTransportContract, SingleDualStackSocketRoutesCanonicalIpv4AndIpv6Endpoints)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(enabled_dual_stack_wildcard_config()));
	ASSERT_EQ(1U, backend.open_requests.size());
	EXPECT_EQ(protocol::IpAddressFamily::Ipv6, backend.open_requests[0].bind_address.family());
	EXPECT_TRUE(backend.open_requests[0].bind_address.is_wildcard());
	EXPECT_FALSE(backend.open_requests[0].ipv6_only)
		<< "The single explicitly configured IPv6 wildcard socket is the verified dual-stack mode.";

	const std::array<std::uint8_t, 4> payload{{0x46U, 0x53U, 0x54U, 0x4cU}};
	ASSERT_EQ(detail::IoStatus::Complete,
		transport.try_send(ipv4_endpoint(), {payload.data(), payload.size()}))
		<< "A canonical IPv4 endpoint must route through the one dual-stack IPv6 socket.";
	ASSERT_EQ(1U, backend.send_handles.size());
	EXPECT_EQ(10U, backend.send_handles[0]);
	EXPECT_EQ(ipv4_endpoint(), backend.send_endpoints[0])
		<< "IPv4-mapped receive addresses are canonical IPv4 EndpointKey values.";

	ASSERT_EQ(detail::IoStatus::Complete,
		transport.try_send(ipv6_endpoint(), {payload.data(), payload.size()}));
	ASSERT_EQ(2U, backend.send_handles.size());
	EXPECT_EQ(10U, backend.send_handles[1]);
}

TEST(TelemetryTransportContract, ExplicitDualStackWildcardAcceptsANarrowedIpv4OnlyAllowlist)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);

	ASSERT_EQ(detail::TransportOpenStatus::Complete,
		transport.open(enabled_dual_stack_ipv4_allowlist_config()))
		<< "An explicit nonempty IPv4 allowlist is sufficient for IPv4 peers of the dual-stack socket; "
		   "unlisted IPv6 sources remain denied instead of forcing broader exposure.";
	ASSERT_EQ(1U, backend.open_requests.size());
	EXPECT_EQ(protocol::IpAddressFamily::Ipv6, backend.open_requests[0].bind_address.family());
	EXPECT_FALSE(backend.open_requests[0].ipv6_only);
}

TEST(TelemetryTransportContract, SendCopiesExactlyOneDatagramIntoPreallocatedTransportStorage)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(enabled_default_config()));

	std::array<std::uint8_t, protocol::MaxDatagramSize> caller_buffer{};
	for (std::size_t index = 0U; index < caller_buffer.size(); ++index) {
		caller_buffer[index] = static_cast<std::uint8_t>((index * 17U + 3U) & 0xffU);
	}
	const auto expected = caller_buffer;

	ASSERT_EQ(detail::IoStatus::Complete,
		transport.try_send(ipv4_endpoint(), {caller_buffer.data(), caller_buffer.size()}));
	ASSERT_EQ(1U, backend.send_data_pointers.size());
	EXPECT_NE(caller_buffer.data(), backend.send_data_pointers[0])
		<< "The backend must never retain or observe the caller-owned ByteView directly.";
	ASSERT_EQ(1U, backend.sent_payloads.size());
	EXPECT_EQ(std::vector<std::uint8_t>(expected.begin(), expected.end()), backend.sent_payloads[0]);

	std::fill(caller_buffer.begin(), caller_buffer.end(), std::uint8_t{0U});
	EXPECT_EQ(std::vector<std::uint8_t>(expected.begin(), expected.end()), backend.sent_payloads[0])
		<< "The one-syscall backend view must originate from transport-owned fixed storage.";
}

TEST(TelemetryTransportContract, PartialSendImpossibleLengthAndTruncatedDatagramFailClosed)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(enabled_default_config()));

	const std::array<std::uint8_t, 4> payload{{1U, 2U, 3U, 4U}};
	backend.send_results.push_back({detail::IoStatus::Complete, payload.size() - 1U});
	EXPECT_EQ(detail::IoStatus::Error,
		transport.try_send(ipv4_endpoint(), {payload.data(), payload.size()}));

	backend.receive_results.push_back(
		{detail::IoStatus::Complete, ipv4_endpoint(), protocol::MaxDatagramSize + 1U, false});
	auto rejected = transport.try_receive();
	EXPECT_EQ(detail::IoStatus::Complete, rejected.status);
	EXPECT_EQ(detail::TransportReceiveDisposition::Rejected, rejected.disposition);
	EXPECT_TRUE(rejected.datagram.bytes.empty());
	backend.receive_results.push_back(
		{detail::IoStatus::Complete, ipv4_endpoint(), protocol::MaxDatagramSize, true});
	rejected = transport.try_receive();
	EXPECT_EQ(detail::IoStatus::Complete, rejected.status);
	EXPECT_EQ(detail::TransportReceiveDisposition::Rejected, rejected.disposition);
	EXPECT_TRUE(rejected.datagram.bytes.empty());
	backend.receive_results.push_back({detail::IoStatus::Complete, ipv4_endpoint(), 0U, false});
	rejected = transport.try_receive();
	EXPECT_EQ(detail::IoStatus::Complete, rejected.status);
	EXPECT_EQ(detail::TransportReceiveDisposition::Rejected, rejected.disposition);
	EXPECT_TRUE(rejected.datagram.bytes.empty());
	backend.receive_results.push_back({detail::IoStatus::Complete, {}, payload.size(), false});
	rejected = transport.try_receive();
	EXPECT_EQ(detail::IoStatus::Complete, rejected.status);
	EXPECT_EQ(detail::TransportReceiveDisposition::Rejected, rejected.disposition);
	EXPECT_TRUE(rejected.datagram.bytes.empty());
}

TEST(TelemetryTransportContract, CloseMakesIoInertAndTheSameObjectCanBeOpenedAgainWithoutLeakingHandles)
{
	ScriptedUdpSocketBackend backend;
	detail::DedicatedUdpTransport transport(backend);
	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(enabled_default_config()));
	EXPECT_EQ(detail::TransportOpenStatus::AlreadyOpen, transport.open(enabled_default_config()));
	EXPECT_EQ(2U, backend.open_requests.size());

	transport.close();
	const std::array<std::uint8_t, 1> byte{{1U}};
	EXPECT_EQ(detail::IoStatus::Closed, transport.try_receive().status);
	EXPECT_EQ(detail::IoStatus::Closed, transport.try_send(ipv4_endpoint(), {byte.data(), byte.size()}));
	EXPECT_TRUE(backend.receive_handles.empty());
	EXPECT_TRUE(backend.send_handles.empty());

	ASSERT_EQ(detail::TransportOpenStatus::Complete, transport.open(enabled_default_config()));
	EXPECT_TRUE(transport.is_open());
	EXPECT_EQ(2U, transport.socket_count());
	EXPECT_EQ(4U, backend.open_requests.size());
	transport.close();
	EXPECT_EQ(4U, backend.closed_handles.size());
	EXPECT_EQ((std::vector<detail::SocketHandle>{10, 11, 12, 13}), backend.closed_handles);
}

void exercise_native_loopback(protocol::IpAddressFamily family)
{
	NativeSocketApiScope socket_api;
	ASSERT_TRUE(socket_api.ready()) << "Native tests own a balanced WinSock bootstrap outside production.";
	detail::NativeUdpSocketBackend backend;
	const auto address = family == protocol::IpAddressFamily::Ipv4 ? ipv4_loopback_address()
																								 : ipv6_loopback_address();
	const detail::SocketOpenRequest request{address, 0U, true, family == protocol::IpAddressFamily::Ipv6};

	const auto receiver = backend.open_socket(request);
	ASSERT_EQ(detail::SocketOpenStatus::Complete, receiver.status);
	ASSERT_TRUE(receiver.local_endpoint.is_valid());
	EXPECT_NE(0U, receiver.local_endpoint.port())
		<< "A native request for ephemeral port zero must publish the nonzero effective port.";
	const auto sender = backend.open_socket(request);
	ASSERT_EQ(detail::SocketOpenStatus::Complete, sender.status);
	ASSERT_TRUE(sender.local_endpoint.is_valid());
	EXPECT_NE(0U, sender.local_endpoint.port())
		<< "A native request for ephemeral port zero must publish the nonzero effective port.";

	std::array<std::uint8_t, protocol::MaxDatagramSize> receive_buffer{};
	auto empty = backend.try_receive(receiver.handle, {receive_buffer.data(), receive_buffer.size()});
	EXPECT_EQ(detail::IoStatus::WouldBlock, empty.status)
		<< "A freshly opened loopback socket must be observably non-blocking.";

	const std::array<std::uint8_t, 6> payload{{0x46U, 0x53U, 0x54U, 0x4cU, 0x01U, 0x01U}};
	const auto sent = backend.try_send(sender.handle, receiver.local_endpoint, {payload.data(), payload.size()});
	ASSERT_EQ(detail::IoStatus::Complete, sent.status);
	ASSERT_EQ(payload.size(), sent.bytes_sent);

	detail::SocketReceiveResult received;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	for (std::size_t attempt = 0U;
		attempt < 2048U && std::chrono::steady_clock::now() < deadline;
		++attempt) {
		received = backend.try_receive(receiver.handle, {receive_buffer.data(), receive_buffer.size()});
		if (received.status != detail::IoStatus::WouldBlock) {
			break;
		}
		std::this_thread::yield();
	}
	EXPECT_EQ(detail::IoStatus::Complete, received.status);
	EXPECT_FALSE(received.truncated);
	EXPECT_EQ(sender.local_endpoint, received.source_endpoint);
	ASSERT_EQ(payload.size(), received.bytes_received);
	EXPECT_TRUE(std::equal(payload.begin(), payload.end(), receive_buffer.begin()));

	backend.close_socket(sender.handle);
	backend.close_socket(receiver.handle);
}

detail::SocketReceiveResult receive_native_bounded(detail::NativeUdpSocketBackend& backend,
	detail::SocketHandle handle,
	std::array<std::uint8_t, protocol::MaxDatagramSize>& receive_buffer)
{
	detail::SocketReceiveResult received;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	for (std::size_t attempt = 0U;
		attempt < 2048U && std::chrono::steady_clock::now() < deadline;
		++attempt) {
		received = backend.try_receive(handle, {receive_buffer.data(), receive_buffer.size()});
		if (received.status != detail::IoStatus::WouldBlock) {
			break;
		}
		std::this_thread::yield();
	}
	return received;
}

TEST(TelemetryTransportContract, NativeBackendSendsAndReceivesDirectUdpOnIpv4AndIpv6Loopback)
{
	exercise_native_loopback(protocol::IpAddressFamily::Ipv4);
	exercise_native_loopback(protocol::IpAddressFamily::Ipv6);
}

TEST(TelemetryTransportContract, NativeDualStackWildcardExchangesIpv4AndIpv6WithCanonicalEndpoints)
{
	NativeSocketApiScope socket_api;
	ASSERT_TRUE(socket_api.ready()) << "Native tests own a balanced WinSock bootstrap outside production.";
	detail::NativeUdpSocketBackend backend;
	const detail::SocketOpenRequest dual_stack_request{ipv6_wildcard_address(), 0U, true, false};
	const detail::SocketOpenRequest ipv4_request{ipv4_loopback_address(), 0U, true, false};
	const detail::SocketOpenRequest ipv6_request{ipv6_loopback_address(), 0U, true, true};

	const auto dual_stack = backend.open_socket(dual_stack_request);
	ASSERT_EQ(detail::SocketOpenStatus::Complete, dual_stack.status);
	ASSERT_EQ(protocol::IpAddressFamily::Ipv6, dual_stack.local_endpoint.family());
	ASSERT_NE(0U, dual_stack.local_endpoint.port());
	const auto ipv4_peer = backend.open_socket(ipv4_request);
	ASSERT_EQ(detail::SocketOpenStatus::Complete, ipv4_peer.status);
	const auto ipv6_peer = backend.open_socket(ipv6_request);
	ASSERT_EQ(detail::SocketOpenStatus::Complete, ipv6_peer.status);

	std::array<std::uint8_t, protocol::MaxDatagramSize> receive_buffer{};
	const std::array<std::uint8_t, 4> ipv4_payload{{0x04U, 0x46U, 0x53U, 0x54U}};
	const auto ipv4_destination = ipv4_endpoint(dual_stack.local_endpoint.port());
	const auto ipv4_sent = backend.try_send(ipv4_peer.handle,
		ipv4_destination,
		{ipv4_payload.data(), ipv4_payload.size()});
	ASSERT_EQ(detail::IoStatus::Complete, ipv4_sent.status);

	const auto ipv4_received = receive_native_bounded(backend, dual_stack.handle, receive_buffer);
	ASSERT_EQ(detail::IoStatus::Complete, ipv4_received.status);
	EXPECT_EQ(protocol::IpAddressFamily::Ipv4, ipv4_received.source_endpoint.family())
		<< "The AF_INET6 socket reports IPv4 peers as mapped addresses; EndpointKey must canonicalize them.";
	EXPECT_EQ(ipv4_peer.local_endpoint, ipv4_received.source_endpoint);
	ASSERT_EQ(ipv4_payload.size(), ipv4_received.bytes_received);
	EXPECT_TRUE(std::equal(ipv4_payload.begin(), ipv4_payload.end(), receive_buffer.begin()));

	const std::array<std::uint8_t, 3> ipv4_reply{{0x14U, 0x01U, 0x01U}};
	const auto ipv4_reply_sent = backend.try_send(dual_stack.handle,
		ipv4_received.source_endpoint,
		{ipv4_reply.data(), ipv4_reply.size()});
	EXPECT_EQ(detail::IoStatus::Complete, ipv4_reply_sent.status)
		<< "A dual-stack AF_INET6 socket must map the canonical IPv4 EndpointKey back to sockaddr_in6.";
	if (ipv4_reply_sent.status == detail::IoStatus::Complete) {
		const auto ipv4_reply_received = receive_native_bounded(backend, ipv4_peer.handle, receive_buffer);
		EXPECT_EQ(detail::IoStatus::Complete, ipv4_reply_received.status);
		EXPECT_EQ(ipv4_reply.size(), ipv4_reply_received.bytes_received);
		EXPECT_TRUE(std::equal(ipv4_reply.begin(), ipv4_reply.end(), receive_buffer.begin()));
	}

	const std::array<std::uint8_t, 4> ipv6_payload{{0x06U, 0x46U, 0x53U, 0x54U}};
	const auto ipv6_destination = ipv6_endpoint(dual_stack.local_endpoint.port());
	const auto ipv6_sent = backend.try_send(ipv6_peer.handle,
		ipv6_destination,
		{ipv6_payload.data(), ipv6_payload.size()});
	ASSERT_EQ(detail::IoStatus::Complete, ipv6_sent.status);
	const auto ipv6_received = receive_native_bounded(backend, dual_stack.handle, receive_buffer);
	ASSERT_EQ(detail::IoStatus::Complete, ipv6_received.status);
	EXPECT_EQ(protocol::IpAddressFamily::Ipv6, ipv6_received.source_endpoint.family());
	EXPECT_EQ(ipv6_peer.local_endpoint, ipv6_received.source_endpoint);
	ASSERT_EQ(ipv6_payload.size(), ipv6_received.bytes_received);
	EXPECT_TRUE(std::equal(ipv6_payload.begin(), ipv6_payload.end(), receive_buffer.begin()));

	const std::array<std::uint8_t, 3> ipv6_reply{{0x16U, 0x01U, 0x01U}};
	const auto ipv6_reply_sent = backend.try_send(dual_stack.handle,
		ipv6_received.source_endpoint,
		{ipv6_reply.data(), ipv6_reply.size()});
	EXPECT_EQ(detail::IoStatus::Complete, ipv6_reply_sent.status);
	if (ipv6_reply_sent.status == detail::IoStatus::Complete) {
		const auto ipv6_reply_received = receive_native_bounded(backend, ipv6_peer.handle, receive_buffer);
		EXPECT_EQ(detail::IoStatus::Complete, ipv6_reply_received.status);
		EXPECT_EQ(ipv6_reply.size(), ipv6_reply_received.bytes_received);
		EXPECT_TRUE(std::equal(ipv6_reply.begin(), ipv6_reply.end(), receive_buffer.begin()));
	}

	backend.close_socket(ipv6_peer.handle);
	backend.close_socket(ipv4_peer.handle);
	backend.close_socket(dual_stack.handle);
}

} // namespace
