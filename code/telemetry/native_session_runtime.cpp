#include "telemetry/native_session_runtime.h"

#include "telemetry/phase1_state_image.h"
#include "telemetry/native_session_runtime_test_seam.h"
#include "telemetry/logging.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/startup_budget.h"

#include <chrono>
#include <limits>
#include <utility>

namespace telemetry::detail {
namespace {

std::uint64_t elapsed_nanoseconds(std::chrono::steady_clock::time_point started,
	std::chrono::steady_clock::time_point ended) noexcept
{
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count();
	return elapsed <= 0 ? 0U : static_cast<std::uint64_t>(elapsed);
}

bool make_controller_config(const TelemetryConfig& source,
	std::uint64_t producer_id,
	SessionControllerConfig& output) noexcept
{
	if (!source.enabled || source.max_clients < 1U || source.max_clients > 4U ||
		source.max_datagrams_per_tick < 1U || source.max_datagrams_per_tick > 256U ||
		producer_id == 0U) {
		return false;
	}

	output.max_clients = source.max_clients;
	output.producer_id = producer_id;
	output.mission_heartbeat_ms = source.mission_heartbeat_ms;
	output.idle_heartbeat_ms = source.idle_heartbeat_ms;
	output.keyframe_seconds = source.keyframe_seconds;
	output.security.enabled = true;
	output.security.port = source.bind_port;
	output.security.discovery_enabled = source.discovery_enabled;
	output.security.source_allowlist = source.allowed_clients;
	output.security.resources.max_clients = source.max_clients;
	output.security.resources.global_state_reassembly_bytes =
		static_cast<std::size_t>(source.max_clients) * protocol::MaxStateReassemblyBytesPerClient;
	output.security.resources.global_video_reassembly_bytes =
		static_cast<std::size_t>(source.max_clients) * protocol::MaxVideoReassemblyBytesPerClient;
	for (std::size_t index = 0U; index < source.bind_addresses.size(); ++index) {
		if (!source.bind_addresses[index].is_loopback()) {
			output.security.bind_mode = protocol::NetworkBindMode::LoopbackAndAllowlisted;
			break;
		}
	}
	return true;
}

TelemetryLogReason log_reason(SessionCloseReason reason) noexcept
{
	switch (reason) {
	case SessionCloseReason::Timeout:
		return TelemetryLogReason::Timeout;
	case SessionCloseReason::MissionDiscontinuity:
		return TelemetryLogReason::MissionDiscontinuity;
	case SessionCloseReason::ProtocolError:
		return TelemetryLogReason::ProtocolError;
	case SessionCloseReason::TransportError:
		return TelemetryLogReason::TransportError;
	case SessionCloseReason::Shutdown:
	default:
		return TelemetryLogReason::Shutdown;
	}
}

TelemetryLogDrop log_drop_reason(SessionIngressDropReason reason) noexcept
{
	switch (reason) {
	case SessionIngressDropReason::SourceNotAllowed:
		return TelemetryLogDrop::Allowlist;
	case SessionIngressDropReason::HelloRateLimited:
	case SessionIngressDropReason::SessionCreationRateLimited:
		return TelemetryLogDrop::RateLimit;
	case SessionIngressDropReason::AntiAmplificationLimit:
		return TelemetryLogDrop::AntiAmplification;
	case SessionIngressDropReason::None:
	case SessionIngressDropReason::DatagramEnvelopeInvalid:
	case SessionIngressDropReason::EndpointSessionMismatch:
	case SessionIngressDropReason::HandshakeCacheFull:
	case SessionIngressDropReason::PayloadInvalid:
	case SessionIngressDropReason::SessionIdUnavailable:
	case SessionIngressDropReason::NoClientSlot:
	default:
		return TelemetryLogDrop::Validation;
	}
}

} // namespace

void NativeOutputCompletionForwarder::complete(SessionController& controller, IoStatus status) noexcept
{
	controller.complete_output(status);
}

NativeSessionRuntime::NativeSessionRuntime(UdpSocketBackend& backend,
	NativeOutputCompletionPort& output_completion) noexcept
	: m_transport(backend), m_output_completion(output_completion)
{
}

NativeSessionRuntime::~NativeSessionRuntime() noexcept
{
	shutdown();
}

NativeSessionStartStatus NativeSessionRuntime::start(const NativeSessionStartRequest& request) noexcept
{
	if (m_state == State::Started) {
		return NativeSessionStartStatus::AlreadyStarted;
	}
	if (m_state != State::Cold || request.config == nullptr || request.session_ids == nullptr ||
		request.packet_sequences == nullptr) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	Capture30Hz capture_cadence;
	if (!capture_cadence.configure(request.config->flight_hz)) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}

