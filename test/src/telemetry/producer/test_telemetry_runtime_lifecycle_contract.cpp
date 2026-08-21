#include "telemetry/runtime.h"

#include "gamesequence/gamesequence.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;

enum class LifecycleCall : std::uint8_t {
	CaptureMainThread = 0,
	MainThreadCheck,
	LoadConfig,
	LoadProducerIdentity,
	DrawSessionCandidate,
	CalculateBudget,
	AllocateRegistry,
	RegisterCandidate,
	StartTransport,
	StopCollection,
	InvalidateMissionStateAndEntities,
	CancelReplication,
	CloseSessionsAndStores,
	ResetMissionScope,
	StopTransport,
	EmitRuntimeSummary,
	ReleaseRuntimeAllocations,
	ReleaseRegistry,
	EmitDiagnostic,
};

enum class RuntimeCallbackKind : std::uint8_t {
	EngineUpdate = 0,
	EngineShutdown,
	MissionLoad,
	EnterState,
	LeaveState,
	None,
};

void invoke_runtime_callback(detail::Runtime& runtime, RuntimeCallbackKind callback) noexcept
{
	switch (callback) {
	case RuntimeCallbackKind::EngineUpdate:
		runtime.on_engine_update();
		break;
	case RuntimeCallbackKind::EngineShutdown:
		runtime.on_engine_shutdown();
		break;
	case RuntimeCallbackKind::MissionLoad:
		runtime.on_game_mission_load();
		break;
	case RuntimeCallbackKind::EnterState:
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		break;
	case RuntimeCallbackKind::LeaveState:
		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		break;
	case RuntimeCallbackKind::None:
		break;
	}
}

detail::Wp03KnownBudgetSubtotal complete_budget() noexcept
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

class LifecycleServices final : public detail::RuntimeStartupServices {
  public:
	void capture_main_thread() noexcept override
	{
		record(LifecycleCall::CaptureMainThread);
		main_thread_captured = true;
	}

	bool is_on_captured_main_thread() noexcept override
	{
		record(LifecycleCall::MainThreadCheck);
		request_reentrant_shutdown(shutdown_during_main_thread_check);
		return main_thread_captured && main_thread_check_result;
	}

	detail::RuntimeConfigResult load_config() noexcept override
	{
		record(LifecycleCall::LoadConfig);
		if (shutdown_during_config && observed_runtime != nullptr) {
			observed_runtime->on_engine_shutdown();
		}
		return config_result;
	}

	detail::IdentityResult load_producer_identity() noexcept override
	{
		record(LifecycleCall::LoadProducerIdentity);
		return identity_result;
	}

	detail::SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		record(LifecycleCall::DrawSessionCandidate);
		return {detail::SessionIdCandidateStatus::Ready, 42U};
	}

	detail::Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t) noexcept override
	{
		record(LifecycleCall::CalculateBudget);
		return complete_budget();
	}

	bool allocate_session_registry() noexcept override
	{
		record(LifecycleCall::AllocateRegistry);
		registry_ready = allocation_result;
		return allocation_result;
	}

	detail::SessionIdRegistrationStatus register_session_candidate(std::uint64_t candidate) noexcept override
	{
		record(LifecycleCall::RegisterCandidate);
		registered_candidate = candidate;
		return registration_status;
	}

	detail::RuntimeTransportStatus start_transport() noexcept override
	{
		record(LifecycleCall::StartTransport);
		transport_active = transport_status == detail::RuntimeTransportStatus::Started;
		return transport_status;
	}

	std::uint64_t monotonic_now_us() noexcept override
	{
		return next_monotonic_us++;
	}

	detail::RuntimeTickStatus service_tick(const detail::RuntimeTickContext& context) noexcept override
	{
		tick_contexts.push_back(context);
		return tick_status;
	}

	void stop_collection() noexcept override
	{
		record(LifecycleCall::StopCollection);
		if (worker_callback_during_stop_collection != RuntimeCallbackKind::None && observed_runtime != nullptr) {
			const auto callback = worker_callback_during_stop_collection;
			worker_callback_during_stop_collection = RuntimeCallbackKind::None;
			std::thread worker([this, callback]() { invoke_runtime_callback(*observed_runtime, callback); });
			worker.join();
		}
		if (shutdown_during_stop_collection && !inside_reentrant_shutdown && observed_runtime != nullptr) {
			inside_reentrant_shutdown = true;
			++reentrant_shutdown_attempts;
			reentrant_shutdown_observed_state = observed_runtime->state();
			observed_runtime->on_engine_shutdown();
			inside_reentrant_shutdown = false;
		}
	}

	void invalidate_mission_state_and_entities() noexcept override
	{
		record(LifecycleCall::InvalidateMissionStateAndEntities);
	}

	void cancel_replication() noexcept override
	{
		record(LifecycleCall::CancelReplication);
	}

	void close_sessions_and_stores() noexcept override
	{
		record(LifecycleCall::CloseSessionsAndStores);
	}

	void reset_mission_scope() noexcept override
	{
		record(LifecycleCall::ResetMissionScope);
	}

	void stop_transport() noexcept override
	{
		record(LifecycleCall::StopTransport);
		transport_active = false;
		request_reentrant_shutdown(shutdown_during_stop_transport);
	}

	void emit_runtime_summary() noexcept override
	{
		record(LifecycleCall::EmitRuntimeSummary);
	}

	void release_runtime_allocations() noexcept override
	{
		record(LifecycleCall::ReleaseRuntimeAllocations);
	}

	void release_session_registry() noexcept override
	{
		record(LifecycleCall::ReleaseRegistry);
		registry_ready = false;
		request_reentrant_shutdown(shutdown_during_release_registry);
	}

	void emit_startup_diagnostic(detail::RuntimeTerminalReason reason) noexcept override
	{
		record(LifecycleCall::EmitDiagnostic);
		diagnostic_reasons.push_back(reason);
	}

	void clear_calls()
	{
		calls.clear();
	}

	std::vector<LifecycleCall> calls;
	std::vector<detail::RuntimeTerminalReason> diagnostic_reasons;
	detail::RuntimeConfigResult config_result{detail::RuntimeConfigStatus::Enabled, 1U};
	detail::IdentityResult identity_result{7U, detail::IdentityError::None};
	detail::Runtime* observed_runtime = nullptr;
	detail::RuntimeState reentrant_shutdown_observed_state = detail::RuntimeState::Cold;
	detail::SessionIdRegistrationStatus registration_status =
		detail::SessionIdRegistrationStatus::Registered;
	detail::RuntimeTransportStatus transport_status = detail::RuntimeTransportStatus::Started;
	detail::RuntimeTickStatus tick_status = detail::RuntimeTickStatus::Unavailable;
	RuntimeCallbackKind worker_callback_during_stop_collection = RuntimeCallbackKind::None;
	std::uint64_t registered_candidate = 0U;
	std::uint64_t next_monotonic_us = 1U;
	std::size_t reentrant_shutdown_attempts = 0U;
	std::vector<detail::RuntimeTickContext> tick_contexts;
	bool main_thread_captured = false;
	bool main_thread_check_result = true;
	bool allocation_result = true;
	bool registry_ready = false;
	bool transport_active = false;
	bool shutdown_during_config = false;
	bool shutdown_during_main_thread_check = false;
	bool shutdown_during_stop_collection = false;
	bool shutdown_during_stop_transport = false;
	bool shutdown_during_release_registry = false;
	bool inside_reentrant_shutdown = false;

  private:
	void request_reentrant_shutdown(bool& requested) noexcept
	{
		if (!requested || inside_reentrant_shutdown || observed_runtime == nullptr) {
			return;
		}
		requested = false;
		inside_reentrant_shutdown = true;
		++reentrant_shutdown_attempts;
		reentrant_shutdown_observed_state = observed_runtime->state();
		observed_runtime->on_engine_shutdown();
		inside_reentrant_shutdown = false;
	}

	void record(LifecycleCall call) noexcept
	{
		calls.push_back(call);
	}
};

