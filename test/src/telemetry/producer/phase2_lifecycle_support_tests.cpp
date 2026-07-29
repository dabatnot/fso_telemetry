#include "telemetry/phase2_state_image.h"
#include "telemetry/phase2_wp03_allocation_tracker.h"
#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_business_state_validation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace telemetry;
using namespace telemetry::detail;
using namespace telemetry::protocol;

const StateAtom* atom(const StateImage& image,
	RecordType type,
	std::uint64_t entity = 0U)
{
	const auto raw = static_cast<std::uint16_t>(type);
	for (const auto& value : image.records()) {
		if (value.key.record_type != raw) continue;
		if (entity == 0U) return &value;
		PacketReader reader({value.value.data(), value.value.size()});
		std::uint64_t found = 0U;
		if (reader.read_u64(found) && found == entity) return &value;
	}
	return nullptr;
}

ValidationError validate_atom(const StateAtom& value)
{
	BusinessRecordMetadata metadata{};
	const RecordEnvelopeView envelope{
		value.key.record_type, value.record_version, RecordFlagNone,
		{value.value.data(), value.value.size()}};
	return validate_business_record(
		envelope, BusinessRecordContainer::FullSnapshot, VersionMinorV1_1, metadata);
}

struct CargoDecoded {
	std::uint64_t presence = 0U;
	std::uint8_t phase = 0U;
	std::uint8_t disclosure = 0U;
	std::uint64_t target = 0U;
	std::uint64_t elapsed = 0U;
	std::uint64_t required = 0U;
	std::uint8_t validity = 0U;
};

CargoDecoded decode_cargo(const StateAtom& value)
{
	PacketReader reader({value.value.data(), value.value.size()});
	CargoDecoded decoded{};
	std::uint64_t ignored = 0U;
	EXPECT_TRUE(reader.read_u64(ignored));
	EXPECT_TRUE(reader.read_u64(decoded.presence));
	EXPECT_TRUE(reader.read_u64(ignored));
	EXPECT_TRUE(reader.read_u8(decoded.phase));
	EXPECT_TRUE(reader.read_u8(decoded.disclosure));
	if ((decoded.presence & CargoScanStatePresenceFlagTarget) != 0U)
		EXPECT_TRUE(reader.read_u64(decoded.target));
	if ((decoded.presence & CargoScanStatePresenceFlagSubsystem) != 0U) {
		std::uint32_t subsystem = 0U;
		EXPECT_TRUE(reader.read_u32(subsystem));
	}
	if ((decoded.presence & CargoScanStatePresenceFlagTiming) != 0U) {
		EXPECT_TRUE(reader.read_u64(decoded.elapsed));
		EXPECT_TRUE(reader.read_u64(decoded.required));
	}
	if ((decoded.presence & CargoScanStatePresenceFlagValidity) != 0U)
		EXPECT_TRUE(reader.read_u8(decoded.validity));
	if ((decoded.presence & CargoScanStatePresenceFlagCargoText) != 0U) {
		std::string_view text;
		EXPECT_TRUE(reader.read_utf8(511U, text));
		EXPECT_EQ("Supplies", text);
	}
	EXPECT_TRUE(reader.at_end());
	return decoded;
}

void expect_empty_weapon(const StateAtom& value, std::uint64_t entity)
{
	PacketReader reader({value.value.data(), value.value.size()});
	std::uint64_t value64 = 0U;
	std::uint32_t value32 = 1U;
	std::uint16_t value16 = 1U;
	ASSERT_TRUE(reader.read_u64(value64));
	EXPECT_EQ(entity, value64);
	ASSERT_TRUE(reader.read_u64(value64));
	EXPECT_EQ(0U, value64);
	ASSERT_TRUE(reader.read_u64(value64));
	for (std::size_t index = 0U; index < 4U; ++index) {
		ASSERT_TRUE(reader.read_u16(value16));
		EXPECT_EQ(0U, value16);
	}
	for (std::size_t index = 0U; index < 4U; ++index) {
		ASSERT_TRUE(reader.read_u32(value32));
		EXPECT_EQ(0U, value32);
	}
	for (std::size_t index = 0U; index < 2U; ++index) {
		ASSERT_TRUE(reader.read_u16(value16));
		EXPECT_EQ(0U, value16);
	}
	EXPECT_TRUE(reader.at_end());
}

std::pair<std::uint64_t, std::uint16_t> decode_docking(
	const StateAtom& value, DockingPhase phase)
{
	PacketReader reader({value.value.data(), value.value.size()});
	std::uint64_t ignored = 0U;
	std::uint64_t leader = 0U;
	std::uint16_t count = 0U;
	std::uint8_t decoded_phase = 0xffU;
	EXPECT_TRUE(reader.read_u64(ignored));
	EXPECT_TRUE(reader.read_u64(ignored));
	EXPECT_TRUE(reader.read_u64(ignored));
	EXPECT_TRUE(reader.read_u8(decoded_phase));
	EXPECT_EQ(static_cast<std::uint8_t>(phase), decoded_phase);
	EXPECT_TRUE(reader.read_u64(leader));
	EXPECT_TRUE(reader.read_u16(count));
	for (std::size_t index = 0U; index < count; ++index) {
		std::uint8_t version = 0U;
		std::uint16_t size = 0U;
		EXPECT_TRUE(reader.read_u8(version));
		EXPECT_EQ(1U, version);
		EXPECT_TRUE(reader.read_u16(size));
		PacketReader item;
		EXPECT_TRUE(reader.subreader(size, item));
		EXPECT_TRUE(item.read_u64(ignored));
		std::uint16_t point = 0U;
		EXPECT_TRUE(item.read_u16(point));
		EXPECT_TRUE(item.read_u16(point));
		std::string_view name;
		EXPECT_TRUE(item.read_utf8(127U, name));
		EXPECT_TRUE(item.read_utf8(127U, name));
		EXPECT_TRUE(item.at_end());
	}
	EXPECT_TRUE(reader.at_end());
	return {leader, count};
}

void expect_complete_atoms(const StateImage& image,
	const std::array<std::uint64_t, 2U>& entities,
	std::size_t count)
{
	for (const auto& record : image.records())
		EXPECT_EQ(ValidationError::None, validate_atom(record));
	for (std::size_t index = 0U; index < count; ++index) {
		const auto entity = entities[index];
		for (const auto type : {RecordType::EntityLifecycle,
				 RecordType::ShipIdentity, RecordType::FlightState,
				 RecordType::DamageState, RecordType::ShieldState,
				 RecordType::EnergyState, RecordType::PropulsionState,
				 RecordType::WeaponState, RecordType::DockingState,
				 RecordType::SupportState})
			EXPECT_NE(nullptr, atom(image, type, entity));
		expect_empty_weapon(*atom(image, RecordType::WeaponState, entity), entity);
	}
	EXPECT_NE(nullptr, atom(image, RecordType::SessionState));
	EXPECT_NE(nullptr, atom(image, RecordType::MissionState));
	EXPECT_NE(nullptr, atom(image, RecordType::ControlState, entities[0]));
	EXPECT_NE(nullptr, atom(image, RecordType::CargoScanState, entities[0]));
}

BusinessStateValidationContext complete_validation_context(
	const Phase2ManifestCandidate& manifest,
	const std::uint64_t* entities,
	std::size_t entity_count)
{
	static BusinessClassCatalogEntry catalog;
	static std::array<std::uint32_t,
		Phase2ManifestLimits::MaxSubsystemsPerShip> subsystem_ids{};
	const auto& ship_class = manifest.class_records[0];
	for (std::size_t index = 0U;
		 index < ship_class.subsystem_count; ++index)
		subsystem_ids[index] =
			ship_class.subsystems[index].subsystem_id;
	catalog = {ship_class.class_id, subsystem_ids.data(),
		ship_class.subsystem_count};
	BusinessStateValidationContext context{};
	context.protocol_minor = VersionMinorV1_1;
	context.required_manifest_id = manifest.manifest_id;
	context.class_manifest_installed = true;
	context.weapon_manifest_installed = true;
	context.class_catalog = &catalog;
	context.class_catalog_count = 1U;
	context.enforce_cockpit_entity_allowlist = true;
	context.cockpit_entity_ids = entities;
	context.cockpit_entity_count = entity_count;
	return context;
}

