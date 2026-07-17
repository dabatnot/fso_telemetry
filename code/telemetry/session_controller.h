#pragma once

#include "telemetry/identity.h"
#include "telemetry/protocol/telemetry_rate_limiter.h"
#include "telemetry/protocol/telemetry_clock.h"
#include "telemetry/protocol/telemetry_counters.h"
#include "telemetry/protocol/telemetry_security.h"
#include "telemetry/protocol/telemetry_session.h"
#include "telemetry/startup_budget.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>

namespace telemetry::detail {

enum class IoStatus : std::uint8_t;

enum class SessionControllerConfigureResult : std::uint8_t { Ready = 0, InvalidConfiguration, AllocationFailure };
enum class SessionIngressDisposition : std::uint8_t {
	Dropped = 0,
	ResponseQueued,
	CachedResponseQueued,
	WelcomeProofApplied,
	Faulted,
};
enum class SessionIngressDropReason : std::uint8_t {
	None = 0,
	SourceNotAllowed,
	DatagramEnvelopeInvalid,
	EndpointSessionMismatch,
	HelloRateLimited,
	SessionCreationRateLimited,
	HandshakeCacheFull,
	AntiAmplificationLimit,
	PayloadInvalid,
	SessionIdUnavailable,
	NoClientSlot,
	WelcomeProofMismatch,
	WelcomeProofExpired,
	OutputBusy,
};
enum class SessionIngressStage : std::uint8_t {
	SourcePolicy = 0,
	DatagramEnvelope,
	EndpointAndSession,
	RateLimit,
	AntiAmplification,
	Payload,
	SessionMutation,
};
enum class ProducerSessionProgress : std::uint8_t { Empty = 0, AwaitWelcomeApplied, ReadyForState, Stale };
enum class SessionCloseReason : std::uint8_t { ProtocolError = 0, Timeout, MissionDiscontinuity, TransportError, Shutdown };

struct SessionIngressResult {
	SessionIngressDisposition disposition = SessionIngressDisposition::Dropped;
	SessionIngressDropReason drop_reason = SessionIngressDropReason::None;
};

class SessionControllerObserver {
  public:
	virtual ~SessionControllerObserver() = default;
	virtual void stage_reached(SessionIngressStage stage) noexcept = 0;
};

struct SessionControllerConfig {
	std::size_t max_clients = 1U;
	std::uint64_t producer_id = 0U;
	std::uint16_t mission_heartbeat_ms = 500U;
	std::uint16_t idle_heartbeat_ms = 1000U;
	protocol::TelemetryOperationalConfig security;
};

struct SessionControllerOutput {
	protocol::EndpointKey endpoint;
	std::array<std::uint8_t, protocol::MaxDatagramSize> bytes{};
	std::size_t size = 0U;
};

struct SessionControllerSlot {
	struct HeartbeatState {
		protocol::ProbeTracker probes;
		protocol::ClockFilter clock_filter;
		std::uint16_t negotiated_interval_ms = 0U;
		std::uint64_t next_periodic_due_us = 0U;
		std::uint64_t stale_timeout_us = 0U;
		std::uint64_t disconnect_timeout_us = 0U;
		std::uint64_t last_valid_network_activity_us = 0U;
		std::uint64_t last_valid_clock_response_us = 0U;
		bool clock_stale = false;
	};

