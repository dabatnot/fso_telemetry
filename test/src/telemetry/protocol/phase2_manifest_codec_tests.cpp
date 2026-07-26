#include <gtest/gtest.h>

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/protocol/telemetry_transaction.h"
#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase2_wp03_test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace telemetry;
using namespace telemetry::protocol;

enum class PartMutation {
	Sha256,
	TransactionSize,
	PartCount,
	ManifestGeneration,
	PartCount65,
	TransactionSize16777217,
	ReservedManifestKind,
	TrailingGarbage,
};

void mutate_part(ManifestPartPayload& payload, PartMutation mutation)
{
	switch (mutation) {
	case PartMutation::Sha256:
		payload.transaction_sha256[0] ^= 1;
		break;
	case PartMutation::TransactionSize:
		++payload.transaction_size;
		break;
	case PartMutation::PartCount:
		++payload.part_count;
		break;
	case PartMutation::ManifestGeneration:
		++payload.manifest_id;
		break;
	case PartMutation::PartCount65:
		payload.part_count = 65;
		break;
	case PartMutation::TransactionSize16777217:
		payload.transaction_size = 16777217;
		break;
	case PartMutation::ReservedManifestKind:
		payload.manifest_kind = static_cast<ManifestKind>(0xffff);
		break;
	case PartMutation::TrailingGarbage:
		payload.record_count = 0;
		break;
	}
}

