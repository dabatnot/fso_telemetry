#pragma once

#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_session_context.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::uint16_t DefaultTelemetryPort = 42042;
constexpr std::size_t MaximumSourceAllowlistEntries = 64;
constexpr std::size_t MaximumDiscoveryDestinations = 16;
constexpr std::size_t MaximumConfiguredClients = 64;

// LoopbackAndAllowlisted keeps the safe loopback listeners and additionally
// permits explicitly configured non-loopback listeners. It never means
// wildcard access: each received non-loopback source must still match the
// allowlist before any datagram byte is parsed.
enum class NetworkBindMode : std::uint8_t {
	LoopbackOnly = 0,
	LoopbackAndAllowlisted = 1,
};

class IpCidr final {
  public:
	IpCidr() noexcept = default;

	static bool from_ipv4(const std::array<std::uint8_t, 4>& address,
		std::uint8_t prefix_length,
		IpCidr& cidr) noexcept;
	static bool from_ipv6(const std::array<std::uint8_t, 16>& address,
		std::uint8_t prefix_length,
		IpCidr& cidr) noexcept;

	bool is_valid() const noexcept;
	bool contains(const EndpointKey& endpoint) const noexcept;
	IpAddressFamily family() const noexcept
	{
		return m_family;
	}
	std::uint8_t prefix_length() const noexcept
	{
		return m_prefix_length;
	}
	const std::array<std::uint8_t, 16>& network() const noexcept
	{
		return m_network;
	}

	friend bool operator==(const IpCidr& lhs, const IpCidr& rhs) noexcept;

  private:
	IpAddressFamily m_family = IpAddressFamily::Invalid;
	std::uint8_t m_prefix_length = 0;
	std::array<std::uint8_t, 16> m_network{};
};

enum class AllowlistAddResult : std::uint8_t {
	Added,
	AlreadyPresent,
	InvalidCidr,
	CapacityExceeded,
};

// A fixed-capacity, allocation-free set. Ports are deliberately ignored:
// allowlisting is by received source address/CIDR, while the session layer
// subsequently binds the exact address and UDP port.
class SourceAllowlist final {
  public:
	AllowlistAddResult add(const IpCidr& cidr) noexcept;
	bool contains(const EndpointKey& endpoint) const noexcept;
	void clear() noexcept;

	std::size_t size() const noexcept
	{
		return m_size;
	}
	bool empty() const noexcept
	{
		return m_size == 0;
	}
	const IpCidr& operator[](std::size_t index) const noexcept;

  private:
	std::array<IpCidr, MaximumSourceAllowlistEntries> m_entries{};
	std::size_t m_size = 0;
};

struct TelemetryResourceBudgetConfig {
	// Phase 0 uses the normative per-client limits. These global capacities
	// must be large enough for every configured client to reach those limits;
	// the video capacity is inactive when target_video_enabled is false.
	std::size_t max_clients = 1;
	std::size_t global_state_reassembly_bytes = MaxStateReassemblyBytesPerClient;
	std::size_t global_video_reassembly_bytes = MaxVideoReassemblyBytesPerClient;
};

struct TelemetryOperationalConfig {
	bool enabled = false;
	std::uint16_t port = DefaultTelemetryPort;
	NetworkBindMode bind_mode = NetworkBindMode::LoopbackOnly;
	bool discovery_enabled = false;
	std::size_t discovery_destination_count = 0;
	bool trusted_full_state_enabled = false;
	bool target_video_enabled = false;
	bool target_video_renderer_validated = false;
	bool target_video_async_readback_validated = false;
	bool target_video_encoder_validated = false;
	SourceAllowlist source_allowlist;
	TelemetryResourceBudgetConfig resources;
};

enum class SecurityConfigurationError : std::uint8_t {
	None,
	ModuleDisabled,
	InvalidPort,
	InvalidBindMode,
	NonLoopbackRequiresAllowlist,
	DiscoveryRequiresDestination,
	TooManyDiscoveryDestinations,
	TrustedFullStateRequiresAllowlist,
	TargetVideoPrerequisitesMissing,
	InvalidClientLimit,
	ClientLimitExceeded,
	ArithmeticOverflow,
	GlobalStateBudgetTooSmall,
	GlobalVideoBudgetTooSmall,
	GlobalBudgetInUse,
};

struct TelemetryResourceBudgetTotals {
	std::size_t required_state_reassembly_bytes = 0;
	std::size_t required_video_reassembly_bytes = 0;
	std::size_t required_total_reassembly_bytes = 0;
	std::size_t configured_total_reassembly_bytes = 0;
};

SecurityConfigurationError validate_security_configuration(const TelemetryOperationalConfig& config,
	TelemetryResourceBudgetTotals& totals) noexcept;
bool telemetry_module_can_start(const TelemetryOperationalConfig& config,
	TelemetryResourceBudgetTotals& totals) noexcept;