	SessionControllerConfig controller_config;
	if (!make_controller_config(*request.config, request.producer_id, controller_config)) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	if (m_fail_session_controller_provision) {
		// No controller has been published and no transport operation has begun.
		// Keep State::Cold so clearing the test-only failpoint permits a retry.
		return NativeSessionStartStatus::AllocationFailure;
	}
	SessionController controller;
	const auto configured = SessionController::configure(controller_config,
		*request.session_ids,
		*request.packet_sequences,
		0U,
		nullptr,
		controller);
	if (configured == SessionControllerConfigureResult::InvalidConfiguration) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	if (configured == SessionControllerConfigureResult::AllocationFailure) {
		return NativeSessionStartStatus::AllocationFailure;
	}
	// Construct every mutable image backing before bind/Ready. A failure is
	// retryable because no transport operation has started yet.
	if (!provision_state_image_pools(request.config->max_clients)) {
		return NativeSessionStartStatus::AllocationFailure;
	}

	const auto opened = m_transport.open(*request.config);
	if (opened == TransportOpenStatus::InvalidConfiguration || opened == TransportOpenStatus::Disabled) {
		release_state_image_pools();
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	if (opened != TransportOpenStatus::Complete) {
		release_state_image_pools();
		return NativeSessionStartStatus::TransportUnavailable;
	}

	m_controller = std::move(controller);
	m_metrics = request.metrics;
	m_log = request.log;
	if (m_metrics != nullptr) {
		m_metrics->set_udp_sockets_open(m_transport.socket_count());
		m_metrics->set_runtime_state(4U); // RuntimeState::Ready, kept local to avoid a dependency cycle.
	}
	m_controller_ready = true;
	m_maximum_attempts = request.config->max_datagrams_per_tick;
	m_producer_id = request.producer_id;
	m_scheduler.reset();
	m_capture_cadence = capture_cadence;
	clear_player_capture();
	m_fault_status = NativeSessionTickStatus::Unavailable;
	m_state = State::Started;
	if (m_log != nullptr) {
		// State-image backings are fully provisioned before Ready; this is the
		// first real high-water observation, not a configured estimate.
		m_log->budget_high_water(TelemetryLogBudget::StateImage, m_state_image_pool_backing_bytes,
			m_state_image_pool_backing_bytes);
	}
	return NativeSessionStartStatus::Started;
}

NativeSessionTickStatus NativeSessionRuntime::service_tick(const NativeSessionTickContext& context,
	const EngineReadView& engine_view) noexcept
{
	const auto measure_performance = m_performance_observation_active;
	const auto tick_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	if (measure_performance) m_last_performance_sample = {};
	const auto r2_status = service_r2_tick(context);
	if (r2_status != NativeSessionTickStatus::Complete) {
		return r2_status;
	}

	const auto cadence = m_capture_cadence.poll(context.now_us, context.mission_active);
	switch (cadence.status) {
	case CaptureCadenceStatus::Inactive:
		m_controller.clear_player_observations();
		clear_player_capture();
		m_last_player_capture_status = NativePlayerCaptureStatus::Inactive;
		if (measure_performance) {
			m_last_performance_sample.tick_duration_ns = elapsed_nanoseconds(tick_started, std::chrono::steady_clock::now());
			refresh_performance_resource_sample();
		}
		return NativeSessionTickStatus::Complete;
	case CaptureCadenceStatus::NotDue:
		if (!m_capture_after_ready_transition) {
			m_last_player_materialization = {};
			m_last_player_capture_status = NativePlayerCaptureStatus::NotDue;
			if (measure_performance) {
				m_last_performance_sample.tick_duration_ns = elapsed_nanoseconds(tick_started, std::chrono::steady_clock::now());
				refresh_performance_resource_sample();
			}
			return NativeSessionTickStatus::Complete;
		}
		break;
	case CaptureCadenceStatus::Due:
		break;
	case CaptureCadenceStatus::InvalidRate:
	case CaptureCadenceStatus::ClockRegression:
	case CaptureCadenceStatus::DeadlineOverflow:
	case CaptureCadenceStatus::Count:
		fail_capture(NativePlayerCaptureStatus::CadenceFailure);
		return NativeSessionTickStatus::PermanentCaptureFailure;
	}

	const auto capture_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	PlayerObservationDto observation;
	const auto capture = collect_player_kinematics(engine_view, context.now_us, observation);
	if (measure_performance) {
		m_last_performance_sample.collect_duration_ns = elapsed_nanoseconds(capture_started, std::chrono::steady_clock::now());
	}
	const auto diff_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	m_applying_engine_capture = true;
	const auto status = apply_collected_player_capture(capture, observation);
	m_applying_engine_capture = false;
	const auto capture_ended = std::chrono::steady_clock::now();
	const auto duration_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		capture_ended - capture_started).count());
	record_capture_metric(m_last_player_capture_status, duration_us);
	if (measure_performance) {
		m_last_performance_sample.diff_duration_ns = elapsed_nanoseconds(diff_started, capture_ended);
		m_last_performance_sample.tick_duration_ns = elapsed_nanoseconds(tick_started, capture_ended);
		refresh_performance_resource_sample();
	}
	return status;
}

