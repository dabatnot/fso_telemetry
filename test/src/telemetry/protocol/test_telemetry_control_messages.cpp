#include "telemetry/protocol/telemetry_control_messages.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using namespace telemetry::protocol;

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

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
	ASSERT_LE(offset + 2U, bytes.size());
	bytes[offset] = static_cast<std::uint8_t>(value);
	bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void put_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value)
{
	ASSERT_LE(offset + 8U, bytes.size());
	for (std::size_t index = 0; index < 8U; ++index) {
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
	}
}

std::vector<std::uint8_t>
raw_extension(std::uint16_t type, std::uint8_t version, std::uint8_t flags, const std::vector<std::uint8_t>& payload)
{
	std::vector<std::uint8_t> bytes(CapabilityExtensionHeaderSize + payload.size(), 0U);
	put_u16(bytes, 0U, type);
	bytes[2U] = version;
	bytes[3U] = flags;
	put_u16(bytes, 4U, static_cast<std::uint16_t>(payload.size()));
	for (std::size_t index = 0; index < payload.size(); ++index) {
		bytes[CapabilityExtensionHeaderSize + index] = payload[index];
	}
	return bytes;
}

HelloPayload valid_hello(ByteView extensions = ByteView{}, std::uint16_t extension_count = 0)
{
	HelloPayload payload;
	payload.client_nonce = 0x0102030405060708ULL;
	payload.client_send_t0_us = 123456U;
	payload.advertised_capabilities = CapabilityCommViewLocalAssets | CapabilityTargetVideoH264 | CapabilityUpdate;
	payload.requested_heartbeat_ms = 1000U;
	payload.extension_count = extension_count;
	payload.extensions = extensions;
	return payload;
}

WelcomePayload valid_welcome(ByteView extensions = ByteView{}, std::uint16_t extension_count = 0)
{
	WelcomePayload payload;
	payload.client_nonce = 0x0102030405060708ULL;
	payload.client_send_t0_us = 1000U;
	payload.producer_receive_t1_us = 1020U;
	payload.producer_send_t2_us = 1030U;
	payload.status = WelcomeStatus::Accepted;
	payload.selected_major = VersionMajor;
	payload.selected_minor = VersionMinor;
	payload.selected_visibility_mode = VisibilityMode::Cockpit;
	payload.producer_capabilities =
		CapabilityCommViewAuthoritativeSource | CapabilityTargetVideoRemoteRender | CapabilityUpdate;
	payload.active_capabilities = CapabilityCommViewLocalAssets | CapabilityCommViewAuthoritativeSource |
								  CapabilityTargetVideoH264 | CapabilityTargetVideoRemoteRender | CapabilityUpdate;
	payload.heartbeat_interval_ms = 1000U;
	payload.reliable_reassembly_timeout_ms = ReliableReassemblyTimeoutV1Ms;
	payload.extension_count = extension_count;
	payload.producer_id = 0x8877665544332211ULL;
	payload.extensions = extensions;
	return payload;
}

template <typename Payload, typename Encode>
std::vector<std::uint8_t> encode_payload(const Payload& payload, std::size_t size, Encode encode)
{
	std::vector<std::uint8_t> bytes(size, 0xa5U);
	std::size_t written = std::numeric_limits<std::size_t>::max();
	EXPECT_EQ(ValidationError::None, encode(payload, mutable_byte_view(bytes), written));
	EXPECT_EQ(size, written);
	return bytes;
}

DiscoveryPayload valid_discovery(ByteView producer_name = ByteView{})
{
	DiscoveryPayload payload;
	payload.producer_id = 0x0102030405060708ULL;
	payload.listen_port = 7808U;
	payload.producer_capabilities =
		CapabilityCommViewAuthoritativeSource | CapabilityTargetVideoRemoteRender | CapabilityUpdate;
	payload.advert_sequence = 0x11223344U;
	payload.producer_name = producer_name;
	return payload;
}

