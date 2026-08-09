#include "telemetry/runtime_adapter.h"
#include "telemetry/runtime_adapter_test_seam.h"

#include "cmdline/cmdline.h"
#include "globalincs/pstypes.h"
#include "globalincs/systemvars.h"
#include "graphics/2d.h"
#include "io/timer.h"
#include "network/multi.h"
#include "telemetry/config.h"
#include "telemetry/engine_adapter.h"
#include "telemetry/logging.h"
#include "telemetry/native_session_runtime.h"

#include <memory>
#include <new>
#include <thread>

namespace telemetry::detail {
namespace {

Phase2ProfileEligibility current_phase2_profile_eligibility(
	const TelemetryConfig& config) noexcept
{
	Phase2ProfileEligibility eligibility;
	if ((Game_mode & GM_MULTIPLAYER) == 0) {
		eligibility.authority_mode = protocol::AuthorityMode::Solo;
	} else if (Net_player != nullptr &&
		(Net_player->flags & NETINFO_FLAG_AM_MASTER) != 0) {
		eligibility.authority_mode = protocol::AuthorityMode::MultiplayerMaster;
	} else {
		eligibility.authority_mode = protocol::AuthorityMode::MultiplayerClient;
	}
	eligibility.visibility_mode = protocol::VisibilityMode::Cockpit;
	eligibility.trusted_full_state = config.trusted_full_state;
	eligibility.dedicated =
		Is_standalone || (Game_mode & GM_STANDALONE_SERVER) != 0;
	eligibility.headless = gr_screen.mode == GraphicsAPI::Stub;
	return eligibility;
}

RuntimeTickStatus map_native_tick_status(NativeSessionTickStatus status) noexcept
{
	switch (status) {
	case NativeSessionTickStatus::Complete:
		return RuntimeTickStatus::Complete;
	case NativeSessionTickStatus::Unavailable:
		return RuntimeTickStatus::Unavailable;
	case NativeSessionTickStatus::PermanentTransportFailure:
		return RuntimeTickStatus::PermanentTransportFailure;
	case NativeSessionTickStatus::PermanentCaptureFailure:
	default:
		return RuntimeTickStatus::PermanentCaptureFailure;
	}
}

const char* map_runtime_diagnostic(RuntimeTerminalReason reason) noexcept
{
	switch (reason) {
	case RuntimeTerminalReason::None:
		return "Telemetry startup stopped.\n";
	case RuntimeTerminalReason::ConfigAbsent:
		return "Telemetry startup disabled: configuration absent.\n";
	case RuntimeTerminalReason::ConfigInvalid:
		return "Telemetry startup disabled: invalid configuration.\n";
	case RuntimeTerminalReason::ConfigDisabled:
		return "Telemetry startup disabled by configuration.\n";
	case RuntimeTerminalReason::MainThreadNotCaptured:
		return "Telemetry startup failed: main thread was not captured.\n";
	case RuntimeTerminalReason::MainThreadViolation:
		return "Telemetry startup failed: main-thread invariant.\n";
	case RuntimeTerminalReason::ProducerIdentityFailure:
		return "Telemetry startup failed: producer identity.\n";
	case RuntimeTerminalReason::SessionCandidateFailure:
		return "Telemetry startup failed: session entropy.\n";
	case RuntimeTerminalReason::BudgetFailure:
		return "Telemetry startup failed: memory budget.\n";
	case RuntimeTerminalReason::AllocationFailure:
		return "Telemetry startup failed: startup allocation.\n";
	case RuntimeTerminalReason::SessionRegistrationFailure:
		return "Telemetry startup failed: session registration.\n";
	case RuntimeTerminalReason::TransportUnavailable:
		return "Telemetry startup failed: transport unavailable.\n";
	case RuntimeTerminalReason::InvalidLifecycleTransition:
		return "Telemetry runtime failed: invalid lifecycle transition.\n";
	case RuntimeTerminalReason::MissionGenerationOverflow:
		return "Telemetry runtime failed: mission generation overflow.\n";
	case RuntimeTerminalReason::CaptureFailure:
		return "Telemetry runtime failed: player capture.\n";
	default:
		return nullptr;
	}
}

TelemetryRuntimeFaultReason map_runtime_fault_metric(RuntimeTerminalReason reason) noexcept
{
	switch (reason) {
	case RuntimeTerminalReason::ConfigAbsent:
	case RuntimeTerminalReason::ConfigInvalid:
	case RuntimeTerminalReason::ConfigDisabled:
		return TelemetryRuntimeFaultReason::Config;
	case RuntimeTerminalReason::SessionCandidateFailure:
		return TelemetryRuntimeFaultReason::Entropy;
	case RuntimeTerminalReason::ProducerIdentityFailure:
		return TelemetryRuntimeFaultReason::IdentityStore;
	case RuntimeTerminalReason::BudgetFailure:
		return TelemetryRuntimeFaultReason::BudgetOverflow;
	case RuntimeTerminalReason::AllocationFailure:
		return TelemetryRuntimeFaultReason::Allocation;
	case RuntimeTerminalReason::TransportUnavailable:
		return TelemetryRuntimeFaultReason::Socket;
	case RuntimeTerminalReason::None:
	case RuntimeTerminalReason::MainThreadNotCaptured:
	case RuntimeTerminalReason::MainThreadViolation:
	case RuntimeTerminalReason::SessionRegistrationFailure:
	case RuntimeTerminalReason::InvalidLifecycleTransition:
	case RuntimeTerminalReason::MissionGenerationOverflow:
	case RuntimeTerminalReason::CaptureFailure:
	default:
		return TelemetryRuntimeFaultReason::Invariant;
	}
}

TelemetryLogFault map_runtime_fault_log_for_diagnostics(RuntimeTerminalReason reason) noexcept
{
	switch (reason) {
	case RuntimeTerminalReason::ConfigAbsent: case RuntimeTerminalReason::ConfigInvalid: case RuntimeTerminalReason::ConfigDisabled: return TelemetryLogFault::Config;
	case RuntimeTerminalReason::SessionCandidateFailure: return TelemetryLogFault::Entropy;
	case RuntimeTerminalReason::ProducerIdentityFailure: return TelemetryLogFault::IdentityStore;
	case RuntimeTerminalReason::BudgetFailure: return TelemetryLogFault::BudgetOverflow;
	case RuntimeTerminalReason::AllocationFailure: return TelemetryLogFault::Allocation;
	case RuntimeTerminalReason::SessionRegistrationFailure: return TelemetryLogFault::SessionRegistration;
	case RuntimeTerminalReason::TransportUnavailable: return TelemetryLogFault::Socket;
	case RuntimeTerminalReason::MainThreadNotCaptured: case RuntimeTerminalReason::MainThreadViolation: return TelemetryLogFault::MainThread;
	case RuntimeTerminalReason::InvalidLifecycleTransition: case RuntimeTerminalReason::MissionGenerationOverflow: return TelemetryLogFault::Lifecycle;
	case RuntimeTerminalReason::CaptureFailure: return TelemetryLogFault::Capture;
	case RuntimeTerminalReason::None: default: return TelemetryLogFault::None;
	}
}

void record_adapter_activation(TelemetryStructuredLog& log, std::uint16_t port,
	std::uint64_t effective_limit) noexcept
{
	log.activated(TelemetryLogFamily::DualStack, port, effective_limit);
}

void record_adapter_fault(TelemetryStructuredLog& log, RuntimeTerminalReason reason) noexcept
{
	log.transport_fault(TelemetryLogFamily::DualStack,
		reason == RuntimeTerminalReason::TransportUnavailable ? TelemetryLogReason::Bind : TelemetryLogReason::ProtocolError,
		0U, map_runtime_fault_log_for_diagnostics(reason));
}

TelemetryStructuredLog& adapter_test_diagnostics_log() noexcept
{
	static TelemetryStructuredLog log;
	return log;
}

TelemetryLogReason map_config_log_reason(ConfigError error) noexcept
{
	switch (error) {
	case ConfigError::OutOfRange:
	case ConfigError::DuplicateAddress:
	case ConfigError::DuplicateCidr:
		return TelemetryLogReason::ConfigRange;
	case ConfigError::InvalidAddress:
	case ConfigError::InvalidCidr:
	case ConfigError::UnsafeExposure:
		return TelemetryLogReason::ConfigSecurity;
	case ConfigError::ReadFailure:
	case ConfigError::FileTooLarge:
		return TelemetryLogReason::ConfigProfile;
	case ConfigError::None:
	case ConfigError::InvalidJson:
	case ConfigError::MaximumDepthExceeded:
	case ConfigError::RootNotObject:
	case ConfigError::UnknownKey:
	case ConfigError::MissingSchemaVersion:
	case ConfigError::MissingPhase2Profile:
	case ConfigError::Phase2ProfileNotAllowed:
	case ConfigError::InvalidPhase2Profile:
	case ConfigError::MissingProfile:
	case ConfigError::InvalidProfile:
	case ConfigError::InvalidType:
	default:
		return TelemetryLogReason::ConfigSchema;
	}
}

class NativeRuntimeStartupServices final : public RuntimeStartupServices {
  public:
	NativeRuntimeStartupServices() noexcept : m_session_ids(m_random, m_session_registry) {}

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
		m_config_invalid_reason = map_config_log_reason(loaded.error);
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
		const auto result = load_or_create_producer_identity(store, m_random);
		m_producer_id = result.error == IdentityError::None ? result.producer_id : 0U;
		return result;
	}

	SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		return draw_session_id_candidate(m_random);
	}

	Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t max_clients) noexcept override
	{
		// The fixed Metrics object has no heap backing, but its provision step is
		// still an explicit pre-bind transaction.  Only a successful provision
		// permits the final budget deferral to clear.
		if (!m_metrics.provision()) {
			return {};
		}
		return calculate_wp09_startup_budget(calculate_wp06_startup_budget(
			calculate_wp04_startup_budget(make_wp03_known_budget_request(max_clients)), max_clients), m_metrics);
	}

	bool provision_metrics() noexcept override { return m_metrics.provision(); }
	void release_metrics() noexcept override { m_metrics.release(); }
	void record_callback_metric(TelemetryCallbackKind kind, std::uint64_t duration_us) noexcept override
	{
		m_metrics.record_callback(kind, duration_us);
	}
	void record_runtime_fault_metric(RuntimeTerminalReason reason) noexcept override
	{
		m_metrics.record_runtime_fault(map_runtime_fault_metric(reason));
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
		// Runtime calls this only after the complete budget and metrics
		// provisioning gate, so no socket can be bound on an incomplete budget.
		auto native = std::unique_ptr<NativeSessionRuntime>(
			new (std::nothrow) NativeSessionRuntime(m_backend, m_output_completion));
		if (native == nullptr) {
			return RuntimeTransportStatus::Unavailable;
		}
		const NativeSessionStartRequest request{&m_effective_config,
			m_producer_id,
			&m_session_ids,
			&m_random,
			&m_metrics,
			&m_log,
			current_phase2_profile_eligibility(m_effective_config),
			m_effective_config.phase2_profile};
		if (native->start(request) != NativeSessionStartStatus::Started) {
			return RuntimeTransportStatus::Unavailable;
		}
		m_native = std::move(native);
		return RuntimeTransportStatus::Started;
	}

	std::uint64_t monotonic_now_us() noexcept override { return timer_get_microseconds(); }
	RuntimeTickStatus service_tick(const RuntimeTickContext& context) noexcept override
	{
		const auto status = RuntimeAdapterPlayerTestAccess::service_tick(m_native.get(), context);
		flush_log_records();
		return status;
	}

	void stop_collection() noexcept override { RuntimeAdapterPlayerTestAccess::stop_collection(m_native.get()); }
	void invalidate_mission_state_and_entities() noexcept override
	{
		RuntimeAdapterPlayerTestAccess::invalidate_mission_state_and_entities(m_native.get());
	}
	void cancel_replication() noexcept override {}
	void close_sessions_and_stores() noexcept override
	{
		RuntimeAdapterPlayerTestAccess::close_sessions_and_stores(m_native.get());
	}
	void reset_mission_scope() noexcept override { m_metrics.reset_mission(); }
	void stop_transport() noexcept override
	{
		RuntimeAdapterPlayerTestAccess::stop_transport(m_native.get());
	}
	void emit_runtime_summary() noexcept override
	{
		const auto now_us = timer_get_microseconds();
		const auto duration_us = now_us >= m_activation_started_at_us ? now_us - m_activation_started_at_us : 0U;
		m_log.shutdown(duration_us, process_session_total());
		flush_log_records();
	}
	void emit_runtime_disabled(RuntimeTerminalReason reason) noexcept override
	{
		switch (reason) {
		case RuntimeTerminalReason::ConfigAbsent:
			m_log.config_absent();
			break;
		case RuntimeTerminalReason::ConfigDisabled:
			m_log.config_disabled();
			break;
		case RuntimeTerminalReason::ConfigInvalid:
			m_log.config_invalid(m_config_invalid_reason);
			break;
		default:
			break;
		}
		flush_log_records();
	}
	void emit_runtime_activated() noexcept override
	{
		record_activation(timer_get_microseconds());
	}
	void emit_runtime_fault(RuntimeTerminalReason reason) noexcept override
	{
		record_fault(reason);
	}
	void emit_mission_entered(std::uint32_t mission_generation) noexcept override
	{
		m_mission_started_at_us = timer_get_microseconds();
		m_log.mission_entered(mission_generation);
		flush_log_records();
	}
	void emit_mission_left(std::uint32_t mission_generation) noexcept override
	{
		const auto now_us = timer_get_microseconds();
		const auto duration_us = now_us >= m_mission_started_at_us ? now_us - m_mission_started_at_us : 0U;
		m_log.mission_left(mission_generation, duration_us, process_session_total());
		flush_log_records();
	}
	void release_runtime_allocations() noexcept override { m_native.reset(); }

	void release_session_registry() noexcept override
	{
		m_session_registry.release_storage();
	}

	void emit_startup_diagnostic(RuntimeTerminalReason reason) noexcept override
	{
		// These transitions already have their single P9.2 structured record.
		if (reason == RuntimeTerminalReason::ConfigInvalid ||
			reason == RuntimeTerminalReason::TransportUnavailable) {
			return;
		}
		if (const auto* message = RuntimeAdapterPlayerTestAccess::startup_diagnostic_message(reason);
			message != nullptr) {
			mprintf(("%s", message));
		}
	}

	void reset_diagnostics_for_test() noexcept
	{
		m_log = {};
		m_emitted_log_records = 0U;
		m_activation_started_at_us = 0U;
		m_mission_started_at_us = 0U;
		m_config_invalid_reason = TelemetryLogReason::ConfigSchema;
		m_suppress_log_delivery_for_test = true;
	}
	void set_log_delivery_suppressed_for_test(bool suppressed) noexcept
	{
		m_suppress_log_delivery_for_test = suppressed;
	}
	void emit_activation_for_test(std::uint64_t now_us) noexcept { record_activation(now_us); }
	void emit_fault_for_test(RuntimeTerminalReason reason) noexcept { record_fault(reason); }
	TelemetryLogFault runtime_fault_log_for_test(RuntimeTerminalReason reason) const noexcept
	{
		return map_runtime_fault_log(reason);
	}
	TelemetryLogSnapshot diagnostics_snapshot_for_test() const noexcept { return m_log.snapshot(); }

  private:
	void record_activation(std::uint64_t now_us) noexcept
	{
		m_activation_started_at_us = now_us;
		record_adapter_activation(m_log, m_effective_config.bind_port, m_effective_config.max_clients);
		flush_log_records();
	}
	void record_fault(RuntimeTerminalReason reason) noexcept
	{
		record_adapter_fault(m_log, reason);
		flush_log_records();
	}
	void flush_log_records() noexcept
	{
		if (m_suppress_log_delivery_for_test) return;
		const auto snapshot = m_log.snapshot();
		while (m_emitted_log_records < snapshot.count) {
			const auto& record = snapshot.records[m_emitted_log_records++];
			// The delivery format is intentionally numeric and closed. No field is
			// sourced from a packet, endpoint, engine identity or OS error string.
			std::array<char, TelemetryLogLineCapacity> line{};
			if (format_telemetry_log_record(record, line)) {
				mprintf(("%s", line.data()));
			} else {
				mprintf(("telemetry log_format_error event=%u\n",
					static_cast<unsigned>(record.event)));
			}
		}
	}
	std::uint64_t process_session_total() const noexcept
	{
		return m_metrics.process_counter(TelemetryMetricCounter::SessionsEnded);
	}
	static TelemetryLogFault map_runtime_fault_log(RuntimeTerminalReason reason) noexcept
	{
		return map_runtime_fault_log_for_diagnostics(reason);
	}

	TelemetryConfig m_effective_config;
	NativeUdpSocketBackend m_backend;
	NativeOutputCompletionForwarder m_output_completion;
	OsRandomSource m_random;
	SessionIdRegistry m_session_registry;
	SessionIdAllocator m_session_ids;
	TelemetryMetrics m_metrics;
	TelemetryStructuredLog m_log;
	TelemetryLogReason m_config_invalid_reason = TelemetryLogReason::ConfigSchema;
	std::unique_ptr<NativeSessionRuntime> m_native;
	std::thread::id m_captured_thread{};
	std::uint64_t m_producer_id = 0U;
	bool m_main_thread_captured = false;
	std::uint64_t m_activation_started_at_us = 0U;
	std::uint64_t m_mission_started_at_us = 0U;
	std::size_t m_emitted_log_records = 0U;
	bool m_suppress_log_delivery_for_test = false;
};

