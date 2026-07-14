#include "telemetry/protocol/telemetry_state_messages.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace telemetry::protocol;

constexpr std::array<std::uint8_t, 6> ExtensionRecord{{
	0x1dU,
	0x00U,
	0x01U,
	0x00U,
	0x00U,
	0x00U,
}};

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

Sha256Digest sequential_digest(std::uint8_t first)
{
	Sha256Digest digest{};
	for (std::size_t index = 0; index < digest.size(); ++index) {
		digest[index] = static_cast<std::uint8_t>(first + index);
	}
	return digest;
}

ManifestPartPayload valid_manifest(ByteView records = byte_view(ExtensionRecord))
{
	ManifestPartPayload payload;
	payload.manifest_id = 0x11223344U;
	payload.part_index = 1U;
	payload.part_count = 3U;
	payload.transaction_size = 18U;
	payload.transaction_sha256 = sequential_digest(0x80U);
	payload.producer_sample_time_us = 0x0102030405060708ULL;
	payload.manifest_kind = ManifestKind::FullRequired;
	payload.record_count = 1U;
	payload.records = records;
	return payload;
}

FullSnapshotPartPayload valid_snapshot(ByteView records = byte_view(ExtensionRecord))
{
	FullSnapshotPartPayload payload;
	payload.snapshot_id = 0xa1b2c3d4U;
	payload.part_index = 0U;
	payload.part_count = 2U;
	payload.transaction_size = 12U;
	payload.transaction_sha256 = sequential_digest(0x20U);
	payload.producer_sample_time_us = 0x1112131415161718ULL;
	payload.required_manifest_id = 0x55667788U;
	payload.snapshot_flags = SnapshotFlagPeriodicKeyframe;
	payload.record_count = 1U;
	payload.records = records;
	return payload;
}

DeltaPayload valid_delta(ByteView records = byte_view(ExtensionRecord))
{
	DeltaPayload payload;
	payload.baseline_snapshot_id = 0x11223344U;
	payload.delta_sequence = 0x55667788U;
	payload.producer_sample_time_us = 0x0102030405060708ULL;
	payload.record_count = 1U;
	payload.records = records;
	return payload;
}

template <typename Payload>
void expect_records(const Payload& payload)
{
	ASSERT_EQ(ExtensionRecord.size(), payload.records.size);
	EXPECT_TRUE(std::equal(ExtensionRecord.begin(), ExtensionRecord.end(), payload.records.begin()));
}

TEST(TelemetryProtocolStateMessages, ManifestGoldenBytesRoundTripAndAliasedEncoding)
{
	const auto original = valid_manifest();
	std::array<std::uint8_t, ManifestPartPayloadPrefixSize + ExtensionRecord.size()> encoded{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_manifest_part_payload(original, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);

	const std::array<std::uint8_t, ManifestPartPayloadPrefixSize + ExtensionRecord.size()> golden{{
		0x44U,
		0x33U,
		0x22U,
		0x11U,
		0x01U,
		0x00U,
		0x03U,
		0x00U,
		0x12U,
		0x00U,
		0x00U,
		0x00U,
		0x80U,
		0x81U,
		0x82U,
		0x83U,
		0x84U,
		0x85U,
		0x86U,
		0x87U,
		0x88U,
		0x89U,
		0x8aU,
		0x8bU,
		0x8cU,
		0x8dU,
		0x8eU,
		0x8fU,
		0x90U,
		0x91U,
		0x92U,
		0x93U,
		0x94U,
		0x95U,
		0x96U,
		0x97U,
		0x98U,
		0x99U,
		0x9aU,
		0x9bU,
		0x9cU,
		0x9dU,
		0x9eU,
		0x9fU,
		0x08U,
		0x07U,
		0x06U,
		0x05U,
		0x04U,
		0x03U,
		0x02U,
		0x01U,
		0x01U,
		0x00U,
		0x01U,
		0x00U,
		0x1dU,
		0x00U,
		0x01U,
		0x00U,
		0x00U,
		0x00U,
	}};
	EXPECT_EQ(golden, encoded);

	ManifestPartPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_manifest_part_payload(byte_view(golden), decoded));
	EXPECT_EQ(original.manifest_id, decoded.manifest_id);
	EXPECT_EQ(original.part_index, decoded.part_index);
	EXPECT_EQ(original.part_count, decoded.part_count);
	EXPECT_EQ(original.transaction_size, decoded.transaction_size);
	EXPECT_EQ(original.transaction_sha256, decoded.transaction_sha256);
	EXPECT_EQ(original.producer_sample_time_us, decoded.producer_sample_time_us);
	EXPECT_EQ(original.manifest_kind, decoded.manifest_kind);
	EXPECT_EQ(original.record_count, decoded.record_count);
	expect_records(decoded);

	std::array<std::uint8_t, ManifestPartPayloadPrefixSize + ExtensionRecord.size()> aliased{};
	std::copy(ExtensionRecord.begin(), ExtensionRecord.end(), aliased.begin());
	auto alias_payload = valid_manifest(ByteView{aliased.data(), ExtensionRecord.size()});
	written = 99U;
	ASSERT_EQ(ValidationError::None, encode_manifest_part_payload(alias_payload, mutable_byte_view(aliased), written));
	EXPECT_EQ(golden, aliased);
}

