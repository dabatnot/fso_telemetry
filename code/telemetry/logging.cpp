#include "telemetry/logging.h"

namespace telemetry::detail {
namespace {
constexpr std::uint64_t DropSummaryPeriodUs = 1'000'000U;

bool valid_reason(TelemetryLogReason reason) noexcept
{
	return static_cast<std::size_t>(reason) < static_cast<std::size_t>(TelemetryLogReason::Count);
}
} // namespace

void TelemetryStructuredLog::append(TelemetryLogRecord record) noexcept
{
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
