#include "telemetry/transport.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace telemetry::detail {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket InvalidNativeSocket = INVALID_SOCKET;

int last_socket_error() noexcept
{
	return WSAGetLastError();
}

bool is_would_block(int error) noexcept
{
	return error == WSAEWOULDBLOCK || error == WSAEINTR;
}

bool is_closed_error(int error) noexcept
{
	return error == WSAENOTSOCK || error == WSAESHUTDOWN || error == WSAECONNABORTED;
}

void close_native_socket(NativeSocket socket) noexcept
{
	closesocket(socket);
}
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr NativeSocket InvalidNativeSocket = -1;

int last_socket_error() noexcept
{
	return errno;
}

bool is_would_block(int error) noexcept
{
	return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}

bool is_closed_error(int error) noexcept
{
	return error == EBADF || error == ENOTSOCK || error == EPIPE;
}

void close_native_socket(NativeSocket socket) noexcept
{
	::close(socket);
}
#endif

NativeSocket native_socket(SocketHandle handle) noexcept
{
	return static_cast<NativeSocket>(handle);
}

SocketHandle public_handle(NativeSocket socket) noexcept
{
	return static_cast<SocketHandle>(socket);
}

bool make_sockaddr(const NumericIpAddress& address,
	std::uint16_t port,
	sockaddr_storage& storage,
	SocketLength& length) noexcept
{
	std::memset(&storage, 0, sizeof(storage));
	if (address.family() == protocol::IpAddressFamily::Ipv4) {
		auto* value = reinterpret_cast<sockaddr_in*>(&storage);
		value->sin_family = AF_INET;
		value->sin_port = htons(port);
		std::memcpy(&value->sin_addr, address.bytes().data(), 4U);
		length = static_cast<SocketLength>(sizeof(sockaddr_in));
		return true;
	}
	if (address.family() == protocol::IpAddressFamily::Ipv6) {
		auto* value = reinterpret_cast<sockaddr_in6*>(&storage);
		value->sin6_family = AF_INET6;
		value->sin6_port = htons(port);
		std::memcpy(&value->sin6_addr, address.bytes().data(), 16U);
		length = static_cast<SocketLength>(sizeof(sockaddr_in6));
		return true;
	}
	return false;
}

bool make_sockaddr(const protocol::EndpointKey& endpoint,
	sockaddr_storage& storage,
	SocketLength& length) noexcept
{
	std::memset(&storage, 0, sizeof(storage));
	if (endpoint.family() == protocol::IpAddressFamily::Ipv4) {
		auto* value = reinterpret_cast<sockaddr_in*>(&storage);
		value->sin_family = AF_INET;
		value->sin_port = htons(endpoint.port());
		std::memcpy(&value->sin_addr, endpoint.address().data(), 4U);
		length = static_cast<SocketLength>(sizeof(sockaddr_in));
		return true;
	}
	if (endpoint.family() == protocol::IpAddressFamily::Ipv6) {
		auto* value = reinterpret_cast<sockaddr_in6*>(&storage);
		value->sin6_family = AF_INET6;
		value->sin6_port = htons(endpoint.port());
		std::memcpy(&value->sin6_addr, endpoint.address().data(), 16U);
		length = static_cast<SocketLength>(sizeof(sockaddr_in6));
		return true;
	}
	return false;
}

bool make_ipv4_mapped_sockaddr(const protocol::EndpointKey& endpoint,
	sockaddr_storage& storage,
	SocketLength& length) noexcept
{
	if (endpoint.family() != protocol::IpAddressFamily::Ipv4) {
		return false;
	}
	std::memset(&storage, 0, sizeof(storage));
	auto* value = reinterpret_cast<sockaddr_in6*>(&storage);
	value->sin6_family = AF_INET6;
	value->sin6_port = htons(endpoint.port());
	auto* address = reinterpret_cast<std::uint8_t*>(&value->sin6_addr);
	address[10] = 0xffU;
	address[11] = 0xffU;
	std::copy_n(endpoint.address().begin(), 4U, address + 12);
	length = static_cast<SocketLength>(sizeof(sockaddr_in6));
	return true;
}

