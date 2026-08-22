#include "telemetry/runtime.h"

#if __has_include("telemetry/native_session_runtime.h")
#include "telemetry/native_session_runtime.h"
#include "telemetry/config.h"
#include "telemetry/engine_adapter.h"
#include "telemetry/phase2_catalog_projection.h"
#include "telemetry/phase3_engine_collector.h"
#include "telemetry/phase3_identity_registry.h"
#include "telemetry/phase3_state_image.h"
#include "telemetry/runtime_adapter.h"
#include "telemetry/runtime_adapter_test_seam.h"
#include "telemetry/phase1_state_image.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/session_controller.h"
#include "telemetry/startup_budget.h"
#include "telemetry/transport.h"
#include "ai/ai.h"
#include "autopilot/autopilot.h"
#include "globalincs/systemvars.h"
#include "gamesequence/gamesequence.h"
#include "hud/hud.h"
#include "hud/hudconfig.h"
#include "iff_defs/iff_defs.h"
#include "object/object.h"
#include "playerman/player.h"
#include "radar/radarsetup.h"
#include "ship/ship.h"
#include "weapon/weapon.h"
#include "telemetry_native_session_runtime_player_test_access.h"
#include "telemetry_runtime_adapter_player_test_access.h"
#define FSO_HAS_NATIVE_SESSION_RUNTIME 1
#else
#define FSO_HAS_NATIVE_SESSION_RUNTIME 0
#endif

#include <gtest/gtest.h>

#include <iostream>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#endif

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
	detail::TelemetryStructuredLog log;
	detail::NativeSessionRuntime runtime;

	NativeFixture() : ids(ids_random, registry), runtime(backend, completion)
	{
		EXPECT_TRUE(registry.allocate_storage());
	}

	detail::NativeSessionStartStatus start(
		telemetry::TelemetryConfig& config)
	{
		detail::NativeSessionStartRequest request{
			&config,
			0x1020304050607080ULL,
			&ids,
			&packet_random,
			nullptr,
			&log};
		return runtime.start(request);
	}

	detail::NativeSessionStartStatus start_requested(telemetry::TelemetryConfig& config,
		telemetry::Phase2Profile requested_phase2_profile)
	{
		detail::NativeSessionStartRequest request{
			&config,
			0x1020304050607080ULL,
			&ids,
			&packet_random,
			nullptr,
			&log};
		request.phase2_eligibility = {};
		request.requested_phase2_profile = requested_phase2_profile;
		return runtime.start(request);
	}

	detail::NativeSessionStartStatus start_with_eligibility(
		telemetry::TelemetryConfig& config,
		const telemetry::Phase2ProfileEligibility& eligibility)
	{
		detail::NativeSessionStartRequest request{
			&config,
			0x1020304050607080ULL,
			&ids,
			&packet_random,
			nullptr,
			&log};
		request.phase2_eligibility = eligibility;
		request.requested_phase2_profile = telemetry::Phase2Profile::CoreGate;
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
		last_read_thread = std::this_thread::get_id();
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
	mutable std::thread::id last_read_thread{};
};

// This is intentionally limited to the authorized Phase 2 capture surface.
// Phase 3 itself still reads the live FS2 player globals below; the split keeps
// the cadence oracle from accidentally using a synthetic Phase 3 projection.
struct MinimalPhase2EngineReadView final : detail::Phase2EngineReadView {
	MinimalPhase2EngineReadView()
		: source(std::make_unique<detail::Phase2ShipSource>())
	{
		source->identity.class_source_key.value = 1U;
		source->raw_static_references.class_capture_key = 1U;
		source->raw_static_catalog.class_count = 1U;
		auto& ship_class = source->raw_static_catalog.class_definitions[0U];
		ship_class.class_capture_key = 1U;
		(void)ship_class.internal_name.assign("phase3-runtime-fixture");
		ship_class.model_mass = 100.0F;
		ship_class.density = 1.0F;
		ship_class.effective_mass = 100.0F;
		auto& alternate_ship_class = source->raw_static_catalog.class_definitions[1U];
		alternate_ship_class.class_capture_key = 2U;
		(void)alternate_ship_class.internal_name.assign("phase3-runtime-fixture-alternate");
		alternate_ship_class.model_mass = 200.0F;
		alternate_ship_class.density = 1.0F;
		alternate_ship_class.effective_mass = 200.0F;
		source->raw_static_catalog.class_count = 2U;
	}

	bool current_thread_is_main() const noexcept override { return true; }
	bool in_mission() const noexcept override { return true; }
	bool player_exists() const noexcept override { return true; }
	bool player_object_exists() const noexcept override { return true; }
	bool player_ship_exists() const noexcept override { return true; }
	bool player_source_is_consistent() const noexcept override { return true; }
	detail::SourceReadResult read_player_root_key(detail::EngineEntityKey& output) const noexcept override
	{
		output.object_signature = 42U;
		return {detail::Phase2SourceReadStatus::Valid};
	}
	detail::SourceReadResult read_discovery_node(
		detail::EngineEntityKey key, detail::Phase2DiscoveryNode& output) const noexcept override
	{
		if (key.object_signature != 42U) return {detail::Phase2SourceReadStatus::InvalidSource};
		output = {};
		output.capture_key.value = 42U;
		return {detail::Phase2SourceReadStatus::Valid};
	}
	detail::SourceReadResult read_ship(
		detail::EngineEntityKey key, detail::Phase2ShipSource& output) const noexcept override
	{
		if (key.object_signature != 42U) return {detail::Phase2SourceReadStatus::InvalidSource};
		output = *source;
		return {detail::Phase2SourceReadStatus::Valid};
	}
	detail::SourceReadResult read_ship_flight(
		detail::EngineEntityKey key, detail::ShipFlightObservation& output) const noexcept override
	{
		if (key.object_signature != 42U) return {detail::Phase2SourceReadStatus::InvalidSource};
		output = source->flight;
		return {detail::Phase2SourceReadStatus::Valid};
	}
	bool read_player_controls(detail::PlayerControlObservation& output) const noexcept override
	{
		output = controls;
		return true;
	}
	bool read_player_cargo_scan(detail::PlayerCargoScanObservation& output) const noexcept override
	{
		output = cargo;
		return true;
	}

	std::unique_ptr<detail::Phase2ShipSource> source;
	detail::PlayerControlObservation controls{};
	detail::PlayerCargoScanObservation cargo{};
};

// The collector must read real engine authorities.  Reserve high, otherwise
// unused slots and restore all pointer/global selections when the test exits.
struct Phase3EngineGlobalsScope final {
	static constexpr int ObjectIndex = MAX_OBJECTS - 2;
	static constexpr int ShipIndex = MAX_SHIPS - 2;
	static constexpr int AiIndex = MAX_AI_INFO - 2;

	Phase3EngineGlobalsScope()
		: prior_player(Player), prior_player_object(Player_obj),
		  prior_player_ship(Player_ship), prior_player_ai(Player_ai),
		  prior_game_mode(Game_mode), prior_current_nav(CurrentNav),
		  prior_see_all(See_all),
		  prior_radar_bright_range(Radar_bright_range),
		  prior_radar_bright_timer(Radar_calc_bright_dist_timer),
		  prior_autopilot_engaged(AutoPilotEngaged), prior_hud_config(HUD_config),
		  prior_ship_info_count(Ship_info.size()),
		  prior_used_list_next(obj_used_list.next), prior_used_list_prev(obj_used_list.prev),
		  prior_missile_list_next(Missile_obj_list.next),
		  prior_missile_list_prev(Missile_obj_list.prev),
		  prior_waypoint_lists(Waypoint_lists)
	{
		std::copy(std::begin(Navs), std::end(Navs), prior_navs.begin());
		for (auto& nav : Navs) nav.clear();
		Waypoint_lists.clear();
		Player = &local_player;
		Player_obj = &Objects[ObjectIndex];
		Player_ship = &Ships[ShipIndex];
		Player_ai = &Ai_info[AiIndex];
		Game_mode |= GM_IN_MISSION;
		CurrentNav = -1;
		AutoPilotEngaged = false;
		HUD_config.rp_dist = RR_SHORT;
		See_all = 0;

		Player_obj->clear();
		Player_obj->type = OBJ_SHIP;
		Player_obj->instance = ShipIndex;
		Player_obj->signature = 42;
		Player_obj->orient = vmd_identity_matrix;
		list_init(&obj_used_list);
		list_init(&Missile_obj_list);
		Player_ship->clear();
		if (Iff_info.empty()) {
			Iff_info.emplace_back();
			added_test_iff = true;
		}
		prior_iff_color_index = Iff_info[0].color_index;
		prior_iff_accessibility_color_index =
			Iff_info[0].accessibility_color_index;
		const auto test_iff_color = iff_init_color(64, 192, 255);
		Iff_info[0].color_index = test_iff_color;
		Iff_info[0].accessibility_color_index = test_iff_color;
		Player_ship->team = 0;
		list_init(&Player_ship->subsys_list);
		Player_ship->weapons.clear();
		Player_ship->objnum = ObjectIndex;
		Player_ship->ai_index = AiIndex;
		// object_get_eye() consults the player's ship class on every full radar
		// refresh. Give the shared runtime fixture a valid minimal class so tests
		// exercising a legal systems tick do not fail in unrelated model lookup.
		Ship_info.emplace_back();
		Player_ship->ship_info_index =
			static_cast<int>(prior_ship_info_count);
		*Player_ai = ai_info{};
		Player_ai->shipnum = ShipIndex;
		Player_ai->target_objnum = -1;
		Player_ai->previous_target_objnum = -1;
		Player_ai->attacker_objnum = -1;
		Player_ai->danger_weapon_objnum = -1;
		Player_ai->nearest_locked_object = -1;
		Radar_bright_range = 1'500.0F;
		Radar_calc_bright_dist_timer = TIMESTAMP::never();
	}

	~Phase3EngineGlobalsScope()
	{
		Player = prior_player;
		Player_obj = prior_player_object;
		Player_ship = prior_player_ship;
		Player_ai = prior_player_ai;
		Game_mode = prior_game_mode;
		CurrentNav = prior_current_nav;
		See_all = prior_see_all;
		Radar_bright_range = prior_radar_bright_range;
		Radar_calc_bright_dist_timer = prior_radar_bright_timer;
		AutoPilotEngaged = prior_autopilot_engaged;
		HUD_config = prior_hud_config;
		Ship_info.resize(prior_ship_info_count);
		obj_used_list.next = prior_used_list_next;
		obj_used_list.prev = prior_used_list_prev;
		Missile_obj_list.next = prior_missile_list_next;
		Missile_obj_list.prev = prior_missile_list_prev;
		std::copy(prior_navs.begin(), prior_navs.end(), std::begin(Navs));
		Waypoint_lists = std::move(prior_waypoint_lists);
		if (added_test_iff) {
			Iff_info.clear();
		} else {
			Iff_info[0].color_index = prior_iff_color_index;
			Iff_info[0].accessibility_color_index =
				prior_iff_accessibility_color_index;
		}
	}

	::player local_player{};
	player* prior_player = nullptr;
	object* prior_player_object = nullptr;
	ship* prior_player_ship = nullptr;
	ai_info* prior_player_ai = nullptr;
	int prior_game_mode = 0;
	int prior_current_nav = -1;
	int prior_see_all = 0;
	float prior_radar_bright_range = 0.0F;
	TIMESTAMP prior_radar_bright_timer = TIMESTAMP::never();
	bool prior_autopilot_engaged = false;
	HUD_CONFIG_TYPE prior_hud_config{};
	std::size_t prior_ship_info_count = 0U;
	object* prior_used_list_next = nullptr;
	object* prior_used_list_prev = nullptr;
	missile_obj* prior_missile_list_next = nullptr;
	missile_obj* prior_missile_list_prev = nullptr;
	std::array<NavPoint, MAX_NAVPOINTS> prior_navs{};
	SCP_vector<waypoint_list> prior_waypoint_lists;
	bool added_test_iff = false;
	int prior_iff_color_index = 0;
	int prior_iff_accessibility_color_index = 0;
};

std::uint64_t phase3_sample_time(const protocol::StateImage& image,
	protocol::RecordType type)
{
	for (const auto& atom : image.records()) {
		if (atom.key.record_type != static_cast<std::uint16_t>(type)) continue;
		EXPECT_GE(atom.value.size(), 24U);
		if (atom.value.size() < 24U) return 0U;
		std::uint64_t value = 0U;
		for (std::size_t offset = 0U; offset < 8U; ++offset)
			value |= static_cast<std::uint64_t>(atom.value[16U + offset]) << (offset * 8U);
		return value;
	}
	ADD_FAILURE() << "Missing Phase 3 singleton record " << static_cast<std::uint16_t>(type);
	return 0U;
}

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
	return NativePlayerProbe::service_tick(fixture.runtime, {now_us, active ? 1U : 0U, active}, view);
}

detail::NativeSessionTickStatus native_tick(NativeFixture& fixture,
	const detail::NativeSessionTickContext& context) noexcept
{
	return NativePlayerAccess::service_r2_tick(fixture.runtime, context);
}

const detail::SessionControllerSlot* slot(const NativeFixture& fixture, std::size_t index = 0U) noexcept
{
	return NativePlayerAccess::slot(fixture.runtime, index);
}

Packet pop_controller_packet(detail::SessionController& controller)
{
	detail::SessionControllerOutput output;
	EXPECT_TRUE(controller.pop_output(output));
	Packet result;
	result.endpoint = output.endpoint;
	result.bytes.assign(output.bytes.begin(), output.bytes.begin() + static_cast<std::ptrdiff_t>(output.size));
	return result;
}

protocol::StateImage phase1_state_image_at(float player_x, std::uint64_t sample_time_us)
{
	detail::Phase1StateImageInput input{};
	input.producer_id = 0x1020304050607080ULL;
	input.negotiated_capability_generation = 1U;
	input.mission.producer_sample_time_us = sample_time_us;
	input.mission.mission_generation = 7U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {detail::CaptureStatus::Valid, detail::CaptureReason::None};
	input.player.entity_id = 42U;
	input.player.value.producer_sample_time_us = sample_time_us;
	input.player.value.position_world = {player_x, 2.0F, 3.0F};
	input.player.value.orientation_local_to_world = {1.0F, 0.0F, 0.0F, 0.0F};
	input.player.value.radius = 1.0F;
	protocol::StateImage image;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::Created, detail::build_phase1_state_image(input, image));
	return image;
}

void activate_live_baseline_without_acknowledging_session_begin(detail::SessionController& controller,
	protocol::EndpointKey endpoint,
	std::uint64_t nonce)
{
	const auto hello_packet = hello(protocol::VersionMinorV1_1, nonce, endpoint);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint, byte_view(hello_packet.bytes), 1'000U, 7U, true).disposition);
	const auto welcome = pop_controller_packet(controller);
	const auto welcome_ack = ack_for(welcome.bytes, endpoint, 2U);
	ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(endpoint, byte_view(welcome_ack.bytes), 2'000U, 7U, true).disposition);
	const auto begin = pop_controller_packet(controller);
	// Keeping SESSION_BEGIN unacknowledged leaves a due reliable item for the
	// priority oracle below.
	protocol::DatagramView begin_view;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(byte_view(begin.bytes),
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, begin_view));
	ASSERT_EQ(protocol::MessageType::SessionBegin, begin_view.header.message_type);
	ASSERT_TRUE(controller.begin_initial_snapshot(0U, phase1_state_image_at(1.0F, 3'000U), 3'000U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(1U, 3'001U));
	const auto snapshot = pop_controller_packet(controller);
	const auto snapshot_ack = ack_for(snapshot.bytes, endpoint, 3U);
	ASSERT_NE(detail::SessionIngressDisposition::Dropped,
		controller.ingest(endpoint, byte_view(snapshot_ack.bytes), 4'000U, 7U, true).disposition);
	ASSERT_EQ(detail::Phase1SnapshotProgress::Live, controller.snapshot_progress(0U));
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
	std::uint32_t mission_generation = 1U,
	bool mission_active = true)
{
	const auto sessions_before = fixture.runtime.active_sessions();
	auto drive_new_send = [&](std::size_t previous, std::uint64_t first_tick) {
		for (std::uint64_t offset = 0U; offset < 8U && fixture.backend.sent.size() == previous; ++offset) {
			native_tick(fixture,
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
		native_tick(fixture, {now_us + 20U + offset, mission_generation, mission_active});
	}
	EXPECT_GT(fixture.backend.receive_script_index, final_ack_script_index)
		<< "The final SESSION_BEGIN ACK must be consumed, not merely queued.";
	EXPECT_EQ(sessions_before + 1U, fixture.runtime.active_sessions())
		<< "establish_ready adds exactly one session without assuming an empty fixture.";
	return begin.header.session_id;
}

bool mission_paused(const protocol::StateImage& image)
{
	for (const auto& atom : image.records()) {
		if (atom.key.record_type !=
			static_cast<std::uint16_t>(protocol::RecordType::MissionState))
			continue;
		// MISSION_STATE: presence u64, generation u32, phase u8, paused bool.
		EXPECT_GT(atom.value.size(), 13U);
		return atom.value.size() > 13U && atom.value[13U] != 0U;
	}
	ADD_FAILURE() << "Missing MISSION_STATE";
	return false;
}

std::uint64_t establish_prewarmed_controller_session(detail::SessionController& controller,
	protocol::EndpointKey endpoint,
	std::uint64_t nonce,
	std::uint64_t now_us,
	std::uint32_t packet_sequence)
{
	const auto request = hello(protocol::VersionMinorV1_1, nonce, endpoint);
	EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint, byte_view(request.bytes), now_us, 0U, false).disposition);
	const auto welcome = pop_controller_packet(controller);
	protocol::DatagramView decoded;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(byte_view(welcome.bytes),
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1}, decoded));
	const auto ack = ack_for(welcome.bytes, endpoint, packet_sequence);
	EXPECT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(endpoint, byte_view(ack.bytes), now_us + 1U, 0U, false).disposition);
	return decoded.header.session_id;
}

std::uint64_t establish_live_controller_session(detail::SessionController& controller,
	protocol::EndpointKey endpoint,
	std::uint64_t nonce,
	std::uint64_t now_us,
	std::uint32_t packet_sequence)
{
	const auto request = hello(protocol::VersionMinorV1_1, nonce, endpoint);
	EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint, byte_view(request.bytes), now_us, 7U, true).disposition);
	const auto welcome = pop_controller_packet(controller);
	protocol::DatagramView decoded;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(byte_view(welcome.bytes),
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1}, decoded));
	const auto welcome_ack = ack_for(welcome.bytes, endpoint, packet_sequence++);
	EXPECT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(endpoint, byte_view(welcome_ack.bytes), now_us + 1U, 7U, true).disposition);
	const auto begin = pop_controller_packet(controller);
	const auto begin_ack = ack_for(begin.bytes, endpoint, packet_sequence++);
	EXPECT_NE(detail::SessionIngressDisposition::Dropped,
		controller.ingest(endpoint, byte_view(begin_ack.bytes), now_us + 2U, 7U, true).disposition);
	EXPECT_TRUE(controller.begin_initial_snapshot(0U,
		phase1_state_image_at(1.0F, now_us + 3U), now_us + 3U));
	for (std::uint64_t offset = 0U;
		offset < 16U && controller.snapshot_progress(0U) != detail::Phase1SnapshotProgress::Live;
		++offset) {
		EXPECT_EQ(1U, controller.service_initial_snapshot_egress(64U, now_us + 4U + offset));
		const auto snapshot = pop_controller_packet(controller);
		const auto snapshot_ack = ack_for(snapshot.bytes, endpoint, packet_sequence++);
		EXPECT_NE(detail::SessionIngressDisposition::Dropped,
			controller.ingest(endpoint, byte_view(snapshot_ack.bytes), now_us + 20U + offset, 7U, true).disposition);
	}
	EXPECT_EQ(detail::Phase1SnapshotProgress::Live, controller.snapshot_progress(0U));
	return decoded.header.session_id;
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

	void capture_main_thread() noexcept override
	{
		detail::capture_phase2_main_thread_authority();
		captured = true;
	}
	bool is_on_captured_main_thread() noexcept override
	{
		++thread_checks;
		return captured && thread_matches;
	}
	detail::RuntimeConfigResult load_config() noexcept override
	{
		return {config_status, config.max_clients};
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
	bool provision_metrics() noexcept override
	{
		++metrics_provision_calls;
		return metrics_provision_succeeds && metrics.provision();
	}
	void release_metrics() noexcept override { metrics.release(); }
	void record_callback_metric(detail::TelemetryCallbackKind kind, std::uint64_t duration_us) noexcept override
	{
		metrics.record_callback(kind, duration_us);
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
		if (use_runtime_adapter_helper && detail::RuntimeAdapterPlayerPublicProbe::contract_available()) {
			return detail::RuntimeAdapterPlayerPublicProbe::service_tick(native.get(), context);
		}
		CountingEngineReadView view;
		view.mission = context.mission_active;
		const auto status = native->service_tick(
			{context.now_us, context.mission_generation, context.mission_active}, view);
		return map_native_tick_status(status);
	}

	static detail::RuntimeTickStatus map_native_tick_status(detail::NativeSessionTickStatus status) noexcept
	{
		if (detail::RuntimeAdapterPlayerPublicProbe::contract_available()) {
			return detail::RuntimeAdapterPlayerPublicProbe::map_tick_status(status);
		}
		switch (status) {
		case detail::NativeSessionTickStatus::Complete:
			return detail::RuntimeTickStatus::Complete;
		case detail::NativeSessionTickStatus::Unavailable:
			return detail::RuntimeTickStatus::Unavailable;
		case detail::NativeSessionTickStatus::PermanentTransportFailure:
			return detail::RuntimeTickStatus::PermanentTransportFailure;
		case detail::NativeSessionTickStatus::PermanentCaptureFailure:
		default:
			return static_cast<detail::RuntimeTickStatus>(3U);
		}
	}

	void stop_collection() noexcept override
	{
		teardown.push_back('1');
		if (use_runtime_adapter_helper && detail::RuntimeAdapterPlayerPublicProbe::contract_available()) {
			detail::RuntimeAdapterPlayerPublicProbe::stop_collection(native.get());
		}
	}
	void invalidate_mission_state_and_entities() noexcept override
	{
		teardown.push_back('2');
		runtime_trace.push_back('L');
		if (use_runtime_adapter_helper && detail::RuntimeAdapterPlayerPublicProbe::contract_available()) {
			detail::RuntimeAdapterPlayerPublicProbe::invalidate_mission_state_and_entities(native.get());
		} else if (native != nullptr) {
			native->purge_all(detail::SessionCloseReason::MissionDiscontinuity);
		}
	}
	void cancel_replication() noexcept override { teardown.push_back('3'); }
	void close_sessions_and_stores() noexcept override
	{
		teardown.push_back('4');
		for (std::size_t index = 0U; index < detail::TelemetryMetricsMaxClients; ++index) {
			metrics.deactivate_session(index);
		}
		if (use_runtime_adapter_helper && detail::RuntimeAdapterPlayerPublicProbe::contract_available()) {
			detail::RuntimeAdapterPlayerPublicProbe::close_sessions_and_stores(native.get());
		} else if (native != nullptr) {
			native->purge_all(detail::SessionCloseReason::Shutdown);
		}
	}
	void reset_mission_scope() noexcept override
	{
		teardown.push_back('5');
		metrics.reset_mission();
	}
	void stop_transport() noexcept override
	{
		teardown.push_back('6');
		if (use_runtime_adapter_helper && detail::RuntimeAdapterPlayerPublicProbe::contract_available()) {
			detail::RuntimeAdapterPlayerPublicProbe::stop_transport(native.get());
		} else if (native != nullptr) {
			native->shutdown();
		}
	}
	void emit_runtime_summary() noexcept override { teardown.push_back('7'); }
	void emit_runtime_disabled(detail::RuntimeTerminalReason reason) noexcept override
	{
		disabled_reasons.push_back(reason);
	}
	void emit_mission_entered(std::uint32_t generation) noexcept override
	{
		lifecycle_log.push_back({'E', generation});
	}
	void emit_mission_left(std::uint32_t generation) noexcept override
	{
		lifecycle_log.push_back({'L', generation});
	}
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
	detail::TelemetryMetrics metrics;
	telemetry::TelemetryConfig config;
	detail::RuntimeConfigStatus config_status = detail::RuntimeConfigStatus::Enabled;
	detail::Wp03KnownBudgetSubtotal budget;
	std::unique_ptr<detail::NativeSessionRuntime> native;
	detail::RuntimeTickContext last_tick{};
	std::vector<char> teardown;
	std::vector<char> runtime_trace;
	std::vector<detail::RuntimeTerminalReason> disabled_reasons;
	std::vector<std::pair<char, std::uint32_t>> lifecycle_log;
	std::uint64_t clock_now_us = 9'000U;
	std::size_t clock_calls = 0U;
	std::size_t service_calls = 0U;
	std::size_t thread_checks = 0U;
	std::size_t budget_calls = 0U;
	std::size_t registry_allocations = 0U;
	std::size_t start_transport_calls = 0U;
	std::size_t native_constructions = 0U;
	std::size_t diagnostics = 0U;
	std::size_t metrics_provision_calls = 0U;
	bool captured = false;
	bool thread_matches = true;
	bool use_runtime_adapter_helper = false;
	bool metrics_provision_succeeds = true;
};

