#include "telemetry/metrics.h"

#include <limits>

namespace telemetry::detail {

bool TelemetryMetrics::provision() noexcept
{
	if (m_snapshot.provisioned) {
		return true;
	}
	m_snapshot = {};
	m_snapshot.provisioned = true;
	return true;
}

void TelemetryMetrics::release() noexcept
{
	m_snapshot = {};
}

void TelemetryMetrics::reset_mission() noexcept
{
	m_snapshot.mission_counters = {};
	m_snapshot.mission_histograms = {};
	m_snapshot.mission_phase2_capture_duration = {};
	m_snapshot.mission_phase2_capture_failures = {};
	m_snapshot.mission_phase2_closure_results = {};
	m_snapshot.mission_phase2_lifecycle_events = {};
	m_snapshot.mission_phase2_support_transitions = {};
	m_snapshot.mission_phase2_support_coalesced = {};
	m_snapshot.mission_phase2_source_limits = {};
	m_snapshot.mission_phase2_image_duration = {};
	m_snapshot.phase2_closure_classes = 0U;
	m_snapshot.phase2_closure_ships = 0U;
	m_snapshot.phase2_closure_weapons = 0U;
	m_snapshot.phase2_closure_subsystems = 0U;
	m_snapshot.phase2_ring_depth = {};
	m_snapshot.phase2_ring_high_water = {};
	m_snapshot.current_player_entity_id = 0U;
}

void TelemetryMetrics::reset_session(std::size_t slot) noexcept
{
	if (valid_slot(slot)) {
		m_snapshot.sessions[slot] = {};
		refresh_clients_high_water();
	}
}

void TelemetryMetrics::activate_session(std::size_t slot, std::uint64_t state) noexcept
{
	if (!valid_slot(slot)) {
		return;
	}
	m_snapshot.sessions[slot] = {};
	m_snapshot.sessions[slot].active = true;
	m_snapshot.sessions[slot].state = state;
	refresh_clients_high_water();
}

void TelemetryMetrics::deactivate_session(std::size_t slot) noexcept { reset_session(slot); }
void TelemetryMetrics::set_runtime_state(std::uint64_t state) noexcept { m_snapshot.runtime_state = state; }

void TelemetryMetrics::set_allocated_bytes(std::uint64_t bytes) noexcept
{
	m_snapshot.allocated_bytes = bytes;
	if (bytes > m_snapshot.allocated_bytes_high_water) {
		m_snapshot.allocated_bytes_high_water = bytes;
	}
}

void TelemetryMetrics::set_udp_sockets_open(std::uint64_t sockets) noexcept
{
	m_snapshot.udp_sockets_open = sockets > 2U ? 2U : sockets;
}
void TelemetryMetrics::set_current_player_entity_id(std::uint64_t entity_id) noexcept
{
	m_snapshot.current_player_entity_id = entity_id;
}

void TelemetryMetrics::set_session_gauges(std::size_t slot, std::uint64_t state, std::uint64_t reassemblies,
	std::uint64_t reassembly_bytes, std::uint64_t reliable_items, std::uint64_t snapshot_candidates,
	std::uint64_t baselines) noexcept
{
	if (!valid_slot(slot)) return;
	auto& metrics = m_snapshot.sessions[slot];
	metrics.state = state;
	metrics.reassemblies_active = reassemblies > 4U ? 4U : reassemblies;
	metrics.reassembly_bytes = reassembly_bytes;
	metrics.reliable_window_items = reliable_items;
	metrics.snapshot_candidates = snapshot_candidates > 1U ? 1U : snapshot_candidates;
	metrics.baselines_active = baselines > 1U ? 1U : baselines;
	if (metrics.reassemblies_active > metrics.reassemblies_active_high_water)
		metrics.reassemblies_active_high_water = metrics.reassemblies_active;
	if (metrics.reassembly_bytes > metrics.reassembly_bytes_high_water)
		metrics.reassembly_bytes_high_water = metrics.reassembly_bytes;
	if (metrics.reliable_window_items > metrics.reliable_window_items_high_water)
		metrics.reliable_window_items_high_water = metrics.reliable_window_items;
}

void TelemetryMetrics::set_pending_items(std::size_t slot, TelemetryPendingKind kind, std::uint64_t value) noexcept
{
	if (!valid_slot(slot) || static_cast<std::size_t>(kind) >= static_cast<std::size_t>(TelemetryPendingKind::Count)) return;
	m_snapshot.sessions[slot].pending_items[static_cast<std::size_t>(kind)] = value;
	auto& high_water = m_snapshot.sessions[slot].pending_items_high_water[static_cast<std::size_t>(kind)];
	if (value > high_water) high_water = value;
}

void TelemetryMetrics::record_datagram(TelemetryDirection direction, TelemetryIoResult result, std::uint64_t bytes) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto direction_index = static_cast<std::size_t>(direction);
	const auto result_index = static_cast<std::size_t>(result);
	if (direction_index >= m_snapshot.datagrams.size() || result_index >= m_snapshot.datagrams[direction_index].size()) return;
	saturating_add(m_snapshot.datagrams[direction_index][result_index], 1U);
	if (result == TelemetryIoResult::Complete) saturating_add(m_snapshot.datagram_bytes[direction_index], bytes);
	increment_process(TelemetryMetricCounter::Datagrams);
	if (result == TelemetryIoResult::WouldBlock) increment_process(TelemetryMetricCounter::WouldBlock);
}

