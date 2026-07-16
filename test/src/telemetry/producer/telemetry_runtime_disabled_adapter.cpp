#include "telemetry/runtime_adapter.h"

#include <cstddef>
#include <cstdint>
#include <thread>

namespace telemetry::detail {
namespace {

class DisabledRuntimeStartupServices final : public RuntimeStartupServices {
  public:
	void reset_observations() noexcept
	{
		m_main_thread_captured = false;
		m_captured_thread = std::thread::id{};
		m_capture_calls = 0U;
		m_main_thread_checks = 0U;
		m_config_loads = 0U;
		m_post_config_calls = 0U;
		m_diagnostic_calls = 0U;
	}

	void capture_main_thread() noexcept override
	{
		++m_capture_calls;
		m_captured_thread = std::this_thread::get_id();
		m_main_thread_captured = true;
	}

	bool is_on_captured_main_thread() noexcept override
	{
		++m_main_thread_checks;
		return m_main_thread_captured && std::this_thread::get_id() == m_captured_thread;
	}

	RuntimeConfigResult load_config() noexcept override
	{
		++m_config_loads;
		return {RuntimeConfigStatus::Absent, 0U};
	}

	IdentityResult load_producer_identity() noexcept override
	{
		++m_post_config_calls;
		return {0U, IdentityError::ProfileReadFailure};
	}

	SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		++m_post_config_calls;
		return {SessionIdCandidateStatus::EntropyFailure, 0U};
	}

	Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t) noexcept override
	{
		++m_post_config_calls;
		Wp03KnownBudgetSubtotal result;
		result.error = StartupBudgetError::InvalidClientCount;
		return result;
	}

	bool allocate_session_registry() noexcept override
	{
		++m_post_config_calls;
		return false;
	}

	SessionIdRegistrationStatus register_session_candidate(std::uint64_t) noexcept override
	{
		++m_post_config_calls;
		return SessionIdRegistrationStatus::StorageUnavailable;
	}

	RuntimeTransportStatus start_transport() noexcept override
	{
		++m_post_config_calls;
		return RuntimeTransportStatus::Unavailable;
	}

	void release_session_registry() noexcept override
	{
		++m_post_config_calls;
	}

	void emit_startup_diagnostic(RuntimeTerminalReason) noexcept override
	{
		++m_diagnostic_calls;
	}

	std::uint64_t capture_calls() const noexcept
	{
		return m_capture_calls;
	}
	std::uint64_t main_thread_checks() const noexcept
	{
		return m_main_thread_checks;
	}
	std::uint64_t config_loads() const noexcept
	{
		return m_config_loads;
	}
	std::uint64_t post_config_calls() const noexcept
	{
		return m_post_config_calls;
	}
	std::uint64_t diagnostic_calls() const noexcept
	{
		return m_diagnostic_calls;
	}

  private:
	std::thread::id m_captured_thread{};
	bool m_main_thread_captured = false;
	std::uint64_t m_capture_calls = 0U;
	std::uint64_t m_main_thread_checks = 0U;
	std::uint64_t m_config_loads = 0U;
	std::uint64_t m_post_config_calls = 0U;
	std::uint64_t m_diagnostic_calls = 0U;
};

DisabledRuntimeStartupServices& disabled_services() noexcept
{
	static DisabledRuntimeStartupServices services;
	return services;
}

std::uint64_t factory_requests = 0U;

} // namespace

RuntimeStartupServices& runtime_startup_services() noexcept
{
	++factory_requests;
	return disabled_services();
}

} // namespace telemetry::detail

namespace telemetry::test_seam {

void reset_runtime_adapter_observations() noexcept
{
	detail::disabled_services().reset_observations();
	detail::factory_requests = 0U;
}

std::uint64_t runtime_adapter_factory_requests() noexcept
{
	return detail::factory_requests;
}

std::uint64_t runtime_adapter_main_thread_captures() noexcept
{
	return detail::disabled_services().capture_calls();
}

std::uint64_t runtime_adapter_main_thread_checks() noexcept
{
	return detail::disabled_services().main_thread_checks();
}

std::uint64_t runtime_adapter_config_loads() noexcept
{
	return detail::disabled_services().config_loads();
}

std::uint64_t runtime_adapter_post_config_calls() noexcept
{
	return detail::disabled_services().post_config_calls();
}

std::uint64_t runtime_adapter_diagnostic_calls() noexcept
{
	return detail::disabled_services().diagnostic_calls();
}

} // namespace telemetry::test_seam
