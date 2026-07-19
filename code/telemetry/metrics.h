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
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryMetricCounter::Count)> mission_counters{};
	std::array<TelemetryHistogramSnapshot, static_cast<std::size_t>(TelemetryMetricHistogram::Count)> process_histograms{};
	std::array<TelemetryHistogramSnapshot, static_cast<std::size_t>(TelemetryMetricHistogram::Count)> mission_histograms{};
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
	void set_session_gauges(std::size_t slot, std::uint64_t state, std::uint64_t reassemblies,
		std::uint64_t reassembly_bytes, std::uint64_t reliable_items,
		std::uint64_t snapshot_candidates, std::uint64_t baselines) noexcept;
	void set_pending_items(std::size_t slot, TelemetryPendingKind kind, std::uint64_t value) noexcept;
	void record_datagram(TelemetryDirection direction, TelemetryIoResult result, std::uint64_t bytes) noexcept;
	void record_capture_result(TelemetryCaptureResult result) noexcept;
	void record_session_end(TelemetrySessionEndReason reason) noexcept;
	void record_runtime_fault(TelemetryRuntimeFaultReason reason) noexcept;

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
