#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_session.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using namespace telemetry::protocol;

constexpr std::uint64_t AllClientCapabilities =
	CapabilityCommViewLocalAssets | CapabilityTargetVideoH264 | CapabilityUpdate;
constexpr std::uint64_t AllProducerCapabilities =
	CapabilityCommViewAuthoritativeSource | CapabilityTargetVideoRemoteRender | CapabilityUpdate;
constexpr std::uint64_t AllActiveCapabilities = VisualCapabilities | CapabilityUpdate;

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

template <typename Container>
MutableByteView mutable_byte_view(Container& bytes)
{
	return MutableByteView{static_cast<std::uint8_t*>(static_cast<void*>(bytes.data())), bytes.size()};
}

EndpointKey endpoint(std::uint8_t last_octet, std::uint16_t port)
{
	return EndpointKey::from_ipv4({127U, 0U, 0U, last_octet}, port);
}

HelloPayload hello(std::uint64_t nonce = 0x0102030405060708ULL, std::uint64_t t0_us = 1'000'000U)
{
	HelloPayload result;
	result.client_nonce = nonce;
	result.client_send_t0_us = t0_us;
	result.advertised_capabilities = AllClientCapabilities;
	result.requested_heartbeat_ms = 1000U;
	return result;
}

struct EncodedWelcome {
	WelcomePayload payload;
	TelemetryDatagramHeader header;
	std::vector<std::uint8_t> bytes;
};

EncodedWelcome welcome_for(const HelloPayload& request,
	WelcomeStatus status = WelcomeStatus::Accepted,
	std::uint64_t session_id = 0x1122334455667788ULL,
	std::uint32_t message_id = 71U,
	std::uint16_t heartbeat_ms = 1000U)
{
	EncodedWelcome result;
	result.payload.client_nonce = request.client_nonce;
	result.payload.client_send_t0_us = request.client_send_t0_us;
	result.payload.producer_receive_t1_us = request.client_send_t0_us + 20U;
	result.payload.producer_send_t2_us = request.client_send_t0_us + 30U;
	result.payload.status = status;
	result.payload.producer_id = 0xaabbccddeeff0011ULL;
	if (status == WelcomeStatus::Accepted) {
		result.payload.selected_major = VersionMajor;
		result.payload.selected_minor = VersionMinor;
		result.payload.selected_visibility_mode = request.requested_visibility_mode;
		result.payload.producer_capabilities = AllProducerCapabilities;
		result.payload.active_capabilities = AllActiveCapabilities;
		result.payload.heartbeat_interval_ms = heartbeat_ms;
		result.payload.reliable_reassembly_timeout_ms = ReliableReassemblyTimeoutV1Ms;
	}

	result.bytes.assign(WelcomePayloadPrefixSize, 0xa5U);
	std::size_t written = std::numeric_limits<std::size_t>::max();
	EXPECT_EQ(ValidationError::None, encode_welcome_payload(result.payload, mutable_byte_view(result.bytes), written));
	EXPECT_EQ(result.bytes.size(), written);

	// Feed the models the decoded view, not the construction object, so this
	// harness exercises the real Welcome codec and its bounded payload view.
	WelcomePayload decoded;
	EXPECT_EQ(ValidationError::None, decode_welcome_payload(byte_view(result.bytes), decoded));
	result.payload = decoded;

	result.header.message_type = MessageType::Welcome;
	result.header.flags = status == WelcomeStatus::Accepted ? MessageFlagAckRequired : MessageFlagNone;
	result.header.session_id = status == WelcomeStatus::Accepted ? session_id : 0U;
	result.header.packet_sequence = 11U;
	result.header.sent_time_us = result.payload.producer_send_t2_us;
	result.header.message_id = message_id;
	result.header.fragment_count = 1U;
	result.header.message_size = static_cast<std::uint32_t>(result.bytes.size());
	result.header.payload_size = static_cast<std::uint16_t>(result.bytes.size());
	result.header.message_crc32 = crc32_iso_hdlc(byte_view(result.bytes));
	EXPECT_EQ(ValidationError::None, validate_fragment_layout(result.header));
	EXPECT_EQ(ValidationError::None, validate_message_crc(result.header, byte_view(result.bytes)));
	return result;
}

SessionBeginPayload session_begin(std::uint32_t snapshot_id = 51U, std::uint32_t manifest_id = 41U)
{
	SessionBeginPayload result;
	result.session_flags = SessionBeginFlagReadOnly | SessionBeginFlagMissionActive;
	if (manifest_id != 0U) {
		result.session_flags |= SessionBeginFlagManifestRequired;
	}
	result.producer_session_start_us = 900'000U;
	result.mission_instance_id = 99U;
	result.initial_snapshot_id = snapshot_id;
	result.required_manifest_id = manifest_id;
	return result;
}

WelcomeAckCandidate applied_ack(const EncodedWelcome& welcome, const EndpointKey& client_endpoint)
{
	WelcomeAckCandidate result;
	result.session_id = welcome.header.session_id;
	result.source_endpoint = client_endpoint;
	result.target_message_id = welcome.header.message_id;
	result.target_message_type = MessageType::Welcome;
	result.ack_flags = static_cast<std::uint8_t>(AckFlag::Validated) | static_cast<std::uint8_t>(AckFlag::Applied);
	result.target_fragment_count = welcome.header.fragment_count;
	result.target_message_crc32 = welcome.header.message_crc32;
	return result;
}

struct ResourcePurgerSpy final : SessionResourcePurger {
	TelemetryReassembler reassembler;
	TelemetryTransactionAssembler transactions;
	std::vector<std::uint64_t> purged_session_ids;

	void purge_session_resources(std::uint64_t session_id) noexcept override
	{
		purged_session_ids.push_back(session_id);
		reassembler.clear();
		transactions.clear();
	}

	void seed(std::uint64_t session_id)
	{
		std::vector<std::uint8_t> message(MaxFragmentPayload + 1U, 0x5aU);
		TelemetryFragmenter fragmenter;
		ASSERT_TRUE(TelemetryFragmenter::create(byte_view(message), MessageSizeClass::State, fragmenter));
		FragmentSlice slice;
		ASSERT_TRUE(fragmenter.fragment(0U, slice));
		DatagramView fragment;
		fragment.header.message_type = MessageType::Delta;
		fragment.header.flags = MessageFlagFragmented;
		fragment.header.session_id = session_id;
		fragment.header.frame_id = 3U;
		fragment.header.message_id = 501U;
		fragment.header.fragment_index = slice.fragment_index;
		fragment.header.fragment_count = slice.fragment_count;
		fragment.header.message_size = slice.message_size;
		fragment.header.fragment_offset = slice.fragment_offset;
		fragment.header.message_crc32 = slice.message_crc32;
		fragment.header.payload_size = static_cast<std::uint16_t>(slice.payload.size);
		fragment.payload = slice.payload;
		ReassembledMessage completed_message;
		ASSERT_EQ(ReassemblyResult::Accepted, reassembler.ingest(fragment, completed_message));

		const std::array<std::uint8_t, 2> all_records{{0x31U, 0x32U}};
		Sha256Digest digest{};
		ASSERT_TRUE(sha256(byte_view(all_records), digest));
		const std::array<std::uint8_t, 1> first_record{{all_records[0]}};
		TransactionPart part;
		part.session_id = session_id;
		part.message_type = MessageType::Manifest;
		part.transaction_id = 601U;
		part.message_id = 602U;
		part.part_count = 2U;
		part.transaction_size = static_cast<std::uint32_t>(all_records.size());
		part.transaction_sha256 = digest;
		part.producer_sample_time_us = 123U;
		part.kind_or_flags = static_cast<std::uint16_t>(ManifestKind::FullRequired);
		part.record_count = 1U;
		part.records = byte_view(first_record);
		CompletedTransaction completed_transaction;
		ASSERT_EQ(TransactionAssemblyResult::Accepted, transactions.ingest(part, 10U, completed_transaction));

		ASSERT_EQ(1U, reassembler.active_reassemblies(MessageSizeClass::State));
		ASSERT_EQ(1U, transactions.active_candidates());
	}
};

struct SessionHarness {
	EndpointKey client_endpoint = endpoint(2U, 7801U);
	EndpointKey producer_endpoint = endpoint(1U, 7800U);
	ResourcePurgerSpy client_purger;
	ResourcePurgerSpy producer_purger;
	ClientSessionModel client{client_purger};
	ProducerSessionModel producer{producer_purger};
	HelloPayload request = hello();
	EncodedWelcome welcome = welcome_for(request);
	CachedWelcomeResponseView cached;

