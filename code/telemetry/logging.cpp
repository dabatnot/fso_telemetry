#include "telemetry/logging.h"

#include <cstdio>

namespace telemetry::detail {
namespace {
constexpr std::uint64_t DropSummaryPeriodUs = 1'000'000U;

bool valid_reason(TelemetryLogReason reason) noexcept
{
	return static_cast<std::size_t>(reason) < static_cast<std::size_t>(TelemetryLogReason::Count);
}
} // namespace

bool format_telemetry_log_record(
	const TelemetryLogRecord& record,
	std::array<char, TelemetryLogLineCapacity>& output) noexcept
{
	const auto written = std::snprintf(output.data(), output.size(),
		"telemetry event=%u level=%u reason=%u family=%u budget=%u fault=%u "
		"slot=%u port=%u version=%u.%u code=%u "
		"p2_profile=%u p2_profile_rejection=%u p2_block=%u "
		"p2_capture_failure=%u p3_block=%u p3_capture_failure=%u "
		"p2_lifecycle=%u p2_support=%u p2_resync=%u "
		"generation=%u records=%u parts=%u bytes=%llu duration_us=%llu "
		"value=%llu limit=%llu high_water=%llu "
		"drops=%llu,%llu,%llu,%llu,%llu,%llu\n",
		static_cast<unsigned>(record.event),
		static_cast<unsigned>(record.level),
		static_cast<unsigned>(record.reason),
		static_cast<unsigned>(record.family),
		static_cast<unsigned>(record.budget),
		static_cast<unsigned>(record.fault),
		static_cast<unsigned>(record.correlation_slot),
		static_cast<unsigned>(record.port),
		static_cast<unsigned>(record.protocol_major),
		static_cast<unsigned>(record.protocol_minor),
		static_cast<unsigned>(record.platform_code),
		static_cast<unsigned>(record.phase2_profile),
		static_cast<unsigned>(record.phase2_profile_rejection),
		static_cast<unsigned>(record.phase2_block),
		static_cast<unsigned>(record.phase2_capture_failure),
		static_cast<unsigned>(record.phase3_block),
		static_cast<unsigned>(record.phase3_capture_failure),
		static_cast<unsigned>(record.phase2_lifecycle),
		static_cast<unsigned>(record.phase2_support),
		static_cast<unsigned>(record.phase2_resync),
		static_cast<unsigned>(record.local_generation),
		static_cast<unsigned>(record.record_count),
		static_cast<unsigned>(record.part_count),
		static_cast<unsigned long long>(record.bytes),
		static_cast<unsigned long long>(record.duration_us),
		static_cast<unsigned long long>(record.value),
		static_cast<unsigned long long>(record.limit),
		static_cast<unsigned long long>(record.high_water),
		static_cast<unsigned long long>(record.drops[0]),
		static_cast<unsigned long long>(record.drops[1]),
		static_cast<unsigned long long>(record.drops[2]),
		static_cast<unsigned long long>(record.drops[3]),
		static_cast<unsigned long long>(record.drops[4]),
		static_cast<unsigned long long>(record.drops[5]));
	return written >= 0 && static_cast<std::size_t>(written) < output.size();
}

void TelemetryStructuredLog::append(TelemetryLogRecord record) noexcept
{
	if (record.level == TelemetryLogLevel::Error) {
		if (m_snapshot.terminal_count <
				m_snapshot.terminal_records.size()) {
			m_snapshot.terminal_records[
				m_snapshot.terminal_count++] = record;
		} else {
			for (std::size_t index = 1U;
				 index < m_snapshot.terminal_records.size(); ++index) {
				m_snapshot.terminal_records[index - 1U] =
					m_snapshot.terminal_records[index];
			}
			m_snapshot.terminal_records.back() = record;
			if (m_snapshot.superseded_terminal_records != UINT64_MAX)
				++m_snapshot.superseded_terminal_records;
		}
	}
	if (m_snapshot.count >= m_snapshot.records.size()) {
		++m_snapshot.dropped_records;
		return;
	}
	m_snapshot.records[m_snapshot.count++] = record;
}

void TelemetryStructuredLog::config_absent() noexcept
{
	if (m_startup_logged) return;
	m_startup_logged = true;
	append({TelemetryLogEvent::ConfigAbsent, TelemetryLogLevel::Info});
}
void TelemetryStructuredLog::config_disabled() noexcept
{
	if (m_startup_logged) return;
	m_startup_logged = true;
	append({TelemetryLogEvent::ConfigDisabled, TelemetryLogLevel::Info});
}
void TelemetryStructuredLog::config_invalid(TelemetryLogReason reason) noexcept
{
	if (m_startup_logged) return;
	m_startup_logged = true;
	TelemetryLogRecord record{TelemetryLogEvent::ConfigInvalid, TelemetryLogLevel::Warning};
	record.reason = valid_reason(reason) ? reason : TelemetryLogReason::ConfigSchema;
	append(record);
}
void TelemetryStructuredLog::activated(TelemetryLogFamily family, std::uint16_t port,
	std::uint64_t effective_limit) noexcept
{
	if (m_activation_logged) return;
	m_activation_logged = true;
	TelemetryLogRecord record{TelemetryLogEvent::Activated, TelemetryLogLevel::Info};
	record.family = family;
	record.port = port;
	record.limit = effective_limit;
	record.protocol_major = 1U;
	record.protocol_minor = 1U;
	append(record);
}
void TelemetryStructuredLog::transport_fault(TelemetryLogFamily family, TelemetryLogReason operation,
	std::uint32_t normalized_platform_code, TelemetryLogFault fault) noexcept
{
	if (m_fault_logged) return;
	m_fault_logged = true;
	TelemetryLogRecord record{TelemetryLogEvent::TransportFault, TelemetryLogLevel::Error};
	record.family = family;
	record.reason = valid_reason(operation) ? operation : TelemetryLogReason::Bind;
	record.platform_code = normalized_platform_code;
	record.fault = static_cast<std::size_t>(fault) < static_cast<std::size_t>(TelemetryLogFault::Count) ?
		fault : TelemetryLogFault::None;
	append(record);
}
void TelemetryStructuredLog::session_opened(std::size_t slot) noexcept
{
	if (slot >= m_session_open.size() || m_session_open[slot]) return;
	m_session_open[slot] = true;
	TelemetryLogRecord record{TelemetryLogEvent::SessionOpened, TelemetryLogLevel::Info};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	append(record);
}
void TelemetryStructuredLog::session_closed(std::size_t slot, TelemetryLogReason reason,
	std::uint64_t duration_us, std::uint64_t aggregate_total) noexcept
{
	if (slot >= m_session_open.size() || !m_session_open[slot]) return;
	m_session_open[slot] = false;
	TelemetryLogRecord record{TelemetryLogEvent::SessionClosed, TelemetryLogLevel::Info};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	record.reason = valid_reason(reason) ? reason : TelemetryLogReason::ProtocolError;
	record.value = duration_us;
	record.limit = aggregate_total;
	append(record);
	// A budget warning is emitted on its first observation.  The corresponding
	// session close carries the bounded summary, never a per-frame update.
	for (std::size_t index = 0U; index < m_budget_warned.size(); ++index) {
		if (!m_budget_warned[index]) continue;
		TelemetryLogRecord summary{TelemetryLogEvent::BudgetSessionSummary, TelemetryLogLevel::Warning};
		summary.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
		summary.budget = static_cast<TelemetryLogBudget>(index);
		summary.limit = m_budget_limits[index];
		summary.high_water = m_budget_high_waters[index];
		append(summary);
	}
}
void TelemetryStructuredLog::mission_entered(std::uint64_t mission_generation) noexcept
{
	TelemetryLogRecord record{TelemetryLogEvent::MissionEntered, TelemetryLogLevel::Info};
	record.value = mission_generation;
	append(record);
}
void TelemetryStructuredLog::mission_left(std::uint64_t mission_generation, std::uint64_t duration_us,
	std::uint64_t aggregate_total) noexcept
{
	TelemetryLogRecord record{TelemetryLogEvent::MissionLeft, TelemetryLogLevel::Info};
	record.value = mission_generation;
	record.limit = duration_us;
	record.high_water = aggregate_total;
	append(record);
}
void TelemetryStructuredLog::record_drop(TelemetryLogDrop reason) noexcept
{
	const auto index = static_cast<std::size_t>(reason);
	if (index < m_pending_drops.size() && m_pending_drops[index] != UINT64_MAX) ++m_pending_drops[index];
}
void TelemetryStructuredLog::flush_drop_summary(std::uint64_t now_us) noexcept
{
	if (m_drop_summary_emitted &&
		(now_us < m_last_drop_summary_us || now_us - m_last_drop_summary_us < DropSummaryPeriodUs)) return;
	bool any = false;
	for (const auto value : m_pending_drops) any = any || value != 0U;
	if (!any) return;
	TelemetryLogRecord record{TelemetryLogEvent::DropSummary, TelemetryLogLevel::Warning};
	record.drops = m_pending_drops;
	append(record);
	m_pending_drops = {};
	m_last_drop_summary_us = now_us;
	m_drop_summary_emitted = true;
}
void TelemetryStructuredLog::budget_high_water(TelemetryLogBudget budget, std::uint64_t limit,
	std::uint64_t high_water) noexcept
{
	const auto index = static_cast<std::size_t>(budget);
	if (index >= m_budget_warned.size()) return;
	m_budget_limits[index] = limit;
	if (high_water > m_budget_high_waters[index]) m_budget_high_waters[index] = high_water;
	if (m_budget_warned[index]) return;
	m_budget_warned[index] = true;
	TelemetryLogRecord record{TelemetryLogEvent::BudgetHighWater, TelemetryLogLevel::Warning};
	record.budget = budget;
	record.limit = limit;
	record.high_water = high_water;
	append(record);
}

void TelemetryStructuredLog::phase2_profile_selected(std::size_t slot,
	TelemetryPhase2Profile profile,
	std::uint64_t capability_mask) noexcept
{
	if (slot >= m_phase2_profile_logged.size() ||
		m_phase2_profile_logged[slot] ||
		static_cast<std::size_t>(profile) >=
			static_cast<std::size_t>(TelemetryPhase2Profile::Count))
		return;
	m_phase2_profile_logged[slot] = true;
	TelemetryLogRecord record{TelemetryLogEvent::Phase2ProfileSelected,
		TelemetryLogLevel::Info};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	record.phase2_profile = profile;
	record.value = capability_mask;
	append(record);
}

void TelemetryStructuredLog::phase2_profile_rejected(
	TelemetryPhase2ProfileRejection reason,
	std::uint64_t capability_mask) noexcept
{
	if (static_cast<std::size_t>(reason) >=
		static_cast<std::size_t>(
			TelemetryPhase2ProfileRejection::Count))
		return;
	TelemetryLogRecord record{TelemetryLogEvent::Phase2ProfileRejected,
		TelemetryLogLevel::Warning};
	record.phase2_profile_rejection = reason;
	record.value = capability_mask;
	append(record);
}

void TelemetryStructuredLog::phase2_manifest(std::size_t slot,
	TelemetryLogEvent event, std::uint32_t local_generation,
	std::uint32_t records, std::uint16_t parts,
	std::uint64_t bytes, std::uint64_t duration_us) noexcept
{
	if (slot >= m_phase2_manifest_logged.size() ||
		(event != TelemetryLogEvent::Phase2ManifestBuilt &&
		 event != TelemetryLogEvent::Phase2ManifestInstalled &&
		 event != TelemetryLogEvent::Phase2ManifestRejected))
		return;
	if (event != TelemetryLogEvent::Phase2ManifestRejected &&
		m_phase2_manifest_logged[slot] == local_generation)
		return;
	if (event != TelemetryLogEvent::Phase2ManifestRejected)
		m_phase2_manifest_logged[slot] = local_generation;
	TelemetryLogRecord record{event,
		event == TelemetryLogEvent::Phase2ManifestRejected
			? TelemetryLogLevel::Warning
			: TelemetryLogLevel::Info};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	record.local_generation = local_generation;
	record.record_count = records;
	record.part_count = parts;
	record.bytes = bytes;
	record.duration_us = duration_us;
	append(record);
}

void TelemetryStructuredLog::phase2_lifecycle(std::size_t slot,
	TelemetryPhase2LifecycleKind kind) noexcept
{
	if (slot >= 4U || static_cast<std::size_t>(kind) >=
		static_cast<std::size_t>(TelemetryPhase2LifecycleKind::Count))
		return;
	TelemetryLogRecord record{TelemetryLogEvent::Phase2Lifecycle,
		TelemetryLogLevel::Info};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	record.phase2_lifecycle = kind;
	append(record);
}

void TelemetryStructuredLog::phase2_support_terminal(std::size_t slot,
	TelemetryPhase2SupportKind kind, bool keyframe_forced) noexcept
{
	if (slot >= 4U || static_cast<std::size_t>(kind) >=
		static_cast<std::size_t>(TelemetryPhase2SupportKind::Count))
		return;
	TelemetryLogRecord record{TelemetryLogEvent::Phase2SupportTerminal,
		TelemetryLogLevel::Debug};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	record.phase2_support = kind;
	record.value = keyframe_forced ? 1U : 0U;
	append(record);
}

void TelemetryStructuredLog::phase2_resync(
	std::size_t slot, TelemetryLogPhase2ResyncResult result) noexcept
{
	if (slot >= 4U || static_cast<std::size_t>(result) >=
		static_cast<std::size_t>(
			TelemetryLogPhase2ResyncResult::Count))
		return;
	TelemetryLogRecord record{TelemetryLogEvent::Phase2Resync,
		TelemetryLogLevel::Info};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	record.phase2_resync = result;
	append(record);
}

void TelemetryStructuredLog::phase2_source_rejected(
	TelemetryPhase2Block block, TelemetryPhase2CaptureFailure reason,
	std::uint64_t now_us) noexcept
{
	if (static_cast<std::size_t>(block) >=
			static_cast<std::size_t>(TelemetryPhase2Block::Count) ||
		static_cast<std::size_t>(reason) >=
			static_cast<std::size_t>(
				TelemetryPhase2CaptureFailure::Count))
		return;
	if (m_phase2_source_rejections != UINT64_MAX)
		++m_phase2_source_rejections;
	if (m_phase2_last_source_log_us != 0U &&
		(now_us < m_phase2_last_source_log_us ||
		 now_us - m_phase2_last_source_log_us < DropSummaryPeriodUs))
		return;
	TelemetryLogRecord record{TelemetryLogEvent::Phase2SourceRejected,
		TelemetryLogLevel::Warning};
	record.phase2_block = block;
	record.phase2_capture_failure = reason;
	record.value = m_phase2_source_rejections;
	append(record);
	m_phase2_source_rejections = 0U;
	m_phase2_last_source_log_us = now_us;
}

void TelemetryStructuredLog::phase2_summary(std::size_t slot,
	std::uint64_t aggregate_events,
	std::uint64_t high_water_bytes) noexcept
{
	if (slot >= 4U) return;
	TelemetryLogRecord record{TelemetryLogEvent::Phase2Summary,
		TelemetryLogLevel::Info};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	record.value = aggregate_events;
	record.high_water = high_water_bytes;
	append(record);
}

void TelemetryStructuredLog::phase3_source_rejected(
	std::size_t slot, TelemetryPhase3Block block,
	TelemetryPhase3CaptureFailure reason) noexcept
{
	if (slot >= 4U || static_cast<std::size_t>(block) >=
			static_cast<std::size_t>(TelemetryPhase3Block::Count) ||
		static_cast<std::size_t>(reason) >=
			static_cast<std::size_t>(TelemetryPhase3CaptureFailure::Count))
		return;
	TelemetryLogRecord record{TelemetryLogEvent::Phase3SourceRejected,
		TelemetryLogLevel::Error};
	record.correlation_slot = static_cast<std::uint8_t>(slot + 1U);
	record.phase3_block = block;
	record.phase3_capture_failure = reason;
	append(record);
}

void TelemetryStructuredLog::shutdown(std::uint64_t duration_us, std::uint64_t aggregate_total) noexcept
{
	if (m_shutdown_logged) return;
	m_shutdown_logged = true;
	TelemetryLogRecord record{TelemetryLogEvent::Shutdown, TelemetryLogLevel::Info};
	record.value = duration_us;
	record.limit = aggregate_total;
	append(record);
}

} // namespace telemetry::detail