bool is_loopback_source(const EndpointKey& endpoint) noexcept;
bool source_is_allowed(const TelemetryOperationalConfig& config, const EndpointKey& source_endpoint) noexcept;

// TrustedFullState needs both its explicit opt-in and an explicit endpoint
// allowlist match. The implicit loopback permission is intentionally not
// sufficient for this more permissive visibility mode.
bool trusted_full_state_is_authorized(const TelemetryOperationalConfig& config,
	const EndpointKey& source_endpoint) noexcept;

constexpr std::size_t ValidationErrorCount =
	static_cast<std::size_t>(ValidationError::InternalSerializationError) + 1U;

class TelemetryIngressCounters final {
  public:
	void record(std::size_t datagram_bytes, ValidationError result) noexcept;
	void clear() noexcept;

	std::uint64_t rejected(ValidationError error) const noexcept;
	std::uint64_t datagrams_received() const noexcept
	{
		return m_datagrams_received;
	}
	std::uint64_t bytes_received() const noexcept
	{
		return m_bytes_received;
	}
	std::uint64_t datagrams_accepted() const noexcept
	{
		return m_datagrams_accepted;
	}
	std::uint64_t bytes_accepted() const noexcept
	{
		return m_bytes_accepted;
	}
	std::uint64_t datagrams_dropped() const noexcept
	{
		return m_datagrams_dropped;
	}
	std::uint64_t bytes_dropped() const noexcept
	{
		return m_bytes_dropped;
	}

  private:
	std::uint64_t m_datagrams_received = 0;
	std::uint64_t m_bytes_received = 0;
	std::uint64_t m_datagrams_accepted = 0;
	std::uint64_t m_bytes_accepted = 0;
	std::uint64_t m_datagrams_dropped = 0;
	std::uint64_t m_bytes_dropped = 0;
	std::array<std::uint64_t, ValidationErrorCount> m_rejected_by_error{};
};

// Implements the normative receive ordering through static fragment-layout
// validation: source policy precedes all datagram reads; envelope validation
// precedes session/direction/endpoint validation; layout validation is last.
// No allocation or state mutation occurs in this function.
ValidationError decode_and_validate_ingress_datagram(const TelemetryOperationalConfig& config,
	const EndpointKey& source_endpoint,
	ByteView datagram,
	const TelemetrySessionContext& context,
	DatagramView& decoded,
	TelemetryIngressCounters& counters) noexcept;

enum class ProtocolAuthority : std::uint8_t {
	Invalid = 0,
	ReadOnlyTelemetry = 1,
};

// This exhaustive registry is the executable Phase 0 proof that no known
// wire message carries simulation-mutation authority.
ProtocolAuthority message_authority(MessageType type) noexcept;

enum class GlobalBudgetResult : std::uint8_t {
	Reserved,
	Exhausted,
	NoClient,
	InvalidClass,
	InvalidAmount,
};

struct GlobalReassemblyBudgetCounters {
	std::uint64_t client_admissions = 0;
	std::uint64_t client_admission_rejections = 0;
	std::uint64_t state_reservations = 0;
	std::uint64_t video_reservations = 0;
	std::uint64_t state_quota_rejections = 0;
	std::uint64_t video_quota_rejections = 0;
	std::size_t peak_state_reserved_bytes = 0;
	std::size_t peak_video_reserved_bytes = 0;
	std::size_t peak_active_clients = 0;
};

// Shared by all per-client reassemblers owned by one telemetry module. The
// owner serializes calls on its receive thread and must outlive every attached
// TelemetryReassembler.
class GlobalReassemblyBudget final {
  public:
	GlobalReassemblyBudget() noexcept = default;
	GlobalReassemblyBudget(const GlobalReassemblyBudget&) = delete;
	GlobalReassemblyBudget& operator=(const GlobalReassemblyBudget&) = delete;

	static SecurityConfigurationError configure(const TelemetryOperationalConfig& config,
		GlobalReassemblyBudget& budget) noexcept;

	bool try_register_client() noexcept;
	bool release_client() noexcept;
	GlobalBudgetResult try_reserve(MessageSizeClass message_class, std::size_t byte_count) noexcept;
	bool release(MessageSizeClass message_class, std::size_t byte_count) noexcept;

	std::size_t capacity_bytes(MessageSizeClass message_class) const noexcept;
	std::size_t reserved_bytes(MessageSizeClass message_class) const noexcept;
	std::size_t active_clients() const noexcept
	{
		return m_active_clients;
	}
	std::size_t client_capacity() const noexcept
	{
		return m_client_capacity;
	}
	const GlobalReassemblyBudgetCounters& counters() const noexcept
	{
		return m_counters;
	}

  private:
	std::size_t m_state_capacity = 0;
	std::size_t m_video_capacity = 0;
	std::size_t m_client_capacity = 0;
	std::size_t m_active_clients = 0;
	std::size_t m_state_reserved = 0;
	std::size_t m_video_reserved = 0;
	GlobalReassemblyBudgetCounters m_counters{};
};

} // namespace telemetry::protocol
