#include "telemetry/entity_id_registry.h"
#include "telemetry/session_controller.h"

#include "telemetry_session_controller_player_test_access.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace telemetry::detail {
struct SessionPlayerMaterializationResult;
}

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

constexpr const char* MissingD2 =
	"WP07-D2 requires SessionController per-slot player observation ownership and materialization.";

template <typename Controller, typename = void>
struct has_d2_api : std::false_type {};

template <typename Controller>
struct has_d2_api<Controller,
	std::void_t<decltype(std::declval<Controller&>().apply_player_observation(
		std::declval<const detail::CaptureResult&>(),
		std::declval<const detail::PlayerObservationDto&>())),
		decltype(std::declval<Controller&>().clear_player_observations()),
		decltype(std::declval<const Controller&>().slot(std::size_t{}).player_entity_ids),
		decltype(std::declval<const Controller&>().slot(std::size_t{}).latest_player_sample),
		decltype(std::declval<const Controller&>().slot(std::size_t{}).latest_player_sample_status),
		decltype(std::declval<const Controller&>().slot(std::size_t{}).has_latest_player_sample)>> : std::true_type {};

template <typename Access, typename Controller, typename = void>
struct has_d2_seed_access : std::false_type {};

template <typename Access, typename Controller>
struct has_d2_seed_access<Access,
	Controller,
	std::void_t<decltype(Access::seed_last_allocated_entity_id(std::declval<Controller&>(),
		std::declval<std::size_t>(),
		std::declval<std::uint64_t>()))>> : std::true_type {};

template <typename Container>
protocol::ByteView bytes(const Container& value, std::size_t size)
{
	return {reinterpret_cast<const std::uint8_t*>(value.data()), size};
}

template <typename Container>
protocol::MutableByteView mutable_bytes(Container& value)
{
	return {reinterpret_cast<std::uint8_t*>(value.data()), value.size()};
}

struct Packet {
	std::array<std::uint8_t, protocol::MaxDatagramSize> bytes{};
	std::size_t size = 0U;
};

struct FixedRandom final : detail::RandomSource {
	std::array<std::uint64_t, 32U> values{};
	std::size_t next = 0U;
	bool next_u64(std::uint64_t& output) noexcept override
	{
		if (next >= values.size()) return false;
		output = values[next++];
		return true;
	}
};

struct Harness {
	explicit Harness(std::size_t clients = 4U) : max_clients(clients), ids(session_random, registry)
	{
		for (std::size_t i = 0U; i < session_random.values.size(); ++i) {
			session_random.values[i] = 0x1000U + i;
			packet_random.values[i] = 0x10203040U + i;
		}
		EXPECT_TRUE(registry.allocate_storage());
		detail::SessionControllerConfig config;
		config.max_clients = clients;
		config.producer_id = 0x1020304050607080ULL;
		config.mission_heartbeat_ms = 500U;
		config.idle_heartbeat_ms = 1000U;
		config.security.enabled = true;
		config.security.port = 42042U;
		config.security.bind_mode = protocol::NetworkBindMode::LoopbackOnly;
		config.security.resources.max_clients = clients;
		config.security.resources.global_state_reassembly_bytes =
			clients * protocol::MaxStateReassemblyBytesPerClient;
		EXPECT_EQ(detail::SessionControllerConfigureResult::Ready,
			detail::SessionController::configure(config, ids, packet_random, 0U, nullptr, controller));
	}

	FixedRandom session_random;
	FixedRandom packet_random;
	std::size_t max_clients = 0U;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator ids;
	detail::SessionController controller;
};

protocol::EndpointKey endpoint(std::uint8_t suffix)
{
	return protocol::EndpointKey::from_ipv4({127U, 0U, 0U, suffix}, static_cast<std::uint16_t>(42042U + suffix));
}

Packet encode(protocol::TelemetryDatagramHeader header, protocol::ByteView payload)
{
	Packet packet;
	header.message_size = static_cast<std::uint32_t>(payload.size);
	header.message_crc32 = protocol::crc32_iso_hdlc(payload);
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{header.version_minor, header.version_minor},
			payload,
			mutable_bytes(packet.bytes),
			packet.size));
	return packet;
}

Packet hello(std::uint64_t nonce, std::uint32_t sequence)
{
	protocol::HelloPayload value;
	value.client_nonce = nonce;
	value.client_send_t0_us = 100U + sequence;
	value.min_major = protocol::VersionMajor;
	value.max_major = protocol::VersionMajor;
	value.min_minor = protocol::VersionMinorV1_1;
	value.max_minor = protocol::VersionMinorV1_1;
	value.requested_visibility_mode = protocol::VisibilityMode::Cockpit;
	value.requested_heartbeat_ms = 500U;
	std::array<std::uint8_t, protocol::HelloPayloadPrefixSize> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_hello_payload(value, mutable_bytes(payload), written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Hello;
	header.packet_sequence = sequence;
	header.sent_time_us = value.client_send_t0_us;
	header.message_id = sequence;
	return encode(header, bytes(payload, written));
}

protocol::DatagramView decode(const detail::SessionControllerOutput& output)
{
	protocol::DatagramView decoded;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram({output.bytes.data(), output.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			decoded));
	return decoded;
}

Packet applied_ack(const protocol::DatagramView& target, std::uint32_t sequence)
{
	protocol::AckPayload value;
	value.target_message_id = target.header.message_id;
	value.target_message_type = target.header.message_type;
	value.ack_flags = protocol::KnownAckFlags;
	value.target_fragment_count = target.header.fragment_count;
	value.target_message_crc32 = target.header.message_crc32;
	std::array<std::uint8_t, protocol::AckPayloadSize> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_ack_payload(value, mutable_bytes(payload), written));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = target.header.session_id;
	header.packet_sequence = sequence;
	header.sent_time_us = 200U + sequence;
	header.message_id = sequence;
	return encode(header, bytes(payload, written));
}

