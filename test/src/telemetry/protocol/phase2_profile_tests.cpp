#include "telemetry/phase2_catalog_projection.h"
#include "telemetry/phase2_profile_gate.h"
#include "telemetry/phase3_state_image.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace {

using namespace telemetry;

TEST(TelemetryPhase2ProfileGate, CoreGateMapsToTheFrozenCoverage)
{
	EXPECT_EQ(0x0401ULL, phase2_profile_coverage(Phase2Profile::CoreGate));
	EXPECT_EQ(Phase2Profile::CoreGate, phase2_profile_from_coverage(0x0401ULL));
}

TEST(TelemetryPhase2ProfileGate, CompleteShipMapsToTheFrozenCoverage)
{
	EXPECT_EQ(0x0583ULL, phase2_profile_coverage(Phase2Profile::CompleteShip));
	EXPECT_EQ(Phase2Profile::CompleteShip, phase2_profile_from_coverage(0x0583ULL));
}

TEST(TelemetryPhase3ProfileGate, CockpitSensorsMapsToTheFrozenCoverage)
{
	EXPECT_EQ(0x07CBULL, phase2_profile_coverage(Phase2Profile::CockpitSensors));
	EXPECT_EQ(Phase2Profile::CockpitSensors, phase2_profile_from_coverage(0x07CBULL));
}

TEST(TelemetryPhase3ProfileGate, CockpitSensorsExcludesLowFrequencyEffectsCommunicationAndVideo)
{
	const auto coverage =
		phase2_profile_coverage(Phase2Profile::CockpitSensors);
	EXPECT_EQ(0U,
		coverage & protocol::StateDomainCoverageBitLowFrequencyEffects);
	EXPECT_EQ(0U,
		Phase3StateDerivedEventCoverage &
			protocol::EventFamilyBitCommunication);
}

TEST(TelemetryPhase2ProfileGate, IncompleteOrExpandedCoverageDoesNotSelectAProfile)
{
	constexpr std::array<std::uint64_t, 12> invalid_coverages{{
		0x0000ULL,
		0x0400ULL,
		0x0001ULL,
		0x0403ULL,
		0x0481ULL,
		0x0501ULL,
		0x0581ULL,
		0x0587ULL,
		0x078BULL,
		0x07C3ULL,
		0x07CAULL,
		0x07CFULL,
	}};

	for (const auto coverage : invalid_coverages) {
		SCOPED_TRACE(coverage);
		EXPECT_EQ(Phase2Profile::None, phase2_profile_from_coverage(coverage));
	}
}

TEST(TelemetryPhase2ProfileGate, InvalidCoverageReturnsAnObservableFailClosedReason)
{
	Phase2Profile selected = Phase2Profile::CompleteShip;
	EXPECT_EQ(Phase2ProfileError::UnsupportedCoverage,
		validate_phase2_profile_coverage(0x0581ULL, selected));
	EXPECT_EQ(Phase2Profile::None, selected);
}

TEST(TelemetryPhase3ProfileGate, ExactCoverageSelectsCockpitSensors)
{
	Phase2Profile selected = Phase2Profile::None;
	EXPECT_EQ(Phase2ProfileError::None,
		validate_phase2_profile_coverage(0x07CBULL, selected));
	EXPECT_EQ(Phase2Profile::CockpitSensors, selected);
}

TEST(TelemetryPhase2ProfileGate, ProfileErrorRegistryIsClosed)
{
	EXPECT_GT(static_cast<std::uint8_t>(Phase2ProfileError::Count),
		static_cast<std::uint8_t>(Phase2ProfileError::None));
}

TEST(TelemetryPhase3ProfileGate, CockpitSensorsEligibilityIsSoloGraphicalOnly)
{
	using telemetry::protocol::AuthorityMode;

	const Phase2ProfileEligibility eligible{AuthorityMode::Solo, false, false};
	EXPECT_EQ(Phase2ProfileError::None,
		validate_cockpit_sensor_producer(eligible));

	const std::array<Phase2ProfileEligibility, 4> ineligible{{
		{AuthorityMode::MultiplayerClient, false, false},
		{AuthorityMode::MultiplayerMaster, false, false},
		{AuthorityMode::Solo, true, false},
		{AuthorityMode::Solo, false, true},
	}};
	for (const auto& input : ineligible) {
		SCOPED_TRACE(static_cast<std::uint8_t>(input.authority_mode));
		EXPECT_NE(Phase2ProfileError::None,
			validate_cockpit_sensor_producer(input));
	}
}

