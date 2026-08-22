#include "telemetry/phase2_state_image.h"
#include "telemetry/phase2_allocation_tracker.h"
#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_business_records.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace {

using namespace telemetry;
using namespace telemetry::detail;
using namespace telemetry::protocol;

struct Wp05Fixture {
	std::unique_ptr<Phase2ObservationDto> observation =
		std::make_unique<Phase2ObservationDto>();
	std::unique_ptr<Phase2ManifestCandidate> manifest =
		std::make_unique<Phase2ManifestCandidate>();
	Phase2Wp05ProjectionInput input{};

	Wp05Fixture()
	{
		observation->capture = {Phase2CaptureStatus::Valid, Phase2CaptureReason::None};
		observation->player_key.value = 55U;
		observation->producer_sample_time_us = 1'000'000U;
		observation->ships.resize(1U);
		auto& ship = observation->ships[0];
		ship.capture_key.value = 55U;
		EXPECT_TRUE(ship.identity.internal_name.assign("wp05-player"));
		ship.identity.class_source_key.value = 41U;
		ship.identity.sample_time_us = observation->producer_sample_time_us;
		ship.flight.sample_time_us = observation->producer_sample_time_us;
		ship.flight.radius = 10.0F;
		ship.propulsion.sample_time_us = observation->producer_sample_time_us;
		ship.subsystems.count = 1U;
		auto& subsystem = ship.subsystems.values[0];
		subsystem.source_key.value = 100U;
		subsystem.sample_time_us = observation->producer_sample_time_us;
		subsystem.kind = ShipSubsystemKind::Turret;
		subsystem.hits_current = 50.0F;
		subsystem.hits_maximum = 100.0F;

		auto& controls = observation->player_controls;
		controls.sample_time_us = observation->producer_sample_time_us;
		controls.mode = PlayerControlModeObservation::Ship;
		controls.engine_control_mode = 0;
		controls.presence = ControlStatePresenceFlagCruise |
			ControlStatePresenceFlagRequestCounters;

		manifest->kind = ManifestKind::FullRequired;
		manifest->manifest_id = 1U;
		manifest->class_record_count = 1U;
		auto& ship_class = manifest->class_records[0];
		ship_class.source_key = 41U;
		ship_class.class_id = 7U;
		ship_class.subsystem_count = 1U;
		ship_class.subsystems = {manifest->subsystem_records.data(), 1U};
		manifest->subsystem_records[0] = {100U, 1001U, 0U, 0U};
		ship_class.bank_count = 2U;
		ship_class.banks = {manifest->bank_records.data(), 2U};
		auto& primary = manifest->bank_records[0];
		primary.bank_id = 2001U;
		primary.weapon_class_id = 3001U;
		primary.owner_subsystem_id = 1001U;
		primary.family = WeaponFamily::Turret;
		primary.source_family = WeaponFamily::Primary;
		primary.canonical_index = 0U;
		primary.source_index = 0U;
		primary.capacity = 20.0F;
		primary.consumes_ammunition = true;
		auto& secondary = manifest->bank_records[1];
		secondary.bank_id = 2002U;
		secondary.weapon_class_id = 3002U;
		secondary.owner_subsystem_id = 1001U;
		secondary.family = WeaponFamily::Turret;
		secondary.source_family = WeaponFamily::Secondary;
		secondary.canonical_index = 1U;
		secondary.source_index = 0U;
		secondary.capacity = 77.0F;

		input.player_entity_id = 9001U;
		input.observation = observation.get();
		input.installed_manifest = manifest.get();
		input.capture_kind = Phase2ProjectionCaptureKind::Keyframe;
	}

	ShipObservationDto& ship() { return observation->ships[0]; }
	PlayerControlObservation& controls() { return observation->player_controls; }
};

const StateAtom* atom(const StateImage& image, RecordType type)
{
	const auto raw = static_cast<std::uint16_t>(type);
	const auto found = std::find_if(image.records().begin(), image.records().end(),
		[raw](const auto& value) { return value.key.record_type == raw; });
	return found == image.records().end() ? nullptr : &*found;
}