void start_ready(detail::Runtime& runtime, LifecycleServices& services)
{
	services.observed_runtime = &runtime;
	runtime.capture_main_thread();
	runtime.on_engine_update();

	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	ASSERT_EQ(detail::RuntimeTerminalReason::None, runtime.terminal_reason());
	ASSERT_EQ(0U, runtime.published_session_id())
		<< "WP05 must not publish a client session; session establishment belongs to WP06.";
	ASSERT_EQ(42U, services.registered_candidate);
	ASSERT_TRUE(services.registry_ready);
	services.clear_calls();
}

void start_mission_active(detail::Runtime& runtime, LifecycleServices& services)
{
	start_ready(runtime, services);
	runtime.on_game_mission_load();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
	runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	ASSERT_TRUE(runtime.mission_publication_allowed());
	services.clear_calls();
}

const std::vector<LifecycleCall> MissionPurgeOrder{
	LifecycleCall::StopCollection,
	LifecycleCall::InvalidateMissionStateAndEntities,
	LifecycleCall::CancelReplication,
	LifecycleCall::CloseSessionsAndStores,
	LifecycleCall::ResetMissionScope,
};

const std::vector<LifecycleCall> GlobalTeardownOrder{
	LifecycleCall::StopCollection,
	LifecycleCall::CloseSessionsAndStores,
	LifecycleCall::InvalidateMissionStateAndEntities,
	LifecycleCall::StopTransport,
	LifecycleCall::EmitRuntimeSummary,
	LifecycleCall::ReleaseRuntimeAllocations,
	LifecycleCall::ReleaseRegistry,
};

const std::vector<LifecycleCall> FaultTeardownOrder{
	LifecycleCall::StopCollection,
	LifecycleCall::CloseSessionsAndStores,
	LifecycleCall::InvalidateMissionStateAndEntities,
	LifecycleCall::StopTransport,
	LifecycleCall::ReleaseRuntimeAllocations,
	LifecycleCall::ReleaseRegistry,
};

template <typename T, typename = void>
struct HasEventBatchHook : std::false_type {};

template <typename T>
struct HasEventBatchHook<T, std::void_t<decltype(&T::emit_event_batch)>> : std::true_type {};

static_assert(!HasEventBatchHook<detail::RuntimeStartupServices>::value,
	"WP05 must not introduce an EVENT_BATCH business-event hook; death/observer/respawn remain Phase 2.");
static_assert(GS_NUM_STATES == 56,
	"A new engine game state requires an explicit WP05 lifecycle disposition and review.");

using GameStateDisposition = detail::RuntimeGameStateDisposition;

constexpr std::array<GameStateDisposition, GS_NUM_STATES> ExpectedGameStateDispositions{{
	GameStateDisposition::Invalid,          // GS_STATE_INVALID
	GameStateDisposition::Terminal,         // GS_STATE_MAIN_MENU
	GameStateDisposition::ActiveRoot,       // GS_STATE_GAME_PLAY
	GameStateDisposition::PreserveContext,  // GS_STATE_GAME_PAUSED
	GameStateDisposition::Terminal,         // GS_STATE_QUIT_GAME
	GameStateDisposition::PreserveContext,  // GS_STATE_OPTIONS_MENU
	GameStateDisposition::Terminal,         // GS_STATE_BARRACKS_MENU
	GameStateDisposition::Terminal,         // GS_STATE_TECH_MENU
	GameStateDisposition::Terminal,         // GS_STATE_TRAINING_MENU
	GameStateDisposition::LoadingPreserve,  // GS_STATE_LOAD_MISSION_MENU
	GameStateDisposition::LoadingPreserve,  // GS_STATE_BRIEFING
	GameStateDisposition::LoadingPreserve,  // GS_STATE_SHIP_SELECT
	GameStateDisposition::PreserveContext,  // GS_STATE_DEBUG_PAUSED
	GameStateDisposition::PreserveContext,  // GS_STATE_HUD_CONFIG
	GameStateDisposition::Terminal,         // GS_STATE_MULTI_JOIN_GAME
	GameStateDisposition::PreserveContext,  // GS_STATE_CONTROL_CONFIG
	GameStateDisposition::LoadingPreserve,  // GS_STATE_WEAPON_SELECT
	GameStateDisposition::PreserveContext,  // GS_STATE_MISSION_LOG_SCROLLBACK
	GameStateDisposition::PreserveContext,  // GS_STATE_DEATH_DIED
	GameStateDisposition::PreserveContext,  // GS_STATE_DEATH_BLEW_UP
	GameStateDisposition::Terminal,         // GS_STATE_SIMULATOR_ROOM
	GameStateDisposition::Terminal,         // GS_STATE_CREDITS
	GameStateDisposition::PreserveContext,  // GS_STATE_SHOW_GOALS
	GameStateDisposition::PreserveContext,  // GS_STATE_HOTKEY_SCREEN
	GameStateDisposition::Terminal,         // GS_STATE_VIEW_MEDALS
	GameStateDisposition::Terminal,         // GS_STATE_MULTI_HOST_SETUP
	GameStateDisposition::Terminal,         // GS_STATE_MULTI_CLIENT_SETUP
	GameStateDisposition::Terminal,         // GS_STATE_DEBRIEF
	GameStateDisposition::Terminal,         // GS_STATE_VIEW_CUTSCENES
	GameStateDisposition::LoadingPreserve,  // GS_STATE_MULTI_STD_WAIT
	GameStateDisposition::Terminal,         // GS_STATE_STANDALONE_MAIN
	GameStateDisposition::PreserveContext,  // GS_STATE_MULTI_PAUSED
	GameStateDisposition::LoadingPreserve,  // GS_STATE_TEAM_SELECT
	GameStateDisposition::PreserveContext,  // GS_STATE_TRAINING_PAUSED
	GameStateDisposition::LoadingPreserve,  // GS_STATE_INGAME_PRE_JOIN
	GameStateDisposition::PreserveContext,  // GS_STATE_EVENT_DEBUG
	GameStateDisposition::Terminal,         // GS_STATE_STANDALONE_POSTGAME
	GameStateDisposition::Terminal,         // GS_STATE_INITIAL_PLAYER_SELECT
	GameStateDisposition::LoadingPreserve,  // GS_STATE_MULTI_MISSION_SYNC
	GameStateDisposition::LoadingPreserve,  // GS_STATE_MULTI_START_GAME
	GameStateDisposition::LoadingPreserve,  // GS_STATE_MULTI_HOST_OPTIONS
	GameStateDisposition::Terminal,         // GS_STATE_MULTI_DOGFIGHT_DEBRIEF
	GameStateDisposition::Terminal,         // GS_STATE_CAMPAIGN_ROOM
	GameStateDisposition::LoadingPreserve,  // GS_STATE_CMD_BRIEF
	GameStateDisposition::LoadingPreserve,  // GS_STATE_RED_ALERT
	GameStateDisposition::Terminal,         // GS_STATE_END_OF_CAMPAIGN
	GameStateDisposition::PreserveContext,  // GS_STATE_GAMEPLAY_HELP
	GameStateDisposition::LoadingPreserve,  // GS_STATE_LOOP_BRIEF
	GameStateDisposition::Terminal,         // GS_STATE_PXO
	GameStateDisposition::PreserveContext,  // GS_STATE_LAB
	GameStateDisposition::Terminal,         // GS_STATE_PXO_HELP
	GameStateDisposition::LoadingPreserve,  // GS_STATE_START_GAME
	GameStateDisposition::LoadingPreserve,  // GS_STATE_FICTION_VIEWER
	GameStateDisposition::Terminal,         // GS_STATE_SCRIPTING
	GameStateDisposition::PreserveContext,  // GS_STATE_SCRIPTING_MISSION
	GameStateDisposition::PreserveContext,  // GS_STATE_INGAME_OPTIONS
}};

