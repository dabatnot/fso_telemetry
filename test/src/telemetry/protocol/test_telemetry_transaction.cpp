#include "telemetry/protocol/telemetry_transaction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes) {
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

std::vector<std::uint8_t> bytes(std::size_t size, std::uint8_t seed = 0x31U) {
	std::vector<std::uint8_t> result(size);
	for (std::size_t index = 0; index < size; ++index) {
		result[index] = static_cast<std::uint8_t>(seed + index * 37U);
	}
	return result;
}

Sha256Digest transaction_digest(const std::vector<std::vector<std::uint8_t>>& records) {
	Sha256 calculator;
	for (const auto& part : records) {
		EXPECT_TRUE(calculator.update(byte_view(part)));
	}
	Sha256Digest result{};
	EXPECT_TRUE(calculator.finalize(result));
	return result;
}

Sha256Digest transaction_digest(ByteView records) {
	Sha256Digest result{};
	EXPECT_TRUE(sha256(records, result));
	return result;
}

TransactionPart manifest_part(const std::vector<std::uint8_t>& records,
	                          const Sha256Digest& digest,
	                          std::uint16_t part_index = 0U,
	                          std::uint16_t part_count = 1U,
	                          std::uint32_t transaction_size = 0U,
	                          std::uint32_t message_id = 100U,
	                          std::uint32_t transaction_id = 7U) {
	TransactionPart result;
	result.session_id = 42U;
	result.message_type = MessageType::Manifest;
	result.transaction_id = transaction_id;
	result.message_id = message_id;
	result.part_index = part_index;
	result.part_count = part_count;
	result.transaction_size = transaction_size == 0U ? static_cast<std::uint32_t>(records.size()) : transaction_size;
	result.transaction_sha256 = digest;
	result.producer_sample_time_us = 123456U;
	result.kind_or_flags = static_cast<std::uint16_t>(ManifestKind::FullRequired);
	result.record_count = 1U;
	result.records = byte_view(records);
	return result;
}

TransactionPart snapshot_part(const std::vector<std::uint8_t>& records,
	                          const Sha256Digest& digest,
	                          std::uint16_t part_index = 0U,
	                          std::uint16_t part_count = 1U,
	                          std::uint32_t transaction_size = 0U,
	                          std::uint32_t message_id = 200U,
	                          std::uint32_t transaction_id = 9U) {
	auto result = manifest_part(records, digest, part_index, part_count, transaction_size, message_id, transaction_id);
	result.message_type = MessageType::FullSnapshot;
	result.frame_id = 77U;
	result.mission_time_us = -7654321;
	result.kind_or_flags = SnapshotFlagInitial;
	result.required_manifest_id = 6U;
	return result;
}

CompletedTransaction sentinel_transaction() {
	CompletedTransaction result;
	result.session_id = 0xfeedbeefU;
	result.message_type = MessageType::TargetVideoFrame;
	result.transaction_id = 0xf00dU;
	result.transaction_size = 1U;
	CompletedTransactionPart part;
	part.message_id = 999U;
	part.record_count = 7U;
	part.records = {0x5aU};
	result.parts.emplace_back(std::move(part));
	return result;
}

void expect_sentinel(const CompletedTransaction& completed) {
	EXPECT_EQ(0xfeedbeefU, completed.session_id);
	EXPECT_EQ(MessageType::TargetVideoFrame, completed.message_type);
	EXPECT_EQ(0xf00dU, completed.transaction_id);
	EXPECT_EQ(1U, completed.transaction_size);
	ASSERT_EQ(1U, completed.parts.size());
	EXPECT_EQ(999U, completed.parts[0].message_id);
	EXPECT_EQ(7U, completed.parts[0].record_count);
	EXPECT_EQ((std::vector<std::uint8_t>{0x5aU}), completed.parts[0].records);
}

std::vector<std::uint32_t> completed_message_ids(const CompletedTransaction& completed) {
	std::vector<std::uint32_t> result;
	for (const auto& part : completed.parts) {
		result.emplace_back(part.message_id);
	}
	return result;
}

