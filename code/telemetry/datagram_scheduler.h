#pragma once

#include "telemetry/transport.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

constexpr std::size_t MaximumDatagramCandidates = 256U;

enum class DatagramDirection : std::uint8_t {
	Ingress = 0,
	Egress,
};

enum class DatagramPriority : std::uint8_t {
	SafetyControl = 0,
	ReliableControl,
	CommittedState,
	Keyframe,
	Heartbeat,
	ReplaceableDelta,
	Count,
};

struct DatagramCandidate {
	DatagramPriority priority = DatagramPriority::Count;
	std::uint8_t owner = 0U;
	bool ready = false;
};

enum class DatagramSelectionStatus : std::uint8_t {
	Selected = 0,
	NoneReady,
	InvalidInput,
	TooManyCandidates,
};

struct DatagramSelectionResult {
	DatagramSelectionStatus status = DatagramSelectionStatus::NoneReady;
	std::size_t index = 0U;
};

class DatagramPrioritySelector final {
  public:
	DatagramSelectionResult select(const DatagramCandidate* candidates, std::size_t count) noexcept;
	void reset() noexcept;

  private:
	std::array<std::uint8_t, static_cast<std::size_t>(DatagramPriority::Count)> m_last_owner{};
	std::array<bool, static_cast<std::size_t>(DatagramPriority::Count)> m_has_last_owner{};
};

class DatagramIoWork {
  public:
	virtual ~DatagramIoWork() noexcept = default;
	virtual bool ingress_ready() const noexcept = 0;
	virtual bool egress_ready() const noexcept = 0;
	virtual IoStatus try_receive() noexcept = 0;
	virtual IoStatus try_send() noexcept = 0;
};

struct DatagramTickResult {
	bool valid_budget = false;
	std::uint16_t attempts = 0U;
	std::uint16_t ingress_attempts = 0U;
	std::uint16_t egress_attempts = 0U;
	IoStatus terminal_status = IoStatus::Complete;
};

class DatagramTickScheduler final {
  public:
	DatagramTickResult run_tick(std::uint16_t maximum_attempts, DatagramIoWork& io) noexcept;
	void reset() noexcept;

  private:
	DatagramDirection m_first_direction = DatagramDirection::Ingress;
};

} // namespace telemetry::detail
