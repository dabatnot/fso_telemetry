#include "telemetry/producer/telemetry_runtime_adapter_player_test_access.h"

#include "globalincs/systemvars.h"
#include "gamesequence/gamesequence.h"
#include "object/object.h"
#include "playerman/player.h"
#include "ship/ship.h"
#include "telemetry/engine_adapter.h"
#include "telemetry/runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

#define REQUIRE_RUNTIME_ADAPTER_D4(Access)                                                                  \
	do {                                                                                                      \
		if (!Access::contract_available()) {                                                                    \
			FAIL() << "WP07-D4 RED: the production runtime-adapter test access to the unique local-FSO-view "   \
					   "helper is absent.";                                                                         \
			return;                                                                                              \
		}                                                                                                       \
	} while (false)

struct FixedRandom final : detail::RandomSource {
	std::uint64_t next = 0x7000U;
	bool next_u64(std::uint64_t& output) noexcept override
	{
		output = next++;
		return true;
	}
};

struct QuietBackend final : detail::UdpSocketBackend {
	detail::SocketOpenResult open_socket(const detail::SocketOpenRequest& request) noexcept override
	{
		++open_calls;
		return {detail::SocketOpenStatus::Complete,
			77U,
			protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, request.port)};
	}
	detail::SocketReceiveResult try_receive(detail::SocketHandle,
		protocol::MutableByteView) noexcept override
	{
		++receive_calls;
		return {receive_status, {}, 0U, false};
	}
	detail::SocketSendResult try_send(detail::SocketHandle,
		const protocol::EndpointKey&,
		protocol::ByteView bytes) noexcept override
	{
		++send_calls;
		return {detail::IoStatus::Complete, bytes.size};
	}
	void close_socket(detail::SocketHandle) noexcept override { ++close_calls; }

	detail::IoStatus receive_status = detail::IoStatus::WouldBlock;
	std::size_t open_calls = 0U;
	std::size_t receive_calls = 0U;
	std::size_t send_calls = 0U;
	std::size_t close_calls = 0U;
};

struct Completion final : detail::NativeOutputCompletionPort {
	void complete(detail::SessionController& controller, detail::IoStatus status) noexcept override
	{
		forwarder.complete(controller, status);
	}
	detail::NativeOutputCompletionForwarder forwarder;
};

struct NativeFixture {
	QuietBackend backend;
	Completion completion;
	FixedRandom id_random;
	FixedRandom packet_random;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator ids{id_random, registry};
	detail::NativeSessionRuntime native{backend, completion};

	NativeFixture()
	{
		detail::capture_phase2_main_thread_authority();
		EXPECT_TRUE(registry.allocate_storage());
		telemetry::TelemetryConfig config;
		config.enabled = true;
		config.bind_addresses.clear();
		config.bind_addresses.add(telemetry::NumericIpAddress::from_ipv4({127U, 0U, 0U, 1U}));
		config.max_datagrams_per_tick = 64U;
		EXPECT_EQ(detail::NativeSessionStartStatus::Started,
			native.start({&config, 0x1020304050607080ULL, &ids, &packet_random}));
	}
};

detail::Wp03KnownBudgetSubtotal complete_runtime_budget() noexcept
{
	detail::Wp03KnownBudgetSubtotal result;
	result.error = detail::StartupBudgetError::None;
	result.is_complete = true;
	result.deferred_categories = 0U;
	result.known_bytes = detail::SessionIdRegistryStorageBytes;
	result.metric_known_bytes = detail::SessionIdRegistryStorageBytes;
	result.session_id_registry_bytes = detail::SessionIdRegistryStorageBytes;
	result.client_slot_count = 1U;
	return result;
}

// This is deliberately a real RuntimeStartupServices composition.  Its tick
// and lifecycle methods delegate to the exact production D4 access seam; it
// does not reproduce the adapter mapping or construct an alternate read view.
struct AdapterRuntimeServices final : detail::RuntimeStartupServices {
	AdapterRuntimeServices() : ids(id_random, registry)
	{
		config.enabled = true;
		config.bind_addresses.clear();
		config.bind_addresses.add(telemetry::NumericIpAddress::from_ipv4({127U, 0U, 0U, 1U}));
		config.max_datagrams_per_tick = 64U;
	}

