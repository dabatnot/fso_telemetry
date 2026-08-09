#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace telemetry::detail {

// Metrics labels are intentionally closed.  These values are local storage
// indexes, never wire values and never user-controlled labels.
enum class TelemetryCallbackKind : std::uint8_t {
	EngineUpdate = 0,
	EngineShutdown,
	GameMissionLoad,
	GameEnterState,
	GameLeaveState,
	Count,
};
enum class TelemetryDirection : std::uint8_t { Rx = 0, Tx, Count };
enum class TelemetryIoResult : std::uint8_t { Complete = 0, WouldBlock, Closed, Error, Count };
enum class TelemetryCaptureResult : std::uint8_t { Valid = 0, NoPlayer, InvalidSource, Count };
enum class TelemetrySnapshotKind : std::uint8_t { Initial = 0, Periodic, Resync, Count };
enum class TelemetryPendingKind : std::uint8_t {
	Reliable = 0, Reassembly, SnapshotCandidate, Delta, ResyncIntent, HeartbeatProbe, Count
};
enum class TelemetryDeltaDropReason : std::uint8_t {
	Replaced = 0, StaleBaseline, UnknownBaseline, SessionEnd, WouldBlock, Count
};
enum class TelemetrySessionEndReason : std::uint8_t {
	Timeout = 0, MissionDiscontinuity, PeerClosed, ProtocolError, TransportError, Shutdown, Count
};
enum class TelemetryRuntimeFaultReason : std::uint8_t {
	Config = 0, Entropy, IdentityStore, BudgetOverflow, Allocation, Socket, Invariant, Count
};
enum class TelemetryPhase2Block : std::uint8_t {
	Identity = 0,
	Flight,
	Control,
	DamageShield,
	EnergyPropulsion,
	Weapons,
	Subsystems,
	SupportCargoDocking,
	Count,
};
enum class TelemetryPhase2ProfileRejection : std::uint8_t {
	UnsupportedAuthority = 0,
	UnsupportedVisibility,
	IncompleteCoverage,
	InvalidSource,
	SourceLimit,
	RecordTooLarge,
	TransactionTooLarge,
	BudgetExceeded,
	ManifestUnavailable,
	Count,
};
enum class TelemetryPhase2CaptureFailure : std::uint8_t {
	Guard = 0,
	NonFinite,
	OutOfRange,
	InvalidReference,
	IncoherentTopology,
	InvalidEnum,
	StaleAuthority,
	Count,
};
enum class TelemetryPhase2ClosureResult : std::uint8_t {
	Created = 0, Unchanged, TopologyChanged, CatalogChanged, Rejected, Count
};
enum class TelemetryPhase2ManifestResult : std::uint8_t {
	Built = 0, Reused, Rejected, Count
};
enum class TelemetryPhase2Profile : std::uint8_t {
	None = 0, CoreGate, CompleteShip, CockpitSensors, Count
};
enum class TelemetryPhase2LifecycleKind : std::uint8_t {
	Appeared = 0, Disabled, DyingStarted, Destroyed, Disappeared, Count
};
enum class TelemetryPhase2SupportKind : std::uint8_t {
	Requested = 0, Approaching, Docking, Repairing, Rearming, Obstructed,
	Aborted, CompletedPrivate, EndedPrivate, Count
};
enum class TelemetryPhase2SupportCoalescedKind : std::uint8_t {
	CompleteThenEndSameEpisode = 0, SupersededByNewEpisode, NewerGeneration, Count
};
enum class TelemetryPhase2KeyframeReason : std::uint8_t {
	Periodic = 0, Topology, Catalog, Lifecycle, Support, Resync, Count
};
enum class TelemetryPhase2AllocationKind : std::uint8_t {
	VectorGrowth = 0, StringGrowth, HeapFallback, LazyIndex, Other, Count
};
enum class TelemetryPhase2SourceLimit : std::uint8_t {
	Ships,
	Classes,
	Weapons,
	Subsystems,
	ClassBanks,
	ImageRecords,
	RecordBytes,
	TransactionBytes,
	Parts,
	Memory,
	Count,
};
enum class TelemetryPhase2Ring : std::uint8_t { Support = 0, Cleanup, Count };
enum class TelemetryPhase2MemoryScope : std::uint8_t {
	Shared = 0, ClientTotal, ProcessTotal, Count
};
enum class TelemetryPhase3Block : std::uint8_t {
	Precondition = 0,
	TargetLocks,
	HudAlerts,
	Radar,
	Threat,
	Cargo,
	Navigation,
	StateImage,
	Count,
};
enum class TelemetryPhase3CaptureFailure : std::uint8_t {
	NoPlayer = 0,
	NotMainThread,
	InvalidSource,
	SourceLimitExceeded,
	IdentityFailure,
	InvalidInput,
	CapacityExceeded,
	EncodingFailed,
	AllocationFailed,
	Count,
};