	void negotiate(std::uint64_t now_ms = 100U, std::uint64_t received_bytes = 120U)
	{
		ASSERT_EQ(SessionModelResult::Applied, client.begin_negotiation(producer_endpoint, request, now_ms));
		ASSERT_EQ(ProducerHandshakeBeginResult::Ready,
			producer.begin_handshake(client_endpoint, request, received_bytes, now_ms, cached));
		ASSERT_EQ(ProducerHandshakeCompleteResult::AwaitingProof,
			producer.complete_handshake(welcome.header, byte_view(welcome.bytes), now_ms + 1U, cached));
		ASSERT_EQ(ClientWelcomeResult::Accepted,
			client.receive_welcome(welcome.header,
				producer_endpoint,
				welcome.payload,
				request.client_send_t0_us + 100U,
				now_ms + 1U));
	}

	void prove(std::uint64_t now_ms = 102U)
	{
		ValidatedWelcomeAppliedProof proof;
		ASSERT_EQ(WelcomeProofValidationResult::Valid,
			producer.accept_welcome_applied_ack(applied_ack(welcome, client_endpoint), now_ms, proof));
		ASSERT_TRUE(proof.valid());
	}

	void make_live()
	{
		const auto begin = session_begin();
		ASSERT_EQ(SessionModelResult::Applied, client.on_session_begin_applied(begin));
		ASSERT_EQ(SessionModelResult::Applied, producer.on_session_begin_applied(begin));
		ASSERT_EQ(SessionModelResult::Applied, client.on_manifest_applied(begin.required_manifest_id));
		ASSERT_EQ(SessionModelResult::Applied, producer.on_manifest_applied(begin.required_manifest_id));
		ASSERT_EQ(SessionModelResult::Applied,
			client.on_full_snapshot_applied(begin.initial_snapshot_id, begin.required_manifest_id));
		ASSERT_EQ(SessionModelResult::Applied,
			producer.on_full_snapshot_applied(begin.initial_snapshot_id, begin.required_manifest_id));
	}
};

TEST(TelemetryProtocolSession, SynchronizationGateRejectsInvalidOrderAndRequiresExactCommittedInputs)
{
	SessionSynchronizationGate gate;
	EXPECT_FALSE(gate.ready_for_live());
	EXPECT_EQ(SessionModelResult::InvalidState, gate.apply_manifest(41U));
	EXPECT_EQ(SessionModelResult::InvalidState, gate.apply_full_snapshot(51U, 41U));
	EXPECT_EQ(SessionModelResult::InvalidState, gate.begin_resynchronization(41U, 51U, false));

	auto begin = session_begin();
	auto invalid = begin;
	invalid.session_flags &= ~SessionBeginFlagReadOnly;
	EXPECT_EQ(SessionModelResult::InvalidArgument, gate.apply_session_begin(invalid));
	EXPECT_FALSE(gate.session_begin_applied());
	ASSERT_EQ(SessionModelResult::Applied, gate.apply_session_begin(begin));
	EXPECT_EQ(begin.required_manifest_id, gate.required_manifest_id());
	EXPECT_EQ(begin.initial_snapshot_id, gate.expected_snapshot_id());
	EXPECT_EQ(SessionModelResult::NoChange, gate.apply_session_begin(begin));
	begin.initial_snapshot_id += 1U;
	EXPECT_EQ(SessionModelResult::InvalidState, gate.apply_session_begin(begin));
	EXPECT_EQ(51U, gate.expected_snapshot_id());

	EXPECT_EQ(SessionModelResult::InvalidArgument, gate.apply_manifest(40U));
	EXPECT_FALSE(gate.required_manifest_applied());
	EXPECT_EQ(SessionModelResult::InvalidState, gate.apply_full_snapshot(51U, 41U));
	ASSERT_EQ(SessionModelResult::Applied, gate.apply_manifest(41U));
	EXPECT_EQ(SessionModelResult::NoChange, gate.apply_manifest(41U));
	EXPECT_EQ(SessionModelResult::InvalidArgument, gate.apply_full_snapshot(50U, 41U));
	EXPECT_EQ(SessionModelResult::InvalidArgument, gate.apply_full_snapshot(51U, 40U));
	ASSERT_EQ(SessionModelResult::Applied, gate.apply_full_snapshot(51U, 41U));
	EXPECT_TRUE(gate.ready_for_live());
	EXPECT_EQ(SessionModelResult::NoChange, gate.apply_full_snapshot(51U, 41U));
	EXPECT_EQ(SessionModelResult::InvalidArgument, gate.apply_full_snapshot(52U, 41U));

	ASSERT_EQ(SessionModelResult::Applied, gate.begin_resynchronization(42U, 52U, false));
	EXPECT_FALSE(gate.ready_for_live());
	EXPECT_EQ(0U, gate.applied_snapshot_id());
	EXPECT_EQ(SessionModelResult::InvalidState, gate.apply_full_snapshot(52U, 42U));
	ASSERT_EQ(SessionModelResult::Applied, gate.apply_manifest(42U));
	ASSERT_EQ(SessionModelResult::Applied, gate.apply_full_snapshot(52U, 42U));
	EXPECT_TRUE(gate.ready_for_live());

	gate.reset();
	ASSERT_EQ(SessionModelResult::Applied, gate.apply_session_begin(session_begin(0U, 0U)));
	EXPECT_EQ(SessionModelResult::InvalidArgument, gate.apply_full_snapshot(0U, 0U));
	ASSERT_EQ(SessionModelResult::Applied, gate.apply_full_snapshot(77U, 0U));
	EXPECT_EQ(77U, gate.expected_snapshot_id());
	EXPECT_TRUE(gate.ready_for_live());
}

TEST(TelemetryProtocolSession, WelcomeAppliedProofIsExactAndPreservesOutputOnEveryFailure)
{
	const auto request = hello();
	const auto encoded = welcome_for(request);
	const auto client_endpoint = endpoint(2U, 7801U);
	const WelcomeMessageIdentity expected{encoded.header.session_id,
		client_endpoint,
		encoded.header.message_id,
		encoded.header.fragment_count,
		encoded.header.message_crc32};
	auto candidate = applied_ack(encoded, client_endpoint);
	ValidatedWelcomeAppliedProof proof;
	ASSERT_EQ(WelcomeProofValidationResult::Valid, validate_welcome_applied_proof(candidate, expected, proof));
	ASSERT_TRUE(proof.valid());
	const auto canary = proof.identity();

	auto expect_failure = [&](WelcomeProofValidationResult expected_result) {
		EXPECT_EQ(expected_result, validate_welcome_applied_proof(candidate, expected, proof));
		EXPECT_TRUE(proof.valid());
		EXPECT_EQ(canary.session_id, proof.identity().session_id);
		EXPECT_EQ(canary.endpoint, proof.identity().endpoint);
		EXPECT_EQ(canary.message_id, proof.identity().message_id);
		EXPECT_EQ(canary.fragment_count, proof.identity().fragment_count);
		EXPECT_EQ(canary.message_crc32, proof.identity().message_crc32);
	};

	for (const auto flags : {std::uint8_t{0},
			 static_cast<std::uint8_t>(AckFlag::Validated),
			 static_cast<std::uint8_t>(AckFlag::Applied),
			 std::uint8_t{0x83U}}) {
		candidate = applied_ack(encoded, client_endpoint);
		candidate.ack_flags = flags;
		expect_failure(WelcomeProofValidationResult::InvalidAckFlags);
	}
	candidate = applied_ack(encoded, client_endpoint);
	candidate.session_id += 1U;
	expect_failure(WelcomeProofValidationResult::SessionMismatch);
	candidate = applied_ack(encoded, endpoint(3U, 7801U));
	expect_failure(WelcomeProofValidationResult::EndpointMismatch);
	candidate = applied_ack(encoded, client_endpoint);
	candidate.target_message_type = MessageType::Hello;
	expect_failure(WelcomeProofValidationResult::WrongMessageType);
	for (const auto mutation : {0, 1, 2}) {
		candidate = applied_ack(encoded, client_endpoint);
		if (mutation == 0) {
			candidate.target_message_id += 1U;
		} else if (mutation == 1) {
			candidate.target_fragment_count += 1U;
		} else {
			candidate.target_message_crc32 ^= 1U;
		}
		expect_failure(WelcomeProofValidationResult::MessageIdentityMismatch);
	}

	WelcomeMessageIdentity malformed = expected;
	malformed.message_id = 0U;
	candidate = applied_ack(encoded, client_endpoint);
	EXPECT_EQ(WelcomeProofValidationResult::MessageIdentityMismatch,
		validate_welcome_applied_proof(candidate, malformed, proof));
	malformed = expected;
	malformed.fragment_count = 2U;
	EXPECT_EQ(WelcomeProofValidationResult::MessageIdentityMismatch,
		validate_welcome_applied_proof(candidate, malformed, proof));
}

