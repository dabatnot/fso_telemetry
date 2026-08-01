#include <gtest/gtest.h>

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/protocol/telemetry_transaction.h"
#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/phase2_manifest_test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
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

struct DecodedWeaponManifest {
	std::uint32_t generation = 0;
	std::uint32_t id = 0;
	std::uint64_t presence = 0;
	std::string name;
	std::string title;
	std::uint8_t subtype = 0;
	std::uint64_t class_flags = 0;
	float max_speed = 0.0F;
	std::uint64_t acceleration_time_us = 0;
	float mass = 0.0F;
	float gravity_constant = 0.0F;
	std::uint64_t lifetime_us = 0;
	float minimum_range = 0.0F;
	float optimal_range = 0.0F;
	float maximum_range = 0.0F;
	std::uint64_t fire_wait_us = 0;
	float energy_consumed = 0.0F;
	float damage = 0.0F;
	std::uint32_t damage_type_id = 0;
	std::uint32_t effect_flags = 0;
	std::uint8_t guidance_type = 0;
	float guidance_fov_rad = 0.0F;
	std::uint64_t lock_time_us = 0;
	float lock_fov_rad = 0.0F;
	float velocity_inherit_amount = 0.0F;
	float cargo_size = 0.0F;
	std::uint64_t rearm_time_us = 0;
	std::uint32_t reloaded_per_batch = 0;
	std::uint16_t burst_count = 0;
	std::uint64_t burst_interval_us = 0;
	std::uint16_t swarm_count = 0;
	std::uint16_t shots_per_trigger = 0;
};

bool decode_weapon_manifest(const RecordEnvelopeView& envelope, DecodedWeaponManifest& value)
{
	if (envelope.raw_record_type != static_cast<std::uint16_t>(RecordType::WeaponManifest)) return false;
	PacketReader reader(envelope.payload);
	std::string_view name;
	std::string_view title;
	if (!reader.read_u32(value.generation) || !reader.read_u32(value.id) ||
		!reader.read_u64(value.presence) || !reader.read_utf8(255, name)) {
		return false;
	}
	value.name.assign(name);
	if ((value.presence & WeaponManifestPresenceFlagTitle) != 0U) {
		if (!reader.read_utf8(255, title)) return false;
		value.title.assign(title);
	}
	if (!reader.read_u8(value.subtype) || !reader.read_u64(value.class_flags) ||
		!reader.read_f32(value.max_speed)) {
		return false;
	}
	if ((value.presence & WeaponManifestPresenceFlagAcceleration) != 0U &&
		!reader.read_u64(value.acceleration_time_us)) {
		return false;
	}
	if (!reader.read_f32(value.mass) || !reader.read_f32(value.gravity_constant) ||
		!reader.read_u64(value.lifetime_us)) {
		return false;
	}
	if ((value.presence & WeaponManifestPresenceFlagRanges) != 0U &&
		(!reader.read_f32(value.minimum_range) || !reader.read_f32(value.optimal_range) ||
			!reader.read_f32(value.maximum_range))) {
		return false;
	}
	if ((value.presence & WeaponManifestPresenceFlagFire) != 0U &&
		(!reader.read_u64(value.fire_wait_us) || !reader.read_f32(value.energy_consumed))) {
		return false;
	}
	if ((value.presence & WeaponManifestPresenceFlagDamage) != 0U &&
		(!reader.read_f32(value.damage) || !reader.read_u32(value.damage_type_id) ||
			!reader.read_u32(value.effect_flags))) {
		return false;
	}
	if ((value.presence & WeaponManifestPresenceFlagGuidance) != 0U &&
		(!reader.read_u8(value.guidance_type) || !reader.read_f32(value.guidance_fov_rad))) {
		return false;
	}
	if ((value.presence & WeaponManifestPresenceFlagLock) != 0U &&
		(!reader.read_u64(value.lock_time_us) || !reader.read_f32(value.lock_fov_rad))) {
		return false;
	}
	if (!reader.read_f32(value.velocity_inherit_amount)) return false;
	if ((value.presence & WeaponManifestPresenceFlagCargoRearm) != 0U &&
		(!reader.read_f32(value.cargo_size) || !reader.read_u64(value.rearm_time_us) ||
			!reader.read_u32(value.reloaded_per_batch))) {
		return false;
	}
	if ((value.presence & WeaponManifestPresenceFlagBurst) != 0U &&
		(!reader.read_u16(value.burst_count) || !reader.read_u64(value.burst_interval_us))) {
		return false;
	}
	if ((value.presence & WeaponManifestPresenceFlagSwarm) != 0U &&
		(!reader.read_u16(value.swarm_count) || !reader.read_u16(value.shots_per_trigger))) {
		return false;
	}
	return reader.at_end();
}

