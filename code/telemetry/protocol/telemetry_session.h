#pragma once

#include "telemetry/protocol/telemetry_capabilities.h"
#include "telemetry/protocol/telemetry_clock.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_counters.h"
#include "telemetry/protocol/telemetry_session_context.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::uint64_t HandshakeCacheLifetimeMs = 10000;
constexpr std::size_t HandshakeCacheCapacity = 8;
constexpr std::size_t MaxCachedWelcomePayloadSize = WelcomePayloadPrefixSize + MaxControlExtensionsSize;

// The session model owns its small clock, probe and capability state. Larger
// protocol resources (reassemblies, transactions, reliable windows, baselines
// and specialized streams) are purged through this callback before an old
// session id can be replaced or forgotten.
class SessionResourcePurger {
  public:
	virtual ~SessionResourcePurger() = default;
	virtual void purge_session_resources(std::uint64_t session_id) noexcept = 0;
};

enum class ClientSessionState : std::uint8_t {
	Disconnected = 0,
	Negotiating = 1,
	Synchronizing = 2,
	Live = 3,
	Stale = 4,
};

enum class ProducerSessionState : std::uint8_t {
	Listening = 0,
	Negotiating = 1,
	AwaitingWelcomeProof = 2,
	Synchronizing = 3,
	Live = 4,
	Closing = 5,
	Rejected = 6,
};

enum class SessionModelResult : std::uint8_t {
	Applied = 0,
	NoChange = 1,
	Rejected = 2,
	InvalidState = 3,
	InvalidArgument = 4,
	EndpointMismatch = 5,
	SessionMismatch = 6,
	CapabilityMismatch = 7,
	ClockSampleInvalid = 8,
	ResourceLimit = 9,
	AntiAmplificationLimit = 10,
};

enum class SessionTimeoutAction : std::uint8_t {
	None = 0,
	ClockInvalidated = 1,
	BecameStale = 2,
	Disconnected = 3,
	Closing = 4,
	MonotonicDiscontinuity = 5,
};

// This gate deliberately consumes only already-applied facts. ACK/NACK window
// ownership remains in P0.5; its owner calls these methods after durable commit
// and after arranging the corresponding APPLIED acknowledgement.
class SessionSynchronizationGate final {
  public:
	void reset() noexcept;
	SessionModelResult apply_session_begin(const SessionBeginPayload& payload) noexcept;
	SessionModelResult begin_resynchronization(std::uint32_t required_manifest_id,
		std::uint32_t expected_snapshot_id,
		bool required_manifest_already_applied) noexcept;
	SessionModelResult apply_manifest(std::uint32_t manifest_id) noexcept;
	SessionModelResult apply_full_snapshot(std::uint32_t snapshot_id, std::uint32_t required_manifest_id) noexcept;

	bool session_begin_applied() const noexcept
	{
		return m_session_begin_applied;
	}
	bool required_manifest_applied() const noexcept
	{
		return m_required_manifest_applied;
	}
	bool full_snapshot_applied() const noexcept
	{
		return m_full_snapshot_applied;
	}
	bool ready_for_live() const noexcept;
	std::uint32_t required_manifest_id() const noexcept
	{
		return m_required_manifest_id;
	}
	std::uint32_t expected_snapshot_id() const noexcept
	{
		return m_expected_snapshot_id;
	}
	std::uint32_t applied_snapshot_id() const noexcept
	{
		return m_applied_snapshot_id;
	}

  private:
	bool m_session_begin_applied = false;
	bool m_required_manifest_applied = false;
	bool m_full_snapshot_applied = false;
	std::uint32_t m_required_manifest_id = 0;
	std::uint32_t m_expected_snapshot_id = 0;
	std::uint32_t m_applied_snapshot_id = 0;
};

struct WelcomeMessageIdentity {
	std::uint64_t session_id = 0;
	EndpointKey endpoint;
	std::uint32_t message_id = 0;
	std::uint16_t fragment_count = 0;
	std::uint32_t message_crc32 = 0;
};

// Fields supplied by the validated ACK envelope and AckPayload codec. Keeping
// these values together prevents the reachability proof from degrading into a
// boolean detached from the session and exact Welcome message.
struct WelcomeAckCandidate {
	std::uint64_t session_id = 0;
	EndpointKey source_endpoint;
	std::uint32_t target_message_id = 0;
	MessageType target_message_type = MessageType::Invalid;
	std::uint8_t ack_flags = 0;
	std::uint16_t target_fragment_count = 0;
	std::uint32_t target_message_crc32 = 0;
};

