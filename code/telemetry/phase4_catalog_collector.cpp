#include "telemetry/phase4_catalog_collector.h"

#if !defined(FSO_TELEMETRY_TEST_SEAMS)
#include "object/object.h"
#include "ship/ship.h"
#include "telemetry/phase2_catalog_projection.h"

#include <limits>
#include <memory>
#include <new>

namespace telemetry::detail {
namespace {

Phase4CatalogDefinitionReadStatus map_source_read(
	Phase2SourceReadStatus status) noexcept
{
	switch (status) {
	case Phase2SourceReadStatus::Valid:
		return Phase4CatalogDefinitionReadStatus::Read;
	case Phase2SourceReadStatus::SourceLimitExceeded:
		return Phase4CatalogDefinitionReadStatus::SourceLimitExceeded;
	default:
		return Phase4CatalogDefinitionReadStatus::InvalidDefinition;
	}
}

Phase4CatalogDefinitionReadStatus map_projection(
	Phase2CatalogProjectionStatus status) noexcept
{
	switch (status) {
	case Phase2CatalogProjectionStatus::Success:
		return Phase4CatalogDefinitionReadStatus::Read;
	case Phase2CatalogProjectionStatus::SourceTemporarilyUnavailable:
		return Phase4CatalogDefinitionReadStatus::NotReady;
	case Phase2CatalogProjectionStatus::SourceLimitExceeded:
		return Phase4CatalogDefinitionReadStatus::SourceLimitExceeded;
	default:
		return Phase4CatalogDefinitionReadStatus::InvalidDefinition;
	}
}

class FsoPhase4CatalogDefinitionReadView final
	: public Phase4CatalogDefinitionReadView {
  public:
	explicit FsoPhase4CatalogDefinitionReadView(
		const FsoEngineReadView& engine) noexcept
		: m_engine(engine),
		  m_ship_source(new (std::nothrow) Phase2ShipSource()),
		  m_observation(new (std::nothrow) Phase2ObservationDto())
	{
	}

	Phase4CatalogDefinitionReadStatus read_ship_definition(
		const Phase4EngineInventoryEntry& inventory_entry,
		Phase2ManifestSource& output) const noexcept override
	{
		if (!ready()) return Phase4CatalogDefinitionReadStatus::NotReady;
		if (inventory_entry.identity.object_type != protocol::ObjectType::Ship ||
			inventory_entry.identity.stable_identity == 0U ||
			inventory_entry.identity.stable_identity >
				std::numeric_limits<std::uint32_t>::max() ||
			inventory_entry.source_class_key == 0U)
			return Phase4CatalogDefinitionReadStatus::InvalidDefinition;

		const object* matched = nullptr;
		for (auto* source = GET_FIRST(&obj_used_list);
			 source != END_OF_LIST(&obj_used_list); source = GET_NEXT(source)) {
			if (source->type != OBJ_SHIP || source->signature <= 0 ||
				static_cast<std::uint64_t>(source->signature) !=
					inventory_entry.identity.stable_identity)
				continue;
			if (matched != nullptr)
				return Phase4CatalogDefinitionReadStatus::InvalidDefinition;
			matched = source;
		}
		if (matched == nullptr)
			return Phase4CatalogDefinitionReadStatus::MissingDefinition;
		if (matched->instance < 0 || matched->instance >= MAX_SHIPS ||
			Ships[matched->instance].objnum != OBJ_INDEX(matched) ||
			Ships[matched->instance].ship_info_index < 0 ||
			static_cast<std::uint32_t>(Ships[matched->instance].ship_info_index) + 1U !=
				inventory_entry.source_class_key)
			return Phase4CatalogDefinitionReadStatus::InvalidDefinition;

		const EngineEntityKey key{OBJ_INDEX(matched),
			static_cast<std::uint32_t>(matched->signature)};
		const auto read = m_engine.read_ship(key, *m_ship_source);
		if (read.status != Phase2SourceReadStatus::Valid)
			return map_source_read(read.status);
		m_observation->ships.clear();
		m_observation->capture = {
			Phase2CaptureStatus::Valid, Phase2CaptureReason::None};
		if (!m_observation->raw_static_catalog.copy_from(
				m_ship_source->raw_static_catalog))
			return Phase4CatalogDefinitionReadStatus::SourceLimitExceeded;
		return map_projection(project_phase2_catalog(*m_observation, output));
	}

	Phase4CatalogDefinitionReadStatus read_weapon_definition(
		std::uint32_t source_key,
		Phase2ManifestSource& output) const noexcept override
	{
		if (!ready()) return Phase4CatalogDefinitionReadStatus::NotReady;
		m_observation->ships.clear();
		m_observation->capture = {
			Phase2CaptureStatus::Valid, Phase2CaptureReason::None};
		const auto read = m_engine.read_phase4_weapon_static(
			source_key, m_observation->raw_static_catalog);
		if (read.status == Phase2SourceReadStatus::InvalidSource)
			return Phase4CatalogDefinitionReadStatus::MissingDefinition;
		if (read.status != Phase2SourceReadStatus::Valid)
			return map_source_read(read.status);
		return map_projection(project_phase2_catalog(*m_observation, output));
	}

  private:
	bool ready() const noexcept
	{
		return m_ship_source != nullptr && m_observation != nullptr &&
			m_engine.current_thread_is_main() && m_engine.in_mission();
	}

	const FsoEngineReadView& m_engine;
	std::unique_ptr<Phase2ShipSource> m_ship_source;
	std::unique_ptr<Phase2ObservationDto> m_observation;
};

} // namespace

Phase4CatalogAssemblyStatus collect_phase4_catalog_definitions(
	const FsoEngineReadView& engine,
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase2ManifestSource& output) noexcept
{
	if (!engine.current_thread_is_main() || !engine.in_mission())
		return Phase4CatalogAssemblyStatus::NotReady;
	FsoPhase4CatalogDefinitionReadView reader(engine);
	return assemble_phase4_catalog_definitions(reader, inventory, output);
}

} // namespace telemetry::detail
#else
namespace telemetry::detail {

Phase4CatalogAssemblyStatus collect_phase4_catalog_definitions(
	const FsoEngineReadView&,
	const std::vector<Phase4EngineInventoryEntry>&,
	Phase2ManifestSource&) noexcept
{
	return Phase4CatalogAssemblyStatus::NotReady;
}

} // namespace telemetry::detail
#endif
