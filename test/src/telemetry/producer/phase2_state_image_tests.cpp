#include "telemetry/phase2_state_image.h"
#include "telemetry/phase2_allocation_tracker.h"
#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_business_state_validation.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

namespace {

using namespace telemetry;
using namespace telemetry::detail;
using namespace telemetry::protocol;

static_assert(Phase2CoreGateCoverage == 0x0401ULL,
	"P2-REQ-004 freezes CoreGate to PLAYER_KINEMATICS | CORE_SHIP.");
static_assert(noexcept(phase2_core_gate_record_count(std::size_t{0})));

struct CoreFixture {
	std::unique_ptr<Phase2ObservationDto> observation;
	std::unique_ptr<Phase2ManifestCandidate> manifest;
	Phase2CoreGateStateImageInput input;

	explicit CoreFixture(std::size_t subsystem_count = 1U) :
		observation(std::make_unique<Phase2ObservationDto>()),
		manifest(std::make_unique<Phase2ManifestCandidate>())
	{
		observation->capture = {Phase2CaptureStatus::Valid, Phase2CaptureReason::None};
		observation->player_key.value = 55U;
		observation->producer_sample_time_us = 1'000'000U;
		observation->ships.resize(1U);
		auto& ship = observation->ships[0];
		ship.capture_key.value = 55U;
		EXPECT_TRUE(ship.identity.internal_name.assign("ship"));
		ship.identity.class_source_key.value = 41U;
		ship.identity.sample_time_us = observation->producer_sample_time_us;
		ship.lifecycle.sample_time_us = observation->producer_sample_time_us;
		ship.flight.sample_time_us = observation->producer_sample_time_us;
		ship.flight.radius = 7.0F;
		ship.damage.sample_time_us = observation->producer_sample_time_us;
		ship.damage.hull_current = 50.0F;
		ship.damage.hull_maximum = 100.0F;
		ship.shields.sample_time_us = observation->producer_sample_time_us;
		ship.energy.sample_time_us = observation->producer_sample_time_us;
		ship.propulsion.sample_time_us = observation->producer_sample_time_us;
		ship.subsystems.count = static_cast<std::uint16_t>(subsystem_count);

		manifest->manifest_id = 1U;
		manifest->kind = ManifestKind::FullRequired;
		manifest->class_record_count = 1U;
		auto& ship_class = manifest->class_records[0];
		ship_class.source_key = 41U;
		ship_class.class_id = 7U;
		ship_class.species_id = 11U;
		ship_class.iff_id = 13U;
		ship_class.armor_id = 17U;
		ship_class.subsystem_count = static_cast<std::uint32_t>(subsystem_count);
		ship_class.subsystems = {manifest->subsystem_records.data(),
			static_cast<std::uint32_t>(subsystem_count)};
		for (std::size_t index = 0U; index < subsystem_count; ++index) {
			auto& source = ship.subsystems.values[index];
			source.source_key.value = static_cast<std::uint32_t>(100U + index);
			source.sample_time_us = observation->producer_sample_time_us;
			source.kind = index == 0U ? ShipSubsystemKind::Engine : ShipSubsystemKind::Generic;
			auto& definition = manifest->subsystem_records[index];
			definition.source_key = source.source_key.value;
			definition.subsystem_id = static_cast<std::uint32_t>(1001U + index);
			definition.canonical_index = static_cast<std::uint32_t>(index);
			definition.armor_id = index == 0U ? 17U : 0U;
		}

		input.producer_id = 0x0102030405060708ULL;
		input.negotiated_capability_generation = 1U;
		input.session_phase = SessionPhase::Live;
		input.mission.producer_sample_time_us = observation->producer_sample_time_us;
		input.mission.mission_generation = 1U;
		input.mission.phase = MissionPhase::None;
		input.mission.time_compression = 1.0F;
		input.player_entity_id = 1U;
		input.observation = observation.get();
		input.installed_manifest = manifest.get();
		input.required_manifest_id = 1U;
		input.manifest_applied = true;
	}

