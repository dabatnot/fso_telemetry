#include "telemetry/runtime.h"

#if __has_include("telemetry/native_session_runtime.h")
#include "telemetry/native_session_runtime.h"
#include "telemetry/engine_adapter.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/session_controller.h"
#include "telemetry/startup_budget.h"
#include "telemetry/transport.h"
#include "telemetry_native_session_runtime_player_test_access.h"
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
#include <limits>
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

Packet heartbeat_request(std::uint64_t session_id,
	protocol::EndpointKey endpoint,
	std::uint32_t packet_sequence,
	std::uint64_t origin_t0_us)
{
	protocol::HeartbeatPayload heartbeat;
	heartbeat.probe_id = packet_sequence;
	heartbeat.kind = protocol::HeartbeatKind::Request;
	heartbeat.origin_t0_us = origin_t0_us;
	std::array<std::uint8_t, protocol::HeartbeatPayloadSize> payload{};
	std::size_t payload_size = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_heartbeat_payload(heartbeat, mutable_byte_view(payload), payload_size));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Heartbeat;
	header.session_id = session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = origin_t0_us;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), payload_size});
	Packet result;
	result.endpoint = endpoint;
	result.bytes.resize(protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
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

using NativePlayerAccess = detail::NativeSessionRuntimePlayerTestAccess;
using NativePlayerProbe = detail::NativeSessionRuntimePlayerPublicProbe;

constexpr std::uint8_t CaptureUnavailable = 0U;
constexpr std::uint8_t CaptureInactive = 1U;
constexpr std::uint8_t CaptureNotDue = 2U;
constexpr std::uint8_t CaptureValid = 3U;
constexpr std::uint8_t CaptureNoPlayer = 4U;
constexpr std::uint8_t CaptureInvalidSource = 5U;
constexpr std::uint8_t CaptureInvariantFailure = 6U;
constexpr std::uint8_t CaptureCadenceFailure = 7U;
constexpr std::uint8_t TickPermanentCaptureFailure = 3U;

#define REQUIRE_NATIVE_PLAYER_D3()                                                                                  \
	do {                                                                                                            \
		if (!NativePlayerProbe::public_contract_available() || !NativePlayerAccess::private_contract_available()) { \
			FAIL() << "WP07-D3 RED: NativeSessionRuntime player capture public/private contract is absent.";        \
			return;                                                                                                 \
		}                                                                                                           \
	} while (false)

struct CountingEngineReadView final : detail::EngineReadView {
	bool in_mission() const noexcept override
	{
		++in_mission_calls;
		return mission;
	}
	bool player_exists() const noexcept override
	{
		++player_calls;
		return player;
	}
	bool player_object_exists() const noexcept override
	{
		++object_calls;
		return object;
	}
	bool player_ship_exists() const noexcept override
	{
		++ship_calls;
		return ship;
	}
	bool player_object_is_ship() const noexcept override
	{
		++object_type_calls;
		return object_is_ship;
	}
	bool player_object_ship_instance_in_range() const noexcept override
	{
		++instance_calls;
		return instance_in_range;
	}
	bool player_object_matches_player() const noexcept override
	{
		++object_match_calls;
		return object_matches;
	}
	bool player_ship_matches_object() const noexcept override
	{
		++ship_match_calls;
		return ship_matches;
	}
	void read_player_kinematics(detail::EnginePlayerKinematicsRead& output) const noexcept override
	{
		++read_calls;
		output = input;
	}

	std::size_t total_calls() const noexcept
	{
		return in_mission_calls + player_calls + object_calls + ship_calls + object_type_calls + instance_calls +
			   object_match_calls + ship_match_calls + read_calls;
	}
	void clear_counts() const noexcept
	{
		in_mission_calls = player_calls = object_calls = ship_calls = object_type_calls = 0U;
		instance_calls = object_match_calls = ship_match_calls = read_calls = 0U;
	}

	detail::EnginePlayerKinematicsRead
		input{42, {1.0f, 2.0f, 3.0f}, {}, {4.0f, 5.0f, 6.0f}, {0.1f, 0.2f, 0.3f}, 7.0f, {}};
	bool mission = true;
	bool player = true;
	bool object = true;
	bool ship = true;
	bool object_is_ship = true;
	bool instance_in_range = true;
	bool object_matches = true;
	bool ship_matches = true;
	mutable std::size_t in_mission_calls = 0U;
	mutable std::size_t player_calls = 0U;
	mutable std::size_t object_calls = 0U;
	mutable std::size_t ship_calls = 0U;
	mutable std::size_t object_type_calls = 0U;
	mutable std::size_t instance_calls = 0U;
	mutable std::size_t object_match_calls = 0U;
	mutable std::size_t ship_match_calls = 0U;
	mutable std::size_t read_calls = 0U;
};

detail::PlayerObservationDto observation(std::uint32_t signature, std::uint64_t time_us, float marker = 1.0f) noexcept
{
	detail::PlayerObservationDto value;
	value.key.object_signature = signature;
	value.value.producer_sample_time_us = time_us;
	value.value.position_world = {marker, marker + 1.0f, marker + 2.0f};
	value.value.orientation_local_to_world = {1.0f, 0.0f, 0.0f, 0.0f};
	value.value.velocity_world = {marker + 3.0f, marker + 4.0f, marker + 5.0f};
	value.value.rotational_velocity_local = {0.1f, 0.2f, 0.3f};
	value.value.radius = marker + 6.0f;
	value.value.physics_mode_flags = 0U;
	return value;
}

void expect_observation(const detail::PlayerObservationDto& actual, const detail::PlayerObservationDto& expected)
{
	EXPECT_EQ(expected.key.object_signature, actual.key.object_signature);
	EXPECT_EQ(expected.value.producer_sample_time_us, actual.value.producer_sample_time_us);
	EXPECT_FLOAT_EQ(expected.value.position_world.x, actual.value.position_world.x);
	EXPECT_FLOAT_EQ(expected.value.position_world.y, actual.value.position_world.y);
	EXPECT_FLOAT_EQ(expected.value.position_world.z, actual.value.position_world.z);
	EXPECT_FLOAT_EQ(expected.value.orientation_local_to_world.w, actual.value.orientation_local_to_world.w);
	EXPECT_FLOAT_EQ(expected.value.orientation_local_to_world.x, actual.value.orientation_local_to_world.x);
	EXPECT_FLOAT_EQ(expected.value.orientation_local_to_world.y, actual.value.orientation_local_to_world.y);
	EXPECT_FLOAT_EQ(expected.value.orientation_local_to_world.z, actual.value.orientation_local_to_world.z);
	EXPECT_FLOAT_EQ(expected.value.velocity_world.x, actual.value.velocity_world.x);
	EXPECT_FLOAT_EQ(expected.value.velocity_world.y, actual.value.velocity_world.y);
	EXPECT_FLOAT_EQ(expected.value.velocity_world.z, actual.value.velocity_world.z);
	EXPECT_FLOAT_EQ(expected.value.rotational_velocity_local.x, actual.value.rotational_velocity_local.x);
	EXPECT_FLOAT_EQ(expected.value.rotational_velocity_local.y, actual.value.rotational_velocity_local.y);
	EXPECT_FLOAT_EQ(expected.value.rotational_velocity_local.z, actual.value.rotational_velocity_local.z);
	EXPECT_FLOAT_EQ(expected.value.radius, actual.value.radius);
	EXPECT_EQ(expected.value.physics_mode_flags, actual.value.physics_mode_flags);
}

void expect_player_sample(const detail::PlayerKinematicsSample& actual,
	const detail::PlayerKinematicsSample& expected)
{
	EXPECT_EQ(expected.entity_id, actual.entity_id);
	detail::PlayerObservationDto actual_value;
	actual_value.value = actual.value;
	detail::PlayerObservationDto expected_value;
	expected_value.value = expected.value;
	expect_observation(actual_value, expected_value);
}

void expect_default_current(const detail::NativePlayerCaptureProbe& current)
{
	EXPECT_FALSE(current.available);
	EXPECT_EQ(detail::CaptureStatus::InvalidSource, current.result.status);
	EXPECT_EQ(detail::CaptureReason::InvalidObservationKey, current.result.reason);
	expect_observation(current.observation, {});
}

void expect_zero_materialization(const detail::SessionPlayerMaterializationResult& result)
{
	EXPECT_EQ(0U, result.eligible_slots);
	EXPECT_EQ(0U, result.materialized_existing_slots);
	EXPECT_EQ(0U, result.materialized_new_slots);
	EXPECT_EQ(0U, result.no_player_slots);
	EXPECT_EQ(0U, result.invalid_source_slots);
	EXPECT_EQ(0U, result.invalid_capture_slots);
	EXPECT_EQ(0U, result.closed_exhausted_slots);
}

void expect_materialization(const detail::SessionPlayerMaterializationResult& result,
	std::size_t eligible,
	std::size_t existing,
	std::size_t created,
	std::size_t no_player,
	std::size_t invalid_source,
	std::size_t invalid_capture,
	std::size_t exhausted)
{
	EXPECT_EQ(eligible, result.eligible_slots);
	EXPECT_EQ(existing, result.materialized_existing_slots);
	EXPECT_EQ(created, result.materialized_new_slots);
	EXPECT_EQ(no_player, result.no_player_slots);
	EXPECT_EQ(invalid_source, result.invalid_source_slots);
	EXPECT_EQ(invalid_capture, result.invalid_capture_slots);
	EXPECT_EQ(exhausted, result.closed_exhausted_slots);
}

std::uint64_t independent_period_us(std::uint32_t flight_hz) noexcept
{
	return 1'000'000U / flight_hz + (1'000'000U % flight_hz != 0U ? 1U : 0U);
}

detail::NativeSessionTickStatus
capture_tick(NativeFixture& fixture, CountingEngineReadView& view, std::uint64_t now_us, bool active = true) noexcept
{
	return NativePlayerProbe::service_tick(fixture.runtime, {now_us, 0U, active}, view);
}

const detail::SessionControllerSlot* slot(const NativeFixture& fixture, std::size_t index = 0U) noexcept
{
	return NativePlayerAccess::slot(fixture.runtime, index);
}

template <typename Runtime>
void instantiate_exact_native_player_contract_asserts()
{
	if constexpr (detail::HasNativePlayerCapturePublicContract<Runtime>::value) {
		static_assert(detail::HasNativePlayerCapturePublicContract<Runtime>::value,
			"D3 accessors, named enum ordinals, CurrentPlayerCapture shape and noexcept returns must be exact.");
	}
	static_assert(std::is_same_v<decltype(NativePlayerAccess::slot(
			std::declval<const Runtime&>(), std::declval<std::size_t>())),
		const detail::SessionControllerSlot*>);
	static_assert(std::is_same_v<decltype(NativePlayerAccess::seed_last_allocated_entity_id(
			std::declval<Runtime&>(), std::declval<std::size_t>(), std::declval<std::uint64_t>())),
		bool>);
	static_assert(std::is_same_v<decltype(NativePlayerAccess::inject_collected_player_capture(
			std::declval<Runtime&>(),
			std::declval<const detail::CaptureResult&>(),
			std::declval<const detail::PlayerObservationDto&>())),
		detail::NativeSessionTickStatus>);
}

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

TEST(TelemetryNativePlayerCaptureContract, ExactPublicContractDefaultsAndTestOnlySeamShape)
{
	instantiate_exact_native_player_contract_asserts<detail::NativeSessionRuntime>();
	REQUIRE_NATIVE_PLAYER_D3();
	expect_default_current(NativePlayerProbe::declared_default_current());
	NativeFixture fixture;
	expect_default_current(NativePlayerProbe::current(fixture.runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
	EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture.runtime));
	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable,
		NativePlayerAccess::inject_collected_player_capture(fixture.runtime,
			{detail::CaptureStatus::Valid, detail::CaptureReason::None},
			observation(42U, 1U)));
}

