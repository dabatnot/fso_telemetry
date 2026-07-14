#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_event_messages.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_reliable_window.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_session_context.h"
#include "telemetry/protocol/telemetry_specialized_lifecycle.h"
#include "telemetry/protocol/telemetry_specialized_views.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef FSTL_PROTOCOL_TEST_ASSET_PATH
#error "FSTL_PROTOCOL_TEST_ASSET_PATH must point at the external telemetry protocol test assets"
#endif

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes) {
	return ByteView{bytes.empty() ? nullptr
	                              : static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())),
	                bytes.size()};
}

std::filesystem::path asset_root() {
	return std::filesystem::path{FSTL_PROTOCOL_TEST_ASSET_PATH};
}

std::vector<std::uint8_t> read_binary(const std::filesystem::path& path) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		ADD_FAILURE() << "cannot open external telemetry vector " << path.string();
		return {};
	}
	return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::vector<std::uint8_t> expected_payload(std::size_t size) {
	std::vector<std::uint8_t> result(size);
	for (std::size_t index = 0; index < size; ++index) {
		result[index] = static_cast<std::uint8_t>(0x31U + index * 37U);
	}
	return result;
}

template <typename Payload, typename Decoder, typename Encoder>
ValidationError canonical_payload_roundtrip(const std::vector<std::uint8_t>& input,
	std::vector<std::uint8_t>& encoded,
	Decoder decoder,
	Encoder encoder) {
	Payload payload;
	if (const auto error = decoder(byte_view(input), payload); error != ValidationError::None) {
		return error;
	}
	encoded.assign(input.size(), 0xa5U);
	std::size_t written = 0;
	if (const auto error = encoder(payload, MutableByteView{encoded.data(), encoded.size()}, written);
		error != ValidationError::None) {
		return error;
	}
	return written == input.size() && encoded == input ? ValidationError::None
													 : ValidationError::InternalSerializationError;
}

ValidationError roundtrip_message_fixture(const std::string& name,
	const std::vector<std::uint8_t>& input,
	std::vector<std::uint8_t>& encoded) {
	if (name == "discovery") {
		return canonical_payload_roundtrip<DiscoveryPayload>(
			input, encoded, decode_discovery_payload, encode_discovery_payload);
	}
	if (name == "hello") {
		return canonical_payload_roundtrip<HelloPayload>(input, encoded, decode_hello_payload, encode_hello_payload);
	}
	if (name == "welcome") {
		return canonical_payload_roundtrip<WelcomePayload>(input, encoded, decode_welcome_payload, encode_welcome_payload);
	}
	if (name == "session_begin") {
		return canonical_payload_roundtrip<SessionBeginPayload>(
			input, encoded, decode_session_begin_payload, encode_session_begin_payload);
	}
	if (name == "manifest") {
		return canonical_payload_roundtrip<ManifestPartPayload>(
			input, encoded, decode_manifest_part_payload, encode_manifest_part_payload);
	}
	if (name == "full_snapshot") {
		return canonical_payload_roundtrip<FullSnapshotPartPayload>(
			input, encoded, decode_full_snapshot_part_payload, encode_full_snapshot_part_payload);
	}
	if (name == "delta") {
		return canonical_payload_roundtrip<DeltaPayload>(input, encoded, decode_delta_payload, encode_delta_payload);
	}
	if (name == "event_batch") {
		EventBatchPayload payload;
		if (const auto error = decode_event_batch_payload(byte_view(input), true, payload);
			error != ValidationError::None) {
			return error;
		}
		encoded.assign(input.size(), 0xa5U);
		std::size_t written = 0;
		if (const auto error = encode_event_batch_payload(
				payload, true, MutableByteView{encoded.data(), encoded.size()}, written);
			error != ValidationError::None) {
			return error;
		}
		return written == input.size() && encoded == input ? ValidationError::None
													 : ValidationError::InternalSerializationError;
	}
	if (name == "heartbeat") {
		return canonical_payload_roundtrip<HeartbeatPayload>(
			input, encoded, decode_heartbeat_payload, encode_heartbeat_payload);
	}
	if (name == "ack") {
		return canonical_payload_roundtrip<AckPayload>(input, encoded, decode_ack_payload, encode_ack_payload);
	}
	if (name == "nack") {
		return canonical_payload_roundtrip<NackPayload>(input, encoded, decode_nack_payload, encode_nack_payload);
	}
	if (name == "resync_request") {
		return canonical_payload_roundtrip<ResyncRequestPayload>(
			input, encoded, decode_resync_request_payload, encode_resync_request_payload);
	}
	if (name == "session_end") {
		return canonical_payload_roundtrip<SessionEndPayload>(
			input, encoded, decode_session_end_payload, encode_session_end_payload);
	}
	if (name == "target_video_subscribe") {
		return canonical_payload_roundtrip<TargetVideoSubscribePayload>(
			input, encoded, decode_target_video_subscribe_payload, encode_target_video_subscribe_payload);
	}
	if (name == "target_video_config") {
		return canonical_payload_roundtrip<TargetVideoConfigPayload>(
			input, encoded, decode_target_video_config_payload, encode_target_video_config_payload);
	}
	if (name == "target_video_frame") {
		return canonical_payload_roundtrip<TargetVideoFramePayload>(
			input, encoded, decode_target_video_frame_payload, encode_target_video_frame_payload);
	}
	if (name == "target_video_keyframe_request") {
		return canonical_payload_roundtrip<TargetVideoKeyframeRequestPayload>(input,
			encoded,
			decode_target_video_keyframe_request_payload,
			encode_target_video_keyframe_request_payload);
	}
	if (name == "target_video_stop") {
		return canonical_payload_roundtrip<TargetVideoStopPayload>(
			input, encoded, decode_target_video_stop_payload, encode_target_video_stop_payload);
	}
	if (name == "target_video_stats") {
		return canonical_payload_roundtrip<TargetVideoStatsPayload>(
			input, encoded, decode_target_video_stats_payload, encode_target_video_stats_payload);
	}
	if (name == "capability_update") {
		return canonical_payload_roundtrip<CapabilityUpdatePayload>(
			input, encoded, decode_capability_update_payload, encode_capability_update_payload);
	}
	return ValidationError::UnknownMessageType;
}