ValidationError validate_atom(const StateAtom& value)
{
	BusinessRecordMetadata metadata{};
	const RecordEnvelopeView envelope{
		value.key.record_type, value.record_version, RecordFlagNone,
		{value.value.data(), value.value.size()}};
	return validate_business_record(
		envelope, BusinessRecordContainer::FullSnapshot, VersionMinor, metadata);
}

void configure_turret(Wp05Fixture& fixture)
{
	auto& subsystem = fixture.ship().subsystems.values[0];
	subsystem.presence = SubsystemStatePresenceFlagAnimatedTransform |
		SubsystemStatePresenceFlagTypeAggregate | SubsystemStatePresenceFlagTurret;
	subsystem.position_local = {{1.0F, 2.0F, 3.0F}};
	subsystem.orientation_local = {{1.0F, 0.0F, 0.0F, 0.0F}};
	subsystem.aggregate_current_hits = 75.0F;
	subsystem.aggregate_maximum_hits = 125.0F;
	subsystem.turret.emplace();
	auto& turret = *subsystem.turret;
	turret.turret_primary_bank_count = 1U;
	turret.turret_secondary_bank_count = 1U;
	turret.turret_primary_bank_weapon_source_keys[0].value = 501U;
	turret.turret_secondary_bank_weapon_source_keys[0].value = 502U;
	turret.turret_primary_banks[0] = {7, 20, 4'000U};
	turret.turret_secondary_banks[0] = {9, 99, 5'000U};
	turret.turret_current_direction_local = {{0.0F, 1.0F, 0.0F}};
	turret.turret_firing_point_count = 3U;
	turret.turret_next_fire_pos = 5;
	turret.turret_next_fire_remaining_us = 6'000U;
	turret.turret_rof_scaler = 0.0F;
	turret.turret_animation = 1;
}

TEST(Phase2ControlTurret, P2TST035MapsAllFiveControlModesWithClosedPriority)
{
	struct ModeCase {
		bool autopilot;
		bool ai;
		std::int32_t engine_mode;
		PlayerControlModeObservation latched;
		bool cursor;
		ControlMode expected;
	};
	const std::array<ModeCase, 5U> cases{{
		{false, true, 0, PlayerControlModeObservation::Ship, false, ControlMode::Unknown},
		{false, false, 0, PlayerControlModeObservation::Ship, false, ControlMode::Ship},
		{false, false, 0, PlayerControlModeObservation::Camera, false, ControlMode::View},
		{false, false, 0, PlayerControlModeObservation::Ship, true, ControlMode::FlightCursor},
		{true, true, 99, PlayerControlModeObservation::Camera, true, ControlMode::Autopilot},
	}};
	for (const auto& mode : cases) {
		Wp05Fixture fixture;
		fixture.controls().autopilot_engaged = mode.autopilot;
		fixture.controls().player_use_ai = mode.ai;
		fixture.controls().engine_control_mode = mode.engine_mode;
		fixture.controls().mode = mode.latched;
		fixture.controls().flight_cursor_active = mode.cursor;
		if (mode.cursor) {
			fixture.controls().presence |= ControlStatePresenceFlagFlightCursor;
			fixture.controls().flight_cursor_sensitivity = 0.5F;
			fixture.controls().effective_aim_extent = 2.0F;
		}
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_wp05_projection(fixture.input, image));
		const auto* control = atom(image, RecordType::ControlState);
		ASSERT_NE(nullptr, control);
		PacketReader reader({control->value.data(), control->value.size()});
		std::uint8_t decoded = 0xffU;
		ASSERT_TRUE(reader.skip(24U + 6U * sizeof(float)) && reader.read_u8(decoded));
		EXPECT_EQ(static_cast<std::uint8_t>(mode.expected), decoded);
	}
}

TEST(Phase2ControlTurret, P2TST035MapsAxesFlagsCruiseCountersAndCursorExactly)
{
	Wp05Fixture fixture;
	auto& controls = fixture.controls();
	controls.pitch = -1.0F;
	controls.heading = -0.5F;
	controls.bank = 0.25F;
	controls.vertical = 0.5F;
	controls.sideways = 0.75F;
	controls.forward = 1.0F;
	controls.action_flags = KnownControlFlags & ~ControlFlagAfterburnerRequested;
	controls.afterburner_requested = true;
	controls.forward_cruise_percent = -42.0F;
	controls.fire_primary_count = 11U;
	controls.fire_secondary_count = 12U;
	controls.fire_countermeasure_count = 13U;
	controls.flight_cursor_active = true;
	controls.presence |= ControlStatePresenceFlagFlightCursor;
	controls.flight_cursor_pitch = -0.25F;
	controls.flight_cursor_heading = 0.5F;
	controls.flight_cursor_sensitivity = 0.75F;
	controls.effective_aim_extent = 2.0F;
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection(fixture.input, image));
	const auto* control = atom(image, RecordType::ControlState);
	ASSERT_NE(nullptr, control);
	EXPECT_EQ(ValidationError::None, validate_atom(*control));
	PacketReader reader({control->value.data(), control->value.size()});
	std::uint64_t value64 = 0U;
	std::array<float, 6U> axes{};
	std::uint8_t mode = 0U;
	std::uint32_t flags = 0U;
	ASSERT_TRUE(reader.read_u64(value64) && reader.read_u64(value64) &&
		reader.read_u64(value64));
	for (auto& axis : axes) ASSERT_TRUE(reader.read_f32(axis));
	ASSERT_TRUE(reader.read_u8(mode) && reader.read_u32(flags));
	EXPECT_EQ((std::array<float, 6U>{{-1.0F, -0.5F, 0.25F, 0.5F, 0.75F, 1.0F}}),
		axes);
	EXPECT_EQ(KnownControlFlags, flags);
	float scalar = 0.0F;
	std::uint16_t counter = 0U;
	ASSERT_TRUE(reader.read_f32(scalar));
	EXPECT_EQ(-42.0F, scalar);
	for (const auto expected : {11U, 12U, 13U}) {
		ASSERT_TRUE(reader.read_u16(counter));
		EXPECT_EQ(expected, counter);
	}
	const std::array<float, 4U> expected_cursor{{-0.25F, 0.5F, 0.75F, 0.5F}};
	for (const auto expected : expected_cursor) {
		ASSERT_TRUE(reader.read_f32(scalar));
		EXPECT_EQ(expected, scalar);
	}
	EXPECT_TRUE(reader.at_end());
}

