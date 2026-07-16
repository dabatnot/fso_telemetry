#include "telemetry/runtime.h"

namespace telemetry::detail {

Runtime::Runtime(RuntimeStartupServices& services) noexcept : m_services(services) {}

void Runtime::capture_main_thread() noexcept
{
	if (m_state != RuntimeState::Cold || m_main_thread_captured) {
		return;
	}
	m_main_thread_captured = true;
	m_services.capture_main_thread();
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
	if (diagnostic) {
		emit_diagnostic_once(reason);
	}
}

void Runtime::enter_faulted(RuntimeTerminalReason reason) noexcept
{
	m_terminal_reason = reason;
	m_state = RuntimeState::Faulted;
	emit_diagnostic_once(reason);
}

void Runtime::on_engine_update() noexcept
{
	if (m_state != RuntimeState::Cold) {
		return;
	}
	if (!m_main_thread_captured) {
		enter_faulted(RuntimeTerminalReason::MainThreadNotCaptured);
		return;
	}
	if (!m_services.is_on_captured_main_thread()) {
		enter_faulted(RuntimeTerminalReason::MainThreadViolation);
		return;
	}

	m_state = RuntimeState::Starting;
	const auto config = m_services.load_config();
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
	if (identity.error != IdentityError::None || identity.producer_id == 0U) {
		enter_faulted(RuntimeTerminalReason::ProducerIdentityFailure);
		return;
	}

	const auto candidate = m_services.draw_session_candidate();
	if (candidate.status != SessionIdCandidateStatus::Ready || candidate.session_id == 0U) {
		enter_faulted(RuntimeTerminalReason::SessionCandidateFailure);
		return;
	}

	const auto budget = m_services.calculate_known_budget(config.max_clients);
	if (budget.error != StartupBudgetError::None || !budget.is_complete || budget.deferred_categories != 0U) {
		enter_faulted(RuntimeTerminalReason::BudgetFailure);
		return;
	}

	if (!m_services.allocate_session_registry()) {
		m_services.release_session_registry();
		enter_faulted(RuntimeTerminalReason::AllocationFailure);
		return;
	}

	if (m_services.register_session_candidate(candidate.session_id) != SessionIdRegistrationStatus::Registered) {
		m_services.release_session_registry();
		enter_faulted(RuntimeTerminalReason::SessionRegistrationFailure);
		return;
	}

	static_cast<void>(m_services.start_transport());
	m_services.release_session_registry();
	enter_faulted(RuntimeTerminalReason::TransportUnavailable);
}

} // namespace telemetry::detail