BusinessRecordContainer fixture_container(RecordType type) {
	if (type == RecordType::ClassManifest || type == RecordType::WeaponManifest ||
		type == RecordType::CommAssetManifest) {
		return BusinessRecordContainer::Manifest;
	}
	if (type == RecordType::CommViewEvent || type == RecordType::Events) {
		return BusinessRecordContainer::EventBatchReliable;
	}
	return BusinessRecordContainer::FullSnapshot;
}

ValidationError validate_invalid_record_fixture(const std::vector<std::uint8_t>& input) {
	RecordEnvelopeIterator iterator(byte_view(input), 1U, RecordFlagPolicy::AllowV1Mutations);
	RecordEnvelopeView record;
	bool has_value = false;
	if (const auto error = iterator.next(record, has_value); error != ValidationError::None) {
		return error;
	}
	if (!has_value) {
		return ValidationError::UnknownRequiredRecord;
	}
	BusinessRecordMetadata metadata;
	return validate_business_record(
		record, fixture_container(static_cast<RecordType>(record.raw_record_type)), metadata);
}

ValidationError validate_invalid_message_payload_fixture(const std::string& name,
	const std::vector<std::uint8_t>& input) {
	if (name == "fixed_payload_truncated" || name == "fixed_payload_trailing_byte") {
		HeartbeatPayload payload;
		return decode_heartbeat_payload(byte_view(input), payload);
	}
	if (name == "video_encoded_frame_size_mismatch") {
		TargetVideoFramePayload payload;
		return decode_target_video_frame_payload(byte_view(input), payload);
	}
	if (name == "nack_bitmap_tail_bits_set") {
		NackPayload payload;
		return decode_nack_payload(byte_view(input), payload);
	}
	if (name == "duplicate_singleton_record") {
		FullSnapshotPartPayload payload;
		if (const auto error = decode_full_snapshot_part_payload(byte_view(input), payload);
			error != ValidationError::None) {
			return error;
		}
		StateImage snapshot;
		return decode_business_snapshot_region(payload.records, payload.record_count, snapshot);
	}
	if (name == "event_batch_record_count_exceeds_region") {
		EventBatchPayload payload;
		return decode_event_batch_payload(byte_view(input), true, payload);
	}
	return ValidationError::UnknownMessageType;
}

EndpointKey client_endpoint(std::uint8_t last_octet, std::uint16_t port = 42042U) {
	return EndpointKey::from_ipv4({127U, 0U, 0U, last_octet}, port);
}

ValidationError configure_default_limiter(ProtocolRateLimiter& limiter) {
	return ProtocolRateLimiter::configure({}, 0U, limiter);
}