template <typename T, typename = void>
struct has_native_contract : std::false_type {};

template <typename T>
struct has_native_contract<T,
	std::void_t<decltype(std::declval<T&>().start(std::declval<const detail::NativeSessionStartRequest&>())),
		decltype(std::declval<T&>().service_tick(std::declval<const detail::NativeSessionTickContext&>(),
			std::declval<const detail::EngineReadView&>())),
		decltype(std::declval<T&>().purge_all(std::declval<detail::SessionCloseReason>())),
		decltype(std::declval<T&>().shutdown()),
		decltype(std::declval<const T&>().socket_count()),
		decltype(std::declval<const T&>().active_sessions()),
		decltype(std::declval<const T&>().owned_usage())>>
	: std::bool_constant<noexcept(std::declval<T&>().start(
		  std::declval<const detail::NativeSessionStartRequest&>())) &&
		  noexcept(std::declval<T&>().service_tick(std::declval<const detail::NativeSessionTickContext&>(),
			  std::declval<const detail::EngineReadView&>())) &&
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

TEST(TelemetryP85PreallocationContract, ProvisionFailurePreventsBindAndAColdRuntimeCanRetry)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(1U);

	NativePlayerAccess::set_session_controller_provision_failure(fixture->runtime, true);
	EXPECT_EQ(detail::NativeSessionStartStatus::AllocationFailure, fixture->start(config));
	EXPECT_EQ(0U, fixture->backend.open_calls)
		<< "SessionController preallocation must finish before the first socket open attempt.";
	EXPECT_EQ(0U, fixture->runtime.socket_count());
	EXPECT_EQ(0U, fixture->runtime.active_sessions());

	NativePlayerAccess::set_session_controller_provision_failure(fixture->runtime, false);
	EXPECT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	EXPECT_GT(fixture->backend.open_calls, 0U)
		<< "A provisioning failure must leave the runtime cold and retryable once the allocation succeeds.";
	EXPECT_GT(fixture->runtime.socket_count(), 0U);
	EXPECT_EQ(0U, fixture->runtime.active_sessions());
}

TEST(TelemetryNativeRuntimeIntegrationContract, S8V4OwnedBudgetPlusOneFailsBeforeTransportBind)
{
	auto config = enabled_config(1U);
	auto baseline = std::make_unique<NativeFixture>();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started,
		baseline->start_requested(config,
			telemetry::Phase2Profile::CoreGate));
	const auto startup_owned_bytes =
		NativePlayerAccess::startup_owned_bytes(baseline->runtime);
	const auto owned_budget =
		NativePlayerAccess::phase2_owned_budget(baseline->runtime);
	ASSERT_GT(startup_owned_bytes, 0U);
	ASSERT_EQ(detail::StartupBudgetError::None, owned_budget.error);
	ASSERT_EQ(startup_owned_bytes, owned_budget.process_owned_bytes);
	ASSERT_LE(owned_budget.shared_owned_bytes,
		detail::Phase2SharedOwnedCapBytes);
	ASSERT_LE(owned_budget.client_owned_bytes,
		detail::Phase2ClientOwnedCapBytes);
	ASSERT_LE(owned_budget.process_owned_bytes,
		detail::Phase2ProcessOwnedCapBytes);
	std::cout << "[ PHASE2 STARTUP OWNED BYTES ] " << startup_owned_bytes << '\n';

	auto fixture = std::make_unique<NativeFixture>();
	NativePlayerAccess::set_startup_owned_budget_adjustment(
		fixture->runtime,
		detail::Phase2SharedOwnedCapBytes -
			owned_budget.shared_owned_bytes + 1U);

	EXPECT_EQ(detail::NativeSessionStartStatus::AllocationFailure,
		fixture->start_requested(config,
			telemetry::Phase2Profile::CoreGate));
	EXPECT_EQ(0U, fixture->backend.open_calls);
	EXPECT_EQ(0U, fixture->runtime.socket_count());
	EXPECT_EQ(0U, fixture->runtime.active_sessions());
}

TEST(TelemetryNativeRuntimeIntegrationContract,
	CompleteShipStartsWithOwnedProjectionStateBeforeTheFirstCapture)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	config.max_clients = detail::TelemetryMetricsMaxClients;

	ASSERT_EQ(detail::NativeSessionStartStatus::Started,
		fixture->start_requested(config, telemetry::Phase2Profile::CompleteShip));
	const auto owned_budget =
		NativePlayerAccess::phase2_owned_budget(fixture->runtime);
	EXPECT_EQ(detail::StartupBudgetError::None, owned_budget.error);
	EXPECT_LE(owned_budget.shared_owned_bytes,
		detail::Phase2SharedOwnedCapBytes);
	EXPECT_LE(owned_budget.client_owned_bytes,
		detail::Phase2ClientOwnedCapBytes);
	EXPECT_LE(owned_budget.process_owned_bytes,
		detail::Phase2ProcessOwnedCapBytes);
	std::cout << "[ PHASE2 COMPLETE SHIP 4-CLIENT OWNED BYTES ] "
			  << owned_budget.shared_owned_bytes << ' '
			  << owned_budget.client_owned_bytes << ' '
			  << owned_budget.process_owned_bytes << '\n';
	EXPECT_GT(fixture->backend.open_calls, 0U);
	EXPECT_EQ(1U, fixture->runtime.socket_count());
	EXPECT_EQ(0U, fixture->runtime.active_sessions());
}

TEST(TelemetryPhase3ResourceContract,
	CockpitSensorsPreallocatesWithinAllThreeCapsAndPlusOneFailsBeforeBind)
{
	auto config = enabled_config();
	config.max_clients = detail::TelemetryMetricsMaxClients;
	auto baseline = std::make_unique<NativeFixture>();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started,
		baseline->start_requested(config,
			telemetry::Phase2Profile::CockpitSensors));
	const auto owned =
		NativePlayerAccess::phase2_owned_budget(baseline->runtime);
	ASSERT_EQ(detail::StartupBudgetError::None, owned.error);
	EXPECT_EQ(NativePlayerAccess::startup_owned_bytes(baseline->runtime),
		owned.process_owned_bytes);
	EXPECT_LE(owned.shared_owned_bytes,
		detail::Phase3SharedOwnedCapBytes);
	EXPECT_LE(owned.client_owned_bytes,
		detail::Phase3ClientOwnedCapBytes);
	EXPECT_LE(owned.process_owned_bytes,
		detail::Phase3ProcessOwnedCapBytes);
	EXPECT_EQ(0U, baseline->runtime.active_sessions());

	auto plus_one = std::make_unique<NativeFixture>();
	NativePlayerAccess::set_startup_owned_budget_adjustment(
		plus_one->runtime,
		detail::Phase3SharedOwnedCapBytes -
			owned.shared_owned_bytes + 1U);
	EXPECT_EQ(detail::NativeSessionStartStatus::AllocationFailure,
		plus_one->start_requested(config,
			telemetry::Phase2Profile::CockpitSensors));
	EXPECT_EQ(0U, plus_one->backend.open_calls);
	EXPECT_EQ(0U, plus_one->runtime.socket_count());
}

TEST(TelemetryNativeRuntimeIntegrationContract,
	ReviewerFinalTst008EligibilityMatrixRejectsBeforeOpenOrBind)
{
	auto config = enabled_config(1U);
	std::vector<telemetry::Phase2ProfileEligibility> rejected;
	auto multiplayer_client = telemetry::Phase2ProfileEligibility{};
	multiplayer_client.authority_mode = protocol::AuthorityMode::MultiplayerClient;
	rejected.push_back(multiplayer_client);
	auto multiplayer_master = telemetry::Phase2ProfileEligibility{};
	multiplayer_master.authority_mode = protocol::AuthorityMode::MultiplayerMaster;
	rejected.push_back(multiplayer_master);
	auto dedicated = telemetry::Phase2ProfileEligibility{};
	dedicated.dedicated = true;
	rejected.push_back(dedicated);
	auto headless = telemetry::Phase2ProfileEligibility{};
	headless.headless = true;
	rejected.push_back(headless);

	for (const auto& eligibility : rejected) {
		auto fixture = std::make_unique<NativeFixture>();
		EXPECT_EQ(detail::NativeSessionStartStatus::InvalidConfiguration,
			fixture->start_with_eligibility(config, eligibility));
		EXPECT_EQ(0U, fixture->backend.open_calls);
		EXPECT_TRUE(fixture->backend.io_trace.empty());
		EXPECT_EQ(0U,
			NativePlayerAccess::startup_allocation_count(fixture->runtime));
		EXPECT_EQ(0U, fixture->runtime.socket_count());
		EXPECT_EQ(0U, fixture->runtime.active_sessions());
	}

	auto solo = std::make_unique<NativeFixture>();
	EXPECT_EQ(detail::NativeSessionStartStatus::Started,
		solo->start_with_eligibility(config, {}));
	EXPECT_GT(solo->backend.open_calls, 0U);
	EXPECT_GT(NativePlayerAccess::startup_allocation_count(solo->runtime), 0U);
}

TEST(TelemetryNativeRuntimeIntegrationContract,
	CoreGateStartsWithOwnedProjectionAndNoExternalManifestInput)
{
	auto config = enabled_config();
	auto accepted = std::make_unique<NativeFixture>();
	EXPECT_EQ(detail::NativeSessionStartStatus::Started,
		accepted->start_requested(config,
			telemetry::Phase2Profile::CoreGate));
	EXPECT_GT(accepted->backend.open_calls, 0U);
	EXPECT_EQ(1U, accepted->runtime.socket_count());
	EXPECT_GT(NativePlayerAccess::startup_owned_bytes(
		accepted->runtime), 0U);
	auto* controller =
		NativePlayerAccess::controller(accepted->runtime);
	ASSERT_NE(nullptr, controller);
	const auto owned = controller->owned_capacity();
	EXPECT_EQ(2U * protocol::MaxStateMessageSize,
		owned.delta_egress_heap_bytes)
		<< "CoreGate deltas must not retain the 512-byte Phase 1 egress capacity.";
	EXPECT_GT(owned.delta_scratch_heap_bytes,
		4U * sizeof(protocol::StateMutation))
		<< "CoreGate needs the Phase 2 mutation scratch or a normal systems tick degenerates into a keyframe.";
}

TEST(TelemetryNativeRuntimeIntegrationContract, KeyframePreparationForcesBothCaptureFamilies)
{
	auto fixture = std::make_unique<NativeFixture>();
	const auto plan =
		NativePlayerAccess::prepare_phase2_keyframe_plan(fixture->runtime);
	EXPECT_TRUE(plan.force_complete_keyframe);
	EXPECT_TRUE(plan.capture_flight_controls);
	EXPECT_TRUE(plan.capture_systems);
}

TEST(TelemetryPhase3CaptureContract,
	EngineCollectorRefusesOffMainThreadBeforeReadingEngineAuthorities)
{
	// The capture authority belongs to this test's main thread.  Invoke the
	// actual collector from a worker: its first product check must reject the
	// call before it can inspect any mutable engine global.
	detail::capture_phase2_main_thread_authority();
	auto identities = std::make_unique<detail::Phase3IdentityRegistry>();
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	detail::Phase3EngineCollectStatus status =
		detail::Phase3EngineCollectStatus::Collected;
	detail::Phase3EngineCollectDiagnostic diagnostic;
	std::thread worker([&] {
		status = detail::collect_phase3_engine_projection(
			{}, *identities, *output, *scratch, &diagnostic);
	});
	worker.join();

	EXPECT_EQ(detail::Phase3EngineCollectStatus::NotMainThread, status);
	EXPECT_EQ(detail::Phase3EngineCollectBlock::Precondition,
		diagnostic.block);
	EXPECT_EQ(detail::Phase3EngineCollectStatus::NotMainThread,
		diagnostic.status);
	EXPECT_FALSE(identities->transaction_active());
}

#if defined(_WIN32)
struct LowStackPhase3CollectContext {
	detail::Phase3IdentityRegistry* identities = nullptr;
	telemetry::Phase3Projection* output = nullptr;
	telemetry::Phase3Projection* scratch = nullptr;
	detail::Phase3EngineCollectStatus status =
		detail::Phase3EngineCollectStatus::Collected;
};

unsigned __stdcall run_low_stack_phase3_collect(void* opaque) noexcept
{
	auto* context = static_cast<LowStackPhase3CollectContext*>(opaque);
	context->status = detail::collect_phase3_engine_projection(
		{}, *context->identities, *context->output, *context->scratch);
	return 0U;
}
#endif

TEST(TelemetryPhase3CaptureContract,
	EngineCollectorEntryFitsTheProductionWindowsThreadStack)
{
#if defined(_WIN32)
	// The monolithic test runner reserves 8 MiB, which previously hid a
	// 2.17 MiB aggregate temporary created by the Phase 3 collector. Exercise
	// the real entry point on a deliberately smaller stack than the 1 MiB
	// production engine thread. The off-main-thread gate is the expected
	// semantic result; reaching it proves function entry did not overflow.
	detail::capture_phase2_main_thread_authority();
	auto identities = std::make_unique<detail::Phase3IdentityRegistry>();
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	LowStackPhase3CollectContext context{
		identities.get(), output.get(), scratch.get()};
	const auto thread = reinterpret_cast<HANDLE>(_beginthreadex(
		nullptr, 512U * 1024U, &run_low_stack_phase3_collect,
		&context, 0U, nullptr));
	ASSERT_NE(nullptr, thread);
	ASSERT_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 10'000U));
	CloseHandle(thread);
	EXPECT_EQ(detail::Phase3EngineCollectStatus::NotMainThread,
		context.status);
	EXPECT_FALSE(identities->transaction_active());
#else
	GTEST_SKIP() << "Windows stack-reservation regression.";
#endif
}

TEST(TelemetryPhase3Visibility,
	NonCockpitTrackTargetDoesNotAllocateAHiddenSensorIdentity)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	constexpr int TargetObjectIndex = MAX_OBJECTS - 3;
	auto& target = Objects[TargetObjectIndex];
	target.clear();
	// An asteroid is a valid live target selection but not a cockpit radar
	// track in the authorized collector surface.
	target.type = OBJ_ASTEROID;
	target.signature = 77;
	list_append(&obj_used_list, &target);
	Player_ai->target_objnum = TargetObjectIndex;
	Player_ai->target_signature = target.signature;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	EXPECT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, true, false},
			identities, *output, *scratch));
	list_remove(&obj_used_list, &target);
	target.clear();
	EXPECT_EQ(0U, output->target.current_target_entity_id);
	EXPECT_EQ(0U, output->target.presence);
	EXPECT_EQ(1U, identities.identity_count())
		<< "Only the reconciled Phase 2 player may exist; the hidden target must not allocate an ID.";
}

TEST(TelemetryPhase3Targeting,
	StaleTargetSignatureRefusesOnlyTheCurrentTargetBlock)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	constexpr int TargetObjectIndex = MAX_OBJECTS - 3;
	auto& target = Objects[TargetObjectIndex];
	target.clear();
	// The game can replace a target between the index and signature updates.
	// That transient pair must suppress TARGET_STATE, not fault the telemetry
	// runtime or allocate an identity for the stale object.
	target.type = OBJ_ASTEROID;
	target.signature = 77;
	list_append(&obj_used_list, &target);
	Player_ai->target_objnum = TargetObjectIndex;
	Player_ai->target_signature = 78;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();

	EXPECT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, true, false},
			identities, *output, *scratch));
	list_remove(&obj_used_list, &target);
	target.clear();
	EXPECT_EQ(0U, output->target.current_target_entity_id);
	EXPECT_EQ(0U, output->target.presence);
	EXPECT_EQ(1U, identities.identity_count());
}

TEST(TelemetryPhase3Targeting,
	TransientInvalidIffOmitsOnlyHudColorWithoutFailingCapture)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	const auto ship_info_count = Ship_info.size();
	Ship_info.emplace_back();
	Player_ship->ship_info_index = static_cast<int>(ship_info_count);
	constexpr int TargetObjectIndex = MAX_OBJECTS - 3;
	constexpr int TargetShipIndex = MAX_SHIPS - 3;
	auto& target = Objects[TargetObjectIndex];
	auto& target_ship = Ships[TargetShipIndex];
	target.clear();
	target.type = OBJ_SHIP;
	target.instance = TargetShipIndex;
	target.signature = 79;
	target_ship.clear();
	target_ship.objnum = TargetObjectIndex;
	target_ship.team = -1;
	target_ship.ship_info_index = static_cast<int>(ship_info_count);
	std::strncpy(target_ship.ship_name, "Transient ship",
		sizeof(target_ship.ship_name) - 1U);
	list_append(&obj_used_list, &target);
	Player_ai->target_objnum = TargetObjectIndex;
	Player_ai->target_signature = target.signature;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();

	const auto status = detail::collect_phase3_engine_projection(
		{64U, 1U, phase2.get(), &binding, 1U, nullptr, true, false},
		identities, *output, *scratch);

	list_remove(&obj_used_list, &target);
	target_ship.clear();
	target.clear();
	Ship_info.resize(ship_info_count);
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected, status);
	EXPECT_NE(0U, output->target.current_target_entity_id);
	EXPECT_EQ(0U, output->target.presence &
		protocol::TargetStatePresenceFlagHudTargetColor)
		<< "A transient invalid IFF relation withdraws color, not TARGET_STATE or the session.";
}

TEST(TelemetryPhase3Targeting,
	RetainedStealthObservationUsesTheRealVisibleCockpitTrack)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	// Primitive sensors keep the normal radar projection deterministic without
	// substituting a telemetry visibility source.  See_all permits the retained
	// engine track to cross the same radar boundary even when AWACS has not run.
	Player_ship->flags.set(Ship::Ship_Flags::Primitive_sensors);
	Player_ship->primitive_sensor_range = 1'000;
	See_all = 1;
	const auto ship_info_count = Ship_info.size();
	Ship_info.emplace_back();
	std::strncpy(Ship_info.back().name, "GTF Myrmidon",
		sizeof(Ship_info.back().name) - 1U);
	Player_ship->ship_info_index = static_cast<int>(ship_info_count);
	constexpr int TargetObjectIndex = MAX_OBJECTS - 3;
	constexpr int TargetShipIndex = MAX_SHIPS - 3;
	auto& target = Objects[TargetObjectIndex];
	auto& target_ship = Ships[TargetShipIndex];
	target.clear();
	target.type = OBJ_SHIP;
	target.instance = TargetShipIndex;
	target.signature = 88;
	target.orient = vmd_identity_matrix;
	target.pos.xyz.z = 100.0F;
	target_ship.clear();
	target_ship.objnum = TargetObjectIndex;
	target_ship.team = Player_ship->team;
	target_ship.ship_info_index = static_cast<int>(ship_info_count);
	std::strncpy(target_ship.ship_name, "Alpha 2",
		sizeof(target_ship.ship_name) - 1U);
	list_init(&target_ship.subsys_list);
	model_subsystem target_subsystem_info;
	std::strncpy(target_subsystem_info.name, "Engine array",
		sizeof(target_subsystem_info.name) - 1U);
	target_subsystem_info.type = SUBSYSTEM_ENGINE;
	ship_subsys target_subsystem;
	target_subsystem.clear();
	target_subsystem.system_info = &target_subsystem_info;
	target_subsystem.parent_objnum = TargetObjectIndex;
	list_append(&target_ship.subsys_list, &target_subsystem);
	model_subsystem lock_subsystem_info;
	std::strncpy(lock_subsystem_info.name, "Navigation",
		sizeof(lock_subsystem_info.name) - 1U);
	lock_subsystem_info.type = SUBSYSTEM_NAVIGATION;
	ship_subsys lock_subsystem;
	lock_subsystem.clear();
	lock_subsystem.system_info = &lock_subsystem_info;
	lock_subsystem.parent_objnum = TargetObjectIndex;
	list_append(&target_ship.subsys_list, &lock_subsystem);
	list_append(&obj_used_list, &target);
	Player_ai->target_objnum = TargetObjectIndex;
	Player_ai->target_signature = target.signature;
	Player_ai->targeted_subsys = &target_subsystem;
	Player_ai->targeted_subsys_parent = TargetObjectIndex;
	Player->locking_subsys = &lock_subsystem;
	Player->locking_subsys_parent = TargetObjectIndex;
	Player_ai->stealth_last_visible_stamp = 1;
	Player_ai->stealth_last_pos.xyz = {10.0F, 20.0F, 30.0F};
	Player_ai->stealth_velocity.xyz = {1.0F, 2.0F, 3.0F};

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	// Deliberately make the independently sampled FLIGHT_STATE pose disagree
	// with the live cockpit eye frame. Radar projection must not consume it.
	phase2->ships[0].flight.orientation_local_to_world =
		{{0.0F, 0.0F, 1.0F, 0.0F}};
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	const auto status = detail::collect_phase3_engine_projection(
		{64U, 1U, phase2.get(), &binding, 1U, nullptr, true, false},
		identities, *output, *scratch);
	// The remainder of this older fixture exercises the primary-weapon lead
	// using a deliberately model-less ship class.  Do not feed its synthetic
	// subsystem into get_subsystem_world_pos(); the first collection above is
	// the subsystem-label scenario under test.
	Player_ai->targeted_subsys = nullptr;
	Player_ai->targeted_subsys_parent = -1;
	Player->locking_subsys = nullptr;
	Player->locking_subsys_parent = -1;
	telemetry::Phase2ManifestCandidate manifest;
	manifest.class_record_count = 1U;
	manifest.class_records[0].source_key = 1U;
	manifest.class_records[0].class_id = 9001U;
	manifest.class_records[0].name.assign("GTF Myrmidon");
	const auto weapon_info_count = Weapon_info.size();
	Weapon_info.emplace_back();
	Weapon_info.back().max_speed = 100.0F;
	Weapon_info.back().lifetime = 10.0F;
	Weapon_info.back().weapon_range = 1'000.0F;
	Player_ship->weapons.num_primary_banks = 1;
	Player_ship->weapons.current_primary_bank = 0;
	Player_ship->weapons.primary_bank_weapons[0] =
		static_cast<int>(weapon_info_count);
	manifest.class_records[0].banks.data = manifest.bank_records.data();
	manifest.class_records[0].banks.capacity = 1U;
	manifest.class_records[0].bank_count = 1U;
	manifest.bank_records[0].bank_id = 7001U;
	manifest.bank_records[0].owner_subsystem_id = 0U;
	manifest.bank_records[0].canonical_index = 0U;
	manifest.bank_records[0].family = telemetry::WeaponFamily::Primary;
	auto revealed_output = std::make_unique<telemetry::Phase3Projection>();
	auto revealed_scratch = std::make_unique<telemetry::Phase3Projection>();
	const auto revealed_status = detail::collect_phase3_engine_projection(
		{65U, 1U, phase2.get(), &binding, 1U, &manifest, true, false},
		identities, *revealed_output, *revealed_scratch);
	auto moved_output = std::make_unique<telemetry::Phase3Projection>();
	auto moved_scratch = std::make_unique<telemetry::Phase3Projection>();
	Player_obj->pos.xyz.z = 25.0F;
	const auto moved_status = detail::collect_phase3_engine_projection(
		{66U, 1U, phase2.get(), &binding, 1U, &manifest, false, true},
		identities, *moved_output, *moved_scratch);
	auto out_of_range_output = std::make_unique<telemetry::Phase3Projection>();
	auto out_of_range_scratch = std::make_unique<telemetry::Phase3Projection>();
	Player_obj->pos = vmd_zero_vector;
	target.pos.xyz.z = Radar_ranges[HUD_config.rp_dist] + 1.0F;
	const auto out_of_range_status =
		detail::collect_phase3_engine_projection(
			{67U, 1U, phase2.get(), &binding, 1U, &manifest, true, true},
			identities, *out_of_range_output, *out_of_range_scratch);

	Player_ai->targeted_subsys = nullptr;
	Player_ai->targeted_subsys_parent = -1;
	Player->locking_subsys = nullptr;
	Player->locking_subsys_parent = -1;
	list_remove(&target_ship.subsys_list, &lock_subsystem);
	list_remove(&target_ship.subsys_list, &target_subsystem);
	list_remove(&obj_used_list, &target);
	target_ship.clear();
	target.clear();
	Ship_info.resize(ship_info_count);
	Weapon_info.resize(weapon_info_count);

	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected, status);
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected, revealed_status);
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected, moved_status);
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		out_of_range_status);
	// The contact list is built only by walking the real HUD radar projection.
	// The single authorized live ship above must therefore appear exactly once,
	// with the projection's observed world position rather than a telemetry
	// reconstruction or an unrelated object-list entry.
	ASSERT_EQ(1U, output->contact_count);
	EXPECT_EQ(output->target.current_target_entity_id,
		output->contacts[0].entity_id);
	EXPECT_EQ(protocol::RadarVisibility::Visible,
		output->contacts[0].visibility);
	EXPECT_FLOAT_EQ(100.0F, output->contacts[0].position_world[2]);
	EXPECT_FLOAT_EQ(100.0F,
		output->contacts[0].radar_local_position[2]);
	EXPECT_FLOAT_EQ(100.0F,
		output->contacts[0].radar_projection_distance);
	EXPECT_NE(0U, output->contacts[0].presence &
		protocol::RadarContactsPresenceFlagRadarVisual);
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::RadarBlipType::NormalShip),
		output->contacts[0].radar_blip_type);
	EXPECT_NE(0U, output->contacts[0].flags & protocol::ContactFlagBright);
	EXPECT_NE(0U, output->contacts[0].flags &
		protocol::ContactFlagCurrentTarget);
	EXPECT_EQ(0U, output->contacts[0].flags &
		(protocol::ContactFlagTagged | protocol::ContactFlagWarp |
		 protocol::ContactFlagBomb));
	EXPECT_NE(0U, output->target.presence &
		protocol::TargetStatePresenceFlagHudTargetColor);
	EXPECT_NE(0U, output->target.presence &
		protocol::TargetStatePresenceFlagHudTargetSubsystemLabel);
	EXPECT_EQ("Engine array",
		std::string(output->target.hud_target_subsystem_label.bytes.data(),
			output->target.hud_target_subsystem_label.size));
	EXPECT_NE(0U, output->target.presence &
		protocol::TargetStatePresenceFlagHudLockSubsystemLabel);
	EXPECT_EQ("Navigation",
		std::string(output->target.hud_lock_subsystem_label.bytes.data(),
			output->target.hud_lock_subsystem_label.size));
	EXPECT_EQ(0U, output->target.presence &
		(protocol::TargetStatePresenceFlagTargetSubsystem |
		 protocol::TargetStatePresenceFlagLockSubsystem))
		<< "HUD subsystem labels remain available without installed manifest IDs.";
	EXPECT_EQ(output->contacts[0].radar_blip_color,
		output->target.hud_target_color)
		<< "A normal selected ship keeps its bright IFF hue on both radar and HUD.";
	EXPECT_EQ("Alpha 2", std::string(output->contacts[0].revealed_name.bytes.data(),
		output->contacts[0].revealed_name.size));
	EXPECT_EQ("GTF Myrmidon",
		std::string(output->contacts[0].hud_type_label.bytes.data(),
			output->contacts[0].hud_type_label.size));
	EXPECT_EQ(0U, output->contacts[0].revealed_class_id)
		<< "The HUD label must not expand CLASS_MANIFEST.";
	ASSERT_EQ(1U, moved_output->contact_count);
	EXPECT_FLOAT_EQ(75.0F,
		moved_output->contacts[0].radar_local_position[2]);
	EXPECT_FLOAT_EQ(75.0F,
		moved_output->contacts[0].radar_projection_distance);
	EXPECT_FLOAT_EQ(100.0F,
		output->contacts[0].radar_local_position[2])
		<< "The previous tick remains atomically self-consistent.";
	EXPECT_EQ(0U, out_of_range_output->contact_count);
	EXPECT_NE(0U, out_of_range_output->target.current_target_entity_id)
		<< "A Target Box target remains public without a radar blip.";
	EXPECT_NE(0U, out_of_range_output->target.presence &
		protocol::TargetStatePresenceFlagHudTargetColor);
	EXPECT_NE(0U, output->target.current_target_entity_id);
	EXPECT_NE(0U, output->target.presence &
		protocol::TargetStatePresenceFlagLastStealthObservation);
	EXPECT_FLOAT_EQ(10.0F, output->target.last_stealth_position[0]);
	EXPECT_FLOAT_EQ(30.0F, output->target.last_stealth_position[2]);
	EXPECT_FLOAT_EQ(1.0F, output->target.last_stealth_velocity[0]);
	EXPECT_FLOAT_EQ(3.0F, output->target.last_stealth_velocity[2]);
	EXPECT_NE(0U, revealed_output->target.presence &
		protocol::TargetStatePresenceFlagRevealedIdentity);
	EXPECT_EQ(9001U, revealed_output->target.revealed_class_id);
	EXPECT_NE(0U, revealed_output->target.presence &
		protocol::TargetStatePresenceFlagLead);
	EXPECT_EQ(7001U, revealed_output->target.lead_bank_id);
}