enum class WelcomeProofValidationResult : std::uint8_t {
	Valid = 0,
	InvalidAckFlags = 1,
	SessionMismatch = 2,
	EndpointMismatch = 3,
	WrongMessageType = 4,
	MessageIdentityMismatch = 5,
	InvalidState = 6,
};

class ValidatedWelcomeAppliedProof final {
  public:
	bool valid() const noexcept
	{
		return m_valid;
	}
	const WelcomeMessageIdentity& identity() const noexcept
	{
		return m_identity;
	}

  private:
	bool m_valid = false;
	WelcomeMessageIdentity m_identity;

	friend WelcomeProofValidationResult validate_welcome_applied_proof(const WelcomeAckCandidate&,
		const WelcomeMessageIdentity&,
		ValidatedWelcomeAppliedProof&) noexcept;
};

// proof is unchanged unless Valid is returned. APPLIED is accepted only as the
// canonical VALIDATED|APPLIED value 0x03, never as APPLIED alone.
WelcomeProofValidationResult validate_welcome_applied_proof(const WelcomeAckCandidate& candidate,
	const WelcomeMessageIdentity& expected,
	ValidatedWelcomeAppliedProof& proof) noexcept;

// payload points into the cache and remains valid only until the next mutating
// cache operation (store, expire, invalidate_session or reset).
struct CachedWelcomeResponseView {
	EndpointKey endpoint;
	std::uint64_t client_nonce = 0;
	WelcomeStatus status = WelcomeStatus::Accepted;
	std::uint64_t session_id = 0;
	std::uint8_t flags = MessageFlagNone;
	std::uint32_t message_id = 0;
	std::uint16_t fragment_count = 0;
	std::uint32_t message_crc32 = 0;
	std::uint64_t stored_at_ms = 0;
	ByteView payload;
};

enum class HandshakeCacheLookupResult : std::uint8_t {
	Found = 0,
	Miss = 1,
	InvalidKey = 2,
};

enum class HandshakeCacheStoreResult : std::uint8_t {
	Stored = 0,
	Existing = 1,
	InvalidKey = 2,
	InvalidWelcome = 3,
	CapacityReached = 4,
};

// Fixed-size cache for the normative (endpoint, client_nonce) handshake key.
// The encoded Welcome payload is copied verbatim so a duplicate Hello cannot
// accidentally renegotiate different capabilities or times.
class ProducerHandshakeCache final {
  public:
	HandshakeCacheLookupResult lookup(const EndpointKey& endpoint,
		std::uint64_t client_nonce,
		std::uint64_t now_ms,
		CachedWelcomeResponseView& response) noexcept;
	HandshakeCacheStoreResult store(const EndpointKey& endpoint,
		std::uint64_t client_nonce,
		const TelemetryDatagramHeader& welcome_header,
		ByteView encoded_welcome_payload,
		std::uint64_t now_ms,
		CachedWelcomeResponseView& response) noexcept;
	void expire(std::uint64_t now_ms) noexcept;
	void reset() noexcept;
	std::size_t size() const noexcept
	{
		return m_size;
	}
	bool has_capacity(std::uint64_t now_ms) noexcept;

  private:
	struct Slot {
		bool occupied = false;
		EndpointKey endpoint;
		std::uint64_t client_nonce = 0;
		WelcomeStatus status = WelcomeStatus::Accepted;
		std::uint64_t session_id = 0;
		std::uint8_t flags = MessageFlagNone;
		std::uint32_t message_id = 0;
		std::uint16_t fragment_count = 0;
		std::uint32_t message_crc32 = 0;
		std::uint64_t stored_at_ms = 0;
		std::uint16_t payload_size = 0;
		std::array<std::uint8_t, MaxCachedWelcomePayloadSize> payload{};
	};

	static CachedWelcomeResponseView view(const Slot& slot) noexcept;
	std::array<Slot, HandshakeCacheCapacity> m_slots{};
	std::size_t m_size = 0;
};