TEST(TelemetryProtocolStateMessages, FullSnapshotGoldenBytesRoundTripAndAliasedEncoding)
{
	const auto original = valid_snapshot();
	std::array<std::uint8_t, FullSnapshotPartPayloadPrefixSize + ExtensionRecord.size()> encoded{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_full_snapshot_part_payload(original, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);

	const std::array<std::uint8_t, FullSnapshotPartPayloadPrefixSize + ExtensionRecord.size()> golden{{
		0xd4U,
		0xc3U,
		0xb2U,
		0xa1U,
		0x00U,
		0x00U,
		0x02U,
		0x00U,
		0x0cU,
		0x00U,
		0x00U,
		0x00U,
		0x20U,
		0x21U,
		0x22U,
		0x23U,
		0x24U,
		0x25U,
		0x26U,
		0x27U,
		0x28U,
		0x29U,
		0x2aU,
		0x2bU,
		0x2cU,
		0x2dU,
		0x2eU,
		0x2fU,
		0x30U,
		0x31U,
		0x32U,
		0x33U,
		0x34U,
		0x35U,
		0x36U,
		0x37U,
		0x38U,
		0x39U,
		0x3aU,
		0x3bU,
		0x3cU,
		0x3dU,
		0x3eU,
		0x3fU,
		0x18U,
		0x17U,
		0x16U,
		0x15U,
		0x14U,
		0x13U,
		0x12U,
		0x11U,
		0x88U,
		0x77U,
		0x66U,
		0x55U,
		0x02U,
		0x00U,
		0x01U,
		0x00U,
		0x1dU,
		0x00U,
		0x01U,
		0x00U,
		0x00U,
		0x00U,
	}};
	EXPECT_EQ(golden, encoded);

	FullSnapshotPartPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_full_snapshot_part_payload(byte_view(golden), decoded));
	EXPECT_EQ(original.snapshot_id, decoded.snapshot_id);
	EXPECT_EQ(original.part_index, decoded.part_index);
	EXPECT_EQ(original.part_count, decoded.part_count);
	EXPECT_EQ(original.transaction_size, decoded.transaction_size);
	EXPECT_EQ(original.transaction_sha256, decoded.transaction_sha256);
	EXPECT_EQ(original.producer_sample_time_us, decoded.producer_sample_time_us);
	EXPECT_EQ(original.required_manifest_id, decoded.required_manifest_id);
	EXPECT_EQ(original.snapshot_flags, decoded.snapshot_flags);
	EXPECT_EQ(original.record_count, decoded.record_count);
	expect_records(decoded);

	std::array<std::uint8_t, FullSnapshotPartPayloadPrefixSize + ExtensionRecord.size()> aliased{};
	std::copy(ExtensionRecord.begin(), ExtensionRecord.end(), aliased.begin());
	auto alias_payload = valid_snapshot(ByteView{aliased.data(), ExtensionRecord.size()});
	written = 99U;
	ASSERT_EQ(ValidationError::None,
		encode_full_snapshot_part_payload(alias_payload, mutable_byte_view(aliased), written));
	EXPECT_EQ(golden, aliased);
}

