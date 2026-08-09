#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <cstdint>

namespace telemetry {

enum class Phase2Profile : std::uint8_t {
	None = 0,
	CoreGate,
	CompleteShip,
	CockpitSensors,
};

enum class Phase2ProfileError : std::uint8_t {
	None = 0,
	UnsupportedCoverage,
	UnsupportedProfile,
	UnsupportedAuthority,
	UnsupportedVisibility,
	TrustedFullStateNotAllowed,
	DedicatedNotAllowed,
	HeadlessNotAllowed,
	Count,
};

struct Phase2ProfileEligibility {
	protocol::AuthorityMode authority_mode = protocol::AuthorityMode::Solo;
	protocol::VisibilityMode visibility_mode = protocol::VisibilityMode::Cockpit;
	bool trusted_full_state = false;
	bool dedicated = false;
	bool headless = false;
};

std::uint64_t phase2_profile_coverage(Phase2Profile profile) noexcept;
Phase2Profile phase2_profile_from_coverage(std::uint64_t coverage) noexcept;
Phase2ProfileError validate_phase2_profile_coverage(std::uint64_t coverage, Phase2Profile& profile) noexcept;
Phase2ProfileError select_phase2_profile(const Phase2ProfileEligibility& eligibility,
	Phase2Profile requested,
	Phase2Profile& selected) noexcept;

} // namespace telemetry