protocol::EndpointKey endpoint_from_sockaddr(const sockaddr_storage& storage) noexcept
{
	if (storage.ss_family == AF_INET) {
		const auto* value = reinterpret_cast<const sockaddr_in*>(&storage);
		std::array<std::uint8_t, 4> address{};
		std::memcpy(address.data(), &value->sin_addr, address.size());
		return protocol::EndpointKey::from_ipv4(address, ntohs(value->sin_port));
	}
	if (storage.ss_family == AF_INET6) {
		const auto* value = reinterpret_cast<const sockaddr_in6*>(&storage);
		std::array<std::uint8_t, 16> address{};
		std::memcpy(address.data(), &value->sin6_addr, address.size());
		return protocol::EndpointKey::from_ipv6(address, ntohs(value->sin6_port));
	}
	return {};
}

bool endpoint_matches_address(const protocol::EndpointKey& endpoint,
	const NumericIpAddress& address) noexcept
{
	if (!endpoint.is_valid() || endpoint.family() != address.family()) {
		return false;
	}
	const auto length = address.family() == protocol::IpAddressFamily::Ipv4 ? 4U : 16U;
	return std::equal(address.bytes().begin(), address.bytes().begin() + length, endpoint.address().begin());
}

IoStatus map_io_error(int error) noexcept
{
	if (is_would_block(error)) {
		return IoStatus::WouldBlock;
	}
	if (is_closed_error(error)) {
		return IoStatus::Closed;
	}
	return IoStatus::Error;
}

TransportOpenStatus map_open_status(SocketOpenStatus status) noexcept
{
	switch (status) {
	case SocketOpenStatus::SocketCreationFailed:
		return TransportOpenStatus::SocketCreationFailed;
	case SocketOpenStatus::NonBlockingFailed:
		return TransportOpenStatus::NonBlockingFailed;
	case SocketOpenStatus::SocketOptionFailed:
		return TransportOpenStatus::SocketOptionFailed;
	case SocketOpenStatus::BindFailed:
		return TransportOpenStatus::BindFailed;
	case SocketOpenStatus::AddressVerificationFailed:
		return TransportOpenStatus::AddressVerificationFailed;
	case SocketOpenStatus::Complete:
		return TransportOpenStatus::Complete;
	}
	return TransportOpenStatus::SocketCreationFailed;
}

bool allowlist_has_catch_all(const protocol::SourceAllowlist& allowlist) noexcept
{
	for (std::size_t index = 0U; index < allowlist.size(); ++index) {
		if (allowlist[index].prefix_length() == 0U) {
			return true;
		}
	}
	return false;
}

} // namespace

bool NativeUdpSocketBackend::remember_socket(SocketHandle handle,
	protocol::IpAddressFamily family,
	bool dual_stack) noexcept
{
	if (handle == InvalidSocketHandle || family == protocol::IpAddressFamily::Invalid) {
		return false;
	}
	for (auto& record : m_socket_records) {
		if (record.handle == handle) {
			return false;
		}
	}
	for (auto& record : m_socket_records) {
		if (record.handle == InvalidSocketHandle) {
			record.handle = handle;
			record.family = family;
			record.dual_stack = dual_stack;
			return true;
		}
	}
	return false;
}

const NativeUdpSocketBackend::SocketRecord* NativeUdpSocketBackend::find_socket(SocketHandle handle) const noexcept
{
	for (const auto& record : m_socket_records) {
		if (record.handle == handle) {
			return &record;
		}
	}
	return nullptr;
}

void NativeUdpSocketBackend::forget_socket(SocketHandle handle) noexcept
{
	for (auto& record : m_socket_records) {
		if (record.handle == handle) {
			record = {};
			return;
		}
	}
}