	void capture_main_thread() noexcept override
	{
		detail::capture_phase2_main_thread_authority();
		captured = true;
	}
	bool is_on_captured_main_thread() noexcept override { return captured; }
	detail::RuntimeConfigResult load_config() noexcept override
	{
		return {detail::RuntimeConfigStatus::Enabled, 1U};
	}
	detail::IdentityResult load_producer_identity() noexcept override
	{
		return {0x1020304050607080ULL, detail::IdentityError::None};
	}
	detail::SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		return {detail::SessionIdCandidateStatus::Ready, 0x1234U};
	}
	detail::Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t) noexcept override
	{
		return complete_runtime_budget();
	}
	bool allocate_session_registry() noexcept override { return registry.allocate_storage(); }
	detail::SessionIdRegistrationStatus register_session_candidate(std::uint64_t candidate) noexcept override
	{
		return registry.register_candidate(candidate);
	}
	detail::RuntimeTransportStatus start_transport() noexcept override
	{
		++start_calls;
		native = std::make_unique<detail::NativeSessionRuntime>(backend, completion);
		return native->start({&config, 0x1020304050607080ULL, &ids, &packet_random}) ==
				detail::NativeSessionStartStatus::Started
			? detail::RuntimeTransportStatus::Started
			: detail::RuntimeTransportStatus::Unavailable;
	}
	std::uint64_t monotonic_now_us() noexcept override
	{
		trace.push_back('K');
		return now_us;
	}
	detail::RuntimeTickStatus service_tick(const detail::RuntimeTickContext& context) noexcept override
	{
		trace.push_back('H');
		++helper_calls;
		contexts.push_back(context);
		return detail::RuntimeAdapterPlayerPublicProbe::service_tick(native.get(), context);
	}
	void stop_collection() noexcept override
	{
		trace.push_back('S');
		detail::RuntimeAdapterPlayerPublicProbe::stop_collection(native.get());
	}
	void invalidate_mission_state_and_entities() noexcept override
	{
		trace.push_back('I');
		detail::RuntimeAdapterPlayerPublicProbe::invalidate_mission_state_and_entities(native.get());
	}
	void cancel_replication() noexcept override { trace.push_back('Q'); }
	void close_sessions_and_stores() noexcept override
	{
		trace.push_back('C');
		detail::RuntimeAdapterPlayerPublicProbe::close_sessions_and_stores(native.get());
	}
	void reset_mission_scope() noexcept override { trace.push_back('M'); }
	void stop_transport() noexcept override
	{
		trace.push_back('X');
		detail::RuntimeAdapterPlayerPublicProbe::stop_transport(native.get());
	}
	void emit_runtime_summary() noexcept override { trace.push_back('U'); }
	void release_runtime_allocations() noexcept override
	{
		trace.push_back('A');
		native.reset();
	}
	void release_session_registry() noexcept override { trace.push_back('R'); }
	void emit_startup_diagnostic(detail::RuntimeTerminalReason reason) noexcept override
	{
		trace.push_back('D');
		diagnostics.push_back(reason);
	}

	QuietBackend backend;
	Completion completion;
	FixedRandom id_random;
	FixedRandom packet_random;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator ids;
	telemetry::TelemetryConfig config;
	std::unique_ptr<detail::NativeSessionRuntime> native;
	std::vector<detail::RuntimeTickContext> contexts;
	std::vector<detail::RuntimeTerminalReason> diagnostics;
	std::vector<char> trace;
	std::uint64_t now_us = 100'000U;
	std::size_t helper_calls = 0U;
	std::size_t start_calls = 0U;
	bool captured = false;
};

void start_ready(detail::Runtime& runtime, AdapterRuntimeServices& services)
{
	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	ASSERT_NE(nullptr, services.native.get());
}

void enter_mission_active(detail::Runtime& runtime, AdapterRuntimeServices& services)
{
	services.now_us += 10'000U;
	runtime.on_game_mission_load();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
	services.now_us += 10'000U;
	runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::MissionActive, runtime.state());
}