void ingest(detail::SessionController& controller,
	const protocol::EndpointKey& source,
	const Packet& packet,
	std::uint64_t now_us)
{
	(void)controller.ingest(source, {packet.bytes.data(), packet.size}, now_us, 1U, true);
}

std::size_t open_awaiting(Harness& harness,
	std::uint8_t suffix,
	std::uint32_t sequence,
	std::uint64_t base_time_us)
{
	const auto source = endpoint(suffix);
	ingest(harness.controller, source, hello(0x5000U + suffix, sequence), base_time_us + 100U);
	detail::SessionControllerOutput welcome;
	EXPECT_TRUE(harness.controller.pop_output(welcome));
	for (std::size_t i = 0U; i < harness.max_clients; ++i) {
		if (harness.controller.slot(i).progress == detail::ProducerSessionProgress::AwaitWelcomeApplied &&
			harness.controller.slot(i).endpoint == source) return i;
	}
	return std::numeric_limits<std::size_t>::max();
}

std::size_t open_ready(Harness& harness,
	std::uint8_t suffix,
	std::uint32_t sequence,
	std::uint64_t base_time_us)
{
	const auto source = endpoint(suffix);
	ingest(harness.controller, source, hello(0x6000U + suffix, sequence), base_time_us + 100U);
	detail::SessionControllerOutput welcome;
	EXPECT_TRUE(harness.controller.pop_output(welcome));
	ingest(harness.controller, source, applied_ack(decode(welcome), sequence + 1U), base_time_us + 200U);
	detail::SessionControllerOutput begin;
	EXPECT_TRUE(harness.controller.pop_output(begin));
	EXPECT_EQ(protocol::MessageType::SessionBegin, decode(begin).header.message_type);
	ingest(harness.controller, source, applied_ack(decode(begin), sequence + 2U), base_time_us + 300U);
	for (std::size_t i = 0U; i < harness.max_clients; ++i) {
		if (harness.controller.slot(i).endpoint == source) {
			EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState,
				harness.controller.slot(i).progress);
		}
		if (harness.controller.slot(i).progress == detail::ProducerSessionProgress::ReadyForState &&
			harness.controller.slot(i).endpoint == source) return i;
	}
	return std::numeric_limits<std::size_t>::max();
}

detail::CaptureResult capture(detail::CaptureStatus status, detail::CaptureReason reason)
{
	detail::CaptureResult result{};
	result.status = status;
	result.reason = reason;
	return result;
}

detail::PlayerObservationDto observation(std::uint32_t signature, float base)
{
	detail::PlayerObservationDto result{};
	result.key.object_signature = signature;
	result.value.producer_sample_time_us = 9000U + signature;
	result.value.position_world = {base, base + 1.0f, base + 2.0f};
	result.value.orientation_local_to_world = {1.0f, 0.0f, 0.0f, 0.0f};
	result.value.velocity_world = {base + 3.0f, base + 4.0f, base + 5.0f};
	result.value.rotational_velocity_local = {base + 6.0f, base + 7.0f, base + 8.0f};
	result.value.radius = base + 9.0f;
	result.value.physics_mode_flags = 0x55U;
	return result;
}

void expect_value_eq(const detail::PlayerKinematicsValue& actual,
	const detail::PlayerKinematicsValue& expected)
{
	EXPECT_EQ(expected.producer_sample_time_us, actual.producer_sample_time_us);
	EXPECT_FLOAT_EQ(expected.position_world.x, actual.position_world.x);
	EXPECT_FLOAT_EQ(expected.position_world.y, actual.position_world.y);
	EXPECT_FLOAT_EQ(expected.position_world.z, actual.position_world.z);
	EXPECT_FLOAT_EQ(expected.orientation_local_to_world.w, actual.orientation_local_to_world.w);
	EXPECT_FLOAT_EQ(expected.orientation_local_to_world.x, actual.orientation_local_to_world.x);
	EXPECT_FLOAT_EQ(expected.orientation_local_to_world.y, actual.orientation_local_to_world.y);
	EXPECT_FLOAT_EQ(expected.orientation_local_to_world.z, actual.orientation_local_to_world.z);
	EXPECT_FLOAT_EQ(expected.velocity_world.x, actual.velocity_world.x);
	EXPECT_FLOAT_EQ(expected.velocity_world.y, actual.velocity_world.y);
	EXPECT_FLOAT_EQ(expected.velocity_world.z, actual.velocity_world.z);
	EXPECT_FLOAT_EQ(expected.rotational_velocity_local.x, actual.rotational_velocity_local.x);
	EXPECT_FLOAT_EQ(expected.rotational_velocity_local.y, actual.rotational_velocity_local.y);
	EXPECT_FLOAT_EQ(expected.rotational_velocity_local.z, actual.rotational_velocity_local.z);
	EXPECT_FLOAT_EQ(expected.radius, actual.radius);
	EXPECT_EQ(expected.physics_mode_flags, actual.physics_mode_flags);
}

void expect_sample_eq(const detail::PlayerKinematicsSample& actual,
	std::uint64_t entity_id,
	const detail::PlayerObservationDto& expected)
{
	EXPECT_EQ(entity_id, actual.entity_id);
	expect_value_eq(actual.value, expected.value);
}

void expect_default_sample(const detail::PlayerKinematicsSample& actual)
{
	const detail::PlayerKinematicsSample defaults{};
	EXPECT_EQ(defaults.entity_id, actual.entity_id);
	expect_value_eq(actual.value, defaults.value);
}