static_assert(ExpectedGameStateDispositions.size() == static_cast<std::size_t>(GS_NUM_STATES));

enum class StableRuntimeSetup : std::uint8_t {
	Cold = 0,
	Disabled,
	Faulted,
	Ready,
	MissionLoading,
	MissionActive,
};

void arrange_stable_state(StableRuntimeSetup setup, detail::Runtime& runtime, LifecycleServices& services)
{
	switch (setup) {
	case StableRuntimeSetup::Cold:
		runtime.capture_main_thread();
		ASSERT_EQ(detail::RuntimeState::Cold, runtime.state());
		break;
	case StableRuntimeSetup::Disabled:
		services.config_result = {detail::RuntimeConfigStatus::Absent, 0U};
		runtime.capture_main_thread();
		runtime.on_engine_update();
		ASSERT_EQ(detail::RuntimeState::Disabled, runtime.state());
		break;
	case StableRuntimeSetup::Faulted:
		services.identity_result = {0U, detail::IdentityError::ProfileReadFailure};
		runtime.capture_main_thread();
		runtime.on_engine_update();
		ASSERT_EQ(detail::RuntimeState::Faulted, runtime.state());
		break;
	case StableRuntimeSetup::Ready:
		start_ready(runtime, services);
		return;
	case StableRuntimeSetup::MissionLoading:
		start_ready(runtime, services);
		runtime.on_game_mission_load();
		runtime.on_engine_update();
		ASSERT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		break;
	case StableRuntimeSetup::MissionActive:
		start_mission_active(runtime, services);
		return;
	}
	services.clear_calls();
}

TEST(TelemetryRuntimeLifecycleContract, StartedFakePublishesReadyOnlyAfterTheCompleteStartupTransaction)
{
	LifecycleServices services;
	detail::Runtime runtime(services);

	start_ready(runtime, services);

	EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::None, runtime.terminal_reason());
	EXPECT_EQ(0U, runtime.published_session_id());
	EXPECT_TRUE(services.registry_ready);
	EXPECT_TRUE(services.diagnostic_reasons.empty());
}

TEST(TelemetryRuntimeLifecycleContract, EveryEngineGameStateHasOneClosedReviewedDisposition)
{
	for (int state = 0; state < GS_NUM_STATES; ++state) {
		SCOPED_TRACE(state);
		EXPECT_EQ(ExpectedGameStateDispositions[static_cast<std::size_t>(state)],
			detail::classify_runtime_game_state(state));
	}

	EXPECT_EQ(GameStateDisposition::Invalid, detail::classify_runtime_game_state(-1));
	EXPECT_EQ(GameStateDisposition::Invalid, detail::classify_runtime_game_state(GS_NUM_STATES));
	EXPECT_EQ(GameStateDisposition::Invalid,
		detail::classify_runtime_game_state(std::numeric_limits<int>::max()));
}

TEST(TelemetryRuntimeLifecycleContract, ContextAndLoadingScreensNeverInventAMissionWithoutMissionLoad)
{
	const std::array<int, 3> representative_states{{
		GS_STATE_OPTIONS_MENU,
		GS_STATE_BRIEFING,
		GS_STATE_MULTI_HOST_OPTIONS,
	}};

	for (const auto state : representative_states) {
		SCOPED_TRACE(state);
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_ready(runtime, services);

		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, state);
		runtime.on_engine_update();

		EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
		EXPECT_EQ(0U, runtime.mission_generation());
		EXPECT_FALSE(runtime.mission_publication_allowed());
	}
}

TEST(TelemetryRuntimeLifecycleContract, BootstrapInvalidToMainMenuAfterStartupDoesNotFaultTheRuntime)
{
	LifecycleServices services;
	detail::Runtime runtime(services);

	runtime.capture_main_thread();
	runtime.on_engine_update();
	ASSERT_EQ(detail::RuntimeState::Ready, runtime.state());
	ASSERT_EQ(detail::RuntimeTerminalReason::None, runtime.terminal_reason());
	ASSERT_TRUE(services.transport_active);
	ASSERT_TRUE(services.registry_ready);
	services.clear_calls();

	// game_init starts telemetry before the initial game-sequence transition.
	// The engine reports its sentinel state while entering the initial UI.
	runtime.on_game_enter_state(GS_STATE_INVALID, GS_STATE_MAIN_MENU);
	EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
	EXPECT_FALSE(runtime.mission_publication_allowed());

	runtime.on_engine_update();

	EXPECT_TRUE(services.calls.empty());
	EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::None, runtime.terminal_reason());
	EXPECT_EQ(0U, runtime.mission_generation());
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.transport_active);
	EXPECT_TRUE(services.registry_ready);
	EXPECT_TRUE(services.diagnostic_reasons.empty());
}

