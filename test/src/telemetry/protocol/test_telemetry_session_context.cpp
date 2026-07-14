#include "telemetry/protocol/telemetry_session_context.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using namespace telemetry::protocol;

EndpointKey ipv4(std::uint8_t last_octet = 1U, std::uint16_t port = 7808U)
{
	return EndpointKey::from_ipv4({{127U, 0U, 0U, last_octet}}, port);
}

TelemetryDatagramHeader header(MessageType type, std::uint64_t session_id = 42U)
{
	TelemetryDatagramHeader result;
	result.message_type = type;
	result.session_id = session_id;
	return result;
}

TEST(TelemetryProtocolSessionContext, EndpointKeysCanonicalizeIpv4MappedIpv6AndRequireAPort)
{
	const EndpointKey invalid;
	EXPECT_FALSE(invalid.is_valid());
	const auto v4 = EndpointKey::from_ipv4({{192U, 0U, 2U, 44U}}, 7808U);
	EXPECT_TRUE(v4.is_valid());
	EXPECT_EQ(IpAddressFamily::Ipv4, v4.family());
	EXPECT_EQ(7808U, v4.port());
	for (std::size_t index = 4U; index < v4.address().size(); ++index) {
		EXPECT_EQ(0U, v4.address()[index]);
	}
	EXPECT_FALSE(EndpointKey::from_ipv4({{192U, 0U, 2U, 44U}}, 0U).is_valid());

	std::array<std::uint8_t, 16> mapped{};
	mapped[10U] = 0xffU;
	mapped[11U] = 0xffU;
	mapped[12U] = 192U;
	mapped[13U] = 0U;
	mapped[14U] = 2U;
	mapped[15U] = 44U;
	const auto mapped_key = EndpointKey::from_ipv6(mapped, 7808U);
	EXPECT_EQ(v4, mapped_key);
	EXPECT_EQ(IpAddressFamily::Ipv4, mapped_key.family());

	std::array<std::uint8_t, 16> v6_address{};
	v6_address[0U] = 0x20U;
	v6_address[1U] = 0x01U;
	v6_address[15U] = 1U;
	const auto v6 = EndpointKey::from_ipv6(v6_address, 7808U);
	EXPECT_TRUE(v6.is_valid());
	EXPECT_EQ(IpAddressFamily::Ipv6, v6.family());
	EXPECT_NE(v4, v6);
	EXPECT_NE(v4, EndpointKey::from_ipv4({{192U, 0U, 2U, 44U}}, 7809U));
}

TEST(TelemetryProtocolSessionContext, FreezesDirectionRegistryAndRoleMatrix)
{
	const std::array<MessageType, 10> producer_to_client{{
		MessageType::Discovery,
		MessageType::Welcome,
		MessageType::SessionBegin,
		MessageType::Manifest,
		MessageType::FullSnapshot,
		MessageType::Delta,
		MessageType::EventBatch,
		MessageType::SessionEnd,
		MessageType::TargetVideoConfig,
		MessageType::TargetVideoFrame,
	}};
	for (const auto type : producer_to_client) {
		EXPECT_EQ(MessageDirection::ProducerToClient, message_direction(type));
		EXPECT_TRUE(role_can_receive(LocalEndpointRole::Client, type));
		EXPECT_FALSE(role_can_receive(LocalEndpointRole::Producer, type));
		EXPECT_TRUE(role_can_send(LocalEndpointRole::Producer, type));
		EXPECT_FALSE(role_can_send(LocalEndpointRole::Client, type));
	}

	const std::array<MessageType, 5> client_to_producer{{
		MessageType::Hello,
		MessageType::ResyncRequest,
		MessageType::TargetVideoSubscribe,
		MessageType::TargetVideoKeyframeRequest,
		MessageType::TargetVideoStats,
	}};
	for (const auto type : client_to_producer) {
		EXPECT_EQ(MessageDirection::ClientToProducer, message_direction(type));
		EXPECT_TRUE(role_can_receive(LocalEndpointRole::Producer, type));
		EXPECT_FALSE(role_can_receive(LocalEndpointRole::Client, type));
		EXPECT_TRUE(role_can_send(LocalEndpointRole::Client, type));
		EXPECT_FALSE(role_can_send(LocalEndpointRole::Producer, type));
	}

	const std::array<MessageType, 5> bidirectional{{
		MessageType::Heartbeat,
		MessageType::Ack,
		MessageType::Nack,
		MessageType::TargetVideoStop,
		MessageType::CapabilityUpdate,
	}};
	for (const auto type : bidirectional) {
		EXPECT_EQ(MessageDirection::Bidirectional, message_direction(type));
		EXPECT_TRUE(role_can_receive(LocalEndpointRole::Client, type));
		EXPECT_TRUE(role_can_receive(LocalEndpointRole::Producer, type));
		EXPECT_TRUE(role_can_send(LocalEndpointRole::Client, type));
		EXPECT_TRUE(role_can_send(LocalEndpointRole::Producer, type));
	}

	EXPECT_EQ(MessageDirection::Invalid, message_direction(MessageType::Invalid));
	EXPECT_EQ(MessageDirection::Invalid, message_direction(static_cast<MessageType>(0xffU)));
	EXPECT_FALSE(role_can_receive(LocalEndpointRole::Invalid, MessageType::Heartbeat));
	EXPECT_FALSE(role_can_send(LocalEndpointRole::Invalid, MessageType::Heartbeat));
}