struct EngineGlobalsGuard {
	EngineGlobalsGuard()
		: old_game_mode(Game_mode), old_player(Player), old_player_object(Player_obj), old_player_ship(Player_ship),
		  old_object_type(Objects[0].type), old_object_instance(Objects[0].instance),
		  old_object_signature(Objects[0].signature), old_object_position(Objects[0].pos),
		  old_object_orientation(Objects[0].orient), old_object_physics(Objects[0].phys_info),
		  old_object_radius(Objects[0].radius), old_object_flags(Objects[0].flags), old_ship_objnum(Ships[0].objnum)
	{
		configure(101, 1.0f);
	}
	~EngineGlobalsGuard()
	{
		Game_mode = old_game_mode;
		Player = old_player;
		Player_obj = old_player_object;
		Player_ship = old_player_ship;
		Objects[0].type = old_object_type;
		Objects[0].instance = old_object_instance;
		Objects[0].signature = old_object_signature;
		Objects[0].pos = old_object_position;
		Objects[0].orient = old_object_orientation;
		Objects[0].phys_info = old_object_physics;
		Objects[0].radius = old_object_radius;
		Objects[0].flags = old_object_flags;
		Ships[0].objnum = old_ship_objnum;
	}

	void configure(int signature, float marker)
	{
		Game_mode |= GM_IN_MISSION;
		local_player.objnum = 0;
		Objects[0].type = OBJ_SHIP;
		Objects[0].instance = 0;
		Objects[0].signature = signature;
		Objects[0].pos.xyz.x = marker;
		Objects[0].pos.xyz.y = marker + 1.0f;
		Objects[0].pos.xyz.z = marker + 2.0f;
		Objects[0].orient = vmd_identity_matrix;
		Objects[0].phys_info.vel.xyz.x = marker + 3.0f;
		Objects[0].phys_info.vel.xyz.y = marker + 4.0f;
		Objects[0].phys_info.vel.xyz.z = marker + 5.0f;
		Objects[0].phys_info.rotvel.xyz.x = 0.1f;
		Objects[0].phys_info.rotvel.xyz.y = 0.2f;
		Objects[0].phys_info.rotvel.xyz.z = 0.3f;
		Objects[0].radius = marker + 6.0f;
		Ships[0].objnum = 0;
		Player = &local_player;
		Player_obj = &Objects[0];
		Player_ship = &Ships[0];
	}

	void null_player_globals()
	{
		Player = nullptr;
		Player_obj = nullptr;
		Player_ship = nullptr;
	}

	int old_game_mode;
	player* old_player;
	object* old_player_object;
	ship* old_player_ship;
	decltype(Objects[0].type) old_object_type;
	decltype(Objects[0].instance) old_object_instance;
	decltype(Objects[0].signature) old_object_signature;
	decltype(Objects[0].pos) old_object_position;
	decltype(Objects[0].orient) old_object_orientation;
	decltype(Objects[0].phys_info) old_object_physics;
	decltype(Objects[0].radius) old_object_radius;
	decltype(Objects[0].flags) old_object_flags;
	decltype(Ships[0].objnum) old_ship_objnum;
	player local_player{};
};