TEST(TelemetryProtocolSession, HandshakeCacheStoresExactWelcomeAndExpiresAtTheNormativeBoundary)
{
	ProducerHandshakeCache cache;
	const auto peer = endpoint(2U, 7801U);
	auto request = hello();
	auto encoded = welcome_for(request);
	CachedWelcomeResponseView view;
	ASSERT_EQ(HandshakeCacheStoreResult::Stored,
		cache.store(peer, request.client_nonce, encoded.header, byte_view(encoded.bytes), 100U, view));
	ASSERT_EQ(1U, cache.size());
	const auto original_bytes = encoded.bytes;
	ASSERT_EQ(original_bytes.size(), view.payload.size);
	EXPECT_TRUE(std::equal(original_bytes.begin(), original_bytes.end(), view.payload.begin()));

	encoded.bytes.assign(encoded.bytes.size(), 0xffU);
	CachedWelcomeResponseView lookup;
	ASSERT_EQ(HandshakeCacheLookupResult::Found,
		cache.lookup(peer, request.client_nonce, 100U + HandshakeCacheLifetimeMs - 1U, lookup));
	EXPECT_EQ(100U, lookup.stored_at_ms);
	EXPECT_EQ(view.session_id, lookup.session_id);
	EXPECT_EQ(view.message_id, lookup.message_id);
	EXPECT_EQ(view.message_crc32, lookup.message_crc32);
	EXPECT_TRUE(std::equal(original_bytes.begin(), original_bytes.end(), lookup.payload.begin()));
	EXPECT_EQ(HandshakeCacheLookupResult::Miss, cache.lookup(endpoint(3U, 7801U), request.client_nonce, 101U, lookup));
	EXPECT_EQ(HandshakeCacheLookupResult::Miss, cache.lookup(peer, request.client_nonce + 1U, 101U, lookup));
	EXPECT_EQ(HandshakeCacheLookupResult::Miss,
		cache.lookup(peer, request.client_nonce, 100U + HandshakeCacheLifetimeMs, lookup));
	EXPECT_EQ(0U, cache.size());

	lookup.session_id = 0xfeedU;
	EXPECT_EQ(HandshakeCacheLookupResult::InvalidKey, cache.lookup(EndpointKey{}, request.client_nonce, 0U, lookup));
	EXPECT_EQ(0xfeedU, lookup.session_id);
	EXPECT_EQ(HandshakeCacheLookupResult::InvalidKey, cache.lookup(peer, 0U, 0U, lookup));
	EXPECT_EQ(0xfeedU, lookup.session_id);
}

