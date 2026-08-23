#include "telemetry/cockpit_producer_eligibility.h"

#include <gtest/gtest.h>

namespace telemetry {
namespace {

TEST(CockpitProducerEligibility, AcceptsSoloGraphicalRuntime)
{
	EXPECT_EQ(CockpitProducerEligibilityError::None,
		validate_cockpit_sensor_producer(
			{protocol::AuthorityMode::Solo, false, false}));
}

TEST(CockpitProducerEligibility, RejectsUnsupportedRuntimeModes)
{
	EXPECT_EQ(CockpitProducerEligibilityError::UnsupportedAuthority,
		validate_cockpit_sensor_producer(
			{protocol::AuthorityMode::MultiplayerMaster, false, false}));
	EXPECT_EQ(CockpitProducerEligibilityError::DedicatedNotAllowed,
		validate_cockpit_sensor_producer(
			{protocol::AuthorityMode::Solo, true, false}));
	EXPECT_EQ(CockpitProducerEligibilityError::HeadlessNotAllowed,
		validate_cockpit_sensor_producer(
			{protocol::AuthorityMode::Solo, false, true}));
}

} // namespace
} // namespace telemetry