struct NonPlayerSlotState {
	detail::ProducerSessionProgress progress{};
	protocol::EndpointKey endpoint{};
	std::uint64_t session_id = 0U;
	std::uint64_t session_start_us = 0U;
	std::uint64_t welcome_deadline_us = 0U;
	std::uint32_t next_message_id = 0U;
	std::uint32_t next_packet_sequence = 0U;
	std::uint32_t welcome_message_id = 0U;
	std::uint16_t welcome_fragment_count = 0U;
	std::uint32_t welcome_message_crc32 = 0U;
	std::size_t reassembly_bytes_reserved = 0U;
	std::size_t reliable_items_in_use = 0U;
	std::uint64_t preproof_validated_bytes_received = 0U;
	std::uint64_t preproof_bytes_sent = 0U;
	bool has_reliability_terminal_policy = false;
	protocol::ReliableTerminalPolicy reliability_terminal_policy{};
	std::uint64_t probe_session_id = 0U;
	std::size_t probes_in_flight = 0U;
	bool clock_filter_valid = false;
	std::size_t clock_sample_count = 0U;
	std::uint16_t negotiated_interval_ms = 0U;
	std::uint64_t next_periodic_due_us = 0U;
	std::uint64_t stale_timeout_us = 0U;
	std::uint64_t disconnect_timeout_us = 0U;
	std::uint64_t last_valid_network_activity_us = 0U;
	std::uint64_t last_valid_clock_response_us = 0U;
	bool clock_stale = false;
};

NonPlayerSlotState non_player_state(const detail::SessionControllerSlot& slot)
{
	return {slot.progress,
		slot.endpoint,
		slot.session_id,
		slot.session_start_us,
		slot.welcome_deadline_us,
		slot.next_message_id,
		slot.next_packet_sequence,
		slot.welcome_message_id,
		slot.welcome_fragment_count,
		slot.welcome_message_crc32,
		slot.reassembly_bytes_reserved,
		slot.reliable_items_in_use,
		slot.preproof_validated_bytes_received,
		slot.preproof_bytes_sent,
		slot.has_reliability_terminal_policy,
		slot.reliability_terminal_policy,
		slot.heartbeat.probes.session_id(),
		slot.heartbeat.probes.in_flight_count(),
		slot.heartbeat.clock_filter.valid(),
		slot.heartbeat.clock_filter.sample_count(),
		slot.heartbeat.negotiated_interval_ms,
		slot.heartbeat.next_periodic_due_us,
		slot.heartbeat.stale_timeout_us,
		slot.heartbeat.disconnect_timeout_us,
		slot.heartbeat.last_valid_network_activity_us,
		slot.heartbeat.last_valid_clock_response_us,
		slot.heartbeat.clock_stale};
}

void expect_non_player_state_eq(const NonPlayerSlotState& actual, const NonPlayerSlotState& expected)
{
	EXPECT_EQ(expected.progress, actual.progress);
	EXPECT_EQ(expected.endpoint, actual.endpoint);
	EXPECT_EQ(expected.session_id, actual.session_id);
	EXPECT_EQ(expected.session_start_us, actual.session_start_us);
	EXPECT_EQ(expected.welcome_deadline_us, actual.welcome_deadline_us);
	EXPECT_EQ(expected.next_message_id, actual.next_message_id);
	EXPECT_EQ(expected.next_packet_sequence, actual.next_packet_sequence);
	EXPECT_EQ(expected.welcome_message_id, actual.welcome_message_id);
	EXPECT_EQ(expected.welcome_fragment_count, actual.welcome_fragment_count);
	EXPECT_EQ(expected.welcome_message_crc32, actual.welcome_message_crc32);
	EXPECT_EQ(expected.reassembly_bytes_reserved, actual.reassembly_bytes_reserved);
	EXPECT_EQ(expected.reliable_items_in_use, actual.reliable_items_in_use);
	EXPECT_EQ(expected.preproof_validated_bytes_received, actual.preproof_validated_bytes_received);
	EXPECT_EQ(expected.preproof_bytes_sent, actual.preproof_bytes_sent);
	EXPECT_EQ(expected.has_reliability_terminal_policy, actual.has_reliability_terminal_policy);
	EXPECT_EQ(expected.reliability_terminal_policy, actual.reliability_terminal_policy);
	EXPECT_EQ(expected.probe_session_id, actual.probe_session_id);
	EXPECT_EQ(expected.probes_in_flight, actual.probes_in_flight);
	EXPECT_EQ(expected.clock_filter_valid, actual.clock_filter_valid);
	EXPECT_EQ(expected.clock_sample_count, actual.clock_sample_count);
	EXPECT_EQ(expected.negotiated_interval_ms, actual.negotiated_interval_ms);
	EXPECT_EQ(expected.next_periodic_due_us, actual.next_periodic_due_us);
	EXPECT_EQ(expected.stale_timeout_us, actual.stale_timeout_us);
	EXPECT_EQ(expected.disconnect_timeout_us, actual.disconnect_timeout_us);
	EXPECT_EQ(expected.last_valid_network_activity_us, actual.last_valid_network_activity_us);
	EXPECT_EQ(expected.last_valid_clock_response_us, actual.last_valid_clock_response_us);
	EXPECT_EQ(expected.clock_stale, actual.clock_stale);
}

template <typename Result>
void expect_balanced(const Result& result)
{
	EXPECT_EQ(result.eligible_slots,
		result.materialized_existing_slots + result.materialized_new_slots + result.no_player_slots +
			result.invalid_source_slots + result.invalid_capture_slots + result.closed_exhausted_slots);
}