struct DecodedClassManifestSubset {
	std::uint32_t generation = 0;
	std::uint32_t id = 0;
	std::uint64_t presence = 0;
	std::string name;
	std::uint32_t species_id = 0;
	std::uint32_t ship_type_id = 0;
	float mass = 0.0F;
	std::array<float, 3> center_of_mass{};
	std::array<float, 9> inertia{};
	std::array<float, 19> motion{};
	float hull = 0.0F;
	float shield = 0.0F;
	std::vector<std::uint32_t> subsystem_armor_ids;
};

bool decode_class_manifest_subset(const RecordEnvelopeView& envelope, DecodedClassManifestSubset& value)
{
	if (envelope.raw_record_type != static_cast<std::uint16_t>(RecordType::ClassManifest)) return false;
	PacketReader reader(envelope.payload);
	std::string_view name;
	if (!reader.read_u32(value.generation) || !reader.read_u32(value.id) ||
		!reader.read_u64(value.presence) || !reader.read_utf8(255, name) ||
		!reader.read_u32(value.species_id) || !reader.read_u32(value.ship_type_id) ||
		!reader.read_f32(value.mass)) {
		return false;
	}
	value.name.assign(name);
	for (auto& component : value.center_of_mass) {
		if (!reader.read_f32(component)) return false;
	}
	if ((value.presence & ClassManifestPresenceFlagInertia) != 0U) {
		for (auto& component : value.inertia) {
			if (!reader.read_f32(component)) return false;
		}
	}
	if ((value.presence & ClassManifestPresenceFlagDamping) != 0U && !reader.skip(5 * sizeof(float))) return false;
	if ((value.presence & ClassManifestPresenceFlagMotion) != 0U) {
		for (auto& component : value.motion) {
			if (!reader.read_f32(component)) return false;
		}
	}
	if ((value.presence & ClassManifestPresenceFlagHull) != 0U && !reader.read_f32(value.hull)) return false;
	if ((value.presence & ClassManifestPresenceFlagShield) != 0U && !reader.read_f32(value.shield)) return false;
	if ((value.presence & ClassManifestPresenceFlagEnergy) != 0U && !reader.skip(5 * sizeof(float))) return false;
	if ((value.presence & ClassManifestPresenceFlagAfterburner) != 0U &&
		!reader.skip(4 * sizeof(float) + sizeof(std::uint64_t))) {
		return false;
	}
	if ((value.presence & ClassManifestPresenceFlagCountermeasure) != 0U &&
		!reader.skip(2 * sizeof(std::uint32_t) + sizeof(std::uint64_t))) {
		return false;
	}
	if ((value.presence & ClassManifestPresenceFlagBanks) != 0U) {
		std::uint16_t count = 0;
		if (!reader.read_u16(count)) return false;
		for (std::uint16_t index = 0; index < count; ++index) {
			std::uint8_t version = 0;
			std::uint16_t item_size = 0;
			if (!reader.read_u8(version) || version != 1 || !reader.read_u16(item_size) ||
				!reader.skip(item_size)) {
				return false;
			}
		}
	}
	if ((value.presence & ClassManifestPresenceFlagSubsystems) != 0U) {
		std::uint16_t count = 0;
		if (!reader.read_u16(count)) return false;
		for (std::uint16_t index = 0; index < count; ++index) {
			std::uint8_t version = 0;
			std::uint16_t item_size = 0;
			PacketReader item;
			std::uint16_t item_presence = 0;
			std::uint32_t subsystem_id = 0;
			std::uint16_t canonical_index = 0;
			std::uint8_t type = 0;
			std::uint8_t reserved = 0;
			std::string_view subsystem_name;
			if (!reader.read_u8(version) || version != 1 || !reader.read_u16(item_size) ||
				!reader.subreader(item_size, item) || !item.read_u16(item_presence) ||
				!item.read_u32(subsystem_id) || !item.read_u16(canonical_index) ||
				!item.read_u8(type) || !item.read_u8(reserved) ||
				!item.read_utf8(255, subsystem_name)) {
				return false;
			}
			std::string_view optional_name;
			if ((item_presence & ClassSubsystemPresenceFlagAltName) != 0U &&
				!item.read_utf8(255, optional_name)) {
				return false;
			}
			if ((item_presence & ClassSubsystemPresenceFlagHudName) != 0U &&
				!item.read_utf8(255, optional_name)) {
				return false;
			}
			float component = 0.0F;
			for (std::uint8_t field = 0; field < 5; ++field) {
				if (!item.read_f32(component)) return false;
			}
			std::uint32_t armor_id = 0;
			if ((item_presence & ClassSubsystemPresenceFlagArmor) != 0U && !item.read_u32(armor_id)) {
				return false;
			}
			std::uint32_t static_flags = 0;
			if (!item.read_u32(static_flags) || !item.at_end()) return false;
			value.subsystem_armor_ids.push_back(armor_id);
		}
	}
	if ((value.presence & ClassManifestPresenceFlagScan) != 0U &&
		!reader.skip(sizeof(std::uint64_t) + 2 * sizeof(float))) {
		return false;
	}
	if ((value.presence & ClassManifestPresenceFlagGlide) != 0U && !reader.skip(sizeof(float))) return false;
	if ((value.presence & ClassManifestPresenceFlagAutoaim) != 0U && !reader.skip(sizeof(float))) return false;
	if ((value.presence & ClassManifestPresenceFlagRadarIcon) != 0U &&
		!reader.skip(sizeof(std::uint32_t))) {
		return false;
	}
	return reader.at_end();
}