TEST(TelemetryNativePlayerCaptureContract, FlightRateStartBoundariesRejectBeforeOpenAndAcceptOneAndSixty)
{
	REQUIRE_NATIVE_PLAYER_D3();
	for (const auto rates : {std::pair<unsigned, unsigned>{0U, 1U},
			 std::pair<unsigned, unsigned>{61U, 60U}}) {
		NativeFixture fixture;
		auto config = enabled_config();
		config.flight_hz = static_cast<std::uint8_t>(rates.first);
		EXPECT_EQ(detail::NativeSessionStartStatus::InvalidConfiguration, fixture.start(config));
		EXPECT_EQ(0U, fixture.backend.open_calls);
		EXPECT_EQ(0U, fixture.runtime.socket_count());
		EXPECT_EQ(0U, fixture.runtime.active_sessions());
		EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture.runtime));
		expect_default_current(NativePlayerProbe::current(fixture.runtime));
		expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
		config.flight_hz = static_cast<std::uint8_t>(rates.second);
		ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
		CountingEngineReadView view;
		EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(fixture, view, 123'456U));
		EXPECT_EQ(1U, view.read_calls);
		EXPECT_EQ(CaptureValid, NativePlayerProbe::last_status(fixture.runtime));
	}
}

TEST(TelemetryNativePlayerCaptureContract, TransportOpenFailureIsColdRetryableAndFirstCaptureIsImmediate)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	fixture.backend.open_statuses = {detail::SocketOpenStatus::SocketCreationFailed,
		detail::SocketOpenStatus::Complete};
	auto config = enabled_config();
	EXPECT_EQ(detail::NativeSessionStartStatus::TransportUnavailable, fixture.start(config));
	EXPECT_EQ(0U, fixture.runtime.socket_count());
	EXPECT_EQ(0U, fixture.runtime.active_sessions());
	EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture.runtime));
	expect_default_current(NativePlayerProbe::current(fixture.runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	CountingEngineReadView view;
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(fixture, view, 777'777U));
	EXPECT_EQ(1U, view.read_calls);
	EXPECT_EQ(CaptureValid, NativePlayerProbe::last_status(fixture.runtime));
}

TEST(TelemetryNativePlayerCaptureContract, AppliedAckPrecedesSameDueTickMaterialization)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	const auto endpoint = peer(21U);
	fixture.backend.receives.push_back({detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 901U, endpoint)});
	fixture.runtime.service_tick({10'000U, 0U, false});
	ASSERT_FALSE(fixture.backend.sent.empty());
	const auto welcome_index = fixture.backend.sent.size() - 1U;
	fixture.backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(fixture.backend.sent[welcome_index], endpoint, 902U)});
	std::size_t begin_index = fixture.backend.sent.size();
	for (std::uint64_t tick = 0U; tick < 32U && begin_index == fixture.backend.sent.size(); ++tick) {
		fixture.runtime.service_tick({10'010U + tick, 0U, false});
		for (std::size_t index = welcome_index + 1U; index < fixture.backend.sent.size(); ++index) {
			if (sent_type(fixture.backend, index) == protocol::MessageType::SessionBegin) {
				begin_index = index;
				break;
			}
		}
	}
	ASSERT_LT(begin_index, fixture.backend.sent.size());
	ASSERT_EQ(protocol::MessageType::SessionBegin, sent_type(fixture.backend, begin_index));
	fixture.backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(fixture.backend.sent[begin_index], endpoint, 903U)});
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(fixture, view, 20'000U));
	ASSERT_EQ(1U, view.read_calls);
	const auto* ready = slot(fixture);
	ASSERT_NE(nullptr, ready);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, ready->progress);
	EXPECT_TRUE(ready->has_latest_player_sample);
	EXPECT_EQ(1U, ready->latest_player_sample.entity_id);
	expect_materialization(NativePlayerProbe::materialization(fixture.runtime), 1U, 0U, 1U, 0U, 0U, 0U, 0U);
	const auto due_current = NativePlayerProbe::current(fixture.runtime);
	const auto due_sample = ready->latest_player_sample;
	view.clear_counts();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(fixture, view, 20'001U));
	EXPECT_EQ(0U, view.total_calls());
	EXPECT_EQ(CaptureNotDue, NativePlayerProbe::last_status(fixture.runtime));
	const auto not_due_current = NativePlayerProbe::current(fixture.runtime);
	EXPECT_EQ(due_current.available, not_due_current.available);
	EXPECT_EQ(due_current.result.status, not_due_current.result.status);
	EXPECT_EQ(due_current.result.reason, not_due_current.result.reason);
	expect_observation(not_due_current.observation, due_current.observation);
	expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
	ASSERT_NE(nullptr, slot(fixture));
	ASSERT_TRUE(slot(fixture)->has_latest_player_sample);
	expect_player_sample(slot(fixture)->latest_player_sample, due_sample);

	NativeFixture not_due;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, not_due.start(config));
	CountingEngineReadView arm;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(not_due, arm, 30'000U));
	const auto preserved_current = NativePlayerProbe::current(not_due.runtime);
	expect_zero_materialization(NativePlayerProbe::materialization(not_due.runtime));
	arm.clear_counts();
	const auto not_due_endpoint = peer(22U);
	not_due.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 904U, not_due_endpoint)});
	not_due.runtime.service_tick({30'001U, 0U, false});
	ASSERT_FALSE(not_due.backend.sent.empty());
	const auto not_due_welcome = not_due.backend.sent.size() - 1U;
	not_due.backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(not_due.backend.sent[not_due_welcome], not_due_endpoint, 905U)});
	for (std::uint64_t tick = 0U;
		tick < 8U && not_due.backend.sent.size() == not_due_welcome + 1U;
		++tick) {
		not_due.runtime.service_tick({30'002U + tick, 0U, false});
	}
	ASSERT_GT(not_due.backend.sent.size(), not_due_welcome + 1U);
	const auto not_due_begin = not_due.backend.sent.size() - 1U;
	not_due.backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(not_due.backend.sent[not_due_begin], not_due_endpoint, 906U)});
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(not_due, arm, 30'020U));
	EXPECT_EQ(0U, arm.total_calls());
	ASSERT_NE(nullptr, slot(not_due));
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(not_due)->progress);
	EXPECT_EQ(CaptureNotDue, NativePlayerProbe::last_status(not_due.runtime));
	const auto current_after_ack = NativePlayerProbe::current(not_due.runtime);
	EXPECT_EQ(preserved_current.available, current_after_ack.available);
	EXPECT_EQ(preserved_current.result.status, current_after_ack.result.status);
	EXPECT_EQ(preserved_current.result.reason, current_after_ack.result.reason);
	expect_observation(current_after_ack.observation, preserved_current.observation);
	expect_zero_materialization(NativePlayerProbe::materialization(not_due.runtime));
}