void expect_complete_schema_round_trip(
	const StateImage& image,
	const Phase2CompleteDomainInput& input)
{
	static constexpr std::array<RecordType, 12U> ExpectedEntityTypes{{
		RecordType::ShipIdentity,
		RecordType::FlightState,
		RecordType::ControlState,
		RecordType::DamageState,
		RecordType::ShieldState,
		RecordType::SubsystemState,
		RecordType::EnergyState,
		RecordType::PropulsionState,
		RecordType::WeaponState,
		RecordType::CargoScanState,
		RecordType::DockingState,
		RecordType::SupportState,
	}};
	for (const auto type : ExpectedEntityTypes) {
		EXPECT_NE(nullptr, atom(image, type));
	}

	std::vector<std::uint8_t> encoded;
	encoded.reserve(image.encoded_snapshot_records_size());
	for (const auto& record : image.records()) {
		BusinessRecordMetadata metadata{};
		ASSERT_TRUE(business_record_metadata(
			record.key.record_type, metadata));
		EXPECT_EQ(metadata.lifecycle, record.lifecycle);
		const auto type =
			static_cast<RecordType>(record.key.record_type);
		const bool entity_scoped =
			type >= RecordType::ShipIdentity &&
			type <= RecordType::EffectState;
		EXPECT_EQ(entity_scoped, record.has_cascade_owner);
		if (entity_scoped) {
			ASSERT_GE(record.value.size(), sizeof(std::uint64_t));
			EXPECT_EQ(static_cast<std::uint16_t>(
						  RecordType::EntityLifecycle),
				record.cascade_owner.record_type);
			ASSERT_EQ(sizeof(std::uint64_t),
				record.cascade_owner.identity.size());
			EXPECT_TRUE(std::equal(
				record.value.begin(),
				record.value.begin() + sizeof(std::uint64_t),
				record.cascade_owner.identity.begin()));
		} else {
			EXPECT_EQ(StateAtomKey{}, record.cascade_owner);
		}

		const RecordEnvelopeView envelope{
			record.key.record_type,
			record.record_version,
			RecordFlagNone,
			{record.value.empty() ? nullptr : record.value.data(),
				record.value.size()}};
		const auto offset = encoded.size();
		encoded.resize(offset + RecordEnvelopeHeaderSize +
			record.value.size());
		std::size_t written = 0U;
		ASSERT_EQ(ValidationError::None,
			encode_business_record(
				envelope,
				BusinessRecordContainer::FullSnapshot,
				VersionMinorV1_1,
				{encoded.data() + offset,
				 encoded.size() - offset},
				written));
		ASSERT_EQ(RecordEnvelopeHeaderSize +
				record.value.size(),
			written);
	}

	const std::array<std::uint64_t, 1U> entities{{
		input.player_entity_id}};
	auto context = complete_validation_context(
		*input.installed_manifest,
		entities.data(), entities.size());
	BusinessStateImageValidator validator(context);
	StateImage decoded;
	ASSERT_EQ(ValidationError::None,
		decode_business_snapshot_region_validated(
			{encoded.data(), encoded.size()},
			static_cast<std::uint16_t>(
				image.records().size()),
			validator, decoded));
	EXPECT_EQ(image, decoded);
	EXPECT_EQ(input.canonical_hash(image),
		input.canonical_hash(decoded));
}

struct Fixture {
	std::unique_ptr<Phase2ObservationDto> observation =
		std::make_unique<Phase2ObservationDto>();
	std::unique_ptr<Phase2ManifestCandidate> manifest =
		std::make_unique<Phase2ManifestCandidate>();
	std::array<Phase2Wp05SubjectBinding, MaximumPhase2ObservationShips> subjects{};
	Phase2Wp07CleanupRing cleanup;
	Phase2Wp07SupportTerminalRing support;
	Phase2Wp07EpisodeLatches latches;
	Phase2CompleteDomainInput input{};

	Fixture()
	{
		observation->capture = {Phase2CaptureStatus::Valid, Phase2CaptureReason::None};
		observation->producer_sample_time_us = 1'000'000U;
		observation->player_key.value = 11U;
		observation->ships.resize(1U);
		add_ship(0U, 11U, 101U, 9001U);
		manifest->manifest_id = 7U;
		manifest->class_record_count = 1U;
		manifest->class_records[0].source_key = 101U;
		manifest->class_records[0].class_id = 501U;
		input.observation = observation.get();
		input.installed_manifest = manifest.get();
		input.subjects = subjects.data();
		input.subject_count = 1U;
		input.player_entity_id = 9001U;
		input.cleanup_ring = &cleanup;
		input.support_terminal_ring = &support;
		input.episode_latches = &latches;
	}

	void add_ship(std::size_t index,
		std::uint32_t key,
		std::uint32_t class_key,
		std::uint64_t entity)
	{
		auto& ship = observation->ships[index];
		ship.capture_key.value = key;
		ship.identity.class_source_key.value = class_key;
		ship.lifecycle.sample_time_us = observation->producer_sample_time_us;
		ship.docking.sample_time_us = observation->producer_sample_time_us;
		ship.support.sample_time_us = observation->producer_sample_time_us;
		subjects[index] = {{key}, entity};
	}
};

TEST(Phase2LifecycleSupport,
	CompleteShipSchemaMetadataRoundTripsForDirectPreallocatedAndRetainedImages)
{
	Fixture fixture;
	auto& ship = fixture.observation->ships[0];
	ship.subsystems.count = 1U;
	auto& subsystem = ship.subsystems.values[0];
	subsystem.source_key.value = 1'001U;
	subsystem.sample_time_us =
		fixture.observation->producer_sample_time_us;
	subsystem.kind = ShipSubsystemKind::Generic;
	auto& ship_class = fixture.manifest->class_records[0];
	ship_class.subsystem_count = 1U;
	ship_class.subsystems = {
		fixture.manifest->subsystem_records.data(), 1U};
	fixture.manifest->subsystem_records[0] = {
		1'001U, 2'001U, 0U, 0U};
	fixture.manifest->aggregate_subsystem_count = 1U;

	StateImage direct;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, direct));
	expect_complete_schema_round_trip(direct, fixture.input);

	Phase2CompleteDomainPool pool;
	ASSERT_TRUE(pool.provision(1U, 1U, 0U, 1U));
	const auto backing_bytes = pool.owned_backing_bytes();
	StateImage preallocated;
	StateImage retained;
	telemetry::test::wp03::GlobalAllocationScope allocations;
	const auto preallocated_status =
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, preallocated);
	fixture.input.retained_state = &preallocated;
	const auto retained_status =
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, retained);
	const auto allocation_count = allocations.finish();

	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		preallocated_status);
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		retained_status);
	EXPECT_EQ(0U, allocation_count);
	EXPECT_EQ(backing_bytes, pool.owned_backing_bytes());
	EXPECT_EQ(direct, preallocated);
	EXPECT_EQ(preallocated, retained);
	expect_complete_schema_round_trip(
		preallocated, fixture.input);
	expect_complete_schema_round_trip(retained, fixture.input);
}

