#pragma once

#include "telemetry/capture_scheduler.h"
#include "telemetry/cockpit_producer_eligibility.h"
#include "telemetry/config.h"
#include "telemetry/datagram_scheduler.h"
#include "telemetry/metrics.h"
#include "telemetry/phase2_observation.h"
#include "telemetry/phase3_engine_collector.h"
#include "telemetry/phase3_identity_registry.h"
#include "telemetry/cockpit_sensors_state_image.h"
#include "telemetry/session_controller.h"
#include "telemetry/startup_budget.h"
#include "telemetry/transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace telemetry::detail {

class NativeSessionRuntimeTestAccess;
class TelemetryStructuredLog;

enum class NativeSessionStartStatus : std::uint8_t {
	Started = 0,
	InvalidConfiguration,
	AlreadyStarted,
	AllocationFailure,
	TransportUnavailable,
};

enum class NativeSessionTickStatus : std::uint8_t {
	Complete = 0,
	Unavailable,
	PermanentTransportFailure,
	PermanentCaptureFailure,
};

enum class NativePlayerCaptureStatus : std::uint8_t {
	Unavailable = 0,
	Inactive,
	NotDue,
	CapturedValid,
	CapturedNoPlayer,
	CapturedInvalidSource,
	CaptureInvariantFailure,
	CadenceFailure,
	Count,
};

struct CurrentPlayerCapture {
	bool available = false;
	CaptureResult result{};
	PlayerObservationDto observation{};
};

struct NativeSessionStartRequest {
	const TelemetryConfig* config = nullptr;
	std::uint64_t producer_id = 0U;
	SessionIdAllocator* session_ids = nullptr;
	RandomSource* packet_sequences = nullptr;
	TelemetryMetrics* metrics = nullptr;
	TelemetryStructuredLog* log = nullptr;
	CockpitProducerEligibility cockpit_eligibility{};
};

struct NativeSessionTickContext {
	std::uint64_t now_us = 0U;
	std::uint32_t mission_generation = 0U;
	bool mission_active = false;
	bool mission_paused = false;
	float time_compression = 1.0F;
	bool resume_transport = false;
};

struct Phase2CapturePlan {
	bool capture_flight_controls = false;
	bool capture_systems = false;
	bool force_complete_keyframe = false;
	bool phase3_refresh_targeting = false;
	bool phase3_refresh_systems = false;
	std::uint64_t producer_sample_time_us = 0U;
};

// Bounded, test-only benchmark observation copied from the actual native
// runtime tick. It is populated only after the explicit test seam enables
// observation, so normal Phase 1 execution pays only guarded assignments.
// Durations are monotonic-clock nanoseconds and are never exported on wire.
struct NativeRuntimePerformanceSample {
	std::uint64_t tick_duration_ns = 0U;
	std::uint64_t collect_duration_ns = 0U;
	std::uint64_t diff_duration_ns = 0U;
	// Test-only subcomponents of the producer capture/diff path. They are
	// populated only while the explicit performance observation seam is armed.
	std::uint64_t state_image_build_duration_ns = 0U;
	std::uint64_t state_image_fill_duration_ns = 0U;
	std::uint64_t state_image_publish_validate_duration_ns = 0U;
	std::uint64_t state_image_adopt_duration_ns = 0U;
	std::uint64_t state_image_semantic_validate_duration_ns = 0U;
	std::uint64_t delta_build_duration_ns = 0U;
	std::uint64_t serialization_duration_ns = 0U;
	std::uint64_t network_duration_ns = 0U;
	std::uint64_t allocation_events = 0U;
	std::uint64_t syscall_count = 0U;
	std::size_t queue_depth = 0U;
	std::size_t baselines_active = 0U;
	bool keyframe = false;
};

enum class NativePhase2FailureStage : std::uint8_t {
	None = 0,
	PlayerCaptureClassification,
	Phase2Precondition,
	Closure,
	Lifecycle,
	Manifest,
	PlayerBinding,
	BlockSamples,
	StateImage,
	BaselineReplace,
	Count,
};

struct NativePhase2FailureDiagnostic {
	std::uint64_t tick_now_us = 0U;
	std::uint64_t phase2_capture_sample_time_us = 0U;
	std::size_t ship_count = 0U;
	std::size_t first_zero_block = Phase2RuntimeSlot::BlockCount;
	std::uint64_t player_entity_id = 0U;
	std::uint32_t player_key = 0U;
	std::uint32_t global_manifest_id = 0U;
	std::uint32_t required_manifest_id = 0U;
	std::size_t client_slot = 0U;
	NativePhase2FailureStage stage = NativePhase2FailureStage::None;
	CaptureStatus player_capture_status = CaptureStatus::Count;
	CaptureReason player_capture_reason = CaptureReason::Count;
	Phase2RuntimeResult runtime_result = Phase2RuntimeResult::Count;
	CockpitSensorsStateImageBuildDiagnostic image_diagnostic{};
	Phase3EngineCollectDiagnostic phase3_diagnostic{};
	Phase3StateImageBuildStatus phase3_image_status =
		Phase3StateImageBuildStatus::Count;
	protocol::ProducerBaselineResult baseline_result =
		protocol::ProducerBaselineResult::InvalidArgument;
	bool capture_observed_this_tick = false;
	bool required_manifest_applied = false;
};