TEST(Phase2ControlTurret, P2TST035InvalidControlInputFailsTransactionally)
{
	for (const auto invalid : {-1.001F, 1.001F,
			 std::numeric_limits<float>::infinity(),
			 std::numeric_limits<float>::quiet_NaN()}) {
		Wp05Fixture fixture;
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_wp05_projection(fixture.input, image));
		const auto sentinel = image;
		fixture.controls().heading = invalid;
		EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
			build_phase2_wp05_projection(fixture.input, image));
		EXPECT_EQ(sentinel, image);
	}
	Wp05Fixture cursor;
	cursor.controls().flight_cursor_active = true;
	cursor.controls().presence |= ControlStatePresenceFlagFlightCursor;
	cursor.controls().flight_cursor_sensitivity = 1.01F;
	StateImage image;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp05_projection(cursor.input, image));
}

TEST(Phase2ControlTurret, P2TST040MapsSubsystemTransformAggregateAndForbidsOtherGroups)
{
	Wp05Fixture fixture;
	configure_turret(fixture);
	EXPECT_FALSE(fixture.manifest->bank_records[1].consumes_ammunition);
	EXPECT_GT(fixture.ship().subsystems.values[0].turret
			->turret_secondary_banks[0].turret_ammunition_current, 0);
	EXPECT_GT(fixture.ship().subsystems.values[0].turret
			->turret_secondary_banks[0].turret_ammunition_capacity, 0);
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection(fixture.input, image));
	const auto* subsystem = atom(image, RecordType::SubsystemState);
	ASSERT_NE(nullptr, subsystem);
	EXPECT_EQ(ValidationError::None, validate_atom(*subsystem));
	PacketReader reader({subsystem->value.data(), subsystem->value.size()});
	std::uint64_t value64 = 0U;
	std::uint32_t subsystem_id = 0U;
	ASSERT_TRUE(reader.read_u64(value64) && reader.read_u32(subsystem_id) &&
		reader.read_u64(value64));
	EXPECT_EQ(1001U, subsystem_id);
	EXPECT_EQ(SubsystemStatePresenceFlagAnimatedTransform |
			SubsystemStatePresenceFlagTypeAggregate | SubsystemStatePresenceFlagTurret,
		value64);
	EXPECT_EQ(0U, value64 & (SubsystemStatePresenceFlagNameOverrides |
			SubsystemStatePresenceFlagAnimations | SubsystemStatePresenceFlagLocalCargo));
	ASSERT_TRUE(reader.skip(8U + 2U));
	std::uint8_t type = 0U;
	ASSERT_TRUE(reader.read_u8(type));
	EXPECT_EQ(static_cast<std::uint8_t>(SubsystemType::Turret), type);
	ASSERT_TRUE(reader.skip(4U + 4U + 4U));
	std::array<float, 7U> transform{};
	for (auto& value : transform) ASSERT_TRUE(reader.read_f32(value));
	EXPECT_EQ((std::array<float, 7U>{{1.0F, 2.0F, 3.0F, 1.0F, 0.0F, 0.0F, 0.0F}}),
		transform);
	float aggregate = 0.0F;
	ASSERT_TRUE(reader.read_f32(aggregate));
	EXPECT_EQ(75.0F, aggregate);
	ASSERT_TRUE(reader.read_f32(aggregate));
	EXPECT_EQ(125.0F, aggregate);
}