	ProducerSessionProgress progress = ProducerSessionProgress::Empty;
	protocol::EndpointKey endpoint;
	std::uint64_t session_id = 0U;
	std::uint64_t session_start_us = 0U;
	std::uint64_t welcome_deadline_us = 0U;
	std::uint32_t next_message_id = 1U;
	std::uint32_t next_packet_sequence = 1U;
	std::uint32_t welcome_message_id = 0U;
	std::uint16_t welcome_fragment_count = 0U;
	std::uint32_t welcome_message_crc32 = 0U;
	std::size_t reassembly_bytes_reserved = 0U;
	std::size_t reliable_items_in_use = 0U;
	std::uint64_t preproof_validated_bytes_received = 0U;
	std::uint64_t preproof_bytes_sent = 0U;
	bool has_reliability_terminal_policy = false;
	protocol::ReliableTerminalPolicy reliability_terminal_policy = protocol::ReliableTerminalPolicy::Drop;
	HeartbeatState heartbeat;
};

enum class PreproofLedgerResult : std::uint8_t {
	Recorded = 0,
	Allowed,
	AntiAmplificationLimit,
	CapacityReached,
	ArithmeticOverflow,
	InvalidEndpoint,
	ProofAlreadyApplied,
};

struct PreproofAccount {
	protocol::EndpointKey endpoint;
	std::uint64_t validated_bytes_received = 0U;
	std::uint64_t bytes_sent = 0U;
	bool proven = false;
};

class PreproofAmplificationLedger final {
  public:
	bool configure(std::size_t capacity) noexcept;
	PreproofLedgerResult note_validated_receive(const protocol::EndpointKey& endpoint, std::uint64_t bytes) noexcept;
	PreproofLedgerResult try_account_send(const protocol::EndpointKey& endpoint, std::uint64_t bytes) noexcept;
	void mark_welcome_proven(const protocol::EndpointKey& endpoint) noexcept;
	void release_contribution(const protocol::EndpointKey& endpoint,
		std::uint64_t received_bytes,
		std::uint64_t sent_bytes) noexcept;
	void erase(const protocol::EndpointKey& endpoint) noexcept;
	PreproofAccount account(const protocol::EndpointKey& endpoint) const noexcept;
	std::size_t size() const noexcept { return m_size; }

  private:
	std::size_t find(const protocol::EndpointKey& endpoint) const noexcept;
	std::array<PreproofAccount, protocol::HandshakeCacheCapacity> m_accounts{};
	std::array<bool, protocol::HandshakeCacheCapacity> m_used{};
	std::size_t m_capacity = 0U;
	std::size_t m_size = 0U;
};

struct SessionControllerOwnedUsage {
	std::size_t active_slots = 0U;
	std::size_t cache_entries = 0U;
	std::size_t preproof_accounts = 0U;
	std::size_t reassembly_bytes = 0U;
	std::size_t reliable_items = 0U;
	bool output_queued = false;
	friend bool operator==(const SessionControllerOwnedUsage& a, const SessionControllerOwnedUsage& b) noexcept
	{
		return a.active_slots == b.active_slots && a.cache_entries == b.cache_entries &&
			a.preproof_accounts == b.preproof_accounts && a.reassembly_bytes == b.reassembly_bytes &&
			a.reliable_items == b.reliable_items && a.output_queued == b.output_queued;
	}
};

using SessionControllerOwnedCapacity = Wp06OwnedCapacity;

class SessionController final {
  public:
	SessionController() = default;
	~SessionController() = default;
	SessionController(SessionController&&) noexcept = default;
	SessionController& operator=(SessionController&&) noexcept = default;
	SessionController(const SessionController&) = delete;
	SessionController& operator=(const SessionController&) = delete;

	static SessionControllerConfigureResult configure(const SessionControllerConfig& config,
		SessionIdAllocator& ids,
		RandomSource& packet_sequences,
		std::uint64_t initial_time_us,
		SessionControllerObserver* observer,
		SessionController& output) noexcept;

	SessionIngressResult ingest(const protocol::EndpointKey& endpoint,
		protocol::ByteView datagram,
		std::uint64_t now_us,
		std::uint32_t mission_generation,
		bool mission_active) noexcept;
	bool pop_output(SessionControllerOutput& output) noexcept;
	bool peek_output(SessionControllerOutput& output) const noexcept;
	void complete_output(IoStatus status) noexcept;
	void service_reliability(std::uint64_t now_us) noexcept;
	void service_session_maintenance(std::uint64_t now_us) noexcept;
	bool has_output() const noexcept { return m_has_output; }
	std::size_t active_slots() const noexcept;
	const SessionControllerSlot& slot(std::size_t index) const noexcept { return m_slots[index]; }
	bool close_slot(std::size_t index, SessionCloseReason reason) noexcept;
	void expire_housekeeping(std::uint64_t now_us) noexcept;
	std::size_t handshake_cache_entries() const noexcept { return m_cache_size; }
	std::size_t preproof_account_count() const noexcept { return m_preproof.size(); }
	PreproofAccount preproof_account(const protocol::EndpointKey& endpoint) const noexcept
	{
		return m_preproof.account(endpoint);
	}
	SessionControllerOwnedCapacity owned_capacity() const noexcept;
	SessionControllerOwnedUsage owned_usage() const noexcept;
	static std::size_t handshake_cache_storage_bytes() noexcept;

