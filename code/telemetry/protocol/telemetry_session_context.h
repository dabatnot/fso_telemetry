#pragma once

#include "telemetry/protocol/telemetry_datagram.h"

#include <array>
#include <cstdint>

namespace telemetry::protocol {

struct WelcomePayload;

enum class IpAddressFamily : std::uint8_t {
	Invalid = 0,
	Ipv4 = 4,
	Ipv6 = 6,
};

// Canonical, allocation-free endpoint identity. IPv4 addresses occupy bytes
// 0..3 and have a zero tail. IPv4-mapped IPv6 input is normalized to IPv4 so
// the same peer cannot acquire two keys through a dual-stack socket.
class EndpointKey {
  public:
	EndpointKey() noexcept = default;

	static EndpointKey from_ipv4(const std::array<std::uint8_t, 4>& address, std::uint16_t port) noexcept;
	static EndpointKey from_ipv6(const std::array<std::uint8_t, 16>& address, std::uint16_t port) noexcept;

	IpAddressFamily family() const noexcept
	{
		return m_family;
	}
	const std::array<std::uint8_t, 16>& address() const noexcept
	{
		return m_address;
	}
	std::uint16_t port() const noexcept
	{
		return m_port;
	}
	bool is_valid() const noexcept;

	friend bool operator==(const EndpointKey& lhs, const EndpointKey& rhs) noexcept;
	friend bool operator!=(const EndpointKey& lhs, const EndpointKey& rhs) noexcept
	{
		return !(lhs == rhs);
	}

  private:
	IpAddressFamily m_family = IpAddressFamily::Invalid;
	std::array<std::uint8_t, 16> m_address{};
	std::uint16_t m_port = 0;
};

enum class LocalEndpointRole : std::uint8_t {
	Invalid = 0,
	Client = 1,
	Producer = 2,
};

enum class MessageDirection : std::uint8_t {
	Invalid = 0,
	ClientToProducer = 1,
	ProducerToClient = 2,
	Bidirectional = 3,
};

MessageDirection message_direction(MessageType type) noexcept;
bool role_can_receive(LocalEndpointRole local_role, MessageType type) noexcept;
bool role_can_send(LocalEndpointRole local_role, MessageType type) noexcept;

// One peer/session binding. active_session_id is zero while validating a
// pre-session channel. The peer endpoint is exact in both phases; allowlist
// and bind policy are deliberately checked by the socket layer first.
struct TelemetrySessionContext {
	LocalEndpointRole local_role = LocalEndpointRole::Invalid;
	EndpointKey peer_endpoint;
	std::uint64_t active_session_id = 0;
	// A live session uses an exact one-element range. While a client is waiting
	// for WELCOME, the range may additionally include minor 0 so a rejection
	// header remains decodable before its payload status is known.
	ProtocolMinorRange accepted_minors = FrozenV1_0MinorRange;
};

// Contextual validation is the stage between
// decode_and_validate_datagram_envelope() and validate_fragment_layout(). It
// performs no allocation and does not retain the datagram.
ValidationError validate_received_datagram_context(const TelemetryDatagramHeader& header,
	const EndpointKey& source_endpoint,
	const TelemetrySessionContext& context) noexcept;

// These relations require fields from the reassembled logical payload. They
// intentionally remain separate from the static contextual pass above.
ValidationError validate_welcome_logical_context(const TelemetryDatagramHeader& header,
	WelcomeStatus status,
	const TelemetrySessionContext& context) noexcept;
ValidationError validate_welcome_logical_context(const TelemetryDatagramHeader& header,
	const WelcomePayload& payload,
	const TelemetrySessionContext& context) noexcept;
ValidationError validate_event_batch_logical_context(const TelemetryDatagramHeader& header,
	EventDeliveryClass delivery_class) noexcept;
ValidationError validate_target_video_frame_logical_context(const TelemetryDatagramHeader& header,
	bool payload_is_idr) noexcept;

} // namespace telemetry::protocol