TEST(TelemetryProtocolSessionContext, ContextValidationRejectsSessionThenDirectionThenEndpointThenType)
{
	TelemetrySessionContext context;
	context.local_role = LocalEndpointRole::Client;
	context.peer_endpoint = ipv4();
	context.active_session_id = 42U;
	const auto source = ipv4();

	auto datagram = header(MessageType::Heartbeat);
	EXPECT_EQ(ValidationError::None, validate_received_datagram_context(datagram, source, context));

	datagram.session_id = 41U;
	EXPECT_EQ(ValidationError::SessionMismatch, validate_received_datagram_context(datagram, ipv4(2U), context));
	datagram = header(MessageType::Hello, 0U);
	EXPECT_EQ(ValidationError::WrongDirection, validate_received_datagram_context(datagram, ipv4(2U), context));
	datagram = header(MessageType::Heartbeat);
	EXPECT_EQ(ValidationError::EndpointMismatch, validate_received_datagram_context(datagram, ipv4(2U), context));

	datagram = header(static_cast<MessageType>(0xffU), 41U);
	EXPECT_EQ(ValidationError::SessionMismatch, validate_received_datagram_context(datagram, ipv4(2U), context));
	datagram.session_id = 42U;
	EXPECT_EQ(ValidationError::EndpointMismatch, validate_received_datagram_context(datagram, ipv4(2U), context));
	EXPECT_EQ(ValidationError::UnknownMessageType, validate_received_datagram_context(datagram, source, context));
}

TEST(TelemetryProtocolSessionContext, PreSessionAndActiveMessagesEnforceSessionAndCaptureFields)
{
	TelemetrySessionContext producer_context;
	producer_context.local_role = LocalEndpointRole::Producer;
	producer_context.peer_endpoint = ipv4();
	const auto source = ipv4();

	auto datagram = header(MessageType::Hello, 0U);
	EXPECT_EQ(ValidationError::None, validate_received_datagram_context(datagram, source, producer_context));
	datagram.session_id = 1U;
	EXPECT_EQ(ValidationError::SessionMismatch, validate_received_datagram_context(datagram, source, producer_context));

	datagram = header(MessageType::Heartbeat, 0U);
	EXPECT_EQ(ValidationError::SessionMismatch, validate_received_datagram_context(datagram, source, producer_context));
	producer_context.active_session_id = 42U;
	datagram.session_id = 42U;
	EXPECT_EQ(ValidationError::None, validate_received_datagram_context(datagram, source, producer_context));
	datagram.frame_id = 1U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_received_datagram_context(datagram, source, producer_context));
	datagram.frame_id = 0U;
	datagram.mission_time_us = -1;
	EXPECT_EQ(ValidationError::OutOfRange, validate_received_datagram_context(datagram, source, producer_context));

	TelemetrySessionContext client_context;
	client_context.local_role = LocalEndpointRole::Client;
	client_context.peer_endpoint = source;
	client_context.active_session_id = 42U;
	datagram = header(MessageType::FullSnapshot, 42U);
	EXPECT_EQ(ValidationError::OutOfRange, validate_received_datagram_context(datagram, source, client_context));
	datagram.frame_id = 1U;
	datagram.mission_time_us = -500;
	EXPECT_EQ(ValidationError::None, validate_received_datagram_context(datagram, source, client_context));
}