  private:
	struct CacheEntry {
		bool used = false;
		protocol::EndpointKey endpoint;
		std::uint64_t nonce = 0U;
		std::uint64_t session_id = 0U;
		std::uint64_t stored_at_us = 0U;
		std::uint64_t accounted_received = 0U;
		std::uint64_t accounted_sent = 0U;
		bool preproof_active = false;
		std::array<std::uint8_t, protocol::MaxDatagramSize> bytes{};
		std::size_t size = 0U;
	};

	void stage(SessionIngressStage value) noexcept;
	std::size_t find_cache(const protocol::EndpointKey& endpoint, std::uint64_t nonce) const noexcept;
	std::size_t free_cache() const noexcept;
	std::size_t free_slot() const noexcept;
	std::size_t find_awaiting_slot() const noexcept;
	std::size_t find_slot(const protocol::EndpointKey& endpoint, std::uint64_t session_id) const noexcept;
	bool initialize_slot(std::size_t index) noexcept;
	bool next_packet_sequence(std::uint32_t& sequence) noexcept;
	void release_cache_preproof(CacheEntry& entry) noexcept;
	void remove_cache_for_session(std::uint64_t session_id) noexcept;
	void clear_all() noexcept;
	bool queue_bytes(const protocol::EndpointKey& endpoint,
		const std::uint8_t* bytes,
		std::size_t size,
		std::size_t owner_slot = std::numeric_limits<std::size_t>::max()) noexcept;
	void apply_terminal_policy(std::size_t slot_index, protocol::ReliableTerminalPolicy policy) noexcept;
	bool queue_retransmission(std::size_t slot_index,
		const protocol::ReliableWindowAction& action,
		std::uint64_t now_us) noexcept;
	bool queue_heartbeat(std::size_t slot_index,
		const protocol::HeartbeatPayload& heartbeat,
		std::uint64_t now_us,
		bool owns_probe,
		const protocol::ProbeToken& probe) noexcept;
	void note_network_activity(std::size_t slot_index, std::uint64_t now_us) noexcept;
	SessionIngressResult ingest_hello(const protocol::EndpointKey& endpoint,
		const protocol::DatagramView& decoded,
		std::size_t received_size,
		std::uint64_t now_us,
		bool mission_active) noexcept;
	SessionIngressResult ingest_ack(const protocol::EndpointKey& endpoint,
		const protocol::DatagramView& decoded,
		std::uint64_t now_us,
		std::uint32_t mission_generation,
		bool mission_active) noexcept;
	SessionIngressResult ingest_nack(const protocol::EndpointKey& endpoint,
		const protocol::DatagramView& decoded,
		std::uint64_t now_us) noexcept;
	SessionIngressResult ingest_heartbeat(const protocol::EndpointKey& endpoint,
		const protocol::DatagramView& decoded,
		std::uint64_t now_us) noexcept;

	SessionControllerConfig m_config{};
	SessionIdAllocator* m_ids = nullptr;
	RandomSource* m_packet_sequences = nullptr;
	SessionControllerObserver* m_observer = nullptr;
	std::unique_ptr<protocol::ProtocolRateLimiter> m_rate_limiter;
	std::unique_ptr<SessionControllerSlot[]> m_slots;
	std::unique_ptr<protocol::PreallocatedReliableControlWindow[]> m_reliable_windows;
	std::unique_ptr<std::uint8_t[]> m_reassembly_backing;
	std::array<CacheEntry, protocol::HandshakeCacheCapacity> m_cache{};
	std::size_t m_cache_size = 0U;
	PreproofAmplificationLedger m_preproof;
	SessionControllerOutput m_output{};
	std::size_t m_output_owner_slot = std::numeric_limits<std::size_t>::max();
	std::size_t m_pending_reliability_slot = std::numeric_limits<std::size_t>::max();
	std::uint64_t m_pending_reliability_time_us = 0U;
	std::size_t m_reliability_cursor = 0U;
	std::size_t m_heartbeat_cursor = 0U;
	bool m_has_output = false;
	bool m_output_reliability_pending = false;
	bool m_pending_preproof_send_accounted = false;
	bool m_output_heartbeat_pending = false;
	bool m_output_heartbeat_owns_probe = false;
	protocol::ProbeToken m_output_heartbeat_probe{};
	bool m_ready = false;
	bool m_faulted = false;
};

} // namespace telemetry::detail