detail::PlayerSampleMaterializeStatus expected_closed_outcome(detail::CaptureStatus status,
	detail::CaptureReason reason)
{
	switch (status) {
	case detail::CaptureStatus::NoPlayer:
		switch (reason) {
		case detail::CaptureReason::NotInMission:
		case detail::CaptureReason::MissingPlayer:
		case detail::CaptureReason::MissingPlayerObject:
		case detail::CaptureReason::MissingPlayerShip:
			return detail::PlayerSampleMaterializeStatus::NoPlayer;
		default:
			return detail::PlayerSampleMaterializeStatus::InvalidCapture;
		}
	case detail::CaptureStatus::InvalidSource:
		switch (reason) {
		case detail::CaptureReason::WrongObjectType:
		case detail::CaptureReason::ShipInstanceOutOfRange:
		case detail::CaptureReason::PlayerObjectMismatch:
		case detail::CaptureReason::PlayerShipMismatch:
		case detail::CaptureReason::InvalidObservationKey:
		case detail::CaptureReason::InvalidPosition:
		case detail::CaptureReason::InvalidOrientation:
		case detail::CaptureReason::InvalidVelocity:
		case detail::CaptureReason::InvalidRotationalVelocity:
		case detail::CaptureReason::InvalidRadius:
			return detail::PlayerSampleMaterializeStatus::InvalidSource;
		default:
			return detail::PlayerSampleMaterializeStatus::InvalidCapture;
		}
	case detail::CaptureStatus::Valid:
	case detail::CaptureStatus::Count:
	default:
		return detail::PlayerSampleMaterializeStatus::InvalidCapture;
	}
}

template <typename Result>
void expect_unique_outcome(const Result& result, detail::PlayerSampleMaterializeStatus expected)
{
	expect_balanced(result);
	EXPECT_EQ(1U, result.eligible_slots);
	EXPECT_EQ(expected == detail::PlayerSampleMaterializeStatus::MaterializedExisting ? 1U : 0U,
		result.materialized_existing_slots);
	EXPECT_EQ(expected == detail::PlayerSampleMaterializeStatus::MaterializedNew ? 1U : 0U,
		result.materialized_new_slots);
	EXPECT_EQ(expected == detail::PlayerSampleMaterializeStatus::NoPlayer ? 1U : 0U,
		result.no_player_slots);
	EXPECT_EQ(expected == detail::PlayerSampleMaterializeStatus::InvalidSource ? 1U : 0U,
		result.invalid_source_slots);
	EXPECT_EQ(expected == detail::PlayerSampleMaterializeStatus::InvalidCapture ? 1U : 0U,
		result.invalid_capture_slots);
	EXPECT_EQ(expected == detail::PlayerSampleMaterializeStatus::EntityIdCounterExhausted ? 1U : 0U,
		result.closed_exhausted_slots);
}

template <typename Controller, typename Access>
void verify_api()
{
	if constexpr (!has_d2_api<Controller>::value || !has_d2_seed_access<Access, Controller>::value) {
		FAIL() << MissingD2;
	} else {
		using ApplySignature = detail::SessionPlayerMaterializationResult (Controller::*)(
			const detail::CaptureResult&, const detail::PlayerObservationDto&) noexcept;
		using ClearSignature = void (Controller::*)() noexcept;
		using SeedSignature = bool (*)(Controller&, std::size_t, std::uint64_t) noexcept;
		static_assert(std::is_same_v<decltype(static_cast<ApplySignature>(&Controller::apply_player_observation)),
			ApplySignature>);
		static_assert(std::is_same_v<decltype(static_cast<ClearSignature>(&Controller::clear_player_observations)),
			ClearSignature>);
		static_assert(std::is_same_v<decltype(static_cast<SeedSignature>(&Access::seed_last_allocated_entity_id)),
			SeedSignature>);
		using Result = decltype(std::declval<Controller&>().apply_player_observation(
			std::declval<const detail::CaptureResult&>(), std::declval<const detail::PlayerObservationDto&>()));
		static_assert(std::is_same_v<Result, detail::SessionPlayerMaterializationResult>);
		static_assert(std::is_same_v<decltype(std::declval<Result>().eligible_slots), std::size_t>);
		static_assert(std::is_same_v<decltype(std::declval<Result>().materialized_existing_slots), std::size_t>);
		static_assert(std::is_same_v<decltype(std::declval<Result>().materialized_new_slots), std::size_t>);
		static_assert(std::is_same_v<decltype(std::declval<Result>().no_player_slots), std::size_t>);
		static_assert(std::is_same_v<decltype(std::declval<Result>().invalid_source_slots), std::size_t>);
		static_assert(std::is_same_v<decltype(std::declval<Result>().invalid_capture_slots), std::size_t>);
		static_assert(std::is_same_v<decltype(std::declval<Result>().closed_exhausted_slots), std::size_t>);
		static_assert(std::is_same_v<decltype(std::declval<detail::SessionControllerSlot>().player_entity_ids),
			detail::EntityIdRegistry>);
		static_assert(std::is_same_v<decltype(std::declval<detail::SessionControllerSlot>().latest_player_sample),
			detail::PlayerKinematicsSample>);
		static_assert(std::is_same_v<decltype(std::declval<detail::SessionControllerSlot>().latest_player_sample_status),
			detail::PlayerSampleMaterializeStatus>);
		static_assert(std::is_same_v<decltype(std::declval<detail::SessionControllerSlot>().has_latest_player_sample), bool>);
		const Result defaults{};
		const auto& [eligible,
			existing,
			materialized,
			no_player,
			invalid_source,
			invalid_capture,
			exhausted] = defaults;
		(void)eligible;
		(void)existing;
		(void)materialized;
		(void)no_player;
		(void)invalid_source;
		(void)invalid_capture;
		(void)exhausted;
		expect_balanced(defaults);
		EXPECT_EQ(0U, defaults.eligible_slots);
		EXPECT_EQ(0U, defaults.materialized_existing_slots);
		EXPECT_EQ(0U, defaults.materialized_new_slots);
		EXPECT_EQ(0U, defaults.no_player_slots);
		EXPECT_EQ(0U, defaults.invalid_source_slots);
		EXPECT_EQ(0U, defaults.invalid_capture_slots);
		EXPECT_EQ(0U, defaults.closed_exhausted_slots);
		const detail::SessionControllerSlot slot{};
		EXPECT_FALSE(slot.has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture, slot.latest_player_sample_status);
		expect_default_sample(slot.latest_player_sample);
		EXPECT_EQ(0U, slot.player_entity_ids.last_allocated_entity_id());
		EXPECT_FALSE(slot.player_entity_ids.has_active_mapping());
	}
}