TEST(TelemetryProtocolControlMessages, DiscoveryGoldenRoundTripIsBoundedUtf8AndAtomic)
{
	const std::array<std::uint8_t, 4> name{{'F', 'S', 'T', 'L'}};
	const auto original = valid_discovery(byte_view(name));
	std::array<std::uint8_t, DiscoveryPayloadPrefixSize + name.size()> encoded{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None,
		encode_discovery_payload(original, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);
	const std::array<std::uint8_t, DiscoveryPayloadPrefixSize + name.size()> golden{{
		0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
		0x80U, 0x1eU, 0x01U, 0x01U, 0x00U, 0x00U,
		0x1aU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
		0x44U, 0x33U, 0x22U, 0x11U, 0x04U, 0x00U,
		'F', 'S', 'T', 'L',
	}};
	EXPECT_EQ(golden, encoded);

	DiscoveryPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_discovery_payload(byte_view(golden), decoded));
	EXPECT_EQ(original.producer_id, decoded.producer_id);
	EXPECT_EQ(original.listen_port, decoded.listen_port);
	EXPECT_EQ(original.producer_capabilities, decoded.producer_capabilities);
	EXPECT_EQ(original.advert_sequence, decoded.advert_sequence);
	ASSERT_EQ(name.size(), decoded.producer_name.size);
	EXPECT_TRUE(std::equal(name.begin(), name.end(), decoded.producer_name.begin()));

	std::array<std::uint8_t, encoded.size() - 1U> short_output{};
	short_output.fill(0xa5U);
	const auto canary = short_output;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_discovery_payload(original, mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, short_output);

	auto truncated = std::vector<std::uint8_t>(golden.begin(), golden.end() - 1U);
	decoded.producer_id = 99U;
	EXPECT_EQ(ValidationError::TruncatedPayload, decode_discovery_payload(byte_view(truncated), decoded));
	EXPECT_EQ(0U, decoded.producer_id);
	auto trailing = std::vector<std::uint8_t>(golden.begin(), golden.end());
	trailing.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_discovery_payload(byte_view(trailing), decoded));
}

TEST(TelemetryProtocolControlMessages, DiscoveryDecodingIsExtensibleButV1EmissionIsStrict)
{
	const std::array<std::uint8_t, 2> invalid_utf8{{0xc0U, 0xafU}};
	auto malformed = valid_discovery(byte_view(invalid_utf8));
	EXPECT_EQ(ValidationError::InvalidUtf8, validate_discovery_payload(malformed));
	const std::array<std::uint8_t, 3> embedded_nul{{'A', 0U, 'B'}};
	malformed.producer_name = byte_view(embedded_nul);
	EXPECT_EQ(ValidationError::InvalidUtf8, validate_discovery_payload(malformed));
	malformed = valid_discovery();
	malformed.producer_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_discovery_payload(malformed));
	malformed = valid_discovery();
	malformed.listen_port = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_discovery_payload(malformed));
	malformed = valid_discovery();
	malformed.min_major = 0U;
	EXPECT_EQ(ValidationError::UnsupportedMajor, validate_discovery_payload(malformed));

	auto bytes = encode_payload(valid_discovery(), DiscoveryPayloadPrefixSize, encode_discovery_payload);
	const auto unknown_bit = std::uint64_t{1} << 63U;
	put_u64(bytes, 14U, valid_discovery().producer_capabilities | unknown_bit);
	DiscoveryPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_discovery_payload(byte_view(bytes), decoded));
	EXPECT_NE(0U, decoded.producer_capabilities & unknown_bit);
	std::array<std::uint8_t, DiscoveryPayloadPrefixSize> output{};
	output.fill(0xa5U);
	const auto canary = output;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::ReservedFlag,
		encode_discovery_payload(decoded, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, output);

	malformed = valid_discovery();
	malformed.producer_capabilities |= CapabilityCommViewLocalAssets;
	written = 99U;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		encode_discovery_payload(malformed, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
}