TEST(TelemetryPhase3RadarContacts,
	IndependentTargetAndRadarRefreshesRetainOwnSampleAndConverge)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	Player_ship->flags.set(Ship::Ship_Flags::Primitive_sensors);
	Player_ship->primitive_sensor_range = 1'000;
	See_all = 1;
	const auto ship_info_count = Ship_info.size();
	Ship_info.emplace_back();
	Player_ship->ship_info_index = static_cast<int>(ship_info_count);
	constexpr int FirstObjectIndex = MAX_OBJECTS - 3;
	constexpr int FirstShipIndex = MAX_SHIPS - 3;
	constexpr int SecondObjectIndex = MAX_OBJECTS - 4;
	constexpr int SecondShipIndex = MAX_SHIPS - 4;
	auto& first = Objects[FirstObjectIndex];
	auto& first_ship = Ships[FirstShipIndex];
	auto& second = Objects[SecondObjectIndex];
	auto& second_ship = Ships[SecondShipIndex];
	first.clear();
	first.type = OBJ_SHIP;
	first.instance = FirstShipIndex;
	first.signature = 101;
	first.orient = vmd_identity_matrix;
	first.pos.xyz.z = 100.0F;
	first_ship.clear();
	first_ship.objnum = FirstObjectIndex;
	first_ship.team = Player_ship->team;
	first_ship.ship_info_index = static_cast<int>(ship_info_count);
	second.clear();
	second.type = OBJ_SHIP;
	second.instance = SecondShipIndex;
	second.signature = 102;
	second.orient = vmd_identity_matrix;
	second.pos.xyz.z = 200.0F;
	second_ship.clear();
	second_ship.objnum = SecondObjectIndex;
	second_ship.team = Player_ship->team;
	second_ship.ship_info_index = static_cast<int>(ship_info_count);
	list_append(&obj_used_list, &first);
	list_append(&obj_used_list, &second);
	Player_ai->target_objnum = FirstObjectIndex;
	Player_ai->target_signature = first.signature;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, true, true},
			identities, *output, *scratch));
	ASSERT_EQ(2U, output->contact_count);
	const auto first_id = output->target.current_target_entity_id;
	ASSERT_NE(0U, first_id);
	const auto second_id = output->contacts[0].entity_id == first_id
		? output->contacts[1].entity_id : output->contacts[0].entity_id;

	Player_ai->previous_target_objnum = FirstObjectIndex;
	Player_ai->target_objnum = SecondObjectIndex;
	Player_ai->target_signature = second.signature;
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{65U, 1U, phase2.get(), &binding, 1U, nullptr, true, false},
			identities, *output, *scratch));
	EXPECT_EQ(second_id, output->target.current_target_entity_id);
	EXPECT_EQ(65U, output->target.producer_sample_time_us);
	for (std::size_t index = 0U; index < output->contact_count; ++index) {
		const auto& contact = output->contacts[index];
		EXPECT_EQ(64U, contact.producer_sample_time_us);
		EXPECT_EQ(contact.entity_id == first_id,
			(contact.flags & protocol::ContactFlagCurrentTarget) != 0U);
	}

	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{66U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	EXPECT_EQ(second_id, output->target.current_target_entity_id);
	EXPECT_EQ(65U, output->target.producer_sample_time_us);
	for (std::size_t index = 0U; index < output->contact_count; ++index) {
		const auto& contact = output->contacts[index];
		EXPECT_EQ(66U, contact.producer_sample_time_us);
		EXPECT_EQ(contact.entity_id == second_id,
			(contact.flags & protocol::ContactFlagCurrentTarget) != 0U);
	}

	// Reuse the selected object slot while the AI still retains the old
	// signature. TARGET_STATE must become empty and the ambiguous replacement
	// must not inherit CURRENT_TARGET or terminate the projection.
	second.signature = 103;
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{67U, 1U, phase2.get(), &binding, 1U, nullptr, true, true},
			identities, *output, *scratch));
	EXPECT_EQ(0U, output->target.current_target_entity_id);
	ASSERT_EQ(1U, output->contact_count);
	EXPECT_EQ(first_id, output->contacts[0].entity_id);
	EXPECT_EQ(0U, output->contacts[0].flags &
		protocol::ContactFlagCurrentTarget);

	Player_ai->target_objnum = -1;
	Player_ai->target_signature = -1;
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{68U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	ASSERT_EQ(2U, output->contact_count);
	for (std::size_t index = 0U; index < output->contact_count; ++index) {
		EXPECT_EQ(0U, output->contacts[index].flags &
			protocol::ContactFlagCurrentTarget);
	}

	list_remove(&obj_used_list, &second);
	list_remove(&obj_used_list, &first);
	second_ship.clear();
	second.clear();
	first_ship.clear();
	first.clear();
	Ship_info.resize(ship_info_count);
}

TEST(TelemetryPhase3RadarVisual,
	SharedResolverMatchesFsoBrightnessAndSpecialTypePrecedence)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	const auto prior_color_index = Iff_info[0].color_index;
	const auto prior_accessibility_color_index =
		Iff_info[0].accessibility_color_index;
	const auto test_color_index = iff_init_color(23, 117, 201);
	Iff_info[0].color_index = test_color_index;
	Iff_info[0].accessibility_color_index = test_color_index;

	const auto ship_info_count = Ship_info.size();
	Ship_info.emplace_back();
	constexpr int ShipIndex = MAX_SHIPS - 5;
	auto& source_ship = Ships[ShipIndex];
	source_ship.clear();
	source_ship.team = Player_ship->team;
	source_ship.ship_info_index = static_cast<int>(ship_info_count);
	object source;
	source.clear();
	source.type = OBJ_SHIP;
	source.instance = ShipIndex;

	RadarContactProjection projection;
	projection.distance = 2'000.0F;
	const auto prior_bright_range = Radar_bright_range;
	Radar_bright_range = 1'500.0F;
	RadarContactVisual visual;
	ASSERT_TRUE(radar_resolve_contact_visual(
		&source, projection, false, visual));
	EXPECT_FALSE(visual.bright);
	EXPECT_EQ(BLIP_TYPE_NORMAL_SHIP, visual.blip_type);
	EXPECT_EQ(iff_get_color_by_team_and_object(
		source_ship.team, Player_ship->team, 0, &source), visual.blip_color);

	ASSERT_TRUE(radar_resolve_contact_visual(
		&source, projection, true, visual));
	EXPECT_TRUE(visual.bright);
	EXPECT_EQ(BLIP_TYPE_NORMAL_SHIP, visual.blip_type);
	EXPECT_EQ(iff_get_color_by_team_and_object(
		source_ship.team, Player_ship->team, 1, &source), visual.blip_color);

	Ship_info.back().flags.set(Ship::Info_Flags::Cargo);
	ASSERT_TRUE(radar_resolve_contact_visual(
		&source, projection, false, visual));
	EXPECT_EQ(BLIP_TYPE_NAVBUOY_CARGO, visual.blip_type);
	EXPECT_EQ(&Radar_colors[RCOL_NAVBUOY_CARGO][0], visual.blip_color);

	source_ship.tag_left = 1.0F;
	ASSERT_TRUE(radar_resolve_contact_visual(
		&source, projection, false, visual));
	EXPECT_EQ(BLIP_TYPE_TAGGED_SHIP, visual.blip_type);
	EXPECT_EQ(&Radar_colors[RCOL_TAGGED][0], visual.blip_color);

	source_ship.flags.set(Ship::Ship_Flags::Arriving_stage_1);
	ASSERT_TRUE(radar_resolve_contact_visual(
		&source, projection, false, visual));
	EXPECT_EQ(BLIP_TYPE_WARPING_SHIP, visual.blip_type)
		<< "Warp stage 1 has priority over tagged and cargo colors.";
	EXPECT_EQ(&Radar_colors[RCOL_WARPING_SHIP][0], visual.blip_color);

	constexpr int WeaponIndex = MAX_WEAPONS - 5;
	const auto prior_lssm_stage = Weapons[WeaponIndex].lssm_stage;
	source.clear();
	source.type = OBJ_WEAPON;
	source.instance = WeaponIndex;
	Weapons[WeaponIndex].lssm_stage = 0;
	ASSERT_TRUE(radar_resolve_contact_visual(
		&source, projection, false, visual));
	EXPECT_EQ(BLIP_TYPE_BOMB, visual.blip_type);
	EXPECT_EQ(&Radar_colors[RCOL_BOMB][0], visual.blip_color);
	Weapons[WeaponIndex].lssm_stage = 2;
	ASSERT_TRUE(radar_resolve_contact_visual(
		&source, projection, false, visual));
	EXPECT_EQ(BLIP_TYPE_WARPING_SHIP, visual.blip_type);
	EXPECT_EQ(&Radar_colors[RCOL_WARPING_SHIP][0], visual.blip_color);

	source.clear();
	source.type = OBJ_JUMP_NODE;
	ASSERT_TRUE(radar_resolve_contact_visual(
		&source, projection, false, visual));
	EXPECT_EQ(BLIP_TYPE_JUMP_NODE, visual.blip_type);
	EXPECT_EQ(&Radar_colors[RCOL_JUMP_NODE][0], visual.blip_color);

	Weapons[WeaponIndex].lssm_stage = prior_lssm_stage;
	Radar_bright_range = prior_bright_range;
	source_ship.clear();
	Ship_info.resize(ship_info_count);
	Iff_info[0].color_index = prior_color_index;
	Iff_info[0].accessibility_color_index =
		prior_accessibility_color_index;
}

TEST(TelemetryPhase3Locks, RealVisibleCockpitTrackProducesAuthorizedLock)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	Player_ship->flags.set(Ship::Ship_Flags::Primitive_sensors);
	Player_ship->primitive_sensor_range = 1'000;
	See_all = 1;
	const auto ship_info_count = Ship_info.size();
	Ship_info.emplace_back();
	constexpr int TargetObjectIndex = MAX_OBJECTS - 3;
	constexpr int TargetShipIndex = MAX_SHIPS - 3;
	auto& target = Objects[TargetObjectIndex];
	auto& target_ship = Ships[TargetShipIndex];
	target.clear();
	target.type = OBJ_SHIP;
	target.instance = TargetShipIndex;
	target.signature = 89;
	target.orient = vmd_identity_matrix;
	target.pos.xyz.z = 100.0F;
	target_ship.clear();
	target_ship.objnum = TargetObjectIndex;
	target_ship.team = Player_ship->team;
	target_ship.ship_info_index = static_cast<int>(ship_info_count);
	list_append(&obj_used_list, &target);
	lock_info lock{};
	lock.obj = &target;
	lock.locked = false;
	lock.target_in_lock_cone = true;
	lock.time_to_lock = 2.0F;
	lock.world_pos = target.pos;
	Player_ship->missile_locks.push_back(lock);

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	const auto status = detail::collect_phase3_engine_projection(
		{64U, 1U, phase2.get(), &binding, 1U, nullptr, true, false},
		identities, *output, *scratch);

	Player_ship->missile_locks.clear();
	list_remove(&obj_used_list, &target);
	target_ship.clear();
	target.clear();
	Ship_info.resize(ship_info_count);

	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected, status);
	ASSERT_EQ(1U, output->lock_count);
	const auto& projected = output->locks[0];
	EXPECT_NE(0U, projected.target_entity_id);
	EXPECT_FALSE(projected.locked);
	EXPECT_TRUE(projected.target_in_lock_cone);
	EXPECT_NE(0U, projected.presence &
		protocol::LockItemPresenceFlagLockAttempt);
	EXPECT_EQ(2'000'000U, projected.time_to_lock_remaining_us);
}

TEST(TelemetryPhase3SensorState,
	EmpIntensityAndRemainingTimeComeFromTheLivePlayerShip)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	Player_ship->emp_intensity = 0.5F;
	Player_ship->emp_decr = 0.25F;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	EXPECT_NE(0U, output->radar.presence &
		protocol::RadarStatePresenceFlagEmp);
	EXPECT_FLOAT_EQ(0.5F, output->radar.emp_intensity);
	EXPECT_EQ(2'000'000U, output->radar.emp_remaining_us);
}

TEST(TelemetryPhase3SensorState,
	PrimitiveSensorRangeComesFromTheLivePlayerShip)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	Player_ship->flags.set(Ship::Ship_Flags::Primitive_sensors);
	Player_ship->primitive_sensor_range = 1234;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	EXPECT_NE(0U, output->radar.presence &
		protocol::RadarStatePresenceFlagPrimitiveRange);
	EXPECT_FLOAT_EQ(1234.0F, output->radar.primitive_range);
}

TEST(TelemetryPhase3SensorState,
	InfiniteRadarRangeUsesTheFiniteFstlSentinel)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	HUD_config.rp_dist = RR_INFINITY;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	EXPECT_EQ(protocol::RadarMode::Infinite, output->radar.mode);
	EXPECT_TRUE(std::isfinite(output->radar.selected_range));
	EXPECT_FLOAT_EQ(1.0e12F, output->radar.selected_range);
	EXPECT_NE(Radar_ranges[RR_INFINITY], output->radar.selected_range)
		<< "The engine's gameplay cutoff is not the FSTL infinity sentinel.";
}

TEST(TelemetryPhase3Navigation,
	EngineCollectorKeepsMissionOrderCurrentDestinationAndWaypointRoute)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	vec3d first{};
	first.xyz.x = 10.0F;
	vec3d second{};
	second.xyz.y = 20.0F;
	Waypoint_lists.emplace_back("Route");
	auto& waypoints = Waypoint_lists.back().get_waypoints();
	waypoints.emplace_back(&first);
	waypoints.emplace_back(&second);
	Navs[0].flags = NP_WAYPOINT | NP_VISITED;
	Navs[0].target_index = 0;
	Navs[0].waypoint_num = 0;
	Navs[0].m_NavName[0] = 'R';
	Navs[0].m_NavName[1] = 'o';
	Navs[0].m_NavName[2] = 'u';
	Navs[0].m_NavName[3] = 't';
	Navs[0].m_NavName[4] = 'e';
	CurrentNav = 0;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	ASSERT_EQ(1U, output->navigation.navpoint_count);
	const auto& navpoint = output->navigation.navpoints[0];
	EXPECT_EQ(1U, navpoint.navpoint_id);
	EXPECT_NE(0U, navpoint.presence &
		protocol::NavPointPresenceFlagWaypointLink);
	EXPECT_EQ(1U, navpoint.waypoint_list_id);
	EXPECT_EQ(0U, navpoint.waypoint_index);
	EXPECT_NE(0U, output->navigation.presence &
		protocol::NavigationStatePresenceFlagCurrentNavpoint);
	EXPECT_EQ(navpoint.navpoint_id,
		output->navigation.current_navpoint_id);
	EXPECT_NE(0U, output->navigation.presence &
		protocol::NavigationStatePresenceFlagWaypointRoute);
	ASSERT_EQ(2U, output->navigation.route_waypoint_count);
	EXPECT_EQ(1U, output->navigation.route_waypoints[0].waypoint_list_id);
	EXPECT_EQ(0U, output->navigation.route_waypoints[0].waypoint_index);
	EXPECT_EQ(1U, output->navigation.route_waypoints[1].waypoint_index);
}

TEST(TelemetryPhase3Navigation,
	NoCurrentNavPublishesClosedRefusalWithoutMutatingControls)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	ASSERT_EQ(-1, CurrentNav);
	ASSERT_FALSE(AutoPilotEngaged);

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	EXPECT_EQ(protocol::AutopilotState::Refused,
		output->navigation.autopilot_state);
	EXPECT_NE(0U, output->navigation.presence &
		protocol::NavigationStatePresenceFlagAutopilotRefusal);
	EXPECT_EQ(protocol::AutopilotRefusal::NoValidNav,
		output->navigation.autopilot_refusal);
	EXPECT_EQ(-1, CurrentNav);
	EXPECT_FALSE(AutoPilotEngaged);
}

TEST(TelemetryPhase3Cargo,
	ScanTargetOutsideCompleteShipClosureReceivesOnlyItsPublicReference)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	// Keep the radar list inert: cargo disclosure is independently authorized
	// by the scanner, so this test verifies that it does not depend on radar or
	// CompleteShip membership.
	Game_mode |= GM_STANDALONE_SERVER;
	constexpr int TargetObjectIndex = MAX_OBJECTS - 3;
	auto& target = Objects[TargetObjectIndex];
	target.clear();
	target.type = OBJ_SHIP;
	target.signature = 77;
	list_append(&obj_used_list, &target);
	Player_ai->target_objnum = TargetObjectIndex;
	Player_ai->target_signature = target.signature;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	phase2->player_cargo_scan.phase =
		detail::CargoScanPhaseObservation::Scanning;
	phase2->player_cargo_scan.presence =
		protocol::CargoScanStatePresenceFlagTarget;
	phase2->player_cargo_scan.target_capture_key.value =
		static_cast<std::uint32_t>(target.signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	EXPECT_NE(0U, output->cargo.target_entity_id);
	EXPECT_NE(0U, output->cargo.presence &
		protocol::CargoScanStatePresenceFlagTarget);
	EXPECT_EQ(0U, output->contact_count)
		<< "The scan must not materialize a RADAR_CONTACTS track.";
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::DisclosureState::Hidden),
		output->cargo.disclosure)
		<< "A target authorization alone must not disclose cargo text.";
	EXPECT_EQ(0U, output->cargo.cargo_text.size);
	EXPECT_EQ(2U, identities.identity_count())
		<< "Only the Phase 2 player and the independently authorized scan target are public.";
}

TEST(TelemetryPhase3Cargo,
	CompletedRevealedScanPreservesPhaseTimingValidityAndText)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	Game_mode |= GM_STANDALONE_SERVER;
	constexpr int TargetObjectIndex = MAX_OBJECTS - 3;
	auto& target = Objects[TargetObjectIndex];
	target.clear();
	target.type = OBJ_SHIP;
	target.signature = 77;
	list_append(&obj_used_list, &target);
	Player_ai->target_objnum = TargetObjectIndex;
	Player_ai->target_signature = target.signature;

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	auto& scan = phase2->player_cargo_scan;
	scan.phase = detail::CargoScanPhaseObservation::Completed;
	scan.presence = protocol::CargoScanStatePresenceFlagTarget |
		protocol::CargoScanStatePresenceFlagTiming |
		protocol::CargoScanStatePresenceFlagValidity |
		protocol::CargoScanStatePresenceFlagCargoText;
	scan.target_capture_key.value = static_cast<std::uint32_t>(target.signature);
	scan.elapsed_us = 10U;
	scan.required_us = 10U;
	scan.validity_flags = protocol::ScanValidityFlagNone;
	ASSERT_TRUE(scan.cargo_text.assign("Recovered cargo"));
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::DisclosureState::Revealed),
		output->cargo.disclosure);
	EXPECT_EQ(10U, output->cargo.elapsed_us);
	EXPECT_EQ(10U, output->cargo.required_us);
	EXPECT_EQ(0U, output->cargo.validity_flags);
	EXPECT_EQ("Recovered cargo", std::string_view(output->cargo.cargo_text.bytes.data(),
		output->cargo.cargo_text.size));
}