template <typename Controller, typename Access>
void verify_ineligible()
{
	if constexpr (!has_d2_api<Controller>::value || !has_d2_seed_access<Access, Controller>::value) FAIL() << MissingD2;
	else {
		Harness h;
		const auto empty_before = non_player_state(h.controller.slot(0U));
		EXPECT_FALSE(Access::seed_last_allocated_entity_id(h.controller, 0U, 9U));
		auto result = h.controller.apply_player_observation(
			capture(detail::CaptureStatus::Valid, detail::CaptureReason::None), observation(42U, 1.0f));
		expect_balanced(result); EXPECT_EQ(0U, result.eligible_slots);
		expect_non_player_state_eq(non_player_state(h.controller.slot(0U)), empty_before);
		EXPECT_EQ(0U, h.controller.slot(0U).player_entity_ids.last_allocated_entity_id());
		EXPECT_FALSE(h.controller.slot(0U).player_entity_ids.has_active_mapping());
		EXPECT_FALSE(h.controller.slot(0U).has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture,
			h.controller.slot(0U).latest_player_sample_status);
		expect_default_sample(h.controller.slot(0U).latest_player_sample);
		const auto awaiting = open_awaiting(h, 2U, 10U, 1'000U);
		ASSERT_LT(awaiting, 4U);
		const auto awaiting_before = non_player_state(h.controller.slot(awaiting));
		result = h.controller.apply_player_observation(
			capture(detail::CaptureStatus::Valid, detail::CaptureReason::None), observation(42U, 1.0f));
		expect_balanced(result); EXPECT_EQ(0U, result.eligible_slots);
		expect_non_player_state_eq(non_player_state(h.controller.slot(awaiting)), awaiting_before);
		EXPECT_EQ(0U, h.controller.slot(awaiting).player_entity_ids.last_allocated_entity_id());
		EXPECT_FALSE(h.controller.slot(awaiting).player_entity_ids.has_active_mapping());
		EXPECT_FALSE(h.controller.slot(awaiting).has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture,
			h.controller.slot(awaiting).latest_player_sample_status);
		expect_default_sample(h.controller.slot(awaiting).latest_player_sample);
		EXPECT_FALSE(Access::seed_last_allocated_entity_id(h.controller, awaiting, 9U));
		EXPECT_FALSE(Access::seed_last_allocated_entity_id(h.controller, 99U, 9U));
	}
}

template <typename Controller, typename Access>
void verify_one_ready()
{
	if constexpr (!has_d2_api<Controller>::value) FAIL() << MissingD2;
	else {
		Harness h; const auto index = open_ready(h, 2U, 10U, 1'000U); ASSERT_LT(index, 4U);
		const auto first_observation = observation(42U, 1.0f);
		auto first = h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid, detail::CaptureReason::None), first_observation);
		expect_unique_outcome(first, detail::PlayerSampleMaterializeStatus::MaterializedNew);
		EXPECT_TRUE(h.controller.slot(index).has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedNew,
			h.controller.slot(index).latest_player_sample_status);
		expect_sample_eq(h.controller.slot(index).latest_player_sample, 1U, first_observation);
		const auto same_observation = observation(42U, 20.0f);
		auto same = h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid, detail::CaptureReason::None), same_observation);
		expect_unique_outcome(same, detail::PlayerSampleMaterializeStatus::MaterializedExisting);
		EXPECT_TRUE(h.controller.slot(index).has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedExisting,
			h.controller.slot(index).latest_player_sample_status);
		expect_sample_eq(h.controller.slot(index).latest_player_sample, 1U, same_observation);
		const auto replacement = observation(77U, 30.0f);
		auto changed = h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid, detail::CaptureReason::None), replacement);
		expect_unique_outcome(changed, detail::PlayerSampleMaterializeStatus::MaterializedNew);
		EXPECT_TRUE(h.controller.slot(index).has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedNew,
			h.controller.slot(index).latest_player_sample_status);
		expect_sample_eq(h.controller.slot(index).latest_player_sample, 2U, replacement);
	}
}

template <typename Controller, typename Access>
void verify_two_ready()
{
	if constexpr (!has_d2_api<Controller>::value) FAIL() << MissingD2;
	else {
		Harness h; const auto a=open_ready(h,2U,10U,1'000U); const auto b=open_ready(h,3U,20U,10'000U); ASSERT_LT(a,4U); ASSERT_LT(b,4U);
		auto source=observation(42U,2.0f); auto first=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),source);
		expect_balanced(first); EXPECT_EQ(2U,first.eligible_slots); EXPECT_EQ(2U,first.materialized_new_slots);
		expect_sample_eq(h.controller.slot(a).latest_player_sample,1U,source); expect_sample_eq(h.controller.slot(b).latest_player_sample,1U,source);
		EXPECT_TRUE(h.controller.slot(a).has_latest_player_sample); EXPECT_TRUE(h.controller.slot(b).has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedNew,h.controller.slot(a).latest_player_sample_status);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedNew,h.controller.slot(b).latest_player_sample_status);
		source.value.position_world.x=99.0f; EXPECT_FLOAT_EQ(2.0f,h.controller.slot(a).latest_player_sample.value.position_world.x); EXPECT_FLOAT_EQ(2.0f,h.controller.slot(b).latest_player_sample.value.position_world.x);
		const auto same_source=observation(42U,4.0f); auto same=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),same_source); expect_balanced(same); EXPECT_EQ(2U,same.materialized_existing_slots);
		expect_sample_eq(h.controller.slot(a).latest_player_sample,1U,same_source); expect_sample_eq(h.controller.slot(b).latest_player_sample,1U,same_source);
		const auto replacement=observation(77U,8.0f); auto replaced=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),replacement);
		expect_balanced(replaced); EXPECT_EQ(2U,replaced.eligible_slots); EXPECT_EQ(2U,replaced.materialized_new_slots);
		expect_sample_eq(h.controller.slot(a).latest_player_sample,2U,replacement); expect_sample_eq(h.controller.slot(b).latest_player_sample,2U,replacement);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedNew,h.controller.slot(a).latest_player_sample_status);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedNew,h.controller.slot(b).latest_player_sample_status);
	}
}