TEST(TelemetryPhase2ProfileGate, EveryEligibilityRejectionHasItsClosedObservableReason)
{
	using telemetry::protocol::AuthorityMode;

	struct Rejection {
		Phase2ProfileEligibility eligibility;
		Phase2ProfileError expected;
	};
	constexpr std::array<Rejection, 4> rejections{{
		{{AuthorityMode::MultiplayerClient, false, false},
			Phase2ProfileError::UnsupportedAuthority},
		{{AuthorityMode::MultiplayerMaster, false, false},
			Phase2ProfileError::UnsupportedAuthority},
		{{AuthorityMode::Solo, true, false},
			Phase2ProfileError::DedicatedNotAllowed},
		{{AuthorityMode::Solo, false, true},
			Phase2ProfileError::HeadlessNotAllowed},
	}};

	for (const auto& rejection : rejections) {
		SCOPED_TRACE(static_cast<std::uint8_t>(rejection.expected));
		EXPECT_EQ(rejection.expected,
			validate_cockpit_sensor_producer(rejection.eligibility));
	}
}

TEST(TelemetryPhase3ProfileGate, EligibilityApiIsNoexceptAndHasNoProfileInput)
{
	static_assert(noexcept(validate_cockpit_sensor_producer(
		std::declval<const Phase2ProfileEligibility&>())));
	static_assert(std::is_trivially_destructible_v<Phase2ProfileEligibility>);
}

TEST(TelemetryPhase2CatalogProjection,
	WeaponAndSubsystemMappingsAreClosedAndAppliedBeforeIds)
{
	auto observation =
		std::make_unique<detail::Phase2ObservationDto>();
	observation->capture.status =
		detail::Phase2CaptureStatus::Valid;
	observation->capture.reason =
		detail::Phase2CaptureReason::None;
	observation->player_key = {1U};
	auto& raw = observation->raw_static_catalog;
	raw.class_count = 1U;
	raw.weapon_count = 4U;
	raw.aggregate_subsystem_count = 6U;

	auto& ship_class = raw.class_definitions[0];
	ship_class.class_capture_key = 1U;
	ASSERT_TRUE(ship_class.internal_name.assign("ProjectionShip"));
	ship_class.subsystem_count = 6U;
	ship_class.subsystem_offset = 0U;
	ship_class.bank_count = 2U;
	ship_class.bank_offset = 0U;
	ship_class.primary_bank_count = 1U;
	ship_class.secondary_bank_count = 1U;
	ship_class.has_scan = true;
	for (std::size_t index = 0U; index < 6U; ++index) {
		auto& subsystem = raw.subsystem_storage[index];
		subsystem.subsystem_capture_key =
			static_cast<std::uint32_t>(index + 1U);
		ASSERT_TRUE(subsystem.internal_name.assign("Subsystem"));
		subsystem.max_hits = 100.0F;
	}
	raw.subsystem_storage[0].subsystem_type_source = 1U;
	raw.subsystem_storage[1].subsystem_type_source = 8U;
	raw.subsystem_storage[2].subsystem_type_source = 9U;
	raw.subsystem_storage[3].subsystem_type_source = 10U;
	raw.subsystem_storage[4].subsystem_type_source = 11U;
	raw.subsystem_storage[5].subsystem_type_source = 7U;

	for (std::size_t index = 0U; index < 4U; ++index) {
		auto& weapon = raw.weapon_definitions[index];
		weapon.weapon_capture_key =
			static_cast<std::uint32_t>(index + 1U);
		ASSERT_TRUE(weapon.internal_name.assign("Weapon"));
		weapon.reloaded_per_batch = 1U;
		weapon.shots_source = 1;
	}
	raw.weapon_definitions[0].weapon_subtype_source = 2U;
	raw.weapon_definitions[0].raw_class_flags =
		protocol::WeaponClassFlagCountermeasure |
		protocol::WeaponClassFlagBeam;
	raw.weapon_definitions[1].weapon_subtype_source = 0U;
	raw.weapon_definitions[1].raw_class_flags =
		protocol::WeaponClassFlagBeam;
	raw.weapon_definitions[2].weapon_subtype_source = 0U;
	raw.weapon_definitions[2].burst_shots = 3;
	raw.weapon_definitions[2].swarm_count_source = -1;
	raw.weapon_definitions[2].shots_source = 2;
	raw.weapon_definitions[3].weapon_subtype_source = 1U;
	for (std::size_t index = 0U; index < 2U; ++index) {
		auto& bank = raw.bank_storage[index];
		bank.bank_capture_key =
			static_cast<std::uint32_t>(index + 1U);
		bank.family_source =
			static_cast<std::uint8_t>(index + 1U);
		bank.source_family = 0U;
		bank.bank_index = 0U;
		bank.weapon_capture_key =
			static_cast<std::uint32_t>(index + 1U);
		bank.fire_point_count = 1U;
		bank.fire_points[0] = {0.0F, 0.0F, 0.0F};
	}

	auto projected =
		std::make_unique<Phase2ManifestSource>();
	ASSERT_EQ(detail::Phase2CatalogProjectionStatus::Success,
		detail::project_phase2_catalog(
			*observation, *projected));
	EXPECT_EQ(protocol::WeaponSubtype::Countermeasure,
		projected->weapons[0].subtype);
	EXPECT_EQ(protocol::WeaponSubtype::Beam,
		projected->weapons[1].subtype);
	EXPECT_EQ(protocol::WeaponSubtype::Primary,
		projected->weapons[2].subtype);
	EXPECT_EQ(protocol::WeaponSubtype::Missile,
		projected->weapons[3].subtype);
	EXPECT_TRUE(projected->weapons[2].has_burst);
	EXPECT_EQ(4U, projected->weapons[2].burst_count);
	EXPECT_TRUE(projected->weapons[2].has_swarm);
	EXPECT_EQ(2U, projected->weapons[2].swarm_count);
	EXPECT_EQ(2U, projected->weapons[2].shots_per_trigger);
	ASSERT_EQ(2U, projected->ship_classes[0].bank_count);
	EXPECT_EQ(protocol::WeaponFamily::Primary,
		projected->ship_classes[0].banks[0].family);
	EXPECT_EQ(protocol::WeaponFamily::None,
		projected->ship_classes[0].banks[0].source_family);
	EXPECT_EQ(protocol::WeaponFamily::Secondary,
		projected->ship_classes[0].banks[1].family);
	EXPECT_EQ(protocol::WeaponFamily::None,
		projected->ship_classes[0].banks[1].source_family);
	EXPECT_NEAR(static_cast<float>(std::acos(0.95)),
		projected->ship_classes[0].scan_max_angle_rad,
		1.0e-6F);

	const std::array<protocol::SubsystemType, 6U> expected{{
		protocol::SubsystemType::Engine,
		protocol::SubsystemType::Reactor,
		protocol::SubsystemType::Other,
		protocol::SubsystemType::Other,
		protocol::SubsystemType::Unknown,
		protocol::SubsystemType::Sensors,
	}};
	for (std::size_t index = 0U; index < expected.size(); ++index)
		EXPECT_EQ(expected[index],
			projected->ship_classes[0].subsystems[index].type);
}