void TelemetryMetrics::record_capture_result(TelemetryCaptureResult result) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(result);
	if (index < m_snapshot.capture_results.size()) saturating_add(m_snapshot.capture_results[index], 1U);
}

void TelemetryMetrics::record_session_end(TelemetrySessionEndReason reason) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(reason);
	if (index < m_snapshot.session_ends.size()) saturating_add(m_snapshot.session_ends[index], 1U);
}

void TelemetryMetrics::record_runtime_fault(TelemetryRuntimeFaultReason reason) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(reason);
	if (index < m_snapshot.runtime_faults.size()) saturating_add(m_snapshot.runtime_faults[index], 1U);
}

void TelemetryMetrics::record_phase2_profile_rejection(
	TelemetryPhase2ProfileRejection reason) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(reason);
	if (index < m_snapshot.phase2_profile_rejections.size())
		saturating_add(m_snapshot.phase2_profile_rejections[index], 1U);
}

void TelemetryMetrics::set_phase2_profile(
	std::size_t slot, TelemetryPhase2Profile profile) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot) ||
		static_cast<std::size_t>(profile) >=
			static_cast<std::size_t>(TelemetryPhase2Profile::Count))
		return;
	m_snapshot.sessions[slot].phase2_profile = profile;
}

void TelemetryMetrics::observe_phase2_capture(
	TelemetryPhase2Block block, std::uint64_t duration_us) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(block);
	if (index >= m_snapshot.phase2_capture_duration.size()) return;
	observe(m_snapshot.phase2_capture_duration[index], duration_us);
	observe(m_snapshot.mission_phase2_capture_duration[index],
		duration_us);
}

void TelemetryMetrics::record_phase2_capture_failure(
	TelemetryPhase2Block block,
	TelemetryPhase2CaptureFailure reason) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto block_index = static_cast<std::size_t>(block);
	const auto reason_index = static_cast<std::size_t>(reason);
	if (block_index >= m_snapshot.phase2_capture_failures.size() ||
		reason_index >=
			m_snapshot.phase2_capture_failures[block_index].size())
		return;
	saturating_add(
		m_snapshot.phase2_capture_failures[block_index][reason_index],
		1U);
	saturating_add(
		m_snapshot.mission_phase2_capture_failures
			[block_index][reason_index],
		1U);
}