TEST(TelemetryRuntimeLifecycleContract, EveryLoadingPreserveStateKeepsAnAppliedMissionLoadingWithoutPurge)
{
	for (int state = 0; state < GS_NUM_STATES; ++state) {
		if (ExpectedGameStateDispositions[static_cast<std::size_t>(state)] !=
			GameStateDisposition::LoadingPreserve) {
			continue;
		}
		SCOPED_TRACE(state);
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_ready(runtime, services);
		runtime.on_game_mission_load();
		runtime.on_engine_update();
		ASSERT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		ASSERT_EQ(1U, runtime.mission_generation());
		services.clear_calls();

		runtime.on_game_leave_state(GS_STATE_BRIEFING, state);
		runtime.on_game_enter_state(GS_STATE_BRIEFING, state);
		EXPECT_TRUE(services.calls.empty());
		runtime.on_engine_update();

		EXPECT_TRUE(services.calls.empty());
		EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		EXPECT_EQ(1U, runtime.mission_generation());
		EXPECT_TRUE(services.transport_active);
		EXPECT_TRUE(services.registry_ready);
	}
}

TEST(TelemetryRuntimeLifecycleContract, EveryContextStatePreservesMissionActiveWithoutPhaseTwoEvents)
{
	for (int state = 0; state < GS_NUM_STATES; ++state) {
		if (ExpectedGameStateDispositions[static_cast<std::size_t>(state)] !=
			GameStateDisposition::PreserveContext) {
			continue;
		}
		SCOPED_TRACE(state);
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, state);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, state);
		runtime.on_engine_update();

		EXPECT_TRUE(services.calls.empty());
		EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
		EXPECT_EQ(1U, runtime.mission_generation());
		EXPECT_TRUE(runtime.mission_publication_allowed());
	}
}

TEST(TelemetryRuntimeLifecycleContract, InvalidOldOrNewStateValuesFailClosedOnEnterAndLeaveAtTheNextTick)
{
	enum class CallbackKind : std::uint8_t { Enter = 0, Leave };
	struct InvalidTransition {
		int old_state;
		int new_state;
	};
	const std::array<CallbackKind, 2> callbacks{{CallbackKind::Enter, CallbackKind::Leave}};
	const std::array<InvalidTransition, 7> invalid_transitions{{
		{GS_STATE_INVALID, GS_STATE_MAIN_MENU},
		{-1, GS_STATE_GAME_PLAY},
		{GS_STATE_GAME_PLAY, -1},
		{GS_NUM_STATES, GS_STATE_GAME_PLAY},
		{GS_STATE_GAME_PLAY, GS_NUM_STATES},
		{std::numeric_limits<int>::max(), GS_STATE_GAME_PLAY},
		{GS_STATE_GAME_PLAY, std::numeric_limits<int>::max()},
	}};

	for (const auto callback : callbacks) {
		for (const auto transition : invalid_transitions) {
			SCOPED_TRACE(static_cast<unsigned int>(callback));
			SCOPED_TRACE(transition.old_state);
			SCOPED_TRACE(transition.new_state);
			LifecycleServices services;
			detail::Runtime runtime(services);
			start_mission_active(runtime, services);

			if (callback == CallbackKind::Enter) {
				runtime.on_game_enter_state(transition.old_state, transition.new_state);
			} else {
				runtime.on_game_leave_state(transition.old_state, transition.new_state);
			}

			EXPECT_TRUE(services.calls.empty());
			EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
			EXPECT_FALSE(runtime.mission_publication_allowed());

			runtime.on_engine_update();

			auto expected = FaultTeardownOrder;
			expected.push_back(LifecycleCall::EmitDiagnostic);
			EXPECT_EQ(expected, services.calls);
			EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
			EXPECT_EQ(detail::RuntimeTerminalReason::InvalidLifecycleTransition,
				runtime.terminal_reason());
			EXPECT_EQ(1U, runtime.diagnostic_count());
		}
	}
}

TEST(TelemetryRuntimeLifecycleContract, InvalidTransitionRemainsStickyWhenAValidTransitionArrivesBeforeTheTick)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);

	runtime.on_game_enter_state(-1, GS_STATE_GAME_PLAY);
	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_GAME_PAUSED);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_GAME_PAUSED);
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_engine_update();

	auto expected = FaultTeardownOrder;
	expected.push_back(LifecycleCall::EmitDiagnostic);
	EXPECT_EQ(expected, services.calls);
	EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::InvalidLifecycleTransition, runtime.terminal_reason());
}

TEST(TelemetryRuntimeLifecycleContract, ExplicitPauseEdgesArePublishedSynchronouslyAndResumeRearmsTransport)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);
	services.tick_contexts.clear();

	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_GAME_PAUSED);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_GAME_PAUSED);
	runtime.on_mission_pause_changed(true);

	ASSERT_EQ(8U, services.tick_contexts.size());
	for (std::size_t index = 0U; index < services.tick_contexts.size(); ++index) {
		const auto& context = services.tick_contexts[index];
		EXPECT_TRUE(context.mission_active);
		EXPECT_TRUE(context.mission_paused);
		EXPECT_EQ(index == 0U, context.resume_transport);
	}
	const auto session_generation = services.tick_contexts.back().mission_generation;

	// EngineUpdate continues to service the transport while pause UI states are
	// active. The explicit edge must remain latched instead of being overwritten
	// by the default tick context immediately after the synchronous drain.
	services.tick_contexts.clear();
	runtime.on_engine_update();
	ASSERT_EQ(1U, services.tick_contexts.size());
	EXPECT_TRUE(services.tick_contexts[0].mission_paused);

	services.tick_contexts.clear();
	runtime.on_game_leave_state(GS_STATE_GAME_PAUSED, GS_STATE_GAME_PLAY);
	runtime.on_game_enter_state(GS_STATE_GAME_PAUSED, GS_STATE_GAME_PLAY);
	runtime.on_mission_pause_changed(false);

	ASSERT_EQ(8U, services.tick_contexts.size());
	EXPECT_TRUE(services.tick_contexts[0].resume_transport);
	EXPECT_FALSE(services.tick_contexts[1].resume_transport);
	for (const auto& context : services.tick_contexts) {
		EXPECT_TRUE(context.mission_active);
		EXPECT_FALSE(context.mission_paused);
		EXPECT_EQ(session_generation, context.mission_generation);
	}
	services.tick_contexts.clear();
	runtime.on_engine_update();
	ASSERT_EQ(1U, services.tick_contexts.size());
	EXPECT_FALSE(services.tick_contexts[0].mission_paused);
	EXPECT_TRUE(runtime.mission_publication_allowed());
}