TEST(TelemetryProtocolStateMessages, DeltaGoldenBytesRoundTripAndAliasedEncoding)
{
	const auto original = valid_delta();
	std::array<std::uint8_t, DeltaPayloadPrefixSize + ExtensionRecord.size()> encoded{};
	std::size_t written = 99U;
	ASSERT_EQ(ValidationError::None, encode_delta_payload(original, mutable_byte_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);

	const std::array<std::uint8_t, DeltaPayloadPrefixSize + ExtensionRecord.size()> golden{{
		0x44U,
		0x33U,
		0x22U,
		0x11U,
		0x88U,
		0x77U,
		0x66U,
		0x55U,
		0x08U,
		0x07U,
		0x06U,
		0x05U,
		0x04U,
		0x03U,
		0x02U,
		0x01U,
		0x01U,
		0x00U,
		0x00U,
		0x00U,
		0x1dU,
		0x00U,
		0x01U,
		0x00U,
		0x00U,
		0x00U,
	}};
	EXPECT_EQ(golden, encoded);

	DeltaPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_delta_payload(byte_view(golden), decoded));
	EXPECT_EQ(original.baseline_snapshot_id, decoded.baseline_snapshot_id);
	EXPECT_EQ(original.delta_sequence, decoded.delta_sequence);
	EXPECT_EQ(original.producer_sample_time_us, decoded.producer_sample_time_us);
	EXPECT_EQ(original.record_count, decoded.record_count);
	expect_records(decoded);

	std::array<std::uint8_t, DeltaPayloadPrefixSize + ExtensionRecord.size()> aliased{};
	std::copy(ExtensionRecord.begin(), ExtensionRecord.end(), aliased.begin());
	auto alias_payload = valid_delta(ByteView{aliased.data(), ExtensionRecord.size()});
	written = 99U;
	ASSERT_EQ(ValidationError::None, encode_delta_payload(alias_payload, mutable_byte_view(aliased), written));
	EXPECT_EQ(golden, aliased);
}

TEST(TelemetryProtocolStateMessages, DecodersRejectEveryShortEncodingAndTrailingRecordBytes)
{
	std::vector<std::uint8_t> manifest(ManifestPartPayloadPrefixSize + ExtensionRecord.size());
	std::vector<std::uint8_t> snapshot(FullSnapshotPartPayloadPrefixSize + ExtensionRecord.size());
	std::vector<std::uint8_t> delta(DeltaPayloadPrefixSize + ExtensionRecord.size());
	std::size_t written = 0U;
	ASSERT_EQ(ValidationError::None,
		encode_manifest_part_payload(valid_manifest(), mutable_byte_view(manifest), written));
	ASSERT_EQ(ValidationError::None,
		encode_full_snapshot_part_payload(valid_snapshot(), mutable_byte_view(snapshot), written));
	ASSERT_EQ(ValidationError::None, encode_delta_payload(valid_delta(), mutable_byte_view(delta), written));

	ManifestPartPayload manifest_payload;
	for (std::size_t size = 0; size < manifest.size(); ++size) {
		manifest_payload = valid_manifest();
		EXPECT_EQ(ValidationError::TruncatedPayload,
			decode_manifest_part_payload(ByteView{manifest.data(), size}, manifest_payload));
		EXPECT_EQ(0U, manifest_payload.manifest_id);
	}
	FullSnapshotPartPayload snapshot_payload;
	for (std::size_t size = 0; size < snapshot.size(); ++size) {
		snapshot_payload = valid_snapshot();
		EXPECT_EQ(ValidationError::TruncatedPayload,
			decode_full_snapshot_part_payload(ByteView{snapshot.data(), size}, snapshot_payload));
		EXPECT_EQ(0U, snapshot_payload.snapshot_id);
	}
	DeltaPayload delta_payload;
	for (std::size_t size = 0; size < delta.size(); ++size) {
		delta_payload = valid_delta();
		EXPECT_EQ(ValidationError::TruncatedPayload, decode_delta_payload(ByteView{delta.data(), size}, delta_payload));
		EXPECT_EQ(0U, delta_payload.baseline_snapshot_id);
	}

	manifest.push_back(0U);
	snapshot.push_back(0U);
	delta.push_back(0U);
	EXPECT_EQ(ValidationError::TrailingBytes, decode_manifest_part_payload(byte_view(manifest), manifest_payload));
	EXPECT_EQ(ValidationError::TrailingBytes, decode_full_snapshot_part_payload(byte_view(snapshot), snapshot_payload));
	EXPECT_EQ(ValidationError::TrailingBytes, decode_delta_payload(byte_view(delta), delta_payload));
	EXPECT_EQ(ValidationError::TruncatedPayload,
		decode_manifest_part_payload(ByteView{nullptr, ManifestPartPayloadPrefixSize}, manifest_payload));
	EXPECT_EQ(ValidationError::TruncatedPayload,
		decode_full_snapshot_part_payload(ByteView{nullptr, FullSnapshotPartPayloadPrefixSize}, snapshot_payload));
	EXPECT_EQ(ValidationError::TruncatedPayload,
		decode_delta_payload(ByteView{nullptr, DeltaPayloadPrefixSize}, delta_payload));
}