template <typename Controller, typename Access>
void verify_stale()
{
	if constexpr (!has_d2_api<Controller>::value) FAIL() << MissingD2;
	else {
		Harness h; const auto i=open_ready(h,2U,10U,1'000U); ASSERT_LT(i,4U);
		(void)h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),observation(42U,1.0f));
		h.controller.service_timeouts(h.controller.slot(i).heartbeat.last_valid_clock_response_us +
			h.controller.slot(i).heartbeat.stale_timeout_us);
		ASSERT_EQ(detail::ProducerSessionProgress::Stale,h.controller.slot(i).progress);
		const auto state_before=non_player_state(h.controller.slot(i));
		const auto usage_before=h.controller.owned_usage();
		const auto update=observation(42U,8.0f);
		auto result=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),update);
		expect_unique_outcome(result,detail::PlayerSampleMaterializeStatus::MaterializedExisting);
		expect_non_player_state_eq(non_player_state(h.controller.slot(i)),state_before);
		EXPECT_EQ(usage_before,h.controller.owned_usage());
		EXPECT_TRUE(h.controller.slot(i).has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedExisting,h.controller.slot(i).latest_player_sample_status);
		expect_sample_eq(h.controller.slot(i).latest_player_sample,1U,update);
		EXPECT_TRUE(h.controller.slot(i).player_entity_ids.has_active_mapping());
		EXPECT_EQ(42U,h.controller.slot(i).player_entity_ids.active_object_signature());
	}
}

template <typename Controller, typename Access>
void verify_failure_matrix()
{
	if constexpr (!has_d2_api<Controller>::value) FAIL() << MissingD2;
	else {
		for(std::uint8_t sv=0U;sv<=static_cast<std::uint8_t>(detail::CaptureStatus::Count);++sv) {
			for(std::uint8_t rv=0U;rv<=static_cast<std::uint8_t>(detail::CaptureReason::Count);++rv) {
				const auto status=static_cast<detail::CaptureStatus>(sv); const auto reason=static_cast<detail::CaptureReason>(rv);
				if(status==detail::CaptureStatus::Valid&&reason==detail::CaptureReason::None) continue;
				Harness h(1U); const auto i=open_ready(h,2U,10U,1'000U); ASSERT_EQ(0U,i);
				(void)h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),observation(42U,1.0f));
				const auto expected=expected_closed_outcome(status,reason);
				auto result=h.controller.apply_player_observation(capture(status,reason),observation(42U,2.0f));
				expect_unique_outcome(result,expected);
				EXPECT_FALSE(h.controller.slot(i).has_latest_player_sample);
				EXPECT_EQ(expected,h.controller.slot(i).latest_player_sample_status);
				expect_default_sample(h.controller.slot(i).latest_player_sample);
				EXPECT_FALSE(h.controller.slot(i).player_entity_ids.has_active_mapping());
				EXPECT_EQ(0U,h.controller.slot(i).player_entity_ids.active_object_signature());
				EXPECT_EQ(0U,h.controller.slot(i).player_entity_ids.active_entity_id());
				EXPECT_EQ(1U,h.controller.slot(i).player_entity_ids.last_allocated_entity_id());
				const auto reappeared=observation(42U,3.0f);
				auto again=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),reappeared);
				expect_unique_outcome(again,detail::PlayerSampleMaterializeStatus::MaterializedNew);
				expect_sample_eq(h.controller.slot(i).latest_player_sample,2U,reappeared);
			}
		}
		Harness invalid_key(1U); const auto index=open_ready(invalid_key,2U,10U,1'000U); ASSERT_EQ(0U,index);
		(void)invalid_key.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),observation(42U,1.0f));
		auto result=invalid_key.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),observation(0U,2.0f));
		expect_unique_outcome(result,detail::PlayerSampleMaterializeStatus::InvalidCapture);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture,invalid_key.controller.slot(index).latest_player_sample_status);
		EXPECT_FALSE(invalid_key.controller.slot(index).has_latest_player_sample);
		expect_default_sample(invalid_key.controller.slot(index).latest_player_sample);
		EXPECT_FALSE(invalid_key.controller.slot(index).player_entity_ids.has_active_mapping());
		EXPECT_EQ(1U,invalid_key.controller.slot(index).player_entity_ids.last_allocated_entity_id());
		const auto reappeared=observation(42U,3.0f);
		result=invalid_key.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),reappeared);
		expect_unique_outcome(result,detail::PlayerSampleMaterializeStatus::MaterializedNew);
		expect_sample_eq(invalid_key.controller.slot(index).latest_player_sample,2U,reappeared);
	}
}

