#include "telemetry/runtime.h"
#include "telemetry/phase2_observation.h"

#include "gamesequence/gamesequence.h"

#include <array>
#include <chrono>
#include <limits>
#include <thread>

namespace telemetry::detail {
namespace {

constexpr std::array<RuntimeGameStateDisposition, GS_NUM_STATES> GameStateDispositions{{
	RuntimeGameStateDisposition::Invalid,          // GS_STATE_INVALID
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_MAIN_MENU
	RuntimeGameStateDisposition::ActiveRoot,       // GS_STATE_GAME_PLAY
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_GAME_PAUSED
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_QUIT_GAME
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_OPTIONS_MENU
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_BARRACKS_MENU
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_TECH_MENU
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_TRAINING_MENU
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_LOAD_MISSION_MENU
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_BRIEFING
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_SHIP_SELECT
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_DEBUG_PAUSED
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_HUD_CONFIG
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_MULTI_JOIN_GAME
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_CONTROL_CONFIG
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_WEAPON_SELECT
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_MISSION_LOG_SCROLLBACK
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_DEATH_DIED
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_DEATH_BLEW_UP
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_SIMULATOR_ROOM
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_CREDITS
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_SHOW_GOALS
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_HOTKEY_SCREEN
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_VIEW_MEDALS
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_MULTI_HOST_SETUP
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_MULTI_CLIENT_SETUP
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_DEBRIEF
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_VIEW_CUTSCENES
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_MULTI_STD_WAIT
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_STANDALONE_MAIN
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_MULTI_PAUSED
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_TEAM_SELECT
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_TRAINING_PAUSED
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_INGAME_PRE_JOIN
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_EVENT_DEBUG
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_STANDALONE_POSTGAME
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_INITIAL_PLAYER_SELECT
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_MULTI_MISSION_SYNC
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_MULTI_START_GAME
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_MULTI_HOST_OPTIONS
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_MULTI_DOGFIGHT_DEBRIEF
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_CAMPAIGN_ROOM
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_CMD_BRIEF
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_RED_ALERT
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_END_OF_CAMPAIGN
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_GAMEPLAY_HELP
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_LOOP_BRIEF
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_PXO
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_LAB
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_PXO_HELP
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_START_GAME
	RuntimeGameStateDisposition::LoadingPreserve,  // GS_STATE_FICTION_VIEWER
	RuntimeGameStateDisposition::Terminal,         // GS_STATE_SCRIPTING
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_SCRIPTING_MISSION
	RuntimeGameStateDisposition::PreserveContext,  // GS_STATE_INGAME_OPTIONS
}};

static_assert(GameStateDispositions.size() == static_cast<std::size_t>(GS_NUM_STATES),
	"Every engine game state requires an explicit telemetry lifecycle disposition");

class CallbackMetricScope final {
  public:
	CallbackMetricScope(RuntimeStartupServices& services, TelemetryCallbackKind kind) noexcept
		: m_services(services), m_kind(kind), m_started_at(std::chrono::steady_clock::now())
	{
	}
	~CallbackMetricScope() noexcept
	{
		finish();
	}
	void finish() noexcept
	{
		if (m_finished) return;
		m_finished = true;
		const auto ended_at = std::chrono::steady_clock::now();
		// The public histogram is microsecond-based.  A completed accepted
		// callback can be shorter than the source clock's resolution (including
		// deterministic test clocks); retain a one-microsecond lower bound rather
		// than publishing an indistinguishable zero-duration completed callback.
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(ended_at - m_started_at).count();
		const auto duration_us = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
		m_services.record_callback_metric(m_kind, duration_us);
	}