TEST(Phase2LifecycleSupport, P2TST041CargoPhasesReferencesTimingAndDisclosureAreExact)
{
	for (const auto phase : {CargoScanPhaseObservation::NotScannable,
			 CargoScanPhaseObservation::Idle,
			 CargoScanPhaseObservation::Scanning,
			 CargoScanPhaseObservation::Completed}) {
		Fixture fixture;
		auto& cargo = fixture.observation->player_cargo_scan;
		cargo.sample_time_us = 1'000'000U;
		cargo.phase = phase;
		if (phase != CargoScanPhaseObservation::NotScannable) {
			cargo.presence = CargoScanStatePresenceFlagTarget |
				CargoScanStatePresenceFlagTiming |
				CargoScanStatePresenceFlagValidity;
			cargo.target_capture_key.value = 11U;
			cargo.elapsed_us = phase == CargoScanPhaseObservation::Completed ? 101U : 50U;
			cargo.required_us = 100U;
			cargo.validity_flags = ScanValidityFlagInRange |
				ScanValidityFlagInAngle | ScanValidityFlagLineOfSight;
		}
		if (phase == CargoScanPhaseObservation::Completed) {
			cargo.presence |= CargoScanStatePresenceFlagCargoText;
			ASSERT_TRUE(cargo.cargo_text.assign("Supplies"));
		}
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		const auto* record = atom(image, RecordType::CargoScanState, 9001U);
		ASSERT_NE(nullptr, record);
		EXPECT_EQ(ValidationError::None, validate_atom(*record));
		const auto decoded = decode_cargo(*record);
		EXPECT_EQ(static_cast<std::uint8_t>(phase), decoded.phase);
		EXPECT_EQ(cargo.presence, decoded.presence);
		EXPECT_EQ(cargo.elapsed_us, decoded.elapsed);
		EXPECT_EQ(cargo.required_us, decoded.required);
		EXPECT_EQ(cargo.validity_flags, decoded.validity);
	}
	struct AuthorityCase {
		bool below_range;
		bool dot_at_or_above;
		bool sensors_ok;
		bool line_of_sight;
		std::uint64_t elapsed_us;
		CargoScanPhaseObservation phase;
	};
	constexpr std::array<AuthorityCase, 6U> authority_cases{{
		{true, true, true, true, 99U, CargoScanPhaseObservation::Scanning},
		{true, true, true, true, 101U, CargoScanPhaseObservation::Completed},
		{false, true, true, true, 0U, CargoScanPhaseObservation::Idle},
		{true, false, true, true, 0U, CargoScanPhaseObservation::Idle},
		{true, true, false, true, 0U, CargoScanPhaseObservation::Idle},
		{true, true, true, false, 0U, CargoScanPhaseObservation::Idle},
	}};
	for (const auto& authority : authority_cases) {
		Fixture fixture;
		auto& cargo = fixture.observation->player_cargo_scan;
		cargo.sample_time_us = 1'000'000U;
		cargo.presence = CargoScanStatePresenceFlagTarget |
			CargoScanStatePresenceFlagTiming |
			CargoScanStatePresenceFlagValidity;
		cargo.target_capture_key.value = 11U;
		cargo.required_us = 100U;
		cargo.elapsed_us = authority.elapsed_us;
		cargo.phase = authority.phase;
		cargo.validity_flags =
			(authority.below_range ? ScanValidityFlagInRange : 0U) |
			(authority.dot_at_or_above ? ScanValidityFlagInAngle : 0U) |
			(authority.line_of_sight ? ScanValidityFlagLineOfSight : 0U);
		if (!authority.sensors_ok)
			cargo.validity_flags = ScanValidityFlagInRange |
				ScanValidityFlagInAngle | ScanValidityFlagLineOfSight;
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		const auto decoded =
			decode_cargo(*atom(image, RecordType::CargoScanState, 9001U));
		EXPECT_EQ(static_cast<std::uint8_t>(authority.phase), decoded.phase);
		EXPECT_EQ(authority.elapsed_us, decoded.elapsed);
		EXPECT_EQ(cargo.validity_flags, decoded.validity);
	}
	Fixture reset;
	auto& cargo = reset.observation->player_cargo_scan;
	cargo.sample_time_us = 1'000'000U;
	cargo.phase = CargoScanPhaseObservation::Idle;
	cargo.presence = CargoScanStatePresenceFlagTarget |
		CargoScanStatePresenceFlagTiming | CargoScanStatePresenceFlagValidity;
	cargo.target_capture_key.value = 11U;
	cargo.required_us = 100U;
	const std::array<std::uint32_t, 4U> validity_cases{{
		ScanValidityFlagNone,
			 ScanValidityFlagInRange,
			 ScanValidityFlagInRange | ScanValidityFlagInAngle,
			 ScanValidityFlagInRange | ScanValidityFlagInAngle |
				ScanValidityFlagLineOfSight,
	}};
	for (const auto validity : validity_cases) {
		cargo.validity_flags = validity;
		cargo.elapsed_us = validity == KnownScanValidityFlags ? 51U : 0U;
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(reset.input, image));
		const auto decoded =
			decode_cargo(*atom(image, RecordType::CargoScanState, 9001U));
		EXPECT_EQ(validity, decoded.validity);
		EXPECT_EQ(cargo.elapsed_us, decoded.elapsed);
	}
	cargo.target_capture_key.value = 12U;
	cargo.elapsed_us = 51U;
	StateImage rejected;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(reset.input, rejected));
	cargo.target_capture_key.value = 11U;
	cargo.elapsed_us = 0U;
	StateImage restarted;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(reset.input, restarted));
	EXPECT_EQ(0U,
		decode_cargo(*atom(restarted, RecordType::CargoScanState, 9001U))
			.elapsed);
}

TEST(Phase2LifecycleSupport, P2TST041CargoAuthorityIsConsumedOnceAndRejectsStaleOrIncoherentReferencesAtomically)
{
	Fixture fixture;
	fixture.input.cargo_authority_generation = 17U;
	fixture.input.expected_cargo_authority_generation = 17U;
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	const auto enabled_image = image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	EXPECT_EQ(enabled_image, image);
	EXPECT_EQ(1U, fixture.input.cargo_authority_consume_count());
	const auto sentinel = image;
	fixture.input.expected_cargo_authority_generation = 18U;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	EXPECT_EQ(sentinel, image);
	EXPECT_EQ(1U, fixture.input.cargo_authority_consume_count());
	fixture.input.cargo_authority_generation = 18U;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	EXPECT_EQ(2U, fixture.input.cargo_authority_consume_count());
}

TEST(Phase2LifecycleSupport, P2TST042DockingPhasesReciprocityLeaderAndTransitiveClosureAreExact)
{
	Fixture fixture;
	fixture.observation->ships.resize(2U);
	fixture.add_ship(1U, 12U, 101U, 9002U);
	fixture.input.subject_count = 2U;
	auto& first = fixture.observation->ships[0].docking;
	auto& second = fixture.observation->ships[1].docking;
	first.relation_count = second.relation_count = 1U;
	first.dock_leader = true;
	first.relations[0].remote_capture_key.value = 12U;
	first.relations[0].local_dockpoint = 3U;
	first.relations[0].remote_dockpoint = 4U;
	second.relations[0].remote_capture_key.value = 11U;
	second.relations[0].local_dockpoint = 4U;
	second.relations[0].remote_dockpoint = 3U;
	for (const auto phase : {DockingPhase::None, DockingPhase::Approach,
			 DockingPhase::Docking, DockingPhase::Docked,
			 DockingPhase::Undocking}) {
		fixture.input.docking_phase_override = phase;
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		const auto* first_record =
			atom(image, RecordType::DockingState, 9001U);
		const auto* second_record =
			atom(image, RecordType::DockingState, 9002U);
		ASSERT_NE(nullptr, first_record);
		ASSERT_NE(nullptr, second_record);
		const std::pair<std::uint64_t, std::uint16_t> expected{9001U, 1U};
		EXPECT_EQ(expected, decode_docking(*first_record, phase));
		EXPECT_EQ(expected, decode_docking(*second_record, phase));
	}
	second.dock_leader = true;
	const auto empty = StateImage{};
	StateImage rejected;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, rejected));
	EXPECT_EQ(empty, rejected);

	Fixture components;
	components.observation->ships.resize(4U);
	for (std::size_t index = 1U; index < 4U; ++index)
		components.add_ship(index, static_cast<std::uint32_t>(11U + index),
			101U, 9001U + index);
	components.input.subject_count = 4U;
	for (const auto pair : {std::pair<std::size_t, std::size_t>{0U, 1U},
			 std::pair<std::size_t, std::size_t>{2U, 3U}}) {
		auto& left = components.observation->ships[pair.first].docking;
		auto& right = components.observation->ships[pair.second].docking;
		left.relation_count = right.relation_count = 1U;
		left.dock_leader = true;
		left.relations[0].remote_capture_key.value =
			components.observation->ships[pair.second].capture_key.value;
		right.relations[0].remote_capture_key.value =
			components.observation->ships[pair.first].capture_key.value;
	}
	StateImage accepted;
	EXPECT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(components.input, accepted));
}