	ShipObservationDto& ship() { return observation->ships[0]; }
};

const StateAtom* atom(const StateImage& image, RecordType type)
{
	const auto raw = static_cast<std::uint16_t>(type);
	const auto found = std::find_if(image.records().begin(), image.records().end(),
		[raw](const auto& value) { return value.key.record_type == raw; });
	return found == image.records().end() ? nullptr : &*found;
}

std::vector<const StateAtom*> atoms(const StateImage& image, RecordType type)
{
	std::vector<const StateAtom*> result;
	for (const auto& value : image.records()) {
		if (value.key.record_type == static_cast<std::uint16_t>(type)) result.push_back(&value);
	}
	return result;
}

BusinessStateValidationContext validation_context(std::uint32_t class_id,
	const std::uint32_t* subsystem_ids,
	std::size_t subsystem_count)
{
	static BusinessClassCatalogEntry catalog;
	catalog = {class_id, subsystem_ids, subsystem_count};
	BusinessStateValidationContext context{};
	context.protocol_minor = VersionMinor;
	context.required_manifest_id = 1U;
	context.class_manifest_installed = true;
	context.weapon_manifest_installed = true;
	context.class_catalog = &catalog;
	context.class_catalog_count = 1U;
	return context;
}

TEST(Phase2StateImage, CoreGateBuildsCanonicalNinePlusNAndNoForbiddenRecords)
{
	for (const std::size_t subsystem_count : {0U, 1U, 1024U}) {
		EXPECT_EQ(9U + subsystem_count, phase2_core_gate_record_count(subsystem_count));
	}

	CoreFixture fixture(1U);
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_core_gate_state_image(fixture.input, image));
	ASSERT_EQ(10U, image.records().size());
	const std::array<std::uint16_t, 10> expected{{
		1U, 2U, 5U, 6U, 7U, 9U, 10U, 11U, 12U, 13U,
	}};
	for (std::size_t index = 0U; index < expected.size(); ++index) {
		EXPECT_EQ(expected[index], image.records()[index].key.record_type);
	}
	for (const auto forbidden : {RecordType::ControlState, RecordType::WeaponState,
		     RecordType::CargoScanState, RecordType::DockingState, RecordType::SupportState,
		     RecordType::LockState, RecordType::TargetState, RecordType::RadarState}) {
		EXPECT_EQ(nullptr, atom(image, forbidden));
	}
	for (const auto& value : image.records()) {
		EXPECT_EQ(1U, value.record_version);
	}
	ASSERT_NE(nullptr, atom(image, RecordType::EntityLifecycle));
	EXPECT_EQ(StateRecordLifecycle::ExplicitCreateDelete,
		atom(image, RecordType::EntityLifecycle)->lifecycle);
	EXPECT_EQ(StateRecordLifecycle::ExplicitCreateDelete,
		atom(image, RecordType::SubsystemState)->lifecycle);
}

TEST(Phase2StateImage, PublicManifestLookupsReachIdentityDamageAndSubsystemWire)
{
	CoreFixture fixture(1U);
	auto& ship = fixture.ship();
	ship.damage.presence = DamageStatePresenceFlagArmor;
	ship.damage.armor_source_key.value = 701U;
	ship.subsystems.values[0].presence = SubsystemStatePresenceFlagArmor;
	ship.subsystems.values[0].armor_source_key.value = 702U;
	fixture.manifest->auxiliary_record_count = 2U;
	fixture.manifest->auxiliary_records[0] =
		{AuxiliaryRegistry::Armor, 701U, 29U};
	fixture.manifest->auxiliary_records[1] =
		{AuxiliaryRegistry::Armor, 702U, 31U};
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_core_gate_state_image(fixture.input, image));

	PacketReader identity({atom(image, RecordType::ShipIdentity)->value.data(),
		atom(image, RecordType::ShipIdentity)->value.size()});
	std::uint64_t u64 = 0U;
	std::uint32_t u32 = 0U;
	std::uint16_t u16 = 0U;
	float f32 = 0.0F;
	std::string_view name;
	ASSERT_TRUE(identity.read_u64(u64) && identity.read_u64(u64) && identity.read_u64(u64));
	ASSERT_TRUE(identity.read_u32(u32));
	EXPECT_EQ(7U, u32);
	ASSERT_TRUE(identity.read_utf8(255U, name));
	ASSERT_TRUE(identity.read_u32(u32));
	EXPECT_EQ(11U, u32);
	ASSERT_TRUE(identity.read_u32(u32) && identity.read_u32(u32));
	EXPECT_EQ(13U, u32);
	ASSERT_TRUE(identity.read_u16(u16) && identity.read_f32(f32));
	EXPECT_TRUE(identity.at_end());

	PacketReader damage({atom(image, RecordType::DamageState)->value.data(),
		atom(image, RecordType::DamageState)->value.size()});
	ASSERT_TRUE(damage.skip(8U + 8U + 8U + 4U + 4U + 2U));
	ASSERT_TRUE(damage.read_u32(u32));
	EXPECT_EQ(29U, u32);
	EXPECT_TRUE(damage.at_end());

	const auto* subsystem = atom(image, RecordType::SubsystemState);
	PacketReader subsystem_reader({subsystem->value.data(), subsystem->value.size()});
	ASSERT_TRUE(subsystem_reader.read_u64(u64) && subsystem_reader.read_u32(u32));
	EXPECT_EQ(1001U, u32);
	ASSERT_TRUE(subsystem_reader.skip(8U + 8U + 2U + 1U + 4U + 4U + 4U));
	ASSERT_TRUE(subsystem_reader.read_u32(u32));
	EXPECT_EQ(31U, u32);
	EXPECT_TRUE(subsystem_reader.at_end());
}