ValidationError configure_video_client(TargetVideoClientLifecycle& lifecycle,
	std::uint64_t target_entity_id,
	std::uint32_t config_generation = 1U) {
	const auto config_bytes = read_binary(
		asset_root() / "vectors" / "valid" / "messages" / "target_video_config" / "target_video_config.bin");
	if (config_bytes.empty()) {
		return ValidationError::InternalSerializationError;
	}
	TargetVideoConfigPayload config;
	if (const auto error = decode_target_video_config_payload(byte_view(config_bytes), config);
		error != ValidationError::None) {
		return error;
	}
	config.config_generation = config_generation;
	lifecycle.reset(true);
	if (lifecycle.begin_subscribe(config.request_id) != VideoClientLifecycleResult::Applied ||
		lifecycle.apply_config(config, target_entity_id) != VideoClientLifecycleResult::Applied ||
		lifecycle.acknowledge_config_applied(config.stream_id, config.config_generation) !=
			VideoClientLifecycleResult::Applied) {
		return ValidationError::InternalSerializationError;
	}
	return ValidationError::None;
}

ValidationError validate_invalid_contextual_message_fixture(const std::string& name,
	const std::vector<std::uint8_t>& input) {
	if (name == "ack_forged_target" || name == "ack_wrong_target_crc") {
		AckPayload ack;
		if (const auto error = decode_ack_payload(byte_view(input), ack); error != ValidationError::None) {
			return error;
		}
		ReliabilityTargetTuple target;
		target.message_id = 100U;
		target.message_type = MessageType::FullSnapshot;
		target.fragment_count = 1U;
		target.message_crc32 = 0x12345678U;
		return validate_ack_target(ack, target);
	}
	if (name == "ack_late_after_retention") {
		AckPayload ack;
		if (const auto error = decode_ack_payload(byte_view(input), ack); error != ValidationError::None) {
			return error;
		}
		ReliableSendWindow window;
		if (const auto error = ReliableSendWindow::configure({}, window); error != ValidationError::None) {
			return error;
		}
		return reliable_response_validation_error(window.acknowledge(42U, client_endpoint(1U), ack, 0U));
	}
	if (name == "ack_wrong_endpoint") {
		AckPayload ack;
		if (const auto error = decode_ack_payload(byte_view(input), ack); error != ValidationError::None) {
			return error;
		}
		TelemetryDatagramHeader header;
		header.message_type = MessageType::Ack;
		header.session_id = 42U;
		TelemetrySessionContext context{LocalEndpointRole::Producer, client_endpoint(1U), 42U};
		return validate_received_datagram_context(header, client_endpoint(2U), context);
	}
	if (name == "client_simulated_producer_command") {
		SessionBeginPayload payload;
		if (const auto error = decode_session_begin_payload(byte_view(input), payload);
			error != ValidationError::None) {
			return error;
		}
		TelemetryDatagramHeader header;
		header.message_type = MessageType::SessionBegin;
		header.session_id = 42U;
		TelemetrySessionContext context{LocalEndpointRole::Producer, client_endpoint(1U), 42U};
		return validate_received_datagram_context(header, client_endpoint(1U), context);
	}
	if (name == "unknown_message_type") {
		if (!input.empty()) {
			return ValidationError::TrailingBytes;
		}
		TelemetryDatagramHeader header;
		header.message_type = static_cast<MessageType>(255U);
		header.session_id = 42U;
		TelemetrySessionContext context{LocalEndpointRole::Producer, client_endpoint(1U), 42U};
		return validate_received_datagram_context(header, client_endpoint(1U), context);
	}
	if (name == "delta_unknown_baseline") {
		DeltaPayload payload;
		if (const auto error = decode_delta_payload(byte_view(input), payload); error != ValidationError::None) {
			return error;
		}
		CumulativeStateDelta delta;
		if (const auto error = decode_business_delta(payload, delta); error != ValidationError::None) {
			return error;
		}
		StateImage baseline;
		if (StateImage::create({}, baseline) != StateImageResult::Created) {
			return ValidationError::InternalSerializationError;
		}
		ProtocolRateLimiter limiter;
		if (const auto error = configure_default_limiter(limiter); error != ValidationError::None) {
			return error;
		}
		ClientReplicationModel client;
		if (client.note_snapshot_candidate(1U, 0U, 0U, 10'000'000U) != SnapshotCandidateResult::Known) {
			return ValidationError::InternalSerializationError;
		}
		ResyncRequestPayload request;
		ClientResyncChannel channel{42U, client_endpoint(1U), limiter, request};
		if (client.commit_snapshot(1U, 0U, baseline, 1U, channel) != SnapshotCommitResult::Committed ||
			client.note_snapshot_candidate(2U, 0U, 2U, 10'000'002U) != SnapshotCandidateResult::Known) {
			return ValidationError::InternalSerializationError;
		}
		return client_delta_validation_error(
			client.receive_delta(delta, 3U, 42U, client_endpoint(1U), limiter, request));
	}
	if (name == "snapshot_missing_manifest") {
		FullSnapshotPartPayload payload;
		if (const auto error = decode_full_snapshot_part_payload(byte_view(input), payload);
			error != ValidationError::None) {
			return error;
		}
		StateImage snapshot;
		if (const auto error = decode_business_snapshot_region(payload.records, payload.record_count, snapshot);
			error != ValidationError::None) {
			return error;
		}
		ProtocolRateLimiter limiter;
		if (const auto error = configure_default_limiter(limiter); error != ValidationError::None) {
			return error;
		}
		ClientReplicationModel client;
		if (client.install_manifest(1U) != ManifestInstallResult::Installed ||
			client.note_snapshot_candidate(payload.snapshot_id,
				payload.required_manifest_id,
				0U,
				10'000'000U) != SnapshotCandidateResult::Known) {
			return ValidationError::InternalSerializationError;
		}
		ResyncRequestPayload request;
		ClientResyncChannel channel{42U, client_endpoint(1U), limiter, request};
		return snapshot_commit_validation_error(client.commit_snapshot(payload.snapshot_id,
			payload.required_manifest_id,
			snapshot,
			1U,
			channel));
	}
	if (name == "video_stale_generation" || name == "video_unknown_stream" ||
		name == "video_unknown_target" || name == "video_old_target_after_change") {
		TargetVideoFramePayload frame;
		if (const auto error = decode_target_video_frame_payload(byte_view(input), frame);
			error != ValidationError::None) {
			return error;
		}
		if (name == "video_unknown_target") {
			return validate_target_video_frame_entity_context(frame, false);
		}
		TargetVideoClientLifecycle lifecycle;
		const auto old_target = name == "video_old_target_after_change";
		const auto target = old_target ? 3U : 2U;
		if (const auto error = configure_video_client(lifecycle, target, old_target ? 2U : 1U);
			error != ValidationError::None) {
			return error;
		}
		return video_lifecycle_validation_error(lifecycle.accept_frame(frame, target, 1U));
	}
	if (name == "video_codec_not_negotiated" || name == "video_profile_not_negotiated" ||
		name == "video_resolution_not_negotiated" || name == "video_bitrate_not_negotiated") {
		TargetVideoConfigPayload config;
		if (const auto error = decode_target_video_config_payload(byte_view(input), config);
			error != ValidationError::None) {
			return error;
		}
		const auto subscribe_bytes = read_binary(asset_root() / "vectors" / "valid" / "messages" /
			"target_video_subscribe" / "target_video_subscribe.bin");
		TargetVideoSubscribePayload subscribe;
		if (const auto error = decode_target_video_subscribe_payload(byte_view(subscribe_bytes), subscribe);
			error != ValidationError::None) {
			return error;
		}
		if (name == "video_bitrate_not_negotiated") {
			subscribe.max_bitrate_kbps = 4000U;
		} else if (name == "video_resolution_not_negotiated") {
			subscribe.max_width = 958U;
			subscribe.preferred_width = 958U;
		}
		return name == "video_codec_not_negotiated"
				   ? validate_target_video_config_for_subscribe(config, subscribe, false)
				   : validate_target_video_config_for_subscribe(config, subscribe);
	}
	return ValidationError::UnknownMessageType;
}