enum class ClientWelcomeResult : std::uint8_t {
	Accepted = 0,
	Rejected = 1,
	InvalidState = 2,
	InvalidPayload = 3,
	EndpointMismatch = 4,
	NonceMismatch = 5,
	SessionMismatch = 6,
	CapabilityMismatch = 7,
	ClockSampleInvalid = 8,
};

class ClientSessionModel final {
  public:
	explicit ClientSessionModel(SessionResourcePurger& purger) noexcept : m_purger(&purger) {}

	SessionModelResult
	begin_negotiation(const EndpointKey& producer_endpoint, const HelloPayload& hello, std::uint64_t now_ms) noexcept;
	ClientWelcomeResult receive_welcome(const TelemetryDatagramHeader& header,
		const EndpointKey& source_endpoint,
		const WelcomePayload& welcome,
		std::uint64_t client_receive_t3_us,
		std::uint64_t now_ms) noexcept;

	SessionModelResult on_session_begin_applied(const SessionBeginPayload& payload) noexcept;
	SessionModelResult on_manifest_applied(std::uint32_t manifest_id) noexcept;
	SessionModelResult on_full_snapshot_applied(std::uint32_t snapshot_id, std::uint32_t required_manifest_id) noexcept;
	SessionModelResult begin_resynchronization(std::uint32_t required_manifest_id,
		std::uint32_t expected_snapshot_id,
		bool required_manifest_already_applied) noexcept;
	SessionModelResult mark_stale() noexcept;
	SessionModelResult disconnect() noexcept;

	bool note_valid_session_datagram(std::uint64_t now_ms) noexcept;
	bool note_valid_heartbeat_response(const OrientedClockSample& sample, std::uint64_t now_ms) noexcept;
	SessionTimeoutAction poll_timeouts(std::uint64_t now_ms) noexcept;

	ClientSessionState state() const noexcept
	{
		return m_state;
	}
	std::uint64_t session_id() const noexcept
	{
		return m_session_id;
	}
	std::uint64_t client_nonce() const noexcept
	{
		return m_client_nonce;
	}
	const EndpointKey& producer_endpoint() const noexcept
	{
		return m_producer_endpoint;
	}
	std::uint64_t producer_id() const noexcept
	{
		return m_producer_id;
	}
	std::uint8_t selected_major() const noexcept
	{
		return m_selected_major;
	}
	std::uint8_t selected_minor() const noexcept
	{
		return m_selected_minor;
	}
	VisibilityMode selected_visibility_mode() const noexcept
	{
		return m_selected_visibility_mode;
	}
	// HELLO retransmission/backoff and its terminal negotiation expiry belong
	// to the P0.5 reliable owner. P0.4 retains the epoch but poll_timeouts()
	// intentionally evaluates only an accepted active session.
	std::uint64_t negotiation_started_ms() const noexcept
	{
		return m_negotiation_started_ms;
	}
	std::uint16_t heartbeat_interval_ms() const noexcept
	{
		return m_heartbeat_interval_ms;
	}
	std::uint32_t stale_timeout_ms() const noexcept
	{
		return m_stale_timeout_ms;
	}
	std::uint32_t disconnect_timeout_ms() const noexcept
	{
		return m_disconnect_timeout_ms;
	}
	const SessionSynchronizationGate& synchronization() const noexcept
	{
		return m_synchronization;
	}
	const ClockFilter& clock_filter() const noexcept
	{
		return m_clock_filter;
	}
	ClockFilter& clock_filter() noexcept
	{
		return m_clock_filter;
	}
	const ProbeTracker& probes() const noexcept
	{
		return m_probes;
	}
	ProbeTracker& probes() noexcept
	{
		return m_probes;
	}
	const CapabilitySessionState& capabilities() const noexcept
	{
		return m_capabilities;
	}
	CapabilitySessionState& capabilities() noexcept
	{
		return m_capabilities;
	}
	TelemetrySessionContext context() const noexcept;
	bool welcome_message_identity(WelcomeMessageIdentity& identity) const noexcept;

  private:
	void purge_active_session() noexcept;
	void clear_pending_negotiation() noexcept;
	void promote_if_synchronized() noexcept;
	bool active() const noexcept
	{
		return m_session_id != 0;
	}