TEST(TelemetryPhase3Threat,
	EngineThreatFlagsUseAuthoritativePriorityWithoutInventingReferences)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	// These are the native Player::threat_flags bits used by the HUD threat
	// authority: dumbfire, lock attempt, then acquired lock in increasing
	// severity.  No attacker or weapon object is installed in this fixture.
	Player->threat_flags = (1 << 0) | (1 << 1) | (1 << 2);
	hud_start_text_flash(nullptr, 0);
	hud_start_text_flash("Collision", 5'000, 200,
		HudTextWarningKind::Collision);

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, *output, *scratch));
	EXPECT_EQ(protocol::ThreatLevel::LockAcquired,
		output->threat.threat_level);
	EXPECT_EQ(0U, output->threat.presence);
	EXPECT_EQ(0U, output->threat.nearest_attacker_entity_id);
	EXPECT_EQ(0U, output->threat.dangerous_weapon_entity_id);
	EXPECT_EQ(0U, output->threat.nearest_homing_entity_id);
	EXPECT_TRUE(output->hud_alert.primary_fire_threat_active);
	EXPECT_EQ(protocol::HudAlertMissileLockState::Acquired,
		output->hud_alert.missile_lock_state);
	EXPECT_EQ(protocol::HudAlertStatePresenceFlagActiveWarning,
		output->hud_alert.presence);
	EXPECT_EQ(protocol::HudAlertWarningKind::Collision,
		output->hud_alert.warning_kind);
	EXPECT_NE(0U, output->hud_alert.warning_instance_id);
	EXPECT_GT(output->hud_alert.warning_remaining_us, 0U);
	EXPECT_EQ("Collision", std::string_view(
		output->hud_alert.warning_text.bytes.data(),
		output->hud_alert.warning_text.size));
	hud_start_text_flash(nullptr, 0);
}

TEST(TelemetryPhase3Threat,
	HudWarningPriorityAndRetriggerChangeOnlyAcceptedInstances)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	hud_start_text_flash(nullptr, 0);
	HudAlertSnapshot first{};
	hud_start_text_flash("Collision", 5'000, 200,
		HudTextWarningKind::Collision);
	ASSERT_TRUE(hud_get_alert_snapshot(first));
	ASSERT_TRUE(first.warning_active);
	const auto collision_instance = first.warning_instance_id;

	// Native HUD priority refuses Launch while another warning is active.
	hud_start_text_flash("Launch", 5'000, 200,
		HudTextWarningKind::Launch);
	HudAlertSnapshot refused{};
	ASSERT_TRUE(hud_get_alert_snapshot(refused));
	EXPECT_EQ(collision_instance, refused.warning_instance_id);
	EXPECT_EQ(HudTextWarningKind::Collision, refused.warning_kind);

	// A real retrigger accepted by the HUD produces a fresh identity.
	hud_start_text_flash("Collision", 5'000, 200,
		HudTextWarningKind::Collision);
	HudAlertSnapshot retriggered{};
	ASSERT_TRUE(hud_get_alert_snapshot(retriggered));
	EXPECT_GT(retriggered.warning_instance_id, collision_instance);
	EXPECT_EQ(HudTextWarningKind::Collision, retriggered.warning_kind);

	// EMP may replace the current warning; once active it rejects Launch.
	hud_start_text_flash("Emp", 5'000, 200, HudTextWarningKind::Emp);
	HudAlertSnapshot emp{};
	ASSERT_TRUE(hud_get_alert_snapshot(emp));
	ASSERT_EQ(HudTextWarningKind::Emp, emp.warning_kind);
	const auto emp_instance = emp.warning_instance_id;
	hud_start_text_flash("Launch", 5'000, 200,
		HudTextWarningKind::Launch);
	HudAlertSnapshot emp_retained{};
	ASSERT_TRUE(hud_get_alert_snapshot(emp_retained));
	EXPECT_EQ(emp_instance, emp_retained.warning_instance_id);
	EXPECT_EQ(HudTextWarningKind::Emp, emp_retained.warning_kind);
	hud_start_text_flash(nullptr, 0);
}

TEST(TelemetryPhase3Threat,
	IncomingHomingMissileUsesTheInstalledManifestClassAndPublicIdentity)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	constexpr int MissileObjectIndex = MAX_OBJECTS - 4;
	constexpr int MissileWeaponIndex = MAX_WEAPONS - 2;
	const auto weapon_info_count = Weapon_info.size();
	Weapon_info.emplace_back();

	auto& missile_object = Objects[MissileObjectIndex];
	missile_object.clear();
	missile_object.type = OBJ_WEAPON;
	missile_object.instance = MissileWeaponIndex;
	missile_object.signature = 91;
	missile_object.orient = vmd_identity_matrix;
	missile_object.pos.xyz.z = 100.0F;
	list_append(&obj_used_list, &missile_object);

	auto& missile = Weapons[MissileWeaponIndex];
	missile = weapon{};
	missile.objnum = MissileObjectIndex;
	missile.weapon_info_index = static_cast<int>(weapon_info_count);
	missile.homing_object = Player_obj;
	missile_obj missile_list_entry{};
	missile_list_entry.objnum = MissileObjectIndex;
	list_append(&Missile_obj_list, &missile_list_entry);

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	phase2->raw_static_catalog.weapon_count = 1U;
	phase2->raw_static_catalog.weapon_definitions[0].weapon_capture_key = 17U;
	phase2->raw_static_catalog.weapon_engine_mapping_count = 1U;
	phase2->raw_static_catalog.weapon_engine_mappings[0] = {
		static_cast<std::uint32_t>(weapon_info_count) + 1U, 17U};
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3CatalogDependencies dependencies;
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::discover_phase3_catalog_dependencies(*phase2, dependencies));
	EXPECT_EQ(0U, dependencies.ship_class_count);
	ASSERT_EQ(1U, dependencies.weapon_count);
	EXPECT_EQ(17U, dependencies.weapon_source_keys[0]);
	telemetry::Phase2ManifestCandidate manifest;
	manifest.weapon_record_count = 1U;
	manifest.weapon_records[0].source_key = 17U;
	manifest.weapon_records[0].weapon_class_id = 9001U;
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	ASSERT_EQ(detail::Phase3EngineCollectStatus::Collected,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, &manifest, false, true},
			identities, *output, *scratch));
	ASSERT_EQ(1U, output->threat.incoming_missile_count);
	const auto& incoming = output->threat.incoming_missiles[0];
	EXPECT_NE(0U, incoming.entity_id);
	EXPECT_EQ(9001U, incoming.weapon_class_id);
	EXPECT_EQ(100.0F, incoming.position_world[2]);
	EXPECT_EQ(protocol::RadarVisibility::NotVisible, incoming.radar_visibility);

	list_remove(&Missile_obj_list, &missile_list_entry);
	list_remove(&obj_used_list, &missile_object);
	missile = weapon{};
	missile_object.clear();
	Weapon_info.resize(weapon_info_count);
}

TEST(TelemetryPhase3Bounds,
	NativeLockSourceAtMaximumPlusOneFailsBeforeAnyProjectionIsPublished)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	Player_ship->missile_locks.resize(telemetry::MaximumPhase3Locks + 1U);

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	ASSERT_EQ(detail::Phase3IdentityProvisionStatus::Ready,
		identities.provision());
	auto output = std::make_unique<telemetry::Phase3Projection>();
	auto scratch = std::make_unique<telemetry::Phase3Projection>();
	EXPECT_EQ(detail::Phase3EngineCollectStatus::SourceLimitExceeded,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, true, false},
			identities, *output, *scratch));
	EXPECT_EQ(0U, output->lock_count);
	Player_ship->missile_locks.clear();
}

TEST(TelemetryPhase3Bounds,
	NativeIncomingMissilesAtMaximumPlusOneFailBeforeAnyProjectionIsPublished)
{
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	constexpr auto count = telemetry::MaximumPhase3IncomingMissiles + 1U;
	static_assert(MAX_OBJECTS > static_cast<int>(count) + 4);
	static_assert(MAX_WEAPONS > static_cast<int>(count) + 4);
	std::array<missile_obj, count> entries{};
	for (std::size_t index = 0U; index < count; ++index) {
		const auto object_index = MAX_OBJECTS - 4 - static_cast<int>(index);
		const auto weapon_index = MAX_WEAPONS - 2 - static_cast<int>(index);
		auto& missile_object = Objects[object_index];
		missile_object.clear();
		missile_object.type = OBJ_WEAPON;
		missile_object.instance = weapon_index;
		missile_object.signature = static_cast<int>(100U + index);
		list_append(&obj_used_list, &missile_object);
		auto& missile = Weapons[weapon_index];
		missile = weapon{};
		missile.objnum = object_index;
		missile.homing_object = Player_obj;
		entries[index].objnum = object_index;
		list_append(&Missile_obj_list, &entries[index]);
	}

	auto phase2 = std::make_unique<detail::Phase2ObservationDto>();
	phase2->ships.resize(1U);
	phase2->ships[0].capture_key.value =
		static_cast<std::uint32_t>(Player_obj->signature);
	const telemetry::Phase2Wp05SubjectBinding binding{
		phase2->ships[0].capture_key, 1U};
	detail::Phase3IdentityRegistry identities;
	EXPECT_EQ(detail::Phase3IdentityProvisionStatus::Ready, identities.provision());
	telemetry::Phase3Projection output{};
	telemetry::Phase3Projection scratch{};
	EXPECT_EQ(detail::Phase3EngineCollectStatus::SourceLimitExceeded,
		detail::collect_phase3_engine_projection(
			{64U, 1U, phase2.get(), &binding, 1U, nullptr, false, true},
			identities, output, scratch));
	EXPECT_EQ(0U, output.threat.incoming_missile_count);
	EXPECT_EQ(0U, output.contact_count);

	for (std::size_t index = 0U; index < count; ++index) {
		const auto object_index = MAX_OBJECTS - 4 - static_cast<int>(index);
		const auto weapon_index = MAX_WEAPONS - 2 - static_cast<int>(index);
		list_remove(&Missile_obj_list, &entries[index]);
		list_remove(&obj_used_list, &Objects[object_index]);
		Weapons[weapon_index] = weapon{};
		Objects[object_index].clear();
	}
}

TEST(TelemetryPhase3CaptureContract,
	DestroyedTargetAndUnknownEnemyWeaponDoNotCloseTheLiveSession)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	auto fixture_storage = std::make_unique<NativeFixture>();
	auto& fixture = *fixture_storage;
	auto config = enabled_config();
	config.flight_hz = 1U;
	config.systems_hz = 20U;
	config.keyframe_seconds = 1U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started,
		fixture.start_requested(config, telemetry::Phase2Profile::CockpitSensors));
	const auto endpoint = peer(92U);
	ASSERT_NE(0U, establish_ready(fixture, endpoint, 92U, 100U, 920U, 1U, true));
	CountingEngineReadView player_view;
	MinimalPhase2EngineReadView phase2_view;
	constexpr std::uint64_t initial_sample = 1'000'000U;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({initial_sample, 1U, true}, player_view, &phase2_view));
	constexpr std::uint64_t bootstrap_sample = initial_sample + 50'000U;
	const auto sends_before_manifest = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample, 1U, true}, player_view, &phase2_view));
	ASSERT_NE(nullptr, slot(fixture));
	ASSERT_NE(0U, slot(fixture)->required_manifest_id);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 1U, 1U, true}, player_view, &phase2_view));
	std::size_t manifest_datagram = fixture.backend.sent.size();
	for (std::size_t index = sends_before_manifest;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::Manifest) {
			manifest_datagram = index;
			break;
		}
	}
	ASSERT_LT(manifest_datagram, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[manifest_datagram], endpoint, 925U)});
	const auto manifest_ack_tick = fixture.runtime.service_tick(
		{bootstrap_sample + 2U, 1U, true}, player_view, &phase2_view);
	const auto manifest_ack_diagnostic =
		NativePlayerAccess::last_phase2_failure_diagnostic(fixture.runtime);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, manifest_ack_tick)
		<< "stage=" << static_cast<unsigned>(manifest_ack_diagnostic.stage)
		<< " runtime=" << static_cast<unsigned>(manifest_ack_diagnostic.runtime_result)
		<< " phase3_block=" << static_cast<unsigned>(
			manifest_ack_diagnostic.phase3_diagnostic.block)
		<< " phase3_status=" << static_cast<unsigned>(
			manifest_ack_diagnostic.phase3_diagnostic.status)
		<< " phase3_image=" << static_cast<unsigned>(
			manifest_ack_diagnostic.phase3_image_status);
	const auto sends_before_initial_egress = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 3U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 4U, 1U, true}, player_view, &phase2_view));
	std::size_t initial_snapshot = fixture.backend.sent.size();
	for (std::size_t index = sends_before_initial_egress;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::FullSnapshot) {
			initial_snapshot = index;
			break;
		}
	}
	ASSERT_LT(initial_snapshot, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[initial_snapshot], endpoint, 926U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 5U, 1U, true}, player_view, &phase2_view));
	ASSERT_NE(nullptr, slot(fixture));
	ASSERT_EQ(detail::Phase1SnapshotProgress::Live, slot(fixture)->snapshot.progress());

	// Promotion to the reliable baseline needs one complete refresh.  ACK it so
	// the invalid Phase 3 source below is the only outstanding state change.
	const auto sends_before_settle = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 6U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 7U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 8U, 1U, true}, player_view, &phase2_view));
	std::size_t settle_snapshot = fixture.backend.sent.size();
	for (std::size_t index = sends_before_settle;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::FullSnapshot) {
			settle_snapshot = index;
			break;
		}
	}
	ASSERT_LT(settle_snapshot, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[settle_snapshot], endpoint, 927U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 9U, 1U, true}, player_view, &phase2_view));

	ASSERT_NE(nullptr, Player_ai);
	Player_ai->target_objnum = -1;
	Player_ai->target_signature = -1;
	const auto sends_before_capture = fixture.backend.sent.size();
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 1'000'000U, 1U, true}, player_view, &phase2_view));

	// Destroying the selected object clears TARGET_STATE on the next flight
	// sample. Retained systemsHz contacts remain a legal older sample and must
	// not turn a healthy cockpit stream into PermanentCaptureFailure.
	EXPECT_EQ(1U, fixture.runtime.active_sessions());
	EXPECT_NE(nullptr, slot(fixture));
	EXPECT_EQ(detail::Phase1SnapshotProgress::Live,
		slot(fixture)->snapshot.progress());
	for (std::size_t index = sends_before_capture;
		index < fixture.backend.sent.size(); ++index) {
		EXPECT_NE(protocol::MessageType::Manifest, sent_type(fixture.backend, index));
	}

	// A newly spawned incoming weapon may use a class that is absent from the
	// observer's current Phase 2 catalog.  It is a legal dynamic dependency:
	// omit that missile until its class is installed, but keep the session Live.
	constexpr int MissileObjectIndex = MAX_OBJECTS - 7;
	constexpr int MissileWeaponIndex = MAX_WEAPONS - 5;
	const auto weapon_info_count = Weapon_info.size();
	Weapon_info.emplace_back();
	auto& missile_object = Objects[MissileObjectIndex];
	missile_object.clear();
	missile_object.type = OBJ_WEAPON;
	missile_object.instance = MissileWeaponIndex;
	missile_object.signature = 1493;
	missile_object.orient = vmd_identity_matrix;
	missile_object.pos.xyz.z = 100.0F;
	list_append(&obj_used_list, &missile_object);
	auto& missile = Weapons[MissileWeaponIndex];
	missile = weapon{};
	missile.objnum = MissileObjectIndex;
	missile.weapon_info_index = static_cast<int>(weapon_info_count);
	missile.homing_object = Player_obj;
	missile_obj missile_list_entry{};
	missile_list_entry.objnum = MissileObjectIndex;
	list_append(&Missile_obj_list, &missile_list_entry);

	const auto sends_before_unknown_weapon = fixture.backend.sent.size();
	const auto unknown_weapon_tick = fixture.runtime.service_tick(
		{bootstrap_sample + 1'050'000U, 1U, true}, player_view, &phase2_view);
	const auto active_sessions_after_unknown_weapon =
		fixture.runtime.active_sessions();
	const auto slot_present_after_unknown_weapon = slot(fixture) != nullptr;
	const auto progress_after_unknown_weapon = slot(fixture) == nullptr
		? detail::Phase1SnapshotProgress::Synchronizing
		: slot(fixture)->snapshot.progress();

	list_remove(&Missile_obj_list, &missile_list_entry);
	list_remove(&obj_used_list, &missile_object);
	missile = weapon{};
	missile_object.clear();
	Weapon_info.resize(weapon_info_count);

	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, unknown_weapon_tick);
	EXPECT_EQ(1U, active_sessions_after_unknown_weapon);
	EXPECT_TRUE(slot_present_after_unknown_weapon);
	EXPECT_EQ(detail::Phase1SnapshotProgress::Live,
		progress_after_unknown_weapon);
	for (std::size_t index = sends_before_unknown_weapon;
		index < fixture.backend.sent.size(); ++index) {
		EXPECT_NE(protocol::MessageType::Manifest, sent_type(fixture.backend, index));
	}
}