SocketOpenResult NativeUdpSocketBackend::open_socket(const SocketOpenRequest& request) noexcept
{
	SocketOpenResult result;
	if (!request.bind_address.is_valid()) {
		result.status = SocketOpenStatus::AddressVerificationFailed;
		return result;
	}

	const auto family = request.bind_address.family() == protocol::IpAddressFamily::Ipv4 ? AF_INET : AF_INET6;
	const auto socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if (socket == InvalidNativeSocket) {
		result.status = SocketOpenStatus::SocketCreationFailed;
		return result;
	}

#if defined(_WIN32)
	BOOL report_udp_connection_reset = FALSE;
	DWORD control_bytes = 0U;
	if (WSAIoctl(socket,
			SIO_UDP_CONNRESET,
			&report_udp_connection_reset,
			static_cast<DWORD>(sizeof(report_udp_connection_reset)),
			nullptr,
			0U,
			&control_bytes,
			nullptr,
			nullptr) == SOCKET_ERROR) {
		close_native_socket(socket);
		result.status = SocketOpenStatus::SocketOptionFailed;
		return result;
	}
#endif

	if (family == AF_INET6) {
		const int ipv6_only = request.ipv6_only ? 1 : 0;
#if defined(_WIN32)
		const auto* option = reinterpret_cast<const char*>(&ipv6_only);
#else
		const auto* option = &ipv6_only;
#endif
		if (setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, option, sizeof(ipv6_only)) != 0) {
			close_native_socket(socket);
			result.status = SocketOpenStatus::SocketOptionFailed;
			return result;
		}
#if defined(_WIN32)
		std::array<std::uint8_t, sizeof(int)> observed_bytes{};
		SocketLength observed_length = static_cast<SocketLength>(observed_bytes.size());
		const auto option_result = getsockopt(socket,
			IPPROTO_IPV6,
			IPV6_V6ONLY,
			reinterpret_cast<char*>(observed_bytes.data()),
			&observed_length);
		const auto observed_length_valid = observed_length > 0 &&
			observed_length <= static_cast<SocketLength>(observed_bytes.size());
		const auto observed_enabled = observed_length_valid && std::any_of(observed_bytes.begin(),
			observed_bytes.begin() + observed_length,
			[](std::uint8_t byte) { return byte != 0U; });
#else
		int observed_ipv6_only = 0;
		SocketLength observed_length = static_cast<SocketLength>(sizeof(observed_ipv6_only));
		const auto option_result = getsockopt(
			socket, IPPROTO_IPV6, IPV6_V6ONLY, &observed_ipv6_only, &observed_length);
		const auto observed_length_valid = observed_length == sizeof(observed_ipv6_only);
		const auto observed_enabled = observed_ipv6_only != 0;
#endif
		if (option_result != 0 || !observed_length_valid || observed_enabled != request.ipv6_only) {
			close_native_socket(socket);
			result.status = SocketOpenStatus::SocketOptionFailed;
			return result;
		}
	}

	if (request.non_blocking) {
#if defined(_WIN32)
		u_long enabled = 1UL;
		if (ioctlsocket(socket, FIONBIO, &enabled) != 0) {
#else
		const auto flags = fcntl(socket, F_GETFL, 0);
		if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) {
#endif
			close_native_socket(socket);
			result.status = SocketOpenStatus::NonBlockingFailed;
			return result;
		}
	}

	sockaddr_storage bind_address{};
	SocketLength bind_length = 0;
	if (!make_sockaddr(request.bind_address, request.port, bind_address, bind_length) ||
		::bind(socket, reinterpret_cast<const sockaddr*>(&bind_address), bind_length) != 0) {
		close_native_socket(socket);
		result.status = SocketOpenStatus::BindFailed;
		return result;
	}

	sockaddr_storage local_address{};
	SocketLength local_length = static_cast<SocketLength>(sizeof(local_address));
	if (getsockname(socket, reinterpret_cast<sockaddr*>(&local_address), &local_length) != 0) {
		close_native_socket(socket);
		result.status = SocketOpenStatus::AddressVerificationFailed;
		return result;
	}
	result.local_endpoint = endpoint_from_sockaddr(local_address);
	const auto port_matches = request.port == 0U ? result.local_endpoint.port() != 0U
															 : result.local_endpoint.port() == request.port;
	if (!endpoint_matches_address(result.local_endpoint, request.bind_address) || !port_matches) {
		close_native_socket(socket);
		result.local_endpoint = {};
		result.status = SocketOpenStatus::AddressVerificationFailed;
		return result;
	}

	result.handle = public_handle(socket);
	if (!remember_socket(result.handle,
			request.bind_address.family(),
			request.bind_address.family() == protocol::IpAddressFamily::Ipv6 && !request.ipv6_only)) {
		close_native_socket(socket);
		result.handle = InvalidSocketHandle;
		result.local_endpoint = {};
		result.status = SocketOpenStatus::SocketCreationFailed;
		return result;
	}
	result.status = SocketOpenStatus::Complete;
	return result;
}