template <typename Controller, typename Access>
void verify_clear()
{
	if constexpr (!has_d2_api<Controller>::value) FAIL() << MissingD2;
	else {
		Harness h; const auto stale=open_ready(h,2U,10U,1'000U); ASSERT_LT(stale,4U);
		h.controller.service_timeouts(h.controller.slot(stale).heartbeat.last_valid_clock_response_us + h.controller.slot(stale).heartbeat.stale_timeout_us);
		ASSERT_EQ(detail::ProducerSessionProgress::Stale,h.controller.slot(stale).progress);
		const auto ready=open_ready(h,3U,20U,4'000'000U); const auto awaiting=open_awaiting(h,4U,30U,4'010'000U); ASSERT_LT(ready,4U);ASSERT_LT(awaiting,4U);
		(void)h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),observation(42U,1.0f));
		std::size_t empty=0U; while(empty<4U&&h.controller.slot(empty).progress!=detail::ProducerSessionProgress::Empty) ++empty; ASSERT_LT(empty,4U);
		const auto stale_before=non_player_state(h.controller.slot(stale)); const auto ready_before=non_player_state(h.controller.slot(ready));
		const auto awaiting_before=non_player_state(h.controller.slot(awaiting)); const auto empty_before=non_player_state(h.controller.slot(empty));
		const auto usage_before=h.controller.owned_usage();
		h.controller.clear_player_observations(); h.controller.clear_player_observations();
		expect_non_player_state_eq(non_player_state(h.controller.slot(stale)),stale_before); expect_non_player_state_eq(non_player_state(h.controller.slot(ready)),ready_before);
		expect_non_player_state_eq(non_player_state(h.controller.slot(awaiting)),awaiting_before); expect_non_player_state_eq(non_player_state(h.controller.slot(empty)),empty_before);
		EXPECT_EQ(usage_before,h.controller.owned_usage());
		for(const auto index : {stale,ready,awaiting,empty}) { EXPECT_FALSE(h.controller.slot(index).has_latest_player_sample); EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture,h.controller.slot(index).latest_player_sample_status); expect_default_sample(h.controller.slot(index).latest_player_sample); EXPECT_FALSE(h.controller.slot(index).player_entity_ids.has_active_mapping()); }
		EXPECT_EQ(1U,h.controller.slot(ready).player_entity_ids.last_allocated_entity_id()); EXPECT_EQ(1U,h.controller.slot(stale).player_entity_ids.last_allocated_entity_id());
		EXPECT_EQ(0U,h.controller.slot(awaiting).player_entity_ids.last_allocated_entity_id()); EXPECT_EQ(0U,h.controller.slot(empty).player_entity_ids.last_allocated_entity_id());
		auto result=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),observation(42U,2.0f)); expect_balanced(result); EXPECT_EQ(2U,result.materialized_new_slots); EXPECT_EQ(2U,h.controller.slot(ready).latest_player_sample.entity_id); EXPECT_EQ(2U,h.controller.slot(stale).latest_player_sample.entity_id);
	}
}

template <typename Controller, typename Access>
void verify_close(bool purge)
{
	if constexpr (!has_d2_api<Controller>::value) FAIL() << MissingD2;
	else {
		Harness h; const auto a=open_ready(h,2U,10U,1'000U); const auto b=open_ready(h,3U,20U,10'000U); ASSERT_LT(a,4U);ASSERT_LT(b,4U);
		(void)h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),observation(42U,1.0f));
		const auto replacement=observation(77U,2.0f); (void)h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),replacement);
		ASSERT_EQ(2U,h.registry.used_count());
		if(purge){
			h.controller.purge_all(detail::SessionCloseReason::MissionDiscontinuity); EXPECT_EQ(0U,h.controller.active_slots());
			for(std::size_t i=0;i<4U;++i){ EXPECT_EQ(detail::ProducerSessionProgress::Empty,h.controller.slot(i).progress); EXPECT_EQ(0U,h.controller.slot(i).session_id); EXPECT_EQ(0U,h.controller.slot(i).heartbeat.probes.session_id()); EXPECT_EQ(0U,h.controller.slot(i).player_entity_ids.last_allocated_entity_id()); EXPECT_FALSE(h.controller.slot(i).player_entity_ids.has_active_mapping()); EXPECT_FALSE(h.controller.slot(i).has_latest_player_sample); EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture,h.controller.slot(i).latest_player_sample_status); expect_default_sample(h.controller.slot(i).latest_player_sample); }
			EXPECT_EQ(2U,h.registry.used_count()); const auto reused=open_ready(h,5U,40U,20'000U); ASSERT_LT(reused,4U); EXPECT_EQ(3U,h.registry.used_count());
			const auto next=observation(77U,3.0f); auto result=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),next); expect_unique_outcome(result,detail::PlayerSampleMaterializeStatus::MaterializedNew); expect_sample_eq(h.controller.slot(reused).latest_player_sample,1U,next);
		}else{
			const auto survivor_state=non_player_state(h.controller.slot(b)); const auto survivor_sample=h.controller.slot(b).latest_player_sample;
			ASSERT_TRUE(h.controller.close_slot(a,detail::SessionCloseReason::Timeout));
			EXPECT_EQ(detail::ProducerSessionProgress::Empty,h.controller.slot(a).progress); EXPECT_EQ(0U,h.controller.slot(a).session_id); EXPECT_EQ(0U,h.controller.slot(a).heartbeat.probes.session_id()); EXPECT_EQ(0U,h.controller.slot(a).player_entity_ids.last_allocated_entity_id()); EXPECT_FALSE(h.controller.slot(a).player_entity_ids.has_active_mapping()); EXPECT_FALSE(h.controller.slot(a).has_latest_player_sample); EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture,h.controller.slot(a).latest_player_sample_status); expect_default_sample(h.controller.slot(a).latest_player_sample);
			expect_non_player_state_eq(non_player_state(h.controller.slot(b)),survivor_state); expect_sample_eq(h.controller.slot(b).latest_player_sample,survivor_sample.entity_id,replacement); EXPECT_EQ(2U,h.controller.slot(b).player_entity_ids.last_allocated_entity_id()); EXPECT_TRUE(h.controller.slot(b).player_entity_ids.has_active_mapping());
			EXPECT_EQ(2U,h.registry.used_count()); const auto reused=open_ready(h,5U,40U,20'000U); ASSERT_EQ(a,reused); EXPECT_EQ(3U,h.registry.used_count());
			const auto next=observation(77U,3.0f); auto result=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),next); expect_balanced(result); EXPECT_EQ(2U,result.eligible_slots); EXPECT_EQ(1U,result.materialized_existing_slots); EXPECT_EQ(1U,result.materialized_new_slots); EXPECT_EQ(0U,result.no_player_slots+result.invalid_source_slots+result.invalid_capture_slots+result.closed_exhausted_slots); expect_sample_eq(h.controller.slot(reused).latest_player_sample,1U,next); expect_sample_eq(h.controller.slot(b).latest_player_sample,2U,next);
		}
	}
}

