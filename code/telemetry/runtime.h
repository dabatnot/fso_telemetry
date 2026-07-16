#pragma once

#include "telemetry/identity.h"
#include "telemetry/startup_budget.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

enum class RuntimeState : std::uint8_t {
	Cold = 0,
	Starting,
	Disabled,
	Faulted,
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

// WP04 owns transport construction. Until it exists, this closed result keeps
// the WP03 startup sequence fail-closed after all earlier gates have passed.
enum class RuntimeTransportStatus : std::uint8_t {
	Unavailable = 0,
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
	virtual void release_session_registry() noexcept = 0;
	virtual void emit_startup_diagnostic(RuntimeTerminalReason reason) noexcept = 0;
};

class Runtime final {
  public:
	explicit Runtime(RuntimeStartupServices& services) noexcept;

	Runtime(const Runtime&) = delete;
	Runtime& operator=(const Runtime&) = delete;
	Runtime(Runtime&&) = delete;
	Runtime& operator=(Runtime&&) = delete;

	void capture_main_thread() noexcept;
	void on_engine_update() noexcept;

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

  private:
	void enter_disabled(RuntimeTerminalReason reason, bool diagnostic) noexcept;
	void enter_faulted(RuntimeTerminalReason reason) noexcept;
	void emit_diagnostic_once(RuntimeTerminalReason reason) noexcept;

	RuntimeStartupServices& m_services;
	RuntimeState m_state = RuntimeState::Cold;
	RuntimeTerminalReason m_terminal_reason = RuntimeTerminalReason::None;
	std::size_t m_diagnostic_count = 0U;
	bool m_main_thread_captured = false;
};

} // namespace telemetry::detail