	SessionResourcePurger* m_purger = nullptr;
	ClientSessionState m_state = ClientSessionState::Disconnected;
	EndpointKey m_producer_endpoint;
	std::uint64_t m_session_id = 0;
	std::uint64_t m_client_nonce = 0;
	std::uint64_t m_producer_id = 0;
	std::uint8_t m_selected_major = 0;
	std::uint8_t m_selected_minor = 0;
	VisibilityMode m_selected_visibility_mode = VisibilityMode::Cockpit;
	std::uint64_t m_pending_nonce = 0;
	std::uint64_t m_pending_t0_us = 0;
	std::uint64_t m_pending_client_capabilities = 0;
	ProtocolMinorRange m_pending_minor_range = FrozenV1_0MinorRange;
	std::uint64_t m_negotiation_started_ms = 0;
	std::uint16_t m_heartbeat_interval_ms = 0;
	std::uint32_t m_stale_timeout_ms = 0;
	std::uint32_t m_disconnect_timeout_ms = 0;
	std::uint64_t m_last_valid_network_activity_ms = 0;
	std::uint64_t m_last_valid_clock_response_ms = 0;
	WelcomeMessageIdentity m_welcome_identity;
	SessionSynchronizationGate m_synchronization;
	ClockFilter m_clock_filter;
	ProbeTracker m_probes;
	CapabilitySessionState m_capabilities;
};

enum class ProducerHandshakeBeginResult : std::uint8_t {
	Ready = 0,
	Cached = 1,
	InvalidHello = 2,
	InvalidEndpoint = 3,
	InvalidState = 4,
	CacheFull = 5,
};

enum class ProducerHandshakeCompleteResult : std::uint8_t {
	AwaitingProof = 0,
	Rejected = 1,
	Existing = 2,
	InvalidState = 3,
	InvalidWelcome = 4,
	CapabilityMismatch = 5,
	CacheFull = 6,
};

class ProducerSessionModel final {
  public:
	explicit ProducerSessionModel(SessionResourcePurger& purger,
		ProtocolMinorRange supported_minors = FrozenV1_0MinorRange) noexcept
		: m_purger(&purger), m_supported_minors(supported_minors)
	{
	}

	ProducerHandshakeBeginResult begin_handshake(const EndpointKey& client_endpoint,
		const HelloPayload& hello,
		std::uint64_t received_datagram_bytes,
		std::uint64_t now_ms,
		CachedWelcomeResponseView& cached_response) noexcept;
	ProducerHandshakeCompleteResult complete_handshake(const TelemetryDatagramHeader& welcome_header,
		ByteView encoded_welcome_payload,
		std::uint64_t now_ms,
		CachedWelcomeResponseView& cached_response) noexcept;

	ProtocolMinorRange supported_minors() const noexcept
	{
		return m_supported_minors;
	}
	ProtocolMinorNegotiationResult pending_minor_negotiation() const noexcept
	{
		return m_pending_minor_negotiation;
	}
	std::uint8_t pending_selected_minor() const noexcept
	{
		return m_pending_selected_minor;
	}

	bool note_preproof_bytes_received(const EndpointKey& endpoint, std::uint64_t bytes) noexcept;
	SessionModelResult try_account_preproof_send(const EndpointKey& endpoint, std::uint64_t bytes) noexcept;
	WelcomeProofValidationResult accept_welcome_applied_ack(const WelcomeAckCandidate& candidate,
		std::uint64_t now_ms,
		ValidatedWelcomeAppliedProof& proof) noexcept;
	bool can_send_heavy_data() const noexcept;

	SessionModelResult on_session_begin_applied(const SessionBeginPayload& payload) noexcept;
	SessionModelResult on_manifest_applied(std::uint32_t manifest_id) noexcept;
	SessionModelResult on_full_snapshot_applied(std::uint32_t snapshot_id, std::uint32_t required_manifest_id) noexcept;
	SessionModelResult begin_resynchronization(std::uint32_t required_manifest_id,
		std::uint32_t expected_snapshot_id,
		bool required_manifest_already_applied) noexcept;
	SessionModelResult begin_closing() noexcept;
	SessionModelResult finish_closing() noexcept;
	SessionModelResult return_to_listening() noexcept;

	bool note_valid_session_datagram(std::uint64_t now_ms) noexcept;
	bool note_valid_heartbeat_response(const OrientedClockSample& sample, std::uint64_t now_ms) noexcept;
	SessionTimeoutAction poll_timeouts(std::uint64_t now_ms) noexcept;

