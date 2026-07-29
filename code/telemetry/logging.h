#pragma once

#include "telemetry/metrics.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

// P9.2 deliberately exposes no text, endpoint, identifier, payload, or OS
// message fields.  Every value that can reach a delivery log is a closed enum
// or a bounded numeric diagnostic selected by the producer.
enum class TelemetryLogLevel : std::uint8_t { Debug = 0, Info, Warning, Error };
enum class TelemetryLogEvent : std::uint8_t {
	ConfigAbsent = 0,
	ConfigDisabled,
	ConfigInvalid,
	Activated,
	TransportFault,
	SessionOpened,
	SessionClosed,
	MissionEntered,
	MissionLeft,
	DropSummary,
	BudgetHighWater,
	BudgetSessionSummary,
	Phase2ProfileSelected,
	Phase2ProfileRejected,
	Phase2ManifestBuilt,
	Phase2ManifestInstalled,
	Phase2ManifestRejected,
	Phase2Lifecycle,
	Phase2SupportTerminal,
	Phase2Resync,
	Phase2SourceRejected,
	Phase2Summary,
	Shutdown,
	Count,
};
enum class TelemetryLogReason : std::uint8_t {
	None = 0,
	ConfigSchema,
	ConfigRange,
	ConfigSecurity,
	ConfigProfile,
	Bind,
	Receive,
	Send,
	Timeout,
	MissionDiscontinuity,
	PeerClosed,
	ProtocolError,
	TransportError,
	Shutdown,
	Count,
};
enum class TelemetryLogFamily : std::uint8_t { None = 0, Ipv4, Ipv6, DualStack };
enum class TelemetryLogBudget : std::uint8_t {
	Startup = 0,
	SessionSlots,
	ReliableWindow,
	ReassemblyBytes,
	StateImage,
	Metrics,
	Count,
};
enum class TelemetryLogFault : std::uint8_t {
	None = 0, Config, Entropy, IdentityStore, BudgetOverflow, Allocation,
	SessionRegistration, Socket, MainThread, Lifecycle, Capture, Count,
};
enum class TelemetryLogDrop : std::uint8_t {
	Validation = 0,
	Allowlist,
	RateLimit,
	AntiAmplification,
	WouldBlock,
	Budget,
	Count,
};
enum class TelemetryLogPhase2ResyncResult : std::uint8_t {
	AcceptedNewCandidate = 0,
	AcceptedCoalesced,
	Count,
};

struct TelemetryLogRecord {
	TelemetryLogEvent event = TelemetryLogEvent::ConfigAbsent;
	TelemetryLogLevel level = TelemetryLogLevel::Info;
	TelemetryLogReason reason = TelemetryLogReason::None;
	TelemetryLogFamily family = TelemetryLogFamily::None;
	TelemetryLogBudget budget = TelemetryLogBudget::Startup;
	TelemetryLogFault fault = TelemetryLogFault::None;
	std::uint8_t correlation_slot = 0U; // local ordinal 1..4, never session_id
	std::uint16_t port = 0U;
	std::uint8_t protocol_major = 0U;
	std::uint8_t protocol_minor = 0U;
	std::uint32_t platform_code = 0U; // normalized, never an OS message
	std::uint64_t value = 0U;
	std::uint64_t limit = 0U;
	std::uint64_t high_water = 0U;
	TelemetryPhase2Profile phase2_profile = TelemetryPhase2Profile::None;
	TelemetryPhase2ProfileRejection phase2_profile_rejection =
		TelemetryPhase2ProfileRejection::UnsupportedAuthority;
	TelemetryPhase2Block phase2_block = TelemetryPhase2Block::Identity;
	TelemetryPhase2CaptureFailure phase2_capture_failure =
		TelemetryPhase2CaptureFailure::Guard;
	TelemetryPhase2LifecycleKind phase2_lifecycle =
		TelemetryPhase2LifecycleKind::Appeared;
	TelemetryPhase2SupportKind phase2_support =
		TelemetryPhase2SupportKind::Requested;
	TelemetryLogPhase2ResyncResult phase2_resync =
		TelemetryLogPhase2ResyncResult::AcceptedNewCandidate;
	std::uint32_t local_generation = 0U;
	std::uint32_t record_count = 0U;
	std::uint16_t part_count = 0U;
	std::uint64_t bytes = 0U;
	std::uint64_t duration_us = 0U;
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryLogDrop::Count)> drops{};
};

