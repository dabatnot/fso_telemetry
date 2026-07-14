#include "telemetry/protocol/telemetry_session.h"

#include <algorithm>
#include <limits>

namespace telemetry::protocol {

namespace {

constexpr std::uint8_t AckAppliedFlags =
	static_cast<std::uint8_t>(AckFlag::Validated) | static_cast<std::uint8_t>(AckFlag::Applied);

bool elapsed_since(std::uint64_t now_ms, std::uint64_t then_ms, std::uint64_t& elapsed_ms) noexcept
{
	if (now_ms < then_ms) {
		return false;
	}
	elapsed_ms = now_ms - then_ms;
	return true;
}

void saturating_add(std::uint64_t& value, std::uint64_t addition) noexcept
{
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	value = addition > maximum - value ? maximum : value + addition;
}

bool welcome_is_rejected(WelcomeStatus status) noexcept
{
	switch (status) {
	case WelcomeStatus::UnsupportedVersion:
	case WelcomeStatus::Unauthorized:
	case WelcomeStatus::Busy:
	case WelcomeStatus::InvalidCapabilities:
		return true;
	case WelcomeStatus::Accepted:
		return false;
	default:
		return false;
	}
}

// The producer must be able to return UnsupportedVersion. The current v1.0
// semantic validator intentionally rejects a non-1.0 range, so the handshake
// model performs only the bounded, version-neutral checks that remain valid
// before version selection. A future structural decoder can replace this
// helper without changing the state machine.
bool structurally_valid_received_hello(const HelloPayload& hello) noexcept
{
	if (hello.client_nonce == 0 ||
		(hello.requested_visibility_mode != VisibilityMode::Cockpit &&
			hello.requested_visibility_mode != VisibilityMode::TrustedFullState) ||
		hello.requested_heartbeat_ms < MinHeartbeatIntervalMs ||
		hello.requested_heartbeat_ms > MaxHeartbeatIntervalMs) {
		return false;
	}
	return validate_capability_extensions(hello.extensions, hello.extension_count) == ValidationError::None;
}

bool validate_cached_welcome(const EndpointKey& endpoint,
	std::uint64_t client_nonce,
	const TelemetryDatagramHeader& header,
	ByteView encoded_payload,
	WelcomePayload& decoded) noexcept
{
	if (!endpoint.is_valid() || client_nonce == 0 || encoded_payload.size > MaxCachedWelcomePayloadSize ||
		(encoded_payload.size != 0 && encoded_payload.data == nullptr)) {
		return false;
	}
	if (header.message_type != MessageType::Welcome || header.payload_size != encoded_payload.size ||
		header.message_size != encoded_payload.size) {
		return false;
	}
	if (validate_fragment_layout(header) != ValidationError::None ||
		validate_message_crc(header, encoded_payload) != ValidationError::None ||
		decode_welcome_payload(encoded_payload, decoded) != ValidationError::None ||
		decoded.client_nonce != client_nonce) {
		return false;
	}

	TelemetrySessionContext context;
	context.local_role = LocalEndpointRole::Client;
	context.peer_endpoint = endpoint;
	context.active_session_id = 0;
	return validate_welcome_logical_context(header, decoded.status, context) == ValidationError::None;
}

} // namespace

void SessionSynchronizationGate::reset() noexcept
{
	*this = SessionSynchronizationGate{};
}

SessionModelResult SessionSynchronizationGate::apply_session_begin(const SessionBeginPayload& payload) noexcept
{
	if (validate_session_begin_payload(payload) != ValidationError::None) {
		return SessionModelResult::InvalidArgument;
	}
	if (m_session_begin_applied) {
		return m_required_manifest_id == payload.required_manifest_id &&
					   m_expected_snapshot_id == payload.initial_snapshot_id
				   ? SessionModelResult::NoChange
				   : SessionModelResult::InvalidState;
	}

	m_session_begin_applied = true;
	m_required_manifest_id = payload.required_manifest_id;
	m_expected_snapshot_id = payload.initial_snapshot_id;
	m_required_manifest_applied = payload.required_manifest_id == 0;
	m_full_snapshot_applied = false;
	m_applied_snapshot_id = 0;
	return SessionModelResult::Applied;
}

SessionModelResult SessionSynchronizationGate::begin_resynchronization(std::uint32_t required_manifest_id,
	std::uint32_t expected_snapshot_id,
	bool required_manifest_already_applied) noexcept
{
	if (!m_session_begin_applied) {
		return SessionModelResult::InvalidState;
	}

	m_required_manifest_id = required_manifest_id;
	m_expected_snapshot_id = expected_snapshot_id;
	m_required_manifest_applied = required_manifest_id == 0 || required_manifest_already_applied;
	m_full_snapshot_applied = false;
	m_applied_snapshot_id = 0;
	return SessionModelResult::Applied;
}

SessionModelResult SessionSynchronizationGate::apply_manifest(std::uint32_t manifest_id) noexcept
{
	if (!m_session_begin_applied || manifest_id == 0) {
		return SessionModelResult::InvalidState;
	}
	if (m_required_manifest_id == 0) {
		return SessionModelResult::NoChange;
	}
	if (manifest_id != m_required_manifest_id) {
		return SessionModelResult::InvalidArgument;
	}
	if (m_required_manifest_applied) {
		return SessionModelResult::NoChange;
	}
	m_required_manifest_applied = true;
	return SessionModelResult::Applied;
}

SessionModelResult SessionSynchronizationGate::apply_full_snapshot(std::uint32_t snapshot_id,
	std::uint32_t required_manifest_id) noexcept
{
	if (!m_session_begin_applied || !m_required_manifest_applied) {
		return SessionModelResult::InvalidState;
	}
	if (snapshot_id == 0 || required_manifest_id != m_required_manifest_id ||
		(m_expected_snapshot_id != 0 && snapshot_id != m_expected_snapshot_id)) {
		return SessionModelResult::InvalidArgument;
	}
	if (m_full_snapshot_applied) {
		return snapshot_id == m_applied_snapshot_id ? SessionModelResult::NoChange : SessionModelResult::InvalidState;
	}

	if (m_expected_snapshot_id == 0) {
		m_expected_snapshot_id = snapshot_id;
	}
	m_applied_snapshot_id = snapshot_id;
	m_full_snapshot_applied = true;
	return SessionModelResult::Applied;
}

bool SessionSynchronizationGate::ready_for_live() const noexcept
{
	return m_session_begin_applied && m_required_manifest_applied && m_full_snapshot_applied;
}

WelcomeProofValidationResult validate_welcome_applied_proof(const WelcomeAckCandidate& candidate,
	const WelcomeMessageIdentity& expected,
	ValidatedWelcomeAppliedProof& proof) noexcept
{
	if (candidate.ack_flags != AckAppliedFlags) {
		return WelcomeProofValidationResult::InvalidAckFlags;
	}
	if (expected.session_id == 0 || candidate.session_id != expected.session_id) {
		return WelcomeProofValidationResult::SessionMismatch;
	}
	if (!expected.endpoint.is_valid() || !candidate.source_endpoint.is_valid() ||
		candidate.source_endpoint != expected.endpoint) {
		return WelcomeProofValidationResult::EndpointMismatch;
	}
	if (candidate.target_message_type != MessageType::Welcome) {
		return WelcomeProofValidationResult::WrongMessageType;
	}
	if (expected.message_id == 0 || expected.fragment_count != 1 ||
		candidate.target_message_id != expected.message_id ||
		candidate.target_fragment_count != expected.fragment_count ||
		candidate.target_message_crc32 != expected.message_crc32) {
		return WelcomeProofValidationResult::MessageIdentityMismatch;
	}

	ValidatedWelcomeAppliedProof validated;
	validated.m_valid = true;
	validated.m_identity = expected;
	proof = validated;
	return WelcomeProofValidationResult::Valid;
}

CachedWelcomeResponseView ProducerHandshakeCache::view(const Slot& slot) noexcept
{
	CachedWelcomeResponseView result;
	result.endpoint = slot.endpoint;
	result.client_nonce = slot.client_nonce;
	result.status = slot.status;
	result.session_id = slot.session_id;
	result.flags = slot.flags;
	result.message_id = slot.message_id;
	result.fragment_count = slot.fragment_count;
	result.message_crc32 = slot.message_crc32;
	result.stored_at_ms = slot.stored_at_ms;
	result.payload = ByteView{slot.payload.data(), slot.payload_size};
	return result;
}

void ProducerHandshakeCache::expire(std::uint64_t now_ms) noexcept
{
	for (auto& slot : m_slots) {
		if (!slot.occupied) {
			continue;
		}
		if (now_ms < slot.stored_at_ms || now_ms - slot.stored_at_ms >= HandshakeCacheLifetimeMs) {
			slot = Slot{};
			--m_size;
		}
	}
}

void ProducerHandshakeCache::reset() noexcept
{
	m_slots = {};
	m_size = 0;
}

bool ProducerHandshakeCache::has_capacity(std::uint64_t now_ms) noexcept
{
	expire(now_ms);
	return m_size < HandshakeCacheCapacity;
}

HandshakeCacheLookupResult ProducerHandshakeCache::lookup(const EndpointKey& endpoint,
	std::uint64_t client_nonce,
	std::uint64_t now_ms,
	CachedWelcomeResponseView& response) noexcept
{
	if (!endpoint.is_valid() || client_nonce == 0) {
		return HandshakeCacheLookupResult::InvalidKey;
	}
	expire(now_ms);
	for (const auto& slot : m_slots) {
		if (slot.occupied && slot.client_nonce == client_nonce && slot.endpoint == endpoint) {
			response = view(slot);
			return HandshakeCacheLookupResult::Found;
		}
	}
	return HandshakeCacheLookupResult::Miss;
}

HandshakeCacheStoreResult ProducerHandshakeCache::store(const EndpointKey& endpoint,
	std::uint64_t client_nonce,
	const TelemetryDatagramHeader& welcome_header,
	ByteView encoded_welcome_payload,
	std::uint64_t now_ms,
	CachedWelcomeResponseView& response) noexcept
{
	CachedWelcomeResponseView existing;
	const auto lookup_result = lookup(endpoint, client_nonce, now_ms, existing);
	if (lookup_result == HandshakeCacheLookupResult::InvalidKey) {
		return HandshakeCacheStoreResult::InvalidKey;
	}
	if (lookup_result == HandshakeCacheLookupResult::Found) {
		response = existing;
		return HandshakeCacheStoreResult::Existing;
	}

	WelcomePayload decoded;
	if (!validate_cached_welcome(endpoint, client_nonce, welcome_header, encoded_welcome_payload, decoded)) {
		return HandshakeCacheStoreResult::InvalidWelcome;
	}
	if (m_size == HandshakeCacheCapacity) {
		return HandshakeCacheStoreResult::CapacityReached;
	}

	for (auto& slot : m_slots) {
		if (slot.occupied) {
			continue;
		}
		slot.occupied = true;
		slot.endpoint = endpoint;
		slot.client_nonce = client_nonce;
		slot.status = decoded.status;
		slot.session_id = welcome_header.session_id;
		slot.flags = welcome_header.flags;
		slot.message_id = welcome_header.message_id;
		slot.fragment_count = welcome_header.fragment_count;
		slot.message_crc32 = welcome_header.message_crc32;
		slot.stored_at_ms = now_ms;
		slot.payload_size = static_cast<std::uint16_t>(encoded_welcome_payload.size);
		std::copy_n(encoded_welcome_payload.data, encoded_welcome_payload.size, slot.payload.data());
		++m_size;
		response = view(slot);
		return HandshakeCacheStoreResult::Stored;
	}
	return HandshakeCacheStoreResult::CapacityReached;
}

SessionModelResult ClientSessionModel::begin_negotiation(const EndpointKey& producer_endpoint,
	const HelloPayload& hello,
	std::uint64_t now_ms) noexcept
{
	if (!producer_endpoint.is_valid() || validate_hello_payload(hello) != ValidationError::None ||
		validate_emittable_capability_offer(CapabilityPeerRole::Client, hello.advertised_capabilities) !=
			ValidationError::None) {
		return SessionModelResult::InvalidArgument;
	}

	if (active()) {
		purge_active_session();
	}
	m_producer_endpoint = producer_endpoint;
	m_pending_nonce = hello.client_nonce;
	m_pending_t0_us = hello.client_send_t0_us;
	m_pending_client_capabilities = hello.advertised_capabilities;
	m_negotiation_started_ms = now_ms;
	m_state = ClientSessionState::Negotiating;
	return SessionModelResult::Applied;
}

ClientWelcomeResult ClientSessionModel::receive_welcome(const TelemetryDatagramHeader& header,
	const EndpointKey& source_endpoint,
	const WelcomePayload& welcome,
	std::uint64_t client_receive_t3_us,
	std::uint64_t now_ms) noexcept
{
	if (m_state != ClientSessionState::Negotiating) {
		return ClientWelcomeResult::InvalidState;
	}
	if (validate_welcome_payload(welcome) != ValidationError::None ||
		validate_fragment_layout(header) != ValidationError::None) {
		return ClientWelcomeResult::InvalidPayload;
	}
	if (!source_endpoint.is_valid() || source_endpoint != m_producer_endpoint) {
		return ClientWelcomeResult::EndpointMismatch;
	}
	if (welcome.client_nonce != m_pending_nonce || welcome.client_send_t0_us != m_pending_t0_us) {
		return ClientWelcomeResult::NonceMismatch;
	}

	TelemetrySessionContext pending_context;
	pending_context.local_role = LocalEndpointRole::Client;
	pending_context.peer_endpoint = m_producer_endpoint;
	pending_context.active_session_id = 0;
	if (validate_welcome_logical_context(header, welcome.status, pending_context) != ValidationError::None) {
		return ClientWelcomeResult::SessionMismatch;
	}

	if (welcome_is_rejected(welcome.status)) {
		m_state = ClientSessionState::Disconnected;
		m_producer_endpoint = EndpointKey{};
		clear_pending_negotiation();
		return ClientWelcomeResult::Rejected;
	}
	if (welcome.status != WelcomeStatus::Accepted || header.session_id == 0) {
		return ClientWelcomeResult::InvalidPayload;
	}

	CapabilitySessionState capabilities;
	if (initialize_capability_session(m_pending_client_capabilities,
			welcome.producer_capabilities,
			welcome.active_capabilities,
			capabilities) != ValidationError::None) {
		return ClientWelcomeResult::CapabilityMismatch;
	}

	ClockSample clock_sample;
	const FourTimestampExchange exchange{welcome.client_send_t0_us,
		welcome.producer_receive_t1_us,
		welcome.producer_send_t2_us,
		client_receive_t3_us};
	if (compute_clock_sample(exchange, clock_sample) != ClockSampleResult::Valid) {
		return ClientWelcomeResult::ClockSampleInvalid;
	}
	OrientedClockSample oriented_sample;
	if (orient_clock_sample(clock_sample, LocalClockRole::Initiator, oriented_sample) !=
		ClockOffsetConversionResult::Converted) {
		return ClientWelcomeResult::ClockSampleInvalid;
	}

	std::uint32_t stale_timeout_ms = 0;
	std::uint32_t disconnect_timeout_ms = 0;
	if (compute_clock_stale_timeout_ms(welcome.heartbeat_interval_ms, stale_timeout_ms) !=
			ClockTimeoutResult::Computed ||
		compute_session_disconnect_timeout_ms(welcome.heartbeat_interval_ms, disconnect_timeout_ms) !=
			ClockTimeoutResult::Computed) {
		return ClientWelcomeResult::InvalidPayload;
	}

	if (active()) {
		purge_active_session();
	}
	m_session_id = header.session_id;
	m_client_nonce = welcome.client_nonce;
	m_producer_id = welcome.producer_id;
	m_selected_major = welcome.selected_major;
	m_selected_minor = welcome.selected_minor;
	m_selected_visibility_mode = welcome.selected_visibility_mode;
	m_heartbeat_interval_ms = welcome.heartbeat_interval_ms;
	m_stale_timeout_ms = stale_timeout_ms;
	m_disconnect_timeout_ms = disconnect_timeout_ms;
	m_last_valid_network_activity_ms = now_ms;
	m_last_valid_clock_response_ms = now_ms;
	m_welcome_identity = WelcomeMessageIdentity{header.session_id,
		source_endpoint,
		header.message_id,
		header.fragment_count,
		header.message_crc32};
	m_synchronization.reset();
	m_clock_filter.reset();
	m_clock_filter.add_sample(oriented_sample);
	(void)m_probes.reset_session(m_session_id);
	m_capabilities = capabilities;
	m_state = ClientSessionState::Synchronizing;
	clear_pending_negotiation();
	return ClientWelcomeResult::Accepted;
}

SessionModelResult ClientSessionModel::on_session_begin_applied(const SessionBeginPayload& payload) noexcept
{
	if (!active() || m_state != ClientSessionState::Synchronizing) {
		return SessionModelResult::InvalidState;
	}
	const auto result = m_synchronization.apply_session_begin(payload);
	promote_if_synchronized();
	return result;
}

SessionModelResult ClientSessionModel::on_manifest_applied(std::uint32_t manifest_id) noexcept
{
	if (!active() || m_state != ClientSessionState::Synchronizing) {
		return SessionModelResult::InvalidState;
	}
	const auto result = m_synchronization.apply_manifest(manifest_id);
	promote_if_synchronized();
	return result;
}

SessionModelResult ClientSessionModel::on_full_snapshot_applied(std::uint32_t snapshot_id,
	std::uint32_t required_manifest_id) noexcept
{
	if (!active() || m_state != ClientSessionState::Synchronizing) {
		return SessionModelResult::InvalidState;
	}
	const auto result = m_synchronization.apply_full_snapshot(snapshot_id, required_manifest_id);
	promote_if_synchronized();
	return result;
}

SessionModelResult ClientSessionModel::begin_resynchronization(std::uint32_t required_manifest_id,
	std::uint32_t expected_snapshot_id,
	bool required_manifest_already_applied) noexcept
{
	if (!active() || (m_state != ClientSessionState::Live && m_state != ClientSessionState::Stale &&
						 m_state != ClientSessionState::Synchronizing)) {
		return SessionModelResult::InvalidState;
	}
	const auto result = m_synchronization.begin_resynchronization(required_manifest_id,
		expected_snapshot_id,
		required_manifest_already_applied);
	if (result == SessionModelResult::Applied) {
		m_state = ClientSessionState::Synchronizing;
	}
	return result;
}

SessionModelResult ClientSessionModel::mark_stale() noexcept
{
	if (!active()) {
		return SessionModelResult::InvalidState;
	}
	// Stale is also the clock-validity boundary. Repeated stale causes (most
	// importantly a monotonic discontinuity) must invalidate a filter that may
	// have been rebuilt by later heartbeat responses while state stayed Stale.
	m_clock_filter.invalidate();
	if (m_state == ClientSessionState::Stale) {
		return SessionModelResult::NoChange;
	}
	if (m_state != ClientSessionState::Synchronizing && m_state != ClientSessionState::Live) {
		return SessionModelResult::InvalidState;
	}
	m_state = ClientSessionState::Stale;
	return SessionModelResult::Applied;
}

SessionModelResult ClientSessionModel::disconnect() noexcept
{
	if (!active() && m_state == ClientSessionState::Disconnected) {
		return SessionModelResult::NoChange;
	}
	if (active()) {
		purge_active_session();
	} else {
		clear_pending_negotiation();
		m_producer_endpoint = EndpointKey{};
		m_state = ClientSessionState::Disconnected;
	}
	return SessionModelResult::Applied;
}

bool ClientSessionModel::note_valid_session_datagram(std::uint64_t now_ms) noexcept
{
	if (!active()) {
		return false;
	}
	if (now_ms < m_last_valid_network_activity_ms) {
		m_last_valid_network_activity_ms = now_ms;
		m_last_valid_clock_response_ms = now_ms;
		(void)mark_stale();
		return false;
	}
	m_last_valid_network_activity_ms = now_ms;
	return true;
}

bool ClientSessionModel::note_valid_heartbeat_response(const OrientedClockSample& sample, std::uint64_t now_ms) noexcept
{
	if (!note_valid_session_datagram(now_ms) || now_ms < m_last_valid_clock_response_ms) {
		(void)mark_stale();
		return false;
	}
	m_clock_filter.add_sample(sample);
	m_last_valid_clock_response_ms = now_ms;
	return true;
}

SessionTimeoutAction ClientSessionModel::poll_timeouts(std::uint64_t now_ms) noexcept
{
	if (!active()) {
		return SessionTimeoutAction::None;
	}
	std::uint64_t network_elapsed = 0;
	std::uint64_t clock_elapsed = 0;
	if (!elapsed_since(now_ms, m_last_valid_network_activity_ms, network_elapsed) ||
		!elapsed_since(now_ms, m_last_valid_clock_response_ms, clock_elapsed)) {
		m_last_valid_network_activity_ms = now_ms;
		m_last_valid_clock_response_ms = now_ms;
		(void)mark_stale();
		return SessionTimeoutAction::MonotonicDiscontinuity;
	}
	if (network_elapsed >= m_disconnect_timeout_ms) {
		(void)disconnect();
		return SessionTimeoutAction::Disconnected;
	}
	if (clock_elapsed >= m_stale_timeout_ms && m_state != ClientSessionState::Stale) {
		(void)mark_stale();
		return SessionTimeoutAction::BecameStale;
	}
	return SessionTimeoutAction::None;
}

TelemetrySessionContext ClientSessionModel::context() const noexcept
{
	return TelemetrySessionContext{LocalEndpointRole::Client, m_producer_endpoint, m_session_id};
}

bool ClientSessionModel::welcome_message_identity(WelcomeMessageIdentity& identity) const noexcept
{
	if (!active() || m_welcome_identity.session_id != m_session_id) {
		return false;
	}
	identity = m_welcome_identity;
	return true;
}

void ClientSessionModel::purge_active_session() noexcept
{
	const auto old_session_id = m_session_id;
	if (old_session_id != 0 && m_purger != nullptr) {
		m_purger->purge_session_resources(old_session_id);
	}
	m_session_id = 0;
	m_client_nonce = 0;
	m_producer_id = 0;
	m_selected_major = 0;
	m_selected_minor = 0;
	m_selected_visibility_mode = VisibilityMode::Cockpit;
	m_producer_endpoint = EndpointKey{};
	m_heartbeat_interval_ms = 0;
	m_stale_timeout_ms = 0;
	m_disconnect_timeout_ms = 0;
	m_last_valid_network_activity_ms = 0;
	m_last_valid_clock_response_ms = 0;
	m_welcome_identity = WelcomeMessageIdentity{};
	m_synchronization.reset();
	m_clock_filter.reset();
	(void)m_probes.reset_session(0);
	m_capabilities.reset();
	clear_pending_negotiation();
	m_state = ClientSessionState::Disconnected;
}

void ClientSessionModel::clear_pending_negotiation() noexcept
{
	m_pending_nonce = 0;
	m_pending_t0_us = 0;
	m_pending_client_capabilities = 0;
	m_negotiation_started_ms = 0;
}

void ClientSessionModel::promote_if_synchronized() noexcept
{
	if (m_state == ClientSessionState::Synchronizing && m_synchronization.ready_for_live()) {
		m_state = ClientSessionState::Live;
	}
}

ProducerHandshakeBeginResult ProducerSessionModel::begin_handshake(const EndpointKey& client_endpoint,
	const HelloPayload& hello,
	std::uint64_t received_datagram_bytes,
	std::uint64_t now_ms,
	CachedWelcomeResponseView& cached_response) noexcept
{
	if (!client_endpoint.is_valid()) {
		return ProducerHandshakeBeginResult::InvalidEndpoint;
	}
	if (!structurally_valid_received_hello(hello)) {
		return ProducerHandshakeBeginResult::InvalidHello;
	}
	if (m_state == ProducerSessionState::Closing) {
		return ProducerHandshakeBeginResult::InvalidState;
	}

	const auto lookup = m_handshake_cache.lookup(client_endpoint, hello.client_nonce, now_ms, cached_response);
	if (lookup == HandshakeCacheLookupResult::Found) {
		// A cached result is a replay, never a session resurrection. After the
		// original session has been purged, retain only enough endpoint-local
		// accounting state to resend the exact cached Welcome within 3x.
		if (!m_client_endpoint.is_valid() &&
			(!m_pending_endpoint.is_valid() || m_pending_endpoint != client_endpoint)) {
			m_pending_endpoint = client_endpoint;
			m_preproof_bytes_received = 0;
			m_preproof_bytes_sent = 0;
		}
		(void)note_preproof_bytes_received(client_endpoint, received_datagram_bytes);
		return ProducerHandshakeBeginResult::Cached;
	}
	if (lookup == HandshakeCacheLookupResult::InvalidKey) {
		return ProducerHandshakeBeginResult::InvalidHello;
	}
	if (!m_handshake_cache.has_capacity(now_ms)) {
		return ProducerHandshakeBeginResult::CacheFull;
	}

	if (active()) {
		purge_active_session();
	}
	m_pending_endpoint = client_endpoint;
	m_pending_nonce = hello.client_nonce;
	m_pending_t0_us = hello.client_send_t0_us;
	m_pending_client_capabilities = hello.advertised_capabilities;
	m_preproof_bytes_received = 0;
	m_preproof_bytes_sent = 0;
	saturating_add(m_preproof_bytes_received, received_datagram_bytes);
	m_state = ProducerSessionState::Negotiating;
	return ProducerHandshakeBeginResult::Ready;
}

ProducerHandshakeCompleteResult ProducerSessionModel::complete_handshake(const TelemetryDatagramHeader& welcome_header,
	ByteView encoded_welcome_payload,
	std::uint64_t now_ms,
	CachedWelcomeResponseView& cached_response) noexcept
{
	if (m_state != ProducerSessionState::Negotiating || !m_pending_endpoint.is_valid() || m_pending_nonce == 0) {
		return ProducerHandshakeCompleteResult::InvalidState;
	}

	WelcomePayload welcome;
	if (!validate_cached_welcome(m_pending_endpoint,
			m_pending_nonce,
			welcome_header,
			encoded_welcome_payload,
			welcome) ||
		welcome.client_send_t0_us != m_pending_t0_us) {
		return ProducerHandshakeCompleteResult::InvalidWelcome;
	}

	CapabilitySessionState capabilities;
	std::uint32_t stale_timeout_ms = 0;
	std::uint32_t disconnect_timeout_ms = 0;
	if (welcome.status == WelcomeStatus::Accepted) {
		if (initialize_capability_session(m_pending_client_capabilities,
				welcome.producer_capabilities,
				welcome.active_capabilities,
				capabilities) != ValidationError::None) {
			return ProducerHandshakeCompleteResult::CapabilityMismatch;
		}
		if (compute_clock_stale_timeout_ms(welcome.heartbeat_interval_ms, stale_timeout_ms) !=
				ClockTimeoutResult::Computed ||
			compute_session_disconnect_timeout_ms(welcome.heartbeat_interval_ms, disconnect_timeout_ms) !=
				ClockTimeoutResult::Computed) {
			return ProducerHandshakeCompleteResult::InvalidWelcome;
		}
	}

	const auto cache_result = m_handshake_cache.store(m_pending_endpoint,
		m_pending_nonce,
		welcome_header,
		encoded_welcome_payload,
		now_ms,
		cached_response);
	if (cache_result == HandshakeCacheStoreResult::Existing) {
		return ProducerHandshakeCompleteResult::Existing;
	}
	if (cache_result == HandshakeCacheStoreResult::CapacityReached) {
		return ProducerHandshakeCompleteResult::CacheFull;
	}
	if (cache_result != HandshakeCacheStoreResult::Stored) {
		return ProducerHandshakeCompleteResult::InvalidWelcome;
	}

	m_client_endpoint = m_pending_endpoint;
	if (welcome.status != WelcomeStatus::Accepted) {
		m_session_id = 0;
		m_client_nonce = 0;
		m_producer_id = 0;
		m_selected_major = 0;
		m_selected_minor = 0;
		m_selected_visibility_mode = VisibilityMode::Cockpit;
		m_heartbeat_interval_ms = 0;
		m_stale_timeout_ms = 0;
		m_disconnect_timeout_ms = 0;
		m_last_valid_network_activity_ms = 0;
		m_last_valid_clock_response_ms = 0;
		m_welcome_proof_received = false;
		m_clock_timeout_reported = false;
		m_expected_welcome = WelcomeMessageIdentity{};
		m_synchronization.reset();
		m_clock_filter.reset();
		(void)m_probes.reset_session(0);
		m_capabilities.reset();
		clear_pending_handshake();
		m_state = ProducerSessionState::Rejected;
		return ProducerHandshakeCompleteResult::Rejected;
	}

	m_session_id = welcome_header.session_id;
	m_client_nonce = welcome.client_nonce;
	m_producer_id = welcome.producer_id;
	m_selected_major = welcome.selected_major;
	m_selected_minor = welcome.selected_minor;
	m_selected_visibility_mode = welcome.selected_visibility_mode;
	m_heartbeat_interval_ms = welcome.heartbeat_interval_ms;
	m_stale_timeout_ms = stale_timeout_ms;
	m_disconnect_timeout_ms = disconnect_timeout_ms;
	m_last_valid_network_activity_ms = now_ms;
	m_last_valid_clock_response_ms = now_ms;
	m_welcome_proof_received = false;
	m_clock_timeout_reported = false;
	m_expected_welcome = WelcomeMessageIdentity{welcome_header.session_id,
		m_client_endpoint,
		welcome_header.message_id,
		welcome_header.fragment_count,
		welcome_header.message_crc32};
	m_synchronization.reset();
	m_clock_filter.reset();
	(void)m_probes.reset_session(m_session_id);
	m_capabilities = capabilities;
	clear_pending_handshake();
	m_state = ProducerSessionState::AwaitingWelcomeProof;
	return ProducerHandshakeCompleteResult::AwaitingProof;
}

bool ProducerSessionModel::note_preproof_bytes_received(const EndpointKey& endpoint, std::uint64_t bytes) noexcept
{
	if (m_welcome_proof_received || !endpoint.is_valid()) {
		return false;
	}
	const auto expected_endpoint = m_state == ProducerSessionState::Negotiating
									   ? m_pending_endpoint
									   : (m_client_endpoint.is_valid() ? m_client_endpoint : m_pending_endpoint);
	if (!expected_endpoint.is_valid() || endpoint != expected_endpoint) {
		return false;
	}
	saturating_add(m_preproof_bytes_received, bytes);
	return true;
}

SessionModelResult ProducerSessionModel::try_account_preproof_send(const EndpointKey& endpoint,
	std::uint64_t bytes) noexcept
{
	if (m_welcome_proof_received) {
		return SessionModelResult::NoChange;
	}
	const auto expected_endpoint = m_state == ProducerSessionState::Negotiating
									   ? m_pending_endpoint
									   : (m_client_endpoint.is_valid() ? m_client_endpoint : m_pending_endpoint);
	if (!endpoint.is_valid() || !expected_endpoint.is_valid() || endpoint != expected_endpoint) {
		return SessionModelResult::EndpointMismatch;
	}

	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	const auto allowed = m_preproof_bytes_received > maximum / 3U ? maximum : m_preproof_bytes_received * 3U;
	if (bytes > maximum - m_preproof_bytes_sent || m_preproof_bytes_sent + bytes > allowed) {
		return SessionModelResult::AntiAmplificationLimit;
	}
	m_preproof_bytes_sent += bytes;
	return SessionModelResult::Applied;
}

WelcomeProofValidationResult ProducerSessionModel::accept_welcome_applied_ack(const WelcomeAckCandidate& candidate,
	std::uint64_t now_ms,
	ValidatedWelcomeAppliedProof& proof) noexcept
{
	if (m_state != ProducerSessionState::AwaitingWelcomeProof || !active()) {
		return WelcomeProofValidationResult::InvalidState;
	}
	const auto validation = validate_welcome_applied_proof(candidate, m_expected_welcome, proof);
	if (validation != WelcomeProofValidationResult::Valid) {
		return validation;
	}

	if (now_ms < m_last_valid_network_activity_ms) {
		m_clock_filter.invalidate();
		m_last_valid_clock_response_ms = now_ms;
	}
	m_last_valid_network_activity_ms = now_ms;
	m_welcome_proof_received = true;
	m_state = ProducerSessionState::Synchronizing;
	return WelcomeProofValidationResult::Valid;
}

bool ProducerSessionModel::can_send_heavy_data() const noexcept
{
	return active() && m_welcome_proof_received &&
		   (m_state == ProducerSessionState::Synchronizing || m_state == ProducerSessionState::Live);
}

SessionModelResult ProducerSessionModel::on_session_begin_applied(const SessionBeginPayload& payload) noexcept
{
	if (!can_send_heavy_data() || m_state != ProducerSessionState::Synchronizing) {
		return SessionModelResult::InvalidState;
	}
	const auto result = m_synchronization.apply_session_begin(payload);
	promote_if_synchronized();
	return result;
}

SessionModelResult ProducerSessionModel::on_manifest_applied(std::uint32_t manifest_id) noexcept
{
	if (!can_send_heavy_data() || m_state != ProducerSessionState::Synchronizing) {
		return SessionModelResult::InvalidState;
	}
	const auto result = m_synchronization.apply_manifest(manifest_id);
	promote_if_synchronized();
	return result;
}

SessionModelResult ProducerSessionModel::on_full_snapshot_applied(std::uint32_t snapshot_id,
	std::uint32_t required_manifest_id) noexcept
{
	if (!can_send_heavy_data() || m_state != ProducerSessionState::Synchronizing) {
		return SessionModelResult::InvalidState;
	}
	const auto result = m_synchronization.apply_full_snapshot(snapshot_id, required_manifest_id);
	promote_if_synchronized();
	return result;
}

SessionModelResult ProducerSessionModel::begin_resynchronization(std::uint32_t required_manifest_id,
	std::uint32_t expected_snapshot_id,
	bool required_manifest_already_applied) noexcept
{
	if (!can_send_heavy_data() ||
		(m_state != ProducerSessionState::Live && m_state != ProducerSessionState::Synchronizing)) {
		return SessionModelResult::InvalidState;
	}
	const auto result = m_synchronization.begin_resynchronization(required_manifest_id,
		expected_snapshot_id,
		required_manifest_already_applied);
	if (result == SessionModelResult::Applied) {
		m_state = ProducerSessionState::Synchronizing;
	}
	return result;
}

SessionModelResult ProducerSessionModel::begin_closing() noexcept
{
	if (!active()) {
		return SessionModelResult::InvalidState;
	}
	m_state = ProducerSessionState::Closing;
	return SessionModelResult::Applied;
}

SessionModelResult ProducerSessionModel::finish_closing() noexcept
{
	if (m_state != ProducerSessionState::Closing) {
		return SessionModelResult::InvalidState;
	}
	if (active()) {
		purge_active_session();
	} else {
		m_state = ProducerSessionState::Listening;
	}
	return SessionModelResult::Applied;
}

SessionModelResult ProducerSessionModel::return_to_listening() noexcept
{
	if (m_state != ProducerSessionState::Rejected) {
		return SessionModelResult::InvalidState;
	}
	m_client_endpoint = EndpointKey{};
	m_preproof_bytes_received = 0;
	m_preproof_bytes_sent = 0;
	m_state = ProducerSessionState::Listening;
	return SessionModelResult::Applied;
}

bool ProducerSessionModel::note_valid_session_datagram(std::uint64_t now_ms) noexcept
{
	if (!active()) {
		return false;
	}
	if (now_ms < m_last_valid_network_activity_ms) {
		m_last_valid_network_activity_ms = now_ms;
		m_last_valid_clock_response_ms = now_ms;
		m_clock_filter.invalidate();
		m_clock_timeout_reported = true;
		return false;
	}
	m_last_valid_network_activity_ms = now_ms;
	return true;
}

bool ProducerSessionModel::note_valid_heartbeat_response(const OrientedClockSample& sample,
	std::uint64_t now_ms) noexcept
{
	if (!note_valid_session_datagram(now_ms) || now_ms < m_last_valid_clock_response_ms) {
		m_clock_filter.invalidate();
		m_clock_timeout_reported = true;
		return false;
	}
	m_clock_filter.add_sample(sample);
	m_last_valid_clock_response_ms = now_ms;
	m_clock_timeout_reported = false;
	return true;
}

SessionTimeoutAction ProducerSessionModel::poll_timeouts(std::uint64_t now_ms) noexcept
{
	if (!active()) {
		return SessionTimeoutAction::None;
	}
	std::uint64_t network_elapsed = 0;
	std::uint64_t clock_elapsed = 0;
	if (!elapsed_since(now_ms, m_last_valid_network_activity_ms, network_elapsed) ||
		!elapsed_since(now_ms, m_last_valid_clock_response_ms, clock_elapsed)) {
		m_last_valid_network_activity_ms = now_ms;
		m_last_valid_clock_response_ms = now_ms;
		m_clock_filter.invalidate();
		m_clock_timeout_reported = true;
		return SessionTimeoutAction::MonotonicDiscontinuity;
	}
	if (network_elapsed >= m_disconnect_timeout_ms) {
		purge_active_session();
		m_state = ProducerSessionState::Closing;
		return SessionTimeoutAction::Closing;
	}
	if (clock_elapsed >= m_stale_timeout_ms && !m_clock_timeout_reported) {
		m_clock_filter.invalidate();
		m_clock_timeout_reported = true;
		return SessionTimeoutAction::ClockInvalidated;
	}
	return SessionTimeoutAction::None;
}

TelemetrySessionContext ProducerSessionModel::context() const noexcept
{
	const auto endpoint = m_state == ProducerSessionState::Negotiating && m_pending_endpoint.is_valid()
							  ? m_pending_endpoint
							  : (m_client_endpoint.is_valid() ? m_client_endpoint : m_pending_endpoint);
	return TelemetrySessionContext{LocalEndpointRole::Producer, endpoint, m_session_id};
}

void ProducerSessionModel::purge_active_session() noexcept
{
	const auto old_session_id = m_session_id;
	if (old_session_id != 0 && m_purger != nullptr) {
		m_purger->purge_session_resources(old_session_id);
	}
	m_session_id = 0;
	m_client_nonce = 0;
	m_producer_id = 0;
	m_selected_major = 0;
	m_selected_minor = 0;
	m_selected_visibility_mode = VisibilityMode::Cockpit;
	m_client_endpoint = EndpointKey{};
	m_heartbeat_interval_ms = 0;
	m_stale_timeout_ms = 0;
	m_disconnect_timeout_ms = 0;
	m_last_valid_network_activity_ms = 0;
	m_last_valid_clock_response_ms = 0;
	m_welcome_proof_received = false;
	m_clock_timeout_reported = false;
	m_expected_welcome = WelcomeMessageIdentity{};
	m_preproof_bytes_received = 0;
	m_preproof_bytes_sent = 0;
	m_synchronization.reset();
	m_clock_filter.reset();
	(void)m_probes.reset_session(0);
	m_capabilities.reset();
	clear_pending_handshake();
	m_state = ProducerSessionState::Listening;
}

void ProducerSessionModel::clear_pending_handshake() noexcept
{
	m_pending_endpoint = EndpointKey{};
	m_pending_nonce = 0;
	m_pending_t0_us = 0;
	m_pending_client_capabilities = 0;
}

void ProducerSessionModel::promote_if_synchronized() noexcept
{
	if (m_state == ProducerSessionState::Synchronizing && m_synchronization.ready_for_live()) {
		m_state = ProducerSessionState::Live;
	}
}

} // namespace telemetry::protocol