	ProducerSessionState state() const noexcept
	{
		return m_state;
	}
	std::uint64_t session_id() const noexcept
	{
		return m_session_id;
	}
	std::uint64_t client_nonce() const noexcept
	{
		return m_client_nonce;
	}
	const EndpointKey& client_endpoint() const noexcept
	{
		return m_client_endpoint;
	}
	std::uint64_t producer_id() const noexcept
	{
		return m_producer_id;
	}
	std::uint8_t selected_major() const noexcept
	{
		return m_selected_major;
	}
	std::uint8_t selected_minor() const noexcept
	{
		return m_selected_minor;
	}
	VisibilityMode selected_visibility_mode() const noexcept
	{
		return m_selected_visibility_mode;
	}
	std::uint64_t preproof_bytes_received() const noexcept
	{
		return m_preproof_bytes_received;
	}
	std::uint64_t preproof_bytes_sent() const noexcept
	{
		return m_preproof_bytes_sent;
	}
	std::uint16_t heartbeat_interval_ms() const noexcept
	{
		return m_heartbeat_interval_ms;
	}
	std::uint32_t stale_timeout_ms() const noexcept
	{
		return m_stale_timeout_ms;
	}
	std::uint32_t disconnect_timeout_ms() const noexcept
	{
		return m_disconnect_timeout_ms;
	}
	const SessionSynchronizationGate& synchronization() const noexcept
	{
		return m_synchronization;
	}
	const ClockFilter& clock_filter() const noexcept
	{
		return m_clock_filter;
	}
	ClockFilter& clock_filter() noexcept
	{
		return m_clock_filter;
	}
	const ProbeTracker& probes() const noexcept
	{
		return m_probes;
	}
	ProbeTracker& probes() noexcept
	{
		return m_probes;
	}
	const CapabilitySessionState& capabilities() const noexcept
	{
		return m_capabilities;
	}
	CapabilitySessionState& capabilities() noexcept
	{
		return m_capabilities;
	}
	const ProducerHandshakeCache& handshake_cache() const noexcept
	{
		return m_handshake_cache;
	}
	ProducerHandshakeCache& handshake_cache() noexcept
	{
		return m_handshake_cache;
	}
	TelemetrySessionContext context() const noexcept;

  private:
	void purge_active_session() noexcept;
	void clear_pending_handshake() noexcept;
	void promote_if_synchronized() noexcept;
	bool active() const noexcept
	{
		return m_session_id != 0;
	}

	SessionResourcePurger* m_purger = nullptr;
	ProtocolMinorRange m_supported_minors = FrozenV1_0MinorRange;
	ProducerSessionState m_state = ProducerSessionState::Listening;
	EndpointKey m_client_endpoint;
	std::uint64_t m_session_id = 0;
	std::uint64_t m_client_nonce = 0;
	std::uint64_t m_producer_id = 0;
	std::uint8_t m_selected_major = 0;
	std::uint8_t m_selected_minor = 0;
	VisibilityMode m_selected_visibility_mode = VisibilityMode::Cockpit;
	EndpointKey m_pending_endpoint;
	std::uint64_t m_pending_nonce = 0;
	std::uint64_t m_pending_t0_us = 0;
	std::uint64_t m_pending_client_capabilities = 0;
	ProtocolMinorNegotiationResult m_pending_minor_negotiation = ProtocolMinorNegotiationResult::InvalidRange;
	std::uint8_t m_pending_selected_minor = VersionMinor;
	std::uint64_t m_preproof_bytes_received = 0;
	std::uint64_t m_preproof_bytes_sent = 0;
	std::uint16_t m_heartbeat_interval_ms = 0;
	std::uint32_t m_stale_timeout_ms = 0;
	std::uint32_t m_disconnect_timeout_ms = 0;
	std::uint64_t m_last_valid_network_activity_ms = 0;
	std::uint64_t m_last_valid_clock_response_ms = 0;
	bool m_welcome_proof_received = false;
	bool m_clock_timeout_reported = false;
	WelcomeMessageIdentity m_expected_welcome;
	SessionSynchronizationGate m_synchronization;
	ClockFilter m_clock_filter;
	ProbeTracker m_probes;
	CapabilitySessionState m_capabilities;
	ProducerHandshakeCache m_handshake_cache;
};

} // namespace telemetry::protocol
