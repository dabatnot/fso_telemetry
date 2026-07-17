#include "telemetry/native_session_runtime.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <utility>

namespace telemetry::detail {
namespace {

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

	const auto opened = m_transport.open(*request.config);
	if (opened == TransportOpenStatus::InvalidConfiguration || opened == TransportOpenStatus::Disabled) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	if (opened != TransportOpenStatus::Complete) {
		return NativeSessionStartStatus::TransportUnavailable;
	}

	m_controller = std::move(controller);
	m_controller_ready = true;
	m_maximum_attempts = request.config->max_datagrams_per_tick;
	m_scheduler.reset();
	m_capture_cadence = capture_cadence;
	clear_player_capture();
	m_fault_status = NativeSessionTickStatus::Unavailable;
	m_state = State::Started;
	return NativeSessionStartStatus::Started;
}

NativeSessionTickStatus NativeSessionRuntime::service_tick(const NativeSessionTickContext& context) noexcept
{
	return service_r2_tick(context);
}

NativeSessionTickStatus NativeSessionRuntime::service_tick(const NativeSessionTickContext& context,
	const EngineReadView& engine_view) noexcept
{
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
		return NativeSessionTickStatus::Complete;
	case CaptureCadenceStatus::NotDue:
		m_last_player_materialization = {};
		m_last_player_capture_status = NativePlayerCaptureStatus::NotDue;
		return NativeSessionTickStatus::Complete;
	case CaptureCadenceStatus::Due:
		break;
	case CaptureCadenceStatus::InvalidRate:
	case CaptureCadenceStatus::ClockRegression:
	case CaptureCadenceStatus::DeadlineOverflow:
	case CaptureCadenceStatus::Count:
		fail_capture(NativePlayerCaptureStatus::CadenceFailure);
		return NativeSessionTickStatus::PermanentCaptureFailure;
	}

	PlayerObservationDto observation;
	const auto capture = collect_player_kinematics(engine_view, context.now_us, observation);
	return apply_collected_player_capture(capture, observation);
}

void NativeSessionRuntime::purge_all(SessionCloseReason reason) noexcept
{
	if (m_controller_ready) {
		m_controller.purge_all(reason);
	}
	m_capture_cadence.stop();
	clear_player_capture();
}

void NativeSessionRuntime::shutdown() noexcept
{
	if (m_state == State::Stopped) {
		return;
	}
	if (m_controller_ready) {
		m_controller.purge_all(SessionCloseReason::Shutdown);
	}
	m_transport.close();
	m_scheduler.reset();
	m_capture_cadence.reset();
	clear_player_capture();
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
	if (received.status == IoStatus::Complete &&
		received.disposition == TransportReceiveDisposition::Datagram) {
		(void)m_controller.ingest(received.datagram.endpoint,
			received.datagram.bytes,
			m_tick_context.now_us,
			m_tick_context.mission_generation,
			m_tick_context.mission_active);
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
	m_output_completion.complete(m_controller, status);
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

	m_tick_context = context;
	m_controller.expire_housekeeping(context.now_us);
	m_controller.service_timeouts(context.now_us);
	m_controller.service_reliability(context.now_us);
	m_controller.service_periodic(context.now_us);
	const auto result = m_scheduler.run_tick(m_maximum_attempts, *this);
	if (!result.valid_budget || result.terminal_status == IoStatus::Closed ||
		result.terminal_status == IoStatus::Error) {
		fail_transport();
		return NativeSessionTickStatus::PermanentTransportFailure;
	}
	return NativeSessionTickStatus::Complete;
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
	return NativeSessionTickStatus::Complete;
}

void NativeSessionRuntime::clear_player_capture() noexcept
{
	m_current_player_capture = {};
	m_last_player_materialization = {};
	m_last_player_capture_status = NativePlayerCaptureStatus::Unavailable;
}

void NativeSessionRuntime::fail_transport() noexcept
{
	if (m_state != State::Started) {
		return;
	}
	m_controller.purge_all(SessionCloseReason::TransportError);
	m_transport.close();
	m_scheduler.reset();
	m_capture_cadence.stop();
	clear_player_capture();
	m_fault_status = NativeSessionTickStatus::PermanentTransportFailure;
	m_state = State::Faulted;
}

void NativeSessionRuntime::fail_capture(NativePlayerCaptureStatus status) noexcept
{
	if (m_state != State::Started) {
		return;
	}
	m_controller.purge_all(SessionCloseReason::ProtocolError);
	m_transport.close();
	m_scheduler.reset();
	m_capture_cadence.stop();
	clear_player_capture();
	m_last_player_capture_status = status;
	m_fault_status = NativeSessionTickStatus::PermanentCaptureFailure;
	m_state = State::Faulted;
}

} // namespace telemetry::detail