TEST(Phase2StateImage, InvalidInputsAreAtomicAndMandatoryOmissionsFailClosed)
{
	CoreFixture fixture(1U);
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_core_gate_state_image(fixture.input, image));
	const auto sentinel = image;
	fixture.input.manifest_applied = false;
	EXPECT_EQ(Phase2StateImageBuildStatus::ManifestUnavailable,
		build_phase2_core_gate_state_image(fixture.input, image));
	EXPECT_EQ(sentinel, image);
	fixture.input.manifest_applied = true;
	fixture.ship().identity.class_source_key.value = 999U;
	EXPECT_EQ(Phase2StateImageBuildStatus::SourceMappingMissing,
		build_phase2_core_gate_state_image(fixture.input, image));
	EXPECT_EQ(sentinel, image);

	const std::array<RecordType, 7> mandatory{{RecordType::ShipIdentity, RecordType::FlightState,
		RecordType::DamageState, RecordType::ShieldState, RecordType::SubsystemState,
		RecordType::EnergyState, RecordType::PropulsionState}};
	const std::uint32_t subsystem_id = 1001U;
	for (const auto omitted : mandatory) {
		auto records = sentinel.records();
		records.erase(std::remove_if(records.begin(), records.end(), [omitted](const auto& value) {
			return value.key.record_type == static_cast<std::uint16_t>(omitted);
		}), records.end());
		StateImage incomplete;
		ASSERT_EQ(StateImageResult::Created, StateImage::create(std::move(records), incomplete));
		auto context = validation_context(7U, &subsystem_id, 1U);
		BusinessStateImageValidator validator(context);
		EXPECT_EQ(ValidationError::InvalidAbsence, validator.validate(incomplete))
			<< static_cast<std::uint16_t>(omitted);
	}
}