class NativeOutputCompletionPort {
  public:
	virtual ~NativeOutputCompletionPort() noexcept = default;
	virtual void complete(SessionController& controller, IoStatus status) noexcept = 0;
};

class NativeOutputCompletionForwarder final : public NativeOutputCompletionPort {
  public:
	void complete(SessionController& controller, IoStatus status) noexcept override;
};

class NativeSessionRuntime final : private DatagramIoWork {
  public:
	NativeSessionRuntime(UdpSocketBackend& backend, NativeOutputCompletionPort& output_completion) noexcept;
	~NativeSessionRuntime() noexcept;

	NativeSessionRuntime(const NativeSessionRuntime&) = delete;
	NativeSessionRuntime& operator=(const NativeSessionRuntime&) = delete;
	NativeSessionRuntime(NativeSessionRuntime&&) = delete;
	NativeSessionRuntime& operator=(NativeSessionRuntime&&) = delete;

	NativeSessionStartStatus start(const NativeSessionStartRequest& request) noexcept;
	NativeSessionTickStatus service_tick(const NativeSessionTickContext& context,
		const EngineReadView& engine_view,
		const Phase2EngineReadView* phase2_view = nullptr) noexcept;
	void stop_collection() noexcept;
	void end_mission_sessions() noexcept;
	void purge_all(SessionCloseReason reason) noexcept;
	void shutdown() noexcept;

	std::size_t socket_count() const noexcept;
	std::size_t active_sessions() const noexcept;
	SessionControllerOwnedUsage owned_usage() const noexcept;
	NativePlayerCaptureStatus last_player_capture_status() const noexcept
	{
		return m_last_player_capture_status;
	}
	const CurrentPlayerCapture& current_player_capture() const noexcept
	{
		return m_current_player_capture;
	}
	const SessionPlayerMaterializationResult& last_player_materialization() const noexcept
	{
		return m_last_player_materialization;
	}

  private:
	friend class NativeSessionRuntimeTestAccess;

	enum class State : std::uint8_t { Cold = 0, Started, Faulted, Stopped };

	bool ingress_ready() const noexcept override;
	bool egress_ready() const noexcept override;
	IoStatus try_receive() noexcept override;
	IoStatus try_send() noexcept override;
	NativeSessionTickStatus service_r2_tick(const NativeSessionTickContext& context) noexcept;
	NativeSessionTickStatus apply_collected_player_capture(const CaptureResult& result,
		const PlayerObservationDto& observation) noexcept;
	void clear_player_capture() noexcept;
	void fail_transport() noexcept;
	void fail_capture(NativePlayerCaptureStatus status) noexcept;
	NativePhase2FailureDiagnostic* begin_phase2_failure_diagnostic(
		NativePhase2FailureStage stage) noexcept;
	void prepare_phase2_keyframe(Phase2CapturePlan& plan) noexcept;
	bool provision_phase2_image_pools(std::size_t client_count) noexcept;
	void release_state_image_pools() noexcept;
	void refresh_metrics_session_scope() noexcept;
	void refresh_log_budget_high_water() noexcept;
	void record_capture_metric(NativePlayerCaptureStatus status, std::uint64_t duration_us) noexcept;
	void refresh_performance_resource_sample() noexcept;
	void observe_phase2_support_transitions() noexcept;
	void reset_phase2_support_tracker() noexcept;
	bool provision_phase2_manifest_state() noexcept;
	bool provision_phase2_catalog_source() noexcept;
	bool provision_phase3_manifest_states(
		std::size_t client_count) noexcept;
	bool refresh_owned_phase2_manifest(
		const Phase2ObservationDto& observation,
		Phase2ManifestError& result) noexcept;
	bool refresh_phase3_manifest(
		std::size_t client_slot,
		const Phase2ObservationDto& observation,
		const Phase2Wp05SubjectBinding* bindings,
		std::size_t binding_count,
		Phase2ManifestError& result,
		Phase3EngineCollectDiagnostic& diagnostic) noexcept;
	void release_unreferenced_phase2_manifest_generation() noexcept;
	void release_phase2_manifest_state() noexcept;
	void release_phase3_manifest_states() noexcept;

