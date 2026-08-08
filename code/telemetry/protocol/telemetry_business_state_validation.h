#pragma once

#include "telemetry/protocol/telemetry_business_records.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

// Arrays supplied through this API are immutable, sorted by their numeric ID
// and unique. They represent catalogs already installed and ACK APPLIED before
// the snapshot that references them.
struct BusinessClassCatalogEntry {
	std::uint32_t class_id = 0;
	const std::uint32_t* subsystem_ids = nullptr;
	std::size_t subsystem_count = 0;
};

struct BusinessSessionInvariants {
	bool enforce = false;
	std::uint64_t producer_id = 0;
	AuthorityMode authority_mode = AuthorityMode::Solo;
	VisibilityMode visibility_mode = VisibilityMode::Cockpit;
	std::uint64_t state_domain_coverage = 0;
	std::uint64_t event_coverage_state_derived = 0;
	std::uint64_t event_coverage_exact = 0;
	std::uint64_t minimum_producer_sample_time_us = 0;
	std::uint32_t capability_generation = 0;
	std::uint64_t negotiated_capabilities = 0;
};

struct BusinessStateValidationContext {
	std::uint8_t protocol_minor = VersionMinor;
	std::uint32_t required_manifest_id = 0;
	bool trusted_full_state_authorized = false;
	bool source_endpoint_allowlisted = false;
	bool communication_exact_hook_available = false;
	// Current CockpitSensors sessions require HUD_ALERT_STATE. Replay callers
	// leave this false when loading captures produced by an older Phase 3
	// contract fingerprint.
	bool require_hud_alert_state = false;
	std::uint64_t exact_event_hook_families = 0;
	bool class_manifest_installed = false;
	bool weapon_manifest_installed = false;
	bool enforce_negotiated_capabilities = false;
	std::uint64_t negotiated_capabilities = 0;

	const BusinessClassCatalogEntry* class_catalog = nullptr;
	std::size_t class_catalog_count = 0;
	const std::uint32_t* weapon_class_ids = nullptr;
	// Parallel to weapon_class_ids. A catalog used by a snapshot containing a
	// weapon entity must provide the exact frozen WeaponClassFlags value so the
	// redundant ENTITY_LIFECYCLE/RADAR bomb bits can be checked transactionally.
	const std::uint64_t* weapon_class_flags = nullptr;
	std::size_t weapon_class_count = 0;

	// When enabled in Cockpit mode, every serialized entity owner must occur in
	// this sorted allowlist. An empty list then means that no entity is visible.
	bool enforce_cockpit_entity_allowlist = false;
	const std::uint64_t* cockpit_entity_ids = nullptr;
	std::size_t cockpit_entity_count = 0;

	BusinessSessionInvariants previous_session;
};

// Transaction-level semantic validator for an already structurally decoded
// immutable snapshot. It checks catalog references, record scopes, coverage,
// visibility ownership and session invariants without consulting FS2Open.
class BusinessStateImageValidator final : public StateImageValidator {
  public:
	explicit BusinessStateImageValidator(const BusinessStateValidationContext& context) noexcept;
	ValidationError validate(const StateImage& image) const noexcept override;
	ValidationError validate_delta_transition(const StateImage& baseline,
		const StateImage& candidate) const noexcept override;
	std::uint8_t protocol_minor() const noexcept override
	{
		return m_context.protocol_minor;
	}

  private:
	const BusinessStateValidationContext& m_context;
};

} // namespace telemetry::protocol