TEST(TelemetryProtocolControlMessages, CapabilityExtensionsDecodeUnknownButRejectInvalidOrDuplicateEnvelopes)
{
	const auto unknown = raw_extension(0x1234U, 7U, 0U, {0xaaU, 0xbbU});
	CapabilityExtensionIterator iterator(byte_view(unknown), 1U);
	CapabilityExtensionView extension;
	bool has_value = false;
	ASSERT_EQ(ValidationError::None, iterator.next(extension, has_value));
	ASSERT_TRUE(has_value);
	EXPECT_EQ(0x1234U, extension.type);
	EXPECT_EQ(7U, extension.version);
	ASSERT_EQ(2U, extension.payload.size);
	EXPECT_EQ(0xaaU, extension.payload.data[0]);
	EXPECT_EQ(0xbbU, extension.payload.data[1]);
	ASSERT_EQ(ValidationError::None, iterator.next(extension, has_value));
	EXPECT_FALSE(has_value);
	EXPECT_EQ(1U, iterator.consumed_count());
	EXPECT_EQ(unknown.size(), iterator.consumed_bytes());

	auto duplicate = raw_extension(1U, 1U, 0U, {});
	const auto second = raw_extension(1U, 2U, 0U, {0xccU});
	duplicate.insert(duplicate.end(), second.begin(), second.end());
	EXPECT_EQ(ValidationError::DuplicateItemKey, validate_capability_extensions(byte_view(duplicate), 2U));

	auto zero_type = raw_extension(0U, 1U, 0U, {});
	EXPECT_EQ(ValidationError::OutOfRange, validate_capability_extensions(byte_view(zero_type), 1U));
	auto reserved_flags = raw_extension(1U, 1U, 1U, {});
	EXPECT_EQ(ValidationError::ReservedFlag, validate_capability_extensions(byte_view(reserved_flags), 1U));

	auto truncated = raw_extension(1U, 1U, 0U, {1U});
	truncated.pop_back();
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_capability_extensions(byte_view(truncated), 1U));
	EXPECT_EQ(ValidationError::TrailingBytes, validate_capability_extensions(byte_view(unknown), 0U));
	EXPECT_EQ(ValidationError::OutOfRange,
		validate_capability_extensions(byte_view(unknown), MaxControlExtensionCount + 1U));
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_capability_extensions(ByteView{nullptr, 1U}, 1U));
}