TEST(TelemetryProtocolStateMessages, TransactionPartsEnforceIdsBoundsKindsFlagsAndRecordFraming)
{
	auto manifest = valid_manifest();
	manifest.manifest_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_manifest_part_payload(manifest));
	manifest = valid_manifest();
	manifest.part_count = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_manifest_part_payload(manifest));
	manifest.part_count = static_cast<std::uint16_t>(MaxTransactionParts + 1U);
	EXPECT_EQ(ValidationError::OutOfRange, validate_manifest_part_payload(manifest));
	manifest = valid_manifest();
	manifest.part_index = manifest.part_count;
	EXPECT_EQ(ValidationError::OutOfRange, validate_manifest_part_payload(manifest));
	manifest = valid_manifest();
	manifest.transaction_size = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_manifest_part_payload(manifest));
	manifest.transaction_size = static_cast<std::uint32_t>(MaxTransactionSize + 1U);
	EXPECT_EQ(ValidationError::OutOfRange, validate_manifest_part_payload(manifest));
	manifest = valid_manifest();
	manifest.record_count = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_manifest_part_payload(manifest));
	manifest = valid_manifest(ByteView{nullptr, ExtensionRecord.size()});
	EXPECT_EQ(ValidationError::TruncatedPayload, validate_manifest_part_payload(manifest));
	manifest = valid_manifest();
	manifest.manifest_kind = static_cast<ManifestKind>(0U);
	EXPECT_EQ(ValidationError::UnknownEnum, validate_manifest_part_payload(manifest));

	auto snapshot = valid_snapshot();
	snapshot.snapshot_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_full_snapshot_part_payload(snapshot));
	snapshot = valid_snapshot();
	snapshot.snapshot_flags = SnapshotFlagNone;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_full_snapshot_part_payload(snapshot));
	snapshot.snapshot_flags = SnapshotFlagInitial | SnapshotFlagResync;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_full_snapshot_part_payload(snapshot));
	snapshot.snapshot_flags = 0x8000U;
	EXPECT_EQ(ValidationError::ReservedFlag, validate_full_snapshot_part_payload(snapshot));

	auto malformed_record = ExtensionRecord;
	malformed_record[0U] = 0U;
	malformed_record[1U] = 0U;
	manifest = valid_manifest(byte_view(malformed_record));
	EXPECT_EQ(ValidationError::OutOfRange, validate_manifest_part_payload(manifest));
	malformed_record = ExtensionRecord;
	malformed_record[3U] = RecordFlagCreate;
	manifest = valid_manifest(byte_view(malformed_record));
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_manifest_part_payload(manifest));
	malformed_record = ExtensionRecord;
	malformed_record[4U] = 1U;
	manifest = valid_manifest(byte_view(malformed_record));
	EXPECT_EQ(ValidationError::BadRecordLength, validate_manifest_part_payload(manifest));

	std::vector<std::uint8_t> extra(ExtensionRecord.begin(), ExtensionRecord.end());
	extra.push_back(0U);
	manifest = valid_manifest(byte_view(extra));
	EXPECT_EQ(ValidationError::TrailingBytes, validate_manifest_part_payload(manifest));
	std::vector<std::uint8_t> two_records(ExtensionRecord.begin(), ExtensionRecord.end());
	two_records.insert(two_records.end(), ExtensionRecord.begin(), ExtensionRecord.end());
	manifest = valid_manifest(byte_view(two_records));
	manifest.record_count = 2U;
	manifest.transaction_size = static_cast<std::uint32_t>(two_records.size());
	EXPECT_EQ(ValidationError::None, validate_manifest_part_payload(manifest));
}