TEST(Phase2LifecycleSupport, P2TST042DockingZeroSixtyFourAndSixtyFiveRelationsFailClosedAtomically)
{
	Fixture fixture;
	fixture.observation->ships.resize(2U);
	fixture.add_ship(1U, 12U, 101U, 9002U);
	fixture.input.subject_count = 2U;
	auto& docking = fixture.observation->ships[0].docking;
	auto& inverse = fixture.observation->ships[1].docking;
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	docking.relation_count = 64U;
	inverse.relation_count = 64U;
	docking.dock_leader = true;
	for (std::size_t index = 0U; index < 64U; ++index) {
		docking.relations[index].remote_capture_key.value = 12U;
		docking.relations[index].local_dockpoint =
			static_cast<std::uint16_t>(index);
		docking.relations[index].remote_dockpoint =
			static_cast<std::uint16_t>(63U - index);
		inverse.relations[index].remote_capture_key.value = 11U;
		inverse.relations[index].local_dockpoint =
			static_cast<std::uint16_t>(63U - index);
		inverse.relations[index].remote_dockpoint =
			static_cast<std::uint16_t>(index);
	}
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	EXPECT_EQ(64U,
		decode_docking(*atom(image, RecordType::DockingState, 9001U),
			DockingPhase::None).second);
	const auto sentinel = image;
	EXPECT_EQ(sentinel, image);
	docking.relation_count = 65U;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	EXPECT_EQ(sentinel, image);
}

TEST(Phase2LifecycleSupport, P2TST043SupportPhasesPredicatesRawWorkAndEpisodeIdentityAreExact)
{
	struct SupportPhaseCase {
		ShipSupportPhase input;
		SupportPhase wire;
	};
	constexpr std::array<SupportPhaseCase, 8U> cases{{
		{ShipSupportPhase::None, SupportPhase::None},
		{ShipSupportPhase::Queued, SupportPhase::Requested},
		{ShipSupportPhase::OnWay, SupportPhase::Approaching},
		{ShipSupportPhase::Docking, SupportPhase::Docking},
		{ShipSupportPhase::Repairing, SupportPhase::Repairing},
		{ShipSupportPhase::Rearming, SupportPhase::Rearming},
		{ShipSupportPhase::Aborted, SupportPhase::Aborted},
		{ShipSupportPhase::Obstructed, SupportPhase::Obstructed},
	}};
	for (const auto& phase_case : cases) {
		const auto phase = phase_case.input;
		Fixture fixture;
		auto& support = fixture.observation->ships[0].support;
		support.phase = phase;
		support.episode_sequence = 23U;
		support.raw_hull_repair_work = 4.0F;
		support.raw_ammunition_rearm_work = 5U;
		support.raw_countermeasure_rearm_pool = -1;
		if (phase != ShipSupportPhase::None) {
			support.presence = SupportStatePresenceFlagSupportEntity;
			support.support_capture_key.value = 11U;
		}
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		const auto* record = atom(image, RecordType::SupportState, 9001U);
		ASSERT_NE(nullptr, record);
		EXPECT_EQ(ValidationError::None, validate_atom(*record));
		PacketReader reader({record->value.data(), record->value.size()});
		ASSERT_TRUE(reader.skip(24U));
		std::uint8_t decoded_phase = 0xffU;
		ASSERT_TRUE(reader.read_u8(decoded_phase));
		EXPECT_EQ(static_cast<std::uint8_t>(phase_case.wire), decoded_phase);
		std::uint8_t flags = 0U;
		ASSERT_TRUE(reader.read_u8(flags));
		EXPECT_EQ(static_cast<std::uint8_t>(support.raw_support_flags), flags);
		ASSERT_TRUE(reader.skip(3U));
		if (phase != ShipSupportPhase::None) {
			std::uint64_t support_id = 0U;
			ASSERT_TRUE(reader.read_u64(support_id));
			EXPECT_EQ(9001U, support_id); // self-repair is a valid closed reference.
		}
		EXPECT_TRUE(reader.at_end());
	}
	for (const auto pool : {-1, 0, 7}) {
		Fixture fixture;
		fixture.observation->ships[0].support.raw_countermeasure_rearm_pool =
			pool;
		StateImage image;
		EXPECT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
	}
	{
		Fixture fixture;
		auto& support = fixture.observation->ships[0].support;
		support.presence = SupportStatePresenceFlagSupportEntity;
		support.support_capture_key.value = 11U;
		support.phase = ShipSupportPhase::Repairing;
		support.episode_sequence = 1U;
		support.raw_support_repairs_hull_authorized = true;
		support.raw_hull_repair_applicable = true;
		support.raw_max_hull_repair_fraction = 1.0F;
		support.raw_max_subsystem_repair_fraction = 1.0F;
		support.raw_hull_repair_rate = 1.0F;
		support.raw_hull_repair_work = 1.0F;
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		EXPECT_FLOAT_EQ(1.0F, support.raw_hull_repair_rate);
		EXPECT_FLOAT_EQ(1.0F, support.raw_max_hull_repair_fraction);
		EXPECT_FLOAT_EQ(1.0F, support.raw_max_subsystem_repair_fraction);
		support.raw_mission_rearm_disallowed = true;
		support.raw_weapon_energy_rearm_applicable = false;
		support.raw_weapon_energy_rearm_work = 0.0F;
		EXPECT_TRUE(support.raw_mission_rearm_disallowed);
		EXPECT_FALSE(support.raw_weapon_energy_rearm_applicable);
		EXPECT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		support.raw_mission_rearm_disallowed = false;
		support.raw_weapon_rearm_disallowed = true;
		support.raw_ammunition_rearm_applicable = false;
		support.raw_ammunition_rearm_work = 0U;
		EXPECT_TRUE(support.raw_weapon_rearm_disallowed);
		EXPECT_FALSE(support.raw_ammunition_rearm_applicable);
		EXPECT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
	}
	reset_phase2_mission_observation_state();
	OnSupportTransition(
		11U, 12U, 0U, SupportTransitionReason::Queue, 100U);
	const auto queued = phase2_seam_handoff_snapshot().support_transition;
	OnSupportTransition(
		11U, 12U, 0U, SupportTransitionReason::OnWay, 101U);
	const auto on_way = phase2_seam_handoff_snapshot().support_transition;
	EXPECT_EQ(SupportTransitionReason::Queue, queued.reason);
	EXPECT_EQ(SupportTransitionReason::OnWay, on_way.reason);
	EXPECT_NE(0U, queued.episode_sequence);
	EXPECT_EQ(queued.episode_sequence, on_way.episode_sequence);
	reset_phase2_mission_observation_state();
}