struct DecodedClassBank {
	std::uint16_t presence = 0;
	std::uint8_t family = 0;
	std::uint16_t canonical_index = 0;
	std::uint32_t bank_id = 0;
	std::uint32_t weapon_class_id = 0;
	float capacity = 0.0F;
};

bool decode_minimal_class_banks(const RecordEnvelopeView& envelope, std::vector<DecodedClassBank>& banks)
{
	if (envelope.raw_record_type != static_cast<std::uint16_t>(RecordType::ClassManifest)) return false;
	PacketReader reader(envelope.payload);
	std::uint32_t generation = 0;
	std::uint32_t id = 0;
	std::uint64_t presence = 0;
	std::string_view name;
	if (!reader.read_u32(generation) || !reader.read_u32(id) || !reader.read_u64(presence) ||
		!reader.read_utf8(255, name) || !reader.skip(8 + 4 + 12)) {
		return false;
	}
	if ((presence & ClassManifestPresenceFlagInertia) != 0U && !reader.skip(9 * sizeof(float))) return false;
	if ((presence & ClassManifestPresenceFlagMotion) != 0U && !reader.skip(19 * sizeof(float))) return false;
	if ((presence & ClassManifestPresenceFlagHull) != 0U && !reader.skip(sizeof(float))) return false;
	if ((presence & ClassManifestPresenceFlagShield) != 0U && !reader.skip(sizeof(float))) return false;
	if ((presence & ClassManifestPresenceFlagAfterburner) != 0U && !reader.skip(4 * sizeof(float) + 8)) return false;
	if ((presence & ClassManifestPresenceFlagCountermeasure) != 0U && !reader.skip(16)) return false;
	if ((presence & ClassManifestPresenceFlagBanks) == 0U) return false;
	std::uint16_t count = 0;
	if (!reader.read_u16(count)) return false;
	for (std::uint16_t index = 0; index < count; ++index) {
		std::uint8_t version = 0;
		std::uint16_t item_size = 0;
		PacketReader item;
		DecodedClassBank bank{};
		std::uint8_t reserved = 0;
		if (!reader.read_u8(version) || version != 1 || !reader.read_u16(item_size) ||
			!reader.subreader(item_size, item) || !item.read_u16(bank.presence) ||
			!item.read_u8(bank.family) || !item.read_u8(reserved) ||
			!item.read_u16(bank.canonical_index) || !item.read_u32(bank.bank_id)) {
			return false;
		}
		if ((bank.presence & ClassBankPresenceFlagWeaponClass) != 0U &&
			!item.read_u32(bank.weapon_class_id)) {
			return false;
		}
		if ((bank.presence & ClassBankPresenceFlagCapacity) != 0U && !item.read_f32(bank.capacity)) {
			return false;
		}
		banks.push_back(bank);
	}
	return true;
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
		test::phase2test::SourceCase source_case = test::phase2test::SourceCase::MultipartFullRequired)
	{
		if (initialize) {
			auto source = test::phase2test::make_source(source_case);
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

TEST(Phase2ManifestCodec,
	P2TST023KeepsTheFrozenWireCounterWithoutBuildingAnArtificialMaximumImage)
{
	using WireRecordCount =
		decltype(ManifestPartPayload{}.record_count);
	static_assert(std::is_same_v<WireRecordCount, std::uint16_t>);
	EXPECT_EQ(65'535U,
		std::numeric_limits<WireRecordCount>::max());

	ProvisionedManifest manifest;
	const auto& candidate = manifest.candidate();
	std::uint32_t projected_record_count = 0U;
	for (std::uint16_t index = 0U;
		 index < candidate.part_count; ++index) {
		projected_record_count +=
			candidate.parts[index].record_count;
	}
	EXPECT_LE(projected_record_count, 4'740U);
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
	for (const auto mutation : {test::phase2test::CatalogMutation::MissingClass,
		     test::phase2test::CatalogMutation::MissingWeapon,
		     test::phase2test::CatalogMutation::DuplicateClass,
		     test::phase2test::CatalogMutation::DuplicateWeapon,
		     test::phase2test::CatalogMutation::UnauthorizedExtraClass,
		     test::phase2test::CatalogMutation::UnauthorizedExtraWeapon}) {
		ProvisionedManifest manifest;
		auto transaction = test::phase2test::mutate_completed_catalog(manifest.candidate(), mutation);
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
	ProvisionedManifest manifest{true, test::phase2test::SourceCase::TwoClassesThreeWeaponsWithDecoys};
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
	std::vector<std::uint32_t> class_ids;
	std::vector<std::uint32_t> weapon_ids;
	std::vector<std::string> class_names;
	std::vector<std::string> weapon_names;
	for (const auto& [id, name] : classes) {
		class_ids.push_back(id);
		class_names.push_back(name);
	}
	for (const auto& [id, name] : weapons) {
		weapon_ids.push_back(id);
		weapon_names.push_back(name);
	}
	std::sort(class_ids.begin(), class_ids.end());
	std::sort(weapon_ids.begin(), weapon_ids.end());
	std::sort(class_names.begin(), class_names.end());
	std::sort(weapon_names.begin(), weapon_names.end());
	EXPECT_EQ((std::vector<std::uint32_t>{1, 2}), class_ids);
	EXPECT_EQ((std::vector<std::uint32_t>{1, 2, 3}), weapon_ids);
	EXPECT_EQ((std::vector<std::string>{"Apollo", "Ulysses"}), class_names);
	EXPECT_EQ((std::vector<std::string>{"Alpha", "Beta", "Gamma"}), weapon_names);
}

TEST(Phase2ManifestCodec, P2REQ015FullRequiredContainsExactlyClassAndWeaponRecordsWithoutSyntheticPadding)
{
	ProvisionedManifest manifest{true, test::phase2test::SourceCase::TwoClassesThreeWeaponsWithDecoys};
	std::size_t class_records = 0;
	std::size_t weapon_records = 0;
	std::size_t unexpected_records = 0;
	std::uint16_t first_unexpected_type = 0;
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
			} else if (envelope.raw_record_type == static_cast<std::uint16_t>(RecordType::WeaponManifest)) {
				++weapon_records;
			} else {
				if (unexpected_records == 0) first_unexpected_type = envelope.raw_record_type;
				++unexpected_records;
			}
		}
	}
	EXPECT_EQ(manifest.candidate().class_record_count, class_records);
	EXPECT_EQ(manifest.candidate().weapon_record_count, weapon_records);
	EXPECT_EQ(0u, unexpected_records)
		<< "first unexpected record type=0x" << std::hex << first_unexpected_type
		<< "; P2-REQ-015 requires one exact FullRequired CLASS_MANIFEST+WEAPON_MANIFEST transaction.";
}

TEST(Phase2ManifestCodec, P2TST047WeaponManifestCarriesEverySupportedSourceFieldExactly)
{
	auto source = test::phase2test::make_source(test::phase2test::SourceCase::TwoClassesThreeWeaponsWithDecoys);
	auto& expected = source->weapons[0];
	expected.title = "Alpha title";
	expected.subtype = WeaponSubtype::Primary;
	expected.class_flags = WeaponClassFlagBallistic | WeaponClassFlagHoming;
	expected.max_speed = 321.0F;
	expected.has_acceleration = true;
	expected.acceleration_time_us = 1234;
	expected.mass = 12.5F;
	expected.gravity_constant = 0.75F;
	expected.lifetime_us = 987654;
	expected.has_ranges = true;
	expected.minimum_range = 10.0F;
	expected.optimal_range = 20.0F;
	expected.maximum_range = 30.0F;
	expected.has_fire = true;
	expected.fire_wait_us = 22000;
	expected.energy_consumed = 3.5F;
	expected.has_damage = true;
	expected.damage = 44.0F;
	expected.effect_flags = WeaponEffectFlagShockwave | WeaponEffectFlagShieldPiercing;
	expected.has_guidance = true;
	expected.guidance_type = 2;
	expected.guidance_fov_rad = 0.5F;
	expected.has_lock = true;
	expected.lock_time_us = 33000;
	expected.lock_fov_rad = 0.25F;
	expected.velocity_inherit_amount = 0.625F;
	expected.has_cargo_rearm = true;
	expected.cargo_size = 2.5F;
	expected.rearm_time_us = 44000;
	expected.reloaded_per_batch = 7;
	expected.has_burst = true;
	expected.burst_count = 3;
	expected.burst_interval_us = 55000;
	expected.has_swarm = true;
	expected.swarm_count = 4;
	expected.shots_per_trigger = 2;

	ProvisionedManifest manifest(false);
	ASSERT_EQ(Phase2ManifestError::None, manifest.slot.rebuild(*source));
	DecodedWeaponManifest decoded{};
	bool found = false;
	for (std::uint16_t part_index = 0; part_index < manifest.candidate().part_count; ++part_index) {
		const auto& part = manifest.candidate().parts[part_index];
		RecordEnvelopeIterator iterator(part.records, part.record_count, RecordFlagPolicy::RequireNone);
		for (;;) {
			RecordEnvelopeView envelope{};
			bool has_value = false;
			ASSERT_EQ(ValidationError::None, iterator.next(envelope, has_value));
			if (!has_value) break;
			DecodedWeaponManifest candidate{};
			if (decode_weapon_manifest(envelope, candidate) && candidate.name == expected.name) {
				decoded = candidate;
				found = true;
			}
		}
	}
	ASSERT_TRUE(found);
	EXPECT_EQ(manifest.candidate().manifest_id, decoded.generation);
	EXPECT_NE(0u, decoded.id);
	EXPECT_EQ(WeaponManifestPresenceFlagTitle | WeaponManifestPresenceFlagAcceleration |
			WeaponManifestPresenceFlagRanges | WeaponManifestPresenceFlagFire |
			WeaponManifestPresenceFlagDamage | WeaponManifestPresenceFlagGuidance |
			WeaponManifestPresenceFlagLock | WeaponManifestPresenceFlagCargoRearm |
			WeaponManifestPresenceFlagBurst | WeaponManifestPresenceFlagSwarm,
		decoded.presence);
	EXPECT_EQ(expected.title, decoded.title);
	EXPECT_EQ(static_cast<std::uint8_t>(expected.subtype), decoded.subtype);
	EXPECT_EQ(expected.class_flags, decoded.class_flags);
	EXPECT_FLOAT_EQ(expected.max_speed, decoded.max_speed);
	EXPECT_EQ(expected.acceleration_time_us, decoded.acceleration_time_us);
	EXPECT_FLOAT_EQ(expected.mass, decoded.mass);
	EXPECT_FLOAT_EQ(expected.gravity_constant, decoded.gravity_constant);
	EXPECT_EQ(expected.lifetime_us, decoded.lifetime_us);
	EXPECT_FLOAT_EQ(expected.minimum_range, decoded.minimum_range);
	EXPECT_FLOAT_EQ(expected.optimal_range, decoded.optimal_range);
	EXPECT_FLOAT_EQ(expected.maximum_range, decoded.maximum_range);
	EXPECT_EQ(expected.fire_wait_us, decoded.fire_wait_us);
	EXPECT_FLOAT_EQ(expected.energy_consumed, decoded.energy_consumed);
	EXPECT_FLOAT_EQ(expected.damage, decoded.damage);
	EXPECT_EQ(0u, decoded.damage_type_id);
	EXPECT_EQ(expected.effect_flags, decoded.effect_flags);
	EXPECT_EQ(expected.guidance_type, decoded.guidance_type);
	EXPECT_FLOAT_EQ(expected.guidance_fov_rad, decoded.guidance_fov_rad);
	EXPECT_EQ(expected.lock_time_us, decoded.lock_time_us);
	EXPECT_FLOAT_EQ(expected.lock_fov_rad, decoded.lock_fov_rad);
	EXPECT_FLOAT_EQ(expected.velocity_inherit_amount, decoded.velocity_inherit_amount);
	EXPECT_FLOAT_EQ(expected.cargo_size, decoded.cargo_size);
	EXPECT_EQ(expected.rearm_time_us, decoded.rearm_time_us);
	EXPECT_EQ(expected.reloaded_per_batch, decoded.reloaded_per_batch);
	EXPECT_EQ(expected.burst_count, decoded.burst_count);
	EXPECT_EQ(expected.burst_interval_us, decoded.burst_interval_us);
	EXPECT_EQ(expected.swarm_count, decoded.swarm_count);
	EXPECT_EQ(expected.shots_per_trigger, decoded.shots_per_trigger);
}

TEST(Phase2ManifestCodec, P2TST047BankCapacityPresenceFollowsConsumesAmmunition)
{
	auto source = test::phase2test::make_source(test::phase2test::SourceCase::TwoClassesThreeWeaponsWithDecoys);
	auto& ship_class = source->ship_classes[0];
	ship_class.bank_count = 2;
	ship_class.banks[0].family = WeaponFamily::Primary;
	ship_class.banks[0].source_family = WeaponFamily::Primary;
	ship_class.banks[0].bank_index = 0;
	ship_class.banks[0].weapon_source_key = 101;
	ship_class.banks[0].consumes_ammunition = false;
	ship_class.banks[0].capacity = 99.0F;
	ship_class.banks[1].family = WeaponFamily::Secondary;
	ship_class.banks[1].source_family = WeaponFamily::Secondary;
	ship_class.banks[1].bank_index = 0;
	ship_class.banks[1].weapon_source_key = 102;
	ship_class.banks[1].consumes_ammunition = true;
	ship_class.banks[1].capacity = 12.0F;

	ProvisionedManifest manifest(false);
	ASSERT_EQ(Phase2ManifestError::None, manifest.slot.rebuild(*source));
	std::vector<DecodedClassBank> banks;
	bool found = false;
	for (std::uint16_t part_index = 0; part_index < manifest.candidate().part_count; ++part_index) {
		const auto& part = manifest.candidate().parts[part_index];
		RecordEnvelopeIterator iterator(part.records, part.record_count, RecordFlagPolicy::RequireNone);
		for (;;) {
			RecordEnvelopeView envelope{};
			bool has_value = false;
			ASSERT_EQ(ValidationError::None, iterator.next(envelope, has_value));
			if (!has_value) break;
			if (envelope.raw_record_type == static_cast<std::uint16_t>(RecordType::ClassManifest) &&
				decode_manifest_identity(envelope).second == ship_class.name) {
				ASSERT_TRUE(decode_minimal_class_banks(envelope, banks));
				found = true;
			}
		}
	}
	ASSERT_TRUE(found);
	ASSERT_EQ(2u, banks.size());
	const auto primary = std::find_if(banks.begin(), banks.end(), [](const auto& bank) {
		return bank.family == static_cast<std::uint8_t>(WeaponFamily::Primary);
	});
	const auto secondary = std::find_if(banks.begin(), banks.end(), [](const auto& bank) {
		return bank.family == static_cast<std::uint8_t>(WeaponFamily::Secondary);
	});
	ASSERT_NE(banks.end(), primary);
	ASSERT_NE(banks.end(), secondary);
	EXPECT_EQ(0u, primary->presence & ClassBankPresenceFlagCapacity);
	EXPECT_EQ(ClassBankPresenceFlagCapacity,
		secondary->presence & ClassBankPresenceFlagCapacity);
	EXPECT_FLOAT_EQ(12.0F, secondary->capacity);
}

TEST(Phase2ManifestCodec, P2TST047ClassManifestWireCarriesEffectivePhysicsAndAuxiliaryIds)
{
	auto source = test::phase2test::make_source(test::phase2test::SourceCase::AllAuxiliaryRegistries);
	auto& expected = source->ship_classes[0];
	expected.effective_mass = 20.0F;
	expected.center_of_mass = {1.0F, 2.0F, 3.0F};
	expected.effective_inertia = {4.0F, 5.0F, 6.0F};
	expected.effective_inertia_matrix = {4.0F, 0.0F, 0.0F, 0.0F, 5.0F, 0.0F, 0.0F, 0.0F, 6.0F};
	expected.half_angle_cosines = {1.0F, 0.0F, -1.0F};
	expected.max_velocity = {1.0F, 2.0F, 3.0F};
	expected.afterburner_max_velocity = {4.0F, 5.0F, 6.0F};
	expected.booster_max_velocity = {7.0F, 8.0F, 9.0F};
	expected.max_rotational_velocity = {10.0F, 11.0F, 12.0F};
	expected.max_rear_velocity = 13.0F;
	expected.forward_accel_time = 14.0F;
	expected.afterburner_forward_accel_time = 15.0F;
	expected.booster_forward_accel_time = 16.0F;
	expected.forward_decel_time = 17.0F;
	expected.slide_accel_time = 18.0F;
	expected.slide_decel_time = 19.0F;
	expected.max_hull_strength = 200.0F;
	expected.max_shield_strength = 150.0F;

	ProvisionedManifest manifest(false);
	ASSERT_EQ(Phase2ManifestError::None, manifest.slot.rebuild(*source));
	const auto class_end =
		manifest.candidate().class_records.begin() + manifest.candidate().class_record_count;
	const auto projected = std::find_if(manifest.candidate().class_records.begin(),
		class_end,
		[&](const auto& value) { return value.name == expected.name; });
	ASSERT_NE(class_end, projected);
	EXPECT_EQ(2u, projected->iff_id);
	EXPECT_EQ(2u, projected->wing_id);
	EXPECT_EQ((std::array<float, 3>{4.0F, 5.0F, 6.0F}), projected->inertia);
	EXPECT_FLOAT_EQ(0.0F, projected->half_angles_rad[0]);
	EXPECT_FLOAT_EQ(static_cast<float>(std::acos(0.0)), projected->half_angles_rad[1]);
	EXPECT_FLOAT_EQ(static_cast<float>(std::acos(-1.0)), projected->half_angles_rad[2])
		<< "ClassManifestV1 has no angle field; the effective angle provenance remains in the canonical projection.";

	DecodedClassManifestSubset decoded{};
	bool found = false;
	for (std::uint16_t part_index = 0; part_index < manifest.candidate().part_count; ++part_index) {
		const auto& part = manifest.candidate().parts[part_index];
		RecordEnvelopeIterator iterator(part.records, part.record_count, RecordFlagPolicy::RequireNone);
		for (;;) {
			RecordEnvelopeView envelope{};
			bool has_value = false;
			ASSERT_EQ(ValidationError::None, iterator.next(envelope, has_value));
			if (!has_value) break;
			DecodedClassManifestSubset candidate{};
			if (decode_class_manifest_subset(envelope, candidate) && candidate.name == expected.name) {
				decoded = candidate;
				found = true;
			}
		}
	}
	ASSERT_TRUE(found);
	EXPECT_EQ(manifest.candidate().manifest_id, decoded.generation);
	EXPECT_EQ(projected->class_id, decoded.id);
	EXPECT_EQ(2u, decoded.species_id);
	EXPECT_EQ(2u, decoded.ship_type_id);
	EXPECT_FLOAT_EQ(expected.effective_mass, decoded.mass);
	EXPECT_EQ((std::array<float, 3>{1.0F, 2.0F, 3.0F}), decoded.center_of_mass);
	EXPECT_EQ(expected.effective_inertia_matrix, decoded.inertia);
	EXPECT_EQ((std::array<float, 19>{
				  1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F,
				  10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F, 17.0F, 18.0F, 19.0F}),
		decoded.motion);
	EXPECT_FLOAT_EQ(200.0F, decoded.hull);
	EXPECT_FLOAT_EQ(150.0F, decoded.shield);
	EXPECT_EQ((std::vector<std::uint32_t>{2}), decoded.subsystem_armor_ids);
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
