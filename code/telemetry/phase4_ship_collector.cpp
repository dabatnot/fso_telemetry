#include "telemetry/phase4_ship_collector.h"

#include "globalincs/linklist.h"
#include "object/object.h"
#include "telemetry/phase2_state_image.h"

#include <new>

namespace telemetry::detail {
namespace {
bool inherited(protocol::RecordType type) noexcept {
	// Phase 4 owns FLIGHT_STATE for every positionable entity. Every other
	// detailed record in the inherited ship range remains part of a SHIP's
	// complete state, including records that become present in a later Phase 2
	// capture capability.
	return type >= protocol::RecordType::ShipIdentity &&
		type <= protocol::RecordType::EffectState &&
		type != protocol::RecordType::FlightState;
}
bool key_for(std::uint64_t signature, EngineEntityKey& key) noexcept {
	if (signature == 0U || signature > UINT32_MAX) return false;
	for (auto* value = GET_FIRST(&obj_used_list); value != END_OF_LIST(&obj_used_list); value = GET_NEXT(value))
		if (value->type == OBJ_SHIP && value->signature > 0 && static_cast<std::uint64_t>(value->signature) == signature) {
			key = {OBJ_INDEX(value), static_cast<std::uint32_t>(value->signature)};
			return true;
		}
	return false;
}
void stamp(ShipObservationDto& ship, std::uint64_t time) noexcept {
	ship.identity.sample_time_us = time; ship.lifecycle.sample_time_us = time; ship.flight.sample_time_us = time;
	ship.damage.sample_time_us = time; ship.shields.sample_time_us = time; ship.energy.sample_time_us = time;
	ship.propulsion.sample_time_us = time; ship.weapons.sample_time_us = time;
	for (std::size_t i = 0U; i < ship.subsystems.count; ++i) ship.subsystems.values[i].sample_time_us = time;
}
} // namespace

Phase4ShipCollectorStatus collect_phase4_ship_records(const FsoEngineReadView& engine,
	const std::vector<Phase4EngineInventoryEntry>& inventory, const Phase2ManifestCandidate* manifest,
	std::uint64_t sample_time_us, std::vector<protocol::StateAtom>& output) noexcept
{
	if (!engine.current_thread_is_main() || !engine.in_mission()) return Phase4ShipCollectorStatus::NotReady;
	if (manifest == nullptr || manifest->manifest_id == 0U) return Phase4ShipCollectorStatus::MissingManifest;
	try {
		std::vector<protocol::StateAtom> candidate;
		for (const auto& entry : inventory) {
			if (entry.identity.object_type != protocol::ObjectType::Ship) continue;
			if (entry.entity_id == 0U || entry.source_class_key == 0U) return Phase4ShipCollectorStatus::InvalidInventory;
			EngineEntityKey key{}; if (!key_for(entry.identity.stable_identity, key)) return Phase4ShipCollectorStatus::ReadFailure;
			Phase2ShipSource source{};
			if (engine.read_ship(key, source).status != Phase2SourceReadStatus::Valid) return Phase4ShipCollectorStatus::ReadFailure;
			Phase2ObservationDto observation{};
			observation.capture = {Phase2CaptureStatus::Valid, Phase2CaptureReason::None}; observation.producer_sample_time_us = sample_time_us;
			observation.player_key = {1U}; observation.ships.resize(1U);
			auto& ship = observation.ships[0]; ship.capture_key = {1U}; ship.identity = source.identity;
			ship.identity.class_source_key = {entry.source_class_key}; ship.lifecycle = source.lifecycle; ship.flight = source.flight;
			ship.damage = source.damage; ship.shields = source.shields; ship.energy = source.energy; ship.propulsion = source.propulsion;
			ship.weapons = source.weapons; ship.subsystems = source.subsystems;
			// The catalogue assigns the SHIP an effective source key.  All
			// member/subsystem and weapon keys remain their catalogue keys; only
			// the class identity itself is remapped from the engine class.
			ship.raw_static_references = source.raw_static_references;
			ship.raw_static_references.class_capture_key = entry.source_class_key;
			observation.raw_static_catalog = source.raw_static_catalog;
			for (std::uint32_t class_index = 0U;
				 class_index < observation.raw_static_catalog.class_count;
				 ++class_index)
				if (observation.raw_static_catalog.class_definitions[class_index]
					.class_capture_key == source.identity.class_source_key.value)
					observation.raw_static_catalog.class_definitions[class_index]
						.class_capture_key = entry.source_class_key;
			stamp(ship, sample_time_us);
			Phase2Wp05SubjectBinding binding{{1U}, entry.entity_id};
			Phase2CompleteDomainInput input{}; input.producer_id = 1U; input.negotiated_capability_generation = 1U;
			input.mission.mission_generation = 1U; input.mission.producer_sample_time_us = sample_time_us;
			input.observation = &observation; input.installed_manifest = manifest; input.subjects = &binding;
			input.subject_count = 1U; input.player_entity_id = entry.entity_id;
			protocol::StateImage image{};
			if (build_phase2_complete_domain(input, image) != Phase2StateImageBuildStatus::Created) return Phase4ShipCollectorStatus::ProjectionFailure;
			for (const auto& atom : image.records()) if (inherited(static_cast<protocol::RecordType>(atom.key.record_type))) candidate.push_back(atom);
			if (candidate.size() > protocol::MaxReplicationStateAtomCount) return Phase4ShipCollectorStatus::CapacityExceeded;
		}
		output = std::move(candidate); return Phase4ShipCollectorStatus::Collected;
	} catch (const std::bad_alloc&) { return Phase4ShipCollectorStatus::CapacityExceeded; }
}
} // namespace telemetry::detail