TEST(TelemetryProtocolSession, HandshakeCacheIsBoundedAndRetainsEveryOutcomeForExactlyTenSeconds)
{
	ProducerHandshakeCache cache;
	const auto peer = endpoint(2U, 7801U);
	CachedWelcomeResponseView output;
	for (std::size_t index = 0; index < HandshakeCacheCapacity; ++index) {
		auto request = hello(100U + index, 1'000U + index);
		const auto encoded =
			welcome_for(request, WelcomeStatus::Accepted, 1'000U + index, static_cast<std::uint32_t>(10U + index));
		ASSERT_EQ(HandshakeCacheStoreResult::Stored,
			cache.store(peer, request.client_nonce, encoded.header, byte_view(encoded.bytes), 100U, output));
	}
	EXPECT_EQ(HandshakeCacheCapacity, cache.size());
	EXPECT_FALSE(cache.has_capacity(100U));

	auto overflow_request = hello(999U, 9'999U);
	const auto overflow = welcome_for(overflow_request, WelcomeStatus::Accepted, 9'999U, 99U);
	output.session_id = 0xfeedU;
	EXPECT_EQ(HandshakeCacheStoreResult::CapacityReached,
		cache.store(peer, overflow_request.client_nonce, overflow.header, byte_view(overflow.bytes), 100U, output));
	EXPECT_EQ(0xfeedU, output.session_id);

	cache.expire(100U + HandshakeCacheLifetimeMs - 1U);
	EXPECT_EQ(HandshakeCacheCapacity, cache.size());
	EXPECT_FALSE(cache.has_capacity(100U + HandshakeCacheLifetimeMs - 1U));
	cache.expire(100U + HandshakeCacheLifetimeMs);
	EXPECT_EQ(0U, cache.size());
	EXPECT_TRUE(cache.has_capacity(100U + HandshakeCacheLifetimeMs));
	ASSERT_EQ(HandshakeCacheStoreResult::Stored,
		cache.store(peer,
			overflow_request.client_nonce,
			overflow.header,
			byte_view(overflow.bytes),
			100U + HandshakeCacheLifetimeMs,
			output));

	cache.reset();
	auto rejected_request = hello(444U, 4'444U);
	const auto rejected = welcome_for(rejected_request, WelcomeStatus::Busy, 0U, 44U);
	ASSERT_EQ(HandshakeCacheStoreResult::Stored,
		cache.store(peer, rejected_request.client_nonce, rejected.header, byte_view(rejected.bytes), 200U, output));
	EXPECT_EQ(1U, cache.size());
	EXPECT_EQ(HandshakeCacheLookupResult::Found,
		cache.lookup(peer, rejected_request.client_nonce, 200U + HandshakeCacheLifetimeMs - 1U, output));
	EXPECT_EQ(HandshakeCacheLookupResult::Miss,
		cache.lookup(peer, rejected_request.client_nonce, 200U + HandshakeCacheLifetimeMs, output));

	auto malformed = rejected.header;
	malformed.message_id = 0U;
	output.session_id = 0xfeedU;
	EXPECT_EQ(HandshakeCacheStoreResult::InvalidWelcome,
		cache.store(peer, 555U, malformed, byte_view(rejected.bytes), 201U, output));
	EXPECT_EQ(0xfeedU, output.session_id);
}

TEST(TelemetryProtocolSession, ProducerReturnsTheExactCachedResponseForDuplicatesAndRenegotiatesAtExpiry)
{
	ResourcePurgerSpy purger;
	ProducerSessionModel producer(purger);
	const auto peer = endpoint(2U, 7801U);
	const auto request = hello();
	const auto encoded = welcome_for(request);
	CachedWelcomeResponseView first;
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready, producer.begin_handshake(peer, request, 120U, 100U, first));
	ASSERT_EQ(ProducerHandshakeCompleteResult::AwaitingProof,
		producer.complete_handshake(encoded.header, byte_view(encoded.bytes), 101U, first));
	ASSERT_EQ(ProducerSessionState::AwaitingWelcomeProof, producer.state());
	ASSERT_EQ(encoded.bytes.size(), first.payload.size);
	const std::vector<std::uint8_t> first_payload(first.payload.begin(), first.payload.end());

	CachedWelcomeResponseView duplicate;
	ASSERT_EQ(ProducerHandshakeBeginResult::Cached, producer.begin_handshake(peer, request, 17U, 102U, duplicate));
	EXPECT_EQ(ProducerSessionState::AwaitingWelcomeProof, producer.state());
	EXPECT_EQ(first.endpoint, duplicate.endpoint);
	EXPECT_EQ(first.client_nonce, duplicate.client_nonce);
	EXPECT_EQ(first.status, duplicate.status);
	EXPECT_EQ(first.session_id, duplicate.session_id);
	EXPECT_EQ(first.flags, duplicate.flags);
	EXPECT_EQ(first.message_id, duplicate.message_id);
	EXPECT_EQ(first.fragment_count, duplicate.fragment_count);
	EXPECT_EQ(first.message_crc32, duplicate.message_crc32);
	EXPECT_EQ(first.stored_at_ms, duplicate.stored_at_ms);
	EXPECT_TRUE(std::equal(first_payload.begin(), first_payload.end(), duplicate.payload.begin()));
	EXPECT_EQ(137U, producer.preproof_bytes_received());

	duplicate.session_id = 0xfeedU;
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready,
		producer.begin_handshake(peer, request, 10U, 101U + HandshakeCacheLifetimeMs, duplicate));
	EXPECT_EQ(0xfeedU, duplicate.session_id);
	EXPECT_EQ(ProducerSessionState::Negotiating, producer.state());
	EXPECT_EQ(0U, producer.session_id());
	ASSERT_EQ(1U, purger.purged_session_ids.size());
	EXPECT_EQ(encoded.header.session_id, purger.purged_session_ids[0]);
	EXPECT_EQ(0U, producer.handshake_cache().size());
}

TEST(TelemetryProtocolSession, AcceptedAndRejectedHandshakePathsRemainDistinctAndRejectedDuplicatesAreCached)
{
	ResourcePurgerSpy client_purger;
	ResourcePurgerSpy producer_purger;
	ClientSessionModel client(client_purger);
	ProducerSessionModel producer(producer_purger);
	const auto client_endpoint = endpoint(2U, 7801U);
	const auto producer_endpoint = endpoint(1U, 7800U);
	const auto request = hello();
	const auto rejected = welcome_for(request, WelcomeStatus::Busy, 0U, 72U);
	CachedWelcomeResponseView response;

	ASSERT_EQ(SessionModelResult::Applied, client.begin_negotiation(producer_endpoint, request, 100U));
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready,
		producer.begin_handshake(client_endpoint, request, 120U, 100U, response));
	ASSERT_EQ(ProducerHandshakeCompleteResult::Rejected,
		producer.complete_handshake(rejected.header, byte_view(rejected.bytes), 101U, response));
	EXPECT_EQ(ProducerSessionState::Rejected, producer.state());
	EXPECT_EQ(0U, producer.session_id());
	EXPECT_FALSE(producer.can_send_heavy_data());
	EXPECT_EQ(ClientWelcomeResult::Rejected,
		client.receive_welcome(rejected.header,
			producer_endpoint,
			rejected.payload,
			request.client_send_t0_us + 100U,
			101U));
	EXPECT_EQ(ClientSessionState::Disconnected, client.state());

	CachedWelcomeResponseView duplicate;
	ASSERT_EQ(ProducerHandshakeBeginResult::Cached,
		producer.begin_handshake(client_endpoint, request, 1U, 102U, duplicate));
	EXPECT_EQ(ProducerSessionState::Rejected, producer.state());
	EXPECT_EQ(WelcomeStatus::Busy, duplicate.status);
	EXPECT_EQ(0U, duplicate.session_id);
	EXPECT_EQ(rejected.header.message_id, duplicate.message_id);
	EXPECT_EQ(rejected.header.message_crc32, duplicate.message_crc32);
	EXPECT_EQ(SessionModelResult::Applied, producer.return_to_listening());
	EXPECT_EQ(ProducerSessionState::Listening, producer.state());
	EXPECT_EQ(SessionModelResult::InvalidState, producer.return_to_listening());
	EXPECT_EQ(HandshakeCacheLookupResult::Found,
		producer.handshake_cache().lookup(client_endpoint, request.client_nonce, 103U, duplicate));
	EXPECT_TRUE(producer_purger.purged_session_ids.empty());
	EXPECT_TRUE(client_purger.purged_session_ids.empty());
}

TEST(TelemetryProtocolSession, AntiAmplificationBudgetIsCumulativeEndpointBoundAndOverflowSafe)
{
	ResourcePurgerSpy purger;
	ProducerSessionModel producer(purger);
	const auto peer = endpoint(2U, 7801U);
	const auto wrong_peer = endpoint(3U, 7801U);
	CachedWelcomeResponseView unused;
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready, producer.begin_handshake(peer, hello(), 100U, 10U, unused));
	EXPECT_EQ(100U, producer.preproof_bytes_received());
	EXPECT_EQ(SessionModelResult::EndpointMismatch, producer.try_account_preproof_send(wrong_peer, 1U));
	EXPECT_FALSE(producer.note_preproof_bytes_received(wrong_peer, 100U));
	EXPECT_EQ(0U, producer.preproof_bytes_sent());
	ASSERT_EQ(SessionModelResult::Applied, producer.try_account_preproof_send(peer, 299U));
	ASSERT_EQ(SessionModelResult::Applied, producer.try_account_preproof_send(peer, 1U));
	EXPECT_EQ(300U, producer.preproof_bytes_sent());
	EXPECT_EQ(SessionModelResult::AntiAmplificationLimit, producer.try_account_preproof_send(peer, 1U));
	EXPECT_EQ(300U, producer.preproof_bytes_sent());
	ASSERT_TRUE(producer.note_preproof_bytes_received(peer, 10U));
	ASSERT_EQ(SessionModelResult::Applied, producer.try_account_preproof_send(peer, 30U));
	EXPECT_EQ(110U, producer.preproof_bytes_received());
	EXPECT_EQ(330U, producer.preproof_bytes_sent());

	ResourcePurgerSpy overflow_purger;
	ProducerSessionModel overflow(overflow_purger);
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready,
		overflow.begin_handshake(peer, hello(99U), std::numeric_limits<std::uint64_t>::max(), 10U, unused));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), overflow.preproof_bytes_received());
	ASSERT_EQ(SessionModelResult::Applied,
		overflow.try_account_preproof_send(peer, std::numeric_limits<std::uint64_t>::max()));
	EXPECT_EQ(SessionModelResult::AntiAmplificationLimit, overflow.try_account_preproof_send(peer, 1U));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), overflow.preproof_bytes_sent());
	EXPECT_TRUE(overflow.note_preproof_bytes_received(peer, 1U));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), overflow.preproof_bytes_received());
}

TEST(TelemetryProtocolSession, NoHeavyDataIsAllowedUntilTheExactWelcomeAppliedProof)
{
	SessionHarness harness;
	harness.negotiate();
	EXPECT_EQ(ClientSessionState::Synchronizing, harness.client.state());
	EXPECT_EQ(ProducerSessionState::AwaitingWelcomeProof, harness.producer.state());
	EXPECT_FALSE(harness.producer.can_send_heavy_data());
	EXPECT_EQ(SessionModelResult::InvalidState, harness.producer.on_session_begin_applied(session_begin()));

	ValidatedWelcomeAppliedProof proof;
	auto candidate = applied_ack(harness.welcome, harness.client_endpoint);
	const auto expect_rejected = [&](WelcomeProofValidationResult expected) {
		EXPECT_EQ(expected, harness.producer.accept_welcome_applied_ack(candidate, 102U, proof));
		EXPECT_FALSE(proof.valid());
		EXPECT_EQ(ProducerSessionState::AwaitingWelcomeProof, harness.producer.state());
		EXPECT_FALSE(harness.producer.can_send_heavy_data());
	};
	candidate.session_id += 1U;
	expect_rejected(WelcomeProofValidationResult::SessionMismatch);
	candidate = applied_ack(harness.welcome, endpoint(3U, 7801U));
	expect_rejected(WelcomeProofValidationResult::EndpointMismatch);
	candidate = applied_ack(harness.welcome, harness.client_endpoint);
	candidate.target_message_id += 1U;
	expect_rejected(WelcomeProofValidationResult::MessageIdentityMismatch);
	candidate = applied_ack(harness.welcome, harness.client_endpoint);
	candidate.target_message_type = MessageType::Hello;
	expect_rejected(WelcomeProofValidationResult::WrongMessageType);
	candidate = applied_ack(harness.welcome, harness.client_endpoint);
	candidate.target_fragment_count += 1U;
	expect_rejected(WelcomeProofValidationResult::MessageIdentityMismatch);
	candidate = applied_ack(harness.welcome, harness.client_endpoint);
	candidate.target_message_crc32 ^= 1U;
	expect_rejected(WelcomeProofValidationResult::MessageIdentityMismatch);
	candidate = applied_ack(harness.welcome, harness.client_endpoint);
	candidate.ack_flags = static_cast<std::uint8_t>(AckFlag::Applied);
	expect_rejected(WelcomeProofValidationResult::InvalidAckFlags);

	harness.prove();
	EXPECT_EQ(ProducerSessionState::Synchronizing, harness.producer.state());
	EXPECT_TRUE(harness.producer.can_send_heavy_data());
	ValidatedWelcomeAppliedProof second;
	EXPECT_EQ(WelcomeProofValidationResult::InvalidState,
		harness.producer.accept_welcome_applied_ack(applied_ack(harness.welcome, harness.client_endpoint),
			103U,
			second));
	EXPECT_FALSE(second.valid());
	EXPECT_EQ(SessionModelResult::NoChange,
		harness.producer.try_account_preproof_send(endpoint(9U, 9U), std::numeric_limits<std::uint64_t>::max()));
}