TEST(Phase2LifecycleSupport, P2TST043TerminalRingLatchesAckCoalescingAndSixtyFourSixtyFiveAreBounded)
{
	Phase2Wp07SupportTerminalRing drained_ring;
	Phase2Wp07EpisodeLatches sessions;
	ASSERT_TRUE(sessions.activate_session(0U));
	ASSERT_TRUE(sessions.activate_session(1U));
	ASSERT_TRUE(drained_ring.record_terminal(
		{11U, 12U, 22U, SupportTransitionReason::Complete, 99U}));
	ASSERT_EQ(Phase2Wp07DrainStatus::Drained,
		drain_support_terminals_once(drained_ring, sessions));
	EXPECT_EQ(0U, drained_ring.size());
	EXPECT_TRUE(sessions.pending(0U, 11U));
	EXPECT_TRUE(sessions.pending(1U, 11U));
	EXPECT_EQ(22U, sessions.value(0U, 11U).episode_sequence);
	EXPECT_EQ(22U, sessions.value(1U, 11U).episode_sequence);
	EXPECT_EQ(Phase2Wp07DrainStatus::NoFacts,
		drain_support_terminals_once(drained_ring, sessions));

	Fixture fixture;
	ASSERT_TRUE(fixture.support.record_terminal(
		{11U, 12U, 23U, SupportTransitionReason::Complete, 100U}));
	ASSERT_TRUE(fixture.support.record_terminal(
		{11U, 12U, 23U, SupportTransitionReason::End, 101U}));
	EXPECT_EQ(2U, fixture.support.size())
		<< "The global ring retains accepted transitions; the per-slot latch owns coalescing.";
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	EXPECT_TRUE(fixture.latches.pending(0U));
	EXPECT_EQ(23U, fixture.latches.value(0U).episode_sequence);
	ASSERT_TRUE(fixture.latches.latch(1U, fixture.support.latest(11U)));
	EXPECT_FALSE(fixture.latches.on_applied(0U, 22U));
	EXPECT_TRUE(fixture.latches.on_applied(0U, 23U));
	EXPECT_FALSE(fixture.latches.pending(0U));
	EXPECT_TRUE(fixture.latches.pending(1U));
	ASSERT_TRUE(fixture.support.record_terminal(
		{11U, 13U, 24U, SupportTransitionReason::Broken, 102U}));
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, image));
	EXPECT_EQ(24U, fixture.latches.value(0U).episode_sequence);
	EXPECT_TRUE(fixture.latches.pending(1U));
	EXPECT_FALSE(fixture.latches.on_applied(0U, 11U, 23U));
	EXPECT_TRUE(fixture.latches.on_applied(0U, 11U, 24U));
	EXPECT_FALSE(fixture.latches.pending(0U, 11U));

	Phase2Wp07SupportTerminalRing bounded;
	for (std::uint32_t episode = 1U; episode <= 64U; ++episode)
		ASSERT_TRUE(bounded.record_terminal(
			{100U + episode, 12U, episode,
			 SupportTransitionReason::Complete, episode}));
	EXPECT_FALSE(bounded.record_terminal(
		{999U, 12U, 65U, SupportTransitionReason::End, 65U}));
	EXPECT_TRUE(bounded.overflowed());
	Fixture overflow;
	overflow.input.support_terminal_ring = &bounded;
	StateImage rejected;
	EXPECT_EQ(Phase2StateImageBuildStatus::CapacityExceeded,
		build_phase2_complete_domain(overflow.input, rejected));
	EXPECT_EQ(StateImage{}, rejected);
}