ValidationError validate_invalid_contextual_record_fixture(const std::string& name,
	const std::vector<std::uint8_t>& input) {
	RecordEnvelopeIterator iterator(byte_view(input), 1U, RecordFlagPolicy::AllowV1Mutations);
	RecordEnvelopeView record;
	bool has_value = false;
	if (const auto error = iterator.next(record, has_value); error != ValidationError::None) {
		return error;
	}
	if (!has_value) {
		return ValidationError::TruncatedPayload;
	}
	if (name == "comm_bundle_hash_mismatch") {
		CommAssetManifestPayloadView manifest;
		if (const auto error = decode_comm_asset_manifest_payload(record.payload, manifest);
			error != ValidationError::None) {
			return error;
		}
		CommViewClientLifecycle lifecycle;
		lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
		Sha256Digest required_hash{};
		required_hash.fill(1U);
		return comm_view_lifecycle_validation_error(
			lifecycle.install_bundle(1U, required_hash, manifest.bundle_hash));
	}
	if (name == "comm_duration_mismatch") {
		CommViewEventPayload event;
		if (const auto error = decode_comm_view_event_payload(record.payload, event);
			error != ValidationError::None) {
			return error;
		}
		CommViewClientLifecycle lifecycle;
		lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
		if (lifecycle.install_bundle(1U) != CommViewClientLifecycleResult::Applied) {
			return ValidationError::InternalSerializationError;
		}
		CommAssetRuntimeStatus asset;
		asset.asset_id = event.head_asset_id;
		asset.duration_us = 200'000U;
		asset.available = true;
		asset.content_valid = true;
		return comm_view_lifecycle_validation_error(lifecycle.apply_event(event, asset));
	}
	return ValidationError::UnknownRequiredRecord;
}