void NativeSessionRuntime::stop_collection() noexcept
{
	if (m_controller_ready) {
		m_controller.clear_player_observations();
	}
	m_capture_cadence.stop();
	clear_player_capture();
	m_last_player_capture_status = NativePlayerCaptureStatus::Inactive;
}

void NativeSessionRuntime::purge_all(SessionCloseReason reason) noexcept
{
	if (m_controller_ready) {
		m_controller.purge_all(reason);
	}
	const auto metric_reason = reason == SessionCloseReason::Timeout ? TelemetrySessionEndReason::Timeout :
			reason == SessionCloseReason::MissionDiscontinuity ? TelemetrySessionEndReason::MissionDiscontinuity :
			reason == SessionCloseReason::TransportError ? TelemetrySessionEndReason::TransportError :
			reason == SessionCloseReason::Shutdown ? TelemetrySessionEndReason::Shutdown : TelemetrySessionEndReason::ProtocolError;
	for (std::size_t index = 0U; index < m_metrics_session_active.size(); ++index) {
		if (m_metrics != nullptr && m_metrics_session_active[index]) {
			m_metrics->increment_session(index, TelemetryMetricCounter::SessionsEnded);
			m_metrics->record_session_end(metric_reason);
			m_metrics->deactivate_session(index);
		}
		if (m_log != nullptr) {
			const auto started_at = m_session_started_at_us[index];
			const auto duration = m_tick_context.now_us >= started_at ? m_tick_context.now_us - started_at : 0U;
			const auto total = m_metrics != nullptr ? m_metrics->snapshot().process_counters[
				static_cast<std::size_t>(TelemetryMetricCounter::SessionsEnded)] : 0U;
			m_log->session_closed(index, log_reason(reason), duration, total);
		}
	}
	m_metrics_session_active = {};
	m_session_started_at_us = {};
	m_capture_cadence.stop();
	clear_player_capture();
}

void NativeSessionRuntime::shutdown() noexcept
{
	if (m_state == State::Stopped) {
		return;
	}
	purge_all(SessionCloseReason::Shutdown);
	m_transport.close();
	if (m_metrics != nullptr) {
		m_metrics->set_udp_sockets_open(0U);
	}
	m_scheduler.reset();
	m_capture_cadence.reset();
	clear_player_capture();
	release_state_image_pools();
	m_state = State::Stopped;
}

std::size_t NativeSessionRuntime::socket_count() const noexcept
{
	return m_transport.socket_count();
}

std::size_t NativeSessionRuntime::active_sessions() const noexcept
{
	return m_controller_ready ? m_controller.active_slots() : 0U;
}

SessionControllerOwnedUsage NativeSessionRuntime::owned_usage() const noexcept
{
	return m_controller_ready ? m_controller.owned_usage() : SessionControllerOwnedUsage{};
}

bool NativeSessionRuntime::ingress_ready() const noexcept
{
	return m_state == State::Started && m_transport.is_open();
}