void TelemetryMetrics::record_phase2_closure(
	TelemetryPhase2ClosureResult result) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(result);
	if (index < m_snapshot.phase2_closure_results.size()) {
		saturating_add(m_snapshot.phase2_closure_results[index], 1U);
		saturating_add(
			m_snapshot.mission_phase2_closure_results[index], 1U);
	}
}

void TelemetryMetrics::set_phase2_closure(std::uint64_t classes,
	std::uint64_t ships, std::uint64_t weapons,
	std::uint64_t subsystems) noexcept
{
	if (!m_snapshot.provisioned) return;
	m_snapshot.phase2_closure_classes = classes;
	m_snapshot.phase2_closure_ships = ships > 64U ? 64U : ships;
	m_snapshot.phase2_closure_weapons = weapons;
	m_snapshot.phase2_closure_subsystems =
		subsystems > 4096U ? 4096U : subsystems;
}

void TelemetryMetrics::record_phase2_manifest(std::size_t slot,
	TelemetryPhase2ManifestResult result, std::uint64_t bytes,
	std::uint64_t parts, std::uint64_t duration_us) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	const auto index = static_cast<std::size_t>(result);
	if (index < m_snapshot.phase2_manifest_results.size()) {
		saturating_add(m_snapshot.phase2_manifest_results[index], 1U);
		saturating_add(
			m_snapshot.sessions[slot].phase2_manifest_builds[index],
			1U);
	}
	auto& session = m_snapshot.sessions[slot];
	session.phase2_manifest_bytes = bytes;
	session.phase2_manifest_parts = parts > 64U ? 64U : parts;
	observe(m_snapshot.phase2_manifest_duration, duration_us);
	observe(session.phase2_manifest_duration, duration_us);
}

void TelemetryMetrics::observe_phase2_image(
	std::uint64_t duration_us) noexcept
{
	if (!m_snapshot.provisioned) return;
	observe(m_snapshot.phase2_image_duration, duration_us);
	observe(m_snapshot.mission_phase2_image_duration, duration_us);
}

void TelemetryMetrics::record_phase2_lifecycle(
	TelemetryPhase2LifecycleKind kind) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(kind);
	if (index >= m_snapshot.phase2_lifecycle_events.size()) return;
	saturating_add(m_snapshot.phase2_lifecycle_events[index], 1U);
	saturating_add(
		m_snapshot.mission_phase2_lifecycle_events[index], 1U);
}

void TelemetryMetrics::record_phase2_support(
	TelemetryPhase2SupportKind kind) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(kind);
	if (index >= m_snapshot.phase2_support_transitions.size()) return;
	saturating_add(m_snapshot.phase2_support_transitions[index], 1U);
	saturating_add(
		m_snapshot.mission_phase2_support_transitions[index], 1U);
}

void TelemetryMetrics::record_phase2_support_coalesced(std::size_t slot,
	TelemetryPhase2SupportCoalescedKind kind) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	const auto index = static_cast<std::size_t>(kind);
	if (index >= m_snapshot.phase2_support_coalesced.size()) return;
	saturating_add(m_snapshot.phase2_support_coalesced[index], 1U);
	saturating_add(
		m_snapshot.mission_phase2_support_coalesced[index], 1U);
}

void TelemetryMetrics::set_phase2_ring(
	TelemetryPhase2Ring ring, std::uint64_t depth) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(ring);
	if (index >= m_snapshot.phase2_ring_depth.size()) return;
	const auto bounded = depth > 64U ? 64U : depth;
	m_snapshot.phase2_ring_depth[index] = bounded;
	auto& high = m_snapshot.phase2_ring_high_water[index];
	if (bounded > high) high = bounded;
}

void TelemetryMetrics::record_phase2_ring_overflow(
	TelemetryPhase2Ring ring) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(ring);
	if (index < m_snapshot.phase2_ring_overflows.size())
		saturating_add(m_snapshot.phase2_ring_overflows[index], 1U);
}