SocketReceiveResult NativeUdpSocketBackend::try_receive(SocketHandle handle,
	protocol::MutableByteView output) noexcept
{
	SocketReceiveResult result;
	if (handle == InvalidSocketHandle || find_socket(handle) == nullptr || output.data == nullptr ||
		output.empty()) {
		return result;
	}

	sockaddr_storage source{};
	SocketLength source_length = static_cast<SocketLength>(sizeof(source));
#if defined(_WIN32)
	WSABUF buffer{static_cast<ULONG>(output.size), reinterpret_cast<char*>(output.data)};
	DWORD flags = 0U;
	DWORD bytes_received = 0U;
	const auto call_result = WSARecvFrom(native_socket(handle),
		&buffer,
		1U,
		&bytes_received,
		&flags,
		reinterpret_cast<sockaddr*>(&source),
		&source_length,
		nullptr,
		nullptr);
	if (call_result == SOCKET_ERROR) {
		const auto error = last_socket_error();
		if (error == WSAEMSGSIZE) {
			result.status = IoStatus::Complete;
			result.source_endpoint = endpoint_from_sockaddr(source);
			result.bytes_received = output.size;
			result.truncated = true;
			return result;
		}
		result.status = map_io_error(error);
		return result;
	}
	result.bytes_received = bytes_received;
#else
	iovec buffer{output.data, output.size};
	msghdr message{};
	message.msg_name = &source;
	message.msg_namelen = source_length;
	message.msg_iov = &buffer;
	message.msg_iovlen = 1U;
	// MSG_TRUNC is an output condition.  Passing it as an input flag is not
	// portable: BSD-derived stacks may report it back even for a datagram that
	// completely fits the supplied buffer.  recvmsg reports actual truncation
	// through msg_flags on every supported POSIX target.
	const auto bytes_received = recvmsg(native_socket(handle), &message, 0);
	if (bytes_received < 0) {
		result.status = map_io_error(last_socket_error());
		return result;
	}
	result.bytes_received = static_cast<std::size_t>(bytes_received);
	result.truncated = (message.msg_flags & MSG_TRUNC) != 0 || result.bytes_received > output.size;
#endif
	result.source_endpoint = endpoint_from_sockaddr(source);
	result.status = IoStatus::Complete;
	return result;
}

SocketSendResult NativeUdpSocketBackend::try_send(SocketHandle handle,
	const protocol::EndpointKey& endpoint,
	protocol::ByteView bytes) noexcept
{
	SocketSendResult result;
	if (handle == InvalidSocketHandle || !endpoint.is_valid() || bytes.data == nullptr || bytes.empty() ||
		bytes.size > protocol::MaxDatagramSize) {
		return result;
	}
	const auto* record = find_socket(handle);
	if (record == nullptr ||
		(record->family == protocol::IpAddressFamily::Ipv4 &&
			endpoint.family() != protocol::IpAddressFamily::Ipv4) ||
		(record->family == protocol::IpAddressFamily::Ipv6 &&
			endpoint.family() == protocol::IpAddressFamily::Ipv4 && !record->dual_stack)) {
		return result;
	}