TEST(TelemetryProtocolSession, SessionIdentityCapabilitiesAndSynchronizationGatesMatchOnBothEndpoints)
{
	SessionHarness harness;
	harness.negotiate();
	harness.prove();

	EXPECT_EQ(harness.welcome.header.session_id, harness.client.session_id());
	EXPECT_EQ(harness.welcome.header.session_id, harness.producer.session_id());
	EXPECT_EQ(harness.request.client_nonce, harness.client.client_nonce());
	EXPECT_EQ(harness.request.client_nonce, harness.producer.client_nonce());
	EXPECT_EQ(harness.welcome.payload.producer_id, harness.client.producer_id());
	EXPECT_EQ(harness.welcome.payload.producer_id, harness.producer.producer_id());
	EXPECT_EQ(VersionMajor, harness.client.selected_major());
	EXPECT_EQ(VersionMinor, harness.producer.selected_minor());
	EXPECT_EQ(VisibilityMode::Cockpit, harness.client.selected_visibility_mode());
	EXPECT_EQ(1000U, harness.client.heartbeat_interval_ms());
	EXPECT_EQ(3000U, harness.client.stale_timeout_ms());
	EXPECT_EQ(10000U, harness.client.disconnect_timeout_ms());
	EXPECT_TRUE(harness.client.clock_filter().valid());
	EXPECT_FALSE(harness.producer.clock_filter().valid());
	EXPECT_TRUE(harness.client.capabilities().initialized());
	EXPECT_TRUE(harness.producer.capabilities().initialized());
	EXPECT_EQ(AllActiveCapabilities, harness.client.capabilities().active_capabilities());
	EXPECT_EQ(AllActiveCapabilities, harness.producer.capabilities().active_capabilities());

	const auto client_context = harness.client.context();
	EXPECT_EQ(LocalEndpointRole::Client, client_context.local_role);
	EXPECT_EQ(harness.producer_endpoint, client_context.peer_endpoint);
	EXPECT_EQ(harness.welcome.header.session_id, client_context.active_session_id);
	const auto producer_context = harness.producer.context();
	EXPECT_EQ(LocalEndpointRole::Producer, producer_context.local_role);
	EXPECT_EQ(harness.client_endpoint, producer_context.peer_endpoint);
	EXPECT_EQ(harness.welcome.header.session_id, producer_context.active_session_id);

	WelcomeMessageIdentity identity;
	ASSERT_TRUE(harness.client.welcome_message_identity(identity));
	EXPECT_EQ(harness.welcome.header.session_id, identity.session_id);
	EXPECT_EQ(harness.producer_endpoint, identity.endpoint);
	EXPECT_EQ(harness.welcome.header.message_id, identity.message_id);
	EXPECT_EQ(1U, identity.fragment_count);
	EXPECT_EQ(harness.welcome.header.message_crc32, identity.message_crc32);

	const auto begin = session_begin();
	EXPECT_EQ(SessionModelResult::InvalidState,
		harness.client.on_full_snapshot_applied(begin.initial_snapshot_id, begin.required_manifest_id));
	EXPECT_EQ(SessionModelResult::InvalidState, harness.producer.on_manifest_applied(begin.required_manifest_id));
	ASSERT_EQ(SessionModelResult::Applied, harness.client.on_session_begin_applied(begin));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.on_session_begin_applied(begin));
	EXPECT_EQ(ClientSessionState::Synchronizing, harness.client.state());
	EXPECT_EQ(ProducerSessionState::Synchronizing, harness.producer.state());
	ASSERT_EQ(SessionModelResult::Applied, harness.client.on_manifest_applied(begin.required_manifest_id));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.on_manifest_applied(begin.required_manifest_id));
	EXPECT_EQ(ClientSessionState::Synchronizing, harness.client.state());
	EXPECT_EQ(ProducerSessionState::Synchronizing, harness.producer.state());
	ASSERT_EQ(SessionModelResult::Applied,
		harness.client.on_full_snapshot_applied(begin.initial_snapshot_id, begin.required_manifest_id));
	ASSERT_EQ(SessionModelResult::Applied,
		harness.producer.on_full_snapshot_applied(begin.initial_snapshot_id, begin.required_manifest_id));
	EXPECT_EQ(ClientSessionState::Live, harness.client.state());
	EXPECT_EQ(ProducerSessionState::Live, harness.producer.state());
	EXPECT_TRUE(harness.producer.can_send_heavy_data());
}

TEST(TelemetryProtocolSession, ClientRejectsEveryInvalidWelcomeWithoutMutatingNegotiationState)
{
	ResourcePurgerSpy purger;
	ClientSessionModel client(purger);
	const auto producer_endpoint = endpoint(1U, 7800U);
	const auto wrong_endpoint = endpoint(9U, 7800U);
	const auto request = hello();
	const auto encoded = welcome_for(request);
	EXPECT_EQ(ClientWelcomeResult::InvalidState,
		client.receive_welcome(encoded.header,
			producer_endpoint,
			encoded.payload,
			request.client_send_t0_us + 100U,
			101U));
	ASSERT_EQ(SessionModelResult::Applied, client.begin_negotiation(producer_endpoint, request, 100U));

	const auto expect_unchanged = [&]() {
		EXPECT_EQ(ClientSessionState::Negotiating, client.state());
		EXPECT_EQ(0U, client.session_id());
		EXPECT_EQ(producer_endpoint, client.producer_endpoint());
		EXPECT_EQ(0U, client.client_nonce());
		EXPECT_TRUE(purger.purged_session_ids.empty());
	};
	EXPECT_EQ(ClientWelcomeResult::EndpointMismatch,
		client
			.receive_welcome(encoded.header, wrong_endpoint, encoded.payload, request.client_send_t0_us + 100U, 101U));
	expect_unchanged();
	auto wrong_nonce = encoded.payload;
	wrong_nonce.client_nonce += 1U;
	EXPECT_EQ(ClientWelcomeResult::NonceMismatch,
		client.receive_welcome(encoded.header, producer_endpoint, wrong_nonce, request.client_send_t0_us + 100U, 101U));
	expect_unchanged();
	auto malformed_payload = encoded.payload;
	malformed_payload.heartbeat_interval_ms = 1U;
	EXPECT_EQ(ClientWelcomeResult::InvalidPayload,
		client.receive_welcome(encoded.header,
			producer_endpoint,
			malformed_payload,
			request.client_send_t0_us + 100U,
			101U));
	expect_unchanged();
	auto wrong_session = encoded.header;
	wrong_session.session_id = 0U;
	EXPECT_EQ(ClientWelcomeResult::SessionMismatch,
		client.receive_welcome(wrong_session,
			producer_endpoint,
			encoded.payload,
			request.client_send_t0_us + 100U,
			101U));
	expect_unchanged();
	auto wrong_flags = encoded.header;
	wrong_flags.flags = MessageFlagNone;
	EXPECT_EQ(ClientWelcomeResult::SessionMismatch,
		client
			.receive_welcome(wrong_flags, producer_endpoint, encoded.payload, request.client_send_t0_us + 100U, 101U));
	expect_unchanged();
	auto incompatible = encoded.payload;
	incompatible.producer_capabilities = CapabilityUpdate;
	EXPECT_EQ(ClientWelcomeResult::CapabilityMismatch,
		client
			.receive_welcome(encoded.header, producer_endpoint, incompatible, request.client_send_t0_us + 100U, 101U));
	expect_unchanged();
	EXPECT_EQ(ClientWelcomeResult::ClockSampleInvalid,
		client
			.receive_welcome(encoded.header, producer_endpoint, encoded.payload, request.client_send_t0_us + 5U, 101U));
	expect_unchanged();

	ASSERT_EQ(ClientWelcomeResult::Accepted,
		client.receive_welcome(encoded.header,
			producer_endpoint,
			encoded.payload,
			request.client_send_t0_us + 100U,
			101U));
	EXPECT_EQ(ClientSessionState::Synchronizing, client.state());
	EXPECT_EQ(ClientWelcomeResult::InvalidState,
		client.receive_welcome(encoded.header,
			producer_endpoint,
			encoded.payload,
			request.client_send_t0_us + 100U,
			102U));
}