TEST(TelemetryProtocolControlMessages, CapabilityExtensionEncodingIsExactAtomicAndV1Strict)
{
	const std::array<std::uint8_t, 3> payload{{0x10U, 0x20U, 0x30U}};
	CapabilityExtensionView extension;
	extension.type = static_cast<std::uint16_t>(CapabilityExtensionType::CommViewNegotiation);
	extension.version = 1U;
	extension.payload = byte_view(payload);

	std::array<std::uint8_t, CapabilityExtensionHeaderSize + payload.size()> output{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_capability_extension(extension, mutable_byte_view(output), written));
	EXPECT_EQ(output.size(), written);
	const std::array<std::uint8_t, 9> golden{{1U, 0U, 1U, 0U, 3U, 0U, 0x10U, 0x20U, 0x30U}};
	EXPECT_EQ(golden, output);

	std::array<std::uint8_t, output.size() - 1U> short_output{};
	short_output.fill(0xa5U);
	const auto canary = short_output;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_capability_extension(extension, mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, short_output);

	extension.type = static_cast<std::uint16_t>(CapabilityExtensionType::TargetVideoNegotiation);
	written = 99U;
	EXPECT_EQ(ValidationError::OutOfRange, encode_capability_extension(extension, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
	extension.type = FirstReservedCapabilityExtensionType;
	EXPECT_EQ(ValidationError::OutOfRange, encode_capability_extension(extension, mutable_byte_view(output), written));
}

TEST(TelemetryProtocolControlMessages, HelloRoundTripEnforcesExactLengthReservedBytesAndOutputSemantics)
{
	const auto extension = raw_extension(1U, 1U, 0U, {0x42U});
	const auto original = valid_hello(byte_view(extension), 1U);
	auto bytes = encode_payload(original, HelloPayloadPrefixSize + extension.size(), encode_hello_payload);

	HelloPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_hello_payload(byte_view(bytes), decoded));
	EXPECT_EQ(original.client_nonce, decoded.client_nonce);
	EXPECT_EQ(original.client_send_t0_us, decoded.client_send_t0_us);
	EXPECT_EQ(original.advertised_capabilities, decoded.advertised_capabilities);
	EXPECT_EQ(original.requested_heartbeat_ms, decoded.requested_heartbeat_ms);
	EXPECT_EQ(1U, decoded.extension_count);
	EXPECT_EQ(extension.size(), decoded.extensions.size);

	auto truncated = bytes;
	truncated.pop_back();
	decoded.client_nonce = 99U;
	EXPECT_EQ(ValidationError::TruncatedPayload, decode_hello_payload(byte_view(truncated), decoded));
	EXPECT_EQ(0U, decoded.client_nonce);
	auto trailing = bytes;
	trailing.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_hello_payload(byte_view(trailing), decoded));

	auto reserved = bytes;
	reserved[21U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_hello_payload(byte_view(reserved), decoded));
	auto unknown_visibility = bytes;
	unknown_visibility[20U] = 0xffU;
	EXPECT_EQ(ValidationError::UnknownEnum, decode_hello_payload(byte_view(unknown_visibility), decoded));

	std::vector<std::uint8_t> short_output(bytes.size() - 1U, 0xa5U);
	const auto canary = short_output;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_hello_payload(original, mutable_byte_view(short_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, short_output);
}

TEST(TelemetryProtocolControlMessages,
	HelloDecodeRetainsUnknownCapabilitiesAndReservedExtensionTypesWithoutReemittingThem)
{
	auto bytes = encode_payload(valid_hello(), HelloPayloadPrefixSize, encode_hello_payload);
	const auto unknown_bit = std::uint64_t{1} << 63U;
	put_u64(bytes, 24U, valid_hello().advertised_capabilities | unknown_bit);

	HelloPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_hello_payload(byte_view(bytes), decoded));
	EXPECT_NE(0U, decoded.advertised_capabilities & unknown_bit);
	std::vector<std::uint8_t> output(HelloPayloadPrefixSize, 0xa5U);
	const auto canary = output;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::ReservedFlag, encode_hello_payload(decoded, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, output);

	const auto reserved_extension =
		raw_extension(static_cast<std::uint16_t>(CapabilityExtensionType::TargetVideoNegotiation), 1U, 0U, {});
	bytes = encode_payload(valid_hello(), HelloPayloadPrefixSize, encode_hello_payload);
	put_u16(bytes, 34U, static_cast<std::uint16_t>(reserved_extension.size()));
	put_u16(bytes, 36U, 1U);
	bytes.insert(bytes.end(), reserved_extension.begin(), reserved_extension.end());
	ASSERT_EQ(ValidationError::None, decode_hello_payload(byte_view(bytes), decoded));
	EXPECT_EQ(1U, decoded.extension_count);
	output.assign(bytes.size(), 0xa5U);
	written = 99U;
	EXPECT_EQ(ValidationError::OutOfRange, encode_hello_payload(decoded, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
}

TEST(TelemetryProtocolControlMessages, HelloAndWelcomeEmissionEnforceCapabilityOwnership)
{
	auto hello = valid_hello();
	hello.advertised_capabilities |= CapabilityCommViewAuthoritativeSource;
	std::array<std::uint8_t, HelloPayloadPrefixSize> hello_output{};
	hello_output.fill(0xa5U);
	const auto hello_canary = hello_output;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		encode_hello_payload(hello, mutable_byte_view(hello_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(hello_canary, hello_output);

	// Receive-side ownership is deliberately permissive so the negotiation
	// owner can answer InvalidCapabilities instead of rejecting the envelope.
	auto hello_bytes = encode_payload(valid_hello(), HelloPayloadPrefixSize, encode_hello_payload);
	put_u64(hello_bytes, 24U, valid_hello().advertised_capabilities | CapabilityCommViewAuthoritativeSource);
	HelloPayload decoded_hello;
	ASSERT_EQ(ValidationError::None, decode_hello_payload(byte_view(hello_bytes), decoded_hello));
	written = 99U;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		encode_hello_payload(decoded_hello, mutable_byte_view(hello_output), written));

	auto welcome = valid_welcome();
	welcome.producer_capabilities |= CapabilityCommViewLocalAssets;
	std::array<std::uint8_t, WelcomePayloadPrefixSize> welcome_output{};
	welcome_output.fill(0xa5U);
	const auto welcome_canary = welcome_output;
	written = 99U;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		encode_welcome_payload(welcome, mutable_byte_view(welcome_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(welcome_canary, welcome_output);

	auto welcome_bytes = encode_payload(valid_welcome(), WelcomePayloadPrefixSize, encode_welcome_payload);
	put_u64(welcome_bytes, 36U, valid_welcome().producer_capabilities | CapabilityCommViewLocalAssets);
	WelcomePayload decoded_welcome;
	ASSERT_EQ(ValidationError::None, decode_welcome_payload(byte_view(welcome_bytes), decoded_welcome));
	written = 99U;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		encode_welcome_payload(decoded_welcome, mutable_byte_view(welcome_output), written));
}

TEST(TelemetryProtocolControlMessages, WelcomeAcceptedAndRejectedFormsRoundTripAndRejectContradictions)
{
	const auto accepted = valid_welcome();
	auto bytes = encode_payload(accepted, WelcomePayloadPrefixSize, encode_welcome_payload);
	WelcomePayload decoded;
	ASSERT_EQ(ValidationError::None, decode_welcome_payload(byte_view(bytes), decoded));
	EXPECT_EQ(WelcomeStatus::Accepted, decoded.status);
	EXPECT_EQ(accepted.active_capabilities, decoded.active_capabilities);
	EXPECT_EQ(accepted.producer_id, decoded.producer_id);

	WelcomePayload rejected = accepted;
	rejected.status = WelcomeStatus::Busy;
	rejected.selected_major = 0U;
	rejected.selected_minor = 0U;
	rejected.selected_visibility_mode = VisibilityMode::Cockpit;
	rejected.producer_capabilities = 0U;
	rejected.active_capabilities = 0U;
	rejected.heartbeat_interval_ms = 0U;
	rejected.reliable_reassembly_timeout_ms = 0U;
	bytes = encode_payload(rejected, WelcomePayloadPrefixSize, encode_welcome_payload);
	ASSERT_EQ(ValidationError::None, decode_welcome_payload(byte_view(bytes), decoded));
	EXPECT_EQ(WelcomeStatus::Busy, decoded.status);

	rejected.heartbeat_interval_ms = 1U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_welcome_payload(rejected));
	auto unknown_status = bytes;
	unknown_status[32U] = 0xffU;
	EXPECT_EQ(ValidationError::UnknownEnum, decode_welcome_payload(byte_view(unknown_status), decoded));
	auto trailing = bytes;
	trailing.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_welcome_payload(byte_view(trailing), decoded));
	auto truncated = bytes;
	truncated.pop_back();
	EXPECT_EQ(ValidationError::TruncatedPayload, decode_welcome_payload(byte_view(truncated), decoded));
}

TEST(TelemetryProtocolControlMessages, WelcomeDecodeRetainsUnknownCapabilityBitsButEncodeRejectsThem)
{
	auto bytes = encode_payload(valid_welcome(), WelcomePayloadPrefixSize, encode_welcome_payload);
	const auto unknown_bit = std::uint64_t{1} << 63U;
	put_u64(bytes, 36U, valid_welcome().producer_capabilities | unknown_bit);
	put_u64(bytes, 44U, valid_welcome().active_capabilities | unknown_bit);

	WelcomePayload decoded;
	ASSERT_EQ(ValidationError::None, decode_welcome_payload(byte_view(bytes), decoded));
	EXPECT_NE(0U, decoded.producer_capabilities & unknown_bit);
	EXPECT_NE(0U, decoded.active_capabilities & unknown_bit);
	std::vector<std::uint8_t> output(bytes.size(), 0xa5U);
	const auto canary = output;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::ReservedFlag, encode_welcome_payload(decoded, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, output);
}

TEST(TelemetryProtocolControlMessages, SessionBeginEnforcesFlagsPresenceRelationsAndExactSize)
{
	SessionBeginPayload payload;
	payload.session_flags = SessionBeginFlagReadOnly | SessionBeginFlagMissionActive | SessionBeginFlagManifestRequired;
	payload.producer_session_start_us = 77U;
	payload.mission_instance_id = 88U;
	payload.initial_snapshot_id = 9U;
	payload.required_manifest_id = 10U;
	auto bytes = encode_payload(payload, SessionBeginPayloadSize, encode_session_begin_payload);
	SessionBeginPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_session_begin_payload(byte_view(bytes), decoded));
	EXPECT_EQ(payload.session_flags, decoded.session_flags);
	EXPECT_EQ(payload.mission_instance_id, decoded.mission_instance_id);
	EXPECT_EQ(payload.required_manifest_id, decoded.required_manifest_id);

	auto malformed = payload;
	malformed.session_flags = SessionBeginFlagNone;
	EXPECT_EQ(ValidationError::InvalidAbsence, validate_session_begin_payload(malformed));
	malformed = payload;
	malformed.session_flags |= 0x80000000U;
	EXPECT_EQ(ValidationError::ReservedFlag, validate_session_begin_payload(malformed));
	malformed = payload;
	malformed.mission_instance_id = 0U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_session_begin_payload(malformed));
	malformed = payload;
	malformed.required_manifest_id = 0U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_session_begin_payload(malformed));

	auto truncated = bytes;
	truncated.pop_back();
	decoded.session_flags = 0xffffffffU;
	EXPECT_EQ(ValidationError::TruncatedPayload, decode_session_begin_payload(byte_view(truncated), decoded));
	EXPECT_EQ(0U, decoded.session_flags);
	bytes.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_session_begin_payload(byte_view(bytes), decoded));
}