enum class TelemetryMetricCounter : std::uint8_t {
	RuntimeFaults = 0,
	Allocations,
	AllocationFailures,
	Datagrams,
	DatagramBytes,
	WouldBlock,
	DatagramDrops,
	ValidationErrors,
	AllowlistDrops,
	RateLimitDrops,
	AntiAmplificationDrops,
	SessionsStarted,
	SessionsEnded,
	HeartbeatProbes,
	HeartbeatSamples,
	Fragments,
	ReassemblyExpired,
	Acks,
	Nacks,
	Retransmissions,
	ResyncRequests,
	SourceNormalizations,
	CaptureAttempts,
	PlayerDiscontinuities,
	SnapshotsCreated,
	SnapshotsApplied,
	DeltasCreated,
	DeltasReplaced,
	DeltasDropped,
	BudgetExhaustions,
	CounterOverflow,
	Count,
};

enum class TelemetryMetricHistogram : std::uint8_t {
	CallbackDuration = 0,
	CaptureDuration,
	DiffDuration,
	SerializationDuration,
	NetworkDuration,
	TickDuration,
	Count,
};

constexpr std::size_t TelemetryMetricHistogramBucketCount = 9U;
constexpr std::size_t TelemetryMetricsMaxClients = 4U;

struct TelemetryHistogramSnapshot {
	std::array<std::uint64_t, TelemetryMetricHistogramBucketCount> buckets{};
	std::uint64_t count = 0U;
	std::uint64_t sum_us = 0U;
};

struct TelemetrySessionMetricsSnapshot {
	bool active = false;
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryMetricCounter::Count)> counters{};
	std::array<TelemetryHistogramSnapshot, static_cast<std::size_t>(TelemetryMetricHistogram::Count)> histograms{};
	std::uint64_t state = 0U;
	std::uint64_t heartbeat_rtt_us = 0U;
	std::int64_t heartbeat_offset_us = 0;
	std::uint64_t reassemblies_active = 0U;
	std::uint64_t reassemblies_active_high_water = 0U;
	std::uint64_t reassembly_bytes = 0U;
	std::uint64_t reassembly_bytes_high_water = 0U;
	std::uint64_t reliable_window_items = 0U;
	std::uint64_t reliable_window_items_high_water = 0U;
	std::uint64_t snapshot_candidates = 0U;
	std::uint64_t baselines_active = 0U;
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPendingKind::Count)> pending_items{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPendingKind::Count)> pending_items_high_water{};
	TelemetryPhase2Profile phase2_profile = TelemetryPhase2Profile::None;
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2ManifestResult::Count)>
		phase2_manifest_builds{};
	TelemetryHistogramSnapshot phase2_manifest_duration{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2KeyframeReason::Count)>
		phase2_forced_keyframes{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2Block::Count)>
		phase2_sample_age_us{};
	std::uint64_t phase2_image_records = 0U;
	std::uint64_t phase2_image_bytes = 0U;
	std::uint64_t phase2_dirty_atoms = 0U;
	std::uint64_t phase2_manifest_bytes = 0U;
	std::uint64_t phase2_manifest_parts = 0U;
	std::uint64_t phase2_manifest_generations = 0U;
	std::uint64_t phase2_support_latches = 0U;
	std::uint64_t phase2_support_latches_high_water = 0U;
	std::uint64_t phase2_manifest_rebuild_coalesced = 0U;
};

