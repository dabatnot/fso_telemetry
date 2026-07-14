#pragma once

#include "telemetry/protocol/telemetry_control_messages.h"

#include <cstdint>

namespace telemetry::protocol {

constexpr std::uint64_t ClientOwnedCapabilities =
	CapabilityCommViewLocalAssets | CapabilityTargetVideoH264 | CapabilityUpdate;
constexpr std::uint64_t ProducerOwnedCapabilities =
	CapabilityCommViewAuthoritativeSource | CapabilityTargetVideoRemoteRender | CapabilityUpdate;
constexpr std::uint64_t CommViewCapabilityPair = CapabilityCommViewLocalAssets | CapabilityCommViewAuthoritativeSource;
constexpr std::uint64_t TargetVideoCapabilityPair = CapabilityTargetVideoH264 | CapabilityTargetVideoRemoteRender;
constexpr std::uint64_t VisualCapabilities = CommViewCapabilityPair | TargetVideoCapabilityPair;

enum class CapabilityPeerRole : std::uint8_t {
	Invalid = 0,
	Client = 1,
	Producer = 2,
};

constexpr std::uint64_t owned_capabilities(CapabilityPeerRole role) noexcept
{
	return role == CapabilityPeerRole::Client ? ClientOwnedCapabilities
											  : (role == CapabilityPeerRole::Producer ? ProducerOwnedCapabilities : 0);
}

// Receive-side capability bitmaps are extensible: unknown bits are ignored.
// Emit-side validation rejects them so a v1.0 implementation never relays or
// advertises a bit it does not understand.
constexpr std::uint64_t effective_capabilities(std::uint64_t received) noexcept
{
	return received & KnownCapabilities;
}

ValidationError validate_emittable_capability_offer(CapabilityPeerRole owner, std::uint64_t capabilities) noexcept;
ValidationError validate_emittable_active_capabilities(std::uint64_t active_capabilities) noexcept;

enum class CapabilityUpdateDisposition : std::uint8_t {
	Rejected = 0,
	Applied = 1,
	Obsolete = 2,
	DuplicateIdentical = 3,
	SameGenerationDifferent = 4,
	NotNegotiated = 5,
	RequiresNewSession = 6,
};

struct CapabilityUpdateOutcome {
	CapabilityUpdateDisposition disposition = CapabilityUpdateDisposition::Rejected;
	ValidationError error = ValidationError::InvalidStateTransition;
	std::uint64_t removed_active_capabilities = 0;

	bool should_ack_applied() const noexcept
	{
		return disposition == CapabilityUpdateDisposition::Applied ||
			   disposition == CapabilityUpdateDisposition::Obsolete ||
			   disposition == CapabilityUpdateDisposition::DuplicateIdentical;
	}

	std::uint64_t removed_visual_capabilities() const noexcept
	{
		return removed_active_capabilities & VisualCapabilities;
	}
};

// Bounded capability state for one session. It deliberately owns no session
// phase or canonical telemetry state: losing a visual pair can only appear in
// removed_active_capabilities and can never demote or close the session.
class CapabilitySessionState {
  public:
	void reset() noexcept;

	bool initialized() const noexcept
	{
		return m_initialized;
	}
	bool capability_updates_were_negotiated() const noexcept
	{
		return m_update_negotiated_initially;
	}
	std::uint64_t client_advertised_capabilities() const noexcept
	{
		return m_client.advertised;
	}
	std::uint64_t producer_advertised_capabilities() const noexcept
	{
		return m_producer.advertised;
	}
	std::uint64_t active_capabilities() const noexcept
	{
		return m_active;
	}
	std::uint64_t client_declared_active_capabilities() const noexcept
	{
		return m_client.declared_active;
	}
	std::uint64_t producer_declared_active_capabilities() const noexcept
	{
		return m_producer.declared_active;
	}

  private:
	struct PeerState {
		std::uint64_t advertised = 0;
		std::uint64_t declared_active = 0;
		std::uint32_t generation = 0;
		CapabilityUpdatePayload last_update{};
		bool has_last_update = false;
		bool update_channel_open = false;
	};

	PeerState m_client;
	PeerState m_producer;
	std::uint64_t m_active = 0;
	bool m_initialized = false;
	bool m_update_negotiated_initially = false;

	friend ValidationError
	initialize_capability_session(std::uint64_t, std::uint64_t, std::uint64_t, CapabilitySessionState&) noexcept;
	friend CapabilityUpdateOutcome
	apply_capability_update(CapabilitySessionState&, CapabilityPeerRole, const CapabilityUpdatePayload&) noexcept;
};

// Initializes state from a validated HELLO/WELCOME negotiation. Unknown offer
// and active bits are ignored, while known wrong-owner or incomplete-pair bits
// reject the negotiation. selected_active is the specialized negotiation
// result (for example after checking the communication bundle).
ValidationError initialize_capability_session(std::uint64_t client_advertised,
	std::uint64_t producer_advertised,
	std::uint64_t selected_active,
	CapabilitySessionState& state) noexcept;

// Producer-side convenience negotiation. compatible_visual_pairs contains
// only complete visual pairs whose specialized parameters are compatible.
ValidationError negotiate_initial_capabilities(std::uint64_t client_advertised,
	std::uint64_t producer_advertised,
	std::uint64_t compatible_visual_pairs,
	CapabilitySessionState& state) noexcept;

CapabilityUpdateOutcome apply_capability_update(CapabilitySessionState& state,
	CapabilityPeerRole sender,
	const CapabilityUpdatePayload& update) noexcept;

} // namespace telemetry::protocol
