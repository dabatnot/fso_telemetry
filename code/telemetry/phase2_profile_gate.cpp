#include "telemetry/phase2_profile_gate.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

namespace telemetry {
namespace {

constexpr std::uint64_t CoreGateCoverage =
	protocol::StateDomainCoverageBitPlayerKinematics | protocol::StateDomainCoverageBitCoreShip;
constexpr std::uint64_t CompleteShipCoverage = CoreGateCoverage |
	protocol::StateDomainCoverageBitControlInputs | protocol::StateDomainCoverageBitWeapons |
	protocol::StateDomainCoverageBitCargoDockSupport;
constexpr std::uint64_t CockpitSensorsCoverage = CompleteShipCoverage |
	protocol::StateDomainCoverageBitRadarSensors |
	protocol::StateDomainCoverageBitTargeting |
	protocol::StateDomainCoverageBitNavigation;

static_assert(CoreGateCoverage == 0x0401ULL, "The Phase 2 core gate coverage is frozen");
static_assert(CompleteShipCoverage == 0x0583ULL, "The Phase 2 complete ship coverage is frozen");
static_assert(CockpitSensorsCoverage == 0x07cbULL, "The Phase 3 cockpit sensor coverage is frozen");
static_assert((CompleteShipCoverage & ~protocol::KnownStateDomainCoverageBits) == 0U,
	"Phase 2 profiles must use only existing FSTL 1.1 state domains");
static_assert((CockpitSensorsCoverage & ~protocol::KnownStateDomainCoverageBits) == 0U,
	"The Phase 3 profile must use only existing FSTL 1.1 state domains");
static_assert(static_cast<std::uint8_t>(Phase2ProfileError::Count) >
		static_cast<std::uint8_t>(Phase2ProfileError::None),
	"The private Phase 2 profile error registry must not be empty");

} // namespace

std::uint64_t phase2_profile_coverage(Phase2Profile profile) noexcept
{
	switch (profile) {
	case Phase2Profile::CoreGate:
		return CoreGateCoverage;
	case Phase2Profile::CompleteShip:
		return CompleteShipCoverage;
	case Phase2Profile::CockpitSensors:
		return CockpitSensorsCoverage;
	case Phase2Profile::None:
	default:
		return protocol::StateDomainCoverageBitNone;
	}
}

Phase2Profile phase2_profile_from_coverage(std::uint64_t coverage) noexcept
{
	switch (coverage) {
	case CoreGateCoverage:
		return Phase2Profile::CoreGate;
	case CompleteShipCoverage:
		return Phase2Profile::CompleteShip;
	case CockpitSensorsCoverage:
		return Phase2Profile::CockpitSensors;
	default:
		return Phase2Profile::None;
	}
}

Phase2ProfileError validate_phase2_profile_coverage(std::uint64_t coverage, Phase2Profile& profile) noexcept
{
	profile = phase2_profile_from_coverage(coverage);
	return profile == Phase2Profile::None ? Phase2ProfileError::UnsupportedCoverage : Phase2ProfileError::None;
}

Phase2ProfileError validate_cockpit_sensor_producer(
	const Phase2ProfileEligibility& eligibility) noexcept
{
	if (eligibility.authority_mode != protocol::AuthorityMode::Solo) {
		return Phase2ProfileError::UnsupportedAuthority;
	}
	if (eligibility.dedicated) {
		return Phase2ProfileError::DedicatedNotAllowed;
	}
	if (eligibility.headless) {
		return Phase2ProfileError::HeadlessNotAllowed;
	}
	return Phase2ProfileError::None;
}

} // namespace telemetry