TEST(TelemetryProtocolControlMessages, HeartbeatEnforcesKindReservedBytesTimestampsAndExactSize)
{
	HeartbeatPayload request;
	request.probe_id = 7U;
	request.kind = HeartbeatKind::Request;
	request.origin_t0_us = 123U;
	auto bytes = encode_payload(request, HeartbeatPayloadSize, encode_heartbeat_payload);
	HeartbeatPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_heartbeat_payload(byte_view(bytes), decoded));
	EXPECT_EQ(request.probe_id, decoded.probe_id);
	EXPECT_EQ(request.origin_t0_us, decoded.origin_t0_us);

	HeartbeatPayload response = request;
	response.kind = HeartbeatKind::Response;
	response.receive_t1_us = 130U;
	response.transmit_t2_us = 140U;
	bytes = encode_payload(response, HeartbeatPayloadSize, encode_heartbeat_payload);
	ASSERT_EQ(ValidationError::None, decode_heartbeat_payload(byte_view(bytes), decoded));

	auto malformed = response;
	malformed.probe_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_heartbeat_payload(malformed));
	malformed = request;
	malformed.receive_t1_us = 1U;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_heartbeat_payload(malformed));
	malformed = response;
	malformed.transmit_t2_us = malformed.receive_t1_us - 1U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_heartbeat_payload(malformed));

	auto reserved = bytes;
	reserved[5U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_heartbeat_payload(byte_view(reserved), decoded));
	auto unknown_kind = bytes;
	unknown_kind[4U] = 0xffU;
	EXPECT_EQ(ValidationError::UnknownEnum, decode_heartbeat_payload(byte_view(unknown_kind), decoded));
	auto truncated = bytes;
	truncated.pop_back();
	EXPECT_EQ(ValidationError::TruncatedPayload, decode_heartbeat_payload(byte_view(truncated), decoded));
	bytes.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_heartbeat_payload(byte_view(bytes), decoded));
}

