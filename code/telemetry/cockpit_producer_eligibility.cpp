#include "telemetry/cockpit_producer_eligibility.h"

namespace telemetry {

CockpitProducerEligibilityError validate_cockpit_sensor_producer(
	const CockpitProducerEligibility& eligibility) noexcept
{
	if (eligibility.authority_mode != protocol::AuthorityMode::Solo)
		return CockpitProducerEligibilityError::UnsupportedAuthority;
	if (eligibility.dedicated)
		return CockpitProducerEligibilityError::DedicatedNotAllowed;
	if (eligibility.headless)
		return CockpitProducerEligibilityError::HeadlessNotAllowed;
	return CockpitProducerEligibilityError::None;
}

} // namespace telemetry