bool NativeSessionRuntime::egress_ready() const noexcept
{
	return m_state == State::Started && m_transport.is_open() && m_controller.has_output();
}

IoStatus NativeSessionRuntime::try_receive() noexcept
{
	const auto received = m_transport.try_receive();
	if (m_performance_observation_active) ++m_last_performance_sample.syscall_count;
	if (m_metrics != nullptr) {
		const auto result = received.status == IoStatus::Complete ? TelemetryIoResult::Complete :
			received.status == IoStatus::WouldBlock ? TelemetryIoResult::WouldBlock :
			received.status == IoStatus::Closed ? TelemetryIoResult::Closed : TelemetryIoResult::Error;
		m_metrics->record_datagram(TelemetryDirection::Rx, result,
			received.status == IoStatus::Complete ? received.datagram.bytes.size : 0U);
	}
	if (received.status == IoStatus::Complete &&
		received.disposition == TransportReceiveDisposition::Datagram) {
		const auto ingress = m_controller.ingest(received.datagram.endpoint,
			received.datagram.bytes,
			m_tick_context.now_us,
			m_tick_context.mission_generation,
			m_tick_context.mission_active);
		m_capture_after_ready_transition = m_capture_after_ready_transition ||
			ingress.disposition == SessionIngressDisposition::WelcomeProofApplied;
		if (m_log != nullptr && ingress.disposition == SessionIngressDisposition::Dropped) {
			m_log->record_drop(log_drop_reason(ingress.drop_reason));
		}
	} else if (m_log != nullptr && received.status == IoStatus::WouldBlock) {
		m_log->record_drop(TelemetryLogDrop::WouldBlock);
	}
	return received.status;
}

IoStatus NativeSessionRuntime::try_send() noexcept
{
	SessionControllerOutput output;
	if (!m_controller.peek_output(output)) {
		return IoStatus::Error;
	}
	const auto status = m_transport.try_send(output.endpoint, {output.bytes.data(), output.size});
	if (m_performance_observation_active) ++m_last_performance_sample.syscall_count;
	if (m_metrics != nullptr) {
		const auto result = status == IoStatus::Complete ? TelemetryIoResult::Complete :
			status == IoStatus::WouldBlock ? TelemetryIoResult::WouldBlock :
			status == IoStatus::Closed ? TelemetryIoResult::Closed : TelemetryIoResult::Error;
		m_metrics->record_datagram(TelemetryDirection::Tx, result, output.size);
	}
	m_output_completion.complete(m_controller, status);
	if (m_log != nullptr && status == IoStatus::WouldBlock) m_log->record_drop(TelemetryLogDrop::WouldBlock);
	return status;
}

NativeSessionTickStatus NativeSessionRuntime::service_r2_tick(const NativeSessionTickContext& context) noexcept
{
	if (m_state == State::Faulted) {
		return m_fault_status;
	}
	if (m_state != State::Started || !m_controller_ready) {
		return NativeSessionTickStatus::Unavailable;
	}

	m_capture_after_ready_transition = false;
	m_tick_context = context;
	m_controller.expire_housekeeping(context.now_us);
	m_controller.service_timeouts(context.now_us);
	m_controller.service_reliability(context.now_us);
	m_controller.service_periodic(context.now_us);
	refresh_metrics_session_scope();
	refresh_log_budget_high_water();
	const auto measure_performance = m_performance_observation_active;
	const auto network_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	const auto result = m_scheduler.run_tick(m_maximum_attempts, *this);
	if (measure_performance) {
		m_last_performance_sample.network_duration_ns = elapsed_nanoseconds(network_started, std::chrono::steady_clock::now());
	}
	if (!result.valid_budget || result.terminal_status == IoStatus::Closed ||
		result.terminal_status == IoStatus::Error) {
		if (m_log != nullptr) m_log->flush_drop_summary(context.now_us);
		fail_transport();
		return NativeSessionTickStatus::PermanentTransportFailure;
	}
	// Ingress/control and existing reliable output are serviced first. Only the
	// idle tail may make an initial snapshot available for a later scheduler
	// pass, so state can never occupy the output slot ahead of a heartbeat or
	// other control response received in this tick.
	const auto serialization_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	(void)m_controller.service_initial_snapshot_egress(m_maximum_attempts, context.now_us);
	// Deltas are the lowest-priority state traffic and are only exposed after
	// control, reliable snapshot work and heartbeat processing above.
	(void)m_controller.service_delta_egress(m_maximum_attempts, context.now_us);
	if (measure_performance) {
		m_last_performance_sample.serialization_duration_ns = elapsed_nanoseconds(serialization_started, std::chrono::steady_clock::now());
	}
	if (m_log != nullptr) m_log->flush_drop_summary(context.now_us);
	return NativeSessionTickStatus::Complete;
}