template <typename Controller, typename Access>
void verify_exhaustion()
{
	if constexpr (!has_d2_api<Controller>::value || !has_d2_seed_access<Access,Controller>::value) FAIL() << MissingD2;
	else {
		Harness h; const auto a=open_ready(h,2U,10U,1'000U); const auto b=open_ready(h,3U,20U,10'000U); ASSERT_LT(a,4U);ASSERT_LT(b,4U);
		(void)h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),observation(9U,0.5f));
		const auto seeded_state=non_player_state(h.controller.slot(a));
		ASSERT_TRUE(Access::seed_last_allocated_entity_id(h.controller,a,std::numeric_limits<std::uint64_t>::max()));
		expect_non_player_state_eq(non_player_state(h.controller.slot(a)),seeded_state);
		EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(),h.controller.slot(a).player_entity_ids.last_allocated_entity_id());
		EXPECT_FALSE(h.controller.slot(a).player_entity_ids.has_active_mapping()); EXPECT_FALSE(h.controller.slot(a).has_latest_player_sample);
		EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture,h.controller.slot(a).latest_player_sample_status); expect_default_sample(h.controller.slot(a).latest_player_sample);
		const auto next=observation(42U,1.0f); auto result=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),next);
		expect_balanced(result); EXPECT_EQ(2U,result.eligible_slots); EXPECT_EQ(0U,result.materialized_existing_slots); EXPECT_EQ(1U,result.materialized_new_slots); EXPECT_EQ(0U,result.no_player_slots); EXPECT_EQ(0U,result.invalid_source_slots); EXPECT_EQ(0U,result.invalid_capture_slots); EXPECT_EQ(1U,result.closed_exhausted_slots);
		EXPECT_EQ(detail::ProducerSessionProgress::Empty,h.controller.slot(a).progress); EXPECT_EQ(0U,h.controller.slot(a).session_id); EXPECT_EQ(0U,h.controller.slot(a).heartbeat.probes.session_id()); EXPECT_EQ(0U,h.controller.slot(a).player_entity_ids.last_allocated_entity_id()); EXPECT_FALSE(h.controller.slot(a).has_latest_player_sample); EXPECT_EQ(detail::PlayerSampleMaterializeStatus::InvalidCapture,h.controller.slot(a).latest_player_sample_status); expect_default_sample(h.controller.slot(a).latest_player_sample);
		EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState,h.controller.slot(b).progress); EXPECT_EQ(2U,h.controller.slot(b).player_entity_ids.last_allocated_entity_id()); EXPECT_TRUE(h.controller.slot(b).has_latest_player_sample); EXPECT_EQ(detail::PlayerSampleMaterializeStatus::MaterializedNew,h.controller.slot(b).latest_player_sample_status); expect_sample_eq(h.controller.slot(b).latest_player_sample,2U,next); EXPECT_EQ(1U,h.controller.active_slots()); EXPECT_EQ(2U,h.registry.used_count());
		const auto update=observation(42U,2.0f); result=h.controller.apply_player_observation(capture(detail::CaptureStatus::Valid,detail::CaptureReason::None),update); expect_unique_outcome(result,detail::PlayerSampleMaterializeStatus::MaterializedExisting); expect_sample_eq(h.controller.slot(b).latest_player_sample,2U,update);
	}
}

#define D2_TEST(name, function) TEST(TelemetrySessionPlayerObservationContract,name){function<detail::SessionController,detail::SessionControllerPlayerTestAccess>();}
D2_TEST(ExactLocalApiResultAndSlotDefaultsExist,verify_api)
D2_TEST(EmptyAndAwaitWelcomeAreUntouchedAndConsumeNoIds,verify_ineligible)
D2_TEST(OneReadyDistinguishesFirstSameKeyAndReplacement,verify_one_ready)
D2_TEST(TwoReadySessionsOwnIndependentIdentityAndSamples,verify_two_ready)
D2_TEST(RealStaleRemainsEligibleWithoutSessionMutation,verify_stale)
D2_TEST(EveryClosedCapturePairMapsOneOutcomeAndClearsWithoutCounterRewind,verify_failure_matrix)
D2_TEST(ClearAllStatesIsIdempotentAndPreservesSessionCounters,verify_clear)
TEST(TelemetrySessionPlayerObservationContract, CloseResetsOnlyReusedSlotAndPreservesProcessSessionRegistry){verify_close<detail::SessionController,detail::SessionControllerPlayerTestAccess>(false);}
TEST(TelemetrySessionPlayerObservationContract, PurgeResetsEverySlotIdentityButNeverProcessSessionIds){verify_close<detail::SessionController,detail::SessionControllerPlayerTestAccess>(true);}
D2_TEST(CounterExhaustionClosesOnlyOwningSlotAndBalancesBuckets,verify_exhaustion)
#undef D2_TEST

} // namespace