TEST(TelemetryProtocolStateMessages, DeltaEnforcesIdentityReservedBytesMutationFlagsAndMaximumSize)
{
	auto delta = valid_delta();
	delta.baseline_snapshot_id = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_delta_payload(delta));
	delta = valid_delta();
	delta.delta_sequence = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_delta_payload(delta));
	delta = valid_delta();
	delta.record_count = 0U;
	EXPECT_EQ(ValidationError::OutOfRange, validate_delta_payload(delta));

	auto record = ExtensionRecord;
	record[3U] = RecordFlagCreate;
	delta = valid_delta(byte_view(record));
	EXPECT_EQ(ValidationError::None, validate_delta_payload(delta));
	record[3U] = RecordFlagDelete;
	EXPECT_EQ(ValidationError::None, validate_delta_payload(valid_delta(byte_view(record))));
	record[3U] = RecordFlagCreate | RecordFlagDelete;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_delta_payload(valid_delta(byte_view(record))));
	record[3U] = RecordFlagPartial;
	EXPECT_EQ(ValidationError::ReservedFlag, validate_delta_payload(valid_delta(byte_view(record))));

	std::vector<std::uint8_t> encoded(DeltaPayloadPrefixSize + ExtensionRecord.size());
	std::size_t written = 0U;
	ASSERT_EQ(ValidationError::None, encode_delta_payload(valid_delta(), mutable_byte_view(encoded), written));
	encoded[18U] = 1U;
	DeltaPayload decoded;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_delta_payload(byte_view(encoded), decoded));
	encoded[18U] = 0U;
	encoded[19U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_delta_payload(byte_view(encoded), decoded));

	const std::uint8_t byte = 0U;
	delta = valid_delta(ByteView{&byte, MaxStateMessageSize - DeltaPayloadPrefixSize + 1U});
	EXPECT_EQ(ValidationError::MessageTooLarge, validate_delta_payload(delta));
	auto manifest = valid_manifest(ByteView{&byte, MaxStatePartSize - ManifestPartPayloadPrefixSize + 1U});
	manifest.transaction_size = static_cast<std::uint32_t>(MaxTransactionSize);
	EXPECT_EQ(ValidationError::MessageTooLarge, validate_manifest_part_payload(manifest));
}

TEST(TelemetryProtocolStateMessages, EncodersAreAtomicOnShortOutputAndValidationFailure)
{
	std::array<std::uint8_t, ManifestPartPayloadPrefixSize + ExtensionRecord.size() - 1U> manifest_output{};
	manifest_output.fill(0xa5U);
	const auto manifest_canary = manifest_output;
	std::size_t written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_manifest_part_payload(valid_manifest(), mutable_byte_view(manifest_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(manifest_canary, manifest_output);

	std::array<std::uint8_t, FullSnapshotPartPayloadPrefixSize + ExtensionRecord.size() - 1U> snapshot_output{};
	snapshot_output.fill(0xa5U);
	const auto snapshot_canary = snapshot_output;
	written = 99U;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_full_snapshot_part_payload(valid_snapshot(), mutable_byte_view(snapshot_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(snapshot_canary, snapshot_output);

	std::array<std::uint8_t, DeltaPayloadPrefixSize + ExtensionRecord.size()> delta_output{};
	delta_output.fill(0xa5U);
	const auto delta_canary = delta_output;
	auto invalid_delta = valid_delta();
	invalid_delta.delta_sequence = 0U;
	written = 99U;
	EXPECT_EQ(ValidationError::OutOfRange,
		encode_delta_payload(invalid_delta, mutable_byte_view(delta_output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(delta_canary, delta_output);
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_delta_payload(valid_delta(), MutableByteView{}, written));
	EXPECT_EQ(0U, written);
}

} // namespace