TEST(TelemetryPhase2CatalogProjection,
	CountsAreValidatedBeforeNarrowingAndUnavailableSourceIsDistinct)
{
	auto observation =
		std::make_unique<detail::Phase2ObservationDto>();
	auto projected =
		std::make_unique<Phase2ManifestSource>();
	observation->capture.status =
		detail::Phase2CaptureStatus::NoPlayer;
	observation->capture.reason =
		detail::Phase2CaptureReason::None;
	EXPECT_EQ(
		detail::Phase2CatalogProjectionStatus::
			SourceTemporarilyUnavailable,
		detail::project_phase2_catalog(
			*observation, *projected));

	observation->capture.status =
		detail::Phase2CaptureStatus::Valid;
	observation->raw_static_catalog.weapon_count = 1U;
	auto& weapon =
		observation->raw_static_catalog.weapon_definitions[0];
	weapon.weapon_capture_key = 1U;
	ASSERT_TRUE(weapon.internal_name.assign("BoundedWeapon"));
	weapon.weapon_subtype_source = 0U;
	weapon.reloaded_per_batch = 1U;
	weapon.shots_source = 4097;
	EXPECT_EQ(
		detail::Phase2CatalogProjectionStatus::
			SourceLimitExceeded,
		detail::project_phase2_catalog(
			*observation, *projected));

	weapon.shots_source = 1;
	weapon.burst_shots = 4096;
	EXPECT_EQ(
		detail::Phase2CatalogProjectionStatus::
			SourceLimitExceeded,
		detail::project_phase2_catalog(
			*observation, *projected));
}

} // namespace
