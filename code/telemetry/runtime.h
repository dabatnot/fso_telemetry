#pragma once

#include "telemetry/identity.h"
#include "telemetry/startup_budget.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace telemetry::detail {

enum class RuntimeState : std::uint8_t {
	Cold = 0,
	Starting,
	Disabled,
	Faulted,
	Ready,
	MissionLoading,
	MissionActive,
	ShuttingDown,
	Stopped,
};

enum class RuntimeTerminalReason : std::uint8_t {
	None = 0,
	ConfigAbsent,
	ConfigInvalid,
	ConfigDisabled,
	MainThreadNotCaptured,
	MainThreadViolation,
	ProducerIdentityFailure,
	SessionCandidateFailure,
	BudgetFailure,
	AllocationFailure,
	SessionRegistrationFailure,
	TransportUnavailable,
	InvalidLifecycleTransition,
	MissionGenerationOverflow,
};

enum class RuntimeGameStateDisposition : std::uint8_t {
	Invalid = 0,
	ActiveRoot,
	PreserveContext,
	LoadingPreserve,
	Terminal,
};

RuntimeGameStateDisposition classify_runtime_game_state(int state) noexcept;

enum class RuntimeCallbackGate : std::uint8_t {
	Open = 0,
	ShuttingDown,
	Stopped,
};

enum class RuntimeLifecycleDestination : std::uint8_t {
	None = 0,
	Ready,
	MissionLoading,
	MissionActive,
};

enum class RuntimeConfigStatus : std::uint8_t {
	Enabled = 0,
	Absent,
	Invalid,
	Disabled,
};

struct RuntimeConfigResult {
	RuntimeConfigStatus status = RuntimeConfigStatus::Absent;
	std::size_t max_clients = 0U;
};

// The native runtime remains fail-closed while the global startup budget is
// incomplete. Started is used by deterministic lifecycle fakes; WP05 does not
// bind the native transport or publish a client session.
enum class RuntimeTransportStatus : std::uint8_t {
	Unavailable = 0,
	Started,
};

class RuntimeStartupServices {
  public:
	virtual ~RuntimeStartupServices() = default;

	virtual void capture_main_thread() noexcept = 0;
	virtual bool is_on_captured_main_thread() noexcept = 0;
	virtual RuntimeConfigResult load_config() noexcept = 0;
	virtual IdentityResult load_producer_identity() noexcept = 0;
	virtual SessionIdCandidateResult draw_session_candidate() noexcept = 0;
	virtual Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t max_clients) noexcept = 0;
	virtual bool allocate_session_registry() noexcept = 0;
	virtual SessionIdRegistrationStatus register_session_candidate(std::uint64_t candidate) noexcept = 0;
	virtual RuntimeTransportStatus start_transport() noexcept = 0;
	virtual void stop_collection() noexcept = 0;
	virtual void invalidate_mission_state_and_entities() noexcept = 0;
	virtual void cancel_replication() noexcept = 0;
	virtual void close_sessions_and_stores() noexcept = 0;
	virtual void reset_mission_scope() noexcept = 0;
	virtual void stop_transport() noexcept = 0;
	virtual void emit_runtime_summary() noexcept = 0;
	virtual void release_runtime_allocations() noexcept = 0;
	virtual void release_session_registry() noexcept = 0;
	virtual void emit_startup_diagnostic(RuntimeTerminalReason reason) noexcept = 0;
};

class RuntimeLifecycleTestAccess;

class Runtime final {
  public:
	explicit Runtime(RuntimeStartupServices& services) noexcept;

	Runtime(const Runtime&) = delete;
	Runtime& operator=(const Runtime&) = delete;
	Runtime(Runtime&&) = delete;
	Runtime& operator=(Runtime&&) = delete;

	void capture_main_thread() noexcept;
	void on_engine_update() noexcept;
	void on_engine_shutdown() noexcept;
	void on_game_mission_load() noexcept;
	void on_game_enter_state(int old_state, int new_state) noexcept;
	void on_game_leave_state(int old_state, int new_state) noexcept;

	RuntimeState state() const noexcept
	{
		return m_state;
	}
	RuntimeTerminalReason terminal_reason() const noexcept
	{
		return m_terminal_reason;
	}
	std::size_t socket_count() const noexcept
	{
		return 0U;
	}
	std::uint64_t published_session_id() const noexcept
	{
		return 0U;
	}
	std::size_t diagnostic_count() const noexcept
	{
		return m_diagnostic_count;
	}
	std::uint32_t mission_generation() const noexcept
	{
		return m_mission_generation;
	}
	bool mission_publication_allowed() const noexcept
	{
		return m_callback_gate.load(std::memory_order_acquire) == RuntimeCallbackGate::Open &&
			m_state == RuntimeState::MissionActive && !m_publication_blocked &&
			!m_wrong_thread_violation.load(std::memory_order_relaxed);
	}

  private:
	friend class RuntimeLifecycleTestAccess;

	bool callback_gate_is_open() const noexcept;
	bool startup_transaction_is_active() const noexcept;
	bool callback_is_on_captured_thread() noexcept;
	void initialize_lifecycle_summary() noexcept;
	void apply_pending_lifecycle() noexcept;
	void record_game_state_transition(int old_state, int new_state) noexcept;
	bool purge_mission() noexcept;
	void teardown_faulted_runtime(RuntimeTerminalReason reason) noexcept;
	void enter_disabled(RuntimeTerminalReason reason, bool diagnostic) noexcept;
	void enter_faulted(RuntimeTerminalReason reason) noexcept;
	void emit_diagnostic_once(RuntimeTerminalReason reason) noexcept;

	RuntimeStartupServices& m_services;
	RuntimeState m_state = RuntimeState::Cold;
	RuntimeTerminalReason m_terminal_reason = RuntimeTerminalReason::None;
	std::size_t m_diagnostic_count = 0U;
	bool m_main_thread_captured = false;
	std::thread::id m_captured_thread{};
	std::atomic<RuntimeCallbackGate> m_callback_gate{RuntimeCallbackGate::Open};
	std::atomic<bool> m_wrong_thread_violation{false};
	std::uint32_t m_mission_generation = 0U;
	RuntimeGameStateDisposition m_pending_game_state = RuntimeGameStateDisposition::Invalid;
	RuntimeLifecycleDestination m_lifecycle_destination = RuntimeLifecycleDestination::None;
	bool m_active_continuity = false;
	bool m_game_state_pending = false;
	bool m_mission_load_pending = false;
	bool m_mission_purge_pending = false;
	bool m_publication_blocked = false;
};

class RuntimeLifecycleTestAccess final {
  public:
	static void set_mission_generation(Runtime& runtime, std::uint32_t generation) noexcept
	{
		runtime.m_mission_generation = generation;
	}
	static bool wrong_thread_violation(const Runtime& runtime) noexcept
	{
		return runtime.m_wrong_thread_violation.load(std::memory_order_relaxed);
	}
};

} // namespace telemetry::detail
