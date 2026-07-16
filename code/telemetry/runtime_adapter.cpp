#include "telemetry/runtime_adapter.h"

#include "cmdline/cmdline.h"
#include "globalincs/pstypes.h"
#include "telemetry/config.h"

#include <thread>

namespace telemetry::detail {
namespace {

class NativeRuntimeStartupServices final : public RuntimeStartupServices {
  public:
	void capture_main_thread() noexcept override
	{
		m_captured_thread = std::this_thread::get_id();
		m_main_thread_captured = true;
	}

	bool is_on_captured_main_thread() noexcept override
	{
		return m_main_thread_captured && std::this_thread::get_id() == m_captured_thread;
	}

	RuntimeConfigResult load_config() noexcept override
	{
		const auto loaded = load_telemetry_config();
		m_effective_config = loaded.effective;
		switch (loaded.status) {
		case ConfigStatus::Absent:
			return RuntimeConfigResult{RuntimeConfigStatus::Absent, 0U};
		case ConfigStatus::ValidDisabled:
			return RuntimeConfigResult{RuntimeConfigStatus::Disabled, m_effective_config.max_clients};
		case ConfigStatus::ValidEnabled:
			return RuntimeConfigResult{RuntimeConfigStatus::Enabled, m_effective_config.max_clients};
		case ConfigStatus::Invalid:
			return RuntimeConfigResult{RuntimeConfigStatus::Invalid, 0U};
		}
		return RuntimeConfigResult{RuntimeConfigStatus::Invalid, 0U};
	}

	IdentityResult load_producer_identity() noexcept override
	{
		NativeProducerProfileStore store(profile_storage_root_for_mode(Cmdline_portable_mode));
		return load_or_create_producer_identity(store, m_random);
	}

	SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		return draw_session_id_candidate(m_random);
	}

	Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t max_clients) noexcept override
	{
		return calculate_wp04_startup_budget(make_wp03_known_budget_request(max_clients));
	}

	bool allocate_session_registry() noexcept override
	{
		return m_session_registry.allocate_storage();
	}

	SessionIdRegistrationStatus register_session_candidate(std::uint64_t candidate) noexcept override
	{
		return m_session_registry.register_candidate(candidate);
	}

	RuntimeTransportStatus start_transport() noexcept override
	{
		return RuntimeTransportStatus::Unavailable;
	}

	void release_session_registry() noexcept override
	{
		m_session_registry.release_storage();
	}

	void emit_startup_diagnostic(RuntimeTerminalReason reason) noexcept override
	{
		switch (reason) {
		case RuntimeTerminalReason::ConfigAbsent:
			mprintf(("Telemetry startup disabled: configuration absent.\n"));
			break;
		case RuntimeTerminalReason::ConfigInvalid:
			mprintf(("Telemetry startup disabled: invalid configuration.\n"));
			break;
		case RuntimeTerminalReason::ConfigDisabled:
			mprintf(("Telemetry startup disabled by configuration.\n"));
			break;
		case RuntimeTerminalReason::MainThreadNotCaptured:
			mprintf(("Telemetry startup failed: main thread was not captured.\n"));
			break;
		case RuntimeTerminalReason::MainThreadViolation:
			mprintf(("Telemetry startup failed: main-thread invariant.\n"));
			break;
		case RuntimeTerminalReason::ProducerIdentityFailure:
			mprintf(("Telemetry startup failed: producer identity.\n"));
			break;
		case RuntimeTerminalReason::SessionCandidateFailure:
			mprintf(("Telemetry startup failed: session entropy.\n"));
			break;
		case RuntimeTerminalReason::BudgetFailure:
			mprintf(("Telemetry startup failed: memory budget.\n"));
			break;
		case RuntimeTerminalReason::AllocationFailure:
			mprintf(("Telemetry startup failed: startup allocation.\n"));
			break;
		case RuntimeTerminalReason::SessionRegistrationFailure:
			mprintf(("Telemetry startup failed: session registration.\n"));
			break;
		case RuntimeTerminalReason::TransportUnavailable:
			mprintf(("Telemetry startup failed: transport unavailable.\n"));
			break;
		case RuntimeTerminalReason::None:
			mprintf(("Telemetry startup stopped.\n"));
			break;
		}
	}

  private:
	TelemetryConfig m_effective_config;
	OsRandomSource m_random;
	SessionIdRegistry m_session_registry;
	std::thread::id m_captured_thread{};
	bool m_main_thread_captured = false;
};

} // namespace

RuntimeStartupServices& runtime_startup_services() noexcept
{
	static NativeRuntimeStartupServices services;
	return services;
}

} // namespace telemetry::detail