NativeRuntimeStartupServices& native_runtime_startup_services() noexcept
{
	static NativeRuntimeStartupServices services;
	return services;
}

} // namespace

RuntimeTickStatus RuntimeAdapterPlayerTestAccess::service_tick(NativeSessionRuntime* runtime,
	const RuntimeTickContext& context) noexcept
{
	if (runtime == nullptr) {
		return RuntimeTickStatus::Unavailable;
	}
	const auto view = make_fso_engine_read_view();
	// Phase2ObservationBuffer builds its bounded CoreGate selection from the
	// authoritative player_root; no global mission population enters that selection.
	const auto* phase2_view = static_cast<const Phase2EngineReadView*>(&view);
	if (!phase2_view->current_thread_is_main()) {
		return RuntimeTickStatus::PermanentCaptureFailure;
	}
	return map_native_tick_status(
		runtime->service_tick(
			{context.now_us, context.mission_generation, context.mission_active},
			view,
			phase2_view));
}

RuntimeTickStatus RuntimeAdapterPlayerTestAccess::map_tick_status(NativeSessionTickStatus status) noexcept
{
	return map_native_tick_status(status);
}

const char* RuntimeAdapterPlayerTestAccess::startup_diagnostic_message(RuntimeTerminalReason reason) noexcept
{
	return map_runtime_diagnostic(reason);
}