TEST(Phase2StateImage, P2TST030DamageAndP2TST031032ShieldBoundaries)
{
	CoreFixture fixture(0U);
	fixture.ship().damage.hull_current = -25.0F;
	fixture.ship().damage.protection_flags =
		ProtectionFlagInvulnerable | ProtectionFlagProtected | ProtectionFlagGuardian;
	fixture.ship().damage.presence = DamageStatePresenceFlagGuardian;
	fixture.ship().damage.guardian_threshold = 100.0F;
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_core_gate_state_image(fixture.input, image))
		<< "P2-TST-030 requires ordinary negative overkill to canonicalize to +0.";
	PacketReader damage({atom(image, RecordType::DamageState)->value.data(),
		atom(image, RecordType::DamageState)->value.size()});
	std::uint64_t u64 = 0U;
	float hull = 1.0F;
	float maximum = 0.0F;
	std::uint16_t protection = 0U;
	ASSERT_TRUE(damage.read_u64(u64) && damage.read_u64(u64) && damage.read_u64(u64));
	ASSERT_TRUE(damage.read_f32(hull) && damage.read_f32(maximum) && damage.read_u16(protection));
	EXPECT_EQ(0.0F, hull);
	EXPECT_FALSE(std::signbit(hull));
	EXPECT_EQ(100.0F, maximum);
	EXPECT_EQ(fixture.ship().damage.protection_flags, protection);

	for (const std::uint8_t count : {1U, 4U, 64U}) {
		CoreFixture shield_fixture(0U);
		auto& shield = shield_fixture.ship().shields;
		shield.has_shields = true;
		shield.segment_count = count;
		shield.presence = ShieldStatePresenceFlagRechargeMax;
		shield.recharge_maximum = 0.0F;
		for (std::uint8_t index = 0U; index < count; ++index) {
			shield.segment_current_hits[index] = static_cast<float>(index);
			shield.segment_maximum_hits[index] = static_cast<float>(index + 1U);
		}
		StateImage shield_image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_core_gate_state_image(shield_fixture.input, shield_image));
		PacketReader reader({atom(shield_image, RecordType::ShieldState)->value.data(),
			atom(shield_image, RecordType::ShieldState)->value.size()});
		bool present = false;
		std::uint16_t decoded_count = 0U;
		ASSERT_TRUE(reader.skip(24U) && reader.read_bool8(present) &&
			reader.read_u16(decoded_count));
		EXPECT_TRUE(present);
		EXPECT_EQ(count, decoded_count);
	}
	CoreFixture overflow(0U);
	overflow.ship().shields.has_shields = true;
	overflow.ship().shields.segment_count = 65U;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_core_gate_state_image(overflow.input, image));
}