TEST(TelemetryPhase3CaptureSchedule,
	NativeRuntimeSeparatesCadencesAndKeyframesUseOneCompleteSample)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto engine_globals = std::make_unique<Phase3EngineGlobalsScope>();
	detail::capture_phase2_main_thread_authority();
	auto fixture_storage = std::make_unique<NativeFixture>();
	auto& fixture = *fixture_storage;
	auto config = enabled_config();
	config.flight_hz = 1U;
	config.systems_hz = 20U;
	config.keyframe_seconds = 1U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started,
		fixture.start_requested(config, telemetry::Phase2Profile::CockpitSensors));
	const auto endpoint = peer(91U);
	ASSERT_NE(0U, establish_ready(fixture, endpoint, 91U, 100U, 910U, 1U, true));
	CountingEngineReadView player_view;
	MinimalPhase2EngineReadView phase2_view;
	constexpr std::uint64_t initial_sample = 1'000'000U;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({initial_sample, 1U, true}, player_view, &phase2_view));
	// The first capture materializes the canonical player binding. The next
	// systems deadline can then reconcile the Phase 2 closure and stage the
	// manifest against that same public identity.
	constexpr std::uint64_t bootstrap_sample = initial_sample + 50'000U;
	const auto sends_before_manifest = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample, 1U, true}, player_view, &phase2_view));
	auto* controller = NativePlayerAccess::controller(fixture.runtime);
	ASSERT_NE(nullptr, controller);
	ASSERT_NE(nullptr, slot(fixture));
	const auto phase2_capture =
		NativePlayerAccess::last_phase2_capture_result(fixture.runtime);
	ASSERT_EQ(detail::Phase2CaptureStatus::Valid, phase2_capture.status)
		<< "reason="
		<< static_cast<unsigned>(phase2_capture.reason);
	const auto manifest_id = slot(fixture)->required_manifest_id;
	ASSERT_NE(0U, manifest_id);
	EXPECT_FALSE(slot(fixture)->required_manifest_applied);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 1U, 1U, true}, player_view, &phase2_view));
	std::size_t manifest_datagram = fixture.backend.sent.size();
	for (std::size_t index = sends_before_manifest;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::Manifest) {
			manifest_datagram = index;
			break;
		}
	}
	ASSERT_LT(manifest_datagram, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[manifest_datagram], endpoint, 915U)});

	// The APPLIED manifest ACK consumes the staged intent, performs
	// a complete capture and starts the initial snapshot.  Its egress/ACK stays
	// on the normal transport path so periodic scheduling begins only once Live.
	const auto schedule_manifest_ack_tick = fixture.runtime.service_tick(
		{bootstrap_sample + 2U, 1U, true}, player_view, &phase2_view);
	const auto schedule_manifest_ack_diagnostic =
		NativePlayerAccess::last_phase2_failure_diagnostic(fixture.runtime);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		schedule_manifest_ack_tick)
		<< "stage=" << static_cast<unsigned>(
			schedule_manifest_ack_diagnostic.stage)
		<< " runtime=" << static_cast<unsigned>(
			schedule_manifest_ack_diagnostic.runtime_result)
		<< " phase3_block=" << static_cast<unsigned>(
			schedule_manifest_ack_diagnostic.phase3_diagnostic.block)
		<< " phase3_status=" << static_cast<unsigned>(
			schedule_manifest_ack_diagnostic.phase3_diagnostic.status)
		<< " phase3_image=" << static_cast<unsigned>(
			schedule_manifest_ack_diagnostic.phase3_image_status);
	ASSERT_NE(nullptr, slot(fixture));
	EXPECT_TRUE(slot(fixture)->required_manifest_applied);
	EXPECT_EQ(manifest_id, slot(fixture)->required_manifest_id);
	const auto sends_before_initial_egress = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 3U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 4U, 1U, true}, player_view, &phase2_view));
	std::size_t initial_snapshot = fixture.backend.sent.size();
	for (std::size_t index = sends_before_initial_egress;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::FullSnapshot) {
			initial_snapshot = index;
			break;
		}
	}
	ASSERT_LT(initial_snapshot, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[initial_snapshot], endpoint, 920U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 5U, 1U, true}, player_view, &phase2_view));
	ASSERT_NE(nullptr, slot(fixture));
	ASSERT_EQ(detail::Phase1SnapshotProgress::Live, slot(fixture)->snapshot.progress());

	// Promotion of a reliable baseline dirties the session state. Let that
	// forced complete refresh settle, then ACK its delta so it cannot mask the
	// independent cadence checks below.
	const auto sends_before_settle = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 6U, 1U, true}, player_view, &phase2_view));
	EXPECT_TRUE(NativePlayerAccess::phase2_capture_plan(fixture.runtime)
		.force_complete_keyframe);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 7U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 8U, 1U, true}, player_view, &phase2_view));
	std::size_t settle_snapshot = fixture.backend.sent.size();
	for (std::size_t index = sends_before_settle;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::FullSnapshot) {
			settle_snapshot = index;
			break;
		}
	}
	ASSERT_LT(settle_snapshot, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[settle_snapshot], endpoint, 921U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({bootstrap_sample + 9U, 1U, true}, player_view, &phase2_view));
	// systemsHz fires before flightHz.  Targeting must retain its flight sample,
	// while every systems-owned Phase 3 singleton receives the systems sample.
	constexpr std::uint64_t systems_sample = bootstrap_sample + 50'000U;
	const auto sends_before_systems_delta = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({systems_sample, 1U, true}, player_view, &phase2_view));
	// A systems-owned Phase 3 refresh changes the captured radar/system sample,
	// not its manifest closure.  In particular it must not re-stage a manifest
	// merely because the current track state was refreshed.
	for (std::size_t index = sends_before_systems_delta;
		index < fixture.backend.sent.size(); ++index)
		EXPECT_NE(protocol::MessageType::Manifest,
			sent_type(fixture.backend, index));
	EXPECT_EQ(manifest_id, slot(fixture)->required_manifest_id);
	EXPECT_TRUE(slot(fixture)->required_manifest_applied);
	const auto systems_plan = NativePlayerAccess::phase2_capture_plan(fixture.runtime);
	EXPECT_FALSE(systems_plan.phase3_refresh_targeting);
	EXPECT_TRUE(systems_plan.phase3_refresh_systems);
	ASSERT_NE(nullptr, slot(fixture));
	const auto& after_systems = slot(fixture)->snapshot.current_state();
	const auto baseline_sample = bootstrap_sample + 6U;
	EXPECT_EQ(baseline_sample,
		phase3_sample_time(after_systems, protocol::RecordType::LockState));
	EXPECT_EQ(baseline_sample,
		phase3_sample_time(after_systems, protocol::RecordType::TargetState));
	EXPECT_EQ(baseline_sample,
		phase3_sample_time(after_systems, protocol::RecordType::HudAlertState));
	for (const auto type : {protocol::RecordType::RadarState,
			 protocol::RecordType::ThreatState,
			 protocol::RecordType::CargoScanState,
			 protocol::RecordType::NavigationState})
		EXPECT_EQ(systems_sample, phase3_sample_time(after_systems, type));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({systems_sample + 1U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({systems_sample + 2U, 1U, true}, player_view, &phase2_view));
	std::size_t systems_delta = fixture.backend.sent.size();
	for (std::size_t index = sends_before_systems_delta;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::Delta) {
			systems_delta = index;
			break;
		}
	}
	ASSERT_LT(systems_delta, fixture.backend.sent.size());
	// The CockpitSensors image appends its radar/sensor records after the
	// Phase 2 rebuild set. Their current-state sample alone is insufficient:
	// they must also be present in this non-keyframe Delta.
	std::vector<std::uint8_t> systems_delta_storage;
	const auto systems_delta_view =
		decode_sent(fixture.backend, systems_delta, systems_delta_storage);
	protocol::DeltaPayload systems_delta_payload;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_delta_payload(
			systems_delta_view.payload, systems_delta_payload));
	protocol::RecordEnvelopeIterator records(systems_delta_payload.records,
		systems_delta_payload.record_count,
		protocol::RecordFlagPolicy::AllowV1Mutations);
	bool contains_threat_state = false;
	for (;;) {
		protocol::RecordEnvelopeView record;
		bool has_record = false;
		ASSERT_EQ(protocol::ValidationError::None,
			records.next(record, has_record));
		if (!has_record) break;
		contains_threat_state = contains_threat_state ||
			((record.record_flags & protocol::RecordFlagDelete) == 0U &&
				record.raw_record_type ==
					static_cast<std::uint16_t>(protocol::RecordType::ThreatState));
	}
	EXPECT_TRUE(contains_threat_state);
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[systems_delta], endpoint, 922U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({systems_sample + 3U, 1U, true}, player_view, &phase2_view));

	constexpr std::uint64_t flight_sample = bootstrap_sample + 1'000'000U;
	const auto sends_before_flight_delta = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({flight_sample, 1U, true}, player_view, &phase2_view));
	ASSERT_NE(nullptr, slot(fixture));
	const auto keyframe_sample = slot(fixture)->next_keyframe_due_us;
	ASSERT_GT(keyframe_sample, flight_sample);
	ASSERT_LT(keyframe_sample, flight_sample + 1'000'000U);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({flight_sample + 1U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({flight_sample + 2U, 1U, true}, player_view, &phase2_view));
	std::size_t flight_delta = fixture.backend.sent.size();
	for (std::size_t index = sends_before_flight_delta;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::Delta) {
			flight_delta = index;
			break;
		}
	}
	ASSERT_LT(flight_delta, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[flight_delta], endpoint, 923U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({flight_sample + 3U, 1U, true}, player_view, &phase2_view));
	const auto sends_before_keyframe = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({keyframe_sample, 1U, true}, player_view, &phase2_view));
	const auto keyframe_plan = NativePlayerAccess::phase2_capture_plan(fixture.runtime);
	EXPECT_TRUE(keyframe_plan.force_complete_keyframe);
	EXPECT_TRUE(keyframe_plan.phase3_refresh_targeting);
	EXPECT_TRUE(keyframe_plan.phase3_refresh_systems);
	ASSERT_NE(nullptr, slot(fixture));
	EXPECT_TRUE(slot(fixture)->snapshot.has_candidate());
	const auto& captured_keyframe = slot(fixture)->snapshot.current_state();
	for (const auto type : {protocol::RecordType::LockState,
			 protocol::RecordType::TargetState,
			 protocol::RecordType::HudAlertState,
			 protocol::RecordType::RadarState,
			 protocol::RecordType::ThreatState,
			 protocol::RecordType::CargoScanState,
			 protocol::RecordType::NavigationState})
		EXPECT_EQ(keyframe_sample, phase3_sample_time(captured_keyframe, type));

	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({keyframe_sample + 1U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({keyframe_sample + 2U, 1U, true}, player_view, &phase2_view));
	std::size_t keyframe_snapshot = fixture.backend.sent.size();
	for (std::size_t index = sends_before_keyframe;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::FullSnapshot) {
			keyframe_snapshot = index;
			break;
		}
	}
	ASSERT_LT(keyframe_snapshot, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[keyframe_snapshot], endpoint, 930U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({keyframe_sample + 3U, 1U, true}, player_view, &phase2_view));
	ASSERT_NE(nullptr, slot(fixture));
	ASSERT_EQ(detail::Phase1SnapshotProgress::Live, slot(fixture)->snapshot.progress());

	// A newly revealed class is a real catalog transition.  It must allocate a
	// strictly newer manifest and only publish the dependent keyframe after the
	// transport ACK has installed that manifest.
	phase2_view.source->identity.class_source_key.value = 2U;
	phase2_view.source->raw_static_references.class_capture_key = 2U;
	const auto definition_sample = keyframe_sample + 50'000U;
	const auto sends_before_definition = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample, 1U, true}, player_view, &phase2_view));
	ASSERT_NE(nullptr, slot(fixture));
	const auto definition_manifest_id = slot(fixture)->required_manifest_id;
	ASSERT_GT(definition_manifest_id, manifest_id);
	EXPECT_FALSE(slot(fixture)->required_manifest_applied);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample + 1U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample + 2U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample + 3U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample + 4U, 1U, true}, player_view, &phase2_view));
	std::size_t definition_manifest = fixture.backend.sent.size();
	for (std::size_t index = sends_before_definition;
		index < fixture.backend.sent.size(); ++index) {
		if (sent_type(fixture.backend, index) == protocol::MessageType::Manifest) {
			definition_manifest = index;
			break;
		}
	}
	ASSERT_LT(definition_manifest, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[definition_manifest], endpoint, 931U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample + 5U, 1U, true}, player_view, &phase2_view));
	ASSERT_NE(nullptr, slot(fixture));
	EXPECT_TRUE(slot(fixture)->required_manifest_applied);
	EXPECT_EQ(definition_manifest_id, slot(fixture)->required_manifest_id);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample + 6U, 1U, true}, player_view, &phase2_view));
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample + 7U, 1U, true}, player_view, &phase2_view));
	bool definition_keyframe_sent = false;
	std::size_t definition_keyframe = fixture.backend.sent.size();
	for (std::size_t index = definition_manifest + 1U;
		 index < fixture.backend.sent.size(); ++index) {
		definition_keyframe_sent = definition_keyframe_sent ||
			(sent_type(fixture.backend, index) == protocol::MessageType::FullSnapshot);
		if (sent_type(fixture.backend, index) ==
				protocol::MessageType::FullSnapshot)
			definition_keyframe = index;
	}
	EXPECT_TRUE(definition_keyframe_sent);
	ASSERT_LT(definition_keyframe, fixture.backend.sent.size());
	fixture.backend.receives.push_back({detail::IoStatus::Complete,
		ack_for(fixture.backend.sent[definition_keyframe], endpoint, 932U)});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick({definition_sample + 8U, 1U, true},
			player_view, &phase2_view));

	// Pausing is a cockpit state transition, not a transport transition. It
	// must update the existing live slot without waiting for another reliable
	// keyframe transaction: the outer game loop may stop immediately afterward.
	const auto session_before_pause = slot(fixture)->session_id;
	const auto pause_sample = definition_sample + 50'000U;
	const auto sends_before_pause = fixture.backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick(
			{pause_sample, 1U, true, true, 1.0F}, player_view, &phase2_view));
	ASSERT_NE(nullptr, slot(fixture));
	EXPECT_EQ(session_before_pause, slot(fixture)->session_id);
	EXPECT_TRUE(mission_paused(slot(fixture)->snapshot.current_state()));
	EXPECT_FALSE(NativePlayerAccess::phase2_capture_plan(fixture.runtime)
		.force_complete_keyframe);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick(
			{pause_sample + 1U, 1U, true, true, 1.0F}, player_view,
			&phase2_view));
	bool pause_delta_sent = false;
	bool pause_keyframe_sent = false;
	for (std::size_t index = sends_before_pause;
		 index < fixture.backend.sent.size(); ++index) {
		pause_delta_sent = pause_delta_sent ||
			(sent_type(fixture.backend, index) ==
			 protocol::MessageType::Delta);
		pause_keyframe_sent = pause_keyframe_sent ||
			(sent_type(fixture.backend, index) ==
			 protocol::MessageType::FullSnapshot);
	}
	EXPECT_TRUE(pause_delta_sent);
	EXPECT_FALSE(pause_keyframe_sent);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		fixture.runtime.service_tick(
			{pause_sample + 2U, 1U, true, false, 1.0F, true}, player_view,
			&phase2_view));
	EXPECT_FALSE(mission_paused(slot(fixture)->snapshot.current_state()));
	EXPECT_TRUE(NativePlayerAccess::phase2_capture_plan(fixture.runtime)
		.force_complete_keyframe);

}

TEST(TelemetryP91RuntimeMetricsContract, MetricsProvisionFailureFaultsBeforeBindAndSuccessfulRetryPublishesCallbacks)
{
	RuntimeCompositionServices failing;
	failing.budget.is_complete = true;
	failing.budget.deferred_categories = 0U;
	failing.metrics_provision_succeeds = false;
	detail::Runtime failed_runtime(failing);
	failed_runtime.capture_main_thread();
	failed_runtime.on_engine_update();
	EXPECT_EQ(detail::RuntimeState::Faulted, failed_runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::AllocationFailure, failed_runtime.terminal_reason());
	EXPECT_EQ(1U, failing.metrics_provision_calls);
	EXPECT_EQ(0U, failing.start_transport_calls);
	EXPECT_EQ(0U, failing.backend.open_calls)
		<< "Metrics provisioning is a pre-bind transaction and must fail closed.";

	RuntimeCompositionServices ready;
	ready.budget.is_complete = true;
	ready.budget.deferred_categories = 0U;
	detail::Runtime runtime(ready);
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	EXPECT_EQ(1U, ready.metrics_provision_calls);
	EXPECT_EQ(1U, ready.start_transport_calls);
	EXPECT_GT(ready.backend.open_calls, 0U);
	runtime.on_game_mission_load();
	const auto metrics = ready.metrics.snapshot();
	EXPECT_TRUE(metrics.provisioned);
	EXPECT_EQ(1U, metrics.callbacks[static_cast<std::size_t>(detail::TelemetryCallbackKind::GameMissionLoad)]);
}

TEST(TelemetryP91MetricsLifecycleContract, MissionLifecycleResetsMissionAndSessionMetricsThroughRuntime)
{
	RuntimeCompositionServices services;
	services.budget.is_complete = true;
	services.budget.deferred_categories = 0U;
	detail::Runtime runtime(services);
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	services.metrics.activate_session(0U, 7U);
	services.metrics.increment_session(0U, detail::TelemetryMetricCounter::HeartbeatProbes, 2U);
	services.metrics.set_current_player_entity_id(0x1234U);
	services.metrics.observe_mission(detail::TelemetryMetricHistogram::CaptureDuration, 25U);

	runtime.on_game_mission_load();
	runtime.on_engine_update();
	const auto metrics = services.metrics.snapshot();
	EXPECT_FALSE(metrics.sessions[0].active);
	EXPECT_EQ(0U, metrics.sessions[0].counters[static_cast<std::size_t>(detail::TelemetryMetricCounter::HeartbeatProbes)]);
	EXPECT_EQ(0U, metrics.current_player_entity_id);
	EXPECT_EQ(0U, metrics.mission_histograms[static_cast<std::size_t>(detail::TelemetryMetricHistogram::CaptureDuration)].count);
	EXPECT_EQ(2U, metrics.process_counters[static_cast<std::size_t>(detail::TelemetryMetricCounter::HeartbeatProbes)]);
}

TEST(TelemetryP91MetricsLifecycleContract, RejectedOffMainCallbackDoesNotMutateMetrics)
{
	RuntimeCompositionServices services;
	services.budget.is_complete = true;
	services.budget.deferred_categories = 0U;
	detail::Runtime runtime(services);
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	const auto callback = static_cast<std::size_t>(detail::TelemetryCallbackKind::GameMissionLoad);
	ASSERT_EQ(0U, services.metrics.snapshot().callbacks[callback]);

	std::thread worker([&] { runtime.on_game_mission_load(); });
	worker.join();
	EXPECT_EQ(0U, services.metrics.snapshot().callbacks[callback])
		<< "A callback rejected before lifecycle work must not alter metrics.";
}

TEST(TelemetryP91MetricsLifecycleContract, AcceptedCallbackPublishesNonZeroDurationIntoItsFixedHistogram)
{
	RuntimeCompositionServices services;
	services.budget.is_complete = true;
	services.budget.deferred_categories = 0U;
	detail::Runtime runtime(services);
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	runtime.on_game_mission_load();
	const auto metrics = services.metrics.snapshot();
	const auto& histogram = metrics.process_histograms[
		static_cast<std::size_t>(detail::TelemetryMetricHistogram::CallbackDuration)];
	EXPECT_EQ(2U, histogram.count);
	EXPECT_GT(histogram.sum_us, 0U);
	std::uint64_t bucket_total = 0U;
	for (const auto count : histogram.buckets) {
		bucket_total += count;
	}
	EXPECT_EQ(histogram.count, bucket_total)
		<< "Every accepted callback duration must be represented by exactly one fixed bucket.";
}

TEST(TelemetryP85SteadyStateAllocationContract, LiveCaptureKeyframeDeltaAndEgressAllocateNothing)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	auto* controller = NativePlayerAccess::controller(fixture->runtime);
	ASSERT_NE(nullptr, controller);
	const auto endpoint = peer(92U);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		native_tick(*fixture, {999U, 7U, true}));
	activate_live_baseline_without_acknowledging_session_begin(*controller, endpoint, 0x92U);

	// Warm-up is deliberately outside the observation window. The tracked path
	// starts with an already-live slot and exercises the P8 mutation/egress work.
	NativePlayerAccess::begin_steady_state_allocation_tracking(fixture->runtime);
	NativePlayerAccess::force_steady_state_allocation_for_tests(fixture->runtime);
	EXPECT_EQ(1U, NativePlayerAccess::steady_state_allocation_count(fixture->runtime))
		<< "The scoped counter must observe a forced runtime-owned allocation event.";
	NativePlayerAccess::begin_steady_state_allocation_tracking(fixture->runtime);
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 90'000U, true));
	for (const auto sample : std::array<float, 4U>{{10.0F, 14.0F, 10.0F, 14.0F}}) {
		const auto now = 90'001U + static_cast<std::uint64_t>((sample == 10.0F ? 0U : 1U));
		ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
			controller->replace_current_state(0U, phase1_state_image_at(sample, now)));
		ASSERT_TRUE(controller->queue_cumulative_delta(0U, now + 1U));
		ASSERT_EQ(1U, controller->service_delta_egress(1U, now + 2U));
		(void)pop_controller_packet(*controller);
	}
	EXPECT_EQ(0U, NativePlayerAccess::steady_state_allocation_count(fixture->runtime))
		<< "Capture and alternating 1/4 delta egresses must remain within startup-owned capacity.";
	NativePlayerAccess::begin_steady_state_allocation_tracking(fixture->runtime);
	controller->service_periodic(2'100'000U);
	while (controller->has_output()) {
		(void)pop_controller_packet(*controller);
	}
	ASSERT_EQ(1U, controller->service_initial_snapshot_egress(1U, 2'100'001U));
	const auto keyframe = pop_controller_packet(*controller);
	protocol::DatagramView keyframe_view;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(byte_view(keyframe.bytes),
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, keyframe_view));
	ASSERT_EQ(protocol::MessageType::FullSnapshot, keyframe_view.header.message_type);
	EXPECT_NE(0U, static_cast<std::uint8_t>(keyframe_view.header.flags & protocol::MessageFlagAckRequired));
	EXPECT_NE(0U, static_cast<std::uint8_t>(keyframe_view.header.flags & protocol::MessageFlagKeyframe));
	protocol::FullSnapshotPartPayload keyframe_payload;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_full_snapshot_part_payload(keyframe_view.payload, keyframe_payload));
	EXPECT_EQ(4U, keyframe_payload.record_count);

	controller->service_reliability(2'100'001U + protocol::ReliableDefaultRtoUs);
	ASSERT_TRUE(controller->has_output());
	const auto retransmission = pop_controller_packet(*controller);
	protocol::DatagramView retransmission_view;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(byte_view(retransmission.bytes),
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, retransmission_view));
	EXPECT_EQ(protocol::MessageType::FullSnapshot, retransmission_view.header.message_type);
	EXPECT_NE(0U, static_cast<std::uint8_t>(retransmission_view.header.flags & protocol::MessageFlagRetransmission));
	EXPECT_EQ(0U, NativePlayerAccess::steady_state_allocation_count(fixture->runtime))
		<< "Periodic keyframe egress and its RTO retransmission must use startup-owned storage.";
}

TEST(TelemetryP85StateImagePoolContract, ExhaustionFailsClosedAndReleasedBackingIsReusable)
{
	detail::Phase1StateImagePool pool;
	ASSERT_TRUE(pool.provision());
	detail::Phase1StateImageInput input{};
	input.producer_id = 0x1020304050607080ULL;
	input.negotiated_capability_generation = 1U;
	input.mission.mission_generation = 7U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {detail::CaptureStatus::Valid, detail::CaptureReason::None};
	input.player.entity_id = 42U;
	input.player.value.orientation_local_to_world = {1.0F, 0.0F, 0.0F, 0.0F};
	input.player.value.radius = 1.0F;

	std::array<protocol::StateImage, detail::Phase1StateImagePool::SlotsPerRecordSet> retained{};
	for (std::size_t index = 0U; index < retained.size(); ++index) {
		input.mission.producer_sample_time_us = 100U + index;
		input.player.value.producer_sample_time_us = 100U + index;
		input.player.value.position_world = {static_cast<float>(index), 2.0F, 3.0F};
		ASSERT_EQ(detail::Phase1StateImageBuildStatus::Created,
			detail::build_phase1_state_image_preallocated(input, pool, retained[index]));
		ASSERT_FALSE(retained[index].records().empty());
	}
	const auto first_record_count = retained[0U].records().size();
	protocol::StateImage rejected;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::AllocationFailed,
		detail::build_phase1_state_image_preallocated(input, pool, rejected));
	EXPECT_TRUE(rejected.records().empty());
	EXPECT_EQ(first_record_count, retained[0U].records().size())
		<< "Pool exhaustion must not mutate any image already retained by a baseline/candidate.";

	retained[2U] = {};
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::Created,
		detail::build_phase1_state_image_preallocated(input, pool, rejected));
	EXPECT_FALSE(rejected.records().empty());
}

TEST(TelemetryNativeRuntimeIntegrationContract, RuntimeCompositionMapsCaptureSeparatelyFromTransportExhaustively)
{
	EXPECT_EQ(0U, static_cast<std::uint8_t>(RuntimeCompositionServices::map_native_tick_status(
			detail::NativeSessionTickStatus::Complete)));
	EXPECT_EQ(1U, static_cast<std::uint8_t>(RuntimeCompositionServices::map_native_tick_status(
			detail::NativeSessionTickStatus::Unavailable)));
	EXPECT_EQ(2U, static_cast<std::uint8_t>(RuntimeCompositionServices::map_native_tick_status(
			detail::NativeSessionTickStatus::PermanentTransportFailure)));
	EXPECT_EQ(3U, static_cast<std::uint8_t>(RuntimeCompositionServices::map_native_tick_status(
			detail::NativeSessionTickStatus::PermanentCaptureFailure)));
	EXPECT_EQ(3U, static_cast<std::uint8_t>(RuntimeCompositionServices::map_native_tick_status(
			static_cast<detail::NativeSessionTickStatus>(0xffU))));
}

TEST(TelemetryP92RuntimeLoggingContract, MissionReplacementLogsOldLeaveBeforeNewEnter)
{
	RuntimeCompositionServices services;
	services.budget.is_complete = true;
	services.budget.deferred_categories = 0U;
	detail::Runtime runtime(services);
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());

	runtime.on_game_mission_load();
	runtime.on_engine_update();
	runtime.on_game_mission_load();
	runtime.on_engine_update();

	EXPECT_EQ((std::vector<std::pair<char, std::uint32_t>>{{'E', 1U}, {'L', 1U}, {'E', 2U}}),
		services.lifecycle_log)
		<< "A replacement must close the old mission aggregate before opening the new generation.";
}

TEST(TelemetryP92RuntimeLoggingContract, ClosedConfigErrorsUseTheRequiredStructuredReasonFamilies)
{
	EXPECT_EQ(detail::TelemetryLogReason::ConfigSchema,
		detail::RuntimeAdapterPlayerTestAccess::config_log_reason(telemetry::ConfigError::InvalidJson));
	EXPECT_EQ(detail::TelemetryLogReason::ConfigRange,
		detail::RuntimeAdapterPlayerTestAccess::config_log_reason(telemetry::ConfigError::OutOfRange));
	EXPECT_EQ(detail::TelemetryLogReason::ConfigSecurity,
		detail::RuntimeAdapterPlayerTestAccess::config_log_reason(telemetry::ConfigError::UnsafeExposure));
	EXPECT_EQ(detail::TelemetryLogReason::ConfigProfile,
		detail::RuntimeAdapterPlayerTestAccess::config_log_reason(telemetry::ConfigError::ReadFailure));
}

TEST(TelemetryP92AdapterLoggingContract, ActualAdapterActivationIsOneShotAndDeclaresProtocolV11)
{
	detail::RuntimeAdapterDiagnosticsTestAccess::reset_for_test();
	detail::RuntimeAdapterDiagnosticsTestAccess::emit_activation_for_test(42U);
	detail::RuntimeAdapterDiagnosticsTestAccess::emit_activation_for_test(43U);
	const auto snapshot = detail::RuntimeAdapterDiagnosticsTestAccess::log_snapshot();
	ASSERT_EQ(1U, snapshot.count);
	const auto& record = snapshot.records[0];
	EXPECT_EQ(detail::TelemetryLogEvent::Activated, record.event);
	EXPECT_EQ(detail::TelemetryLogLevel::Info, record.level);
	EXPECT_EQ(1U, record.protocol_major);
	EXPECT_EQ(protocol::VersionMinorV1_1, record.protocol_minor);
}

TEST(TelemetryP92AdapterLoggingContract, EveryRuntimeFaultMapsToOneClosedOneShotNumericRecord)
{
	const std::array<std::pair<detail::RuntimeTerminalReason, detail::TelemetryLogFault>, 15U> cases{{
		{detail::RuntimeTerminalReason::None, detail::TelemetryLogFault::None},
		{detail::RuntimeTerminalReason::ConfigAbsent, detail::TelemetryLogFault::Config},
		{detail::RuntimeTerminalReason::ConfigInvalid, detail::TelemetryLogFault::Config},
		{detail::RuntimeTerminalReason::ConfigDisabled, detail::TelemetryLogFault::Config},
		{detail::RuntimeTerminalReason::MainThreadNotCaptured, detail::TelemetryLogFault::MainThread},
		{detail::RuntimeTerminalReason::MainThreadViolation, detail::TelemetryLogFault::MainThread},
		{detail::RuntimeTerminalReason::ProducerIdentityFailure, detail::TelemetryLogFault::IdentityStore},
		{detail::RuntimeTerminalReason::SessionCandidateFailure, detail::TelemetryLogFault::Entropy},
		{detail::RuntimeTerminalReason::BudgetFailure, detail::TelemetryLogFault::BudgetOverflow},
		{detail::RuntimeTerminalReason::AllocationFailure, detail::TelemetryLogFault::Allocation},
		{detail::RuntimeTerminalReason::SessionRegistrationFailure, detail::TelemetryLogFault::SessionRegistration},
		{detail::RuntimeTerminalReason::TransportUnavailable, detail::TelemetryLogFault::Socket},
		{detail::RuntimeTerminalReason::InvalidLifecycleTransition, detail::TelemetryLogFault::Lifecycle},
		{detail::RuntimeTerminalReason::MissionGenerationOverflow, detail::TelemetryLogFault::Lifecycle},
		{detail::RuntimeTerminalReason::CaptureFailure, detail::TelemetryLogFault::Capture},
	}};

	for (const auto& entry : cases) {
		detail::RuntimeAdapterDiagnosticsTestAccess::reset_for_test();
		detail::RuntimeAdapterDiagnosticsTestAccess::emit_fault_for_test(entry.first);
		detail::RuntimeAdapterDiagnosticsTestAccess::emit_fault_for_test(entry.first);
		const auto snapshot = detail::RuntimeAdapterDiagnosticsTestAccess::log_snapshot();
		ASSERT_EQ(1U, snapshot.count);
		const auto& record = snapshot.records[0];
		EXPECT_EQ(detail::TelemetryLogEvent::TransportFault, record.event);
		EXPECT_EQ(detail::TelemetryLogLevel::Error, record.level);
		EXPECT_EQ(entry.second, record.fault);
		EXPECT_EQ(entry.second, detail::RuntimeAdapterDiagnosticsTestAccess::runtime_fault_log(entry.first));
		EXPECT_EQ(entry.first == detail::RuntimeTerminalReason::TransportUnavailable
				? detail::TelemetryLogReason::Bind
				: detail::TelemetryLogReason::ProtocolError,
			record.reason);
		EXPECT_LT(static_cast<std::uint8_t>(record.event), static_cast<std::uint8_t>(detail::TelemetryLogEvent::Count));
		EXPECT_LE(static_cast<std::uint8_t>(record.level), static_cast<std::uint8_t>(detail::TelemetryLogLevel::Error));
		EXPECT_LT(static_cast<std::uint8_t>(record.reason), static_cast<std::uint8_t>(detail::TelemetryLogReason::Count));
		EXPECT_LT(static_cast<std::uint8_t>(record.fault), static_cast<std::uint8_t>(detail::TelemetryLogFault::Count));
	}
}