TEST(Phase2LifecycleSupport,
	P2TST043GlobalSupportRingBoundsNonterminalAndTerminalTransitionsTogether)
{
	reset_phase2_mission_observation_state();
	for (std::uint32_t assisted = 1U; assisted <= 32U; ++assisted) {
		OnSupportTransition(assisted, 1'000U + assisted, 0U,
			SupportTransitionReason::Queue, assisted);
	}
	EXPECT_EQ(32U, phase2_support_ring_depth())
		<< "Accepted nonterminal transitions share the bounded global ring.";
	EXPECT_FALSE(phase2_support_ring_overflowed());
	EXPECT_FALSE(phase2_wp07_seam_overflowed());

	for (std::uint32_t assisted = 1U; assisted <= 32U; ++assisted) {
		OnSupportTransition(assisted, 1'000U + assisted,
			1U,
			SupportTransitionReason::Complete,
			1'000U + assisted);
	}
	EXPECT_EQ(64U, phase2_support_ring_depth());
	EXPECT_FALSE(phase2_support_ring_overflowed());

	OnSupportTransition(1U, 1'001U, 1U,
		SupportTransitionReason::OnWay, 2'000U);
	EXPECT_EQ(64U, phase2_support_ring_depth())
		<< "Overflow never overwrites an accepted terminal.";
	EXPECT_TRUE(phase2_support_ring_overflowed());
	EXPECT_TRUE(phase2_wp07_seam_overflowed());

	Phase2CaptureDiagnostics accepted{};
	accepted.source_count = 1U;
	accepted.source_signatures[0] = 1U;
	Phase2Wp07GlobalEventBatch batch{};
	EXPECT_EQ(Phase2Wp07DrainStatus::RingOverflow,
		prepare_phase2_global_events(accepted, batch));
	EXPECT_EQ(0U, batch.support_count);
	EXPECT_EQ(64U, phase2_support_ring_depth());
	reset_phase2_mission_observation_state();
	EXPECT_EQ(0U, phase2_support_ring_depth());
	EXPECT_FALSE(phase2_support_ring_overflowed());
	EXPECT_FALSE(phase2_wp07_seam_overflowed());
}

TEST(Phase2LifecycleSupport,
	P2TST043BeginAndTerminalSupportTransitionsRemainInTheGlobalRing)
{
	reset_phase2_mission_observation_state();
	OnSupportTransition(11U, 12U, 0U,
		SupportTransitionReason::Queue, 100U);
	OnSupportTransition(11U, 12U, 0U,
		SupportTransitionReason::OnWay, 101U);
	OnSupportTransition(11U, 12U, 0U,
		SupportTransitionReason::Begin, 102U);
	OnSupportTransition(11U, 12U, 0U,
		SupportTransitionReason::Complete, 103U);
	ASSERT_EQ(4U, phase2_support_ring_depth());

	Phase2CaptureDiagnostics accepted{};
	accepted.source_count = 2U;
	accepted.source_signatures[0] = 11U;
	accepted.source_signatures[1] = 12U;
	Phase2Wp07GlobalEventBatch batch{};
	ASSERT_EQ(Phase2Wp07DrainStatus::Drained,
		prepare_phase2_global_events(accepted, batch));
	ASSERT_EQ(4U, batch.support_count);
	EXPECT_EQ(SupportTransitionReason::Queue, batch.support[0].reason);
	EXPECT_EQ(SupportTransitionReason::OnWay, batch.support[1].reason);
	EXPECT_EQ(SupportTransitionReason::Begin, batch.support[2].reason);
	EXPECT_EQ(SupportTransitionReason::Complete, batch.support[3].reason);
	EXPECT_EQ(batch.support[0].episode_sequence,
		batch.support[2].episode_sequence);
	EXPECT_EQ(batch.support[0].episode_sequence,
		batch.support[3].episode_sequence);
	EXPECT_EQ(4U, phase2_support_ring_depth())
		<< "Prepare remains non-destructive for nonterminal and terminal facts.";
	ASSERT_TRUE(commit_phase2_global_events(batch));
	EXPECT_EQ(0U, phase2_support_ring_depth());
	reset_phase2_mission_observation_state();
}

TEST(Phase2LifecycleSupport, P2TST044NonFiniteTimersAndInvalidSamplesLeaveImageAndRingsAtomic)
{
	float canonical = 1.0F;
	ASSERT_TRUE(canonicalize_phase2_float(-0.0F, 10.0F, canonical));
	EXPECT_FLOAT_EQ(0.0F, canonical);
	EXPECT_FALSE(std::signbit(canonical));
	std::uint64_t remaining = 1U;
	EXPECT_TRUE(phase2_timestamp_remaining_us(10, 9, 3'600'000'000ULL,
		remaining));
	EXPECT_EQ(0U, remaining);
	EXPECT_TRUE(phase2_timestamp_remaining_us(10, 3'600'010,
		3'600'000'000ULL, remaining));
	EXPECT_EQ(3'600'000'000ULL, remaining);
	EXPECT_FALSE(phase2_timestamp_remaining_us(10, 3'600'011,
		3'600'000'000ULL, remaining));
	for (const auto invalid : {std::numeric_limits<float>::quiet_NaN(),
			 std::numeric_limits<float>::infinity(),
			 -std::numeric_limits<float>::infinity()}) {
		Fixture fixture;
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		const auto sentinel = image;
		fixture.observation->ships[0].support.raw_hull_repair_work = invalid;
		EXPECT_NE(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		EXPECT_EQ(sentinel, image);
		EXPECT_EQ(0U, fixture.support.size());
	}
}

TEST(Phase2LifecycleSupport, P2TST046CleanupMappingFenceAndRespawnNewIdIntentAreOrdered)
{
	const std::array<std::pair<ShipCleanupMode, LifecyclePhase>, 3U> cases{{
		{ShipCleanupMode::Destroyed, LifecyclePhase::Destroyed},
		{ShipCleanupMode::Departed, LifecyclePhase::Departing},
		{ShipCleanupMode::Vanished, LifecyclePhase::Removed},
	}};
	for (const auto& cleanup_case : cases) {
		Fixture fixture;
		ASSERT_TRUE(fixture.cleanup.record(
			{11U, cleanup_case.first}, 1'000U));
		const auto intent = fixture.cleanup.latest();
		EXPECT_EQ(cleanup_case.first, intent.mode);
		EXPECT_TRUE(intent.event_ready);
		EXPECT_TRUE(intent.fence_ready);
		EXPECT_LT(intent.event_order, intent.purge_order);
		EXPECT_TRUE(intent.new_entity_id_request);
		EXPECT_NE(0U, intent.replacement_entity_id);
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, image));
		EXPECT_EQ(0U, fixture.cleanup.size());
		const auto* lifecycle =
			atom(image, RecordType::EntityLifecycle, 9001U);
		ASSERT_NE(nullptr, lifecycle);
		PacketReader reader(
			{lifecycle->value.data(), lifecycle->value.size()});
		ASSERT_TRUE(reader.skip(25U));
		std::uint8_t phase = 0xffU;
		ASSERT_TRUE(reader.read_u8(phase));
		EXPECT_EQ(static_cast<std::uint8_t>(cleanup_case.second), phase);
	}
	Phase2Wp07CleanupRing closure_ring;
	ASSERT_TRUE(closure_ring.record(
		{77U, ShipCleanupMode::Destroyed}, 900U));
	ASSERT_TRUE(closure_ring.record(
		{11U, ShipCleanupMode::Vanished}, 1'000U)); // red-alert store removes.
	const std::array<std::uint32_t, 1U> closure{{11U}};
	Phase2Wp07CleanupBatch batch;
	ASSERT_EQ(Phase2Wp07DrainStatus::Drained,
		drain_cleanup_once(
			closure_ring, closure.data(), closure.size(), batch));
	EXPECT_EQ(0U, closure_ring.size());
	ASSERT_EQ(1U, batch.count);
	EXPECT_EQ(11U, batch.intents[0].object_signature);
	EXPECT_EQ(ShipCleanupMode::Vanished, batch.intents[0].mode);
	EXPECT_TRUE(batch.intents[0].event_ready);
	EXPECT_TRUE(batch.intents[0].fence_ready);
	EXPECT_LT(batch.intents[0].event_order,
		batch.intents[0].purge_order);
	EXPECT_TRUE(batch.intents[0].new_entity_id_request);
	EXPECT_NE(0U, batch.intents[0].replacement_entity_id);
	auto consumed = std::make_unique<Fixture>();
	consumed->input.cleanup_ring = nullptr;
	consumed->input.cleanup_batch = &batch;
	StateImage consumed_image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(consumed->input, consumed_image));
	const auto* red_alert =
		atom(consumed_image, RecordType::EntityLifecycle, 9001U);
	ASSERT_NE(nullptr, red_alert);
	PacketReader red_alert_reader(
		{red_alert->value.data(), red_alert->value.size()});
	ASSERT_TRUE(red_alert_reader.skip(25U));
	std::uint8_t red_alert_phase = 0xffU;
	ASSERT_TRUE(red_alert_reader.read_u8(red_alert_phase));
	EXPECT_EQ(static_cast<std::uint8_t>(LifecyclePhase::Removed),
		red_alert_phase);

	auto invalid_batch = batch;
	invalid_batch.intents[0].fence_ready = false;
	auto invalid = std::make_unique<Fixture>();
	invalid->input.cleanup_ring = nullptr;
	invalid->input.cleanup_batch = &invalid_batch;
	StateImage invalid_image;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(invalid->input, invalid_image));
	EXPECT_EQ(StateImage{}, invalid_image);

	Phase2Wp07CleanupRing combined_ring;
	ASSERT_TRUE(combined_ring.record(
		{11U, ShipCleanupMode::Destroyed}, 1'000U));
	ASSERT_TRUE(combined_ring.record(
		{11U, ShipCleanupMode::Departed}, 1'001U));
	Phase2Wp07CleanupBatch combined_batch;
	auto combined = std::make_unique<Fixture>();
	StateImage combined_image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(combined->input, combined_image));
	const auto combined_image_sentinel = combined_image;
	EXPECT_EQ(Phase2Wp07DrainStatus::InvalidInput,
		drain_cleanup_once(combined_ring, closure.data(), closure.size(),
			combined_batch));
	EXPECT_EQ(2U, combined_ring.size());
	EXPECT_EQ(0U, combined_batch.count);
	EXPECT_EQ(combined_image_sentinel, combined_image);

	reset_phase2_mission_observation_state();
	auto hook_fixture = std::make_unique<Fixture>();
	StateImage hook_image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(hook_fixture->input, hook_image));
	const auto hook_image_sentinel = hook_image;
	OnShipCleanup(
		11U, static_cast<ShipCleanupMode>(0xffU));
	EXPECT_TRUE(phase2_wp07_seam_overflowed());
	EXPECT_FALSE(phase2_seam_handoff_snapshot().has_ship_cleanup);
	EXPECT_EQ(hook_image_sentinel, hook_image);
	reset_phase2_mission_observation_state();
	EXPECT_FALSE(phase2_wp07_seam_overflowed());
	EXPECT_FALSE(phase2_seam_handoff_snapshot().has_ship_cleanup);

	Phase2Wp07CleanupRing identities;
	std::uint64_t prior = 0U;
	for (const auto& cleanup_case : cases) {
		ASSERT_TRUE(identities.record({77U, cleanup_case.first}, 1'000U));
		EXPECT_GT(identities.latest().replacement_entity_id, prior);
		prior = identities.latest().replacement_entity_id;
	}
	auto outside = std::make_unique<Fixture>();
	StateImage baseline;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(outside->input, baseline));
	ASSERT_TRUE(outside->cleanup.record(
		{77U, ShipCleanupMode::Destroyed}, 2'000U));
	StateImage ignored;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(outside->input, ignored));
	EXPECT_EQ(baseline, ignored);
	EXPECT_FALSE(outside->cleanup.record(
		{77U, static_cast<ShipCleanupMode>(0xffU)}, 2'000U));
}

TEST(Phase2LifecycleSupport, P2TST066PauseCompressionMenuAndObserverStayTransactional)
{
	Fixture fixture;
	fixture.input.mission.paused = true;
	fixture.input.mission.time_compression = 4.0F;
	StateImage paused;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, paused));
	ASSERT_NE(nullptr, atom(paused, RecordType::MissionState));
	EXPECT_EQ(ValidationError::None,
		validate_atom(*atom(paused, RecordType::MissionState)));

	fixture.input.mission.paused = false;
	fixture.input.mission.time_compression = 0.25F;
	StateImage compressed;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, compressed));
	EXPECT_NE(*atom(paused, RecordType::MissionState),
		*atom(compressed, RecordType::MissionState));

	const auto sentinel = compressed;
	fixture.observation->capture = {
		Phase2CaptureStatus::NoPlayer,
		Phase2CaptureReason::NotInMission};
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, compressed));
	EXPECT_EQ(sentinel, compressed)
		<< "Menu/inactive mission cannot partially replace the last valid image.";

	fixture.observation->capture = {
		Phase2CaptureStatus::UnsupportedEngineState,
		Phase2CaptureReason::UnsupportedShipBlock};
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, compressed));
	EXPECT_EQ(sentinel, compressed)
		<< "Observer/unsupported state remains closed without dereferencing a partial source.";
}

TEST(Phase2LifecycleSupport, P2TST070K1K2OrderOmissionsAndReadyPoolNoGrowthAreDeterministic)
{
	Fixture fixture;
	StateImage k1;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, k1));
	EXPECT_EQ(14U, k1.records().size());
	expect_complete_atoms(k1, {{9001U, 0U}}, 1U);
	EXPECT_TRUE(std::is_sorted(k1.records().begin(), k1.records().end(),
		[](const auto& left, const auto& right) { return left.key < right.key; }));
	const auto k1_hash = fixture.input.canonical_hash(k1);

	fixture.observation->ships.resize(2U);
	fixture.add_ship(1U, 12U, 101U, 9002U);
	fixture.input.subject_count = 2U;
	StateImage k2;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, k2));
	EXPECT_EQ(24U, k2.records().size());
	expect_complete_atoms(k2, {{9001U, 9002U}}, 2U);
	std::reverse(fixture.subjects.begin(), fixture.subjects.begin() + 2U);
	StateImage permuted;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain(fixture.input, permuted));
	EXPECT_EQ(k2, permuted);
	EXPECT_NE(k1_hash, fixture.input.canonical_hash(k2));
	const std::array<std::uint64_t, 2U> k2_entities{{9001U, 9002U}};
	for (const auto type : {RecordType::EntityLifecycle,
			 RecordType::ShipIdentity,
			 RecordType::FlightState, RecordType::DamageState,
			 RecordType::ShieldState, RecordType::EnergyState,
			 RecordType::PropulsionState, RecordType::WeaponState,
			 RecordType::DockingState, RecordType::SupportState}) {
		fixture.input.omit_record_for_test = type;
		const auto sentinel = k2;
		EXPECT_NE(Phase2StateImageBuildStatus::Created,
			build_phase2_complete_domain(fixture.input, k2));
		EXPECT_EQ(sentinel, k2);

		auto records = sentinel.records();
		records.erase(std::remove_if(records.begin(), records.end(),
			[type](const auto& value) {
				return value.key.record_type ==
					static_cast<std::uint16_t>(type);
			}), records.end());
		if (type == RecordType::EntityLifecycle)
			for (auto& record : records) {
				record.has_cascade_owner = false;
				record.cascade_owner = {};
			}
		StateImage incomplete;
		ASSERT_EQ(StateImageResult::Created,
			StateImage::create(std::move(records), incomplete));
		auto context = complete_validation_context(
			*fixture.manifest, k2_entities.data(),
			k2_entities.size());
		BusinessStateImageValidator validator(context);
		EXPECT_EQ(ValidationError::InvalidAbsence,
			validator.validate(incomplete))
			<< static_cast<std::uint16_t>(type);
	}
	fixture.input.omit_record_for_test.reset();

	fixture.observation->ships.resize(64U);
	for (std::size_t ship = 0U; ship < 64U; ++ship) {
		if (ship >= 2U)
			fixture.add_ship(ship,
				static_cast<std::uint32_t>(11U + ship),
				101U, 9001U + ship);
		auto& source = fixture.observation->ships[ship];
		source.subsystems.count = 64U;
		for (std::size_t subsystem = 0U;
			 subsystem < 64U; ++subsystem) {
			auto& value = source.subsystems.values[subsystem];
			value = {};
			value.source_key.value =
				static_cast<std::uint32_t>(1'000U + subsystem);
			value.sample_time_us =
				fixture.observation->producer_sample_time_us;
			value.kind = ShipSubsystemKind::Generic;
		}
	}
	fixture.input.subject_count = 64U;
	auto& common_class = fixture.manifest->class_records[0];
	common_class.subsystem_count = 64U;
	common_class.subsystems = {
		fixture.manifest->subsystem_records.data(), 64U};
	for (std::size_t subsystem = 0U;
		 subsystem < 64U; ++subsystem)
		fixture.manifest->subsystem_records[subsystem] = {
			static_cast<std::uint32_t>(1'000U + subsystem),
			static_cast<std::uint32_t>(2'000U + subsystem),
			static_cast<std::uint32_t>(subsystem), 0U};
	fixture.manifest->aggregate_subsystem_count = 64U;

	Phase2CompleteDomainPool pool;
	ASSERT_TRUE(pool.provision(64U, 4096U, 64U, 64U));
	StateImage maximum;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, maximum));
	ASSERT_EQ(4U + 10U * 64U + 4096U,
		maximum.records().size());
	const auto bytes = pool.owned_backing_bytes();
	telemetry::test::wp03::GlobalAllocationScope allocations;
	StateImage measured;
	EXPECT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, measured));
	EXPECT_EQ(0U, allocations.finish());
	EXPECT_EQ(bytes, pool.owned_backing_bytes());
	EXPECT_EQ(maximum, measured);

	fixture.input.subject_count = 65U;
	StateImage k65;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, k65));
	EXPECT_EQ(StateImage{}, k65);
	fixture.input.subject_count = 64U;

	fixture.manifest->class_record_count = 2U;
	auto& last_ship = fixture.observation->ships[63U];
	last_ship.identity.class_source_key.value = 102U;
	last_ship.subsystems.count = 65U;
	auto& extra = last_ship.subsystems.values[64U];
	extra = {};
	extra.source_key.value = 3'064U;
	extra.sample_time_us =
		fixture.observation->producer_sample_time_us;
	extra.kind = ShipSubsystemKind::Generic;
	auto& second_class = fixture.manifest->class_records[1U];
	second_class.source_key = 102U;
	second_class.class_id = 502U;
	second_class.subsystem_count = 65U;
	second_class.subsystems = {
		fixture.manifest->subsystem_records.data() + 64U, 65U};
	for (std::size_t subsystem = 0U;
		 subsystem < 65U; ++subsystem)
		fixture.manifest->subsystem_records[64U + subsystem] = {
			static_cast<std::uint32_t>(3'000U + subsystem),
			static_cast<std::uint32_t>(4'000U + subsystem),
			static_cast<std::uint32_t>(subsystem), 0U};
	fixture.manifest->aggregate_subsystem_count = 129U;
	StateImage n4097;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, n4097));
	EXPECT_EQ(StateImage{}, n4097);
}