  private:
	RuntimeStartupServices& m_services;
	TelemetryCallbackKind m_kind;
	std::chrono::steady_clock::time_point m_started_at;
	bool m_finished = false;
};

} // namespace

RuntimeGameStateDisposition classify_runtime_game_state(int state) noexcept
{
	if (state < 0 || state >= GS_NUM_STATES) {
		return RuntimeGameStateDisposition::Invalid;
	}
	return GameStateDispositions[static_cast<std::size_t>(state)];
}

Runtime::Runtime(RuntimeStartupServices& services) noexcept : m_services(services) {}

void Runtime::capture_main_thread() noexcept
{
	if (m_state != RuntimeState::Cold || m_main_thread_captured) {
		return;
	}
	m_captured_thread = std::this_thread::get_id();
	m_main_thread_captured = true;
	m_services.capture_main_thread();
}

bool Runtime::callback_gate_is_open() const noexcept
{
	return m_callback_gate.load(std::memory_order_acquire) == RuntimeCallbackGate::Open;
}

bool Runtime::startup_transaction_is_active() const noexcept
{
	return callback_gate_is_open() && m_state == RuntimeState::Starting;
}

bool Runtime::callback_is_on_captured_thread() noexcept
{
	if (m_captured_thread == std::thread::id{}) {
		return true;
	}
	if (std::this_thread::get_id() == m_captured_thread) {
		return true;
	}
	m_wrong_thread_violation.store(true, std::memory_order_relaxed);
	return false;
}

void Runtime::emit_diagnostic_once(RuntimeTerminalReason reason) noexcept
{
	if (m_diagnostic_count != 0U) {
		return;
	}
	m_diagnostic_count = 1U;
	m_services.emit_startup_diagnostic(reason);
}

void Runtime::enter_disabled(RuntimeTerminalReason reason, bool diagnostic) noexcept
{
	m_terminal_reason = reason;
	m_state = RuntimeState::Disabled;
	m_publication_blocked = true;
	m_services.emit_runtime_disabled(reason);
	if (diagnostic) {
		emit_diagnostic_once(reason);
	}
}

void Runtime::enter_faulted(RuntimeTerminalReason reason) noexcept
{
	m_terminal_reason = reason;
	m_state = RuntimeState::Faulted;
	m_publication_blocked = true;
	m_services.record_runtime_fault_metric(reason);
	m_services.emit_runtime_fault(reason);
	emit_diagnostic_once(reason);
}

void Runtime::teardown_faulted_runtime(RuntimeTerminalReason reason) noexcept
{
	m_terminal_reason = reason;
	m_state = RuntimeState::Faulted;
	m_publication_blocked = true;
	m_services.record_runtime_fault_metric(reason);
	m_services.emit_runtime_fault(reason);
	m_mission_load_pending = false;
	m_mission_purge_pending = false;
	m_game_state_pending = false;
	m_lifecycle_destination = RuntimeLifecycleDestination::None;
	m_active_continuity = false;

	m_services.stop_collection();
	if (!callback_gate_is_open()) {
		return;
	}
	m_services.close_sessions_and_stores();
	if (!callback_gate_is_open()) {
		return;
	}
	m_services.invalidate_mission_state_and_entities();
	if (!callback_gate_is_open()) {
		return;
	}
	m_services.stop_transport();
	if (!callback_gate_is_open()) {
		return;
	}
	m_services.release_runtime_allocations();
	if (!callback_gate_is_open()) {
		return;
	}
	m_services.release_metrics();
	if (!callback_gate_is_open()) {
		return;
	}
	m_services.release_session_registry();
	if (!callback_gate_is_open()) {
		return;
	}
	emit_diagnostic_once(reason);
}

bool Runtime::purge_mission() noexcept
{
	m_services.stop_collection();
	if (!callback_gate_is_open()) {
		return false;
	}
	m_services.invalidate_mission_state_and_entities();
	if (!callback_gate_is_open()) {
		return false;
	}
	m_services.cancel_replication();
	if (!callback_gate_is_open()) {
		return false;
	}
	m_services.close_sessions_and_stores();
	if (!callback_gate_is_open()) {
		return false;
	}
	m_services.reset_mission_scope();
	return callback_gate_is_open();
}

void Runtime::record_game_state_transition(int old_state, int new_state) noexcept
{
	const auto old_disposition = classify_runtime_game_state(old_state);
	const auto new_disposition = classify_runtime_game_state(new_state);
	const auto bootstrap_old_state_sentinel = old_state == GS_STATE_INVALID && m_mission_generation == 0U &&
		(m_state == RuntimeState::Cold || m_state == RuntimeState::Ready);
	if ((old_disposition == RuntimeGameStateDisposition::Invalid && !bootstrap_old_state_sentinel) ||
		new_disposition == RuntimeGameStateDisposition::Invalid) {
		m_pending_game_state = RuntimeGameStateDisposition::Invalid;
		m_game_state_pending = true;
		m_publication_blocked = true;
		return;
	}
	if (m_game_state_pending && m_pending_game_state == RuntimeGameStateDisposition::Invalid) {
		return;
	}
	initialize_lifecycle_summary();
	const auto previous_destination = m_lifecycle_destination;
	const auto active_root_allowed =
		m_lifecycle_destination == RuntimeLifecycleDestination::MissionLoading || m_active_continuity;
	if (new_disposition == RuntimeGameStateDisposition::ActiveRoot && !active_root_allowed) {
		m_pending_game_state = RuntimeGameStateDisposition::Invalid;
		m_game_state_pending = true;
		m_publication_blocked = true;
		return;
	}

	m_pending_game_state = new_disposition;
	m_game_state_pending = true;
	switch (new_disposition) {
	case RuntimeGameStateDisposition::ActiveRoot:
		m_lifecycle_destination = RuntimeLifecycleDestination::MissionActive;
		m_active_continuity = true;
		break;
	case RuntimeGameStateDisposition::PreserveContext:
		break;
	case RuntimeGameStateDisposition::LoadingPreserve:
		if (m_lifecycle_destination == RuntimeLifecycleDestination::MissionActive) {
			m_lifecycle_destination = RuntimeLifecycleDestination::Ready;
		}
		break;
	case RuntimeGameStateDisposition::Terminal:
		m_lifecycle_destination = RuntimeLifecycleDestination::Ready;
		break;
	case RuntimeGameStateDisposition::Invalid:
		break;
	}
	const auto terminal_discontinuity = new_disposition == RuntimeGameStateDisposition::Terminal &&
		(previous_destination == RuntimeLifecycleDestination::MissionActive ||
			previous_destination == RuntimeLifecycleDestination::MissionLoading);
	const auto loading_discontinuity =
		new_disposition == RuntimeGameStateDisposition::LoadingPreserve &&
		previous_destination == RuntimeLifecycleDestination::MissionActive;
	if (terminal_discontinuity || loading_discontinuity) {
		m_mission_purge_pending = true;
		m_publication_blocked = true;
	}
}

void Runtime::initialize_lifecycle_summary() noexcept
{
	if (m_lifecycle_destination != RuntimeLifecycleDestination::None) {
		return;
	}

	switch (m_state) {
	case RuntimeState::MissionActive:
		m_lifecycle_destination = RuntimeLifecycleDestination::MissionActive;
		m_active_continuity = true;
		break;
	case RuntimeState::MissionLoading:
		m_lifecycle_destination = RuntimeLifecycleDestination::MissionLoading;
		m_active_continuity = false;
		break;
	default:
		m_lifecycle_destination = RuntimeLifecycleDestination::Ready;
		m_active_continuity = false;
		break;
	}
}

void Runtime::apply_pending_lifecycle() noexcept
{
	if (!m_mission_load_pending && !m_mission_purge_pending && !m_game_state_pending) {
		return;
	}

	const auto disposition = m_pending_game_state;
	const auto has_game_state = m_game_state_pending;
	const auto has_mission_load = m_mission_load_pending;
	const auto needs_mission_purge = m_mission_purge_pending || has_mission_load;
	const auto destination = m_lifecycle_destination;

	m_mission_load_pending = false;
	m_mission_purge_pending = false;
	m_game_state_pending = false;
	m_lifecycle_destination = RuntimeLifecycleDestination::None;
	m_active_continuity = false;

	if (has_game_state && disposition == RuntimeGameStateDisposition::Invalid) {
		teardown_faulted_runtime(RuntimeTerminalReason::InvalidLifecycleTransition);
		return;
	}

	if (has_mission_load && m_mission_generation == std::numeric_limits<std::uint32_t>::max()) {
		teardown_faulted_runtime(RuntimeTerminalReason::MissionGenerationOverflow);
		return;
	}

	if (needs_mission_purge) {
		// A replacement load is a real discontinuity too: close the previous
		// mission before incrementing/publishing the next generation.
		reset_phase2_mission_observation_state();
		if (m_mission_generation != 0U) {
			m_services.emit_mission_left(m_mission_generation);
		}
		if (!purge_mission()) {
			return;
		}
	}

	if (has_mission_load) {
		++m_mission_generation;
		m_services.emit_mission_entered(m_mission_generation);
	}

	if (destination == RuntimeLifecycleDestination::MissionActive) {
		m_state = RuntimeState::MissionActive;
	} else if (destination == RuntimeLifecycleDestination::Ready) {
		m_state = RuntimeState::Ready;
	} else if (destination == RuntimeLifecycleDestination::MissionLoading || has_mission_load) {
		m_state = RuntimeState::MissionLoading;
	} else if (needs_mission_purge) {
		m_state = RuntimeState::Ready;
	}

	m_publication_blocked = false;
}

void Runtime::on_engine_update() noexcept
{
	if (!callback_gate_is_open()) {
		return;
	}
	if (!callback_is_on_captured_thread()) {
		return;
	}
	if (!m_main_thread_captured) {
		enter_faulted(RuntimeTerminalReason::MainThreadNotCaptured);
		return;
	}
	if (m_wrong_thread_violation.load(std::memory_order_relaxed)) {
		if (m_state != RuntimeState::Faulted) {
			teardown_faulted_runtime(RuntimeTerminalReason::MainThreadViolation);
		}
		return;
	}
	if (m_state == RuntimeState::Disabled || m_state == RuntimeState::Faulted) {
		return;
	}
	CallbackMetricScope callback_metric(m_services, TelemetryCallbackKind::EngineUpdate);

	if (m_state == RuntimeState::Cold) {
		const auto main_thread_matches = m_services.is_on_captured_main_thread();
		if (!callback_gate_is_open() || m_state != RuntimeState::Cold) {
			return;
		}
		if (!main_thread_matches) {
			enter_faulted(RuntimeTerminalReason::MainThreadViolation);
			return;
		}

		m_state = RuntimeState::Starting;
		const auto config = m_services.load_config();
		if (!startup_transaction_is_active()) {
			return;
		}
		switch (config.status) {
		case RuntimeConfigStatus::Absent:
			enter_disabled(RuntimeTerminalReason::ConfigAbsent, false);
			return;
		case RuntimeConfigStatus::Invalid:
			enter_disabled(RuntimeTerminalReason::ConfigInvalid, true);
			return;
		case RuntimeConfigStatus::Disabled:
			enter_disabled(RuntimeTerminalReason::ConfigDisabled, false);
			return;
		case RuntimeConfigStatus::Enabled:
			break;
		}

		const auto identity = m_services.load_producer_identity();
		if (!startup_transaction_is_active()) {
			return;
		}
		if (identity.error != IdentityError::None || identity.producer_id == 0U) {
			enter_faulted(RuntimeTerminalReason::ProducerIdentityFailure);
			return;
		}

		const auto candidate = m_services.draw_session_candidate();
		if (!startup_transaction_is_active()) {
			return;
		}
		if (candidate.status != SessionIdCandidateStatus::Ready || candidate.session_id == 0U) {
			enter_faulted(RuntimeTerminalReason::SessionCandidateFailure);
			return;
		}

		const auto budget = m_services.calculate_known_budget(config.max_clients);
		if (!startup_transaction_is_active()) {
			return;
		}
		if (budget.error != StartupBudgetError::None || !budget.is_complete ||
			budget.deferred_categories != 0U) {
			enter_faulted(RuntimeTerminalReason::BudgetFailure);
			return;
		}

		if (!m_services.provision_metrics()) {
			enter_faulted(RuntimeTerminalReason::AllocationFailure);
			return;
		}
		if (!startup_transaction_is_active()) {
			return;
		}

		const auto registry_allocated = m_services.allocate_session_registry();
		if (!startup_transaction_is_active()) {
			return;
		}
		if (!registry_allocated) {
			m_services.release_session_registry();
			if (!startup_transaction_is_active()) {
				return;
			}
			enter_faulted(RuntimeTerminalReason::AllocationFailure);
			m_services.release_metrics();
			return;
		}

		const auto registration_status = m_services.register_session_candidate(candidate.session_id);
		if (!startup_transaction_is_active()) {
			return;
		}
		if (registration_status != SessionIdRegistrationStatus::Registered) {
			m_services.release_session_registry();
			if (!startup_transaction_is_active()) {
				return;
			}
			enter_faulted(RuntimeTerminalReason::SessionRegistrationFailure);
			m_services.release_metrics();
			return;
		}

		const auto transport_status = m_services.start_transport();
		if (!startup_transaction_is_active()) {
			return;
		}
		if (transport_status != RuntimeTransportStatus::Started) {
			m_services.stop_transport();
			if (!startup_transaction_is_active()) {
				return;
			}
			m_services.release_session_registry();
			if (!startup_transaction_is_active()) {
				return;
			}
			enter_faulted(RuntimeTerminalReason::TransportUnavailable);
			m_services.release_metrics();
			return;
		}

		m_terminal_reason = RuntimeTerminalReason::None;
		m_state = RuntimeState::Ready;
		m_publication_blocked = false;
		m_services.emit_runtime_activated();
	}

	if (m_state == RuntimeState::Ready || m_state == RuntimeState::MissionLoading ||
		m_state == RuntimeState::MissionActive) {
		const auto now_us = m_services.monotonic_now_us();
		if (!callback_gate_is_open()) {
			return;
		}
		apply_pending_lifecycle();
		if (!callback_gate_is_open() || (m_state != RuntimeState::Ready &&
				m_state != RuntimeState::MissionLoading && m_state != RuntimeState::MissionActive)) {
			return;
		}
		const RuntimeTickContext context{now_us,
			m_mission_generation,
			m_state == RuntimeState::MissionActive && !m_publication_blocked};
		switch (m_services.service_tick(context)) {
		case RuntimeTickStatus::PermanentTransportFailure:
			if (callback_gate_is_open()) {
				teardown_faulted_runtime(RuntimeTerminalReason::TransportUnavailable);
			}
			break;
		case RuntimeTickStatus::PermanentCaptureFailure:
			if (callback_gate_is_open()) {
				teardown_faulted_runtime(RuntimeTerminalReason::CaptureFailure);
			}
			break;
		case RuntimeTickStatus::Complete:
		case RuntimeTickStatus::Unavailable:
			break;
		}
	}
}

void Runtime::on_engine_shutdown() noexcept
{
	if (!callback_gate_is_open()) {
		return;
	}
	if (!callback_is_on_captured_thread()) {
		return;
	}
	CallbackMetricScope callback_metric(m_services, TelemetryCallbackKind::EngineShutdown);
	auto expected_gate = RuntimeCallbackGate::Open;
	if (!m_callback_gate.compare_exchange_strong(expected_gate,
			RuntimeCallbackGate::ShuttingDown,
			std::memory_order_acq_rel,
			std::memory_order_acquire)) {
		return;
	}

	m_state = RuntimeState::ShuttingDown;
	m_publication_blocked = true;
	m_mission_load_pending = false;
	m_mission_purge_pending = false;
	m_game_state_pending = false;
	m_lifecycle_destination = RuntimeLifecycleDestination::None;
	m_active_continuity = false;

	m_services.stop_collection();
	m_services.close_sessions_and_stores();
	m_services.invalidate_mission_state_and_entities();
	m_services.stop_transport();
	m_services.emit_runtime_summary();
	// The process-summary callback metric must be published while the fixed
	// Metrics owner is still provisioned.  The scope remains harmless at exit.
	callback_metric.finish();
	m_services.release_runtime_allocations();
	m_services.release_metrics();
	m_services.release_session_registry();
	m_state = RuntimeState::Stopped;
	m_callback_gate.store(RuntimeCallbackGate::Stopped, std::memory_order_release);
}

void Runtime::on_game_mission_load() noexcept
{
	if (!callback_gate_is_open()) {
		return;
	}
	if (!callback_is_on_captured_thread()) {
		return;
	}
	if (m_state == RuntimeState::Disabled || m_state == RuntimeState::Faulted) {
		return;
	}
	CallbackMetricScope callback_metric(m_services, TelemetryCallbackKind::GameMissionLoad);
	m_mission_load_pending = true;
	m_mission_purge_pending = true;
	// Restart Mission re-enters GAME_PLAY before the replacement mission-load
	// callback arrives.  That queued ActiveRoot transition is authoritative for
	// the state after the load applies; do not overwrite it with the loading
	// fallback.  Other orders (including terminal -> load) still intentionally
	// resolve to MissionLoading until a subsequent GAME_PLAY transition.
	const auto game_play_already_pending = m_game_state_pending &&
		m_pending_game_state == RuntimeGameStateDisposition::ActiveRoot;
	if (!game_play_already_pending) {
		m_lifecycle_destination = RuntimeLifecycleDestination::MissionLoading;
	}
	m_active_continuity = false;
	m_publication_blocked = true;
}

void Runtime::on_game_enter_state(int old_state, int new_state) noexcept
{
	if (!callback_gate_is_open()) {
		return;
	}
	if (!callback_is_on_captured_thread()) {
		return;
	}
	if (m_state == RuntimeState::Disabled || m_state == RuntimeState::Faulted) {
		return;
	}
	CallbackMetricScope callback_metric(m_services, TelemetryCallbackKind::GameEnterState);
	record_game_state_transition(old_state, new_state);
}

void Runtime::on_game_leave_state(int old_state, int new_state) noexcept
{
	if (!callback_gate_is_open()) {
		return;
	}
	if (!callback_is_on_captured_thread()) {
		return;
	}
	if (m_state == RuntimeState::Disabled || m_state == RuntimeState::Faulted) {
		return;
	}
	CallbackMetricScope callback_metric(m_services, TelemetryCallbackKind::GameLeaveState);
	record_game_state_transition(old_state, new_state);
}

} // namespace telemetry::detail
