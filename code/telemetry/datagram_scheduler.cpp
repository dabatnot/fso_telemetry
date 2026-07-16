#include "telemetry/datagram_scheduler.h"

#include <limits>

namespace telemetry::detail {

DatagramSelectionResult DatagramPrioritySelector::select(const DatagramCandidate* candidates,
	std::size_t count) noexcept
{
	if (count == 0U) {
		return {DatagramSelectionStatus::NoneReady, 0U};
	}
	if (candidates == nullptr) {
		return {DatagramSelectionStatus::InvalidInput, 0U};
	}
	if (count > MaximumDatagramCandidates) {
		return {DatagramSelectionStatus::TooManyCandidates, 0U};
	}

	auto selected_priority = DatagramPriority::Count;
	for (std::size_t index = 0U; index < count; ++index) {
		if (candidates[index].priority >= DatagramPriority::Count) {
			return {DatagramSelectionStatus::InvalidInput, 0U};
		}
		if (candidates[index].ready && candidates[index].priority < selected_priority) {
			selected_priority = candidates[index].priority;
		}
	}
	if (selected_priority == DatagramPriority::Count) {
		return {DatagramSelectionStatus::NoneReady, 0U};
	}

	const auto priority_index = static_cast<std::size_t>(selected_priority);
	std::uint16_t selected_owner = std::numeric_limits<std::uint16_t>::max();
	if (m_has_last_owner[priority_index]) {
		for (std::size_t index = 0U; index < count; ++index) {
			const auto& candidate = candidates[index];
			if (candidate.ready && candidate.priority == selected_priority &&
				candidate.owner > m_last_owner[priority_index] && candidate.owner < selected_owner) {
				selected_owner = candidate.owner;
			}
		}
	}
	if (selected_owner == std::numeric_limits<std::uint16_t>::max()) {
		for (std::size_t index = 0U; index < count; ++index) {
			const auto& candidate = candidates[index];
			if (candidate.ready && candidate.priority == selected_priority &&
				candidate.owner < selected_owner) {
				selected_owner = candidate.owner;
			}
		}
	}

	for (std::size_t index = 0U; index < count; ++index) {
		const auto& candidate = candidates[index];
		if (candidate.ready && candidate.priority == selected_priority && candidate.owner == selected_owner) {
			m_last_owner[priority_index] = candidate.owner;
			m_has_last_owner[priority_index] = true;
			return {DatagramSelectionStatus::Selected, index};
		}
	}
	return {DatagramSelectionStatus::NoneReady, 0U};
}

void DatagramPrioritySelector::reset() noexcept
{
	m_last_owner.fill(0U);
	m_has_last_owner.fill(false);
}

DatagramTickResult DatagramTickScheduler::run_tick(std::uint16_t maximum_attempts,
	DatagramIoWork& io) noexcept
{
	DatagramTickResult result;
	if (maximum_attempts == 0U || maximum_attempts > 256U) {
		return result;
	}
	result.valid_budget = true;

	auto direction = m_first_direction;
	m_first_direction = m_first_direction == DatagramDirection::Ingress ? DatagramDirection::Egress
																			  : DatagramDirection::Ingress;
	bool ingress_available = io.ingress_ready();
	bool egress_available = io.egress_ready();
	bool ingress_blocked = false;
	bool egress_blocked = false;

	while (result.attempts < maximum_attempts && (ingress_available || egress_available)) {
		if ((direction == DatagramDirection::Ingress && !ingress_available) ||
			(direction == DatagramDirection::Egress && !egress_available)) {
			direction = direction == DatagramDirection::Ingress ? DatagramDirection::Egress
																		  : DatagramDirection::Ingress;
			continue;
		}

		IoStatus status;
		if (direction == DatagramDirection::Ingress) {
			status = io.try_receive();
			++result.ingress_attempts;
		} else {
			status = io.try_send();
			++result.egress_attempts;
		}
		++result.attempts;
		result.terminal_status = status;

		if (status == IoStatus::Closed || status == IoStatus::Error) {
			break;
		}
		if (status == IoStatus::WouldBlock) {
			if (direction == DatagramDirection::Ingress) {
				ingress_available = false;
				ingress_blocked = true;
			} else {
				egress_available = false;
				egress_blocked = true;
			}
		} else {
			ingress_available = !ingress_blocked && io.ingress_ready();
			egress_available = !egress_blocked && io.egress_ready();
		}
		direction = direction == DatagramDirection::Ingress ? DatagramDirection::Egress
																		  : DatagramDirection::Ingress;
	}
	return result;
}

void DatagramTickScheduler::reset() noexcept
{
	m_first_direction = DatagramDirection::Ingress;
}

} // namespace telemetry::detail