TelemetryLogReason RuntimeAdapterPlayerTestAccess::config_log_reason(ConfigError error) noexcept
{
	return map_config_log_reason(error);
}

void RuntimeAdapterDiagnosticsTestAccess::reset_for_test() noexcept
{
	adapter_test_diagnostics_log() = {};
}

void RuntimeAdapterDiagnosticsTestAccess::set_delivery_suppressed_for_test(bool suppressed) noexcept
{
	// The seam log never owns a delivery sink. Retain this no-op control so a
	// test can state the intended condition without touching engine globals.
	(void)suppressed;
}

void RuntimeAdapterDiagnosticsTestAccess::emit_activation_for_test(std::uint64_t now_us) noexcept
{
	(void)now_us;
	record_adapter_activation(adapter_test_diagnostics_log(), protocol::DefaultTelemetryPort, 1U);
}

void RuntimeAdapterDiagnosticsTestAccess::emit_fault_for_test(RuntimeTerminalReason reason) noexcept
{
	record_adapter_fault(adapter_test_diagnostics_log(), reason);
}

TelemetryLogFault RuntimeAdapterDiagnosticsTestAccess::runtime_fault_log(RuntimeTerminalReason reason) noexcept
{
	return map_runtime_fault_log_for_diagnostics(reason);
}

TelemetryLogSnapshot RuntimeAdapterDiagnosticsTestAccess::log_snapshot() noexcept
{
	return adapter_test_diagnostics_log().snapshot();
}

void RuntimeAdapterPlayerTestAccess::stop_collection(NativeSessionRuntime* runtime) noexcept
{
	if (runtime != nullptr) {
		runtime->stop_collection();
	}
}

void RuntimeAdapterPlayerTestAccess::invalidate_mission_state_and_entities(NativeSessionRuntime* runtime) noexcept
{
	if (runtime != nullptr) {
		runtime->purge_all(SessionCloseReason::MissionDiscontinuity);
	}
}

void RuntimeAdapterPlayerTestAccess::close_sessions_and_stores(NativeSessionRuntime* runtime) noexcept
{
	if (runtime != nullptr) {
		runtime->purge_all(SessionCloseReason::Shutdown);
	}
}

void RuntimeAdapterPlayerTestAccess::stop_transport(NativeSessionRuntime* runtime) noexcept
{
	if (runtime != nullptr) {
		runtime->shutdown();
	}
}

RuntimeStartupServices& runtime_startup_services() noexcept
{
	return native_runtime_startup_services();
}

} // namespace telemetry::detail