TEST(TelemetryProtocolSessionContext, WelcomeStatusControlsSessionAndAckRequiredFlags)
{
	TelemetrySessionContext context;
	context.local_role = LocalEndpointRole::Client;
	context.peer_endpoint = ipv4();

	auto welcome = header(MessageType::Welcome, 42U);
	welcome.flags = MessageFlagAckRequired;
	EXPECT_EQ(ValidationError::None, validate_welcome_logical_context(welcome, WelcomeStatus::Accepted, context));
	welcome.flags = MessageFlagAckRequired | MessageFlagRetransmission;
	EXPECT_EQ(ValidationError::None, validate_welcome_logical_context(welcome, WelcomeStatus::Accepted, context));
	welcome.flags = MessageFlagNone;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag,
		validate_welcome_logical_context(welcome, WelcomeStatus::Accepted, context));
	welcome.session_id = 0U;
	welcome.flags = MessageFlagAckRequired;
	EXPECT_EQ(ValidationError::SessionMismatch,
		validate_welcome_logical_context(welcome, WelcomeStatus::Accepted, context));

	context.active_session_id = 42U;
	welcome.session_id = 43U;
	EXPECT_EQ(ValidationError::SessionMismatch,
		validate_welcome_logical_context(welcome, WelcomeStatus::Accepted, context));
	welcome.session_id = 0U;
	welcome.flags = MessageFlagNone;
	EXPECT_EQ(ValidationError::None, validate_welcome_logical_context(welcome, WelcomeStatus::Busy, context));
	welcome.flags = MessageFlagRetransmission;
	EXPECT_EQ(ValidationError::None, validate_welcome_logical_context(welcome, WelcomeStatus::Unauthorized, context));
	welcome.flags = MessageFlagAckRequired;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag,
		validate_welcome_logical_context(welcome, WelcomeStatus::InvalidCapabilities, context));
	welcome.session_id = 1U;
	EXPECT_EQ(ValidationError::SessionMismatch,
		validate_welcome_logical_context(welcome, WelcomeStatus::Busy, context));
	welcome.session_id = 0U;
	EXPECT_EQ(ValidationError::UnknownEnum,
		validate_welcome_logical_context(welcome, static_cast<WelcomeStatus>(0xffU), context));
	welcome.message_type = MessageType::Heartbeat;
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_welcome_logical_context(welcome, WelcomeStatus::Busy, context));
}

TEST(TelemetryProtocolSessionContext, EventDeliveryClassControlsAckAndRetransmissionFlags)
{
	auto event = header(MessageType::EventBatch);
	event.flags = MessageFlagNone;
	EXPECT_EQ(ValidationError::None, validate_event_batch_logical_context(event, EventDeliveryClass::Replaceable));
	event.flags = MessageFlagFragmented;
	EXPECT_EQ(ValidationError::None, validate_event_batch_logical_context(event, EventDeliveryClass::Replaceable));
	event.flags = MessageFlagAckRequired;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag,
		validate_event_batch_logical_context(event, EventDeliveryClass::Replaceable));

	event.flags = MessageFlagAckRequired;
	EXPECT_EQ(ValidationError::None, validate_event_batch_logical_context(event, EventDeliveryClass::Reliable));
	event.flags = MessageFlagFragmented | MessageFlagAckRequired | MessageFlagRetransmission;
	EXPECT_EQ(ValidationError::None, validate_event_batch_logical_context(event, EventDeliveryClass::Reliable));
	event.flags = MessageFlagRetransmission;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag,
		validate_event_batch_logical_context(event, EventDeliveryClass::Reliable));
	EXPECT_EQ(ValidationError::UnknownEnum,
		validate_event_batch_logical_context(event, static_cast<EventDeliveryClass>(0xffU)));
	event.message_type = MessageType::Delta;
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_event_batch_logical_context(event, EventDeliveryClass::Reliable));
}

TEST(TelemetryProtocolSessionContext, TargetVideoIdrPayloadAndHeaderFlagMustAgree)
{
	auto frame = header(MessageType::TargetVideoFrame);
	frame.flags = MessageFlagVideoIdr;
	EXPECT_EQ(ValidationError::None, validate_target_video_frame_logical_context(frame, true));
	frame.flags = MessageFlagFragmented | MessageFlagVideoIdr | MessageFlagRetransmission;
	EXPECT_EQ(ValidationError::None, validate_target_video_frame_logical_context(frame, true));
	frame.flags = MessageFlagNone;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_target_video_frame_logical_context(frame, true));

	EXPECT_EQ(ValidationError::None, validate_target_video_frame_logical_context(frame, false));
	frame.flags = MessageFlagFragmented;
	EXPECT_EQ(ValidationError::None, validate_target_video_frame_logical_context(frame, false));
	frame.flags = MessageFlagVideoIdr;
	EXPECT_EQ(ValidationError::ReservedHeaderFlag, validate_target_video_frame_logical_context(frame, false));
	frame.message_type = MessageType::Heartbeat;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_target_video_frame_logical_context(frame, false));
}

} // namespace
