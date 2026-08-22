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

enum class CockpitProducerEligibilityError : std::uint8_t {
	None = 0,
	UnsupportedCoverage,
	UnsupportedAuthority,
	DedicatedNotAllowed,
	HeadlessNotAllowed,
	Count,
};

struct CockpitProducerEligibility {
	protocol::AuthorityMode authority_mode = protocol::AuthorityMode::Solo;
	bool dedicated = false;
	bool headless = false;
};

// Temporary aliases for the historical profile-gate tests and builders.
// Production runtime/controller code uses the cockpit-only names above.
using Phase2ProfileError = CockpitProducerEligibilityError;
using Phase2ProfileEligibility = CockpitProducerEligibility;

std::uint64_t phase2_profile_coverage(Phase2Profile profile) noexcept;
Phase2Profile phase2_profile_from_coverage(std::uint64_t coverage) noexcept;
CockpitProducerEligibilityError validate_phase2_profile_coverage(
	std::uint64_t coverage, Phase2Profile& profile) noexcept;
CockpitProducerEligibilityError validate_cockpit_sensor_producer(
	const CockpitProducerEligibility& eligibility) noexcept;

} // namespace telemetry