constexpr std::size_t TelemetryLogRecordCapacity = 32U;

struct TelemetryLogSnapshot {
	std::array<TelemetryLogRecord, TelemetryLogRecordCapacity> records{};
	std::size_t count = 0U;
	std::uint64_t dropped_records = 0U;
};

class TelemetryStructuredLog final {
  public:
	TelemetryLogSnapshot snapshot() const noexcept { return m_snapshot; }

	void config_absent() noexcept;
	void config_disabled() noexcept;
	void config_invalid(TelemetryLogReason reason) noexcept;
	void activated(TelemetryLogFamily family, std::uint16_t port, std::uint64_t effective_limit) noexcept;
	void transport_fault(TelemetryLogFamily family, TelemetryLogReason operation,
		std::uint32_t normalized_platform_code, TelemetryLogFault fault = TelemetryLogFault::Socket) noexcept;
	void session_opened(std::size_t slot) noexcept;
	void session_closed(std::size_t slot, TelemetryLogReason reason, std::uint64_t duration_us,
		std::uint64_t aggregate_total) noexcept;
	void mission_entered(std::uint64_t mission_generation) noexcept;
	void mission_left(std::uint64_t mission_generation, std::uint64_t duration_us,
		std::uint64_t aggregate_total) noexcept;
	void record_drop(TelemetryLogDrop reason) noexcept;
	void flush_drop_summary(std::uint64_t now_us) noexcept;
	void budget_high_water(TelemetryLogBudget budget, std::uint64_t limit, std::uint64_t high_water) noexcept;
	void phase2_profile_selected(std::size_t slot,
		TelemetryPhase2Profile profile, std::uint64_t capability_mask) noexcept;
	void phase2_profile_rejected(
		TelemetryPhase2ProfileRejection reason,
		std::uint64_t capability_mask) noexcept;
	void phase2_manifest(std::size_t slot, TelemetryLogEvent event,
		std::uint32_t local_generation, std::uint32_t records,
		std::uint16_t parts, std::uint64_t bytes,
		std::uint64_t duration_us) noexcept;
	void phase2_lifecycle(std::size_t slot,
		TelemetryPhase2LifecycleKind kind) noexcept;
	void phase2_support_terminal(std::size_t slot,
		TelemetryPhase2SupportKind kind, bool keyframe_forced) noexcept;
	void phase2_resync(std::size_t slot,
		TelemetryLogPhase2ResyncResult result) noexcept;
	void phase2_source_rejected(TelemetryPhase2Block block,
		TelemetryPhase2CaptureFailure reason,
		std::uint64_t now_us) noexcept;
	void phase2_summary(std::size_t slot,
		std::uint64_t aggregate_events,
		std::uint64_t high_water_bytes) noexcept;
	void shutdown(std::uint64_t duration_us, std::uint64_t aggregate_total) noexcept;

  private:
	void append(TelemetryLogRecord record) noexcept;
	bool m_startup_logged = false;
	bool m_activation_logged = false;
	bool m_fault_logged = false;
	bool m_shutdown_logged = false;
	std::array<bool, 4U> m_session_open{};
	std::array<bool, static_cast<std::size_t>(TelemetryLogBudget::Count)> m_budget_warned{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryLogBudget::Count)> m_budget_limits{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryLogBudget::Count)> m_budget_high_waters{};
	std::array<std::uint64_t, static_cast<std::size_t>(TelemetryLogDrop::Count)> m_pending_drops{};
	std::uint64_t m_last_drop_summary_us = 0U;
	bool m_drop_summary_emitted = false;
	std::array<bool, 4U> m_phase2_profile_logged{};
	std::array<std::uint32_t, 4U> m_phase2_manifest_logged{};
	std::uint64_t m_phase2_last_source_log_us = 0U;
	std::uint64_t m_phase2_source_rejections = 0U;
	TelemetryLogSnapshot m_snapshot{};
};

} // namespace telemetry::detail