	DedicatedUdpTransport m_transport;
	NativeOutputCompletionPort& m_output_completion;
	SessionController m_controller;
	DatagramTickScheduler m_scheduler;
	Capture30Hz m_capture_cadence;
	Capture30Hz m_systems_capture_cadence;
	Phase2ObservationBuffer m_phase2_observation;
	Phase2CapturePlan m_phase2_capture_plan{};
	Phase2CaptureResult m_last_phase2_capture_result{};
	std::array<std::unique_ptr<Phase3Projection>, 4U>
		m_phase3_projections{};
	std::array<std::unique_ptr<Phase3Projection>, 4U>
		m_phase3_projection_scratch{};
	std::array<std::unique_ptr<Phase3IdentityRegistry>, 4U>
		m_phase3_identity_registries{};
	CurrentPlayerCapture m_current_player_capture;
	SessionPlayerMaterializationResult m_last_player_materialization;
	NativeSessionTickContext m_tick_context{};
	NativePlayerCaptureStatus m_last_player_capture_status = NativePlayerCaptureStatus::Unavailable;
	NativeSessionTickStatus m_fault_status = NativeSessionTickStatus::Unavailable;
	std::uint16_t m_maximum_attempts = 0U;
	std::uint64_t m_producer_id = 0U;
	State m_state = State::Cold;
	bool m_controller_ready = false;
	// Test-only failpoint, reached before controller provisioning and bind. It
	// is deliberately inert in normal runtime operation.
	bool m_fail_session_controller_provision = false;
	std::size_t m_startup_owned_budget_test_adjustment = 0U;
	std::size_t m_startup_owned_bytes = 0U;
	std::uint64_t m_startup_allocation_count = 0U;
	std::size_t m_cockpit_sensor_image_pool_backing_bytes = 0U;
	Phase2OwnedBudget m_phase2_owned_budget{};
	Phase2Wp07GlobalEventBatch m_phase2_event_batch_scratch{};
	Phase2GlobalFanoutResult m_phase2_fanout_scratch{};
	struct Phase2SupportTrackerEntry {
		Phase2CaptureLocalKey capture_key{};
		std::uint32_t signature = 0U;
		ShipSupportPhase previous = ShipSupportPhase::None;
		bool active = false;
	};
	std::array<Phase2SupportTrackerEntry,
		MaximumPhase2ObservationShips> m_phase2_support_tracker{};
	std::array<bool, MaximumPhase2ObservationShips>
		m_phase2_support_seen_scratch{};
	// Retained only by the production-owned friend seam to prove that the
	// scoped observer sees a real runtime allocation event.
	std::unique_ptr<std::uint8_t[]> m_test_allocation_probe;
	std::array<CockpitSensorsStateImagePool, 4U>
		m_cockpit_sensor_image_pools{};
	struct Phase3ManifestState {
		std::unique_ptr<Phase2ManifestStorage> storage;
		std::unique_ptr<Phase2ManifestSlot> slot;
		const Phase2ManifestCandidate* manifest = nullptr;
		bool catalog_projection_pending = false;
	};
	// Cockpit catalog visibility is observer-specific.  Each client owns its
	// selected candidate while a bounded mission workspace is reused only to
	// encode it into that client's reliable egress queue.
	std::array<Phase3ManifestState, 4U> m_phase3_manifest_states{};
	std::unique_ptr<std::uint8_t[]> m_phase3_manifest_workspace;
	std::size_t m_phase3_manifest_workspace_owner = 4U;
	std::unique_ptr<Phase2ManifestSource>
		m_phase3_manifest_selection_source;
	std::size_t m_phase3_manifest_backing_bytes = 0U;
	std::unique_ptr<std::uint8_t[]> m_phase2_manifest_backing;
	std::unique_ptr<Phase2ManifestSource> m_phase2_manifest_source;
	std::unique_ptr<Phase2ManifestStorage> m_phase2_manifest_storage;
	std::unique_ptr<Phase2ManifestSlot> m_phase2_manifest_slot;
	std::size_t m_phase2_manifest_backing_bytes = 0U;
	const Phase2ManifestCandidate* m_phase2_manifest = nullptr;
	bool m_phase2_catalog_projection_pending = false;
	TelemetryMetrics* m_metrics = nullptr;
	TelemetryStructuredLog* m_log = nullptr;
	std::array<bool, TelemetryMetricsMaxClients> m_metrics_session_active{};
	std::array<std::uint64_t, TelemetryMetricsMaxClients> m_session_started_at_us{};
	std::array<std::uint64_t, TelemetryMetricsMaxClients>
		m_phase2_started_snapshot_sequences{};
	bool m_capture_after_ready_transition = false;
	bool m_capture_for_phase3_keyframe = false;
	bool m_capture_for_pause_transition = false;
	bool m_pause_state_initialized = false;
	bool m_last_mission_paused = false;
	bool m_applying_engine_capture = false;
	bool m_phase2_event_pipeline_failed_closed = false;
	bool m_performance_observation_active = false;
	NativeRuntimePerformanceSample m_last_performance_sample{};
	NativePhase2FailureDiagnostic m_last_phase2_failure_diagnostic{};
};

} // namespace telemetry::detail
