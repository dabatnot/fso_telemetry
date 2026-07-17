#pragma once

#include "telemetry/config.h"
#include "telemetry/datagram_scheduler.h"
#include "telemetry/session_controller.h"
#include "telemetry/transport.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

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
};

struct NativeSessionStartRequest {
	const TelemetryConfig* config = nullptr;
	std::uint64_t producer_id = 0U;
	SessionIdAllocator* session_ids = nullptr;
	RandomSource* packet_sequences = nullptr;
};

struct NativeSessionTickContext {
	std::uint64_t now_us = 0U;
	std::uint32_t mission_generation = 0U;
	bool mission_active = false;
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
	NativeSessionTickStatus service_tick(const NativeSessionTickContext& context) noexcept;
	void purge_all(SessionCloseReason reason) noexcept;
	void shutdown() noexcept;

	std::size_t socket_count() const noexcept;
	std::size_t active_sessions() const noexcept;
	SessionControllerOwnedUsage owned_usage() const noexcept;

  private:
	enum class State : std::uint8_t { Cold = 0, Started, Faulted, Stopped };

	bool ingress_ready() const noexcept override;
	bool egress_ready() const noexcept override;
	IoStatus try_receive() noexcept override;
	IoStatus try_send() noexcept override;
	void fail_transport() noexcept;

	DedicatedUdpTransport m_transport;
	NativeOutputCompletionPort& m_output_completion;
	SessionController m_controller;
	DatagramTickScheduler m_scheduler;
	NativeSessionTickContext m_tick_context{};
	std::uint16_t m_maximum_attempts = 0U;
	State m_state = State::Cold;
	bool m_controller_ready = false;
};

} // namespace telemetry::detail