TEST(Phase2LifecycleSupport,
	P2REQ019030046PreallocatedFastPathGrowsToSupportAndPublishesRepairingThenRearming)
{
	Fixture fixture;
	Phase2CompleteDomainPool pool;
	ASSERT_TRUE(pool.provision(2U, 0U, 0U, 2U));
	const auto backing_bytes = pool.owned_backing_bytes();

	StateImage one_ship;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, one_ship));
	expect_complete_atoms(one_ship, {{9001U, 0U}}, 1U);

	fixture.observation->ships.resize(2U);
	fixture.add_ship(1U, 12U, 101U, 9002U);
	fixture.input.subject_count = 2U;
	auto& root_support = fixture.observation->ships[0].support;
	root_support.presence = SupportStatePresenceFlagSupportEntity;
	root_support.support_capture_key.value = 12U;
	root_support.phase = ShipSupportPhase::Repairing;
	root_support.episode_sequence = 1U;

	StateImage repairing;
	StateImage rearming;
	telemetry::test::wp03::GlobalAllocationScope allocations;
	const auto repairing_status =
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, repairing);
	root_support.phase = ShipSupportPhase::Rearming;
	fixture.observation->producer_sample_time_us += 100'000U;
	root_support.sample_time_us =
		fixture.observation->producer_sample_time_us;
	const auto rearming_status =
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, rearming);
	const auto allocation_count = allocations.finish();

	ASSERT_EQ(Phase2StateImageBuildStatus::Created, repairing_status)
		<< "A valid 1->2 ship CompleteShip closure must not report "
		   "CapacityExceeded and become PermanentCaptureFailure.";
	ASSERT_EQ(Phase2StateImageBuildStatus::Created, rearming_status)
		<< "Repairing->Rearming on a stable preallocated closure must not "
		   "report CapacityExceeded and become PermanentCaptureFailure.";
	EXPECT_EQ(0U, allocation_count);
	EXPECT_EQ(backing_bytes, pool.owned_backing_bytes());
	expect_complete_atoms(repairing, {{9001U, 9002U}}, 2U);
	expect_complete_atoms(rearming, {{9001U, 9002U}}, 2U);

	const auto expect_support = [](const StateImage& image,
		SupportPhase expected_phase) {
		const auto* record =
			atom(image, RecordType::SupportState, 9001U);
		ASSERT_NE(nullptr, record);
		ASSERT_EQ(ValidationError::None, validate_atom(*record));
		PacketReader reader(
			{record->value.data(), record->value.size()});
		std::uint64_t entity = 0U;
		std::uint64_t presence = 0U;
		std::uint64_t sample_time = 0U;
		std::uint64_t support_entity = 0U;
		std::uint8_t phase = 0xffU;
		std::uint8_t flags = 0xffU;
		ASSERT_TRUE(reader.read_u64(entity));
		ASSERT_TRUE(reader.read_u64(presence));
		ASSERT_TRUE(reader.read_u64(sample_time));
		ASSERT_TRUE(reader.read_u8(phase));
		ASSERT_TRUE(reader.read_u8(flags));
		ASSERT_TRUE(reader.skip(3U));
		ASSERT_TRUE(reader.read_u64(support_entity));
		EXPECT_TRUE(reader.at_end());
		EXPECT_EQ(9001U, entity);
		EXPECT_EQ(SupportStatePresenceFlagSupportEntity, presence);
		EXPECT_EQ(static_cast<std::uint8_t>(expected_phase), phase);
		EXPECT_EQ(0U, flags);
		EXPECT_EQ(9002U, support_entity);
	};
	expect_support(repairing, SupportPhase::Repairing);
	expect_support(rearming, SupportPhase::Rearming);
}