TEST(TelemetryNativePlayerCaptureContract, OneSharedCaptureFansOutToReadyAndStaleSlots)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config(64U);
	config.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	(void)establish_ready(fixture, peer(23U), 910U, 100'000U, 910U);
	(void)establish_ready(fixture, peer(24U), 920U, 200'000U, 920U);
	const auto* first = slot(fixture, 0U);
	const auto* second = slot(fixture, 1U);
	ASSERT_NE(nullptr, first);
	ASSERT_NE(nullptr, second);
	ASSERT_LT(first->heartbeat.last_valid_network_activity_us,
		second->heartbeat.last_valid_network_activity_us);
	const auto first_stale_due = first->heartbeat.last_valid_network_activity_us +
		first->heartbeat.stale_timeout_us;
	const auto second_stale_due = second->heartbeat.last_valid_network_activity_us +
		second->heartbeat.stale_timeout_us;
	ASSERT_LT(first_stale_due, second_stale_due);
	fixture.runtime.service_tick({first_stale_due, 0U, true});
	ASSERT_EQ(detail::ProducerSessionProgress::Stale, slot(fixture, 0U)->progress);
	ASSERT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(fixture, 1U)->progress);
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		capture_tick(fixture, view, first_stale_due));
	EXPECT_EQ(1U, view.in_mission_calls);
	EXPECT_EQ(1U, view.read_calls);
	expect_materialization(NativePlayerProbe::materialization(fixture.runtime), 2U, 0U, 2U, 0U, 0U, 0U, 0U);
	for (std::size_t index = 0U; index < 2U; ++index) {
		const auto* current_slot = slot(fixture, index);
		ASSERT_NE(nullptr, current_slot);
		EXPECT_TRUE(current_slot->has_latest_player_sample);
		EXPECT_EQ(1U, current_slot->latest_player_sample.entity_id);
		EXPECT_EQ(first_stale_due, current_slot->latest_player_sample.value.producer_sample_time_us);
	}
	view.input.position_world.x = 99.0f;
	EXPECT_FLOAT_EQ(1.0f, slot(fixture, 0U)->latest_player_sample.value.position_world.x);
	EXPECT_FLOAT_EQ(1.0f, slot(fixture, 1U)->latest_player_sample.value.position_world.x);
}