// Deliberately flat and copyable: diagnostics/tests can snapshot this without
// exposing live runtime ownership or any unbounded label/payload storage.
struct TelemetryMetricsSnapshot {
	bool provisioned = false;
	std::uint64_t runtime_state = 0U;
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryCallbackKind::Count)> callbacks{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryMetricCounter::Count)> process_counters{};
	std::array<std::array<std::uint64_t, static_cast<std::size_t>(TelemetryIoResult::Count)>,
		static_cast<std::size_t>(TelemetryDirection::Count)> datagrams{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryDirection::Count)> datagram_bytes{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryCaptureResult::Count)> capture_results{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetrySnapshotKind::Count)> snapshots_created{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetrySnapshotKind::Count)> snapshots_applied{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryDeltaDropReason::Count)> delta_drops{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetrySessionEndReason::Count)> session_ends{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryRuntimeFaultReason::Count)> runtime_faults{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2ProfileRejection::Count)>
		phase2_profile_rejections{};
	std::array<TelemetryHistogramSnapshot, static_cast<std::size_t>(TelemetryPhase2Block::Count)>
		phase2_capture_duration{};
	std::array<std::array<std::uint64_t,
			static_cast<std::size_t>(TelemetryPhase2CaptureFailure::Count)>,
		static_cast<std::size_t>(TelemetryPhase2Block::Count)>
		phase2_capture_failures{};
	std::array<std::array<std::uint64_t,
			static_cast<std::size_t>(TelemetryPhase3CaptureFailure::Count)>,
		static_cast<std::size_t>(TelemetryPhase3Block::Count)>
		phase3_capture_failures{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2ClosureResult::Count)>
		phase2_closure_results{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2ManifestResult::Count)>
		phase2_manifest_results{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2AllocationKind::Count)>
		phase2_allocations_after_ready{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2SourceLimit::Count)>
		phase2_source_limits{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2LifecycleKind::Count)>
		phase2_lifecycle_events{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2SupportKind::Count)>
		phase2_support_transitions{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2SupportCoalescedKind::Count)>
		phase2_support_coalesced{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2Ring::Count)>
		phase2_ring_overflows{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2KeyframeReason::Count)>
		phase2_forced_keyframes{};
	TelemetryHistogramSnapshot phase2_manifest_duration{};
	TelemetryHistogramSnapshot phase2_image_duration{};
	std::uint64_t phase2_manifest_rebuild_coalesced = 0U;
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2MemoryScope::Count)>
		phase2_memory_bytes{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2MemoryScope::Count)>
		phase2_memory_high_water{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryMetricCounter::Count)> mission_counters{};
	std::array<TelemetryHistogramSnapshot, static_cast<std::size_t>(TelemetryMetricHistogram::Count)> process_histograms{};
	std::array<TelemetryHistogramSnapshot, static_cast<std::size_t>(TelemetryMetricHistogram::Count)> mission_histograms{};
	std::array<TelemetryHistogramSnapshot, static_cast<std::size_t>(TelemetryPhase2Block::Count)>
		mission_phase2_capture_duration{};
	std::array<std::array<std::uint64_t,
			static_cast<std::size_t>(TelemetryPhase2CaptureFailure::Count)>,
		static_cast<std::size_t>(TelemetryPhase2Block::Count)>
		mission_phase2_capture_failures{};
	std::array<std::array<std::uint64_t,
			static_cast<std::size_t>(TelemetryPhase3CaptureFailure::Count)>,
		static_cast<std::size_t>(TelemetryPhase3Block::Count)>
		mission_phase3_capture_failures{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2ClosureResult::Count)>
		mission_phase2_closure_results{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2LifecycleKind::Count)>
		mission_phase2_lifecycle_events{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2SupportKind::Count)>
		mission_phase2_support_transitions{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2SupportCoalescedKind::Count)>
		mission_phase2_support_coalesced{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2SourceLimit::Count)>
		mission_phase2_source_limits{};
	TelemetryHistogramSnapshot mission_phase2_image_duration{};
	std::uint64_t phase2_closure_classes = 0U;
	std::uint64_t phase2_closure_ships = 0U;
	std::uint64_t phase2_closure_weapons = 0U;
	std::uint64_t phase2_closure_subsystems = 0U;
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2Ring::Count)>
		phase2_ring_depth{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryPhase2Ring::Count)>
		phase2_ring_high_water{};
	std::array<TelemetrySessionMetricsSnapshot, TelemetryMetricsMaxClients> sessions{};
	std::uint64_t allocated_bytes = 0U;
	std::uint64_t allocated_bytes_high_water = 0U;
	std::uint64_t udp_sockets_open = 0U;
	std::uint64_t clients_active = 0U;
	std::uint64_t clients_active_high_water = 0U;
	std::uint64_t current_player_entity_id = 0U;
};

class TelemetryMetrics final {
  public:
	static constexpr std::size_t StorageBytes = sizeof(TelemetryMetricsSnapshot);

	bool provision() noexcept;
	void release() noexcept;
	bool is_provisioned() const noexcept { return m_snapshot.provisioned; }
	std::size_t owned_bytes() const noexcept { return m_snapshot.provisioned ? StorageBytes : 0U; }
	TelemetryMetricsSnapshot snapshot() const noexcept { return m_snapshot; }