TEST(TelemetryProtocolSession, InvalidNegotiationInputsNeverDestroyAnActiveClientSession)
{
	SessionHarness harness;
	harness.negotiate();
	const auto old_session = harness.client.session_id();
	auto invalid_hello = hello(0U);
	EXPECT_EQ(SessionModelResult::InvalidArgument,
		harness.client.begin_negotiation(harness.producer_endpoint, invalid_hello, 200U));
	EXPECT_EQ(SessionModelResult::InvalidArgument, harness.client.begin_negotiation(EndpointKey{}, hello(99U), 200U));
	EXPECT_EQ(old_session, harness.client.session_id());
	EXPECT_EQ(ClientSessionState::Synchronizing, harness.client.state());
	EXPECT_TRUE(harness.client.capabilities().initialized());
	EXPECT_TRUE(harness.client_purger.purged_session_ids.empty());
}

TEST(TelemetryProtocolSession, ProducerHandshakeValidationFailuresPreservePendingStateAndOutput)
{
	ResourcePurgerSpy purger;
	ProducerSessionModel producer(purger);
	const auto peer = endpoint(2U, 7801U);
	auto request = hello();
	auto encoded = welcome_for(request);
	CachedWelcomeResponseView output;
	output.session_id = 0xfeedU;
	EXPECT_EQ(ProducerHandshakeCompleteResult::InvalidState,
		producer.complete_handshake(encoded.header, byte_view(encoded.bytes), 100U, output));
	EXPECT_EQ(0xfeedU, output.session_id);
	EXPECT_EQ(ProducerHandshakeBeginResult::InvalidEndpoint,
		producer.begin_handshake(EndpointKey{}, request, 100U, 100U, output));
	request.client_nonce = 0U;
	EXPECT_EQ(ProducerHandshakeBeginResult::InvalidHello, producer.begin_handshake(peer, request, 100U, 100U, output));

	request = hello();
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready, producer.begin_handshake(peer, request, 100U, 100U, output));
	EXPECT_EQ(ProducerSessionState::Negotiating, producer.state());
	auto malformed_header = encoded.header;
	malformed_header.message_id = 0U;
	output.session_id = 0xfeedU;
	EXPECT_EQ(ProducerHandshakeCompleteResult::InvalidWelcome,
		producer.complete_handshake(malformed_header, byte_view(encoded.bytes), 101U, output));
	EXPECT_EQ(0xfeedU, output.session_id);
	EXPECT_EQ(ProducerSessionState::Negotiating, producer.state());
	auto wrong_t0 = encoded.payload;
	wrong_t0.client_send_t0_us += 1U;
	std::vector<std::uint8_t> wrong_bytes(WelcomePayloadPrefixSize);
	std::size_t written = 0U;
	ASSERT_EQ(ValidationError::None, encode_welcome_payload(wrong_t0, mutable_byte_view(wrong_bytes), written));
	auto wrong_t0_header = encoded.header;
	wrong_t0_header.message_crc32 = crc32_iso_hdlc(byte_view(wrong_bytes));
	EXPECT_EQ(ProducerHandshakeCompleteResult::InvalidWelcome,
		producer.complete_handshake(wrong_t0_header, byte_view(wrong_bytes), 101U, output));
	EXPECT_EQ(ProducerSessionState::Negotiating, producer.state());
	ASSERT_EQ(ProducerHandshakeCompleteResult::AwaitingProof,
		producer.complete_handshake(encoded.header, byte_view(encoded.bytes), 101U, output));
	EXPECT_EQ(ProducerSessionState::AwaitingWelcomeProof, producer.state());
	EXPECT_EQ(ProducerHandshakeCompleteResult::InvalidState,
		producer.complete_handshake(encoded.header, byte_view(encoded.bytes), 102U, output));
}

TEST(TelemetryProtocolSession, LiveSessionsResynchronizeWithoutBypassingManifestAndSnapshotCommitGates)
{
	SessionHarness harness;
	harness.negotiate();
	harness.prove();
	harness.make_live();
	ASSERT_EQ(ClientSessionState::Live, harness.client.state());
	ASSERT_EQ(ProducerSessionState::Live, harness.producer.state());

	EXPECT_EQ(SessionModelResult::Applied, harness.client.mark_stale());
	EXPECT_EQ(SessionModelResult::NoChange, harness.client.mark_stale());
	EXPECT_FALSE(harness.client.clock_filter().valid());
	ASSERT_EQ(SessionModelResult::Applied, harness.client.begin_resynchronization(42U, 52U, false));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.begin_resynchronization(42U, 52U, false));
	EXPECT_EQ(ClientSessionState::Synchronizing, harness.client.state());
	EXPECT_EQ(ProducerSessionState::Synchronizing, harness.producer.state());
	EXPECT_EQ(SessionModelResult::InvalidState, harness.client.on_full_snapshot_applied(52U, 42U));
	EXPECT_EQ(SessionModelResult::InvalidState, harness.producer.on_full_snapshot_applied(52U, 42U));
	EXPECT_EQ(SessionModelResult::InvalidArgument, harness.client.on_manifest_applied(41U));
	EXPECT_EQ(SessionModelResult::InvalidArgument, harness.producer.on_manifest_applied(41U));
	ASSERT_EQ(SessionModelResult::Applied, harness.client.on_manifest_applied(42U));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.on_manifest_applied(42U));
	ASSERT_EQ(SessionModelResult::Applied, harness.client.on_full_snapshot_applied(52U, 42U));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.on_full_snapshot_applied(52U, 42U));
	EXPECT_EQ(ClientSessionState::Live, harness.client.state());
	EXPECT_EQ(ProducerSessionState::Live, harness.producer.state());

	ASSERT_EQ(SessionModelResult::Applied, harness.client.begin_resynchronization(0U, 53U, true));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.begin_resynchronization(0U, 53U, true));
	ASSERT_EQ(SessionModelResult::Applied, harness.client.on_full_snapshot_applied(53U, 0U));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.on_full_snapshot_applied(53U, 0U));
	EXPECT_EQ(ClientSessionState::Live, harness.client.state());
	EXPECT_EQ(ProducerSessionState::Live, harness.producer.state());
}

TEST(TelemetryProtocolSession, CapabilityWithdrawalDoesNotDemoteOrCloseALiveSession)
{
	SessionHarness harness;
	harness.negotiate();
	harness.prove();
	harness.make_live();
	CapabilityUpdatePayload update;
	update.capability_generation = 1U;
	update.advertised_capabilities = AllClientCapabilities & ~CapabilityTargetVideoH264;
	update.active_capabilities = AllActiveCapabilities & ~TargetVideoCapabilityPair;
	update.effective_time_us = 500U;
	update.reason = CapabilityUpdateReason::RuntimeAvailability;
	const auto client_outcome =
		apply_capability_update(harness.client.capabilities(), CapabilityPeerRole::Client, update);
	const auto producer_outcome =
		apply_capability_update(harness.producer.capabilities(), CapabilityPeerRole::Client, update);
	ASSERT_EQ(CapabilityUpdateDisposition::Applied, client_outcome.disposition);
	ASSERT_EQ(CapabilityUpdateDisposition::Applied, producer_outcome.disposition);
	EXPECT_EQ(TargetVideoCapabilityPair, client_outcome.removed_visual_capabilities());
	EXPECT_EQ(AllActiveCapabilities & ~TargetVideoCapabilityPair, harness.client.capabilities().active_capabilities());
	EXPECT_EQ(AllActiveCapabilities & ~TargetVideoCapabilityPair,
		harness.producer.capabilities().active_capabilities());
	EXPECT_EQ(ClientSessionState::Live, harness.client.state());
	EXPECT_EQ(ProducerSessionState::Live, harness.producer.state());
	EXPECT_TRUE(harness.producer.can_send_heavy_data());
}