TEST(Phase2ControlTurret, P2TST040TurretIsMechanicalPublicAndPhase3Free)
{
	const auto read_turret_presence = [](const StateImage& image) {
		const auto* subsystem = atom(image, RecordType::SubsystemState);
		EXPECT_NE(nullptr, subsystem);
		PacketReader reader({subsystem->value.data(), subsystem->value.size()});
		EXPECT_TRUE(reader.skip(8U + 4U + 8U + 8U + 2U + 1U + 4U + 4U + 4U +
			3U * 4U + 4U * 4U + 2U * 4U));
		std::uint32_t presence = 0U;
		EXPECT_TRUE(reader.read_u32(presence));
		return presence;
	};
	for (const std::uint16_t count : {0U, 1U, 64U}) {
		Wp05Fixture boundary;
		configure_turret(boundary);
		boundary.ship().subsystems.values[0].turret->turret_firing_point_count = count;
		boundary.ship().subsystems.values[0].turret->turret_next_fire_pos =
			count == 0U ? 0 : static_cast<std::int32_t>(count + 1U);
		StateImage boundary_image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_wp05_projection(boundary.input, boundary_image));
		const auto presence = read_turret_presence(boundary_image);
		EXPECT_EQ(count != 0U,
			(presence & TurretStatePresenceFlagNextFirePoint) != 0U);
	}
	Wp05Fixture overflow;
	configure_turret(overflow);
	StateImage overflow_image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection(overflow.input, overflow_image));
	const auto sentinel = overflow_image;
	overflow.ship().subsystems.values[0].turret->turret_firing_point_count = 65U;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp05_projection(overflow.input, overflow_image));
	EXPECT_EQ(sentinel, overflow_image);

	Wp05Fixture fixture;
	configure_turret(fixture);
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection(fixture.input, image));
	const auto* subsystem = atom(image, RecordType::SubsystemState);
	PacketReader reader({subsystem->value.data(), subsystem->value.size()});
	ASSERT_TRUE(reader.skip(8U + 4U + 8U + 8U + 2U + 1U + 4U + 4U + 4U +
		3U * 4U + 4U * 4U + 2U * 4U));
	std::uint32_t turret_presence = 0U;
	std::uint64_t target = 1U;
	ASSERT_TRUE(reader.read_u32(turret_presence) && reader.read_u64(target));
	EXPECT_EQ(0U, target);
	constexpr std::uint32_t Forbidden = TurretStatePresenceFlagTargetSubsystem |
		TurretStatePresenceFlagAimPoint | TurretStatePresenceFlagTimeInRange |
		TurretStatePresenceFlagOptimalRange | TurretStatePresenceFlagTargetPriority |
		TurretStatePresenceFlagInaccuracy | TurretStatePresenceFlagSwarm |
		TurretStatePresenceFlagAwacs;
	EXPECT_EQ(0U, turret_presence & Forbidden);
	EXPECT_EQ(TurretStatePresenceFlagNextFirePoint | TurretStatePresenceFlagCooldown |
			TurretStatePresenceFlagRateMultiplier | TurretStatePresenceFlagAnimation |
			TurretStatePresenceFlagBanks,
		turret_presence);
	std::array<float, 3U> direction{};
	for (auto& value : direction) ASSERT_TRUE(reader.read_f32(value));
	EXPECT_EQ((std::array<float, 3U>{{0.0F, 1.0F, 0.0F}}), direction);
	std::uint16_t next = 0U;
	ASSERT_TRUE(reader.read_u16(next) && reader.skip(2U));
	EXPECT_EQ(2U, next);
	std::uint64_t duration = 0U;
	ASSERT_TRUE(reader.read_u64(duration));
	EXPECT_EQ(6'000U, duration);
	float rate = 0.0F;
	ASSERT_TRUE(reader.read_f32(rate));
	EXPECT_EQ(3.0F, rate);
	std::uint8_t animation = 0U;
	ASSERT_TRUE(reader.read_u8(animation) && reader.read_u64(duration));
	EXPECT_EQ(static_cast<std::uint8_t>(AnimationState::Moving), animation);
	std::uint16_t bank_count = 0U;
	ASSERT_TRUE(reader.read_u16(bank_count));
	EXPECT_EQ(2U, bank_count);
	std::array<std::uint32_t, 2U> bank_ids{};
	for (std::size_t index = 0U; index < bank_ids.size(); ++index) {
		std::uint8_t item_version = 0U;
		std::uint16_t item_size = 0U;
		PacketReader item;
		ASSERT_TRUE(reader.read_u8(item_version) && reader.read_u16(item_size) &&
			reader.subreader(item_size, item));
		EXPECT_EQ(1U, item_version);
		std::uint16_t item_presence = 0U;
		std::uint8_t family = 0U;
		std::uint16_t bank_index = 0U;
		std::uint32_t weapon = 0U;
		ASSERT_TRUE(item.read_u16(item_presence) && item.read_u8(family) &&
			item.skip(1U) && item.read_u16(bank_index) &&
			item.read_u32(bank_ids[index]) && item.read_u32(weapon));
		EXPECT_EQ(index == 0U ? TurretBankPresenceFlagAmmo :
			TurretBankPresenceFlagNone, item_presence);
		if (item_presence != 0U) ASSERT_TRUE(item.skip(4U + 4U));
		ASSERT_TRUE(item.read_u64(duration));
		EXPECT_TRUE(item.at_end());
	}
	EXPECT_EQ((std::array<std::uint32_t, 2U>{{2001U, 2002U}}), bank_ids);
	EXPECT_TRUE(reader.at_end());
}