std::vector<std::uint8_t> completed_records(const CompletedTransaction& completed) {
	std::vector<std::uint8_t> result;
	for (const auto& part : completed.parts) {
		result.insert(result.end(), part.records.begin(), part.records.end());
	}
	return result;
}

TEST(TelemetryProtocolTransaction, CompletesSinglePartManifestAtomically) {
	const auto records = bytes(37U);
	const auto digest = transaction_digest(byte_view(records));
	const auto part = manifest_part(records, digest);
	TelemetryTransactionAssembler assembler;
	auto completed = sentinel_transaction();

	ASSERT_EQ(TransactionAssemblyResult::Completed, assembler.ingest(part, 10U, completed));
	EXPECT_EQ(part.session_id, completed.session_id);
	EXPECT_EQ(MessageType::Manifest, completed.message_type);
	EXPECT_EQ(part.transaction_id, completed.transaction_id);
	EXPECT_EQ(records.size(), completed.transaction_size);
	EXPECT_EQ(digest, completed.transaction_sha256);
	EXPECT_EQ(part.producer_sample_time_us, completed.producer_sample_time_us);
	EXPECT_EQ(0U, completed.frame_id);
	EXPECT_EQ(0, completed.mission_time_us);
	EXPECT_EQ(part.kind_or_flags, completed.kind_or_flags);
	EXPECT_EQ(0U, completed.required_manifest_id);
	ASSERT_EQ(1U, completed.parts.size());
	EXPECT_EQ(part.message_id, completed.parts[0].message_id);
	EXPECT_EQ(part.record_count, completed.parts[0].record_count);
	EXPECT_EQ(records, completed.parts[0].records);
	EXPECT_EQ(records.size(), completed.parts[0].records_view().size);
	EXPECT_EQ(0U, assembler.active_candidates());
	EXPECT_EQ(0U, assembler.reserved_bytes());
}