TEST(TelemetryRuntimeLifecycleContract, MissionLoadBlocksPublicationImmediatelyThenPurgesOnTheNextMainThreadTick)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_ready(runtime, services);

	EXPECT_EQ(0U, runtime.mission_generation());
	runtime.on_game_mission_load();

	EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
	EXPECT_EQ(0U, runtime.mission_generation());
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
	EXPECT_EQ(1U, runtime.mission_generation());
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.transport_active) << "Mission purge must retain the process-lifetime listener.";
	EXPECT_TRUE(services.registry_ready) << "Mission purge must retain the process session-ID registry.";
}

TEST(TelemetryRuntimeLifecycleContract, RepeatedLoadsCoalesceBeforeATickAndDistinctAppliedLoadsAdvanceGenerationOnce)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_ready(runtime, services);

	runtime.on_game_mission_load();
	runtime.on_game_mission_load();
	runtime.on_game_mission_load();
	EXPECT_TRUE(services.calls.empty());
	EXPECT_EQ(0U, runtime.mission_generation());

	runtime.on_engine_update();
	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(1U, runtime.mission_generation());
	EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
	services.clear_calls();

	runtime.on_game_mission_load();
	runtime.on_game_mission_load();
	runtime.on_engine_update();
	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(2U, runtime.mission_generation());
	EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
	EXPECT_TRUE(services.transport_active);
	EXPECT_TRUE(services.registry_ready);
}

TEST(TelemetryRuntimeLifecycleContract, MissionLoadFollowedByTerminalPairPurgesOnceAndEndsReady)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);

	runtime.on_game_mission_load();
	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
	EXPECT_EQ(2U, runtime.mission_generation());
	EXPECT_TRUE(services.transport_active);
	EXPECT_TRUE(services.registry_ready);
}

TEST(TelemetryRuntimeLifecycleContract, TerminalPairFollowedByMissionLoadPurgesOnceAndEndsMissionLoading)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);

	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_mission_load();
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
	EXPECT_EQ(2U, runtime.mission_generation());
	EXPECT_TRUE(services.transport_active);
	EXPECT_TRUE(services.registry_ready);
}

TEST(TelemetryRuntimeLifecycleContract, TerminalThenMissionLoadThenGamePlayPreservesObservationOrderWithoutAQueue)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);

	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_mission_load();
	runtime.on_game_leave_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	EXPECT_EQ(2U, runtime.mission_generation());
	EXPECT_TRUE(runtime.mission_publication_allowed());
}

TEST(TelemetryRuntimeLifecycleContract, RestartMissionSelfTransitionLoadsAfterGamePlayAndReactivatesTheReplacementGeneration)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);
	ASSERT_EQ(1U, runtime.mission_generation());
	services.tick_contexts.clear();

	// ESC -> Restart Mission posts a forced GAME_PLAY -> GAME_PLAY transition.
	// The engine enters GAME_PLAY before mission_load() emits GameMissionLoad.
	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_GAME_PLAY);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_GAME_PLAY);
	runtime.on_game_mission_load();
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	EXPECT_EQ(2U, runtime.mission_generation());
	EXPECT_TRUE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.transport_active) << "Restart Mission must retain the process-lifetime listener.";
	EXPECT_TRUE(services.registry_ready) << "Restart Mission must retain the process session-ID registry.";
	ASSERT_EQ(1U, services.tick_contexts.size());
	EXPECT_EQ(2U, services.tick_contexts.front().mission_generation);
	EXPECT_TRUE(services.tick_contexts.front().mission_active);
}

TEST(TelemetryRuntimeLifecycleContract, PreserveContextKeepsThePendingActiveDestinationAfterTerminalReentry)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);

	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
	runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_OPTIONS_MENU);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_OPTIONS_MENU);
	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	EXPECT_EQ(1U, runtime.mission_generation());
	EXPECT_TRUE(runtime.mission_publication_allowed());
}

TEST(TelemetryRuntimeLifecycleContract, LoadingPreserveTurnsThePendingActiveDestinationIntoReady)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);

	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
	runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_BRIEFING);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_BRIEFING);
	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
	EXPECT_EQ(1U, runtime.mission_generation());
	EXPECT_FALSE(runtime.mission_publication_allowed());
}

TEST(TelemetryRuntimeLifecycleContract, LoadingPreservePurgesThePendingActiveDestinationFromMissionLoading)
{
	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_ready(runtime, services);
		runtime.on_game_mission_load();
		runtime.on_engine_update();
		ASSERT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		ASSERT_EQ(1U, runtime.mission_generation());
		services.clear_calls();

		runtime.on_game_leave_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_BRIEFING);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_BRIEFING);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
		EXPECT_EQ(1U, runtime.mission_generation());
		EXPECT_FALSE(runtime.mission_publication_allowed());
	}

	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_ready(runtime, services);
		runtime.on_game_mission_load();
		runtime.on_engine_update();
		ASSERT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		ASSERT_EQ(1U, runtime.mission_generation());
		services.clear_calls();

		runtime.on_game_leave_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_BRIEFING);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_BRIEFING);
		runtime.on_game_leave_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
		EXPECT_EQ(1U, runtime.mission_generation());
		EXPECT_TRUE(runtime.mission_publication_allowed());
	}
}

TEST(TelemetryRuntimeLifecycleContract, LoadingPreserveCannotResurrectALoadClosedByANewerTerminalMarker)
{
	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		runtime.on_game_mission_load();
		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_BRIEFING);
		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_BRIEFING);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
		EXPECT_EQ(2U, runtime.mission_generation());
		EXPECT_FALSE(runtime.mission_publication_allowed());
	}

	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_mission_load();
		runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_BRIEFING);
		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_BRIEFING);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		EXPECT_EQ(2U, runtime.mission_generation());
		EXPECT_FALSE(runtime.mission_publication_allowed());
	}
}

TEST(TelemetryRuntimeLifecycleContract, ActiveRootCannotReuseALoadClosedByANewerTerminalMarker)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_ready(runtime, services);

	runtime.on_game_mission_load();
	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
	runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_engine_update();

	auto expected = FaultTeardownOrder;
	expected.push_back(LifecycleCall::EmitDiagnostic);
	EXPECT_EQ(expected, services.calls);
	EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::InvalidLifecycleTransition, runtime.terminal_reason());
	EXPECT_EQ(0U, runtime.mission_generation());
	EXPECT_FALSE(runtime.mission_publication_allowed());
}