	sockaddr_storage destination{};
	SocketLength destination_length = 0;
	const auto address_ready = record->family == protocol::IpAddressFamily::Ipv6 && record->dual_stack &&
			endpoint.family() == protocol::IpAddressFamily::Ipv4
		? make_ipv4_mapped_sockaddr(endpoint, destination, destination_length)
		: make_sockaddr(endpoint, destination, destination_length);
	if (!address_ready) {
		return result;
	}
#if defined(_WIN32)
	const auto bytes_sent = sendto(native_socket(handle),
		reinterpret_cast<const char*>(bytes.data),
		static_cast<int>(bytes.size),
		0,
		reinterpret_cast<const sockaddr*>(&destination),
		destination_length);
	if (bytes_sent == SOCKET_ERROR) {
#else
	const auto bytes_sent = sendto(native_socket(handle),
		bytes.data,
		bytes.size,
		0,
		reinterpret_cast<const sockaddr*>(&destination),
		destination_length);
	if (bytes_sent < 0) {
#endif
		result.status = map_io_error(last_socket_error());
		return result;
	}
	result.status = IoStatus::Complete;
	result.bytes_sent = static_cast<std::size_t>(bytes_sent);
	return result;
}

void NativeUdpSocketBackend::close_socket(SocketHandle handle) noexcept
{
	if (handle != InvalidSocketHandle) {
		forget_socket(handle);
		close_native_socket(native_socket(handle));
	}
}

DedicatedUdpTransport::DedicatedUdpTransport(UdpSocketBackend& backend) noexcept : m_backend(backend) {}

DedicatedUdpTransport::~DedicatedUdpTransport() noexcept
{
	close();
}

TransportOpenStatus DedicatedUdpTransport::open(const TelemetryConfig& config) noexcept
{
	if (is_open()) {
		return TransportOpenStatus::AlreadyOpen;
	}
	if (!config.enabled) {
		return TransportOpenStatus::Disabled;
	}
	if (config.bind_addresses.empty() || config.bind_port == 0U || config.max_datagrams_per_tick == 0U ||
		config.max_datagrams_per_tick > 256U) {
		return TransportOpenStatus::InvalidConfiguration;
	}
	for (std::size_t index = 0U; index < config.bind_addresses.size(); ++index) {
		const auto& address = config.bind_addresses[index];
		if (!address.is_valid() ||
			(!address.is_loopback() && config.allowed_clients.empty()) ||
			(address.is_wildcard() && allowlist_has_catch_all(config.allowed_clients))) {
			return TransportOpenStatus::InvalidConfiguration;
		}
	}

	for (std::size_t index = 0U; index < config.bind_addresses.size(); ++index) {
		const auto& address = config.bind_addresses[index];
		const SocketOpenRequest request{address,
			config.bind_port,
			true,
			address.family() == protocol::IpAddressFamily::Ipv6 &&
				!(config.bind_addresses.size() == 1U && address.is_wildcard())};
		const auto opened = m_backend.open_socket(request);
		if (opened.status != SocketOpenStatus::Complete || opened.handle == InvalidSocketHandle ||
			!endpoint_matches_address(opened.local_endpoint, address) ||
			opened.local_endpoint.port() != config.bind_port) {
			const auto failure = opened.status == SocketOpenStatus::Complete
				? TransportOpenStatus::AddressVerificationFailed
				: map_open_status(opened.status);
			std::array<SocketHandle, MaximumBindAddresses> prior_handles{};
			prior_handles.fill(InvalidSocketHandle);
			const auto prior_count = m_socket_count;
			for (std::size_t prior_index = 0U; prior_index < prior_count; ++prior_index) {
				prior_handles[prior_index] = m_sockets[prior_index].handle;
				m_sockets[prior_index] = {};
			}
			m_socket_count = 0U;
			m_receive_cursor = 0U;
			if (opened.handle != InvalidSocketHandle) {
				m_backend.close_socket(opened.handle);
			}
			for (std::size_t prior_index = prior_count; prior_index > 0U; --prior_index) {
				if (prior_handles[prior_index - 1U] != InvalidSocketHandle) {
					m_backend.close_socket(prior_handles[prior_index - 1U]);
				}
			}
			return failure;
		}
		m_sockets[m_socket_count].handle = opened.handle;
		m_sockets[m_socket_count].local_endpoint = opened.local_endpoint;
		m_sockets[m_socket_count].dual_stack =
			request.bind_address.family() == protocol::IpAddressFamily::Ipv6 && !request.ipv6_only;
		++m_socket_count;
	}
	m_receive_cursor = 0U;
	return TransportOpenStatus::Complete;
}

TransportReceiveResult DedicatedUdpTransport::try_receive() noexcept
{
	TransportReceiveResult result;
	if (!is_open()) {
		return result;
	}

	const auto slot_index = m_receive_cursor % m_socket_count;
	m_receive_cursor = (slot_index + 1U) % m_socket_count;
	const auto received = m_backend.try_receive(m_sockets[slot_index].handle,
		{m_receive_buffer.data(), m_receive_buffer.size()});
	result.status = received.status;
	if (received.status != IoStatus::Complete) {
		return result;
	}
	if (received.truncated || received.bytes_received == 0U ||
		received.bytes_received > protocol::MaxDatagramSize || !received.source_endpoint.is_valid()) {
		result.disposition = TransportReceiveDisposition::Rejected;
		return result;
	}
	result.disposition = TransportReceiveDisposition::Datagram;
	result.datagram.endpoint = received.source_endpoint;
	result.datagram.bytes = {m_receive_buffer.data(), received.bytes_received};
	return result;
}

IoStatus DedicatedUdpTransport::try_send(const protocol::EndpointKey& endpoint,
	protocol::ByteView bytes) noexcept
{
	if (!is_open()) {
		return IoStatus::Closed;
	}
	if (!endpoint.is_valid() || bytes.data == nullptr || bytes.empty() || bytes.size > protocol::MaxDatagramSize) {
		return IoStatus::Error;
	}
	for (std::size_t index = 0U; index < m_socket_count; ++index) {
		if (m_sockets[index].local_endpoint.family() != endpoint.family() &&
			!(m_sockets[index].dual_stack && endpoint.family() == protocol::IpAddressFamily::Ipv4)) {
			continue;
		}
		std::copy_n(bytes.data, bytes.size, m_send_buffer.data());
		const auto sent = m_backend.try_send(m_sockets[index].handle,
			endpoint,
			{m_send_buffer.data(), bytes.size});
		if (sent.status == IoStatus::Complete && sent.bytes_sent != bytes.size) {
			return IoStatus::Error;
		}
		return sent.status;
	}
	return IoStatus::Error;
}

void DedicatedUdpTransport::close() noexcept
{
	std::array<SocketHandle, MaximumBindAddresses> handles{};
	handles.fill(InvalidSocketHandle);
	const auto count = m_socket_count;
	for (std::size_t index = 0U; index < count; ++index) {
		handles[index] = m_sockets[index].handle;
		m_sockets[index] = {};
	}
	m_socket_count = 0U;
	m_receive_cursor = 0U;

	for (std::size_t index = 0U; index < count; ++index) {
		if (handles[index] != InvalidSocketHandle) {
			m_backend.close_socket(handles[index]);
		}
	}
}

protocol::EndpointKey DedicatedUdpTransport::local_endpoint(protocol::IpAddressFamily family) const noexcept
{
	for (std::size_t index = 0U; index < m_socket_count; ++index) {
		if (m_sockets[index].local_endpoint.family() == family) {
			return m_sockets[index].local_endpoint;
		}
	}
	return {};
}

} // namespace telemetry::detail