TEST(TelemetryNativePlayerCaptureContract, CadenceSkipsCatchUpAndAppliesExactlyOncePerDueTick)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	config.flight_hz = 30U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	(void)establish_ready(fixture, peer(25U), 930U, 1'000U, 930U);
	CountingEngineReadView view;
	const auto period = independent_period_us(config.flight_hz);
	const std::uint64_t first = 50'000U;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(fixture, view, first));
	EXPECT_EQ(1U, view.read_calls);
	expect_materialization(NativePlayerProbe::materialization(fixture.runtime), 1U, 0U, 1U, 0U, 0U, 0U, 0U);
	(void)capture_tick(fixture, view, first + period - 1U);
	EXPECT_EQ(1U, view.read_calls);
	const auto hitch = first + period * 10U;
	(void)capture_tick(fixture, view, hitch);
	EXPECT_EQ(2U, view.read_calls);
	expect_materialization(NativePlayerProbe::materialization(fixture.runtime), 1U, 1U, 0U, 0U, 0U, 0U, 0U);
	(void)capture_tick(fixture, view, hitch);
	(void)capture_tick(fixture, view, hitch + period - 1U);
	EXPECT_EQ(2U, view.read_calls);
	(void)capture_tick(fixture, view, hitch + period);
	EXPECT_EQ(3U, view.read_calls);
	EXPECT_EQ(1U, slot(fixture)->latest_player_sample.entity_id);
}