TEST(TelemetryRuntimeLifecycleContract, ActiveMissionCannotReuseALoadClosedByANewerTerminalMarker)
{
	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		runtime.on_game_mission_load();
		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_engine_update();

		auto expected = FaultTeardownOrder;
		expected.push_back(LifecycleCall::EmitDiagnostic);
		EXPECT_EQ(expected, services.calls);
		EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
		EXPECT_EQ(detail::RuntimeTerminalReason::InvalidLifecycleTransition,
			runtime.terminal_reason());
		EXPECT_EQ(1U, runtime.mission_generation());
		EXPECT_FALSE(runtime.mission_publication_allowed());
	}

	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_ready(runtime, services);

		runtime.on_game_mission_load();
		runtime.on_game_leave_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
		EXPECT_EQ(1U, runtime.mission_generation());
		EXPECT_TRUE(runtime.mission_publication_allowed());
	}

	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		runtime.on_game_mission_load();
		runtime.on_game_leave_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
		EXPECT_EQ(2U, runtime.mission_generation());
		EXPECT_TRUE(runtime.mission_publication_allowed());
	}

	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		runtime.on_game_leave_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
		EXPECT_EQ(1U, runtime.mission_generation());
		EXPECT_TRUE(runtime.mission_publication_allowed());
	}
}

TEST(TelemetryRuntimeLifecycleContract, LeaveThenReenterBeforeTheNextTickCoalescesOnePurgeAndKeepsTheFinalState)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);
	ASSERT_EQ(1U, runtime.mission_generation());

	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_DEBRIEF);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_DEBRIEF);
	EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_game_leave_state(GS_STATE_DEBRIEF, GS_STATE_GAME_PLAY);
	runtime.on_game_enter_state(GS_STATE_DEBRIEF, GS_STATE_GAME_PLAY);
	EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	EXPECT_EQ(1U, runtime.mission_generation()) << "Leave/reentry is not a distinct mission load.";
	EXPECT_TRUE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.transport_active);
	EXPECT_TRUE(services.registry_ready);
}

TEST(TelemetryRuntimeLifecycleContract, NormalTerminalLeaveAndEnterPairProducesExactlyOneMissionPurge)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);

	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);

	EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_TRUE(services.calls.empty());

	runtime.on_engine_update();

	EXPECT_EQ(MissionPurgeOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
	EXPECT_EQ(1U, runtime.mission_generation());
	EXPECT_TRUE(services.transport_active);
	EXPECT_TRUE(services.registry_ready);
}

TEST(TelemetryRuntimeLifecycleContract, LoadingScreenFromActivePurgesButOnlyAnOpenLoadCanEndInMissionLoading)
{
	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MULTI_HOST_OPTIONS);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::Ready, runtime.state());
		EXPECT_EQ(1U, runtime.mission_generation());
	}

	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		runtime.on_game_mission_load();
		runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_BRIEFING);
		runtime.on_engine_update();

		EXPECT_EQ(MissionPurgeOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		EXPECT_EQ(2U, runtime.mission_generation());
	}
}

TEST(TelemetryRuntimeLifecycleContract, GamePlayRequiresAnAppliedMissionLoadBeforeMissionActivation)
{
	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_ready(runtime, services);

		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_engine_update();

		EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
		EXPECT_NE(detail::RuntimeTerminalReason::None, runtime.terminal_reason());
		EXPECT_FALSE(runtime.mission_publication_allowed());
	}

	{
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_ready(runtime, services);
		runtime.on_game_mission_load();
		runtime.on_engine_update();
		ASSERT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		services.clear_calls();

		runtime.on_game_enter_state(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
		EXPECT_EQ(detail::RuntimeState::MissionLoading, runtime.state());
		EXPECT_FALSE(runtime.mission_publication_allowed());
		EXPECT_TRUE(services.calls.empty());

		runtime.on_engine_update();
		EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
		EXPECT_TRUE(runtime.mission_publication_allowed());
	}
}

TEST(TelemetryRuntimeLifecycleContract, MissionGenerationOverflowPurgesResourcesAndFaultsWithoutWrapping)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);
	detail::RuntimeLifecycleTestAccess::set_mission_generation(
		runtime, std::numeric_limits<std::uint32_t>::max());
	ASSERT_EQ(std::numeric_limits<std::uint32_t>::max(), runtime.mission_generation());

	runtime.on_game_mission_load();
	runtime.on_engine_update();

	auto expected = FaultTeardownOrder;
	expected.push_back(LifecycleCall::EmitDiagnostic);
	EXPECT_EQ(expected, services.calls);
	EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::MissionGenerationOverflow, runtime.terminal_reason());
	EXPECT_EQ(std::numeric_limits<std::uint32_t>::max(), runtime.mission_generation());
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_FALSE(services.transport_active);
	EXPECT_FALSE(services.registry_ready);
	ASSERT_EQ(1U, services.diagnostic_reasons.size());
	EXPECT_EQ(detail::RuntimeTerminalReason::MissionGenerationOverflow, services.diagnostic_reasons.front());
}

TEST(TelemetryRuntimeLifecycleContract, EngineShutdownUsesFixedOrderAndAllLaterCallbacksAreInert)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);

	runtime.on_engine_shutdown();

	EXPECT_EQ(GlobalTeardownOrder, services.calls);
	EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
	EXPECT_EQ(0U, runtime.published_session_id());
	EXPECT_FALSE(services.registry_ready);

	const auto calls_after_first_shutdown = services.calls;
	runtime.on_engine_shutdown();
	runtime.on_engine_update();
	runtime.on_game_mission_load();
	runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
	runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);

	EXPECT_EQ(calls_after_first_shutdown, services.calls);
	EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
}

TEST(TelemetryRuntimeLifecycleContract, ShutdownConvergesFromEveryStableStateAndNeverRunsTwice)
{
	const std::array<StableRuntimeSetup, 6> setups{{
		StableRuntimeSetup::Cold,
		StableRuntimeSetup::Disabled,
		StableRuntimeSetup::Faulted,
		StableRuntimeSetup::Ready,
		StableRuntimeSetup::MissionLoading,
		StableRuntimeSetup::MissionActive,
	}};

	for (const auto setup : setups) {
		SCOPED_TRACE(static_cast<unsigned int>(setup));
		LifecycleServices services;
		detail::Runtime runtime(services);
		arrange_stable_state(setup, runtime, services);
		services.clear_calls();

		runtime.on_engine_shutdown();

		EXPECT_EQ(GlobalTeardownOrder, services.calls);
		EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
		EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
		EXPECT_EQ(0U, runtime.published_session_id());
		EXPECT_FALSE(runtime.mission_publication_allowed());
		EXPECT_FALSE(services.transport_active);
		EXPECT_FALSE(services.registry_ready);

		const auto calls_after_shutdown = services.calls;
		runtime.on_engine_shutdown();
		runtime.on_engine_update();
		runtime.on_game_mission_load();
		runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
		runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
		EXPECT_EQ(calls_after_shutdown, services.calls);
		EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
	}
}

