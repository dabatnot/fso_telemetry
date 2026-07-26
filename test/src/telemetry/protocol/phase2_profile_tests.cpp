#include "telemetry/phase2_profile_gate.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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

TEST(TelemetryPhase2ProfileGate, IncompleteOrExpandedCoverageDoesNotSelectAProfile)
{
	constexpr std::array<std::uint64_t, 8> invalid_coverages{{
		0x0000ULL,
		0x0400ULL,
		0x0001ULL,
		0x0403ULL,
		0x0481ULL,
		0x0501ULL,
		0x0581ULL,
		0x0587ULL,
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

TEST(TelemetryPhase2ProfileGate, ProfileErrorRegistryIsClosed)
{
	EXPECT_GT(static_cast<std::uint8_t>(Phase2ProfileError::Count),
		static_cast<std::uint8_t>(Phase2ProfileError::None));
}

TEST(TelemetryPhase2ProfileGate, CompleteShipEligibilityIsSoloCockpitNonTrustedOnly)
{
	using telemetry::protocol::AuthorityMode;
	using telemetry::protocol::VisibilityMode;

	Phase2Profile selected = Phase2Profile::None;
	const Phase2ProfileEligibility eligible{
		AuthorityMode::Solo, VisibilityMode::Cockpit, false, false, false};
	EXPECT_EQ(Phase2ProfileError::None,
		select_phase2_profile(eligible, Phase2Profile::CompleteShip, selected));
	EXPECT_EQ(Phase2Profile::CompleteShip, selected);

	const std::array<Phase2ProfileEligibility, 6> ineligible{{
		{AuthorityMode::MultiplayerClient, VisibilityMode::Cockpit, false, false, false},
		{AuthorityMode::MultiplayerMaster, VisibilityMode::Cockpit, false, false, false},
		{AuthorityMode::Solo, VisibilityMode::Cockpit, true, false, false},
		{AuthorityMode::Solo, VisibilityMode::Cockpit, false, true, false},
		{AuthorityMode::Solo, VisibilityMode::TrustedFullState, false, false, false},
		{AuthorityMode::Solo, VisibilityMode::Cockpit, false, false, true},
	}};
	for (const auto& input : ineligible) {
		selected = Phase2Profile::CompleteShip;
		SCOPED_TRACE(static_cast<std::uint8_t>(input.authority_mode));
		EXPECT_NE(Phase2ProfileError::None,
			select_phase2_profile(input, Phase2Profile::CompleteShip, selected));
		EXPECT_EQ(Phase2Profile::None, selected);
	}
}

TEST(TelemetryPhase2ProfileGate, EveryEligibilityRejectionHasItsClosedObservableReason)
{
	using telemetry::protocol::AuthorityMode;
	using telemetry::protocol::VisibilityMode;

	struct Rejection {
		Phase2ProfileEligibility eligibility;
		Phase2Profile requested;
		Phase2ProfileError expected;
	};
	constexpr std::array<Rejection, 7> rejections{{
		{{AuthorityMode::Solo, VisibilityMode::Cockpit, false, false, false},
			Phase2Profile::None, Phase2ProfileError::UnsupportedProfile},
		{{AuthorityMode::MultiplayerClient, VisibilityMode::Cockpit, false, false, false},
			Phase2Profile::CompleteShip, Phase2ProfileError::UnsupportedAuthority},
		{{AuthorityMode::Solo, VisibilityMode::TrustedFullState, false, false, false},
			Phase2Profile::CompleteShip, Phase2ProfileError::UnsupportedVisibility},
		{{AuthorityMode::Solo, VisibilityMode::Cockpit, true, false, false},
			Phase2Profile::CompleteShip, Phase2ProfileError::TrustedFullStateNotAllowed},
		{{AuthorityMode::Solo, VisibilityMode::Cockpit, false, true, false},
			Phase2Profile::CompleteShip, Phase2ProfileError::DedicatedNotAllowed},
		{{AuthorityMode::Solo, VisibilityMode::Cockpit, false, false, true},
			Phase2Profile::CompleteShip, Phase2ProfileError::HeadlessNotAllowed},
		{{AuthorityMode::MultiplayerMaster, VisibilityMode::TrustedFullState, true, true, true},
			Phase2Profile::CompleteShip, Phase2ProfileError::UnsupportedAuthority},
	}};

	for (const auto& rejection : rejections) {
		Phase2Profile selected = Phase2Profile::CompleteShip;
		SCOPED_TRACE(static_cast<std::uint8_t>(rejection.expected));
		EXPECT_EQ(rejection.expected,
			select_phase2_profile(rejection.eligibility, rejection.requested, selected));
		EXPECT_EQ(Phase2Profile::None, selected);
	}
}

TEST(TelemetryPhase2ProfileGate, S11TST008EligibilityMatrixSelectsOnlySoloCockpitUntrusted)
{
	using telemetry::protocol::AuthorityMode;
	using telemetry::protocol::VisibilityMode;

	struct Case {
		Phase2ProfileEligibility eligibility;
		Phase2ProfileError expected_error;
		Phase2Profile expected_profile;
	};
	constexpr std::array<Case, 7> cases{{
		{{AuthorityMode::Solo, VisibilityMode::Cockpit, false, false, false},
			Phase2ProfileError::None, Phase2Profile::CompleteShip},
		{{AuthorityMode::MultiplayerClient, VisibilityMode::Cockpit, false, false, false},
			Phase2ProfileError::UnsupportedAuthority, Phase2Profile::None},
		{{AuthorityMode::MultiplayerMaster, VisibilityMode::Cockpit, false, false, false},
			Phase2ProfileError::UnsupportedAuthority, Phase2Profile::None},
		{{AuthorityMode::Solo, VisibilityMode::TrustedFullState, false, false, false},
			Phase2ProfileError::UnsupportedVisibility, Phase2Profile::None},
		{{AuthorityMode::Solo, VisibilityMode::Cockpit, true, false, false},
			Phase2ProfileError::TrustedFullStateNotAllowed, Phase2Profile::None},
		{{AuthorityMode::Solo, VisibilityMode::Cockpit, false, true, false},
			Phase2ProfileError::DedicatedNotAllowed, Phase2Profile::None},
		{{AuthorityMode::Solo, VisibilityMode::Cockpit, false, false, true},
			Phase2ProfileError::HeadlessNotAllowed, Phase2Profile::None},
	}};

	static_assert(noexcept(select_phase2_profile(
		std::declval<const Phase2ProfileEligibility&>(),
		Phase2Profile::CompleteShip,
		std::declval<Phase2Profile&>())));
	static_assert(std::is_trivially_destructible_v<Phase2ProfileEligibility>);

	for (const auto& test_case : cases) {
		Phase2Profile selected = Phase2Profile::CompleteShip;
		SCOPED_TRACE(static_cast<std::uint8_t>(test_case.expected_error));
		EXPECT_EQ(test_case.expected_error,
			select_phase2_profile(
				test_case.eligibility, Phase2Profile::CompleteShip, selected));
		EXPECT_EQ(test_case.expected_profile, selected);
	}
}

} // namespace