TEST(Phase2LifecycleSupport,
	P2REQ019030046RetainedStatePreservesEveryNonDueAtomAcrossSystemsAndFlightTicks)
{
	Fixture fixture;
	Phase2CompleteDomainPool pool;
	ASSERT_TRUE(pool.provision(2U, 0U, 0U, 2U));
	const auto backing_bytes = pool.owned_backing_bytes();

	StateImage one_ship;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, one_ship));

	fixture.observation->ships.resize(2U);
	fixture.add_ship(1U, 12U, 101U, 9002U);
	fixture.input.subject_count = 2U;
	auto& root = fixture.observation->ships[0];
	root.support.presence =
		SupportStatePresenceFlagSupportEntity;
	root.support.support_capture_key.value = 12U;
	root.support.phase = ShipSupportPhase::Repairing;
	root.support.episode_sequence = 1U;
	StateImage repairing;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, repairing));

	const auto expect_exact_atom = [](const StateImage& expected,
		const StateImage& actual, RecordType type,
		std::uint64_t entity) {
		const auto* expected_atom = atom(expected, type, entity);
		const auto* actual_atom = atom(actual, type, entity);
		ASSERT_NE(nullptr, expected_atom);
		ASSERT_NE(nullptr, actual_atom);
		EXPECT_EQ(expected_atom->key, actual_atom->key);
		EXPECT_EQ(expected_atom->record_version,
			actual_atom->record_version);
		EXPECT_EQ(expected_atom->lifecycle,
			actual_atom->lifecycle);
		EXPECT_EQ(expected_atom->has_cascade_owner,
			actual_atom->has_cascade_owner);
		EXPECT_EQ(expected_atom->cascade_owner,
			actual_atom->cascade_owner);
		EXPECT_EQ(expected_atom->value, actual_atom->value);
	};

	root.support.phase = ShipSupportPhase::Rearming;
	fixture.observation->producer_sample_time_us += 100'000U;
	root.support.sample_time_us =
		fixture.observation->producer_sample_time_us;
	fixture.input.retained_state = &repairing;
	fixture.input.refresh_flight_controls = false;
	fixture.input.refresh_systems = true;
	StateImage systems_only;
	StateImage flight_only;
	telemetry::test::wp03::GlobalAllocationScope allocations;
	const auto systems_status =
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, systems_only);

	root.flight.position_world[0] += 1.0F;
	root.flight.sample_time_us =
		fixture.observation->producer_sample_time_us + 100'000U;
	fixture.observation->player_controls.pitch = 0.5F;
	fixture.observation->player_controls.sample_time_us =
		root.flight.sample_time_us;
	fixture.observation->producer_sample_time_us =
		root.flight.sample_time_us;
	fixture.input.retained_state = &systems_only;
	fixture.input.refresh_flight_controls = true;
	fixture.input.refresh_systems = false;
	const auto flight_status =
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, flight_only);
	const auto allocation_count = allocations.finish();

	ASSERT_EQ(Phase2StateImageBuildStatus::Created, systems_status);
	ASSERT_EQ(Phase2StateImageBuildStatus::Created, flight_status);
	EXPECT_EQ(0U, allocation_count);
	EXPECT_EQ(backing_bytes, pool.owned_backing_bytes());
	for (const auto entity : {9001U, 9002U})
		expect_exact_atom(repairing, systems_only,
			RecordType::FlightState, entity);
	expect_exact_atom(repairing, systems_only,
		RecordType::ControlState, 9001U);
	for (const auto entity : {9001U, 9002U})
		for (const auto type : {
				 RecordType::EntityLifecycle,
				 RecordType::ShipIdentity,
				 RecordType::DamageState,
				 RecordType::ShieldState,
				 RecordType::EnergyState,
				 RecordType::PropulsionState,
				 RecordType::WeaponState,
				 RecordType::DockingState,
				 RecordType::SupportState})
			expect_exact_atom(systems_only, flight_only,
				type, entity);
	expect_exact_atom(systems_only, flight_only,
		RecordType::CargoScanState, 9001U);

	const auto* support =
		atom(systems_only, RecordType::SupportState, 9001U);
	ASSERT_NE(nullptr, support);
	PacketReader reader(
		{support->value.data(), support->value.size()});
	std::uint64_t ignored = 0U;
	std::uint8_t phase = 0xffU;
	ASSERT_TRUE(reader.read_u64(ignored));
	ASSERT_TRUE(reader.read_u64(ignored));
	ASSERT_TRUE(reader.read_u64(ignored));
	ASSERT_TRUE(reader.read_u8(phase));
	EXPECT_EQ(static_cast<std::uint8_t>(
		SupportPhase::Rearming), phase);
}

TEST(Phase2LifecycleSupport,
	P2REQ019030046BothRefreshesAreIdenticalWithAndWithoutRetainedState)
{
	Fixture fixture;
	fixture.observation->ships.resize(2U);
	fixture.add_ship(1U, 12U, 101U, 9002U);
	fixture.input.subject_count = 2U;
	auto& support = fixture.observation->ships[0].support;
	support.presence = SupportStatePresenceFlagSupportEntity;
	support.support_capture_key.value = 12U;
	support.phase = ShipSupportPhase::Rearming;
	support.episode_sequence = 1U;
	fixture.input.refresh_flight_controls = true;
	fixture.input.refresh_systems = true;

	Phase2CompleteDomainPool pool;
	ASSERT_TRUE(pool.provision(2U, 0U, 0U, 2U));
	const auto backing_bytes = pool.owned_backing_bytes();
	StateImage without_retained;
	StateImage with_retained;
	telemetry::test::wp03::GlobalAllocationScope allocations;
	const auto without_status =
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, without_retained);
	fixture.input.retained_state = &without_retained;
	const auto with_status =
		build_phase2_complete_domain_preallocated(
			fixture.input, pool, with_retained);
	const auto allocation_count = allocations.finish();

	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		without_status);
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		with_status);
	EXPECT_EQ(without_retained, with_retained);
	EXPECT_EQ(fixture.input.canonical_hash(without_retained),
		fixture.input.canonical_hash(with_retained));
	EXPECT_EQ(0U, allocation_count);
	EXPECT_EQ(backing_bytes, pool.owned_backing_bytes());
}

} // namespace
