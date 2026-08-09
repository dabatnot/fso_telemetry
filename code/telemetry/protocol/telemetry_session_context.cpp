#include "telemetry/protocol/telemetry_session_context.h"

#include "telemetry/protocol/telemetry_control_messages.h"

#include <algorithm>

namespace telemetry::protocol {

namespace {

constexpr bool is_ipv4_mapped(const std::array<std::uint8_t, 16>& address) noexcept
{
	for (std::size_t i = 0; i < 10; ++i) {
		if (address[i] != 0) {
			return false;
		}
	}
	return address[10] == 0xff && address[11] == 0xff;
}

ValidationError validate_static_capture_fields(const TelemetryDatagramHeader& header) noexcept
{
	switch (header.message_type) {
	case MessageType::FullSnapshot:
	case MessageType::Delta:
	case MessageType::EventBatch:
		return header.frame_id != 0 ? ValidationError::None : ValidationError::OutOfRange;
	case MessageType::Discovery:
	case MessageType::Hello:
	case MessageType::Welcome:
	case MessageType::SessionBegin:
	case MessageType::Manifest:
	case MessageType::Heartbeat:
	case MessageType::Ack:
	case MessageType::Nack:
	case MessageType::ResyncRequest:
	case MessageType::SessionEnd:
	case MessageType::TargetVideoSubscribe:
	case MessageType::TargetVideoConfig:
	case MessageType::TargetVideoFrame:
	case MessageType::TargetVideoKeyframeRequest:
	case MessageType::TargetVideoStop:
	case MessageType::TargetVideoStats:
	case MessageType::CapabilityUpdate:
		return header.frame_id == 0 && header.mission_time_us == 0 ? ValidationError::None
																   : ValidationError::OutOfRange;
	default:
		return ValidationError::UnknownMessageType;
	}
}

ValidationError validate_static_session(const TelemetryDatagramHeader& header,
	const TelemetrySessionContext& context) noexcept
{
	switch (header.message_type) {
	case MessageType::Discovery:
	case MessageType::Hello:
		return header.session_id == 0 ? ValidationError::None : ValidationError::SessionMismatch;
	case MessageType::Welcome:
		// Accepted versus rejected is encoded in the logical payload, so zero
		// versus non-zero is checked only by validate_welcome_logical_context().
		return ValidationError::None;
	case MessageType::SessionBegin:
	case MessageType::Manifest:
	case MessageType::FullSnapshot:
	case MessageType::Delta:
	case MessageType::EventBatch:
	case MessageType::Heartbeat:
	case MessageType::Ack:
	case MessageType::Nack:
	case MessageType::ResyncRequest:
	case MessageType::SessionEnd:
	case MessageType::TargetVideoSubscribe:
	case MessageType::TargetVideoConfig:
	case MessageType::TargetVideoFrame:
	case MessageType::TargetVideoKeyframeRequest:
	case MessageType::TargetVideoStop:
	case MessageType::TargetVideoStats:
	case MessageType::CapabilityUpdate:
		return context.active_session_id != 0 && header.session_id == context.active_session_id
				   ? ValidationError::None
				   : ValidationError::SessionMismatch;
	default:
		return ValidationError::UnknownMessageType;
	}
}

ValidationError validate_payload_dependent_flags(const TelemetryDatagramHeader& header,
	std::uint8_t required,
	std::uint8_t allowed) noexcept
{
	if ((header.flags & required) != required || (header.flags & ~allowed) != 0) {
		return ValidationError::ReservedHeaderFlag;
	}
	return ValidationError::None;
}

} // namespace

EndpointKey EndpointKey::from_ipv4(const std::array<std::uint8_t, 4>& address, std::uint16_t port) noexcept
{
	EndpointKey endpoint;
	endpoint.m_family = IpAddressFamily::Ipv4;
	std::copy(address.begin(), address.end(), endpoint.m_address.begin());
	endpoint.m_port = port;
	return endpoint;
}

EndpointKey EndpointKey::from_ipv6(const std::array<std::uint8_t, 16>& address, std::uint16_t port) noexcept
{
	if (is_ipv4_mapped(address)) {
		std::array<std::uint8_t, 4> ipv4{};
		std::copy(address.begin() + 12, address.end(), ipv4.begin());
		return from_ipv4(ipv4, port);
	}

	EndpointKey endpoint;
	endpoint.m_family = IpAddressFamily::Ipv6;
	endpoint.m_address = address;
	endpoint.m_port = port;
	return endpoint;
}

bool EndpointKey::is_valid() const noexcept
{
	if (m_port == 0) {
		return false;
	}
	if (m_family == IpAddressFamily::Ipv4) {
		return std::all_of(m_address.begin() + 4, m_address.end(), [](std::uint8_t byte) { return byte == 0; });
	}
	return m_family == IpAddressFamily::Ipv6 && !is_ipv4_mapped(m_address);
}

bool operator==(const EndpointKey& lhs, const EndpointKey& rhs) noexcept
{
	return lhs.m_family == rhs.m_family && lhs.m_port == rhs.m_port && lhs.m_address == rhs.m_address;
}

MessageDirection message_direction(MessageType type) noexcept
{
	switch (type) {
	case MessageType::Discovery:
	case MessageType::Welcome:
	case MessageType::SessionBegin:
	case MessageType::Manifest:
	case MessageType::FullSnapshot:
	case MessageType::Delta:
	case MessageType::EventBatch:
	case MessageType::SessionEnd:
	case MessageType::TargetVideoConfig:
	case MessageType::TargetVideoFrame:
		return MessageDirection::ProducerToClient;
	case MessageType::Hello:
	case MessageType::ResyncRequest:
	case MessageType::TargetVideoSubscribe:
	case MessageType::TargetVideoKeyframeRequest:
	case MessageType::TargetVideoStats:
		return MessageDirection::ClientToProducer;
	case MessageType::Heartbeat:
	case MessageType::Ack:
	case MessageType::Nack:
	case MessageType::TargetVideoStop:
	case MessageType::CapabilityUpdate:
		return MessageDirection::Bidirectional;
	default:
		return MessageDirection::Invalid;
	}
}

bool role_can_receive(LocalEndpointRole local_role, MessageType type) noexcept
{
	const auto direction = message_direction(type);
	if (direction == MessageDirection::Bidirectional) {
		return local_role == LocalEndpointRole::Client || local_role == LocalEndpointRole::Producer;
	}
	return (local_role == LocalEndpointRole::Client && direction == MessageDirection::ProducerToClient) ||
		   (local_role == LocalEndpointRole::Producer && direction == MessageDirection::ClientToProducer);
}

bool role_can_send(LocalEndpointRole local_role, MessageType type) noexcept
{
	const auto direction = message_direction(type);
	if (direction == MessageDirection::Bidirectional) {
		return local_role == LocalEndpointRole::Client || local_role == LocalEndpointRole::Producer;
	}
	return (local_role == LocalEndpointRole::Client && direction == MessageDirection::ClientToProducer) ||
		   (local_role == LocalEndpointRole::Producer && direction == MessageDirection::ProducerToClient);
}

ValidationError validate_received_datagram_context(const TelemetryDatagramHeader& header,
	const EndpointKey& source_endpoint,
	const TelemetrySessionContext& context) noexcept
{
	if (const auto error = validate_datagram_header_version(header, context.accepted_minors);
		error != ValidationError::None) {
		return error;
	}
	if ((header.message_type == MessageType::Discovery || header.message_type == MessageType::Hello) &&
		header.version_minor != VersionMinorV1_0) {
		return ValidationError::UnsupportedMinor;
	}
	if (context.active_session_id != 0 && context.accepted_minors.minimum != context.accepted_minors.maximum) {
		return ValidationError::InvalidStateTransition;
	}
	const auto direction = message_direction(header.message_type);
	if (direction == MessageDirection::Invalid) {
		// The type is reported only after the session and endpoint are known,
		// which keeps UnsupportedMessage responses bound to an active peer.
		const auto expected_session = context.active_session_id;
		if (header.session_id != expected_session) {
			return ValidationError::SessionMismatch;
		}
	} else if (const auto error = validate_static_session(header, context); error != ValidationError::None) {
		return error;
	}
	if (direction != MessageDirection::Invalid && !role_can_receive(context.local_role, header.message_type)) {
		return ValidationError::WrongDirection;
	}
	if (!context.peer_endpoint.is_valid() || !source_endpoint.is_valid() || source_endpoint != context.peer_endpoint) {
		return ValidationError::EndpointMismatch;
	}
	if (direction == MessageDirection::Invalid) {
		return ValidationError::UnknownMessageType;
	}
	return validate_static_capture_fields(header);
}

ValidationError validate_welcome_logical_context(const TelemetryDatagramHeader& header,
	WelcomeStatus status,
	const TelemetrySessionContext& context) noexcept
{
	if (header.message_type != MessageType::Welcome) {
		return ValidationError::InvalidStateTransition;
	}

	const auto retransmission = static_cast<std::uint8_t>(MessageFlagRetransmission);
	const auto ack_required = static_cast<std::uint8_t>(MessageFlagAckRequired);
	if (status == WelcomeStatus::Accepted) {
		if (const auto error = validate_datagram_header_version(header, context.accepted_minors);
			error != ValidationError::None) {
			return error;
		}
		if (header.session_id == 0 ||
			(context.active_session_id != 0 && header.session_id != context.active_session_id)) {
			return ValidationError::SessionMismatch;
		}
		return validate_payload_dependent_flags(header,
			ack_required,
			static_cast<std::uint8_t>(ack_required | retransmission));
	}
	switch (status) {
	case WelcomeStatus::UnsupportedVersion:
	case WelcomeStatus::Unauthorized:
	case WelcomeStatus::Busy:
	case WelcomeStatus::InvalidCapabilities:
		break;
	default:
		return ValidationError::UnknownEnum;
	}
	if (const auto error = validate_datagram_header_version(header, FrozenV1_0MinorRange);
		error != ValidationError::None) {
		return error;
	}
	if (header.session_id != 0) {
		return ValidationError::SessionMismatch;
	}
	return validate_payload_dependent_flags(header, 0, retransmission);
}

ValidationError validate_welcome_logical_context(const TelemetryDatagramHeader& header,
	const WelcomePayload& payload,
	const TelemetrySessionContext& context) noexcept
{
	if (const auto error = validate_welcome_logical_context(header, payload.status, context);
		error != ValidationError::None) {
		return error;
	}
	if (payload.status != WelcomeStatus::Accepted) {
		return ValidationError::None;
	}
	if (header.version_major != payload.selected_major) {
		return ValidationError::UnsupportedMajor;
	}
	return header.version_minor == payload.selected_minor ? ValidationError::None
											  : ValidationError::UnsupportedMinor;
}

ValidationError validate_event_batch_logical_context(const TelemetryDatagramHeader& header,
	EventDeliveryClass delivery_class) noexcept
{
	if (header.message_type != MessageType::EventBatch) {
		return ValidationError::InvalidStateTransition;
	}

	const auto fragmented = static_cast<std::uint8_t>(MessageFlagFragmented);
	const auto ack_required = static_cast<std::uint8_t>(MessageFlagAckRequired);
	const auto retransmission = static_cast<std::uint8_t>(MessageFlagRetransmission);
	switch (delivery_class) {
	case EventDeliveryClass::Replaceable:
		return validate_payload_dependent_flags(header, 0, fragmented);
	case EventDeliveryClass::Reliable:
		return validate_payload_dependent_flags(header,
			ack_required,
			static_cast<std::uint8_t>(fragmented | ack_required | retransmission));
	default:
		return ValidationError::UnknownEnum;
	}
}

ValidationError validate_target_video_frame_logical_context(const TelemetryDatagramHeader& header,
	bool payload_is_idr) noexcept
{
	if (header.message_type != MessageType::TargetVideoFrame) {
		return ValidationError::InvalidStateTransition;
	}

	const auto fragmented = static_cast<std::uint8_t>(MessageFlagFragmented);
	const auto video_idr = static_cast<std::uint8_t>(MessageFlagVideoIdr);
	const auto retransmission = static_cast<std::uint8_t>(MessageFlagRetransmission);
	if (payload_is_idr) {
		return validate_payload_dependent_flags(header,
			video_idr,
			static_cast<std::uint8_t>(fragmented | video_idr | retransmission));
	}
	return validate_payload_dependent_flags(header, 0, fragmented);
}

} // namespace telemetry::protocol