void expect_capture(const detail::NativeSessionRuntime& native,
	std::uint32_t signature,
	std::uint64_t time_us,
	float marker)
{
	ASSERT_TRUE(native.current_player_capture().available);
	EXPECT_EQ(detail::CaptureStatus::Valid, native.current_player_capture().result.status);
	const auto& observation = native.current_player_capture().observation;
	EXPECT_EQ(signature, observation.key.object_signature);
	EXPECT_EQ(time_us, observation.value.producer_sample_time_us);
	EXPECT_FLOAT_EQ(marker, observation.value.position_world.x);
	EXPECT_FLOAT_EQ(marker + 1.0f, observation.value.position_world.y);
	EXPECT_FLOAT_EQ(marker + 2.0f, observation.value.position_world.z);
	EXPECT_FLOAT_EQ(marker + 3.0f, observation.value.velocity_world.x);
	EXPECT_FLOAT_EQ(marker + 4.0f, observation.value.velocity_world.y);
	EXPECT_FLOAT_EQ(marker + 5.0f, observation.value.velocity_world.z);
	EXPECT_FLOAT_EQ(marker + 6.0f, observation.value.radius);
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void exact_api_contract()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	EXPECT_FALSE(detail::HasLegacyNativeOneArgumentTick<detail::NativeSessionRuntime>::value);
	EXPECT_TRUE((detail::HasD4ClosedRuntimeEnums<detail::RuntimeTickStatus,
		detail::RuntimeTerminalReason>::value));
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void real_globals_capture()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	auto fixture = std::make_unique<NativeFixture>();
	EngineGlobalsGuard globals;
	EXPECT_EQ(detail::RuntimeTickStatus::Complete,
		Access::service_tick(&fixture->native, {100'000U, 7U, true}));
	expect_capture(fixture->native, 101, 100'000U, 1.0f);
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void callback_copy_lifetime()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	auto fixture = std::make_unique<NativeFixture>();
	EngineGlobalsGuard globals;
	ASSERT_EQ(detail::RuntimeTickStatus::Complete,
		Access::service_tick(&fixture->native, {100'000U, 1U, true}));
	const auto copied = fixture->native.current_player_capture().observation;
	globals.configure(202, 20.0f);
	globals.null_player_globals();
	EXPECT_EQ(101U, copied.key.object_signature);
	EXPECT_FLOAT_EQ(1.0f, copied.value.position_world.x);
	globals.configure(202, 20.0f);
	ASSERT_EQ(detail::RuntimeTickStatus::Complete,
		Access::service_tick(&fixture->native, {200'000U, 1U, true}));
	expect_capture(fixture->native, 202, 200'000U, 20.0f);
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void mission_active_gate()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	AdapterRuntimeServices services;
	detail::Runtime runtime(services);
	EngineGlobalsGuard globals;
	globals.null_player_globals();
	start_ready(runtime, services);
	ASSERT_EQ(1U, services.contexts.size());
	globals.configure(303, 30.0f);
	enter_mission_active(runtime, services);
	ASSERT_NE(nullptr, services.native.get());
	expect_capture(*services.native, 303, services.now_us, 30.0f);
	globals.null_player_globals();
	services.now_us += 10'000U;
	runtime.on_game_mission_load();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
	services.now_us += 10'000U;
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	ASSERT_EQ(5U, services.contexts.size());
	EXPECT_EQ((std::array<bool, 5U>{false, false, true, false, false}),
		(std::array<bool, 5U>{services.contexts[0].mission_active,
			services.contexts[1].mission_active,
			services.contexts[2].mission_active,
			services.contexts[3].mission_active,
			services.contexts[4].mission_active}));
	EXPECT_EQ((std::array<std::uint32_t, 5U>{0U, 1U, 1U, 2U, 2U}),
		(std::array<std::uint32_t, 5U>{services.contexts[0].mission_generation,
			services.contexts[1].mission_generation,
			services.contexts[2].mission_generation,
			services.contexts[3].mission_generation,
			services.contexts[4].mission_generation}));
	EXPECT_EQ(static_cast<std::uint8_t>(detail::NativePlayerCaptureStatus::Inactive),
		static_cast<std::uint8_t>(services.native->last_player_capture_status()));
	EXPECT_FALSE(services.native->current_player_capture().available);
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void exhaustive_mapping()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	using RuntimeStatus = std::remove_cv_t<std::remove_reference_t<decltype(
		Access::map_tick_status(detail::NativeSessionTickStatus::Complete))>>;
	EXPECT_EQ(RuntimeStatus::Complete, Access::map_tick_status(detail::NativeSessionTickStatus::Complete));
	EXPECT_EQ(RuntimeStatus::Unavailable, Access::map_tick_status(detail::NativeSessionTickStatus::Unavailable));
	EXPECT_EQ(RuntimeStatus::PermanentTransportFailure,
		Access::map_tick_status(detail::NativeSessionTickStatus::PermanentTransportFailure));
	EXPECT_EQ(3U, static_cast<std::uint8_t>(
		Access::map_tick_status(detail::NativeSessionTickStatus::PermanentCaptureFailure)));
	EXPECT_EQ(3U, static_cast<std::uint8_t>(
		Access::map_tick_status(static_cast<detail::NativeSessionTickStatus>(0xffU))));
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void capture_fault_is_sticky()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	AdapterRuntimeServices services;
	detail::Runtime runtime(services);
	EngineGlobalsGuard globals;
	start_ready(runtime, services);
	enter_mission_active(runtime, services);
	ASSERT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	services.trace.clear();
	const auto calls_before_fault = services.helper_calls;
	services.now_us -= 1U;
	runtime.on_engine_update();
	EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
	EXPECT_EQ(14U, static_cast<std::uint8_t>(runtime.terminal_reason()));
	ASSERT_EQ(1U, services.diagnostics.size());
	EXPECT_EQ(14U, static_cast<std::uint8_t>(services.diagnostics.front()));
	EXPECT_EQ(calls_before_fault + 1U, services.helper_calls)
		<< "The faulting tick may construct exactly one local FSO view.";
	EXPECT_EQ((std::vector<char>{'K', 'H', 'S', 'C', 'I', 'X', 'A', 'R', 'D'}), services.trace);
	globals.null_player_globals();
	runtime.on_engine_update();
	runtime.on_engine_update();
	EXPECT_EQ(calls_before_fault + 1U, services.helper_calls);
	EXPECT_EQ(1U, services.diagnostics.size());
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void receive_transport_faults_are_terminal()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	for (const auto receive_failure :
		std::array<detail::IoStatus, 2U>{detail::IoStatus::Closed, detail::IoStatus::Error}) {
		SCOPED_TRACE(static_cast<unsigned>(receive_failure));
		AdapterRuntimeServices services;
		detail::Runtime runtime(services);
		EngineGlobalsGuard globals;
		start_ready(runtime, services);
		services.trace.clear();
		const auto helper_calls_before_fault = services.helper_calls;
		services.backend.receive_status = receive_failure;
		++services.now_us;
		runtime.on_engine_update();

		EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
		EXPECT_EQ(detail::RuntimeTerminalReason::TransportUnavailable, runtime.terminal_reason());
		EXPECT_NE(14U, static_cast<std::uint8_t>(runtime.terminal_reason()))
			<< "A native transport failure must stay distinct from CaptureFailure.";
		ASSERT_EQ(1U, services.diagnostics.size());
		EXPECT_EQ(detail::RuntimeTerminalReason::TransportUnavailable, services.diagnostics.front());
		EXPECT_EQ((std::vector<char>{'K', 'H', 'S', 'C', 'I', 'X', 'A', 'R', 'D'}), services.trace);
		EXPECT_EQ(helper_calls_before_fault + 1U, services.helper_calls);
		EXPECT_EQ(1U, services.backend.close_calls);

		globals.null_player_globals();
		runtime.on_engine_update();
		runtime.on_engine_update();
		EXPECT_EQ(helper_calls_before_fault + 1U, services.helper_calls);
		EXPECT_EQ(1U, services.diagnostics.size());
		EXPECT_EQ(1U, services.backend.close_calls);
	}
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void fault_then_lifecycle_never_reads_view()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	AdapterRuntimeServices services;
	detail::Runtime runtime(services);
	EngineGlobalsGuard globals;
	start_ready(runtime, services);
	enter_mission_active(runtime, services);
	services.now_us -= 1U;
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Faulted, runtime.state());
	ASSERT_EQ(nullptr, services.native.get());
	const auto calls = services.helper_calls;
	const auto starts = services.start_calls;
	const auto trace = services.trace;
	globals.null_player_globals();
	runtime.on_game_mission_load();
	runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	runtime.on_engine_update();
	runtime.on_engine_update();
	EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
	EXPECT_EQ(calls, services.helper_calls);
	EXPECT_EQ(starts, services.start_calls);
	EXPECT_EQ(trace, services.trace);
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void shutdown_is_idempotent()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	AdapterRuntimeServices services;
	detail::Runtime runtime(services);
	EngineGlobalsGuard globals;
	start_ready(runtime, services);
	services.trace.clear();
	const auto helper_calls = services.helper_calls;
	runtime.on_engine_shutdown();
	EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
	EXPECT_EQ((std::vector<char>{'S', 'C', 'I', 'X', 'U', 'A', 'R'}), services.trace);
	EXPECT_EQ(nullptr, services.native.get());
	const auto trace = services.trace;
	runtime.on_engine_shutdown();
	runtime.on_engine_update();
	EXPECT_EQ(trace, services.trace);
	EXPECT_EQ(helper_calls, services.helper_calls);
}

template <typename Access = detail::RuntimeAdapterPlayerPublicProbe>
void null_runtime_is_inert()
{
	REQUIRE_RUNTIME_ADAPTER_D4(Access);
	EXPECT_EQ(detail::RuntimeTickStatus::Unavailable, Access::service_tick(nullptr, {1U, 0U, true}));
	Access::stop_collection(nullptr);
	Access::invalidate_mission_state_and_entities(nullptr);
	Access::close_sessions_and_stores(nullptr);
	Access::stop_transport(nullptr);
}

TEST(TelemetryRuntimeAdapterPlayerContract, ExactD4ApiRemovesLegacyTickAndAppendsClosedOrdinals)
{
	exact_api_contract();
}
TEST(TelemetryRuntimeAdapterPlayerContract, ProductionHelperBuildsLocalFsoViewAndCapturesRealGlobals)
{
	real_globals_capture();
}
TEST(TelemetryRuntimeAdapterPlayerContract, EveryCallbackCopiesThenForgetsAllEngineGlobals)
{
	callback_copy_lifetime();
}
TEST(TelemetryRuntimeAdapterPlayerContract, MissionActiveFalseNeverReadsGlobalsAndTrueUsesFreshView)
{
	mission_active_gate();
}
TEST(TelemetryRuntimeAdapterPlayerContract, NativeToRuntimeMappingIsExhaustiveAndUnknownFailsAsCapture)
{
	exhaustive_mapping();
}
TEST(TelemetryRuntimeAdapterPlayerContract, CaptureFailureHasAnExplicitClosedProductionDiagnostic)
{
	using Access = detail::RuntimeAdapterPlayerPublicProbe;
	if (!Access::diagnostic_contract_available()) {
		FAIL() << "WP07-D4 RED: the production runtime adapter does not expose the closed diagnostic mapping "
				  "used by emit_startup_diagnostic().";
		return;
	}

	const auto* capture =
		Access::startup_diagnostic_message(detail::RuntimeTerminalReason::CaptureFailure);
	ASSERT_NE(nullptr, capture)
		<< "CaptureFailure must not fall through the production diagnostic switch.";
	EXPECT_STREQ("Telemetry runtime failed: player capture.\n", capture);
	for (std::uint8_t ordinal = 0U;
		ordinal <= static_cast<std::uint8_t>(detail::RuntimeTerminalReason::CaptureFailure);
		++ordinal) {
		SCOPED_TRACE(static_cast<unsigned>(ordinal));
		EXPECT_NE(nullptr,
			Access::startup_diagnostic_message(
				static_cast<detail::RuntimeTerminalReason>(ordinal)))
			<< "Every closed RuntimeTerminalReason needs an explicit production diagnostic.";
	}
	EXPECT_EQ(nullptr,
		Access::startup_diagnostic_message(
			static_cast<detail::RuntimeTerminalReason>(0xffU)))
		<< "An unknown ordinal must fail closed instead of borrowing a misleading diagnostic.";
}
TEST(TelemetryRuntimeAdapterPlayerContract, CaptureFailureIsStickyAndNoViewIsReadAfterFault)
{
	capture_fault_is_sticky();
}
TEST(TelemetryRuntimeAdapterPlayerContract, ReceiveClosedAndErrorFaultAsTransportWithoutPostTerminalHelper)
{
	receive_transport_faults_are_terminal();
}
TEST(TelemetryRuntimeAdapterPlayerContract, FaultTeardownPurgesStopsAndNeverRearms)
{
	fault_then_lifecycle_never_reads_view();
}
TEST(TelemetryRuntimeAdapterPlayerContract, ShutdownUsesStopThenPurgeThenTransportStopAndIsIdempotent)
{
	shutdown_is_idempotent();
}
TEST(TelemetryRuntimeAdapterPlayerContract, NullNativeLifecycleAndServiceAreInert)
{
	null_runtime_is_inert();
}

#undef REQUIRE_RUNTIME_ADAPTER_D4

} // namespace
