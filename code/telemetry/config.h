#pragma once

#include "telemetry/protocol/telemetry_security.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace telemetry {

constexpr std::size_t MaximumBindAddresses = 2;
constexpr std::size_t MaximumAllowedClients = 32;
constexpr std::size_t MaximumTelemetryConfigBytes = 16U * 1024U;

enum class VisibilityMode : std::uint8_t {
	Cockpit = 0,
};

// A numeric address without a port. Keeping the parsed binary form prevents
// alternate IPv6 spellings from bypassing duplicate and exposure checks.
class NumericIpAddress final {
  public:
	NumericIpAddress() noexcept = default;

	static NumericIpAddress from_ipv4(const std::array<std::uint8_t, 4>& address) noexcept;
	static NumericIpAddress from_ipv6(const std::array<std::uint8_t, 16>& address) noexcept;

	protocol::IpAddressFamily family() const noexcept
	{
		return m_family;
	}
	const std::array<std::uint8_t, 16>& bytes() const noexcept
	{
		return m_bytes;
	}
	bool is_valid() const noexcept;
	bool is_loopback() const noexcept;
	bool is_wildcard() const noexcept;

	friend bool operator==(const NumericIpAddress& lhs, const NumericIpAddress& rhs) noexcept;

  private:
	protocol::IpAddressFamily m_family = protocol::IpAddressFamily::Invalid;
	std::array<std::uint8_t, 16> m_bytes{};
};

enum class BindAddressAddResult : std::uint8_t {
	Added = 0,
	Duplicate,
	Invalid,
	CapacityExceeded,
};

class BindAddressList final {
  public:
	BindAddressAddResult add(const NumericIpAddress& address) noexcept;
	void clear() noexcept;

	std::size_t size() const noexcept
	{
		return m_size;
	}
	bool empty() const noexcept
	{
		return m_size == 0;
	}
	const NumericIpAddress& operator[](std::size_t index) const noexcept;

  private:
	std::array<NumericIpAddress, MaximumBindAddresses> m_entries{};
	std::size_t m_size = 0;
};

struct TelemetryConfig {
	TelemetryConfig() noexcept;

	std::uint8_t schema_version = 4;
	bool enabled = false;
	BindAddressList bind_addresses;
	std::uint16_t bind_port = protocol::DefaultTelemetryPort;
	protocol::SourceAllowlist allowed_clients;
	bool discovery_enabled = false;
	VisibilityMode visibility_mode = VisibilityMode::Cockpit;
	std::uint8_t max_clients = 1;
	std::uint8_t flight_hz = 30;
	std::uint8_t systems_hz = 10;
	std::uint8_t keyframe_seconds = 2;
	std::uint16_t mission_heartbeat_ms = 500;
	std::uint16_t idle_heartbeat_ms = 1000;
	std::uint16_t max_datagrams_per_tick = 64;
};

enum class ConfigStatus : std::uint8_t {
	Absent = 0,
	ValidDisabled,
	ValidEnabled,
	Invalid,
};

// Closed diagnostic values only. No parser text, path or input-controlled data
// crosses this boundary.
enum class ConfigError : std::uint8_t {
	None = 0,
	FileTooLarge,
	ReadFailure,
	InvalidJson,
	MaximumDepthExceeded,
	RootNotObject,
	UnknownKey,
	MissingSchemaVersion,
	InvalidType,
	OutOfRange,
	InvalidAddress,
	DuplicateAddress,
	InvalidCidr,
	DuplicateCidr,
	UnsafeExposure,
};

struct ConfigLoadResult {
	ConfigStatus status = ConfigStatus::Absent;
	TelemetryConfig effective;
	ConfigError error = ConfigError::None;
};

ConfigLoadResult load_telemetry_config() noexcept;

namespace detail {

enum class ConfigLocationKind : std::uint8_t {
	Absent = 0,
	LooseUserRoot,
	LooseGameRoot,
	LooseActiveMod,
	VpOnly,
};

struct ConfigLocationObservation {
	ConfigLocationKind kind = ConfigLocationKind::Absent;
	std::size_t offset = 0;
	std::string_view bytes;
};

ConfigLoadResult parse_telemetry_config_json(std::string_view input) noexcept;
ConfigLoadResult load_telemetry_config_from_observation(const ConfigLocationObservation& observation) noexcept;

} // namespace detail

} // namespace telemetry