void TelemetryMetrics::set_phase2_support_latches(
	std::size_t slot, std::uint64_t count) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	auto& session = m_snapshot.sessions[slot];
	session.phase2_support_latches = count > 64U ? 64U : count;
	if (session.phase2_support_latches >
		session.phase2_support_latches_high_water)
		session.phase2_support_latches_high_water =
			session.phase2_support_latches;
}

void TelemetryMetrics::record_phase2_forced_keyframe(std::size_t slot,
	TelemetryPhase2KeyframeReason reason) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	const auto index = static_cast<std::size_t>(reason);
	if (index >= m_snapshot.phase2_forced_keyframes.size()) return;
	saturating_add(m_snapshot.phase2_forced_keyframes[index], 1U);
	saturating_add(
		m_snapshot.sessions[slot].phase2_forced_keyframes[index], 1U);
}

void TelemetryMetrics::set_phase2_sample_age(std::size_t slot,
	TelemetryPhase2Block block, std::uint64_t age_us) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	const auto index = static_cast<std::size_t>(block);
	if (index < m_snapshot.sessions[slot].phase2_sample_age_us.size())
		m_snapshot.sessions[slot].phase2_sample_age_us[index] = age_us;
}

void TelemetryMetrics::set_phase2_manifest_generations(
	std::size_t slot, std::uint64_t generations) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	m_snapshot.sessions[slot].phase2_manifest_generations =
		generations > 2U ? 2U : generations;
}

void TelemetryMetrics::record_phase2_manifest_rebuild_coalesced(
	std::size_t slot) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	saturating_add(
		m_snapshot.sessions[slot]
			.phase2_manifest_rebuild_coalesced, 1U);
	saturating_add(
		m_snapshot.phase2_manifest_rebuild_coalesced, 1U);
}

void TelemetryMetrics::record_phase2_source_limit(
	TelemetryPhase2SourceLimit limit) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(limit);
	if (index < m_snapshot.phase2_source_limits.size()) {
		saturating_add(m_snapshot.phase2_source_limits[index], 1U);
		saturating_add(
			m_snapshot.mission_phase2_source_limits[index], 1U);
	}
}

void TelemetryMetrics::record_phase2_allocation_after_ready(
	TelemetryPhase2AllocationKind kind) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(kind);
	if (index < m_snapshot.phase2_allocations_after_ready.size())
		saturating_add(m_snapshot.phase2_allocations_after_ready[index], 1U);
}

void TelemetryMetrics::set_phase2_memory(
	TelemetryPhase2MemoryScope scope, std::uint64_t bytes) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(scope);
	if (index >= m_snapshot.phase2_memory_bytes.size()) return;
	m_snapshot.phase2_memory_bytes[index] = bytes;
	auto& high_water = m_snapshot.phase2_memory_high_water[index];
	if (bytes > high_water) high_water = bytes;
}

void TelemetryMetrics::set_phase2_session_state(std::size_t slot,
	std::uint64_t image_records, std::uint64_t image_bytes,
	std::uint64_t dirty_atoms,
	std::uint64_t manifest_generations) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	auto& session = m_snapshot.sessions[slot];
	session.phase2_image_records = image_records;
	session.phase2_image_bytes = image_bytes;
	session.phase2_dirty_atoms = dirty_atoms;
	session.phase2_manifest_generations =
		manifest_generations > 2U ? 2U : manifest_generations;
}

void TelemetryMetrics::record_callback(TelemetryCallbackKind kind, std::uint64_t duration_us) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(kind);
	if (index >= m_snapshot.callbacks.size()) return;
	saturating_add(m_snapshot.callbacks[index], 1U);
	observe_process(TelemetryMetricHistogram::CallbackDuration, duration_us);
}

void TelemetryMetrics::increment_process(TelemetryMetricCounter counter, std::uint64_t amount) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(counter);
	if (index < m_snapshot.process_counters.size()) saturating_add(m_snapshot.process_counters[index], amount);
}