void NativeSessionRuntime::refresh_performance_resource_sample() noexcept
{
	if (!m_performance_observation_active || !m_controller_ready) return;
	const auto usage = m_controller.owned_usage();
	m_last_performance_sample.allocation_events = m_controller.phase1_observed_allocation_count();
	m_last_performance_sample.queue_depth = usage.reliable_items + (usage.output_queued ? 1U : 0U);
	m_last_performance_sample.baselines_active = 0U;
	m_last_performance_sample.keyframe = false;
	for (std::size_t index = 0U; index < m_controller.owned_capacity().client_slots; ++index) {
		const auto& snapshot = m_controller.slot(index).snapshot;
		if (snapshot.has_active_baseline()) ++m_last_performance_sample.baselines_active;
		if (snapshot.has_candidate()) m_last_performance_sample.keyframe = true;
	}
}

void NativeSessionRuntime::refresh_metrics_session_scope() noexcept
{
	if (!m_controller_ready) return;
	for (std::size_t index = 0U; index < m_controller.owned_capacity().client_slots; ++index) {
		const auto& slot = m_controller.slot(index);
		const bool active = slot.progress != ProducerSessionProgress::Empty;
		if (active && !m_metrics_session_active[index]) {
			if (m_metrics != nullptr) {
				m_metrics->activate_session(index, static_cast<std::uint64_t>(slot.progress));
				m_metrics->increment_session(index, TelemetryMetricCounter::SessionsStarted);
			}
			m_session_started_at_us[index] = slot.session_start_us;
			if (m_log != nullptr) m_log->session_opened(index);
		} else if (!active && m_metrics_session_active[index]) {
			if (m_metrics != nullptr) {
				m_metrics->increment_session(index, TelemetryMetricCounter::SessionsEnded);
				m_metrics->deactivate_session(index);
			}
			if (m_log != nullptr) {
				const auto started_at = m_session_started_at_us[index];
				const auto duration = m_tick_context.now_us >= started_at ? m_tick_context.now_us - started_at : 0U;
				const auto total = m_metrics != nullptr ? m_metrics->snapshot().process_counters[
					static_cast<std::size_t>(TelemetryMetricCounter::SessionsEnded)] : 0U;
				m_log->session_closed(index, TelemetryLogReason::PeerClosed, duration, total);
			}
			m_session_started_at_us[index] = 0U;
		}
		m_metrics_session_active[index] = active;
		if (active && m_metrics != nullptr) {
			m_metrics->set_session_gauges(index,
				static_cast<std::uint64_t>(slot.progress),
				0U, 0U, 0U,
				slot.snapshot.has_candidate() ? 1U : 0U,
				slot.snapshot.has_active_baseline() ? 1U : 0U);
		}
	}
}

void NativeSessionRuntime::refresh_log_budget_high_water() noexcept
{
	if (m_log == nullptr || !m_controller_ready) return;
	const auto capacity = m_controller.owned_capacity();
	const auto usage = m_controller.owned_usage();
	if (capacity.client_slots != 0U && usage.active_slots >= capacity.client_slots) {
		m_log->budget_high_water(TelemetryLogBudget::SessionSlots, capacity.client_slots, usage.active_slots);
	}
	const auto reliable_item_limit = capacity.client_slots * protocol::ReliableWindowMaximumEntries;
	if (reliable_item_limit != 0U && usage.reliable_items != 0U) {
		m_log->budget_high_water(TelemetryLogBudget::ReliableWindow, reliable_item_limit,
			usage.reliable_items);
	}
	if (capacity.state_reassembly_bytes != 0U && usage.reassembly_bytes != 0U) {
		m_log->budget_high_water(TelemetryLogBudget::ReassemblyBytes, capacity.state_reassembly_bytes,
			usage.reassembly_bytes);
	}
}

