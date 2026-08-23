#include "telemetry/cockpit_sensors_state_image.h"
#include "telemetry/protocol/telemetry_business_state_validation.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <vector>

namespace {

using telemetry::protocol::BusinessStateImageValidator;
using telemetry::protocol::BusinessStateValidationContext;
using telemetry::protocol::StateImage;
using telemetry::protocol::StateImageResult;
using telemetry::protocol::ValidationError;

struct NoPlayerCockpitImage {
	std::unique_ptr<telemetry::detail::Phase2ObservationDto> observation =
		std::make_unique<telemetry::detail::Phase2ObservationDto>();
	std::unique_ptr<telemetry::Phase2ManifestCandidate> manifest =
		std::make_unique<telemetry::Phase2ManifestCandidate>();
	std::unique_ptr<telemetry::CockpitSensorsStateImagePool> pool =
		std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	std::unique_ptr<telemetry::Phase3Projection> projection =
		std::make_unique<telemetry::Phase3Projection>();
	telemetry::CockpitSensorsStateImageInput input{};
	StateImage image;

	NoPlayerCockpitImage()
	{
		observation->capture.status =
			telemetry::detail::Phase2CaptureStatus::NoPlayer;
		observation->capture.reason =
			telemetry::detail::Phase2CaptureReason::None;
		manifest->manifest_id = 1U;
		input.producer_id = 7U;
		input.session_phase = telemetry::protocol::SessionPhase::Live;
		input.mission.mission_generation = 1U;
		input.mission.phase = telemetry::protocol::MissionPhase::Active;
		input.observation = observation.get();
		input.installed_manifest = manifest.get();
		EXPECT_TRUE(pool->provision(1U, 1U, 1U, 1U));
		EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
			telemetry::build_cockpit_sensors_state_image_preallocated(
				input, *pool, *projection, image));
	}
};

BusinessStateValidationContext context()
{
	BusinessStateValidationContext result{};
	result.protocol_minor = telemetry::protocol::VersionMinor;
	result.required_manifest_id = 1U;
	result.class_manifest_installed = true;
	result.weapon_manifest_installed = true;
	return result;
}

StateImage with_coverage(const StateImage& source, std::uint64_t coverage)
{
	auto records = source.records();
	for (auto& record : records) {
		if (record.key.record_type != static_cast<std::uint16_t>(
				telemetry::protocol::RecordType::SessionState))
			continue;
		for (std::size_t index = 0U; index < 8U; ++index)
			record.value[40U + index] = static_cast<std::uint8_t>(
				coverage >> (index * 8U));
	}
	StateImage result;
	EXPECT_EQ(StateImageResult::Created,
		StateImage::create(std::move(records), result));
	return result;
}

TEST(TelemetryBusinessStateValidation, AcceptsOnlyTheCockpitSensorsProductImage)
{
	NoPlayerCockpitImage fixture;
	auto validation_context = context();
	ASSERT_EQ(telemetry::protocol::VersionMinor,
		validation_context.protocol_minor);
	ASSERT_TRUE(telemetry::protocol::is_supported_version_minor(
		validation_context.protocol_minor));
	BusinessStateImageValidator validator(validation_context);
	EXPECT_EQ(ValidationError::None, validator.validate(fixture.image));
}

TEST(TelemetryBusinessStateValidation, RejectsEveryRemovedHistoricalCoverage)
{
	NoPlayerCockpitImage fixture;
	auto validation_context = context();
	BusinessStateImageValidator validator(validation_context);
	for (const auto coverage : std::array<std::uint64_t, 3U>{
			telemetry::protocol::StateDomainCoverageBitCoreShip,
			telemetry::protocol::StateDomainCoverageBitCoreShip |
				telemetry::protocol::StateDomainCoverageBitPlayerKinematics,
			telemetry::protocol::StateDomainCoverageBitCoreShip |
				telemetry::protocol::StateDomainCoverageBitControlInputs |
				telemetry::protocol::StateDomainCoverageBitWeapons |
				telemetry::protocol::StateDomainCoverageBitCargoDockSupport |
				telemetry::protocol::StateDomainCoverageBitPlayerKinematics}) {
		EXPECT_NE(ValidationError::None,
			validator.validate(with_coverage(fixture.image, coverage)))
			<< coverage;
	}
}

TEST(TelemetryBusinessStateValidation, RejectsAnyProtocolMinorOtherThanFstl11)
{
	NoPlayerCockpitImage fixture;
	auto invalid_context = context();
	invalid_context.protocol_minor = 0U;
	BusinessStateImageValidator validator(invalid_context);
	EXPECT_EQ(ValidationError::UnsupportedMinor,
		validator.validate(fixture.image));
}

} // namespace