	void reset_mission() noexcept;
	void reset_session(std::size_t slot) noexcept;
	void activate_session(std::size_t slot, std::uint64_t state) noexcept;
	void deactivate_session(std::size_t slot) noexcept;
	void set_runtime_state(std::uint64_t state) noexcept;
	void set_allocated_bytes(std::uint64_t bytes) noexcept;
	void set_udp_sockets_open(std::uint64_t sockets) noexcept;
	void set_current_player_entity_id(std::uint64_t entity_id) noexcept;
	std::uint64_t process_counter(TelemetryMetricCounter counter) const noexcept;
	std::uint64_t phase2_memory_high_water(TelemetryPhase2MemoryScope scope) const noexcept;
	void set_session_gauges(std::size_t slot, std::uint64_t state, std::uint64_t reassemblies,
		std::uint64_t reassembly_bytes, std::uint64_t reliable_items,
		std::uint64_t snapshot_candidates, std::uint64_t baselines) noexcept;
	void set_pending_items(std::size_t slot, TelemetryPendingKind kind, std::uint64_t value) noexcept;
	void record_datagram(TelemetryDirection direction, TelemetryIoResult result, std::uint64_t bytes) noexcept;
	void record_capture_result(TelemetryCaptureResult result) noexcept;
	void record_session_end(TelemetrySessionEndReason reason) noexcept;
	void record_runtime_fault(TelemetryRuntimeFaultReason reason) noexcept;
	void record_phase2_profile_rejection(TelemetryPhase2ProfileRejection reason) noexcept;
	void set_phase2_profile(std::size_t slot, TelemetryPhase2Profile profile) noexcept;
	void observe_phase2_capture(TelemetryPhase2Block block,
		std::uint64_t duration_us) noexcept;
	void record_phase2_capture_failure(TelemetryPhase2Block block,
		TelemetryPhase2CaptureFailure reason) noexcept;
	void record_phase3_capture_failure(TelemetryPhase3Block block,
		TelemetryPhase3CaptureFailure reason) noexcept;
	void record_phase2_closure(TelemetryPhase2ClosureResult result) noexcept;
	void set_phase2_closure(std::uint64_t classes, std::uint64_t ships,
		std::uint64_t weapons, std::uint64_t subsystems) noexcept;
	void record_phase2_manifest(std::size_t slot,
		TelemetryPhase2ManifestResult result,
		std::uint64_t bytes, std::uint64_t parts,
		std::uint64_t duration_us) noexcept;
	void observe_phase2_image(std::uint64_t duration_us) noexcept;
	void record_phase2_lifecycle(TelemetryPhase2LifecycleKind kind) noexcept;
	void record_phase2_support(TelemetryPhase2SupportKind kind) noexcept;
	void record_phase2_support_coalesced(std::size_t slot,
		TelemetryPhase2SupportCoalescedKind kind) noexcept;
	void set_phase2_ring(TelemetryPhase2Ring ring, std::uint64_t depth) noexcept;
	void record_phase2_ring_overflow(TelemetryPhase2Ring ring) noexcept;
	void set_phase2_support_latches(std::size_t slot, std::uint64_t count) noexcept;
	void record_phase2_forced_keyframe(std::size_t slot,
		TelemetryPhase2KeyframeReason reason) noexcept;
	void set_phase2_sample_age(std::size_t slot, TelemetryPhase2Block block,
		std::uint64_t age_us) noexcept;
	void set_phase2_manifest_generations(std::size_t slot,
		std::uint64_t generations) noexcept;
	void record_phase2_manifest_rebuild_coalesced(std::size_t slot) noexcept;
	void record_phase2_source_limit(TelemetryPhase2SourceLimit limit) noexcept;
	void record_phase2_allocation_after_ready(TelemetryPhase2AllocationKind kind) noexcept;
	void set_phase2_memory(TelemetryPhase2MemoryScope scope, std::uint64_t bytes) noexcept;
	void set_phase2_session_state(std::size_t slot,
		std::uint64_t image_records, std::uint64_t image_bytes,
		std::uint64_t dirty_atoms,
		std::uint64_t manifest_generations) noexcept;

	void record_callback(TelemetryCallbackKind kind, std::uint64_t duration_us) noexcept;
	void increment_process(TelemetryMetricCounter counter, std::uint64_t amount = 1U) noexcept;
	void increment_session(std::size_t slot, TelemetryMetricCounter counter, std::uint64_t amount = 1U) noexcept;
	void increment_mission(TelemetryMetricCounter counter, std::uint64_t amount = 1U) noexcept;
	void observe_process(TelemetryMetricHistogram histogram, std::uint64_t duration_us) noexcept;
	void observe_session(std::size_t slot, TelemetryMetricHistogram histogram, std::uint64_t duration_us) noexcept;
	void observe_mission(TelemetryMetricHistogram histogram, std::uint64_t duration_us) noexcept;

  private:
	static bool valid_slot(std::size_t slot) noexcept { return slot < TelemetryMetricsMaxClients; }
	static std::size_t histogram_bucket(std::uint64_t duration_us) noexcept;
	void saturating_add(std::uint64_t& value, std::uint64_t amount) noexcept;
	void observe(TelemetryHistogramSnapshot& histogram, std::uint64_t duration_us) noexcept;
	void refresh_clients_high_water() noexcept;

	TelemetryMetricsSnapshot m_snapshot{};
};

static_assert(std::is_trivially_copyable_v<TelemetryMetricsSnapshot>,
	"Metrics snapshots must remain a bounded value object");
static_assert(sizeof(TelemetryMetrics) == TelemetryMetrics::StorageBytes,
	"The startup budget must price the exact fixed Metrics representation");

} // namespace telemetry::detail