TEST(TelemetryNativePlayerCaptureContract, InactiveClearsCurrentPlayerWithoutReadingAndReactivationAllocatesNewId)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	(void)establish_ready(fixture, peer(32U), 991U, 1'000U, 991U);
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(fixture, view, 10'000U, true));
	ASSERT_TRUE(slot(fixture)->has_latest_player_sample);
	ASSERT_EQ(1U, slot(fixture)->latest_player_sample.entity_id);
	view.clear_counts();
	(void)capture_tick(fixture, view, 20'000U, false);
	(void)capture_tick(fixture, view, 9'000'000U, false);
	EXPECT_EQ(0U, view.total_calls());
	EXPECT_EQ(CaptureInactive, NativePlayerProbe::last_status(fixture.runtime));
	expect_default_current(NativePlayerProbe::current(fixture.runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
	ASSERT_FALSE(slot(fixture)->has_latest_player_sample);
	EXPECT_EQ(1U, slot(fixture)->player_entity_ids.last_allocated_entity_id());
	(void)capture_tick(fixture, view, 10'000'000U, true);
	EXPECT_EQ(1U, view.read_calls);
	EXPECT_EQ(CaptureValid, NativePlayerProbe::last_status(fixture.runtime));
	ASSERT_TRUE(slot(fixture)->has_latest_player_sample);
	EXPECT_EQ(2U, slot(fixture)->latest_player_sample.entity_id);
	const auto calls = view.total_calls();
	(void)capture_tick(fixture, view, 10'000'000U + independent_period_us(config.flight_hz) - 1U, true);
	EXPECT_EQ(calls, view.total_calls());
	EXPECT_EQ(CaptureNotDue, NativePlayerProbe::last_status(fixture.runtime));
}

TEST(TelemetryNativePlayerCaptureContract, LegalAbsenceAndInvalidSourceClearAndReappearanceGetsNewIds)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	config.flight_hz = 60U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	(void)establish_ready(fixture, peer(26U), 940U, 1'000U, 940U);
	CountingEngineReadView view;
	const auto period = independent_period_us(config.flight_hz);
	std::uint64_t now = 100'000U;
	(void)capture_tick(fixture, view, now);
	ASSERT_EQ(1U, slot(fixture)->latest_player_sample.entity_id);
	view.player = false;
	(void)capture_tick(fixture, view, now += period);
	EXPECT_EQ(CaptureNoPlayer, NativePlayerProbe::last_status(fixture.runtime));
	const auto no_player = NativePlayerProbe::current(fixture.runtime);
	EXPECT_TRUE(no_player.available);
	EXPECT_EQ(detail::CaptureStatus::NoPlayer, no_player.result.status);
	expect_observation(no_player.observation, {});
	EXPECT_FALSE(slot(fixture)->has_latest_player_sample);
	expect_materialization(NativePlayerProbe::materialization(fixture.runtime), 1U, 0U, 0U, 1U, 0U, 0U, 0U);
	view.player = true;
	(void)capture_tick(fixture, view, now += period);
	ASSERT_EQ(2U, slot(fixture)->latest_player_sample.entity_id);
	view.object_is_ship = false;
	(void)capture_tick(fixture, view, now += period);
	EXPECT_EQ(CaptureInvalidSource, NativePlayerProbe::last_status(fixture.runtime));
	const auto invalid = NativePlayerProbe::current(fixture.runtime);
	EXPECT_TRUE(invalid.available);
	EXPECT_EQ(detail::CaptureStatus::InvalidSource, invalid.result.status);
	expect_observation(invalid.observation, {});
	EXPECT_FALSE(slot(fixture)->has_latest_player_sample);
	expect_materialization(NativePlayerProbe::materialization(fixture.runtime), 1U, 0U, 0U, 0U, 1U, 0U, 0U);
	view.object_is_ship = true;
	(void)capture_tick(fixture, view, now += period);
	EXPECT_EQ(3U, slot(fixture)->latest_player_sample.entity_id);
}

TEST(TelemetryNativePlayerCaptureContract, ExhaustiveCaptureResultPairsAcceptOnlyClosedContract)
{
	REQUIRE_NATIVE_PLAYER_D3();
	for (std::uint8_t status_value = 0U; status_value <= static_cast<std::uint8_t>(detail::CaptureStatus::Count);
		 ++status_value) {
		for (std::uint8_t reason_value = 0U; reason_value <= static_cast<std::uint8_t>(detail::CaptureReason::Count);
			 ++reason_value) {
			NativeFixture fixture;
			auto config = enabled_config();
			ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
			(void)establish_ready(fixture, peer(27U), 950U, 1'000U, 950U);
			const auto valid = detail::CaptureResult{detail::CaptureStatus::Valid, detail::CaptureReason::None};
			const auto seeded = observation(42U, 10U, 2.0f);
			const auto status = static_cast<detail::CaptureStatus>(status_value);
			const auto reason = static_cast<detail::CaptureReason>(reason_value);
			const bool valid_pair = status == detail::CaptureStatus::Valid && reason == detail::CaptureReason::None;
			const bool no_player_pair = status == detail::CaptureStatus::NoPlayer &&
										reason >= detail::CaptureReason::NotInMission &&
										reason <= detail::CaptureReason::MissingPlayerShip;
			const bool invalid_pair = status == detail::CaptureStatus::InvalidSource &&
									  reason >= detail::CaptureReason::WrongObjectType &&
									  reason < detail::CaptureReason::Count;
			if (!valid_pair) {
				ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
					NativePlayerAccess::inject_collected_player_capture(fixture.runtime, valid, seeded));
			}
			const detail::CaptureResult candidate{status, reason};
			const auto result = NativePlayerAccess::inject_collected_player_capture(fixture.runtime,
				candidate,
				observation(77U, 20U, 9.0f));
			if (valid_pair || no_player_pair || invalid_pair) {
				EXPECT_EQ(detail::NativeSessionTickStatus::Complete, result);
				const auto current = NativePlayerProbe::current(fixture.runtime);
				EXPECT_TRUE(current.available);
				EXPECT_EQ(status, current.result.status);
				EXPECT_EQ(reason, current.result.reason);
				EXPECT_EQ(valid_pair       ? CaptureValid
						  : no_player_pair ? CaptureNoPlayer
										   : CaptureInvalidSource,
					NativePlayerProbe::last_status(fixture.runtime));
				if (valid_pair) {
					expect_observation(current.observation, observation(77U, 20U, 9.0f));
					EXPECT_TRUE(slot(fixture)->has_latest_player_sample);
				} else {
					expect_observation(current.observation, {});
					EXPECT_FALSE(slot(fixture)->has_latest_player_sample);
				}
				EXPECT_EQ(1U, fixture.runtime.socket_count());
			} else {
				EXPECT_EQ(TickPermanentCaptureFailure, static_cast<std::uint8_t>(result));
				EXPECT_EQ(CaptureInvariantFailure, NativePlayerProbe::last_status(fixture.runtime));
				expect_default_current(NativePlayerProbe::current(fixture.runtime));
				expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
				EXPECT_EQ(0U, fixture.runtime.active_sessions());
				EXPECT_EQ(0U, fixture.runtime.socket_count());
				EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable,
					NativePlayerAccess::inject_collected_player_capture(fixture.runtime, valid, seeded));
				CountingEngineReadView fault_view;
				EXPECT_EQ(TickPermanentCaptureFailure,
					static_cast<std::uint8_t>(capture_tick(fixture, fault_view, 30'000U)));
				EXPECT_EQ(TickPermanentCaptureFailure,
					static_cast<std::uint8_t>(fixture.runtime.service_tick({30'001U, 0U, true})));
				EXPECT_EQ(0U, fault_view.total_calls());
			}
		}
	}
}

TEST(TelemetryNativePlayerCaptureContract, ClockRegressionFailsClosedBeforeReadingAndStaysSticky)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	(void)establish_ready(fixture, peer(28U), 960U, 1'000U, 960U);
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(fixture, view, 100'000U));
	view.clear_counts();
	EXPECT_EQ(TickPermanentCaptureFailure, static_cast<std::uint8_t>(capture_tick(fixture, view, 99'999U)));
	EXPECT_EQ(0U, view.total_calls());
	EXPECT_EQ(CaptureCadenceFailure, NativePlayerProbe::last_status(fixture.runtime));
	EXPECT_EQ(0U, fixture.runtime.active_sessions());
	EXPECT_EQ(0U, fixture.runtime.socket_count());
	expect_default_current(NativePlayerProbe::current(fixture.runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
	EXPECT_EQ(TickPermanentCaptureFailure, static_cast<std::uint8_t>(capture_tick(fixture, view, 200'000U)));
	EXPECT_EQ(0U, view.total_calls());
}

TEST(TelemetryNativePlayerCaptureContract, DeadlineOverflowFailsClosedBeforeReadingAndStaysSticky)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	CountingEngineReadView view;
	EXPECT_EQ(TickPermanentCaptureFailure,
		static_cast<std::uint8_t>(capture_tick(fixture, view, std::numeric_limits<std::uint64_t>::max())));
	EXPECT_EQ(0U, view.total_calls());
	EXPECT_EQ(CaptureCadenceFailure, NativePlayerProbe::last_status(fixture.runtime));
	EXPECT_EQ(0U, fixture.runtime.socket_count());
	expect_default_current(NativePlayerProbe::current(fixture.runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
	EXPECT_EQ(TickPermanentCaptureFailure, static_cast<std::uint8_t>(capture_tick(fixture, view, 1U)));
	EXPECT_EQ(0U, view.total_calls());
}

TEST(TelemetryNativePlayerCaptureContract, EntityIdExhaustionClosesOnlyAffectedSlotAndTickCompletes)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	config.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	(void)establish_ready(fixture, peer(29U), 970U, 1'000U, 970U);
	(void)establish_ready(fixture, peer(30U), 980U, 2'000U, 980U);
	ASSERT_TRUE(NativePlayerAccess::seed_last_allocated_entity_id(fixture.runtime,
		0U,
		std::numeric_limits<std::uint64_t>::max()));
	CountingEngineReadView view;
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(fixture, view, 100'000U));
	EXPECT_EQ(1U, view.read_calls);
	expect_materialization(NativePlayerProbe::materialization(fixture.runtime), 2U, 0U, 1U, 0U, 0U, 0U, 1U);
	EXPECT_EQ(1U, fixture.runtime.active_sessions());
	EXPECT_EQ(detail::ProducerSessionProgress::Empty, slot(fixture, 0U)->progress);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(fixture, 1U)->progress);
	EXPECT_EQ(1U, slot(fixture, 1U)->latest_player_sample.entity_id);
	EXPECT_EQ(1U, fixture.runtime.socket_count());
}

TEST(TelemetryNativePlayerCaptureContract, PurgeClearsAndRearmsAtIndependentExactBoundaries)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	config.flight_hz = 30U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	CountingEngineReadView view;
	(void)capture_tick(fixture, view, 10'000U);
	fixture.runtime.purge_all(detail::SessionCloseReason::MissionDiscontinuity);
	EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture.runtime));
	expect_default_current(NativePlayerProbe::current(fixture.runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
	view.clear_counts();
	const std::uint64_t t2 = 987'654U;
	const auto period = independent_period_us(config.flight_hz);
	(void)capture_tick(fixture, view, t2);
	EXPECT_EQ(1U, view.read_calls);
	(void)capture_tick(fixture, view, t2 + period - 1U);
	EXPECT_EQ(1U, view.read_calls);
	EXPECT_EQ(CaptureNotDue, NativePlayerProbe::last_status(fixture.runtime));
	(void)capture_tick(fixture, view, t2 + period);
	EXPECT_EQ(2U, view.read_calls);
}

TEST(TelemetryNativePlayerCaptureContract, ShutdownClearsAndAllCaptureSeamsStayUnavailable)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	CountingEngineReadView view;
	(void)capture_tick(fixture, view, 10'000U);
	fixture.runtime.shutdown();
	EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture.runtime));
	expect_default_current(NativePlayerProbe::current(fixture.runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
	view.clear_counts();
	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable, capture_tick(fixture, view, 20'000U));
	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable, fixture.runtime.service_tick({20'001U, 0U, true}));
	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable,
		NativePlayerAccess::inject_collected_player_capture(fixture.runtime,
			{detail::CaptureStatus::Valid, detail::CaptureReason::None},
			observation(42U, 20'000U)));
	EXPECT_EQ(0U, view.total_calls());
}

TEST(TelemetryNativePlayerCaptureContract, OneArgumentCompatibilityPreservesPlayerStateWhileR2Progresses)
{
	REQUIRE_NATIVE_PLAYER_D3();
	NativeFixture fixture;
	auto config = enabled_config();
	config.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
	const auto ready_endpoint = peer(31U);
	const auto ready_session_id = establish_ready(fixture, ready_endpoint, 990U, 1'000U, 990U);
	CountingEngineReadView view;
	(void)capture_tick(fixture, view, 100'000U);
	const auto status_before = NativePlayerProbe::last_status(fixture.runtime);
	const auto current_before = NativePlayerProbe::current(fixture.runtime);
	const auto materialization_before = NativePlayerProbe::materialization(fixture.runtime);
	const auto sample_before = slot(fixture)->latest_player_sample;
	auto expect_preserved = [&] {
		EXPECT_EQ(status_before, NativePlayerProbe::last_status(fixture.runtime));
		const auto current_after = NativePlayerProbe::current(fixture.runtime);
		EXPECT_EQ(current_before.available, current_after.available);
		EXPECT_EQ(current_before.result.status, current_after.result.status);
		EXPECT_EQ(current_before.result.reason, current_after.result.reason);
		expect_observation(current_after.observation, current_before.observation);
		expect_materialization(NativePlayerProbe::materialization(fixture.runtime),
			materialization_before.eligible_slots,
			materialization_before.materialized_existing_slots,
			materialization_before.materialized_new_slots,
			materialization_before.no_player_slots,
			materialization_before.invalid_source_slots,
			materialization_before.invalid_capture_slots,
			materialization_before.closed_exhausted_slots);
		ASSERT_NE(nullptr, slot(fixture));
		ASSERT_TRUE(slot(fixture)->has_latest_player_sample);
		expect_player_sample(slot(fixture)->latest_player_sample, sample_before);
	};
	const auto period = independent_period_us(config.flight_hz);
	const auto activity_before = slot(fixture)->heartbeat.last_valid_network_activity_us;
	const auto clock_response_before = slot(fixture)->heartbeat.last_valid_clock_response_us;
	const std::uint64_t heartbeat_tick = 100'001U;
	fixture.backend.receives.push_back(
		{detail::IoStatus::Complete, heartbeat_request(ready_session_id, ready_endpoint, 991U, heartbeat_tick - 1U)});
	const auto sends_before_heartbeat = fixture.backend.sent.size();
	fixture.runtime.service_tick({heartbeat_tick, 0U, true});
	ASSERT_GT(fixture.backend.sent.size(), sends_before_heartbeat);
	EXPECT_EQ(protocol::MessageType::Heartbeat,
		sent_type(fixture.backend, fixture.backend.sent.size() - 1U));
	ASSERT_NE(nullptr, slot(fixture));
	EXPECT_LT(activity_before, slot(fixture)->heartbeat.last_valid_network_activity_us);
	EXPECT_EQ(heartbeat_tick, slot(fixture)->heartbeat.last_valid_network_activity_us);
	EXPECT_EQ(clock_response_before, slot(fixture)->heartbeat.last_valid_clock_response_us)
		<< "A valid client Request refreshes activity and queues Heartbeat Response; it is not a clock-response sample.";
	expect_preserved();

	const auto second_endpoint = peer(33U);
	const auto sends_before_second_hello = fixture.backend.sent.size();
	fixture.backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 992U, second_endpoint)});
	fixture.runtime.service_tick({heartbeat_tick + 1U, 0U, false});
	expect_preserved();
	ASSERT_GT(fixture.backend.sent.size(), sends_before_second_hello);
	const auto second_welcome = fixture.backend.sent.size() - 1U;
	EXPECT_EQ(protocol::MessageType::Welcome, sent_type(fixture.backend, second_welcome));
	ASSERT_NE(nullptr, slot(fixture, 1U));
	EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, slot(fixture, 1U)->progress);
	EXPECT_EQ(2U, fixture.runtime.active_sessions());

	const auto sends_before_second_welcome_ack = fixture.backend.sent.size();
	fixture.backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(fixture.backend.sent[second_welcome], second_endpoint, 993U)});
	const auto second_welcome_ack_script_index = fixture.backend.receives.size() - 1U;
	fixture.runtime.service_tick({heartbeat_tick + 2U, 0U, false});
	expect_preserved();
	EXPECT_GT(fixture.backend.receive_script_index, second_welcome_ack_script_index)
		<< "The one-argument overload must ingest the real WELCOME ACK before later egress.";
	ASSERT_NE(nullptr, slot(fixture, 1U));
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(fixture, 1U)->progress)
		<< "Applying the WELCOME proof is the observable R2 handshake transition.";
	for (std::uint64_t offset = 0U;
		offset < 8U && fixture.backend.sent.size() == sends_before_second_welcome_ack;
		++offset) {
		fixture.runtime.service_tick({heartbeat_tick + 3U + offset, 0U, false});
		expect_preserved();
	}
	ASSERT_GT(fixture.backend.sent.size(), sends_before_second_welcome_ack);
	EXPECT_EQ(protocol::MessageType::SessionBegin,
		sent_type(fixture.backend, fixture.backend.sent.size() - 1U));
	ASSERT_NE(nullptr, slot(fixture, 1U));
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(fixture, 1U)->progress);

	fixture.runtime.service_tick({100'000U + period * 2U, 0U, true});
	expect_preserved();
	fixture.runtime.service_tick({100'000U + period * 4U, 0U, true});
	expect_preserved();

	const auto disconnect_due = slot(fixture)->heartbeat.last_valid_network_activity_us +
		slot(fixture)->heartbeat.disconnect_timeout_us;
	fixture.runtime.service_tick({disconnect_due, 0U, true});
	ASSERT_NE(nullptr, slot(fixture));
	EXPECT_EQ(detail::ProducerSessionProgress::Empty, slot(fixture)->progress)
		<< "Only the real R2 maintenance deadline may clear the preserved player slot.";
	EXPECT_EQ(status_before, NativePlayerProbe::last_status(fixture.runtime));
	const auto current_after_close = NativePlayerProbe::current(fixture.runtime);
	EXPECT_EQ(current_before.available, current_after_close.available);
	EXPECT_EQ(current_before.result.status, current_after_close.result.status);
	EXPECT_EQ(current_before.result.reason, current_after_close.result.reason);
	expect_observation(current_after_close.observation, current_before.observation);
	expect_materialization(NativePlayerProbe::materialization(fixture.runtime),
		materialization_before.eligible_slots,
		materialization_before.materialized_existing_slots,
		materialization_before.materialized_new_slots,
		materialization_before.no_player_slots,
		materialization_before.invalid_source_slots,
		materialization_before.invalid_capture_slots,
		materialization_before.closed_exhausted_slots);
}

TEST(TelemetryNativePlayerCaptureContract, ClosedAndErrorTransportFailuresAreStickyAndPreemptCapture)
{
	REQUIRE_NATIVE_PLAYER_D3();
	for (const auto io_status : {detail::IoStatus::Closed, detail::IoStatus::Error}) {
		NativeFixture fixture;
		auto config = enabled_config();
		ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture.start(config));
		fixture.backend.receives.push_back({io_status, {}});
		CountingEngineReadView view;
		EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure, capture_tick(fixture, view, 100'000U));
		EXPECT_EQ(0U, view.total_calls());
		EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture.runtime));
		expect_default_current(NativePlayerProbe::current(fixture.runtime));
		expect_zero_materialization(NativePlayerProbe::materialization(fixture.runtime));
		EXPECT_EQ(0U, fixture.runtime.socket_count());
		EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure, capture_tick(fixture, view, 200'000U));
		EXPECT_EQ(0U, view.total_calls());
	}
}

#undef REQUIRE_NATIVE_PLAYER_D3

#endif

} // namespace
