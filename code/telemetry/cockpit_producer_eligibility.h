#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <cstdint>

namespace telemetry {

enum class CockpitProducerEligibilityError : std::uint8_t {
	None = 0,
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

CockpitProducerEligibilityError validate_cockpit_sensor_producer(
	const CockpitProducerEligibility& eligibility) noexcept;

} // namespace telemetry