TEST(TelemetryNativeRuntimeIntegrationContract, BudgetGateConstructsNativeStackOnlyInsideSuccessfulStartTransport)
{
	RuntimeCompositionServices real_services;
	ASSERT_EQ(detail::StartupBudgetError::None, real_services.budget.error);
	ASSERT_FALSE(real_services.budget.is_complete);
	ASSERT_EQ(0x0080U, real_services.budget.deferred_categories)
		<< "P8.5 prices baseline, delta and serialization storage; Metrics is the sole deferred category.";
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

TEST(TelemetryRuntimeAdapterPlayerContract, StopCollectionDiffersFromMissionPurgeAndRearmsImmediately)
{
	if (!detail::RuntimeAdapterPlayerPublicProbe::contract_available()) {
		FAIL() << "WP07-D4 RED: the production runtime-adapter test access to the unique local-FSO-view "
				  "helper is absent.";
		return;
	}
	RuntimeCompositionServices services;
	services.use_runtime_adapter_helper = true;
	services.budget.is_complete = true;
	services.budget.deferred_categories = 0U;
	detail::Runtime runtime(services);
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	ASSERT_NE(nullptr, services.native.get());
	runtime.on_game_mission_load();
	runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::MissionActive, runtime.state());

	auto drive = [&](std::uint64_t now_us) {
		services.clock_now_us = now_us;
		runtime.on_engine_update();
	};
	auto establish = [&](protocol::EndpointKey endpoint,
		std::uint64_t nonce,
		std::uint64_t now_us,
		std::uint32_t packet_base) {
		const auto sessions_before = services.native->active_sessions();
		auto drive_new_send = [&](std::size_t previous, std::uint64_t first_tick) {
			for (std::uint64_t offset = 0U; offset < 8U && services.backend.sent.size() == previous; ++offset) {
				drive(first_tick + offset);
			}
		};
		services.backend.receives.push_back(
			{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, nonce, endpoint)});
		drive_new_send(services.backend.sent.size(), now_us);
		EXPECT_FALSE(services.backend.sent.empty());
		const auto welcome_index = services.backend.sent.size() - 1U;
		EXPECT_EQ(protocol::MessageType::Welcome, sent_type(services.backend, welcome_index));
		services.backend.receives.push_back(
			{detail::IoStatus::Complete,
				ack_for(services.backend.sent[welcome_index], endpoint, packet_base + 1U)});
		drive_new_send(services.backend.sent.size(), now_us + 10U);
		const auto begin_index = services.backend.sent.size() - 1U;
		EXPECT_EQ(protocol::MessageType::SessionBegin, sent_type(services.backend, begin_index));
		std::vector<std::uint8_t> stable;
		const auto begin = decode_sent(services.backend, begin_index, stable);
		services.backend.receives.push_back(
			{detail::IoStatus::Complete,
				ack_for(services.backend.sent[begin_index], endpoint, packet_base + 2U)});
		const auto final_ack = services.backend.receives.size() - 1U;
		for (std::uint64_t offset = 0U;
			offset < 8U && services.backend.receive_script_index <= final_ack;
			++offset) {
			drive(now_us + 20U + offset);
		}
		EXPECT_EQ(sessions_before + 1U, services.native->active_sessions());
		return begin.header.session_id;
	};

	const auto first_session = establish(peer(90U), 900U, 10'000U, 1'000U);
	ASSERT_NE(0U, first_session);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		NativePlayerAccess::inject_collected_player_capture(*services.native,
			{detail::CaptureStatus::Valid, detail::CaptureReason::None},
			observation(90U, 20'000U, 9.0f)));
	const auto* ready = NativePlayerAccess::slot(*services.native, 0U);
	ASSERT_NE(nullptr, ready);
	ASSERT_EQ(detail::ProducerSessionProgress::ReadyForState, ready->progress);
	ASSERT_EQ(first_session, ready->session_id);
	ASSERT_TRUE(ready->has_latest_player_sample);
	ASSERT_EQ(1U, ready->latest_player_sample.entity_id);
	ASSERT_EQ(1U, ready->player_entity_ids.last_allocated_entity_id());

	services.stop_collection();
	ready = NativePlayerAccess::slot(*services.native, 0U);
	ASSERT_NE(nullptr, ready);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, ready->progress);
	EXPECT_EQ(first_session, ready->session_id);
	EXPECT_EQ(1U, ready->player_entity_ids.last_allocated_entity_id());
	EXPECT_FALSE(ready->has_latest_player_sample);
	EXPECT_EQ(1U, services.native->active_sessions());
	EXPECT_EQ(1U, services.native->socket_count());

	services.teardown.clear();
	services.runtime_trace.clear();
	services.service_calls = 0U;
	runtime.on_game_mission_load();
	drive(30'000U);
	EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
	EXPECT_EQ((std::vector<char>{'1', '2', '3', '4', '5'}), services.teardown)
		<< "Mission update must stop collection and invalidate mission state without closing the transport.";
	EXPECT_EQ((std::vector<char>{'C', 'L', 'T'}), services.runtime_trace)
		<< "The helper-backed inactive tick follows the complete mission purge.";
	EXPECT_EQ(1U, services.service_calls);
	EXPECT_FALSE(services.last_tick.mission_active);
	EXPECT_EQ(runtime.mission_generation(), services.last_tick.mission_generation);
	EXPECT_EQ(1U, services.native->active_sessions())
		<< "The ended mission remains only long enough to complete SESSION_END or be replaced by a new HELLO.";
	EXPECT_EQ(1U, services.native->socket_count());
	runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	drive(35'000U);
	ASSERT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	ASSERT_FALSE(services.backend.sent.empty());
	const auto end_index = services.backend.sent.size() - 1U;
	ASSERT_EQ(protocol::MessageType::SessionEnd, sent_type(services.backend, end_index));
	services.backend.receives.push_back(
		{detail::IoStatus::Complete,
			ack_for(services.backend.sent[end_index], peer(90U), 1'999U)});
	for (std::uint64_t offset = 0U;
		offset < 8U && services.native->active_sessions() != 0U;
		++offset) {
		drive(35'001U + offset);
	}
	ASSERT_EQ(0U, services.native->active_sessions());
	// A living client keeps its UDP endpoint across missions and renews only
	// its nonce, so the producer atomically replaces the ended logical session.
	const auto second_session = establish(peer(90U), 901U, 40'000U, 2'000U);
	EXPECT_NE(0U, second_session);
	EXPECT_NE(first_session, second_session);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		NativePlayerAccess::inject_collected_player_capture(*services.native,
			{detail::CaptureStatus::Valid, detail::CaptureReason::None},
			observation(91U, 50'000U, 10.0f)));
	ready = NativePlayerAccess::slot(*services.native, 0U);
	ASSERT_NE(nullptr, ready);
	EXPECT_TRUE(ready->has_latest_player_sample);
	EXPECT_EQ(1U, ready->latest_player_sample.entity_id)
		<< "Mission purge resets the per-session entity-ID registry before immediate rearm.";
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
	auto timeout = std::make_unique<NativeFixture>();
	auto broad = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, timeout->start(broad));
	timeout->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 110U, peer(4U))});
	native_tick(*timeout, {1'000U, 1U, true});
	ASSERT_FALSE(timeout->backend.sent.empty());
	const auto welcome = timeout->backend.sent.back();
	timeout->backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(welcome, peer(4U), 111U)});
	native_tick(*timeout, {2'000U, 1U, true});
	ASSERT_GT(timeout->runtime.owned_usage().reliable_items, 0U);
	const auto sends_before_timeout = timeout->backend.send_calls;
	native_tick(*timeout, {2'000U + 10'000'000U, 1U, true});
	EXPECT_EQ(sends_before_timeout, timeout->backend.send_calls);
	EXPECT_EQ(detail::SessionControllerOwnedUsage{}, timeout->runtime.owned_usage());

	// 3. REL > periodic: client A is Ready with heartbeat due while client B has
	// a SESSION_BEGIN retry due.  The next emitted datagram must be B's byte-
	// identical reliable control, not A's heartbeat.
	auto priority = std::make_unique<NativeFixture>();
	auto two_clients = enabled_config(1U);
	two_clients.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, priority->start(two_clients));
	(void)establish_ready(*priority, peer(5U), 120U, 20'000U, 120U);
	const auto sent_before_second = priority->backend.sent.size();
	priority->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 121U, peer(6U))});
	for (std::uint64_t tick = 0U; tick < 8U && priority->backend.sent.size() == sent_before_second; ++tick) {
		native_tick(*priority, {21'000U + tick, 1U, true});
	}
	ASSERT_GT(priority->backend.sent.size(), sent_before_second);
	const auto second_welcome = priority->backend.sent[sent_before_second];
	const auto sent_before_begin = priority->backend.sent.size();
	priority->backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(second_welcome, peer(6U), 122U)});
	for (std::uint64_t tick = 0U; tick < 8U && priority->backend.sent.size() == sent_before_begin; ++tick) {
		native_tick(*priority, {22'000U + tick, 1U, true});
	}
	ASSERT_GT(priority->backend.sent.size(), sent_before_begin);
	ASSERT_EQ(protocol::MessageType::SessionBegin, sent_type(priority->backend, sent_before_begin));
	const auto simultaneous_due = 1'020'100U;
	const auto sent_before_due = priority->backend.sent.size();
	for (std::uint64_t offset = 0U; offset < 4U && priority->backend.sent.size() == sent_before_due; ++offset) {
		native_tick(*priority, {simultaneous_due + offset, 1U, true});
	}
	ASSERT_GT(priority->backend.sent.size(), sent_before_due);
	EXPECT_EQ(protocol::MessageType::SessionBegin,
		sent_type(priority->backend, sent_before_due));
	const auto sent_after_rel = priority->backend.sent.size();
	for (std::uint64_t offset = 4U; offset < 8U && priority->backend.sent.size() == sent_after_rel; ++offset) {
		native_tick(*priority, {simultaneous_due + offset, 1U, true});
	}
	ASSERT_GT(priority->backend.sent.size(), sent_after_rel);
	EXPECT_EQ(protocol::MessageType::Heartbeat,
		sent_type(priority->backend, sent_after_rel))
		<< "After the first due REL completes, the already-due heartbeat follows without waiting for another RTO.";

	// 4. Periodic > I/O: after persistent alternation selects egress first and
	// N=1, a due heartbeat consumes the sole syscall; an already queued HELLO
	// remains untouched until a later tick.
	auto periodic = std::make_unique<NativeFixture>();
	auto one = enabled_config(1U);
	one.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, periodic->start(one));
	(void)establish_ready(*periodic, peer(7U), 130U, 30'000U, 130U);
	periodic->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 131U, peer(8U))});
	const auto rx_before = periodic->backend.receive_script_index;
	const auto sent_before_periodic = periodic->backend.sent.size();
	// WELCOME proof is applied at 30'010.  The SESSION_BEGIN ACK consumed at
	// 30'020 makes the session Ready but does not re-anchor the periodic clock.
	constexpr std::uint64_t ExactPeriodicDueUs = 1'030'010U;
	native_tick(*periodic, {ExactPeriodicDueUs, 1U, true});
	ASSERT_GT(periodic->backend.sent.size(), sent_before_periodic);
	EXPECT_EQ(protocol::MessageType::Heartbeat,
		sent_type(periodic->backend, periodic->backend.sent.size() - 1U));
	EXPECT_EQ(rx_before, periodic->backend.receive_script_index)
		<< "Periodic scheduling precedes and consumes the N=1 I/O opportunity.";
}

TEST(TelemetryPhase1DeltaEgressContract, RuntimeQueuesDeltaOnlyAfterReliableAndHeartbeatTail)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(1U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	auto* controller = NativePlayerAccess::controller(fixture->runtime);
	ASSERT_NE(nullptr, controller);
	const auto endpoint = peer(91U);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		native_tick(*fixture, {999U, 7U, true}));
	activate_live_baseline_without_acknowledging_session_begin(*controller, endpoint, 0x91U);
	ASSERT_GT(controller->slot(0U).reliable_items_in_use, 0U);
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller->replace_current_state(0U, phase1_state_image_at(9.0F, 5'000U)));
	ASSERT_TRUE(controller->queue_cumulative_delta(0U, 5'001U));

	const auto first = fixture->backend.sent.size();
	for (std::uint64_t offset = 0U; offset < 8U && fixture->backend.sent.size() == first; ++offset) {
		ASSERT_EQ(detail::NativeSessionTickStatus::Complete, native_tick(*fixture, {1'002'000U + offset, 7U, true}));
	}
	ASSERT_EQ(first + 1U, fixture->backend.sent.size());
	EXPECT_EQ(protocol::MessageType::SessionBegin, sent_type(fixture->backend, first))
		<< "A due reliable control retransmission must precede DELTA.";

	for (std::uint64_t offset = 8U; offset < 16U && fixture->backend.sent.size() == first + 1U; ++offset) {
		ASSERT_EQ(detail::NativeSessionTickStatus::Complete, native_tick(*fixture, {1'002'000U + offset, 7U, true}));
	}
	ASSERT_EQ(first + 2U, fixture->backend.sent.size());
	EXPECT_EQ(protocol::MessageType::Heartbeat, sent_type(fixture->backend, first + 1U))
		<< "DELTA remains tail traffic: a due heartbeat must precede a delta queued by the prior idle tail.";

	// The superseded non-reliable delta may be discarded while yielding the
	// output slot; P8.3 only permits it to occupy the idle tail, never to delay
	// a due reliable/control/heartbeat datagram.
}

TEST(TelemetryPhase1DeltaEgressContract, RealRuntimeEventuallyEmitsQueuedDeltaAfterPriorityWorkDrains)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	auto* controller = NativePlayerAccess::controller(fixture->runtime);
	ASSERT_NE(nullptr, controller);
	const auto endpoint = peer(93U);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		native_tick(*fixture, {999U, 7U, true}));
	// This establishes the same live baseline used by the native allocation
	// contract, then drives the actual runtime R2 scheduler rather than invoking
	// controller egress directly.
	activate_live_baseline_without_acknowledging_session_begin(*controller, endpoint, 0x93U);
	while (controller->has_output()) (void)pop_controller_packet(*controller);
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller->replace_current_state(0U, phase1_state_image_at(12.0F, 6'000U)));
	ASSERT_TRUE(controller->queue_cumulative_delta(0U, 6'001U));
	const auto sent_before = fixture->backend.sent.size();
	bool delta_emitted = false;
	for (std::uint64_t offset = 0U; offset < 32U && !delta_emitted; ++offset) {
		ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
			native_tick(*fixture, {6'100U + offset, 7U, true}));
		for (std::size_t index = sent_before; index < fixture->backend.sent.size(); ++index) {
			delta_emitted = delta_emitted || sent_type(fixture->backend, index) == protocol::MessageType::Delta;
		}
	}
	EXPECT_TRUE(delta_emitted)
		<< "Once no reliable/control/heartbeat work is due, queued DELTA must reach the real transport scheduler.";
}

TEST(TelemetryPhase1DeltaEgressContract, LiveStateCannotStarveAPrewarmedSessionBegin)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(1U);
	config.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	auto* controller = NativePlayerAccess::controller(fixture->runtime);
	ASSERT_NE(nullptr, controller);
	const auto live_endpoint = peer(94U);
	const auto waiting_endpoint = peer(95U);
	ASSERT_NE(0U, establish_live_controller_session(
		*controller, live_endpoint, 0x940U, 10'000U, 1'000U));
	const auto waiting_session_id = establish_prewarmed_controller_session(
		*controller, waiting_endpoint, 0x950U, 20'000U, 2'000U);
	ASSERT_NE(0U, waiting_session_id);
	ASSERT_EQ(detail::ProducerSessionProgress::ReadyForState,
		controller->slot(0U).progress);
	ASSERT_EQ(detail::ProducerSessionProgress::Prewarmed,
		controller->slot(1U).progress);

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller->replace_current_state(0U,
			phase1_state_image_at(9.0F, 30'000U)));
	ASSERT_TRUE(controller->queue_cumulative_delta(0U, 30'001U));
	CountingEngineReadView view;
	const auto first_send = fixture->backend.sent.size();
	const auto period = independent_period_us(config.flight_hz);
	std::size_t waiting_begin = fixture->backend.sent.size();
	for (std::uint64_t tick = 0U; tick < 12U &&
		waiting_begin == fixture->backend.sent.size(); ++tick) {
		const auto sent_before = fixture->backend.sent.size();
		ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
			capture_tick(*fixture, view, 1'000'000U + tick * period));
		EXPECT_LE(fixture->backend.sent.size() - sent_before, 1U)
			<< "max_datagrams_per_tick=1 remains a hard shared budget.";
		for (std::size_t index = sent_before;
			index < fixture->backend.sent.size(); ++index) {
			if (sent_type(fixture->backend, index) ==
					protocol::MessageType::SessionBegin &&
				fixture->backend.send_endpoints[index] == waiting_endpoint) {
				waiting_begin = index;
				break;
			}
		}
	}

	ASSERT_EQ(detail::ProducerSessionProgress::ReadyForState,
		controller->slot(1U).progress)
		<< "Continuous state capture from the live slot must not leave the other slot Prewarmed.";
	ASSERT_LT(waiting_begin, fixture->backend.sent.size());
	std::vector<std::uint8_t> stable;
	EXPECT_EQ(waiting_session_id,
		decode_sent(fixture->backend, waiting_begin, stable).header.session_id)
		<< "Prewarm activation preserves the negotiated transport session.";
	for (std::size_t index = first_send + 1U; index < waiting_begin; ++index) {
		const auto type = sent_type(fixture->backend, index);
		EXPECT_NE(protocol::MessageType::Delta, type);
		EXPECT_NE(protocol::MessageType::FullSnapshot, type)
			<< "After the already-queued output drains, SESSION_BEGIN precedes newly prepared state.";
	}
}

TEST(TelemetryNativeRuntimeIntegrationContract, OneDatagramBudgetActivatesEveryPrewarmedClient)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(1U);
	config.max_clients = 4U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	auto* controller = NativePlayerAccess::controller(fixture->runtime);
	ASSERT_NE(nullptr, controller);
	std::array<std::uint64_t, 4U> session_ids{};
	for (std::size_t index = 0U; index < session_ids.size(); ++index) {
		session_ids[index] = establish_prewarmed_controller_session(
			*controller,
			peer(static_cast<std::uint8_t>(100U + index)),
			0xa00U + index,
			40'000U + index * 10U,
			3'000U + static_cast<std::uint32_t>(index));
		ASSERT_NE(0U, session_ids[index]);
	}

	const auto first_send = fixture->backend.sent.size();
	for (std::uint64_t tick = 0U; tick < 16U; ++tick) {
		const auto sent_before = fixture->backend.sent.size();
		ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
			native_tick(*fixture, {50'000U + tick, 7U, true}));
		EXPECT_LE(fixture->backend.sent.size() - sent_before, 1U);
	}

	std::array<bool, 4U> observed{};
	for (std::size_t index = first_send; index < fixture->backend.sent.size(); ++index) {
		if (sent_type(fixture->backend, index) != protocol::MessageType::SessionBegin)
			continue;
		std::vector<std::uint8_t> stable;
		const auto session_id = decode_sent(fixture->backend, index, stable).header.session_id;
		for (std::size_t slot_index = 0U; slot_index < session_ids.size(); ++slot_index)
			observed[slot_index] = observed[slot_index] ||
				session_id == session_ids[slot_index];
	}
	for (std::size_t index = 0U; index < session_ids.size(); ++index) {
		EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState,
			controller->slot(index).progress);
		EXPECT_TRUE(observed[index]);
	}
}

TEST(TelemetryNativeRuntimeIntegrationContract, SharedBudgetAlternatesAcrossTicksAndWouldBlockStopsOnlyOneDirection)
{
	for (const auto invalid_budget : {0U, 257U}) {
		auto invalid = std::make_unique<NativeFixture>();
		auto config = enabled_config(static_cast<std::uint16_t>(invalid_budget));
		EXPECT_EQ(detail::NativeSessionStartStatus::InvalidConfiguration, invalid->start(config));
		EXPECT_EQ(0U, invalid->backend.open_calls);
	}

	// With N=1 and both directions repeatedly made ready, the persistent first
	// direction alternation is externally visible as R,S,R,S across four ticks.
	auto alternating = std::make_unique<NativeFixture>();
	auto one = enabled_config(1U);
	one.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, alternating->start(one));
	alternating->backend.io_trace.clear();
	alternating->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 20U, peer(2U))});
	alternating->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 21U, peer(3U))});
	for (std::uint64_t tick = 0U; tick < 4U; ++tick) {
		native_tick(*alternating, {2'000U + tick, 0U, false});
	}
	EXPECT_EQ((std::vector<char>{'R', 'S', 'R', 'S'}), alternating->backend.io_trace);

	// Preload a real WELCOME without any send attempt: three rejected Complete
	// receives followed by the HELLO consume all N=4 units.  The next two ticks
	// then prove TX-WouldBlock -> RX progress and RX-WouldBlock -> TX progress.
	auto blocked = std::make_unique<NativeFixture>();
	auto four = enabled_config(4U);
	four.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, blocked->start(four));
	for (std::uint8_t source = 20U; source < 23U; ++source) {
		blocked->backend.receives.push_back({detail::IoStatus::Complete, Packet{{}, peer(source)}});
	}
	blocked->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 30U, peer(4U))});
	const auto setup_rx = blocked->backend.receive_calls;
	const auto setup_tx = blocked->backend.send_calls;
	native_tick(*blocked, {2'100U, 0U, false});
	EXPECT_EQ(4U, blocked->backend.receive_calls - setup_rx);
	EXPECT_EQ(0U, blocked->backend.send_calls - setup_tx);
	ASSERT_TRUE(blocked->runtime.owned_usage().output_queued);

	blocked->backend.sends.push_back(detail::IoStatus::WouldBlock);
	blocked->backend.receives.push_back({detail::IoStatus::Complete, Packet{{}, peer(23U)}});
	blocked->backend.io_trace.clear();
	const auto tx_block_rx = blocked->backend.receive_calls;
	const auto tx_block_tx = blocked->backend.send_calls;
	native_tick(*blocked, {2'101U, 0U, false});
	const auto tx_block_rx_delta = blocked->backend.receive_calls - tx_block_rx;
	const auto tx_block_tx_delta = blocked->backend.send_calls - tx_block_tx;
	ASSERT_FALSE(blocked->backend.io_trace.empty());
	EXPECT_EQ('S', blocked->backend.io_trace.front());
	EXPECT_EQ(1U, tx_block_tx_delta);
	EXPECT_GE(tx_block_rx_delta, 1U);
	EXPECT_LE(tx_block_rx_delta + tx_block_tx_delta, 4U);
	ASSERT_TRUE(blocked->runtime.owned_usage().output_queued);

	blocked->backend.io_trace.clear();
	blocked->backend.receives.push_back({detail::IoStatus::WouldBlock, {}});
	const auto rx_block_rx = blocked->backend.receive_calls;
	const auto rx_block_tx = blocked->backend.send_calls;
	native_tick(*blocked, {2'102U, 0U, false});
	const auto rx_block_rx_delta = blocked->backend.receive_calls - rx_block_rx;
	const auto rx_block_tx_delta = blocked->backend.send_calls - rx_block_tx;
	ASSERT_FALSE(blocked->backend.io_trace.empty());
	EXPECT_EQ('R', blocked->backend.io_trace.front());
	EXPECT_EQ(1U, rx_block_rx_delta);
	EXPECT_EQ(1U, rx_block_tx_delta);
	EXPECT_LE(rx_block_rx_delta + rx_block_tx_delta, 4U);
	EXPECT_FALSE(blocked->runtime.owned_usage().output_queued);
}

TEST(TelemetryNativeRuntimeIntegrationContract, AllowlistAndVersionNegotiationComposeWithoutDurableRejectedState)
{
	auto accepted = std::make_unique<NativeFixture>();
	auto accepted_config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, accepted->start(accepted_config));
	accepted->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 10U)});
	native_tick(*accepted, {3'000U, 0U, false});
	ASSERT_EQ(1U, accepted->backend.sent.size());
	EXPECT_EQ(protocol::MessageType::Welcome, sent_type(accepted->backend, 0U));
	EXPECT_EQ(1U, accepted->runtime.active_sessions());

	auto unsupported = std::make_unique<NativeFixture>();
	auto unsupported_config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, unsupported->start(unsupported_config));
	unsupported->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_0, 11U)});
	native_tick(*unsupported, {3'050U, 0U, false});
	ASSERT_EQ(1U, unsupported->backend.sent.size());
	std::vector<std::uint8_t> storage;
	const auto datagram = decode_sent(unsupported->backend, 0U, storage);
	ASSERT_EQ(protocol::VersionMinorV1_0, datagram.header.version_minor);
	ASSERT_EQ(protocol::MessageType::Welcome, datagram.header.message_type);
	EXPECT_EQ(0U, datagram.header.session_id);
	protocol::WelcomePayload welcome;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_welcome_payload(datagram.payload, welcome));
	EXPECT_EQ(protocol::WelcomeStatus::UnsupportedVersion, welcome.status);
	EXPECT_EQ(0U, welcome.selected_major);
	EXPECT_EQ(0U, welcome.selected_minor);
	const auto unsupported_usage = unsupported->runtime.owned_usage();
	EXPECT_EQ(0U, unsupported_usage.active_slots);
	EXPECT_EQ(0U, unsupported_usage.reliable_items);
	EXPECT_EQ(1U, unsupported_usage.cache_entries)
		<< "The bounded rejection replay cache is not a durable client slot or heavy REL window.";

	auto rejected = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, rejected->start(config));
	const auto usage_before = rejected->runtime.owned_usage();
	const auto id_random_before = rejected->ids_random.next;
	const auto packet_random_before = rejected->packet_random.next;
	rejected->backend.receives.push_back({detail::IoStatus::Complete,
		hello(protocol::VersionMinorV1_1, 99U, protocol::EndpointKey::from_ipv4({192U, 0U, 2U, 1U}, 43000U))});
	native_tick(*rejected, {3'100U, 0U, false});
	EXPECT_TRUE(rejected->backend.sent.empty());
	EXPECT_EQ(usage_before, rejected->runtime.owned_usage());
	EXPECT_EQ(id_random_before, rejected->ids_random.next);
	EXPECT_EQ(packet_random_before, rejected->packet_random.next);
}

TEST(TelemetryP92NativeLoggingContract, RealWouldBlockIngressIsAggregatedAtTheR2Cadence)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(1U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));

	fixture->backend.receives.push_back({detail::IoStatus::WouldBlock, {}});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, native_tick(*fixture, {1'000'000U, 0U, false}));
	const auto first = NativePlayerAccess::log_snapshot(fixture->runtime);
	ASSERT_GE(first.count, 2U);
	EXPECT_TRUE(std::any_of(first.records.begin(), first.records.begin() + first.count,
		[](const detail::TelemetryLogRecord& record) {
			return record.event == detail::TelemetryLogEvent::DropSummary &&
				record.drops[static_cast<std::size_t>(detail::TelemetryLogDrop::WouldBlock)] == 1U;
		}));

	fixture->backend.receives.push_back({detail::IoStatus::WouldBlock, {}});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, native_tick(*fixture, {1'999'999U, 0U, false}));
	EXPECT_EQ(first.count, NativePlayerAccess::log_snapshot(fixture->runtime).count)
		<< "The runtime must retain repeated drops until the next one-second aggregate window.";

	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, native_tick(*fixture, {2'000'000U, 0U, false}));
	const auto second = NativePlayerAccess::log_snapshot(fixture->runtime);
	EXPECT_EQ(first.count + 1U, second.count);
	EXPECT_EQ(detail::TelemetryLogEvent::DropSummary, second.records[second.count - 1U].event);
	EXPECT_GT(second.records[second.count - 1U].drops[static_cast<std::size_t>(detail::TelemetryLogDrop::WouldBlock)], 0U);
}

TEST(TelemetryP92NativeLoggingContract, RealSessionCloseSummarizesObservedBudgetAndRemainsSilentAfterShutdown)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	ASSERT_NE(0U, establish_ready(*fixture, peer(71U), 710U, 10'000U, 710U));

	fixture->runtime.shutdown();
	const auto closed = NativePlayerAccess::log_snapshot(fixture->runtime);
	EXPECT_TRUE(std::any_of(closed.records.begin(), closed.records.begin() + closed.count,
		[](const detail::TelemetryLogRecord& record) {
			return record.event == detail::TelemetryLogEvent::BudgetHighWater &&
				record.budget == detail::TelemetryLogBudget::StateImage && record.high_water != 0U;
		}));
	EXPECT_TRUE(std::any_of(closed.records.begin(), closed.records.begin() + closed.count,
		[](const detail::TelemetryLogRecord& record) {
			return record.event == detail::TelemetryLogEvent::SessionClosed &&
				record.reason == detail::TelemetryLogReason::Shutdown && record.value >= 10U;
		}));
	EXPECT_TRUE(std::any_of(closed.records.begin(), closed.records.begin() + closed.count,
		[](const detail::TelemetryLogRecord& record) {
			return record.event == detail::TelemetryLogEvent::BudgetSessionSummary &&
				record.budget == detail::TelemetryLogBudget::StateImage;
		}));

	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable, native_tick(*fixture, {20'000U, 0U, false}));
	EXPECT_EQ(closed.count, NativePlayerAccess::log_snapshot(fixture->runtime).count)
		<< "A stopped runtime must not emit per-tick diagnostics after its terminal summary.";
}

TEST(TelemetryP93NativePerformanceContract, ExplicitObservationMeasuresActualTicksAndSteadyResources)
{
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	CountingEngineReadView view;

	// The measurement seam is inert until explicitly armed after startup/warm-up.
	const auto inert = NativePlayerAccess::last_performance_sample(fixture->runtime);
	EXPECT_EQ(0U, inert.tick_duration_ns);
	EXPECT_EQ(0U, inert.collect_duration_ns);
	EXPECT_EQ(0U, inert.diff_duration_ns);
	EXPECT_EQ(0U, inert.network_duration_ns);
	NativePlayerAccess::begin_performance_observation(fixture->runtime);
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 90'000U, true));
	const auto first = NativePlayerAccess::last_performance_sample(fixture->runtime);
	EXPECT_GT(first.tick_duration_ns, 0U);
	EXPECT_GT(first.collect_duration_ns, 0U);
	EXPECT_GT(first.diff_duration_ns, 0U);
	EXPECT_EQ(0U, first.allocation_events);
	EXPECT_EQ(0U, first.baselines_active);

	// A real socket WouldBlock remains a measured network tick, but creates no
	// allocation or unbounded queue/baseline state in the steady observation.
	fixture->backend.receives.push_back({detail::IoStatus::WouldBlock, {}});
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 90'001U, true));
	const auto steady = NativePlayerAccess::last_performance_sample(fixture->runtime);
	EXPECT_GT(steady.tick_duration_ns, 0U);
	EXPECT_GT(steady.network_duration_ns, 0U);
	EXPECT_GT(steady.syscall_count, 0U);
	EXPECT_EQ(first.allocation_events, steady.allocation_events);
	EXPECT_LE(steady.baselines_active, config.max_clients);
	EXPECT_LE(steady.queue_depth, static_cast<std::size_t>(config.max_clients) * protocol::ReliableWindowMaximumEntries + 1U);
}

TEST(TelemetryNativeRuntimeIntegrationContract, PermanentReceiveClosedOrErrorPurgesAllAndNeverReopens)
{
	for (const auto status : {detail::IoStatus::Closed, detail::IoStatus::Error}) {
		auto fixture = std::make_unique<NativeFixture>();
		auto config = enabled_config(64U);
		config.max_clients = 4U;
		ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
		(void)establish_ready(*fixture, peer(10U), 200U, 40'000U, 200U);
		(void)establish_ready(*fixture, peer(11U), 201U, 41'000U, 210U);
		fixture->backend.receives.push_back(
			{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 202U, peer(12U))});
		fixture->backend.sends.push_back(detail::IoStatus::WouldBlock);
		native_tick(*fixture, {42'000U, 0U, false});
		const auto rich = fixture->runtime.owned_usage();
		ASSERT_GE(rich.active_slots, 3U);
		ASSERT_GT(rich.cache_entries, 0U);
		ASSERT_GT(rich.preproof_accounts, 0U);
		ASSERT_GT(rich.reliable_items, 0U);
		ASSERT_TRUE(rich.output_queued);
		const auto closes_before = fixture->backend.closed.size();
		fixture->backend.receives.push_back({status, {}});
		EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure,
			native_tick(*fixture, {42'001U, 0U, false}));
		EXPECT_EQ(0U, fixture->runtime.socket_count());
		EXPECT_EQ(detail::SessionControllerOwnedUsage{}, fixture->runtime.owned_usage());
		EXPECT_EQ(closes_before + 1U, fixture->backend.closed.size());
		const auto calls = fixture->backend.open_calls + fixture->backend.receive_calls + fixture->backend.send_calls;
		native_tick(*fixture, {42'002U, 0U, false});
		native_tick(*fixture, {42'003U, 0U, false});
		EXPECT_EQ(calls, fixture->backend.open_calls + fixture->backend.receive_calls + fixture->backend.send_calls);
		EXPECT_EQ(closes_before + 1U, fixture->backend.closed.size());
	}
}

TEST(TelemetryNativeRuntimeIntegrationContract, PermanentSendClosedOrErrorCompletesOnceThenPurgesGlobally)
{
	for (const auto status : {detail::IoStatus::Closed, detail::IoStatus::Error}) {
		auto fixture = std::make_unique<NativeFixture>();
		auto config = enabled_config(64U);
		config.max_clients = 4U;
		ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
		(void)establish_ready(*fixture, peer(13U), 210U, 50'000U, 220U);
		(void)establish_ready(*fixture, peer(14U), 211U, 51'000U, 230U);
		fixture->backend.receives.push_back(
			{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 212U, peer(15U))});
		fixture->backend.sends.push_back(detail::IoStatus::WouldBlock);
		native_tick(*fixture, {52'000U, 0U, false});
		const auto rich = fixture->runtime.owned_usage();
		ASSERT_GE(rich.active_slots, 3U);
		ASSERT_GT(rich.cache_entries, 0U);
		ASSERT_GT(rich.preproof_accounts, 0U);
		ASSERT_GT(rich.reliable_items, 0U);
		ASSERT_TRUE(rich.output_queued);
		const auto sends_before = fixture->backend.send_calls;
		const auto closes_before = fixture->backend.closed.size();
		const auto completions_before = fixture->completion.calls;
		fixture->backend.sends.push_back(status);
		EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure,
			native_tick(*fixture, {52'001U, 0U, false}));
		EXPECT_EQ(sends_before + 1U, fixture->backend.send_calls)
			<< "The selected controller output is completed exactly once on fatal send.";
		EXPECT_EQ(completions_before + 1U, fixture->completion.calls);
		EXPECT_EQ(status, fixture->completion.last_status);
		EXPECT_EQ(rich, fixture->completion.usage_before);
		EXPECT_FALSE(fixture->completion.usage_after.output_queued);
		EXPECT_EQ(rich.active_slots - 1U, fixture->completion.usage_after.active_slots);
		EXPECT_GT(fixture->completion.usage_after.active_slots, 0U)
			<< "The focused completion closes only its owner; global purge must happen afterwards.";
		EXPECT_EQ(0U, fixture->runtime.socket_count());
		EXPECT_EQ(detail::SessionControllerOwnedUsage{}, fixture->runtime.owned_usage());
		EXPECT_EQ(closes_before + 1U, fixture->backend.closed.size());
		native_tick(*fixture, {52'002U, 0U, false});
		native_tick(*fixture, {52'003U, 0U, false});
		EXPECT_EQ(sends_before + 1U, fixture->backend.send_calls);
		EXPECT_EQ(completions_before + 1U, fixture->completion.calls);
		EXPECT_EQ(closes_before + 1U, fixture->backend.closed.size());
		EXPECT_EQ(1U, fixture->backend.open_calls);
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
	auto fixture = std::make_unique<NativeFixture>();
	expect_default_current(NativePlayerProbe::current(fixture->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
	EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture->runtime));
	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable,
		NativePlayerAccess::inject_collected_player_capture(fixture->runtime,
			{detail::CaptureStatus::Valid, detail::CaptureReason::None},
			observation(42U, 1U)));
}

TEST(TelemetryNativePlayerCaptureContract, FlightRateStartBoundariesRejectBeforeOpenAndAcceptOneAndSixty)
{
	REQUIRE_NATIVE_PLAYER_D3();
	for (const auto rates : {std::pair<unsigned, unsigned>{0U, 1U},
			 std::pair<unsigned, unsigned>{61U, 60U}}) {
		auto fixture = std::make_unique<NativeFixture>();
		auto config = enabled_config();
		config.flight_hz = static_cast<std::uint8_t>(rates.first);
		EXPECT_EQ(detail::NativeSessionStartStatus::InvalidConfiguration, fixture->start(config));
		EXPECT_EQ(0U, fixture->backend.open_calls);
		EXPECT_EQ(0U, fixture->runtime.socket_count());
		EXPECT_EQ(0U, fixture->runtime.active_sessions());
		EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture->runtime));
		expect_default_current(NativePlayerProbe::current(fixture->runtime));
		expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
		config.flight_hz = static_cast<std::uint8_t>(rates.second);
		ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
		CountingEngineReadView view;
		EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 123'456U));
		EXPECT_EQ(1U, view.read_calls);
		EXPECT_EQ(CaptureValid, NativePlayerProbe::last_status(fixture->runtime));
	}
}

TEST(TelemetryNativePlayerCaptureContract, TransportOpenFailureIsColdRetryableAndFirstCaptureIsImmediate)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	fixture->backend.open_statuses = {detail::SocketOpenStatus::SocketCreationFailed,
		detail::SocketOpenStatus::Complete};
	auto config = enabled_config();
	EXPECT_EQ(detail::NativeSessionStartStatus::TransportUnavailable, fixture->start(config));
	EXPECT_EQ(0U, fixture->runtime.socket_count());
	EXPECT_EQ(0U, fixture->runtime.active_sessions());
	EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture->runtime));
	expect_default_current(NativePlayerProbe::current(fixture->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	CountingEngineReadView view;
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 777'777U));
	EXPECT_EQ(1U, view.read_calls);
	EXPECT_EQ(CaptureValid, NativePlayerProbe::last_status(fixture->runtime));
}

TEST(TelemetryNativePlayerCaptureContract, AppliedAckPrecedesSameDueTickMaterialization)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	const auto endpoint = peer(21U);
	fixture->backend.receives.push_back({detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 901U, endpoint)});
	native_tick(*fixture, {10'000U, 1U, true});
	ASSERT_FALSE(fixture->backend.sent.empty());
	const auto welcome_index = fixture->backend.sent.size() - 1U;
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(fixture->backend.sent[welcome_index], endpoint, 902U)});
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		NativePlayerProbe::service_tick(fixture->runtime, {10'010U, 1U, true}, view));
	ASSERT_EQ(1U, view.read_calls);
	ASSERT_GT(fixture->backend.sent.size(), welcome_index + 1U);
	EXPECT_EQ(protocol::MessageType::SessionBegin, sent_type(fixture->backend, fixture->backend.sent.size() - 1U));
	const auto* ready = slot(*fixture);
	ASSERT_NE(nullptr, ready);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, ready->progress);
	EXPECT_TRUE(ready->has_latest_player_sample);
	EXPECT_EQ(1U, ready->latest_player_sample.entity_id);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime), 1U, 0U, 1U, 0U, 0U, 0U, 0U);
	const auto due_current = NativePlayerProbe::current(fixture->runtime);
	const auto due_sample = ready->latest_player_sample;
	view.clear_counts();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 20'001U));
	EXPECT_EQ(0U, view.total_calls());
	EXPECT_EQ(CaptureNotDue, NativePlayerProbe::last_status(fixture->runtime));
	const auto not_due_current = NativePlayerProbe::current(fixture->runtime);
	EXPECT_EQ(due_current.available, not_due_current.available);
	EXPECT_EQ(due_current.result.status, not_due_current.result.status);
	EXPECT_EQ(due_current.result.reason, not_due_current.result.reason);
	expect_observation(not_due_current.observation, due_current.observation);
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
	ASSERT_NE(nullptr, slot(*fixture));
	ASSERT_TRUE(slot(*fixture)->has_latest_player_sample);
	expect_player_sample(slot(*fixture)->latest_player_sample, due_sample);

	auto not_due = std::make_unique<NativeFixture>();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, not_due->start(config));
	CountingEngineReadView arm;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*not_due, arm, 30'000U));
	expect_zero_materialization(NativePlayerProbe::materialization(not_due->runtime));
	arm.clear_counts();
	const auto not_due_endpoint = peer(22U);
	not_due->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 904U, not_due_endpoint)});
	native_tick(*not_due, {30'001U, 1U, true});
	ASSERT_FALSE(not_due->backend.sent.empty());
	const auto not_due_welcome = not_due->backend.sent.size() - 1U;
	not_due->backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(not_due->backend.sent[not_due_welcome], not_due_endpoint, 905U)});
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete,
		NativePlayerProbe::service_tick(not_due->runtime, {30'002U, 1U, true}, arm));
	ASSERT_EQ(1U, arm.read_calls)
		<< "WP07 strict ReadyForState RED: the transition must force exactly one immediate capture even "
			  "when the regular 30 Hz deadline is not due.";
	EXPECT_EQ(std::this_thread::get_id(), arm.last_read_thread)
		<< "The forced capture remains synchronous on the EngineUpdate/main thread.";
	ASSERT_NE(nullptr, slot(*not_due));
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(*not_due)->progress);
	EXPECT_EQ(CaptureValid, NativePlayerProbe::last_status(not_due->runtime));
	const auto current_after_ack = NativePlayerProbe::current(not_due->runtime);
	EXPECT_TRUE(current_after_ack.available);
	EXPECT_EQ(30'002U, current_after_ack.observation.value.producer_sample_time_us);
	EXPECT_TRUE(slot(*not_due)->has_latest_player_sample);
	EXPECT_EQ(30'002U, slot(*not_due)->latest_player_sample.value.producer_sample_time_us);
	expect_materialization(NativePlayerProbe::materialization(not_due->runtime), 1U, 0U, 1U, 0U, 0U, 0U, 0U);

	const auto forced_sample = slot(*not_due)->latest_player_sample;
	arm.clear_counts();
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*not_due, arm, 30'003U));
	EXPECT_EQ(0U, arm.total_calls()) << "ReadyForState is an edge trigger, not a capture loop.";
	EXPECT_EQ(CaptureNotDue, NativePlayerProbe::last_status(not_due->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(not_due->runtime));
	ASSERT_NE(nullptr, slot(*not_due));
	ASSERT_TRUE(slot(*not_due)->has_latest_player_sample);
	expect_player_sample(slot(*not_due)->latest_player_sample, forced_sample);

	arm.clear_counts();
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*not_due, arm, 30'004U, false));
	EXPECT_EQ(0U, arm.total_calls()) << "A ReadyForState edge never bypasses the out-of-mission no-op gate.";
	EXPECT_EQ(CaptureInactive, NativePlayerProbe::last_status(not_due->runtime));
}

TEST(TelemetryNativePlayerCaptureContract, MultipleReadyTransitionsInOneTickShareOneCapture)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	config.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 40'000U));
	view.clear_counts();

	const auto first_endpoint = peer(31U);
	const auto second_endpoint = peer(32U);
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 931U, first_endpoint)});
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 932U, second_endpoint)});
	native_tick(*fixture, {40'001U, 1U, true});
	std::vector<std::size_t> welcomes;
	for (std::size_t index = 0U; index < fixture->backend.sent.size(); ++index) {
		if (sent_type(fixture->backend, index) == protocol::MessageType::Welcome) welcomes.push_back(index);
	}
	ASSERT_EQ(2U, welcomes.size());
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete,
			ack_for(fixture->backend.sent[welcomes[0]], fixture->backend.send_endpoints[welcomes[0]], 933U)});
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete,
			ack_for(fixture->backend.sent[welcomes[1]], fixture->backend.send_endpoints[welcomes[1]], 934U)});

	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		NativePlayerProbe::service_tick(fixture->runtime, {40'020U, 1U, true}, view));
	EXPECT_EQ(1U, view.read_calls) << "All ReadyForState edges in one EngineUpdate share one canonical capture.";
	ASSERT_NE(nullptr, slot(*fixture, 0U));
	ASSERT_NE(nullptr, slot(*fixture, 1U));
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(*fixture, 0U)->progress);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(*fixture, 1U)->progress);
	EXPECT_TRUE(slot(*fixture, 0U)->has_latest_player_sample);
	EXPECT_TRUE(slot(*fixture, 1U)->has_latest_player_sample);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime), 2U, 0U, 2U, 0U, 0U, 0U, 0U);
}

TEST(TelemetryNativePlayerCaptureContract, EngineUpdateBuildsAndEgressesTheP8SnapshotWithinTheSharedBudget)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(1U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	const auto endpoint = peer(81U);
	ASSERT_NE(0U, establish_ready(*fixture, endpoint, 0xb81U, 80'000U, 1'800U, 7U, true));
	CountingEngineReadView view;
	const auto sent_before_capture = fixture->backend.sent.size();
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		capture_tick(*fixture, view, 81'000U, true));
	ASSERT_EQ(1U, view.read_calls) << "The D5/P8 image is sourced by exactly one main-thread EngineUpdate read.";
	ASSERT_NE(nullptr, slot(*fixture));
	EXPECT_TRUE(slot(*fixture)->snapshot.has_candidate())
		<< "A valid D5/P8.1 capture must start the controller-owned initial snapshot transaction.";
	EXPECT_LE(fixture->backend.sent.size() - sent_before_capture, 1U)
		<< "max_datagrams_per_tick=1 forbids a capture tick from overspending egress budget.";

	for (std::uint64_t now = 81'001U; now != 81'004U; ++now) {
		const auto sent_before = fixture->backend.sent.size();
		ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, now, true));
		EXPECT_LE(fixture->backend.sent.size() - sent_before, 1U)
			<< "Initial-snapshot egress must share the native per-tick send budget.";
	}

	std::size_t snapshot_index = fixture->backend.sent.size();
	for (std::size_t index = sent_before_capture; index < fixture->backend.sent.size(); ++index) {
		if (sent_type(fixture->backend, index) == protocol::MessageType::FullSnapshot) {
			snapshot_index = index;
			break;
		}
	}
	ASSERT_LT(snapshot_index, fixture->backend.sent.size())
		<< "EngineUpdate must drive the captured P8.1 image through begin_initial_snapshot and egress.";
	std::vector<std::uint8_t> stable;
	const auto snapshot = decode_sent(fixture->backend, snapshot_index, stable);
	EXPECT_NE(0U, static_cast<std::uint8_t>(snapshot.header.flags & protocol::MessageFlagAckRequired));
	EXPECT_NE(0U, static_cast<std::uint8_t>(snapshot.header.flags & protocol::MessageFlagKeyframe));
	protocol::FullSnapshotPartPayload payload;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_full_snapshot_part_payload(snapshot.payload, payload));
	EXPECT_EQ(4U, payload.record_count) << "The runtime egress must serialize the D5/P8.1 canonical player image.";
}

TEST(TelemetryNativePlayerCaptureContract, TerminalTransportFailureAfterReadyTransitionSuppressesCapture)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 50'000U));
	view.clear_counts();
	const auto endpoint = peer(33U);
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 941U, endpoint)});
	native_tick(*fixture, {50'001U, 0U, false});
	ASSERT_FALSE(fixture->backend.sent.empty());
	const auto welcome = fixture->backend.sent.size() - 1U;
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(fixture->backend.sent[welcome], endpoint, 942U)});
	fixture->backend.receives.push_back({detail::IoStatus::Error, {}});

	EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure,
		capture_tick(*fixture, view, 50'020U));
	EXPECT_EQ(0U, view.total_calls()) << "A terminal transport result aborts the tick before engine capture.";
}

TEST(TelemetryNativePlayerCaptureContract, ReadyTransitionDuringInactiveMissionDoesNotCapture)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 60'000U));
	view.clear_counts();
	const auto endpoint = peer(34U);
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 951U, endpoint)});
	native_tick(*fixture, {60'001U, 0U, false});
	ASSERT_FALSE(fixture->backend.sent.empty());
	const auto welcome = fixture->backend.sent.size() - 1U;
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(fixture->backend.sent[welcome], endpoint, 952U)});

	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 60'020U, false));
	EXPECT_EQ(0U, view.total_calls()) << "ReadyForState never bypasses the mission-active capture gate.";
	ASSERT_NE(nullptr, slot(*fixture));
	EXPECT_EQ(detail::ProducerSessionProgress::Prewarmed, slot(*fixture)->progress);
	EXPECT_EQ(CaptureInactive, NativePlayerProbe::last_status(fixture->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
}

TEST(TelemetryNativePlayerCaptureContract, OneSharedCaptureFansOutToReadyAndStaleSlots)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config(64U);
	config.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	(void)establish_ready(*fixture, peer(23U), 910U, 100'000U, 910U);
	(void)establish_ready(*fixture, peer(24U), 920U, 200'000U, 920U);
	const auto* first = slot(*fixture, 0U);
	const auto* second = slot(*fixture, 1U);
	ASSERT_NE(nullptr, first);
	ASSERT_NE(nullptr, second);
	ASSERT_LT(first->heartbeat.last_valid_network_activity_us,
		second->heartbeat.last_valid_network_activity_us);
	const auto first_stale_due = first->heartbeat.last_valid_network_activity_us +
		first->heartbeat.stale_timeout_us;
	const auto second_stale_due = second->heartbeat.last_valid_network_activity_us +
		second->heartbeat.stale_timeout_us;
	ASSERT_LT(first_stale_due, second_stale_due);
	native_tick(*fixture, {first_stale_due, 1U, true});
	ASSERT_EQ(detail::ProducerSessionProgress::Stale, slot(*fixture, 0U)->progress);
	ASSERT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(*fixture, 1U)->progress);
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete,
		capture_tick(*fixture, view, first_stale_due));
	EXPECT_EQ(1U, view.in_mission_calls);
	EXPECT_EQ(1U, view.read_calls);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime), 2U, 0U, 2U, 0U, 0U, 0U, 0U);
	for (std::size_t index = 0U; index < 2U; ++index) {
		const auto* current_slot = slot(*fixture, index);
		ASSERT_NE(nullptr, current_slot);
		EXPECT_TRUE(current_slot->has_latest_player_sample);
		EXPECT_EQ(1U, current_slot->latest_player_sample.entity_id);
		EXPECT_EQ(first_stale_due, current_slot->latest_player_sample.value.producer_sample_time_us);
	}
	view.input.position_world.x = 99.0f;
	EXPECT_FLOAT_EQ(1.0f, slot(*fixture, 0U)->latest_player_sample.value.position_world.x);
	EXPECT_FLOAT_EQ(1.0f, slot(*fixture, 1U)->latest_player_sample.value.position_world.x);
}