void NativeSessionRuntime::record_capture_metric(NativePlayerCaptureStatus status, std::uint64_t duration_us) noexcept
{
	if (m_metrics == nullptr) return;
	m_metrics->observe_mission(TelemetryMetricHistogram::CaptureDuration, duration_us);
	if (status == NativePlayerCaptureStatus::CapturedValid) {
		m_metrics->increment_mission(TelemetryMetricCounter::CaptureAttempts);
		m_metrics->record_capture_result(TelemetryCaptureResult::Valid);
	} else if (status == NativePlayerCaptureStatus::CapturedNoPlayer) {
		m_metrics->increment_mission(TelemetryMetricCounter::CaptureAttempts);
		m_metrics->record_capture_result(TelemetryCaptureResult::NoPlayer);
	} else if (status == NativePlayerCaptureStatus::CapturedInvalidSource) {
		m_metrics->increment_mission(TelemetryMetricCounter::CaptureAttempts);
		m_metrics->record_capture_result(TelemetryCaptureResult::InvalidSource);
	}
}

NativeSessionTickStatus NativeSessionRuntime::apply_collected_player_capture(const CaptureResult& result,
	const PlayerObservationDto& observation) noexcept
{
	NativePlayerCaptureStatus status;
	PlayerObservationDto effective_observation;
	if (result.status == CaptureStatus::Valid && result.reason == CaptureReason::None) {
		status = NativePlayerCaptureStatus::CapturedValid;
		effective_observation = observation;
	} else if (result.status == CaptureStatus::NoPlayer && result.reason >= CaptureReason::NotInMission &&
		result.reason <= CaptureReason::MissingPlayerShip) {
		status = NativePlayerCaptureStatus::CapturedNoPlayer;
	} else if (result.status == CaptureStatus::InvalidSource && result.reason >= CaptureReason::WrongObjectType &&
		result.reason < CaptureReason::Count) {
		status = NativePlayerCaptureStatus::CapturedInvalidSource;
	} else {
		fail_capture(NativePlayerCaptureStatus::CaptureInvariantFailure);
		return NativeSessionTickStatus::PermanentCaptureFailure;
	}

	const auto materialization = m_controller.apply_player_observation(result, effective_observation);
	CurrentPlayerCapture current;
	current.available = true;
	current.result = result;
	current.observation = effective_observation;
	m_current_player_capture = current;
	m_last_player_materialization = materialization;
	m_last_player_capture_status = status;

	if (!m_applying_engine_capture || !m_tick_context.mission_active) {
		return NativeSessionTickStatus::Complete;
	}
	const auto mission_generation = m_tick_context.mission_generation == 0U ? 1U : m_tick_context.mission_generation;
	for (std::size_t index = 0U; index < m_controller.owned_capacity().client_slots; ++index) {
		const auto& slot = m_controller.slot(index);
		if (slot.progress != ProducerSessionProgress::ReadyForState) {
			continue;
		}
		Phase1StateImageInput input;
		input.producer_id = m_producer_id;
		input.negotiated_capability_generation = 1U;
		input.session_phase = slot.snapshot.progress() == Phase1SnapshotProgress::Live ? protocol::SessionPhase::Live
																													 : protocol::SessionPhase::Synchronizing;
		input.mission.producer_sample_time_us = m_tick_context.now_us;
		input.mission.mission_generation = mission_generation;
		input.mission.phase = m_tick_context.mission_active ? protocol::MissionPhase::Active : protocol::MissionPhase::None;
		input.mission.paused = false;
		input.mission.time_compression = 1.0F;
		input.player_capture = result;
		input.player = slot.latest_player_sample;
		protocol::StateImage image;
		Phase1StateImageBuildTiming image_timing{};
		const auto image_build_started = m_performance_observation_active ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		const auto image_status =
			build_phase1_state_image_preallocated(input, m_state_image_pools[index], image,
				m_performance_observation_active ? &image_timing : nullptr);
		if (m_performance_observation_active) {
			m_last_performance_sample.state_image_build_duration_ns +=
				elapsed_nanoseconds(image_build_started, std::chrono::steady_clock::now());
			m_last_performance_sample.state_image_fill_duration_ns += image_timing.fill_records_ns;
			m_last_performance_sample.state_image_publish_validate_duration_ns += image_timing.publish_validate_ns;
			m_last_performance_sample.state_image_adopt_duration_ns += image_timing.adopt_preallocated_ns;
			m_last_performance_sample.state_image_semantic_validate_duration_ns += image_timing.semantic_validate_ns;
		}
		if (image_status == Phase1StateImageBuildStatus::AllocationFailed) {
			// All immutable backings are still retained by current/active/candidate
			// state. Preserve them and drop this replaceable capture; the next tick
			// retries once a baseline/candidate releases an owned backing.
			continue;
		}
		if (image_status != Phase1StateImageBuildStatus::Created) {
			fail_capture(NativePlayerCaptureStatus::CaptureInvariantFailure);
			return NativeSessionTickStatus::PermanentCaptureFailure;
		}
		if (!slot.snapshot.has_candidate() && !slot.snapshot.has_active_baseline()) {
			(void)m_controller.begin_initial_snapshot(index, image, m_tick_context.now_us);
			continue;
		}
		const auto delta_build_started = m_performance_observation_active ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		if (m_controller.replace_current_state(index, image) != protocol::ProducerBaselineResult::Applied) {
			fail_capture(NativePlayerCaptureStatus::CaptureInvariantFailure);
			return NativeSessionTickStatus::PermanentCaptureFailure;
		}
		if (slot.snapshot.has_active_baseline()) {
			(void)m_controller.queue_cumulative_delta(index, m_tick_context.now_us);
		}
		if (m_performance_observation_active) {
			m_last_performance_sample.delta_build_duration_ns +=
				elapsed_nanoseconds(delta_build_started, std::chrono::steady_clock::now());
		}
	}
	return NativeSessionTickStatus::Complete;
}