std::uint32_t read_u32_le(const std::uint8_t* bytes)
{
	return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
		(static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::pair<std::uint32_t, std::string> decode_manifest_identity(const RecordEnvelopeView& envelope)
{
	EXPECT_GE(envelope.payload.size, 18u);
	if (envelope.payload.size < 18) return {};
	const auto id = read_u32_le(envelope.payload.data + 4);
	const auto name_size = static_cast<std::size_t>(envelope.payload.data[16]) |
		(static_cast<std::size_t>(envelope.payload.data[17]) << 8);
	EXPECT_LE(18u + name_size, envelope.payload.size);
	if (18u + name_size > envelope.payload.size) return {};
	return {id, std::string(reinterpret_cast<const char*>(envelope.payload.data + 18), name_size)};
}

std::vector<std::uint8_t> extension_record(std::size_t payload_size, std::uint8_t fill)
{
	EXPECT_LE(payload_size, 65535u);
	std::vector<std::uint8_t> result;
	result.reserve(RecordEnvelopeHeaderSize + payload_size);
	result.push_back(0xfe);
	result.push_back(0x7f);
	result.push_back(1);
	result.push_back(0);
	result.push_back(static_cast<std::uint8_t>(payload_size));
	result.push_back(static_cast<std::uint8_t>(payload_size >> 8));
	result.insert(result.end(), payload_size, fill);
	return result;
}

std::vector<std::uint8_t> exact_quarter_mib_part(std::uint8_t fill)
{
	std::vector<std::uint8_t> result;
	for (const auto payload_size : {65535u, 65535u, 65535u, 65515u}) {
		const auto record = extension_record(payload_size, fill);
		result.insert(result.end(), record.begin(), record.end());
	}
	EXPECT_EQ(MaxTransactionSize / MaxTransactionParts, result.size());
	return result;
}

struct ProvisionedManifest {
	struct State {
		std::vector<std::uint8_t> arena = std::vector<std::uint8_t>(2U * MaxTransactionSize);
		Phase2ManifestStorage storage{MutableByteView{arena.data(), arena.size()}};
		Phase2ManifestSlot slot{storage};
	};
	std::unique_ptr<State> state = std::make_unique<State>();
	Phase2ManifestSlot& slot = state->slot;

	explicit ProvisionedManifest(bool initialize = true,
		test::wp03::SourceCase source_case = test::wp03::SourceCase::MultipartFullRequired)
	{
		if (initialize) {
			auto source = test::wp03::make_source(source_case);
			EXPECT_EQ(Phase2ManifestError::None, slot.rebuild(*source));
		}
	}

	const Phase2ManifestCandidate& candidate() const { return slot.staged_candidate(); }
};

TransactionPart transaction_part(const ManifestPartPayload& payload, std::uint32_t message_id)
{
	TransactionPart part{};
	part.session_id = 55;
	part.message_type = MessageType::Manifest;
	part.transaction_id = payload.manifest_id;
	part.message_id = message_id;
	part.part_index = payload.part_index;
	part.part_count = payload.part_count;
	part.transaction_size = payload.transaction_size;
	part.transaction_sha256 = payload.transaction_sha256;
	part.producer_sample_time_us = payload.producer_sample_time_us;
	part.kind_or_flags = static_cast<std::uint16_t>(payload.manifest_kind);
	part.record_count = payload.record_count;
	part.records = payload.records;
	return part;
}

void expect_payload_equal(const ManifestPartPayload& expected, const ManifestPartPayload& actual)
{
	EXPECT_EQ(expected.manifest_id, actual.manifest_id);
	EXPECT_EQ(expected.part_index, actual.part_index);
	EXPECT_EQ(expected.part_count, actual.part_count);
	EXPECT_EQ(expected.transaction_size, actual.transaction_size);
	EXPECT_EQ(expected.transaction_sha256, actual.transaction_sha256);
	EXPECT_EQ(expected.producer_sample_time_us, actual.producer_sample_time_us);
	EXPECT_EQ(expected.manifest_kind, actual.manifest_kind);
	EXPECT_EQ(expected.record_count, actual.record_count);
	ASSERT_EQ(expected.records.size, actual.records.size);
	EXPECT_TRUE(std::equal(expected.records.begin(), expected.records.end(), actual.records.begin()));
}

void expect_completed_sentinel(const CompletedTransaction& completed)
{
	EXPECT_EQ(0xfeedu, completed.transaction_id);
	EXPECT_EQ(0xbeefu, completed.transaction_size);
}

CompletedTransaction sentinel()
{
	CompletedTransaction value{};
	value.transaction_id = 0xfeed;
	value.transaction_size = 0xbeef;
	return value;
}

TEST(Phase2ManifestCodec, P2TST023MultipartPayloadRoundTripsThroughThePhase0Codec)
{
	ProvisionedManifest manifest;
	const auto& candidate = manifest.candidate();
	ASSERT_GE(candidate.part_count, 3u) << "Fixture must genuinely exercise parts[1] and reordering.";
	for (std::uint16_t i = 0; i < candidate.part_count; ++i) {
		const auto& original = candidate.parts[i];
		std::vector<std::uint8_t> encoded(ManifestPartPayloadPrefixSize + original.records.size);
		std::size_t written = 99;
		ASSERT_EQ(ValidationError::None,
			encode_manifest_part_payload(original, MutableByteView{encoded.data(), encoded.size()}, written));
		ASSERT_EQ(encoded.size(), written);
		ManifestPartPayload decoded{};
		ASSERT_EQ(ValidationError::None,
			decode_manifest_part_payload(ByteView{encoded.data(), encoded.size()}, decoded));
		expect_payload_equal(original, decoded);
	}
}

TEST(Phase2ManifestCodec, P2TST023AcceptsExactly65535RecordEnvelopesInOnePart)
{
	const auto empty = extension_record(0, 0);
	std::vector<std::uint8_t> records;
	records.reserve(65535u * empty.size());
	for (std::uint32_t index = 0; index < 65535; ++index) {
		records.insert(records.end(), empty.begin(), empty.end());
	}
	Sha256Digest digest{};
	ASSERT_TRUE(sha256(ByteView{records.data(), records.size()}, digest));
	ManifestPartPayload payload{};
	payload.manifest_id = 1;
	payload.part_count = 1;
	payload.transaction_size = static_cast<std::uint32_t>(records.size());
	payload.transaction_sha256 = digest;
	payload.manifest_kind = ManifestKind::FullRequired;
	payload.record_count = 65535;
	payload.records = ByteView{records.data(), records.size()};
	std::vector<std::uint8_t> encoded(ManifestPartPayloadPrefixSize + records.size());
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None,
		encode_manifest_part_payload(payload, MutableByteView{encoded.data(), encoded.size()}, written));
	EXPECT_EQ(encoded.size(), written);
}

TEST(Phase2ManifestCodec, P2TST024LossDuplicateReorderAndRetransmitCommitOnlyOnce)
{
	ProvisionedManifest manifest;
	const auto& candidate = manifest.candidate();
	ASSERT_GE(candidate.part_count, 3u);
	TelemetryTransactionAssembler assembler{};
	auto completed = sentinel();

	for (std::uint16_t reverse = candidate.part_count; reverse > 1; --reverse) {
		const auto index = static_cast<std::uint16_t>(reverse - 1);
		ASSERT_EQ(TransactionAssemblyResult::Accepted,
			assembler.ingest(transaction_part(candidate.parts[index], 100 + index), reverse, completed));
		expect_completed_sentinel(completed);
	}
	ASSERT_EQ(TransactionAssemblyResult::Duplicate,
		assembler.ingest(transaction_part(candidate.parts[1], 101), 50, completed));
	expect_completed_sentinel(completed);

	ASSERT_EQ(TransactionAssemblyResult::Completed,
		assembler.ingest(transaction_part(candidate.parts[0], 100), 51, completed));
	EXPECT_EQ(candidate.manifest_id, completed.transaction_id);
	EXPECT_EQ(candidate.encoded_size, completed.transaction_size);
	EXPECT_EQ(candidate.transaction_sha256, completed.transaction_sha256);
	EXPECT_EQ(candidate.part_count, completed.parts.size());
	EXPECT_EQ(0u, assembler.active_candidates());
}

TEST(Phase2ManifestCodec, P2TST025IncoherentShaSizeCountAndManifestGenerationNeverExposeCatalogs)
{
	ProvisionedManifest manifest;
	const auto& candidate = manifest.candidate();
	ASSERT_GE(candidate.part_count, 3u);
	for (const auto mutation : {PartMutation::Sha256,
		     PartMutation::TransactionSize,
		     PartMutation::PartCount,
		     PartMutation::ManifestGeneration}) {
		TelemetryTransactionAssembler assembler{};
		auto completed = sentinel();
		for (std::uint16_t index = 0; index < candidate.part_count; ++index) {
			auto payload = candidate.parts[index];
			if (index + 1 == candidate.part_count) {
				mutate_part(payload, mutation);
			}
			const auto outcome = assembler.ingest(transaction_part(payload, 200 + index), index, completed);
			if (index + 1 < candidate.part_count) {
				ASSERT_EQ(TransactionAssemblyResult::Accepted, outcome);
			} else {
				EXPECT_NE(TransactionAssemblyResult::Completed, outcome.result);
			}
		}
		expect_completed_sentinel(completed);
		EXPECT_FALSE(manifest.slot.catalog_visible(candidate.manifest_id));
	}
}

TEST(Phase2ManifestCodec, P2REQ015ClientValidationRejectsMissingDuplicateAndExtraCatalogRecordsAtomically)
{
	for (const auto mutation : {test::wp03::CatalogMutation::MissingClass,
		     test::wp03::CatalogMutation::MissingWeapon,
		     test::wp03::CatalogMutation::DuplicateClass,
		     test::wp03::CatalogMutation::DuplicateWeapon,
		     test::wp03::CatalogMutation::UnauthorizedExtraClass,
		     test::wp03::CatalogMutation::UnauthorizedExtraWeapon}) {
		ProvisionedManifest manifest;
		auto transaction = test::wp03::mutate_completed_catalog(manifest.candidate(), mutation);
		const auto staged_id = manifest.slot.staged_manifest_id();
		const auto staged_hash = manifest.slot.staged_candidate().transaction_sha256;
		EXPECT_EQ(Phase2ManifestError::InvalidFullRequiredCatalog,
			manifest.slot.validate_and_install(transaction));
		EXPECT_EQ(staged_id, manifest.slot.staged_manifest_id());
		EXPECT_EQ(staged_hash, manifest.slot.staged_candidate().transaction_sha256);
	}
}

TEST(Phase2ManifestCodec, P2REQ015LeastPrivilegeIsProvedFromDecodedWireRecords)
{
	ProvisionedManifest manifest{true, test::wp03::SourceCase::TwoClassesThreeWeaponsWithDecoys};
	std::size_t class_records = 0;
	std::size_t weapon_records = 0;
	std::vector<std::pair<std::uint32_t, std::string>> classes;
	std::vector<std::pair<std::uint32_t, std::string>> weapons;
	for (std::uint16_t part_index = 0; part_index < manifest.candidate().part_count; ++part_index) {
		const auto& part = manifest.candidate().parts[part_index];
		RecordEnvelopeIterator iterator(part.records, part.record_count, RecordFlagPolicy::RequireNone);
		for (;;) {
			RecordEnvelopeView envelope{};
			bool has_value = false;
			ASSERT_EQ(ValidationError::None, iterator.next(envelope, has_value));
			if (!has_value) break;
			if (envelope.raw_record_type == static_cast<std::uint16_t>(RecordType::ClassManifest)) {
				++class_records;
				classes.push_back(decode_manifest_identity(envelope));
			}
			if (envelope.raw_record_type == static_cast<std::uint16_t>(RecordType::WeaponManifest)) {
				++weapon_records;
				weapons.push_back(decode_manifest_identity(envelope));
			}
		}
	}
	EXPECT_EQ(2u, class_records);
	EXPECT_EQ(3u, weapon_records);
	std::sort(classes.begin(), classes.end());
	std::sort(weapons.begin(), weapons.end());
	EXPECT_EQ((std::vector<std::pair<std::uint32_t, std::string>>{{1, "Apollo"}, {2, "Ulysses"}}), classes);
	EXPECT_EQ((std::vector<std::pair<std::uint32_t, std::string>>{{1, "Alpha"}, {2, "Beta"}, {3, "Gamma"}}),
		weapons);
}

TEST(Phase2ManifestCodec, P2TST028AcceptsExactSixteenMiBAndSixtyFourPartsThenRejectsEachPlusOne)
{
	std::vector<std::vector<std::uint8_t>> records;
	records.reserve(MaxTransactionParts);
	std::vector<std::uint8_t> transaction_bytes;
	transaction_bytes.reserve(MaxTransactionSize);
	for (std::size_t index = 0; index < MaxTransactionParts; ++index) {
		records.push_back(exact_quarter_mib_part(static_cast<std::uint8_t>(index)));
		transaction_bytes.insert(transaction_bytes.end(), records.back().begin(), records.back().end());
	}
	ASSERT_EQ(MaxTransactionSize, transaction_bytes.size());
	Sha256Digest digest{};
	ASSERT_TRUE(sha256(ByteView{transaction_bytes.data(), transaction_bytes.size()}, digest));
	TelemetryTransactionAssembler assembler{};
	auto completed = sentinel();
	for (std::uint16_t index = 0; index < MaxTransactionParts; ++index) {
		TransactionPart part{};
		part.session_id = 55;
		part.message_type = MessageType::Manifest;
		part.transaction_id = 7;
		part.message_id = 300 + index;
		part.part_index = index;
		part.part_count = static_cast<std::uint16_t>(MaxTransactionParts);
		part.transaction_size = static_cast<std::uint32_t>(MaxTransactionSize);
		part.transaction_sha256 = digest;
		part.kind_or_flags = static_cast<std::uint16_t>(ManifestKind::FullRequired);
		part.record_count = 4;
		part.records = ByteView{records[index].data(), records[index].size()};
		const auto expected = index + 1 == MaxTransactionParts ? TransactionAssemblyResult::Completed
		                                                      : TransactionAssemblyResult::Accepted;
		ASSERT_EQ(expected, assembler.ingest(part, index, completed));
	}
	EXPECT_EQ(MaxTransactionSize, completed.transaction_size);
	EXPECT_EQ(MaxTransactionParts, completed.parts.size());

	TelemetryTransactionAssembler rejected{};
	TransactionPart oversized{};
	oversized.session_id = 55;
	oversized.message_type = MessageType::Manifest;
	oversized.transaction_id = 8;
	oversized.part_count = 1;
	oversized.transaction_size = static_cast<std::uint32_t>(MaxTransactionSize + 1);
	oversized.transaction_sha256 = digest;
	oversized.kind_or_flags = static_cast<std::uint16_t>(ManifestKind::FullRequired);
	oversized.record_count = 4;
	oversized.records = ByteView{records[0].data(), records[0].size()};
	completed = sentinel();
	EXPECT_EQ(TransactionAssemblyResult::InvalidPart, rejected.ingest(oversized, 0, completed));
	expect_completed_sentinel(completed);
	oversized.transaction_size = static_cast<std::uint32_t>(MaxTransactionSize);
	oversized.part_count = static_cast<std::uint16_t>(MaxTransactionParts + 1);
	EXPECT_EQ(TransactionAssemblyResult::InvalidPart, rejected.ingest(oversized, 1, completed));
	expect_completed_sentinel(completed);
}

TEST(Phase2ManifestCodec, P2AC005ManifestAppliedPrecedesSnapshotAndPromotionWaitsForSnapshotApplied)
{
	ProvisionedManifest manifest;
	const auto id = manifest.candidate().manifest_id;
	EXPECT_FALSE(manifest.slot.can_publish_snapshot_requiring(id));
	ASSERT_EQ(Phase2ManifestError::None, manifest.slot.on_manifest_applied(id));
	EXPECT_TRUE(manifest.slot.can_publish_keyframe_requiring(id));
	EXPECT_FALSE(manifest.slot.is_active(id));
	EXPECT_TRUE(manifest.slot.is_staged(id));
	ASSERT_EQ(Phase2ManifestError::None, manifest.slot.on_dependent_snapshot_applied(777, id));
	EXPECT_TRUE(manifest.slot.is_active(id));
	EXPECT_FALSE(manifest.slot.is_staged(id));
}

TEST(Phase2ManifestCodec, P2REQ018DeltaCannotChangeRequiredManifestId)
{
	ProvisionedManifest manifest;
	const auto id = manifest.candidate().manifest_id;
	ASSERT_EQ(Phase2ManifestError::None, manifest.slot.on_manifest_applied(id));
	EXPECT_EQ(Phase2ManifestError::ManifestIdChangeRequiresKeyframe,
		manifest.slot.validate_delta_required_manifest_id(id + 1));
	EXPECT_EQ(id, manifest.slot.required_manifest_id());
}

TEST(Phase2ManifestCodec, P2REQ050HostileTruncationCorpusFailsClosedWithoutPartialOutput)
{
	ProvisionedManifest manifest;
	const auto& payload = manifest.candidate().parts[0];
	std::vector<std::uint8_t> encoded(ManifestPartPayloadPrefixSize + payload.records.size);
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None,
		encode_manifest_part_payload(payload, MutableByteView{encoded.data(), encoded.size()}, written));
	ASSERT_EQ(encoded.size(), written);

	for (std::size_t cut = 0; cut < encoded.size(); ++cut) {
		ManifestPartPayload decoded{};
		decoded.manifest_id = 0xfeed;
		decoded.part_count = 9;
		decoded.records = ByteView{encoded.data(), encoded.size()};
		const auto result = decode_manifest_part_payload(ByteView{encoded.data(), cut}, decoded);
		EXPECT_NE(ValidationError::None, result) << "cut=" << cut;
		EXPECT_EQ(0u, decoded.manifest_id);
		EXPECT_EQ(0u, decoded.part_count);
		EXPECT_TRUE(decoded.records.empty());
	}
}

TEST(Phase2ManifestCodec, P2REQ050HostileLengthsAndReservedValuesAreRejectedBeforeEgress)
{
	ProvisionedManifest manifest;
	const auto base = manifest.candidate().parts[0];
	for (const auto mutation : {PartMutation::PartCount65,
		     PartMutation::TransactionSize16777217,
		     PartMutation::ReservedManifestKind,
		     PartMutation::TrailingGarbage}) {
		auto hostile = base;
		mutate_part(hostile, mutation);
		std::vector<std::uint8_t> output(MaxStatePartSize);
		std::size_t written = 123;
		EXPECT_NE(ValidationError::None,
			encode_manifest_part_payload(hostile, MutableByteView{output.data(), output.size()}, written));
		EXPECT_EQ(0u, written);
	}
}

} // namespace