TEST(TelemetryNativePlayerCaptureContract, CadenceSkipsCatchUpAndAppliesExactlyOncePerDueTick)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	config.flight_hz = 30U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	(void)establish_ready(*fixture, peer(25U), 930U, 1'000U, 930U);
	CountingEngineReadView view;
	const auto period = independent_period_us(config.flight_hz);
	const std::uint64_t first = 50'000U;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, first));
	EXPECT_EQ(1U, view.read_calls);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime), 1U, 0U, 1U, 0U, 0U, 0U, 0U);
	(void)capture_tick(*fixture, view, first + period - 1U);
	EXPECT_EQ(1U, view.read_calls);
	const auto hitch = first + period * 10U;
	(void)capture_tick(*fixture, view, hitch);
	EXPECT_EQ(2U, view.read_calls);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime), 1U, 1U, 0U, 0U, 0U, 0U, 0U);
	(void)capture_tick(*fixture, view, hitch);
	(void)capture_tick(*fixture, view, hitch + period - 1U);
	EXPECT_EQ(2U, view.read_calls);
	(void)capture_tick(*fixture, view, hitch + period);
	EXPECT_EQ(3U, view.read_calls);
	EXPECT_EQ(1U, slot(*fixture)->latest_player_sample.entity_id);
}