TEST(TelemetryProtocolTransaction, AcceptsSnapshotPartsOutOfOrderAndPublishesIdsInPartOrder) {
	const std::vector<std::vector<std::uint8_t>> records{{0x01U, 0x02U}, {0x10U, 0x11U, 0x12U}, {0x20U}};
	const auto digest = transaction_digest(records);
	const auto total_size = static_cast<std::uint32_t>(6U);
	std::array<TransactionPart, 3> parts{{snapshot_part(records[0], digest, 0U, 3U, total_size, 300U),
	                                      snapshot_part(records[1], digest, 1U, 3U, total_size, 301U),
	                                      snapshot_part(records[2], digest, 2U, 3U, total_size, 302U)}};
	TelemetryTransactionAssembler assembler;
	auto completed = sentinel_transaction();

	EXPECT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(parts[2], 100U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(parts[0], 101U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(TransactionAssemblyResult::Completed, assembler.ingest(parts[1], 102U, completed));
	EXPECT_EQ(MessageType::FullSnapshot, completed.message_type);
	EXPECT_EQ(77U, completed.frame_id);
	EXPECT_EQ(-7654321, completed.mission_time_us);
	EXPECT_EQ(SnapshotFlagInitial, completed.kind_or_flags);
	EXPECT_EQ(6U, completed.required_manifest_id);
	EXPECT_EQ((std::vector<std::uint32_t>{300U, 301U, 302U}), completed_message_ids(completed));
	EXPECT_EQ((std::vector<std::uint8_t>{0x01U, 0x02U, 0x10U, 0x11U, 0x12U, 0x20U}),
	          completed_records(completed));
}

TEST(TelemetryProtocolTransaction, IdenticalDuplicateIsIdempotentAndLeavesOutputUntouched) {
	const std::vector<std::vector<std::uint8_t>> records{{0x01U}, {0x02U}};
	const auto digest = transaction_digest(records);
	const auto first = manifest_part(records[0], digest, 0U, 2U, 2U, 10U);
	const auto second = manifest_part(records[1], digest, 1U, 2U, 2U, 11U);
	TelemetryTransactionAssembler assembler;
	auto completed = sentinel_transaction();

	ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(first, 0U, completed));
	EXPECT_EQ(TransactionAssemblyResult::Duplicate, assembler.ingest(first, 1U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(1U, assembler.active_candidates());
	EXPECT_EQ(1U, assembler.reserved_bytes());
	EXPECT_EQ(TransactionAssemblyResult::Completed, assembler.ingest(second, 2U, completed));
	EXPECT_EQ((std::vector<std::uint8_t>{0x01U, 0x02U}), completed_records(completed));
}

TEST(TelemetryProtocolTransaction, ContradictoryDuplicateAndCrossIndexMessageIdPurgeCandidate) {
	const std::vector<std::vector<std::uint8_t>> records{{0x01U, 0x02U}, {0x03U}};
	const auto digest = transaction_digest(records);
	const auto first = manifest_part(records[0], digest, 0U, 2U, 3U, 100U);
	auto completed = sentinel_transaction();

	using Mutation = std::function<void(TransactionPart&, std::vector<std::uint8_t>&)>;
	const std::array<std::pair<const char*, Mutation>, 3> mutations{{
	    {"message id", [](TransactionPart& part, std::vector<std::uint8_t>&) { ++part.message_id; }},
	    {"record count", [](TransactionPart& part, std::vector<std::uint8_t>&) { ++part.record_count; }},
	    {"record bytes", [](TransactionPart& part, std::vector<std::uint8_t>& owned) {
		     owned[0] ^= 0x80U;
		     part.records = byte_view(owned);
	     }},
	}};
	for (const auto& item : mutations) {
		SCOPED_TRACE(item.first);
		TelemetryTransactionAssembler assembler;
		ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(first, 10U, completed));
		auto owned = records[0];
		auto contradictory = manifest_part(owned, digest, 0U, 2U, 3U, 100U);
		item.second(contradictory, owned);
		EXPECT_EQ(TransactionAssemblyResult::SemanticValidationFailed,
		          assembler.ingest(contradictory, 11U, completed));
		expect_sentinel(completed);
		EXPECT_EQ(0U, assembler.active_candidates());
		EXPECT_EQ(0U, assembler.reserved_bytes());
	}

	TelemetryTransactionAssembler assembler;
	ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(first, 20U, completed));
	const auto reused_message_id = manifest_part(records[1], digest, 1U, 2U, 3U, first.message_id);
	EXPECT_EQ(TransactionAssemblyResult::SemanticValidationFailed,
	          assembler.ingest(reused_message_id, 21U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, assembler.active_candidates());
}

TEST(TelemetryProtocolTransaction, ValidRepeatedMetadataContradictionsAndSameIdNewHashPurgeCandidate) {
	const std::vector<std::vector<std::uint8_t>> records{{0x01U}, {0x02U}};
	const auto digest = transaction_digest(records);
	// Keep spare declared size so a valid alternate part_count can be tested
	// as a repeated-metadata contradiction rather than an intrinsic error.
	const auto first = snapshot_part(records[0], digest, 0U, 2U, 4U, 100U);
	const auto consistent_second = snapshot_part(records[1], digest, 1U, 2U, 4U, 101U);
	auto completed = sentinel_transaction();

	using Mutation = std::function<void(TransactionPart&)>;
	const std::array<std::pair<const char*, Mutation>, 7> mutations{{
	    {"part count", [](TransactionPart& part) { part.part_count = 3U; }},
	    {"transaction size", [](TransactionPart& part) { part.transaction_size = 5U; }},
	    {"sample time", [](TransactionPart& part) { ++part.producer_sample_time_us; }},
	    {"frame id", [](TransactionPart& part) { ++part.frame_id; }},
	    {"mission time", [](TransactionPart& part) { ++part.mission_time_us; }},
	    {"snapshot kind", [](TransactionPart& part) { part.kind_or_flags = SnapshotFlagResync; }},
	    {"manifest dependency", [](TransactionPart& part) { ++part.required_manifest_id; }},
	}};
	for (const auto& item : mutations) {
		SCOPED_TRACE(item.first);
		TelemetryTransactionAssembler assembler;
		ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(first, 10U, completed));
		auto contradiction = consistent_second;
		item.second(contradiction);
		EXPECT_EQ(TransactionAssemblyResult::SemanticValidationFailed,
		          assembler.ingest(contradiction, 11U, completed));
		expect_sentinel(completed);
		EXPECT_EQ(0U, assembler.active_candidates());
		EXPECT_EQ(0U, assembler.reserved_bytes());
	}

	TelemetryTransactionAssembler assembler;
	ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(first, 20U, completed));
	auto changed_hash = consistent_second;
	changed_hash.transaction_sha256[0] ^= 0x01U;
	EXPECT_EQ(TransactionAssemblyResult::SemanticValidationFailed, assembler.ingest(changed_hash, 21U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, assembler.active_candidates());
}

TEST(TelemetryProtocolTransaction, RejectsInvalidIntrinsicFieldsBeforeReservationOrCopy) {
	const auto records = bytes(1U);
	const auto digest = transaction_digest(byte_view(records));
	const auto valid_manifest = manifest_part(records, digest);
	const auto valid_snapshot = snapshot_part(records, digest);
	auto completed = sentinel_transaction();
	const std::uint8_t byte = 0U;

	using Mutation = std::function<void(TransactionPart&)>;
	const std::array<std::pair<const char*, Mutation>, 20> invalid{{
	    {"type", [](TransactionPart& part) { part.message_type = MessageType::Delta; }},
	    {"session", [](TransactionPart& part) { part.session_id = 0U; }},
	    {"transaction id", [](TransactionPart& part) { part.transaction_id = 0U; }},
	    {"message id", [](TransactionPart& part) { part.message_id = 0U; }},
	    {"zero parts", [](TransactionPart& part) { part.part_count = 0U; }},
	    {"too many parts", [](TransactionPart& part) { part.part_count = static_cast<std::uint16_t>(MaxTransactionParts + 1U); }},
	    {"index", [](TransactionPart& part) { part.part_index = part.part_count; }},
	    {"zero size", [](TransactionPart& part) { part.transaction_size = 0U; }},
	    {"large size", [](TransactionPart& part) { part.transaction_size = static_cast<std::uint32_t>(MaxTransactionSize + 1U); }},
	    {"size below part count", [](TransactionPart& part) { part.part_count = 2U; }},
	    {"zero record count", [](TransactionPart& part) { part.record_count = 0U; }},
	    {"empty records", [](TransactionPart& part) { part.records = ByteView{}; }},
	    {"invalid view", [](TransactionPart& part) { part.records = ByteView{nullptr, 1U}; }},
	    {"records exceed transaction", [&byte](TransactionPart& part) { part.records = ByteView{&byte, 2U}; }},
	    {"records exceed part prefix", [&byte](TransactionPart& part) {
		     part.transaction_size = static_cast<std::uint32_t>(MaxStatePartSize);
		     part.records = ByteView{&byte, MaxStatePartSize - 56U + 1U};
	     }},
	    {"manifest kind", [](TransactionPart& part) { part.kind_or_flags = 2U; }},
	    {"manifest dependency", [](TransactionPart& part) { part.required_manifest_id = 1U; }},
	    {"manifest frame", [](TransactionPart& part) { part.frame_id = 1U; }},
	    {"manifest mission", [](TransactionPart& part) { part.mission_time_us = 1; }},
	    {"manifest combined invalids", [](TransactionPart& part) {
		     part.kind_or_flags = 0U;
		     part.frame_id = 1U;
	     }},
	}};
	for (const auto& item : invalid) {
		SCOPED_TRACE(item.first);
		TelemetryTransactionAssembler assembler;
		auto part = valid_manifest;
		item.second(part);
		EXPECT_EQ(TransactionAssemblyResult::InvalidPart, assembler.ingest(part, 1U, completed));
		expect_sentinel(completed);
		EXPECT_EQ(0U, assembler.active_candidates());
		EXPECT_EQ(0U, assembler.reserved_bytes());
	}

	const std::array<std::pair<const char*, Mutation>, 5> invalid_snapshots{{
	    {"no snapshot kind", [](TransactionPart& part) { part.kind_or_flags = SnapshotFlagNone; }},
	    {"multiple snapshot kinds", [](TransactionPart& part) { part.kind_or_flags = SnapshotFlagInitial | SnapshotFlagResync; }},
	    {"reserved snapshot flag", [](TransactionPart& part) { part.kind_or_flags = 0x8000U; }},
	    {"zero snapshot frame", [](TransactionPart& part) { part.frame_id = 0U; }},
	    {"snapshot records exceed part prefix", [&byte](TransactionPart& part) {
		     part.transaction_size = static_cast<std::uint32_t>(MaxStatePartSize);
		     part.records = ByteView{&byte, MaxStatePartSize - 60U + 1U};
	     }},
	}};
	for (const auto& item : invalid_snapshots) {
		SCOPED_TRACE(item.first);
		TelemetryTransactionAssembler assembler;
		auto part = valid_snapshot;
		item.second(part);
		EXPECT_EQ(TransactionAssemblyResult::InvalidPart, assembler.ingest(part, 1U, completed));
		expect_sentinel(completed);
		EXPECT_EQ(0U, assembler.active_candidates());
	}
}

TEST(TelemetryProtocolTransaction, SizeSumAndHashFailuresPurgeWithoutPublishing) {
	const std::vector<std::vector<std::uint8_t>> records{{0x01U}, {0x02U}};
	const auto digest = transaction_digest(records);
	auto completed = sentinel_transaction();

	TelemetryTransactionAssembler short_sum;
	auto first = manifest_part(records[0], digest, 0U, 2U, 3U, 10U);
	auto second = manifest_part(records[1], digest, 1U, 2U, 3U, 11U);
	ASSERT_EQ(TransactionAssemblyResult::Accepted, short_sum.ingest(first, 0U, completed));
	EXPECT_EQ(TransactionAssemblyResult::TransactionSizeMismatch, short_sum.ingest(second, 1U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, short_sum.active_candidates());

	TelemetryTransactionAssembler overflow;
	const std::vector<std::uint8_t> two_bytes{0x02U, 0x03U};
	const std::vector<std::vector<std::uint8_t>> overflowing_records{{0x01U}, two_bytes};
	const auto overflowing_digest = transaction_digest(overflowing_records);
	first = manifest_part(overflowing_records[0], overflowing_digest, 0U, 2U, 2U, 10U);
	second = manifest_part(overflowing_records[1], overflowing_digest, 1U, 2U, 2U, 11U);
	ASSERT_EQ(TransactionAssemblyResult::Accepted, overflow.ingest(first, 0U, completed));
	EXPECT_EQ(TransactionAssemblyResult::TransactionSizeMismatch, overflow.ingest(second, 1U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, overflow.active_candidates());
	EXPECT_EQ(0U, overflow.reserved_bytes());

	TelemetryTransactionAssembler wrong_hash;
	auto bad_digest = digest;
	bad_digest[0] ^= 1U;
	first = manifest_part(records[0], bad_digest, 0U, 2U, 2U, 10U);
	second = manifest_part(records[1], bad_digest, 1U, 2U, 2U, 11U);
	ASSERT_EQ(TransactionAssemblyResult::Accepted, wrong_hash.ingest(first, 0U, completed));
	EXPECT_EQ(TransactionAssemblyResult::TransactionHashMismatch, wrong_hash.ingest(second, 1U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(0U, wrong_hash.active_candidates());
	EXPECT_EQ(0U, wrong_hash.reserved_bytes());
}

TEST(TelemetryProtocolTransaction, SupportsSixtyFourPartsAndExactSixteenMiBTransactionLimit) {
	std::vector<std::vector<std::uint8_t>> small_parts(MaxTransactionParts);
	for (std::size_t index = 0; index < small_parts.size(); ++index) {
		small_parts[index] = {static_cast<std::uint8_t>(index)};
	}
	const auto small_digest = transaction_digest(small_parts);
	TelemetryTransactionAssembler many_parts;
	auto completed = sentinel_transaction();
	for (std::size_t reverse = small_parts.size(); reverse > 0; --reverse) {
		const auto index = reverse - 1U;
		const auto part = manifest_part(small_parts[index],
		                                small_digest,
		                                static_cast<std::uint16_t>(index),
		                                static_cast<std::uint16_t>(small_parts.size()),
		                                static_cast<std::uint32_t>(small_parts.size()),
		                                static_cast<std::uint32_t>(1000U + index));
		const auto expected = index == 0U ? TransactionAssemblyResult::Completed : TransactionAssemblyResult::Accepted;
		ASSERT_EQ(expected, many_parts.ingest(part, static_cast<std::uint64_t>(small_parts.size() - reverse), completed));
	}
	ASSERT_EQ(MaxTransactionParts, completed.parts.size());
	for (std::size_t index = 0; index < completed.parts.size(); ++index) {
		EXPECT_EQ(1000U + index, completed.parts[index].message_id);
		ASSERT_EQ(1U, completed.parts[index].records.size());
		EXPECT_EQ(static_cast<std::uint8_t>(index), completed.parts[index].records[0]);
	}

	const auto large_records = bytes(MaxTransactionSize, 0x17U);
	const auto large_digest = transaction_digest(byte_view(large_records));
	constexpr std::size_t MaxManifestRecordsPerPart = MaxStatePartSize - 56U;
	const auto large_part_count = static_cast<std::uint16_t>(
	    (large_records.size() + MaxManifestRecordsPerPart - 1U) / MaxManifestRecordsPerPart);
	ASSERT_LE(large_part_count, MaxTransactionParts);
	TelemetryTransactionAssembler exact_limit;
	completed = sentinel_transaction();
	for (std::uint16_t index = 0; index < large_part_count; ++index) {
		const auto offset = static_cast<std::size_t>(index) * MaxManifestRecordsPerPart;
		const auto count = std::min(MaxManifestRecordsPerPart, large_records.size() - offset);
		auto part = manifest_part(small_parts[0],
		                          large_digest,
		                          index,
		                          large_part_count,
		                          static_cast<std::uint32_t>(large_records.size()),
		                          2000U + index,
		                          99U);
		part.records = ByteView{large_records.data() + static_cast<std::ptrdiff_t>(offset), count};
		const auto expected = index + 1U == large_part_count ? TransactionAssemblyResult::Completed
		                                                     : TransactionAssemblyResult::Accepted;
		ASSERT_EQ(expected, exact_limit.ingest(part, index, completed));
	}
	EXPECT_EQ(large_records, completed_records(completed));
	EXPECT_EQ(0U, exact_limit.active_candidates());
	EXPECT_EQ(0U, exact_limit.reserved_bytes());

	TelemetryTransactionAssembler rejected;
	auto oversized = manifest_part(small_parts[0], large_digest);
	oversized.transaction_size = static_cast<std::uint32_t>(MaxTransactionSize + 1U);
	EXPECT_EQ(TransactionAssemblyResult::InvalidPart, rejected.ingest(oversized, 0U, completed));
	oversized = manifest_part(small_parts[0], large_digest);
	oversized.part_count = static_cast<std::uint16_t>(MaxTransactionParts + 1U);
	EXPECT_EQ(TransactionAssemblyResult::InvalidPart, rejected.ingest(oversized, 0U, completed));
}

TEST(TelemetryProtocolTransaction, EnforcesOneCandidatePerTypeAndKeepsQuotaStableAfterRefusal) {
	const std::vector<std::vector<std::uint8_t>> records{{0x01U}, {0x02U}};
	const auto digest = transaction_digest(records);
	const auto manifest = manifest_part(records[0], digest, 0U, 2U, 2U, 10U, 1U);
	const auto snapshot = snapshot_part(records[0], digest, 0U, 2U, 2U, 20U, 2U);
	TelemetryTransactionAssembler assembler;
	auto completed = sentinel_transaction();
	ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(manifest, 0U, completed));
	ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(snapshot, 0U, completed));
	EXPECT_EQ(MaxCandidateTransactionsPerClient, assembler.active_candidates());
	EXPECT_EQ(2U, assembler.reserved_bytes());

	auto newer_manifest = manifest;
	newer_manifest.transaction_id = 3U;
	EXPECT_EQ(TransactionAssemblyResult::CandidateBusy, assembler.ingest(newer_manifest, 1U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(MaxCandidateTransactionsPerClient, assembler.active_candidates());
	EXPECT_EQ(2U, assembler.reserved_bytes());

	auto malformed = manifest;
	malformed.records = ByteView{nullptr, 1U};
	EXPECT_EQ(TransactionAssemblyResult::InvalidPart, assembler.ingest(malformed, 1U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(MaxCandidateTransactionsPerClient, assembler.active_candidates());
	EXPECT_EQ(2U, assembler.reserved_bytes());
	EXPECT_EQ(33'554'432U, MaxCandidateTransactionBytesPerClient);

	ASSERT_TRUE(assembler.discard(MessageType::Manifest));
	EXPECT_FALSE(assembler.discard(MessageType::Manifest));
	EXPECT_EQ(1U, assembler.active_candidates());
	EXPECT_EQ(1U, assembler.reserved_bytes());
	EXPECT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(newer_manifest, 2U, completed));
	EXPECT_EQ(2U, assembler.active_candidates());
	EXPECT_EQ(2U, assembler.reserved_bytes());
	assembler.clear();
	EXPECT_EQ(0U, assembler.active_candidates());
	EXPECT_EQ(0U, assembler.reserved_bytes());
}

TEST(TelemetryProtocolTransaction, ExpiresAtExactlyTenSecondsWithoutClockRegressionExtension) {
	const std::vector<std::vector<std::uint8_t>> records{{0x01U}, {0x02U}};
	const auto digest = transaction_digest(records);
	const auto first = manifest_part(records[0], digest, 0U, 2U, 2U, 10U, 1U);
	TelemetryTransactionAssembler assembler;
	auto completed = sentinel_transaction();
	ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(first, 5000U, completed));
	EXPECT_EQ(0U, assembler.expire(4000U));
	EXPECT_EQ(0U, assembler.expire(14999U));
	EXPECT_EQ(TransactionAssemblyResult::Duplicate, assembler.ingest(first, 14999U, completed));
	expect_sentinel(completed);
	EXPECT_EQ(1U, assembler.expire(15000U));
	EXPECT_EQ(0U, assembler.active_candidates());
	EXPECT_EQ(0U, assembler.reserved_bytes());

	auto replacement = first;
	replacement.transaction_id = 2U;
	EXPECT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(replacement, 15000U, completed));
	EXPECT_EQ(1U, assembler.active_candidates());
	assembler.clear();

	TelemetryTransactionAssembler expired_by_ingest;
	ASSERT_EQ(TransactionAssemblyResult::Accepted, expired_by_ingest.ingest(first, 5000U, completed));
	const auto late_same_transaction = expired_by_ingest.ingest(first, 15000U, completed);
	EXPECT_EQ(TransactionAssemblyResult::StaleTransaction, late_same_transaction.result);
	EXPECT_EQ(1U, late_same_transaction.expiration.count);
	EXPECT_TRUE(late_same_transaction.expiration.manifest_expired);
	EXPECT_EQ(1U, late_same_transaction.expiration.manifest_id);
	EXPECT_EQ(0U, expired_by_ingest.active_candidates());
	EXPECT_EQ(0U, expired_by_ingest.reserved_bytes());

	const auto repeated_late_part = expired_by_ingest.ingest(first, 15001U, completed);
	EXPECT_EQ(TransactionAssemblyResult::StaleTransaction, repeated_late_part.result);
	EXPECT_EQ(0U, repeated_late_part.expiration.count);
	EXPECT_EQ(TransactionAssemblyResult::Accepted, expired_by_ingest.ingest(replacement, 15002U, completed));
	EXPECT_EQ(1U, expired_by_ingest.active_candidates());
}

} // namespace