TEST(TelemetryProtocolSession, ClientSessionReplacementPurgesRealResourcesAndAllEpochLocalState)
{
	SessionHarness harness;
	harness.negotiate();
	const auto old_session = harness.client.session_id();
	harness.client_purger.seed(old_session);
	ProbeToken probe;
	ASSERT_EQ(ProbeStartResult::Started, harness.client.probes().begin_probe(1234U, probe));
	ASSERT_EQ(1U, harness.client.probes().in_flight_count());
	ASSERT_TRUE(harness.client.clock_filter().valid());
	ASSERT_TRUE(harness.client.capabilities().initialized());

	const auto new_endpoint = endpoint(4U, 7900U);
	const auto new_hello = hello(0x9988776655443322ULL, 2'000'000U);
	ASSERT_EQ(SessionModelResult::Applied, harness.client.begin_negotiation(new_endpoint, new_hello, 200U));
	ASSERT_EQ(1U, harness.client_purger.purged_session_ids.size());
	EXPECT_EQ(old_session, harness.client_purger.purged_session_ids[0]);
	EXPECT_EQ(0U, harness.client_purger.reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, harness.client_purger.reassembler.reserved_bytes(MessageSizeClass::State));
	EXPECT_EQ(0U, harness.client_purger.transactions.active_candidates());
	EXPECT_EQ(0U, harness.client_purger.transactions.reserved_bytes());
	EXPECT_EQ(ClientSessionState::Negotiating, harness.client.state());
	EXPECT_EQ(0U, harness.client.session_id());
	EXPECT_EQ(new_endpoint, harness.client.producer_endpoint());
	EXPECT_EQ(0U, harness.client.probes().session_id());
	EXPECT_EQ(0U, harness.client.probes().in_flight_count());
	EXPECT_FALSE(harness.client.clock_filter().valid());
	EXPECT_EQ(0U, harness.client.clock_filter().sample_count());
	EXPECT_FALSE(harness.client.capabilities().initialized());
	WelcomeMessageIdentity identity;
	EXPECT_FALSE(harness.client.welcome_message_identity(identity));

	const auto replacement = welcome_for(new_hello, WelcomeStatus::Accepted, old_session + 1U, 72U);
	ASSERT_EQ(ClientWelcomeResult::Accepted,
		harness.client.receive_welcome(replacement.header,
			new_endpoint,
			replacement.payload,
			new_hello.client_send_t0_us + 100U,
			201U));
	EXPECT_EQ(old_session + 1U, harness.client.session_id());
	EXPECT_EQ(old_session + 1U, harness.client.probes().session_id());
	EXPECT_EQ(0U, harness.client.probes().in_flight_count());
	EXPECT_TRUE(harness.client.clock_filter().valid());
	EXPECT_TRUE(harness.client.capabilities().initialized());
}

TEST(TelemetryProtocolSession, ProducerSessionReplacementPurgesResourcesButRetainsOldHandshakeReplay)
{
	SessionHarness harness;
	harness.negotiate();
	harness.prove();
	const auto old_session = harness.producer.session_id();
	harness.producer_purger.seed(old_session);
	ProbeToken probe;
	ASSERT_EQ(ProbeStartResult::Started, harness.producer.probes().begin_probe(1234U, probe));
	const OrientedClockSample clock_sample{25, 40U};
	ASSERT_TRUE(harness.producer.note_valid_heartbeat_response(clock_sample, 103U));
	ASSERT_TRUE(harness.producer.clock_filter().valid());
	ASSERT_TRUE(harness.producer.capabilities().initialized());

	const auto new_endpoint = endpoint(4U, 7901U);
	const auto new_hello = hello(0x9988776655443322ULL, 2'000'000U);
	CachedWelcomeResponseView output;
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready,
		harness.producer.begin_handshake(new_endpoint, new_hello, 140U, 200U, output));
	ASSERT_EQ(1U, harness.producer_purger.purged_session_ids.size());
	EXPECT_EQ(old_session, harness.producer_purger.purged_session_ids[0]);
	EXPECT_EQ(0U, harness.producer_purger.reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, harness.producer_purger.transactions.active_candidates());
	EXPECT_EQ(ProducerSessionState::Negotiating, harness.producer.state());
	EXPECT_EQ(0U, harness.producer.session_id());
	EXPECT_EQ(new_endpoint, harness.producer.context().peer_endpoint);
	EXPECT_EQ(0U, harness.producer.probes().session_id());
	EXPECT_EQ(0U, harness.producer.probes().in_flight_count());
	EXPECT_FALSE(harness.producer.clock_filter().valid());
	EXPECT_FALSE(harness.producer.capabilities().initialized());

	CachedWelcomeResponseView old_replay;
	ASSERT_EQ(HandshakeCacheLookupResult::Found,
		harness.producer.handshake_cache().lookup(harness.client_endpoint,
			harness.request.client_nonce,
			101U + HandshakeCacheLifetimeMs - 1U,
			old_replay));
	EXPECT_EQ(old_session, old_replay.session_id);
	EXPECT_EQ(ProducerSessionState::Negotiating, harness.producer.state());
	EXPECT_EQ(new_endpoint, harness.producer.context().peer_endpoint);
	EXPECT_EQ(HandshakeCacheLookupResult::Miss,
		harness.producer.handshake_cache().lookup(harness.client_endpoint,
			harness.request.client_nonce,
			101U + HandshakeCacheLifetimeMs,
			old_replay));

	const auto replacement = welcome_for(new_hello, WelcomeStatus::Accepted, old_session + 1U, 72U);
	ASSERT_EQ(ProducerHandshakeCompleteResult::AwaitingProof,
		harness.producer.complete_handshake(replacement.header, byte_view(replacement.bytes), 201U, output));
	EXPECT_EQ(old_session + 1U, harness.producer.session_id());
	EXPECT_EQ(old_session + 1U, harness.producer.probes().session_id());
	EXPECT_EQ(0U, harness.producer.probes().in_flight_count());
	EXPECT_FALSE(harness.producer.clock_filter().valid());
	EXPECT_TRUE(harness.producer.capabilities().initialized());
}

TEST(TelemetryProtocolSession, RejectedProducerCanNegotiateANewPeerWithoutLeakingOldContext)
{
	ResourcePurgerSpy purger;
	ProducerSessionModel producer(purger);
	const auto old_endpoint = endpoint(2U, 7801U);
	const auto old_hello = hello(100U, 1'000U);
	const auto rejection = welcome_for(old_hello, WelcomeStatus::Busy, 0U, 10U);
	CachedWelcomeResponseView output;
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready,
		producer.begin_handshake(old_endpoint, old_hello, 100U, 100U, output));
	ASSERT_EQ(ProducerHandshakeCompleteResult::Rejected,
		producer.complete_handshake(rejection.header, byte_view(rejection.bytes), 101U, output));
	ASSERT_EQ(ProducerSessionState::Rejected, producer.state());
	ASSERT_EQ(old_endpoint, producer.context().peer_endpoint);

	const auto new_endpoint = endpoint(3U, 7802U);
	const auto new_hello = hello(200U, 2'000U);
	output.endpoint = old_endpoint;
	ASSERT_EQ(ProducerHandshakeBeginResult::Ready,
		producer.begin_handshake(new_endpoint, new_hello, 100U, 102U, output));
	EXPECT_EQ(ProducerSessionState::Negotiating, producer.state());
	EXPECT_EQ(new_endpoint, producer.context().peer_endpoint);
	EXPECT_EQ(0U, producer.context().active_session_id);
	EXPECT_EQ(old_endpoint, output.endpoint);
	EXPECT_EQ(HandshakeCacheLookupResult::Found,
		producer.handshake_cache().lookup(old_endpoint, old_hello.client_nonce, 103U, output));
	EXPECT_EQ(WelcomeStatus::Busy, output.status);
	EXPECT_EQ(ProducerSessionState::Negotiating, producer.state());
	EXPECT_EQ(new_endpoint, producer.context().peer_endpoint);
}

TEST(TelemetryProtocolSession, ClosingPurgesEpochStateButKeepsExactWelcomeReplayUntilExpiry)
{
	SessionHarness harness;
	harness.negotiate();
	harness.prove();
	const auto old_session = harness.producer.session_id();
	harness.producer_purger.seed(old_session);
	ProbeToken probe;
	ASSERT_EQ(ProbeStartResult::Started, harness.producer.probes().begin_probe(1234U, probe));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.begin_closing());
	EXPECT_EQ(ProducerSessionState::Closing, harness.producer.state());
	CachedWelcomeResponseView output;
	EXPECT_EQ(ProducerHandshakeBeginResult::InvalidState,
		harness.producer.begin_handshake(harness.client_endpoint, harness.request, 1U, 103U, output));
	ASSERT_EQ(SessionModelResult::Applied, harness.producer.finish_closing());
	EXPECT_EQ(ProducerSessionState::Listening, harness.producer.state());
	EXPECT_EQ(0U, harness.producer.session_id());
	EXPECT_FALSE(harness.producer.can_send_heavy_data());
	EXPECT_EQ(0U, harness.producer_purger.reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, harness.producer_purger.transactions.active_candidates());
	EXPECT_EQ(0U, harness.producer.probes().session_id());
	EXPECT_FALSE(harness.producer.capabilities().initialized());
	ASSERT_EQ(1U, harness.producer_purger.purged_session_ids.size());
	EXPECT_EQ(old_session, harness.producer_purger.purged_session_ids[0]);

	ASSERT_EQ(HandshakeCacheLookupResult::Found,
		harness.producer.handshake_cache().lookup(harness.client_endpoint,
			harness.request.client_nonce,
			101U + HandshakeCacheLifetimeMs - 1U,
			output));
	EXPECT_EQ(old_session, output.session_id);
	ASSERT_EQ(ProducerHandshakeBeginResult::Cached,
		harness.producer.begin_handshake(harness.client_endpoint, harness.request, 1U, 102U, output));
	EXPECT_EQ(ProducerSessionState::Listening, harness.producer.state());
	EXPECT_EQ(0U, harness.producer.session_id());
	EXPECT_FALSE(harness.producer.can_send_heavy_data());

	ASSERT_EQ(ProducerHandshakeBeginResult::Ready,
		harness.producer
			.begin_handshake(harness.client_endpoint, harness.request, 1U, 101U + HandshakeCacheLifetimeMs, output));
	EXPECT_EQ(ProducerSessionState::Negotiating, harness.producer.state());
	EXPECT_EQ(0U, harness.producer.handshake_cache().size());
	EXPECT_EQ(SessionModelResult::InvalidState, harness.producer.finish_closing());
}