TEST(TelemetryProtocolControlMessages, CapabilityUpdateDecodeIsExtensibleButEncodingAndV1TailAreStrict)
{
	CapabilityUpdatePayload update;
	update.capability_generation = 1U;
	update.advertised_capabilities = CapabilityCommViewLocalAssets | CapabilityUpdate;
	update.active_capabilities =
		CapabilityCommViewLocalAssets | CapabilityCommViewAuthoritativeSource | CapabilityUpdate;
	update.effective_time_us = 500U;
	update.reason = CapabilityUpdateReason::RuntimeAvailability;
	auto bytes = encode_payload(update, CapabilityUpdatePayloadSize, encode_capability_update_payload);
	CapabilityUpdatePayload decoded;
	ASSERT_EQ(ValidationError::None, decode_capability_update_payload(byte_view(bytes), decoded));
	EXPECT_EQ(update.capability_generation, decoded.capability_generation);
	EXPECT_EQ(update.active_capabilities, decoded.active_capabilities);

	const auto unknown_bit = std::uint64_t{1} << 63U;
	put_u64(bytes, 4U, update.advertised_capabilities | unknown_bit);
	put_u64(bytes, 12U, update.active_capabilities | unknown_bit);
	ASSERT_EQ(ValidationError::None, decode_capability_update_payload(byte_view(bytes), decoded));
	EXPECT_NE(0U, decoded.advertised_capabilities & unknown_bit);
	std::vector<std::uint8_t> output(bytes.size(), 0xa5U);
	const auto canary = output;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::ReservedFlag,
		encode_capability_update_payload(decoded, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(canary, output);

	auto malformed = bytes;
	malformed[29U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_capability_update_payload(byte_view(malformed), decoded));
	malformed = bytes;
	put_u16(malformed, 32U, 1U);
	EXPECT_EQ(ValidationError::OutOfRange, decode_capability_update_payload(byte_view(malformed), decoded));
	malformed = bytes;
	malformed[28U] = 0xffU;
	EXPECT_EQ(ValidationError::UnknownEnum, decode_capability_update_payload(byte_view(malformed), decoded));
	malformed = bytes;
	malformed[0U] = malformed[1U] = malformed[2U] = malformed[3U] = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, decode_capability_update_payload(byte_view(malformed), decoded));

	update.active_capabilities = CapabilityCommViewLocalAssets;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_capability_update_payload(update));
	auto truncated = bytes;
	truncated.pop_back();
	EXPECT_EQ(ValidationError::TruncatedPayload, decode_capability_update_payload(byte_view(truncated), decoded));
	bytes.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_capability_update_payload(byte_view(bytes), decoded));
}

} // namespace