TEST(TelemetryRuntimeLifecycleContract, ShutdownDuringStartingStopsTheOuterStartupTransaction)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	services.observed_runtime = &runtime;
	services.shutdown_during_config = true;
	runtime.capture_main_thread();
	services.clear_calls();

	runtime.on_engine_update();

	std::vector<LifecycleCall> expected{
		LifecycleCall::MainThreadCheck,
		LifecycleCall::LoadConfig,
	};
	expected.insert(expected.end(), GlobalTeardownOrder.begin(), GlobalTeardownOrder.end());
	EXPECT_EQ(expected, services.calls);
	EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
	EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
	EXPECT_EQ(0, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::LoadProducerIdentity));
	EXPECT_EQ(0, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::StartTransport));

	const auto calls_after_stop = services.calls;
	runtime.on_engine_update();
	runtime.on_engine_shutdown();
	EXPECT_EQ(calls_after_stop, services.calls);
}

TEST(TelemetryRuntimeLifecycleContract, ReentrantShutdownWhileShuttingDownIsANoopAndSummaryRemainsUnique)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_ready(runtime, services);
	services.shutdown_during_stop_collection = true;

	runtime.on_engine_shutdown();

	EXPECT_EQ(GlobalTeardownOrder, services.calls);
	EXPECT_EQ(1U, services.reentrant_shutdown_attempts);
	EXPECT_EQ(detail::RuntimeState::ShuttingDown, services.reentrant_shutdown_observed_state);
	EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
	EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());

	const auto calls_after_stop = services.calls;
	runtime.on_engine_shutdown();
	EXPECT_EQ(calls_after_stop, services.calls);
}

TEST(TelemetryRuntimeLifecycleContract, ShutdownInsideMissionPurgeStopsTheOuterTransactionWithoutResurrection)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);
	ASSERT_EQ(1U, runtime.mission_generation());
	services.shutdown_during_stop_collection = true;

	runtime.on_game_mission_load();
	runtime.on_engine_update();

	auto expected = std::vector<LifecycleCall>{LifecycleCall::StopCollection};
	expected.insert(expected.end(), GlobalTeardownOrder.begin(), GlobalTeardownOrder.end());
	EXPECT_EQ(expected, services.calls);
	EXPECT_EQ(1U, services.reentrant_shutdown_attempts);
	EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
	EXPECT_EQ(1U, runtime.mission_generation()) << "A load interrupted by shutdown was never applied.";
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
	EXPECT_EQ(0U, runtime.diagnostic_count());
}

TEST(TelemetryRuntimeLifecycleContract, ShutdownInsideFaultTeardownStopsEveryOuterHookAfterStopped)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	start_mission_active(runtime, services);
	detail::RuntimeLifecycleTestAccess::set_mission_generation(
		runtime, std::numeric_limits<std::uint32_t>::max());
	services.shutdown_during_stop_collection = true;

	runtime.on_game_mission_load();
	runtime.on_engine_update();

	auto expected = std::vector<LifecycleCall>{LifecycleCall::StopCollection};
	expected.insert(expected.end(), GlobalTeardownOrder.begin(), GlobalTeardownOrder.end());
	EXPECT_EQ(expected, services.calls);
	EXPECT_EQ(1U, services.reentrant_shutdown_attempts);
	EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
	EXPECT_FALSE(runtime.mission_publication_allowed());
	EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
	EXPECT_EQ(0U, runtime.diagnostic_count()) << "The interrupted fault teardown must not emit after Stopped.";
}

TEST(TelemetryRuntimeLifecycleContract, ShutdownInsideMainThreadCheckDominatesItsReturnedFailure)
{
	LifecycleServices services;
	detail::Runtime runtime(services);
	services.observed_runtime = &runtime;
	services.shutdown_during_main_thread_check = true;
	services.main_thread_check_result = false;
	runtime.capture_main_thread();
	services.clear_calls();

	runtime.on_engine_update();

	auto expected = std::vector<LifecycleCall>{LifecycleCall::MainThreadCheck};
	expected.insert(expected.end(), GlobalTeardownOrder.begin(), GlobalTeardownOrder.end());
	EXPECT_EQ(expected, services.calls);
	EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
	EXPECT_EQ(1U, services.reentrant_shutdown_attempts);
	EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
	EXPECT_EQ(0U, runtime.diagnostic_count());
}

TEST(TelemetryRuntimeLifecycleContract, ShutdownInsideRegistryRollbackDominatesAllocationAndRegistrationFailures)
{
	enum class FailureSite : std::uint8_t { Allocation = 0, Registration };
	for (const auto site : {FailureSite::Allocation, FailureSite::Registration}) {
		SCOPED_TRACE(static_cast<int>(site));
		LifecycleServices services;
		detail::Runtime runtime(services);
		services.observed_runtime = &runtime;
		services.shutdown_during_release_registry = true;
		if (site == FailureSite::Allocation) {
			services.allocation_result = false;
		} else {
			services.registration_status = detail::SessionIdRegistrationStatus::Duplicate;
		}
		runtime.capture_main_thread();
		services.clear_calls();

		runtime.on_engine_update();

		std::vector<LifecycleCall> expected{
			LifecycleCall::MainThreadCheck,
			LifecycleCall::LoadConfig,
			LifecycleCall::LoadProducerIdentity,
			LifecycleCall::DrawSessionCandidate,
			LifecycleCall::CalculateBudget,
			LifecycleCall::AllocateRegistry,
		};
		if (site == FailureSite::Registration) {
			expected.push_back(LifecycleCall::RegisterCandidate);
		}
		expected.push_back(LifecycleCall::ReleaseRegistry);
		expected.insert(expected.end(), GlobalTeardownOrder.begin(), GlobalTeardownOrder.end());
		EXPECT_EQ(expected, services.calls);
		EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
		EXPECT_EQ(1U, services.reentrant_shutdown_attempts);
		EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
		EXPECT_EQ(0U, runtime.diagnostic_count());
	}
}