ReassembledMessage sentinel_message() {
	ReassembledMessage result;
	result.header.message_id = 0xfeedbeefU;
	result.message_class = MessageSizeClass::Video;
	result.payload = {0x5aU};
	return result;
}

void expect_sentinel(const ReassembledMessage& message) {
	EXPECT_EQ(0xfeedbeefU, message.header.message_id);
	EXPECT_EQ(MessageSizeClass::Video, message.message_class);
	EXPECT_EQ((std::vector<std::uint8_t>{0x5aU}), message.payload);
}

struct ValidVectorCase {
	std::size_t message_size;
	std::uint16_t fragment_count;
};

TEST(TelemetryProtocolVectors, EveryExternalMessagePayloadDecodesAndReencodesCanonically) {
	const auto root = asset_root() / "vectors" / "valid" / "messages";
	std::vector<std::filesystem::path> directories;
	for (const auto& entry : std::filesystem::directory_iterator(root)) {
		if (entry.is_directory()) {
			directories.push_back(entry.path());
		}
	}
	std::sort(directories.begin(), directories.end());
	ASSERT_EQ(20U, directories.size());

	for (const auto& directory : directories) {
		const auto name = directory.filename().string();
		SCOPED_TRACE(name);
		const auto input = read_binary(directory / (name + ".bin"));
		ASSERT_FALSE(input.empty());
		std::vector<std::uint8_t> encoded;
		EXPECT_EQ(ValidationError::None, roundtrip_message_fixture(name, input, encoded));
		EXPECT_EQ(input, encoded);
	}
}

TEST(TelemetryProtocolVectors, EveryExternalBusinessRecordDecodesAndReencodesCanonically) {
	const auto root = asset_root() / "vectors" / "valid" / "records";
	std::vector<std::filesystem::path> directories;
	for (const auto& entry : std::filesystem::directory_iterator(root)) {
		if (entry.is_directory()) {
			directories.push_back(entry.path());
		}
	}
	std::sort(directories.begin(), directories.end());
	ASSERT_EQ(28U, directories.size());

	for (const auto& directory : directories) {
		const auto name = directory.filename().string();
		SCOPED_TRACE(name);
		const auto input = read_binary(directory / (name + ".bin"));
		ASSERT_FALSE(input.empty());

		RecordEnvelopeIterator iterator(byte_view(input), 1U, RecordFlagPolicy::AllowV1Mutations);
		RecordEnvelopeView record;
		bool has_value = false;
		ASSERT_EQ(ValidationError::None, iterator.next(record, has_value));
		ASSERT_TRUE(has_value);
		bool trailing = true;
		RecordEnvelopeView ignored;
		ASSERT_EQ(ValidationError::None, iterator.next(ignored, trailing));
		ASSERT_FALSE(trailing);

		BusinessRecordMetadata metadata;
		ASSERT_EQ(ValidationError::None,
			validate_business_record(record,
				fixture_container(static_cast<RecordType>(record.raw_record_type)),
				metadata));
		ASSERT_EQ(record.raw_record_type, static_cast<std::uint16_t>(metadata.type));

		std::vector<std::uint8_t> encoded(input.size(), 0xa5U);
		std::size_t written = 0;
		ASSERT_EQ(ValidationError::None,
			encode_business_record(record,
				fixture_container(metadata.type),
				MutableByteView{encoded.data(), encoded.size()},
				written));
		EXPECT_EQ(input.size(), written);
		EXPECT_EQ(input, encoded);
	}
}