TEST(TelemetryProtocolSession, ClientClockStalenessAndNetworkDisconnectUseIndependentDeadlines)
{
	SessionHarness harness;
	harness.negotiate();
	ASSERT_TRUE(harness.client.clock_filter().valid());
	ASSERT_TRUE(harness.client.note_valid_session_datagram(3'101U));
	EXPECT_EQ(SessionTimeoutAction::BecameStale, harness.client.poll_timeouts(3'101U));
	EXPECT_EQ(ClientSessionState::Stale, harness.client.state());
	EXPECT_FALSE(harness.client.clock_filter().valid());
	EXPECT_EQ(SessionTimeoutAction::None, harness.client.poll_timeouts(3'101U));

	const OrientedClockSample sample{12, 30U};
	ASSERT_TRUE(harness.client.note_valid_heartbeat_response(sample, 3'102U));
	EXPECT_TRUE(harness.client.clock_filter().valid());
	EXPECT_EQ(ClientSessionState::Stale, harness.client.state());
	EXPECT_EQ(SessionTimeoutAction::None, harness.client.poll_timeouts(13'101U));
	harness.client_purger.seed(harness.client.session_id());
	EXPECT_EQ(SessionTimeoutAction::Disconnected, harness.client.poll_timeouts(13'102U));
	EXPECT_EQ(ClientSessionState::Disconnected, harness.client.state());
	EXPECT_EQ(0U, harness.client.session_id());
	EXPECT_EQ(0U, harness.client_purger.reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, harness.client_purger.transactions.active_candidates());
	EXPECT_EQ(SessionTimeoutAction::None, harness.client.poll_timeouts(13'103U));
}

TEST(TelemetryProtocolSession, ProducerClockInvalidationDoesNotCloseARecentlyActiveSession)
{
	SessionHarness harness;
	harness.negotiate();
	harness.prove();
	const OrientedClockSample sample{12, 30U};
	ASSERT_TRUE(harness.producer.note_valid_heartbeat_response(sample, 103U));
	ASSERT_TRUE(harness.producer.clock_filter().valid());
	ASSERT_TRUE(harness.producer.note_valid_session_datagram(3'103U));
	EXPECT_EQ(SessionTimeoutAction::ClockInvalidated, harness.producer.poll_timeouts(3'103U));
	EXPECT_FALSE(harness.producer.clock_filter().valid());
	EXPECT_EQ(ProducerSessionState::Synchronizing, harness.producer.state());
	EXPECT_TRUE(harness.producer.can_send_heavy_data());
	EXPECT_EQ(SessionTimeoutAction::None, harness.producer.poll_timeouts(3'104U));

	ASSERT_TRUE(harness.producer.note_valid_heartbeat_response(sample, 3'104U));
	EXPECT_TRUE(harness.producer.clock_filter().valid());
	ASSERT_TRUE(harness.producer.note_valid_session_datagram(6'104U));
	EXPECT_EQ(SessionTimeoutAction::ClockInvalidated, harness.producer.poll_timeouts(6'104U));
	EXPECT_EQ(SessionTimeoutAction::None, harness.producer.poll_timeouts(16'103U));
	harness.producer_purger.seed(harness.producer.session_id());
	EXPECT_EQ(SessionTimeoutAction::Closing, harness.producer.poll_timeouts(16'104U));
	EXPECT_EQ(ProducerSessionState::Closing, harness.producer.state());
	EXPECT_EQ(0U, harness.producer.session_id());
	EXPECT_FALSE(harness.producer.can_send_heavy_data());
	EXPECT_EQ(0U, harness.producer_purger.reassembler.active_reassemblies(MessageSizeClass::State));
	EXPECT_EQ(0U, harness.producer_purger.transactions.active_candidates());
	EXPECT_EQ(SessionModelResult::Applied, harness.producer.finish_closing());
	EXPECT_EQ(ProducerSessionState::Listening, harness.producer.state());
}

TEST(TelemetryProtocolSession, MonotonicDiscontinuityReanchorsClientAndProducerToTheNewClockDomain)
{
	SessionHarness harness;
	harness.negotiate();
	harness.prove();
	const OrientedClockSample sample{12, 30U};
	ASSERT_TRUE(harness.producer.note_valid_heartbeat_response(sample, 103U));

	EXPECT_FALSE(harness.client.note_valid_session_datagram(90U));
	EXPECT_EQ(ClientSessionState::Stale, harness.client.state());
	EXPECT_FALSE(harness.client.clock_filter().valid());
	EXPECT_TRUE(harness.client.note_valid_session_datagram(91U));
	EXPECT_TRUE(harness.client.note_valid_heartbeat_response(sample, 92U));
	EXPECT_TRUE(harness.client.clock_filter().valid());
	EXPECT_EQ(SessionTimeoutAction::None, harness.client.poll_timeouts(92U));
	EXPECT_EQ(SessionTimeoutAction::MonotonicDiscontinuity, harness.client.poll_timeouts(80U));
	EXPECT_FALSE(harness.client.clock_filter().valid());
	EXPECT_TRUE(harness.client.note_valid_session_datagram(81U));
	EXPECT_TRUE(harness.client.note_valid_heartbeat_response(sample, 82U));
	EXPECT_TRUE(harness.client.clock_filter().valid());

	EXPECT_FALSE(harness.producer.note_valid_session_datagram(90U));
	EXPECT_FALSE(harness.producer.clock_filter().valid());
	EXPECT_TRUE(harness.producer.note_valid_session_datagram(91U));
	EXPECT_TRUE(harness.producer.note_valid_heartbeat_response(sample, 92U));
	EXPECT_TRUE(harness.producer.clock_filter().valid());
	EXPECT_EQ(SessionTimeoutAction::None, harness.producer.poll_timeouts(92U));
	EXPECT_EQ(SessionTimeoutAction::MonotonicDiscontinuity, harness.producer.poll_timeouts(80U));
	EXPECT_FALSE(harness.producer.clock_filter().valid());
	EXPECT_TRUE(harness.producer.note_valid_session_datagram(81U));
	EXPECT_TRUE(harness.producer.note_valid_heartbeat_response(sample, 82U));
	EXPECT_TRUE(harness.producer.clock_filter().valid());
}

} // namespace
