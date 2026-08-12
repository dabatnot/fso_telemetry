#include "telemetry/phase4_ship_state_scope.h"

#include <gtest/gtest.h>

namespace {

using telemetry::detail::Phase4EntityTypeBinding;
using telemetry::detail::Phase4ShipStateScopeStatus;
using telemetry::detail::validate_phase4_ship_state_scope;
using telemetry::protocol::ObjectType;
using telemetry::protocol::RecordType;
using telemetry::protocol::StateAtom;

StateAtom atom(RecordType type, std::uint64_t entity_id)
{
	StateAtom result;
	result.key.record_type = static_cast<std::uint16_t>(type);
	for (std::size_t index = 0U; index < 8U; ++index)
		result.key.identity.push_back(static_cast<std::uint8_t>(entity_id >> (index * 8U)));
	return result;
}

TEST(TelemetryPhase4ShipStateScope, AllowsDetailedStatesOnlyForKnownShips)
{
	const std::vector<Phase4EntityTypeBinding> entities{{1U, ObjectType::Ship},
		{2U, ObjectType::Weapon}};
	const std::vector<StateAtom> records{atom(RecordType::DamageState, 1U),
		atom(RecordType::FlightState, 2U)};
	EXPECT_EQ(Phase4ShipStateScopeStatus::Valid,
		validate_phase4_ship_state_scope(records, entities));
}

TEST(TelemetryPhase4ShipStateScope, RejectsDetailedStateForWeaponOrUnknownEntity)
{
	const std::vector<Phase4EntityTypeBinding> entities{{1U, ObjectType::Ship},
		{2U, ObjectType::Weapon}};
	EXPECT_EQ(Phase4ShipStateScopeStatus::NonShipDetailedState,
		validate_phase4_ship_state_scope({atom(RecordType::WeaponState, 2U)}, entities));
	EXPECT_EQ(Phase4ShipStateScopeStatus::UnknownEntity,
		validate_phase4_ship_state_scope({atom(RecordType::ShieldState, 9U)}, entities));
}

} // namespace