TEST(Phase2StateImage, P2TST033EtsModesAndP2TST034PropulsionPresenceAreExact)
{
	for (const auto mode : {0U, 1U, 2U}) {
		CoreFixture fixture(0U);
		auto& energy = fixture.ship().energy;
		if (mode == static_cast<unsigned>(EtsMode::Available) ||
			mode == static_cast<unsigned>(EtsMode::Locked)) {
			energy.shield_recharge_index = 2U;
			energy.weapon_recharge_index = 4U;
			energy.engine_recharge_index = 6U;
		}
		energy.ets_available = mode == static_cast<unsigned>(EtsMode::Available);
		if (mode == static_cast<unsigned>(EtsMode::Available)) {
			energy.presence = EnergyStatePresenceFlagWeaponEnergy |
				EnergyStatePresenceFlagRegeneration |
				EnergyStatePresenceFlagDeferredTransfers |
				EnergyStatePresenceFlagPowerOutput |
				EnergyStatePresenceFlagEngineIntegrity;
			energy.weapon_energy_current = 5.0F;
			energy.weapon_energy_maximum = 10.0F;
			energy.shield_regeneration_rate = 1.25F;
			energy.weapon_regeneration_rate = 2.5F;
			energy.deferred_weapon_transfer = -3.0F;
			energy.deferred_shield_transfer = 4.0F;
			energy.power_output = 5.5F;
			energy.engine_integrity_current = 60.0F;
			energy.engine_integrity_maximum = 80.0F;
		}
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_core_gate_state_image(fixture.input, image));
		PacketReader reader({atom(image, RecordType::EnergyState)->value.data(),
			atom(image, RecordType::EnergyState)->value.size()});
		std::uint64_t decoded_presence = 0U;
		std::uint8_t shield_index = 99U;
		std::uint8_t weapon_index = 99U;
		std::uint8_t engine_index = 99U;
		std::uint8_t decoded_mode = 99U;
		std::uint8_t reserved = 99U;
		ASSERT_TRUE(reader.skip(8U) && reader.read_u64(decoded_presence) &&
			reader.skip(8U) && reader.read_u8(decoded_mode) &&
			reader.read_u8(shield_index) && reader.read_u8(weapon_index) &&
			reader.read_u8(engine_index) && reader.read_u8(reserved));
		EXPECT_EQ(mode, decoded_mode);
		EXPECT_EQ(0U, reserved);
		if (mode == static_cast<unsigned>(EtsMode::Absent)) {
			EXPECT_EQ(0U, shield_index);
			EXPECT_EQ(0U, weapon_index);
			EXPECT_EQ(0U, engine_index);
		} else {
			EXPECT_EQ(2U, shield_index);
			EXPECT_EQ(4U, weapon_index);
			EXPECT_EQ(6U, engine_index);
		}
		if (mode == static_cast<unsigned>(EtsMode::Available)) {
			EXPECT_EQ(energy.presence, decoded_presence);
			std::array<float, 9U> decoded{};
			for (auto& value : decoded) ASSERT_TRUE(reader.read_f32(value));
			const std::array<float, 9U> expected{{5.0F, 10.0F, 1.25F, 2.5F,
				-3.0F, 4.0F, 5.5F, 60.0F, 80.0F}};
			EXPECT_EQ(expected, decoded);
		} else {
			EXPECT_EQ(0U, decoded_presence);
		}
		EXPECT_TRUE(reader.at_end());
	}

	CoreFixture fixture(0U);
	auto& propulsion = fixture.ship().propulsion;
	propulsion.presence = PropulsionStatePresenceFlagFuel |
		PropulsionStatePresenceFlagConsumption | PropulsionStatePresenceFlagEngagement |
		PropulsionStatePresenceFlagDynamics | PropulsionStatePresenceFlagEngineWash;
	propulsion.propulsion_flags = PropulsionFlagAfterburnerAvailable |
		PropulsionFlagAfterburnerRequested;
	propulsion.afterburner_fuel = 4.0F;
	propulsion.afterburner_capacity = 10.0F;
	propulsion.burn_rate = 2.0F;
	propulsion.recovery_rate = 1.0F;
	propulsion.minimum_to_engage = 3.0F;
	propulsion.cooldown_remaining_us = 4000U;
	propulsion.time_since_last_stop_us = 5000U;
	propulsion.fuel_at_last_engagement = 6.0F;
	propulsion.forward_acceleration_time_constant = 0.75F;
	propulsion.afterburner_max_velocity = {{100.0F, 200.0F, 300.0F}};
	propulsion.engine_wash_intensity = 0.625F;
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_core_gate_state_image(fixture.input, image));
	PacketReader reader({atom(image, RecordType::PropulsionState)->value.data(),
		atom(image, RecordType::PropulsionState)->value.size()});
	std::uint64_t presence = 0U;
	std::uint64_t duration = 0U;
	std::uint16_t flags = 0U;
	std::uint16_t reserved = 1U;
	ASSERT_TRUE(reader.skip(8U) && reader.read_u64(presence) && reader.skip(8U) &&
		reader.read_u16(flags) && reader.read_u16(reserved));
	EXPECT_EQ(propulsion.presence, presence);
	EXPECT_EQ(propulsion.propulsion_flags, flags);
	EXPECT_EQ(0U, reserved);
	EXPECT_EQ(0U, presence & PropulsionStatePresenceFlagRcs);
	std::array<float, 9U> decoded{};
	ASSERT_TRUE(reader.read_f32(decoded[0]) && reader.read_f32(decoded[1]) &&
		reader.read_f32(decoded[2]) && reader.read_f32(decoded[3]) &&
		reader.read_f32(decoded[4]) && reader.read_u64(duration));
	EXPECT_EQ(4000U, duration);
	ASSERT_TRUE(reader.read_u64(duration));
	EXPECT_EQ(5000U, duration);
	ASSERT_TRUE(reader.read_f32(decoded[5]) && reader.read_f32(decoded[6]) &&
		reader.read_f32(decoded[7]) && reader.read_f32(decoded[8]));
	float engine_wash = 0.0F;
	ASSERT_TRUE(reader.read_f32(engine_wash));
	const std::array<float, 9U> expected{{4.0F, 10.0F, 2.0F, 1.0F, 3.0F,
		6.0F, 0.75F, 100.0F, 200.0F}};
	EXPECT_EQ(expected, decoded);
	EXPECT_EQ(300.0F, engine_wash);
	ASSERT_TRUE(reader.read_f32(engine_wash));
	EXPECT_EQ(0.625F, engine_wash);
	EXPECT_TRUE(reader.at_end()) << "RCS must remain absent from the WP04 CoreGate payload.";

	CoreFixture invalid(0U);
	invalid.ship().propulsion.presence = PropulsionStatePresenceFlagFuel;
	invalid.ship().propulsion.propulsion_flags =
		PropulsionFlagAfterburnerAvailable | PropulsionFlagAfterburnerLocked |
		PropulsionFlagAfterburnerActive;
	invalid.ship().propulsion.afterburner_capacity = 10.0F;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_core_gate_state_image(invalid.input, image));
}