void NativeSessionRuntime::clear_player_capture() noexcept
{
	m_current_player_capture = {};
	m_last_player_materialization = {};
	m_last_player_capture_status = NativePlayerCaptureStatus::Unavailable;
}

bool NativeSessionRuntime::provision_state_image_pools(std::size_t client_count) noexcept
{
	if (client_count == 0U || client_count > m_state_image_pools.size()) {
		return false;
	}
	std::size_t expected_backing_bytes = 0U;
	if (!checked_multiply_size(client_count, Phase1StateImagePool::BackingBytesPerClient, expected_backing_bytes) ||
		expected_backing_bytes > WP03ProvisionalKnownBudgetCapBytes) {
		return false;
	}
	release_state_image_pools();
	for (std::size_t index = 0U; index < client_count; ++index) {
		if (!m_state_image_pools[index].provision()) {
			release_state_image_pools();
			return false;
		}
	}
	std::size_t backing_bytes = 0U;
	for (std::size_t index = 0U; index < client_count; ++index) {
		const auto owned = m_state_image_pools[index].owned_backing_bytes();
		if (owned == 0U || !checked_add_size(backing_bytes, owned, backing_bytes)) {
			release_state_image_pools();
			return false;
		}
	}
	if (backing_bytes != expected_backing_bytes) {
		release_state_image_pools();
		return false;
	}
	m_state_image_pool_backing_bytes = backing_bytes;
	const auto budget = calculate_wp06_startup_budget(
		calculate_wp04_startup_budget(make_wp03_known_budget_request(client_count)), client_count);
	if (!wp06_budget_matches_state_image_pool(budget, client_count, m_state_image_pool_backing_bytes)) {
		release_state_image_pools();
		return false;
	}
	return true;
}

void NativeSessionRuntime::release_state_image_pools() noexcept
{
	for (auto& pool : m_state_image_pools) {
		pool.reset();
	}
	m_state_image_pool_backing_bytes = 0U;
}

std::uint64_t NativeSessionRuntime::state_image_pool_allocation_count() const noexcept
{
	std::uint64_t total = 0U;
	for (const auto& pool : m_state_image_pools) {
		const auto count = pool.successful_allocation_count();
		if (std::numeric_limits<std::uint64_t>::max() - total < count) {
			return std::numeric_limits<std::uint64_t>::max();
		}
		total += count;
	}
	return total;
}

std::size_t NativeSessionRuntime::state_image_pool_backing_bytes() const noexcept
{
	return m_state_image_pool_backing_bytes;
}