struct InvalidProtocolFixtureCase {
	const char* category;
	const char* name;
	ValidationError expected_error;
};

TEST(TelemetryProtocolVectors, ExternalInvalidPayloadOnlyMessagesMatchTheStableErrorTaxonomy) {
	const std::array<InvalidProtocolFixtureCase, 6> cases{{
		{"fixed_payload_truncated", "fixed_payload_truncated", ValidationError::TruncatedPayload},
		{"fixed_payload_trailing", "fixed_payload_trailing_byte", ValidationError::TrailingBytes},
		{"encoded_frame_size_mismatch", "video_encoded_frame_size_mismatch", ValidationError::TruncatedPayload},
		{"nack_bitmap_incoherent", "nack_bitmap_tail_bits_set", ValidationError::ReservedFlag},
		{"record_duplicate_singleton", "duplicate_singleton_record", ValidationError::DuplicateRecord},
		{"record_count_truncated_region",
			"event_batch_record_count_exceeds_region",
			ValidationError::TruncatedPayload},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "messages";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto input = read_binary(root / test_case.category / (std::string{test_case.name} + ".bin"));
		ASSERT_FALSE(input.empty());
		EXPECT_EQ(test_case.expected_error, validate_invalid_message_payload_fixture(test_case.name, input));
	}
}

TEST(TelemetryProtocolVectors, ExternalInvalidContextualMessagesMatchTheStableErrorTaxonomy) {
	const std::array<InvalidProtocolFixtureCase, 16> cases{{
		{"ack_forged", "ack_forged_target", ValidationError::InvalidStateTransition},
		{"ack_late", "ack_late_after_retention", ValidationError::InvalidStateTransition},
		{"ack_wrong_crc", "ack_wrong_target_crc", ValidationError::InvalidStateTransition},
		{"ack_wrong_endpoint", "ack_wrong_endpoint", ValidationError::EndpointMismatch},
		{"client_simulated_command", "client_simulated_producer_command", ValidationError::WrongDirection},
		{"client_undefined_command", "unknown_message_type", ValidationError::UnknownMessageType},
		{"stale_video_target", "video_old_target_after_change", ValidationError::StaleGeneration},
		{"unknown_baseline", "delta_unknown_baseline", ValidationError::StaleBaseline},
		{"unknown_generation", "video_stale_generation", ValidationError::StaleGeneration},
		{"unknown_manifest", "snapshot_missing_manifest", ValidationError::MissingManifest},
		{"unknown_stream", "video_unknown_stream", ValidationError::InvalidStateTransition},
		{"unknown_target", "video_unknown_target", ValidationError::UnknownEntity},
		{"video_bitrate_not_negotiated", "video_bitrate_not_negotiated", ValidationError::CapabilityNotNegotiated},
		{"video_codec_not_negotiated", "video_codec_not_negotiated", ValidationError::CapabilityNotNegotiated},
		{"video_profile_not_negotiated", "video_profile_not_negotiated", ValidationError::CapabilityNotNegotiated},
		{"video_resolution_not_negotiated",
			"video_resolution_not_negotiated",
			ValidationError::CapabilityNotNegotiated},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "messages";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto input = read_binary(root / test_case.category / (std::string{test_case.name} + ".bin"));
		if (std::string{test_case.name} != "unknown_message_type") {
			ASSERT_FALSE(input.empty());
		}
		EXPECT_EQ(test_case.expected_error, validate_invalid_contextual_message_fixture(test_case.name, input));
	}
}

TEST(TelemetryProtocolVectors, ExternalInvalidRecordFixturesMatchTheStableErrorTaxonomy) {
	const std::array<InvalidProtocolFixtureCase, 18> cases{{
		{"bool_outside_0_1", "bool_outside_closed_domain", ValidationError::OutOfRange},
		{"closed_enum_unknown", "closed_enum_unknown", ValidationError::UnknownEnum},
		{"comm_bundle_inconsistent", "comm_bundle_version_inconsistent", ValidationError::OutOfRange},
		{"invalid_utf8", "invalid_utf8_string", ValidationError::InvalidUtf8},
		{"non_finite_float", "nan_float", ValidationError::NonFiniteFloat},
		{"non_finite_float", "positive_infinity_float", ValidationError::NonFiniteFloat},
		{"non_finite_float", "negative_infinity_float", ValidationError::NonFiniteFloat},
		{"quaternion_non_canonical", "quaternion_non_canonical_sign", ValidationError::OutOfRange},
		{"quaternion_non_finite", "quaternion_non_finite", ValidationError::NonFiniteFloat},
		{"quaternion_non_normalizable", "quaternion_zero_non_normalizable", ValidationError::OutOfRange},
		{"quota_before_allocation", "comm_asset_count_over_quota", ValidationError::ResourceLimit},
		{"record_duplicate_item_key", "duplicate_comm_asset_id", ValidationError::DuplicateItemKey},
		{"record_length_excessive", "record_declared_length_exceeds_input", ValidationError::BadRecordLength},
		{"record_reserved_flag", "reserved_record_flag", ValidationError::ReservedFlag},
		{"record_truncated", "record_truncated_header", ValidationError::TruncatedPayload},
		{"record_invalid_type", "invalid_record_type_zero", ValidationError::OutOfRange},
		{"record_unsupported_version", "unsupported_record_version", ValidationError::UnsupportedRecordVersion},
		{"string_too_long", "string_over_field_limit", ValidationError::StringTooLong},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "records";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto input = read_binary(root / test_case.category / (std::string{test_case.name} + ".bin"));
		ASSERT_FALSE(input.empty());
		EXPECT_EQ(test_case.expected_error, validate_invalid_record_fixture(input));
	}
}

TEST(TelemetryProtocolVectors, ExternalInvalidContextualRecordsMatchTheStableErrorTaxonomy) {
	const std::array<InvalidProtocolFixtureCase, 2> cases{{
		{"comm_bundle_hash_inconsistent", "comm_bundle_hash_mismatch", ValidationError::InvalidStateTransition},
		{"comm_duration_inconsistent", "comm_duration_mismatch", ValidationError::InvalidStateTransition},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "records";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto input = read_binary(root / test_case.category / (std::string{test_case.name} + ".bin"));
		ASSERT_FALSE(input.empty());
		EXPECT_EQ(test_case.expected_error, validate_invalid_contextual_record_fixture(test_case.name, input));
	}
}

TEST(TelemetryProtocolVectors, ExternalCanonicalBoundaryFixturesDecodeAndReassembleExactly) {
	const std::array<ValidVectorCase, 6> cases{{
	    {0U, 1U}, {1U, 1U}, {1132U, 1U}, {1133U, 2U}, {2264U, 2U}, {2265U, 3U},
	}};

	for (const auto& test_case : cases) {
		SCOPED_TRACE(testing::Message() << "external vector size " << test_case.message_size);
		const auto directory = asset_root() / "vectors" / "valid" / "datagrams" / "transport" /
		                       ("transport_" + std::to_string(test_case.message_size) + "_bytes");
		const auto logical = expected_payload(test_case.message_size);
		const auto expected_crc = crc32_iso_hdlc(byte_view(logical));
		TelemetryReassembler reassembler;
		auto completed = sentinel_message();

		for (std::uint16_t index = 0; index < test_case.fragment_count; ++index) {
			const auto filename = (index < 10U ? "00" : index < 100U ? "0" : "") + std::to_string(index) + ".bin";
			const auto encoded = read_binary(directory / filename);
			const auto expected_slice = test_case.message_size == 0U
			                                ? 0U
			                                : std::min<std::size_t>(MaxFragmentPayload,
			                                                        test_case.message_size -
			                                                            static_cast<std::size_t>(index) * MaxFragmentPayload);
			ASSERT_EQ(HeaderSizeV1 + expected_slice, encoded.size());

			DatagramView decoded;
			ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(encoded), decoded));
			EXPECT_EQ(test_case.message_size == 0U ? MessageType::Heartbeat : MessageType::Delta,
			          decoded.header.message_type);
			EXPECT_EQ(test_case.fragment_count > 1U ? MessageFlagFragmented : MessageFlagNone,
			          decoded.header.flags);
			EXPECT_EQ(test_case.message_size, decoded.header.message_size);
			EXPECT_EQ(test_case.fragment_count, decoded.header.fragment_count);
			EXPECT_EQ(index, decoded.header.fragment_index);
			EXPECT_EQ(static_cast<std::size_t>(index) * MaxFragmentPayload, decoded.header.fragment_offset);
			EXPECT_EQ(expected_slice, decoded.header.payload_size);
			EXPECT_EQ(expected_slice, decoded.payload.size);
			EXPECT_EQ(expected_crc, decoded.header.message_crc32);
			if (expected_slice != 0U) {
				EXPECT_TRUE(std::equal(decoded.payload.begin(),
				                       decoded.payload.end(),
				                       logical.begin() + static_cast<std::ptrdiff_t>(decoded.header.fragment_offset)));
			}

			const auto expected_result = index + 1U == test_case.fragment_count ? ReassemblyResult::Completed
			                                                                    : ReassemblyResult::Accepted;
			ASSERT_EQ(expected_result, reassembler.ingest(decoded, completed));
			if (expected_result != ReassemblyResult::Completed) {
				expect_sentinel(completed);
			}
		}
		EXPECT_EQ(logical, completed.payload);
		EXPECT_EQ(ValidationError::None, validate_message_crc(completed.header, completed.payload_view()));
		EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
		EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));
	}
}