TEST(Phase2ControlTurret, P2TST034CrossCoherenceIsStrictOnlyForSameSampleOrKeyframe)
{
	Wp05Fixture fixture;
	fixture.ship().propulsion.presence = PropulsionStatePresenceFlagFuel;
	fixture.ship().propulsion.propulsion_flags =
		PropulsionFlagAfterburnerAvailable | PropulsionFlagAfterburnerActive;
	fixture.ship().propulsion.afterburner_capacity = 10.0F;
	fixture.ship().propulsion.afterburner_fuel = 5.0F;
	fixture.ship().flight.physics_mode_flags = PhysicsModeFlagNone;
	StateImage image;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp05_projection(fixture.input, image));
	fixture.ship().flight.physics_mode_flags = PhysicsModeFlagAfterburner;
	EXPECT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection(fixture.input, image));
	fixture.input.capture_kind = Phase2ProjectionCaptureKind::Delta;
	fixture.ship().propulsion.sample_time_us -= 1U;
	fixture.ship().flight.physics_mode_flags = PhysicsModeFlagNone;
	EXPECT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection(fixture.input, image));
}

TEST(Phase2ControlTurret, ProjectionRoundTripsAndDoesNotGrowAfterReady)
{
	Wp05Fixture fixture;
	configure_turret(fixture);
	Phase2Wp05ProjectionPool pool;
	ASSERT_TRUE(pool.provision(1024U));
	const auto owned = pool.owned_backing_bytes();
	ASSERT_GT(owned, 0U);
	StateImage warmup;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection_preallocated(fixture.input, pool, warmup));
	for (const auto& value : warmup.records()) {
		EXPECT_EQ(ValidationError::None, validate_atom(value));
	}
	StateImage measured;
	telemetry::test::phase2test::GlobalAllocationScope allocations;
	const auto status =
		build_phase2_wp05_projection_preallocated(fixture.input, pool, measured);
	const auto growth = allocations.finish();
	EXPECT_EQ(Phase2StateImageBuildStatus::Created, status);
	EXPECT_EQ(0U, growth);
	EXPECT_EQ(owned, pool.owned_backing_bytes());
	EXPECT_EQ(warmup.records(), measured.records());

	Wp05Fixture multiple;
	ShipObservationDto second = multiple.ship();
	second.capture_key.value = 66U;
	second.identity.class_source_key.value = 42U;
	second.subsystems.values[0].source_key.value = 100U;
	second.subsystems.values[0].kind = ShipSubsystemKind::Engine;
	second.subsystems.values[0].presence = SubsystemStatePresenceFlagNone;
	second.subsystems.values[0].turret.reset();
	multiple.observation->ships.push_back(std::move(second));
	multiple.manifest->class_record_count = 2U;
	auto& second_class = multiple.manifest->class_records[1];
	second_class.source_key = 42U;
	second_class.class_id = 8U;
	second_class.subsystem_count = 1U;
	second_class.subsystems = {multiple.manifest->subsystem_records.data() + 1U, 1U};
	multiple.manifest->subsystem_records[1] = {100U, 1101U, 0U, 0U};
	const std::array<Phase2Wp05SubjectBinding, 2U> bindings{{
		{{55U}, 9001U},
		{{66U}, 9002U},
	}};
	multiple.input.subjects = bindings.data();
	multiple.input.subject_count = bindings.size();
	StateImage multi_image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection(multiple.input, multi_image));
	std::size_t control_count = 0U;
	std::array<std::array<std::uint64_t, 2U>, 2U> public_pairs{};
	std::size_t subsystem_count = 0U;
	for (const auto& value : multi_image.records()) {
		if (value.key.record_type ==
			static_cast<std::uint16_t>(RecordType::ControlState)) {
			++control_count;
			continue;
		}
		if (value.key.record_type !=
			static_cast<std::uint16_t>(RecordType::SubsystemState)) {
			continue;
		}
		ASSERT_LT(subsystem_count, public_pairs.size());
		PacketReader reader({value.value.data(), value.value.size()});
		std::uint32_t subsystem_id = 0U;
		ASSERT_TRUE(reader.read_u64(public_pairs[subsystem_count][0]) &&
			reader.read_u32(subsystem_id));
		public_pairs[subsystem_count][1] = subsystem_id;
		++subsystem_count;
	}
	EXPECT_EQ(1U, control_count);
	EXPECT_EQ(2U, subsystem_count);
	EXPECT_EQ((std::array<std::array<std::uint64_t, 2U>, 2U>{{
		{{9001U, 1001U}}, {{9002U, 1101U}},
	}}), public_pairs);

	const auto multi_sentinel = multi_image;
	auto invalid_bindings = bindings;
	invalid_bindings[1].capture_key.value = 55U;
	multiple.input.subjects = invalid_bindings.data();
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp05_projection(multiple.input, multi_image));
	EXPECT_EQ(multi_sentinel, multi_image);
	invalid_bindings = bindings;
	invalid_bindings[1].entity_id = 9001U;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp05_projection(multiple.input, multi_image));
	EXPECT_EQ(multi_sentinel, multi_image);
	invalid_bindings = bindings;
	invalid_bindings[1].capture_key.value = 77U;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp05_projection(multiple.input, multi_image));
	EXPECT_EQ(multi_sentinel, multi_image);
	multiple.input.subjects = bindings.data();
	multiple.input.subject_count = 1U;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp05_projection(multiple.input, multi_image));
	EXPECT_EQ(multi_sentinel, multi_image);

	Wp05Fixture aggregate;
	ShipObservationDto aggregate_second = aggregate.ship();
	aggregate_second.capture_key.value = 66U;
	aggregate_second.identity.class_source_key.value = 42U;
	aggregate_second.subsystems.values[0].source_key.value = 5000U;
	aggregate_second.subsystems.values[0].kind = ShipSubsystemKind::Engine;
	aggregate_second.subsystems.values[0].presence = SubsystemStatePresenceFlagNone;
	aggregate_second.subsystems.values[0].turret.reset();
	auto& aggregate_first = aggregate.ship();
	aggregate_first.subsystems.count = 1024U;
	auto& aggregate_first_class = aggregate.manifest->class_records[0];
	aggregate_first_class.subsystem_count = 1024U;
	aggregate_first_class.subsystems =
		{aggregate.manifest->subsystem_records.data(), 1024U};
	for (std::size_t index = 0U; index < 1024U; ++index) {
		auto& source = aggregate_first.subsystems.values[index];
		source = {};
		source.source_key.value = static_cast<std::uint32_t>(index + 1U);
		source.sample_time_us = aggregate.observation->producer_sample_time_us;
		source.kind = ShipSubsystemKind::Generic;
		auto& definition = aggregate.manifest->subsystem_records[index];
		definition = {static_cast<std::uint32_t>(index + 1U),
			static_cast<std::uint32_t>(index + 1U),
			static_cast<std::uint32_t>(index), 0U};
	}
	aggregate.observation->ships.push_back(std::move(aggregate_second));
	aggregate.manifest->class_record_count = 2U;
	auto& aggregate_second_class = aggregate.manifest->class_records[1];
	aggregate_second_class.source_key = 42U;
	aggregate_second_class.class_id = 8U;
	aggregate_second_class.subsystem_count = 1U;
	aggregate_second_class.subsystems =
		{aggregate.manifest->subsystem_records.data() + 1024U, 1U};
	aggregate.manifest->subsystem_records[1024U] =
		{5000U, 2001U, 0U, 0U};
	const std::array<Phase2Wp05SubjectBinding, 2U> aggregate_bindings{{
		{{55U}, 9001U},
		{{66U}, 9002U},
	}};
	aggregate.input.subjects = aggregate_bindings.data();
	aggregate.input.subject_count = aggregate_bindings.size();
	Phase2Wp05ProjectionPool aggregate_pool;
	ASSERT_TRUE(aggregate_pool.provision(4096U));
	const auto aggregate_owned = aggregate_pool.owned_backing_bytes();
	ASSERT_GT(aggregate_owned, 0U);
	StateImage aggregate_warmup;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp05_projection_preallocated(
			aggregate.input, aggregate_pool, aggregate_warmup));
	EXPECT_EQ(1026U, aggregate_warmup.records().size());
	StateImage aggregate_measured;
	telemetry::test::phase2test::GlobalAllocationScope aggregate_allocations;
	const auto aggregate_status =
		build_phase2_wp05_projection_preallocated(
			aggregate.input, aggregate_pool, aggregate_measured);
	const auto aggregate_growth = aggregate_allocations.finish();
	EXPECT_EQ(Phase2StateImageBuildStatus::Created, aggregate_status);
	EXPECT_EQ(0U, aggregate_growth);
	EXPECT_EQ(aggregate_owned, aggregate_pool.owned_backing_bytes());
	EXPECT_EQ(aggregate_warmup.records(), aggregate_measured.records());
	EXPECT_FALSE(aggregate_pool.provision(4097U));
	EXPECT_TRUE(aggregate_pool.ready());
	EXPECT_EQ(4096U, aggregate_pool.maximum_subsystems());
	EXPECT_EQ(aggregate_owned, aggregate_pool.owned_backing_bytes());
}

} // namespace