void TelemetryMetrics::increment_session(std::size_t slot, TelemetryMetricCounter counter, std::uint64_t amount) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	increment_process(counter, amount);
	const auto index = static_cast<std::size_t>(counter);
	if (index < m_snapshot.sessions[slot].counters.size()) saturating_add(m_snapshot.sessions[slot].counters[index], amount);
}

void TelemetryMetrics::increment_mission(TelemetryMetricCounter counter, std::uint64_t amount) noexcept
{
	if (!m_snapshot.provisioned) return;
	increment_process(counter, amount);
	const auto index = static_cast<std::size_t>(counter);
	if (index < m_snapshot.mission_counters.size()) saturating_add(m_snapshot.mission_counters[index], amount);
}

void TelemetryMetrics::observe_process(TelemetryMetricHistogram histogram, std::uint64_t duration_us) noexcept
{
	if (!m_snapshot.provisioned) return;
	const auto index = static_cast<std::size_t>(histogram);
	if (index < m_snapshot.process_histograms.size()) observe(m_snapshot.process_histograms[index], duration_us);
}

void TelemetryMetrics::observe_session(std::size_t slot, TelemetryMetricHistogram histogram, std::uint64_t duration_us) noexcept
{
	if (!m_snapshot.provisioned || !valid_slot(slot)) return;
	observe_process(histogram, duration_us);
	const auto index = static_cast<std::size_t>(histogram);
	if (index < m_snapshot.sessions[slot].histograms.size()) observe(m_snapshot.sessions[slot].histograms[index], duration_us);
}

void TelemetryMetrics::observe_mission(TelemetryMetricHistogram histogram, std::uint64_t duration_us) noexcept
{
	if (!m_snapshot.provisioned) return;
	observe_process(histogram, duration_us);
	const auto index = static_cast<std::size_t>(histogram);
	if (index < m_snapshot.mission_histograms.size()) observe(m_snapshot.mission_histograms[index], duration_us);
}

std::size_t TelemetryMetrics::histogram_bucket(std::uint64_t duration_us) noexcept
{
	constexpr std::array<std::uint64_t, 8U> upper_bounds{{5U, 10U, 25U, 50U, 100U, 250U, 500U, 1000U}};
	for (std::size_t index = 0U; index < upper_bounds.size(); ++index) {
		if (duration_us <= upper_bounds[index]) return index;
	}
	return upper_bounds.size();
}

void TelemetryMetrics::saturating_add(std::uint64_t& value, std::uint64_t amount) noexcept
{
	if (amount > std::numeric_limits<std::uint64_t>::max() - value) {
		value = std::numeric_limits<std::uint64_t>::max();
		const auto overflow = static_cast<std::size_t>(TelemetryMetricCounter::CounterOverflow);
		if (overflow < m_snapshot.process_counters.size() && &value != &m_snapshot.process_counters[overflow]) {
			// The overflow counter itself is also saturating, but avoids recursion.
			auto& count = m_snapshot.process_counters[overflow];
			if (count != std::numeric_limits<std::uint64_t>::max()) ++count;
		}
		return;
	}
	value += amount;
}

void TelemetryMetrics::observe(TelemetryHistogramSnapshot& histogram, std::uint64_t duration_us) noexcept
{
	saturating_add(histogram.buckets[histogram_bucket(duration_us)], 1U);
	saturating_add(histogram.count, 1U);
	saturating_add(histogram.sum_us, duration_us);
}

void TelemetryMetrics::refresh_clients_high_water() noexcept
{
	std::uint64_t active = 0U;
	for (const auto& session : m_snapshot.sessions) if (session.active) ++active;
	m_snapshot.clients_active = active;
	if (active > m_snapshot.clients_active_high_water) m_snapshot.clients_active_high_water = active;
}

} // namespace telemetry::detail
