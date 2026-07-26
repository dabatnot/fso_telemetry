#include "telemetry/phase2_profile_gate.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

namespace telemetry {
namespace {

constexpr std::uint64_t CoreGateCoverage =
	protocol::StateDomainCoverageBitPlayerKinematics | protocol::StateDomainCoverageBitCoreShip;
constexpr std::uint64_t CompleteShipCoverage = CoreGateCoverage |
	protocol::StateDomainCoverageBitControlInputs | protocol::StateDomainCoverageBitWeapons |
	protocol::StateDomainCoverageBitCargoDockSupport;

static_assert(CoreGateCoverage == 0x0401ULL, "The Phase 2 core gate coverage is frozen");
static_assert(CompleteShipCoverage == 0x0583ULL, "The Phase 2 complete ship coverage is frozen");
static_assert((CompleteShipCoverage & ~protocol::KnownStateDomainCoverageBitsV1_1) == 0U,
	"Phase 2 profiles must use only existing FSTL 1.1 state domains");
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
	default:
		return Phase2Profile::None;
	}
}

Phase2ProfileError validate_phase2_profile_coverage(std::uint64_t coverage, Phase2Profile& profile) noexcept
{
	profile = phase2_profile_from_coverage(coverage);
	return profile == Phase2Profile::None ? Phase2ProfileError::UnsupportedCoverage : Phase2ProfileError::None;
}

Phase2ProfileError select_phase2_profile(const Phase2ProfileEligibility& eligibility,
	Phase2Profile requested,
	Phase2Profile& selected) noexcept
{
	selected = Phase2Profile::None;
	if (requested != Phase2Profile::CoreGate && requested != Phase2Profile::CompleteShip) {
		return Phase2ProfileError::UnsupportedProfile;
	}
	if (eligibility.authority_mode != protocol::AuthorityMode::Solo) {
		return Phase2ProfileError::UnsupportedAuthority;
	}
	if (eligibility.visibility_mode != protocol::VisibilityMode::Cockpit) {
		return Phase2ProfileError::UnsupportedVisibility;
	}
	if (eligibility.trusted_full_state) {
		return Phase2ProfileError::TrustedFullStateNotAllowed;
	}
	if (eligibility.dedicated) {
		return Phase2ProfileError::DedicatedNotAllowed;
	}
	if (eligibility.headless) {
		return Phase2ProfileError::HeadlessNotAllowed;
	}
	selected = requested;
	return Phase2ProfileError::None;
}

} // namespace telemetry
