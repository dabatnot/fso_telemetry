#pragma once

#include "telemetry/config.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_session_context.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace telemetry::detail {

using SocketHandle = std::uintptr_t;
constexpr SocketHandle InvalidSocketHandle = std::numeric_limits<SocketHandle>::max();

enum class IoStatus : std::uint8_t {
	Complete = 0,
	WouldBlock,
	Closed,
	Error,
};

enum class SocketOpenStatus : std::uint8_t {
	Complete = 0,
	SocketCreationFailed,
	NonBlockingFailed,
	SocketOptionFailed,
	BindFailed,
	AddressVerificationFailed,
};

enum class TransportOpenStatus : std::uint8_t {
	Complete = 0,
	Disabled,
	InvalidConfiguration,
	AlreadyOpen,
	SocketCreationFailed,
	NonBlockingFailed,
	SocketOptionFailed,
	BindFailed,
	AddressVerificationFailed,
};

struct SocketOpenRequest {
	NumericIpAddress bind_address;
	std::uint16_t port = 0U;
	bool non_blocking = true;
	bool ipv6_only = false;
};

struct SocketOpenResult {
	SocketOpenStatus status = SocketOpenStatus::SocketCreationFailed;
	SocketHandle handle = InvalidSocketHandle;
	protocol::EndpointKey local_endpoint;
};

struct SocketReceiveResult {
	IoStatus status = IoStatus::Error;
	protocol::EndpointKey source_endpoint;
	std::size_t bytes_received = 0U;
	bool truncated = false;
};

struct SocketSendResult {
	IoStatus status = IoStatus::Error;
	std::size_t bytes_sent = 0U;
};

class UdpSocketBackend {
  public:
	virtual ~UdpSocketBackend() noexcept = default;
	virtual SocketOpenResult open_socket(const SocketOpenRequest& request) noexcept = 0;
	virtual SocketReceiveResult try_receive(SocketHandle handle, protocol::MutableByteView output) noexcept = 0;
	virtual SocketSendResult try_send(SocketHandle handle,
		const protocol::EndpointKey& endpoint,
		protocol::ByteView bytes) noexcept = 0;
	virtual void close_socket(SocketHandle handle) noexcept = 0;
};

class NativeUdpSocketBackend final : public UdpSocketBackend {
  public:
	NativeUdpSocketBackend() noexcept = default;
	NativeUdpSocketBackend(const NativeUdpSocketBackend&) = delete;
	NativeUdpSocketBackend& operator=(const NativeUdpSocketBackend&) = delete;
	NativeUdpSocketBackend(NativeUdpSocketBackend&&) = delete;
	NativeUdpSocketBackend& operator=(NativeUdpSocketBackend&&) = delete;

	SocketOpenResult open_socket(const SocketOpenRequest& request) noexcept override;
	SocketReceiveResult try_receive(SocketHandle handle, protocol::MutableByteView output) noexcept override;
	SocketSendResult try_send(SocketHandle handle,
		const protocol::EndpointKey& endpoint,
		protocol::ByteView bytes) noexcept override;
	void close_socket(SocketHandle handle) noexcept override;

  private:
	struct SocketRecord {
		SocketHandle handle = InvalidSocketHandle;
		protocol::IpAddressFamily family = protocol::IpAddressFamily::Invalid;
		bool dual_stack = false;
	};

	static constexpr std::size_t MaximumTrackedSockets = 16U;
	bool remember_socket(SocketHandle handle, protocol::IpAddressFamily family, bool dual_stack) noexcept;
	const SocketRecord* find_socket(SocketHandle handle) const noexcept;
	void forget_socket(SocketHandle handle) noexcept;

	std::array<SocketRecord, MaximumTrackedSockets> m_socket_records{};
};

struct ReceivedDatagram {
	protocol::EndpointKey endpoint;
	protocol::ByteView bytes;
};

enum class TransportReceiveDisposition : std::uint8_t {
	None = 0,
	Datagram,
	Rejected,
};

struct TransportReceiveResult {
	IoStatus status = IoStatus::Closed;
	TransportReceiveDisposition disposition = TransportReceiveDisposition::None;
	ReceivedDatagram datagram;
};

class DedicatedUdpTransport final {
  public:
	explicit DedicatedUdpTransport(UdpSocketBackend& backend) noexcept;
	~DedicatedUdpTransport() noexcept;

	DedicatedUdpTransport(const DedicatedUdpTransport&) = delete;
	DedicatedUdpTransport& operator=(const DedicatedUdpTransport&) = delete;

	TransportOpenStatus open(const TelemetryConfig& config) noexcept;
	TransportReceiveResult try_receive() noexcept;
	IoStatus try_send(const protocol::EndpointKey& endpoint, protocol::ByteView bytes) noexcept;
	void close() noexcept;

	bool is_open() const noexcept
	{
		return m_socket_count != 0U;
	}
	std::size_t socket_count() const noexcept
	{
		return m_socket_count;
	}
	protocol::EndpointKey local_endpoint(protocol::IpAddressFamily family) const noexcept;

  private:
	struct SocketSlot {
		SocketHandle handle = InvalidSocketHandle;
		protocol::EndpointKey local_endpoint;
		bool dual_stack = false;
	};

	UdpSocketBackend& m_backend;
	std::array<SocketSlot, MaximumBindAddresses> m_sockets{};
	std::array<std::uint8_t, protocol::MaxDatagramSize> m_receive_buffer{};
	std::array<std::uint8_t, protocol::MaxDatagramSize> m_send_buffer{};
	std::size_t m_socket_count = 0U;
	std::size_t m_receive_cursor = 0U;
};

} // namespace telemetry::detail