void NativeSessionRuntime::fail_transport() noexcept
{
	if (m_state != State::Started) {
		return;
	}
	purge_all(SessionCloseReason::TransportError);
	m_transport.close();
	m_scheduler.reset();
	m_capture_cadence.stop();
	clear_player_capture();
	release_state_image_pools();
	m_producer_id = 0U;
	m_fault_status = NativeSessionTickStatus::PermanentTransportFailure;
	m_state = State::Faulted;
}

void NativeSessionRuntime::fail_capture(NativePlayerCaptureStatus status) noexcept
{
	if (m_state != State::Started) {
		return;
	}
	purge_all(SessionCloseReason::ProtocolError);
	m_transport.close();
	m_scheduler.reset();
	m_capture_cadence.stop();
	clear_player_capture();
	release_state_image_pools();
	m_last_player_capture_status = status;
	m_fault_status = NativeSessionTickStatus::PermanentCaptureFailure;
	m_state = State::Faulted;
}

const SessionControllerSlot* NativeSessionRuntimeTestAccess::slot(const NativeSessionRuntime& runtime,
	std::size_t index) noexcept
{
	return runtime.m_controller_ready && index < runtime.m_controller.owned_capacity().client_slots
			? &runtime.m_controller.slot(index)
			: nullptr;
}

SessionController* NativeSessionRuntimeTestAccess::controller(NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_controller_ready ? &runtime.m_controller : nullptr;
}

void NativeSessionRuntimeTestAccess::set_session_controller_provision_failure(NativeSessionRuntime& runtime,
	bool fail) noexcept
{
	runtime.m_fail_session_controller_provision = fail;
}

void NativeSessionRuntimeTestAccess::begin_steady_state_allocation_tracking(NativeSessionRuntime& runtime) noexcept
{
	if (runtime.m_controller_ready) {
		runtime.m_controller.begin_phase1_allocation_observation();
	}
}

std::uint64_t NativeSessionRuntimeTestAccess::steady_state_allocation_count(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_controller_ready ? runtime.m_controller.phase1_observed_allocation_count() : 0U;
}

void NativeSessionRuntimeTestAccess::force_steady_state_allocation_for_tests(NativeSessionRuntime& runtime) noexcept
{
	if (!runtime.m_controller_ready) {
		return;
	}
	auto allocation = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[1U]);
	if (allocation == nullptr) {
		return;
	}
	runtime.m_test_allocation_probe = std::move(allocation);
	runtime.m_controller.note_phase1_runtime_allocation_for_test();
}

void NativeSessionRuntimeTestAccess::begin_performance_observation(NativeSessionRuntime& runtime) noexcept
{
	runtime.m_performance_observation_active = runtime.m_controller_ready;
	runtime.m_last_performance_sample = {};
	if (runtime.m_performance_observation_active) runtime.m_controller.begin_phase1_allocation_observation();
}

NativeRuntimePerformanceSample NativeSessionRuntimeTestAccess::last_performance_sample(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_last_performance_sample;
}

bool NativeSessionRuntimeTestAccess::seed_last_allocated_entity_id(NativeSessionRuntime& runtime,
	std::size_t index,
	std::uint64_t last_id) noexcept
{
	return runtime.m_controller_ready &&
		SessionControllerTestAccess::seed_last_allocated_entity_id(runtime.m_controller, index, last_id);
}

NativeSessionTickStatus NativeSessionRuntimeTestAccess::inject_collected_player_capture(NativeSessionRuntime& runtime,
	const CaptureResult& result,
	const PlayerObservationDto& observation) noexcept
{
	if (runtime.m_state != NativeSessionRuntime::State::Started || !runtime.m_controller_ready) {
		return NativeSessionTickStatus::Unavailable;
	}
	return runtime.apply_collected_player_capture(result, observation);
}

NativeSessionTickStatus NativeSessionRuntimeTestAccess::service_r2_tick(NativeSessionRuntime& runtime,
	const NativeSessionTickContext& context) noexcept
{
	return runtime.service_r2_tick(context);
}

TelemetryLogSnapshot NativeSessionRuntimeTestAccess::log_snapshot(const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_log != nullptr ? runtime.m_log->snapshot() : TelemetryLogSnapshot{};
}

} // namespace telemetry::detail