TEST(TelemetryProtocolVectors, EveryExternalTruncatedHeaderFixtureReturnsDatagramTooShort) {
	const auto root = asset_root() / "vectors" / "invalid" / "datagrams" / "transport";
	for (std::size_t size = 0; size < HeaderSizeV1; ++size) {
		SCOPED_TRACE(testing::Message() << "external truncated header size " << size);
		const auto suffix = size < 10U ? "0" + std::to_string(size) : std::to_string(size);
		const auto encoded = read_binary(root / ("truncated_header_" + suffix) / "000.bin");
		ASSERT_EQ(size, encoded.size());
		DatagramView decoded;
		decoded.header.message_id = 0xfeedbeefU;
		EXPECT_EQ(ValidationError::DatagramTooShort, decode_and_validate_datagram(byte_view(encoded), decoded));
		EXPECT_EQ(0U, decoded.header.message_id);
		EXPECT_EQ(nullptr, decoded.payload.data);
		EXPECT_EQ(0U, decoded.payload.size);
	}
}

struct InvalidDatagramCase {
	const char* name;
	ValidationError expected_error;
};

TEST(TelemetryProtocolVectors, ExternalSingleDatagramMutationsReturnTheirDeclaredErrors) {
	const std::array<InvalidDatagramCase, 8> cases{{
	    {"bad_datagram_crc", ValidationError::BadDatagramCrc},
	    {"reserved_header_flag", ValidationError::ReservedHeaderFlag},
	    {"zero_fragment_count", ValidationError::BadFragmentCount},
	    {"fragment_index_out_of_range", ValidationError::BadFragmentIndex},
	    {"fragment_offset_noncanonical", ValidationError::BadFragmentOffset},
	    {"fragment_slice_noncanonical", ValidationError::BadFragmentSlice},
	    {"trailing_datagram_byte", ValidationError::BadDatagramLength},
	    // The datagram itself is valid; this one is checked after logical reassembly below.
	    {"bad_message_crc", ValidationError::None},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "datagrams" / "transport";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto encoded = read_binary(root / test_case.name / "000.bin");
		DatagramView decoded;
		const auto actual = decode_and_validate_datagram(byte_view(encoded), decoded);
		EXPECT_EQ(test_case.expected_error, actual);
		if (actual != ValidationError::None) {
			EXPECT_EQ(nullptr, decoded.payload.data);
			EXPECT_EQ(0U, decoded.payload.size);
		}

		if (std::string{test_case.name} == "bad_message_crc") {
			TelemetryReassembler reassembler;
			auto completed = sentinel_message();
			ASSERT_EQ(ValidationError::None, actual);
			EXPECT_EQ(ReassemblyResult::MessageCrcMismatch, reassembler.ingest(decoded, completed));
			expect_sentinel(completed);
			EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
		}
	}
}

TEST(TelemetryProtocolVectors, ExternalMetadataAndDuplicateContradictionsPurgeReassembly) {
	const std::array<const char*, 2> names{{"inconsistent_fragment_metadata", "contradictory_duplicate_fragment"}};
	const auto root = asset_root() / "vectors" / "invalid" / "datagrams" / "transport";
	for (const auto* name : names) {
		SCOPED_TRACE(name);
		TelemetryReassembler reassembler;
		auto completed = sentinel_message();
		for (std::size_t index = 0; index < 2U; ++index) {
			const auto filename = index == 0U ? "000.bin" : "001.bin";
			const auto encoded = read_binary(root / name / filename);
			DatagramView decoded;
			ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(encoded), decoded));
			const auto expected = index == 0U ? ReassemblyResult::Accepted : ReassemblyResult::InconsistentFragment;
			EXPECT_EQ(expected, reassembler.ingest(decoded, completed));
			expect_sentinel(completed);
		}
		EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
		EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));
	}
}

} // namespace