TEST(TelemetryNativePlayerCaptureContract, InactiveClearsCurrentPlayerWithoutReadingAndReactivationAllocatesNewId)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	(void)establish_ready(*fixture, peer(32U), 991U, 1'000U, 991U);
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 10'000U, true));
	ASSERT_TRUE(slot(*fixture)->has_latest_player_sample);
	ASSERT_EQ(1U, slot(*fixture)->latest_player_sample.entity_id);
	view.clear_counts();
	(void)capture_tick(*fixture, view, 20'000U, false);
	(void)capture_tick(*fixture, view, 9'000'000U, false);
	EXPECT_EQ(0U, view.total_calls());
	EXPECT_EQ(CaptureInactive, NativePlayerProbe::last_status(fixture->runtime));
	expect_default_current(NativePlayerProbe::current(fixture->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
	ASSERT_FALSE(slot(*fixture)->has_latest_player_sample);
	EXPECT_EQ(1U, slot(*fixture)->player_entity_ids.last_allocated_entity_id());
	(void)capture_tick(*fixture, view, 10'000'000U, true);
	EXPECT_EQ(1U, view.read_calls);
	EXPECT_EQ(CaptureValid, NativePlayerProbe::last_status(fixture->runtime));
	ASSERT_TRUE(slot(*fixture)->has_latest_player_sample);
	EXPECT_EQ(2U, slot(*fixture)->latest_player_sample.entity_id);
	const auto calls = view.total_calls();
	(void)capture_tick(*fixture, view, 10'000'000U + independent_period_us(config.flight_hz) - 1U, true);
	EXPECT_EQ(calls, view.total_calls());
	EXPECT_EQ(CaptureNotDue, NativePlayerProbe::last_status(fixture->runtime));
}

TEST(TelemetryNativePlayerCaptureContract, LegalAbsenceAndInvalidSourceClearAndReappearanceGetsNewIds)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	config.flight_hz = 60U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	(void)establish_ready(*fixture, peer(26U), 940U, 1'000U, 940U);
	CountingEngineReadView view;
	const auto period = independent_period_us(config.flight_hz);
	std::uint64_t now = 100'000U;
	(void)capture_tick(*fixture, view, now);
	ASSERT_EQ(1U, slot(*fixture)->latest_player_sample.entity_id);
	view.player = false;
	(void)capture_tick(*fixture, view, now += period);
	EXPECT_EQ(CaptureNoPlayer, NativePlayerProbe::last_status(fixture->runtime));
	const auto no_player = NativePlayerProbe::current(fixture->runtime);
	EXPECT_TRUE(no_player.available);
	EXPECT_EQ(detail::CaptureStatus::NoPlayer, no_player.result.status);
	expect_observation(no_player.observation, {});
	EXPECT_FALSE(slot(*fixture)->has_latest_player_sample);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime), 1U, 0U, 0U, 1U, 0U, 0U, 0U);
	view.player = true;
	(void)capture_tick(*fixture, view, now += period);
	ASSERT_EQ(2U, slot(*fixture)->latest_player_sample.entity_id);
	view.object_is_ship = false;
	(void)capture_tick(*fixture, view, now += period);
	EXPECT_EQ(CaptureInvalidSource, NativePlayerProbe::last_status(fixture->runtime));
	const auto invalid = NativePlayerProbe::current(fixture->runtime);
	EXPECT_TRUE(invalid.available);
	EXPECT_EQ(detail::CaptureStatus::InvalidSource, invalid.result.status);
	expect_observation(invalid.observation, {});
	EXPECT_FALSE(slot(*fixture)->has_latest_player_sample);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime), 1U, 0U, 0U, 0U, 1U, 0U, 0U);
	view.object_is_ship = true;
	(void)capture_tick(*fixture, view, now += period);
	EXPECT_EQ(3U, slot(*fixture)->latest_player_sample.entity_id);
}

TEST(TelemetryNativePlayerCaptureContract, ExhaustiveCaptureResultPairsAcceptOnlyClosedContract)
{
	REQUIRE_NATIVE_PLAYER_D3();
	for (std::uint8_t status_value = 0U; status_value <= static_cast<std::uint8_t>(detail::CaptureStatus::Count);
		 ++status_value) {
		for (std::uint8_t reason_value = 0U; reason_value <= static_cast<std::uint8_t>(detail::CaptureReason::Count);
			 ++reason_value) {
			auto fixture = std::make_unique<NativeFixture>();
			auto config = enabled_config();
			ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
			(void)establish_ready(*fixture, peer(27U), 950U, 1'000U, 950U);
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
					NativePlayerAccess::inject_collected_player_capture(fixture->runtime, valid, seeded));
			}
			const detail::CaptureResult candidate{status, reason};
			const auto result = NativePlayerAccess::inject_collected_player_capture(fixture->runtime,
				candidate,
				observation(77U, 20U, 9.0f));
			if (valid_pair || no_player_pair || invalid_pair) {
				EXPECT_EQ(detail::NativeSessionTickStatus::Complete, result);
				const auto current = NativePlayerProbe::current(fixture->runtime);
				EXPECT_TRUE(current.available);
				EXPECT_EQ(status, current.result.status);
				EXPECT_EQ(reason, current.result.reason);
				EXPECT_EQ(valid_pair       ? CaptureValid
						  : no_player_pair ? CaptureNoPlayer
										   : CaptureInvalidSource,
					NativePlayerProbe::last_status(fixture->runtime));
				if (valid_pair) {
					expect_observation(current.observation, observation(77U, 20U, 9.0f));
					EXPECT_TRUE(slot(*fixture)->has_latest_player_sample);
				} else {
					expect_observation(current.observation, {});
					EXPECT_FALSE(slot(*fixture)->has_latest_player_sample);
				}
				EXPECT_EQ(1U, fixture->runtime.socket_count());
			} else {
				EXPECT_EQ(TickPermanentCaptureFailure, static_cast<std::uint8_t>(result));
				EXPECT_EQ(CaptureInvariantFailure, NativePlayerProbe::last_status(fixture->runtime));
				expect_default_current(NativePlayerProbe::current(fixture->runtime));
				expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
				EXPECT_EQ(0U, fixture->runtime.active_sessions());
				EXPECT_EQ(0U, fixture->runtime.socket_count());
				EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable,
					NativePlayerAccess::inject_collected_player_capture(fixture->runtime, valid, seeded));
				CountingEngineReadView fault_view;
				EXPECT_EQ(TickPermanentCaptureFailure,
					static_cast<std::uint8_t>(capture_tick(*fixture, fault_view, 30'000U)));
				EXPECT_EQ(TickPermanentCaptureFailure,
					static_cast<std::uint8_t>(native_tick(*fixture, {30'001U, 1U, true})));
				EXPECT_EQ(0U, fault_view.total_calls());
			}
		}
	}
}

TEST(TelemetryNativePlayerCaptureContract, ClockRegressionFailsClosedBeforeReadingAndStaysSticky)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	(void)establish_ready(*fixture, peer(28U), 960U, 1'000U, 960U);
	CountingEngineReadView view;
	ASSERT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 100'000U));
	view.clear_counts();
	EXPECT_EQ(TickPermanentCaptureFailure, static_cast<std::uint8_t>(capture_tick(*fixture, view, 99'999U)));
	EXPECT_EQ(0U, view.total_calls());
	EXPECT_EQ(CaptureCadenceFailure, NativePlayerProbe::last_status(fixture->runtime));
	EXPECT_EQ(0U, fixture->runtime.active_sessions());
	EXPECT_EQ(0U, fixture->runtime.socket_count());
	expect_default_current(NativePlayerProbe::current(fixture->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
	EXPECT_EQ(TickPermanentCaptureFailure, static_cast<std::uint8_t>(capture_tick(*fixture, view, 200'000U)));
	EXPECT_EQ(0U, view.total_calls());
}

TEST(TelemetryNativePlayerCaptureContract, DeadlineOverflowFailsClosedBeforeReadingAndStaysSticky)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	CountingEngineReadView view;
	EXPECT_EQ(TickPermanentCaptureFailure,
		static_cast<std::uint8_t>(capture_tick(*fixture, view, std::numeric_limits<std::uint64_t>::max())));
	EXPECT_EQ(0U, view.total_calls());
	EXPECT_EQ(CaptureCadenceFailure, NativePlayerProbe::last_status(fixture->runtime));
	EXPECT_EQ(0U, fixture->runtime.socket_count());
	expect_default_current(NativePlayerProbe::current(fixture->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
	EXPECT_EQ(TickPermanentCaptureFailure, static_cast<std::uint8_t>(capture_tick(*fixture, view, 1U)));
	EXPECT_EQ(0U, view.total_calls());
}

TEST(TelemetryNativePlayerCaptureContract, EntityIdExhaustionClosesOnlyAffectedSlotAndTickCompletes)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	config.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	(void)establish_ready(*fixture, peer(29U), 970U, 1'000U, 970U);
	(void)establish_ready(*fixture, peer(30U), 980U, 2'000U, 980U);
	ASSERT_TRUE(NativePlayerAccess::seed_last_allocated_entity_id(fixture->runtime,
		0U,
		std::numeric_limits<std::uint64_t>::max()));
	CountingEngineReadView view;
	EXPECT_EQ(detail::NativeSessionTickStatus::Complete, capture_tick(*fixture, view, 100'000U));
	EXPECT_EQ(1U, view.read_calls);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime), 2U, 0U, 1U, 0U, 0U, 0U, 1U);
	EXPECT_EQ(1U, fixture->runtime.active_sessions());
	EXPECT_EQ(detail::ProducerSessionProgress::Empty, slot(*fixture, 0U)->progress);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(*fixture, 1U)->progress);
	EXPECT_EQ(1U, slot(*fixture, 1U)->latest_player_sample.entity_id);
	EXPECT_EQ(1U, fixture->runtime.socket_count());
}

TEST(TelemetryNativePlayerCaptureContract, PurgeClearsAndRearmsAtIndependentExactBoundaries)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	config.flight_hz = 30U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	CountingEngineReadView view;
	(void)capture_tick(*fixture, view, 10'000U);
	fixture->runtime.purge_all(detail::SessionCloseReason::MissionDiscontinuity);
	EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture->runtime));
	expect_default_current(NativePlayerProbe::current(fixture->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
	view.clear_counts();
	const std::uint64_t t2 = 987'654U;
	const auto period = independent_period_us(config.flight_hz);
	(void)capture_tick(*fixture, view, t2);
	EXPECT_EQ(1U, view.read_calls);
	(void)capture_tick(*fixture, view, t2 + period - 1U);
	EXPECT_EQ(1U, view.read_calls);
	EXPECT_EQ(CaptureNotDue, NativePlayerProbe::last_status(fixture->runtime));
	(void)capture_tick(*fixture, view, t2 + period);
	EXPECT_EQ(2U, view.read_calls);
}

TEST(TelemetryNativePlayerCaptureContract, ShutdownClearsAndAllCaptureSeamsStayUnavailable)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	CountingEngineReadView view;
	(void)capture_tick(*fixture, view, 10'000U);
	fixture->runtime.shutdown();
	EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture->runtime));
	expect_default_current(NativePlayerProbe::current(fixture->runtime));
	expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
	view.clear_counts();
	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable, capture_tick(*fixture, view, 20'000U));
	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable, native_tick(*fixture, {20'001U, 0U, true}));
	EXPECT_EQ(detail::NativeSessionTickStatus::Unavailable,
		NativePlayerAccess::inject_collected_player_capture(fixture->runtime,
			{detail::CaptureStatus::Valid, detail::CaptureReason::None},
			observation(42U, 20'000U)));
	EXPECT_EQ(0U, view.total_calls());
}

TEST(TelemetryNativePlayerCaptureContract, R2OnlyTestSeamPreservesPlayerStateWhileR2Progresses)
{
	REQUIRE_NATIVE_PLAYER_D3();
	auto fixture = std::make_unique<NativeFixture>();
	auto config = enabled_config();
	config.max_clients = 2U;
	ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
	const auto ready_endpoint = peer(31U);
	const auto ready_session_id = establish_ready(*fixture, ready_endpoint, 990U, 1'000U, 990U);
	CountingEngineReadView view;
	(void)capture_tick(*fixture, view, 100'000U);
	const auto status_before = NativePlayerProbe::last_status(fixture->runtime);
	const auto current_before = NativePlayerProbe::current(fixture->runtime);
	const auto materialization_before = NativePlayerProbe::materialization(fixture->runtime);
	const auto sample_before = slot(*fixture)->latest_player_sample;
	auto expect_preserved = [&] {
		EXPECT_EQ(status_before, NativePlayerProbe::last_status(fixture->runtime));
		const auto current_after = NativePlayerProbe::current(fixture->runtime);
		EXPECT_EQ(current_before.available, current_after.available);
		EXPECT_EQ(current_before.result.status, current_after.result.status);
		EXPECT_EQ(current_before.result.reason, current_after.result.reason);
		expect_observation(current_after.observation, current_before.observation);
		expect_materialization(NativePlayerProbe::materialization(fixture->runtime),
			materialization_before.eligible_slots,
			materialization_before.materialized_existing_slots,
			materialization_before.materialized_new_slots,
			materialization_before.no_player_slots,
			materialization_before.invalid_source_slots,
			materialization_before.invalid_capture_slots,
			materialization_before.closed_exhausted_slots);
		ASSERT_NE(nullptr, slot(*fixture));
		ASSERT_TRUE(slot(*fixture)->has_latest_player_sample);
		expect_player_sample(slot(*fixture)->latest_player_sample, sample_before);
	};
	const auto period = independent_period_us(config.flight_hz);
	const auto activity_before = slot(*fixture)->heartbeat.last_valid_network_activity_us;
	const auto clock_response_before = slot(*fixture)->heartbeat.last_valid_clock_response_us;
	const std::uint64_t heartbeat_tick = 100'001U;
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, heartbeat_request(ready_session_id, ready_endpoint, 991U, heartbeat_tick - 1U)});
	const auto sends_before_heartbeat = fixture->backend.sent.size();
	bool heartbeat_sent = false;
	for (std::uint64_t offset = 0U; offset < 8U && !heartbeat_sent; ++offset) {
		native_tick(*fixture, {heartbeat_tick + offset, 1U, true});
		for (std::size_t index = sends_before_heartbeat;
			index < fixture->backend.sent.size(); ++index) {
			heartbeat_sent = heartbeat_sent ||
				sent_type(fixture->backend, index) == protocol::MessageType::Heartbeat;
		}
	}
	ASSERT_TRUE(heartbeat_sent);
	ASSERT_NE(nullptr, slot(*fixture));
	EXPECT_LT(activity_before, slot(*fixture)->heartbeat.last_valid_network_activity_us);
	EXPECT_EQ(heartbeat_tick, slot(*fixture)->heartbeat.last_valid_network_activity_us);
	EXPECT_EQ(clock_response_before, slot(*fixture)->heartbeat.last_valid_clock_response_us)
		<< "A valid client Request refreshes activity and queues Heartbeat Response; it is not a clock-response sample.";
	expect_preserved();

	const auto second_endpoint = peer(33U);
	const auto sends_before_second_hello = fixture->backend.sent.size();
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, hello(protocol::VersionMinorV1_1, 992U, second_endpoint)});
	native_tick(*fixture, {heartbeat_tick + 1U, 1U, true});
	expect_preserved();
	ASSERT_GT(fixture->backend.sent.size(), sends_before_second_hello);
	const auto second_welcome = fixture->backend.sent.size() - 1U;
	EXPECT_EQ(protocol::MessageType::Welcome, sent_type(fixture->backend, second_welcome));
	ASSERT_NE(nullptr, slot(*fixture, 1U));
	EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, slot(*fixture, 1U)->progress);
	EXPECT_EQ(2U, fixture->runtime.active_sessions());

	const auto sends_before_second_welcome_ack = fixture->backend.sent.size();
	fixture->backend.receives.push_back(
		{detail::IoStatus::Complete, ack_for(fixture->backend.sent[second_welcome], second_endpoint, 993U)});
	const auto second_welcome_ack_script_index = fixture->backend.receives.size() - 1U;
	native_tick(*fixture, {heartbeat_tick + 2U, 1U, true});
	expect_preserved();
	EXPECT_GT(fixture->backend.receive_script_index, second_welcome_ack_script_index)
		<< "The R2-only test seam must ingest the real WELCOME ACK before later egress.";
	ASSERT_NE(nullptr, slot(*fixture, 1U));
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(*fixture, 1U)->progress)
		<< "Applying the WELCOME proof is the observable R2 handshake transition.";
	for (std::uint64_t offset = 0U;
		offset < 8U && fixture->backend.sent.size() == sends_before_second_welcome_ack;
		++offset) {
		native_tick(*fixture, {heartbeat_tick + 3U + offset, 1U, true});
		expect_preserved();
	}
	ASSERT_GT(fixture->backend.sent.size(), sends_before_second_welcome_ack);
	EXPECT_EQ(protocol::MessageType::SessionBegin,
		sent_type(fixture->backend, fixture->backend.sent.size() - 1U));
	ASSERT_NE(nullptr, slot(*fixture, 1U));
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, slot(*fixture, 1U)->progress);

	native_tick(*fixture, {100'000U + period * 2U, 1U, true});
	expect_preserved();
	native_tick(*fixture, {100'000U + period * 4U, 1U, true});
	expect_preserved();

	const auto disconnect_due = slot(*fixture)->heartbeat.last_valid_network_activity_us +
		slot(*fixture)->heartbeat.disconnect_timeout_us;
	native_tick(*fixture, {disconnect_due, 1U, true});
	ASSERT_NE(nullptr, slot(*fixture));
	EXPECT_EQ(detail::ProducerSessionProgress::Empty, slot(*fixture)->progress)
		<< "Only the real R2 maintenance deadline may clear the preserved player slot.";
	EXPECT_EQ(status_before, NativePlayerProbe::last_status(fixture->runtime));
	const auto current_after_close = NativePlayerProbe::current(fixture->runtime);
	EXPECT_EQ(current_before.available, current_after_close.available);
	EXPECT_EQ(current_before.result.status, current_after_close.result.status);
	EXPECT_EQ(current_before.result.reason, current_after_close.result.reason);
	expect_observation(current_after_close.observation, current_before.observation);
	expect_materialization(NativePlayerProbe::materialization(fixture->runtime),
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
		auto fixture = std::make_unique<NativeFixture>();
		auto config = enabled_config();
		ASSERT_EQ(detail::NativeSessionStartStatus::Started, fixture->start(config));
		fixture->backend.receives.push_back({io_status, {}});
		CountingEngineReadView view;
		EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure, capture_tick(*fixture, view, 100'000U));
		EXPECT_EQ(0U, view.total_calls());
		EXPECT_EQ(CaptureUnavailable, NativePlayerProbe::last_status(fixture->runtime));
		expect_default_current(NativePlayerProbe::current(fixture->runtime));
		expect_zero_materialization(NativePlayerProbe::materialization(fixture->runtime));
		EXPECT_EQ(0U, fixture->runtime.socket_count());
		EXPECT_EQ(detail::NativeSessionTickStatus::PermanentTransportFailure, capture_tick(*fixture, view, 200'000U));
		EXPECT_EQ(0U, view.total_calls());
	}
}

#undef REQUIRE_NATIVE_PLAYER_D3

#endif

} // namespace