TEST(TelemetryRuntimeLifecycleContract, ShutdownInsideTransportFailureCleanupDominatesStopAndReleaseHooks)
{
	enum class HookSite : std::uint8_t { StopTransport = 0, ReleaseRegistry };
	for (const auto site : {HookSite::StopTransport, HookSite::ReleaseRegistry}) {
		SCOPED_TRACE(static_cast<int>(site));
		LifecycleServices services;
		detail::Runtime runtime(services);
		services.observed_runtime = &runtime;
		services.transport_status = detail::RuntimeTransportStatus::Unavailable;
		services.shutdown_during_stop_transport = site == HookSite::StopTransport;
		services.shutdown_during_release_registry = site == HookSite::ReleaseRegistry;
		runtime.capture_main_thread();
		services.clear_calls();

		runtime.on_engine_update();

		std::vector<LifecycleCall> expected{
			LifecycleCall::MainThreadCheck,
			LifecycleCall::LoadConfig,
			LifecycleCall::LoadProducerIdentity,
			LifecycleCall::DrawSessionCandidate,
			LifecycleCall::CalculateBudget,
			LifecycleCall::AllocateRegistry,
			LifecycleCall::RegisterCandidate,
			LifecycleCall::StartTransport,
			LifecycleCall::StopTransport,
		};
		if (site == HookSite::ReleaseRegistry) {
			expected.push_back(LifecycleCall::ReleaseRegistry);
		}
		expected.insert(expected.end(), GlobalTeardownOrder.begin(), GlobalTeardownOrder.end());
		EXPECT_EQ(expected, services.calls);
		EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
		EXPECT_EQ(1U, services.reentrant_shutdown_attempts);
		EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
		EXPECT_EQ(0U, runtime.diagnostic_count());
	}
}

TEST(TelemetryRuntimeLifecycleContract, EveryCallbackIsAtomicallyInertThroughoutShuttingDown)
{
	const std::array<RuntimeCallbackKind, 5> callbacks{{
		RuntimeCallbackKind::EngineUpdate,
		RuntimeCallbackKind::EngineShutdown,
		RuntimeCallbackKind::MissionLoad,
		RuntimeCallbackKind::EnterState,
		RuntimeCallbackKind::LeaveState,
	}};

	for (const auto callback : callbacks) {
		SCOPED_TRACE(static_cast<unsigned int>(callback));
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_ready(runtime, services);
		ASSERT_FALSE(detail::RuntimeLifecycleTestAccess::wrong_thread_violation(runtime));
		services.worker_callback_during_stop_collection = callback;

		runtime.on_engine_shutdown();

		EXPECT_EQ(GlobalTeardownOrder, services.calls);
		EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
		EXPECT_FALSE(detail::RuntimeLifecycleTestAccess::wrong_thread_violation(runtime))
			<< "The atomic ShuttingDown gate must win before any worker reads non-atomic runtime state.";
		EXPECT_EQ(1, std::count(services.calls.begin(), services.calls.end(), LifecycleCall::EmitRuntimeSummary));
	}
}

TEST(TelemetryRuntimeLifecycleContract, EveryWrongThreadCallbackOnlyLatchesUntilMainThreadTeardown)
{
	enum class CallbackKind : std::uint8_t {
		EngineUpdate = 0,
		EngineShutdown,
		MissionLoad,
		EnterState,
		LeaveState,
	};

	const std::array<CallbackKind, 5> callbacks{{
		CallbackKind::EngineUpdate,
		CallbackKind::EngineShutdown,
		CallbackKind::MissionLoad,
		CallbackKind::EnterState,
		CallbackKind::LeaveState,
	}};

	for (const auto callback : callbacks) {
		SCOPED_TRACE(static_cast<unsigned int>(callback));
		LifecycleServices services;
		detail::Runtime runtime(services);
		start_mission_active(runtime, services);

		std::thread wrong_thread([&runtime, callback]() {
			switch (callback) {
			case CallbackKind::EngineUpdate:
				runtime.on_engine_update();
				break;
			case CallbackKind::EngineShutdown:
				runtime.on_engine_shutdown();
				break;
			case CallbackKind::MissionLoad:
				runtime.on_game_mission_load();
				break;
			case CallbackKind::EnterState:
				runtime.on_game_enter_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
				break;
			case CallbackKind::LeaveState:
				runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
				break;
			}
		});
		wrong_thread.join();

		EXPECT_TRUE(services.calls.empty())
			<< "No service, engine-global, teardown or diagnostic hook may run on the wrong thread.";
		EXPECT_TRUE(services.diagnostic_reasons.empty());
		EXPECT_EQ(detail::RuntimeState::MissionActive, runtime.state());
		EXPECT_EQ(detail::RuntimeTerminalReason::None, runtime.terminal_reason());
		EXPECT_FALSE(runtime.mission_publication_allowed())
			<< "The cross-thread invariant latch must block publication before deferred cleanup.";

		runtime.on_engine_update();

		auto expected = FaultTeardownOrder;
		expected.push_back(LifecycleCall::EmitDiagnostic);
		EXPECT_EQ(expected, services.calls);
		EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
		EXPECT_EQ(detail::RuntimeTerminalReason::MainThreadViolation, runtime.terminal_reason());
		EXPECT_EQ(1U, runtime.diagnostic_count());
		ASSERT_EQ(1U, services.diagnostic_reasons.size());
		EXPECT_EQ(detail::RuntimeTerminalReason::MainThreadViolation, services.diagnostic_reasons.front());
	}
}

TEST(TelemetryRuntimeLifecycleContract, EveryStoppedCallbackIsInertEvenWhenInvokedFromAnotherThread)
{
	enum class CallbackKind : std::uint8_t {
		EngineUpdate = 0,
		EngineShutdown,
		MissionLoad,
		EnterState,
		LeaveState,
	};
	const std::array<CallbackKind, 5> callbacks{{
		CallbackKind::EngineUpdate,
		CallbackKind::EngineShutdown,
		CallbackKind::MissionLoad,
		CallbackKind::EnterState,
		CallbackKind::LeaveState,
	}};

	LifecycleServices services;
	detail::Runtime runtime(services);
	start_ready(runtime, services);
	runtime.on_engine_shutdown();
	ASSERT_EQ(detail::RuntimeState::Stopped, runtime.state());
	ASSERT_FALSE(detail::RuntimeLifecycleTestAccess::wrong_thread_violation(runtime));
	services.clear_calls();

	for (const auto callback : callbacks) {
		SCOPED_TRACE(static_cast<unsigned int>(callback));
		std::thread late_thread([&runtime, callback]() {
			switch (callback) {
			case CallbackKind::EngineUpdate:
				runtime.on_engine_update();
				break;
			case CallbackKind::EngineShutdown:
				runtime.on_engine_shutdown();
				break;
			case CallbackKind::MissionLoad:
				runtime.on_game_mission_load();
				break;
			case CallbackKind::EnterState:
				runtime.on_game_enter_state(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
				break;
			case CallbackKind::LeaveState:
				runtime.on_game_leave_state(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
				break;
			}
		});
		late_thread.join();

		EXPECT_TRUE(services.calls.empty());
		EXPECT_EQ(detail::RuntimeState::Stopped, runtime.state());
		EXPECT_FALSE(detail::RuntimeLifecycleTestAccess::wrong_thread_violation(runtime));
	}
}

} // namespace