TEST(Phase2StateImage, P2TST039SubsystemCardinalityBijectionAndZeroMaximumAreExact)
{
	for (const std::size_t count : {0U, 1U, 1024U}) {
		CoreFixture fixture(count);
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_core_gate_state_image(fixture.input, image));
		EXPECT_EQ(count, atoms(image, RecordType::SubsystemState).size());
		EXPECT_EQ(9U + count, image.records().size());
	}
	CoreFixture ambiguous(2U);
	ambiguous.ship().subsystems.values[1].source_key =
		ambiguous.ship().subsystems.values[0].source_key;
	StateImage image;
	EXPECT_EQ(Phase2StateImageBuildStatus::SourceMappingMissing,
		build_phase2_core_gate_state_image(ambiguous.input, image));
	CoreFixture zero(1U);
	zero.ship().subsystems.values[0].hits_maximum = 0.0F;
	zero.ship().subsystems.values[0].hits_current = 0.01F;
	EXPECT_EQ(Phase2StateImageBuildStatus::SourceMappingMissing,
		build_phase2_core_gate_state_image(zero.input, image));
}

TEST(Phase2StateImage, PreallocatedReadyBuildDoesNotGrowAndFailurePreservesImage)
{
	CoreFixture fixture(7U);
	Phase2StateImagePool pool;
	ASSERT_TRUE(pool.provision(1024U));
	const auto owned = pool.owned_backing_bytes();
	ASSERT_GT(owned, 0U);
	StateImage first;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_core_gate_state_image_preallocated(fixture.input, pool, first));
	StateImage second;
	telemetry::test::phase2test::GlobalAllocationScope allocations;
	const auto status =
		build_phase2_core_gate_state_image_preallocated(fixture.input, pool, second);
	const auto growth = allocations.finish();
	EXPECT_EQ(Phase2StateImageBuildStatus::Created, status);
	EXPECT_EQ(0U, growth);
	EXPECT_EQ(owned, pool.owned_backing_bytes());
	const auto sentinel = second;
	fixture.input.manifest_applied = false;
	EXPECT_EQ(Phase2StateImageBuildStatus::ManifestUnavailable,
		build_phase2_core_gate_state_image_preallocated(fixture.input, pool, second));
	EXPECT_EQ(sentinel, second);
}

TEST(Phase2StateImage, FrozenPhase2PromotionGoldenHashAndSemanticImageRemainValid)
{
	const auto path = std::filesystem::path(FSTL_PROTOCOL_TEST_ASSET_PATH) /
		"vectors-v1.1" / "phase2-promotion" / "phase2-promotion.bin";
	std::ifstream stream(path, std::ios::binary);
	ASSERT_TRUE(stream.good()) << path.string();
	const std::vector<std::uint8_t> bytes{
		std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	Sha256Digest digest{};
	ASSERT_TRUE(sha256({bytes.data(), bytes.size()}, digest));
	const Sha256Digest expected{{0xab, 0xb4, 0xd3, 0xb9, 0xeb, 0x5e, 0x2a, 0x1b,
		0x1d, 0x84, 0xa8, 0xd1, 0xc2, 0xcf, 0x23, 0x5b, 0xe6, 0x2e, 0x83, 0x2d,
		0x16, 0xbc, 0x33, 0xc0, 0xf6, 0x31, 0x56, 0x65, 0x0c, 0xb1, 0x07, 0xb0}};
	EXPECT_EQ(expected, digest);
	FullSnapshotPartPayload payload{};
	ASSERT_EQ(ValidationError::None,
		decode_full_snapshot_part_payload({bytes.data(), bytes.size()}, payload));
	EXPECT_EQ(10U, payload.record_count);
	const std::uint32_t subsystem_id = 2U;
	auto context = validation_context(1U, &subsystem_id, 1U);
	BusinessStateImageValidator validator(context);
	StateImage image;
	EXPECT_EQ(ValidationError::None,
		decode_business_snapshot_region_validated(
			payload.records, payload.record_count, validator, image));
	EXPECT_EQ(10U, image.records().size());
}

} // namespace
