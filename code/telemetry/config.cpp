#include "telemetry/config.h"

#include "telemetry/json_preflight.h"

#include "cfile/cfile.h"
#include "libs/jansson.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string_view>

namespace telemetry {

namespace {

constexpr char ConfigFilename[] = "telemetry.json";
constexpr std::size_t MaximumJsonContainerDepth = 4;

struct CFileCloser {
	void operator()(CFILE* file) const noexcept
	{
		if (file != nullptr) {
			cfclose(file);
		}
	}
};

constexpr const char* KnownConfigKeys[]{
	"schemaVersion",
	"enabled",
	"bindAddresses",
	"bindPort",
	"allowedClients",
	"discoveryEnabled",
	"visibilityMode",
	"trustedFullState",
	"maxClients",
	"flightHz",
	"keyframeSeconds",
	"missionHeartbeatMs",
	"idleHeartbeatMs",
	"maxDatagramsPerTick",
};

ConfigLoadResult absent_result() noexcept
{
	return ConfigLoadResult{};
}

ConfigLoadResult invalid_result(ConfigError error) noexcept
{
	ConfigLoadResult result;
	result.status = ConfigStatus::Invalid;
	result.error = error;
	return result;
}

ConfigLoadResult valid_result(const TelemetryConfig& config) noexcept
{
	ConfigLoadResult result;
	result.status = config.enabled ? ConfigStatus::ValidEnabled : ConfigStatus::ValidDisabled;
	result.effective = config;
	return result;
}

bool is_known_key(const char* key) noexcept
{
	return std::any_of(std::begin(KnownConfigKeys), std::end(KnownConfigKeys), [key](const char* known) {
		return std::strcmp(key, known) == 0;
	});
}

bool json_container_depth_is_valid(const json_t* value, std::size_t depth) noexcept
{
	if (!json_is_object(value) && !json_is_array(value)) {
		return true;
	}
	if (depth > MaximumJsonContainerDepth) {
		return false;
	}

	if (json_is_object(value)) {
		void* iterator = json_object_iter(const_cast<json_t*>(value));
		while (iterator != nullptr) {
			const auto* child = json_object_iter_value(iterator);
			if (!json_container_depth_is_valid(child, depth + 1U)) {
				return false;
			}
			iterator = json_object_iter_next(const_cast<json_t*>(value), iterator);
		}
		return true;
	}

	const auto count = json_array_size(value);
	for (std::size_t index = 0; index < count; ++index) {
		if (!json_container_depth_is_valid(json_array_get(value, index), depth + 1U)) {
			return false;
		}
	}
	return true;
}

bool read_bounded_integer(const json_t* object,
	const char* key,
	std::uint64_t minimum,
	std::uint64_t maximum,
	std::uint64_t& output,
	ConfigError& error) noexcept
{
	const auto* value = json_object_get(object, key);
	if (value == nullptr) {
		return true;
	}
	if (!json_is_integer(value)) {
		error = ConfigError::InvalidType;
		return false;
	}
	const auto integer = json_integer_value(value);
	if (integer < 0 || static_cast<std::uint64_t>(integer) < minimum ||
		static_cast<std::uint64_t>(integer) > maximum) {
		error = ConfigError::OutOfRange;
		return false;
	}
	output = static_cast<std::uint64_t>(integer);
	return true;
}

bool read_boolean(const json_t* object, const char* key, bool& output, ConfigError& error) noexcept
{
	const auto* value = json_object_get(object, key);
	if (value == nullptr) {
		return true;
	}
	if (!json_is_boolean(value)) {
		error = ConfigError::InvalidType;
		return false;
	}
	output = json_is_true(value) != 0;
	return true;
}

bool is_ipv4_mapped(const std::array<std::uint8_t, 16>& address) noexcept
{
	return std::all_of(address.begin(), address.begin() + 10, [](std::uint8_t byte) { return byte == 0U; }) &&
		address[10] == 0xffU && address[11] == 0xffU;
}

bool copy_json_string_to_c_buffer(const json_t* value, char* buffer, std::size_t capacity) noexcept
{
	if (!json_is_string(value)) {
		return false;
	}
	const auto length = json_string_length(value);
	const auto* text = json_string_value(value);
	if (length == 0U || length >= capacity || std::memchr(text, '\0', length) != nullptr) {
		return false;
	}
	std::memcpy(buffer, text, length);
	buffer[length] = '\0';
	return true;
}

bool parse_numeric_address(const json_t* value, NumericIpAddress& output) noexcept
{
	std::array<char, INET6_ADDRSTRLEN> text{};
	if (!copy_json_string_to_c_buffer(value, text.data(), text.size())) {
		return false;
	}

	in_addr ipv4{};
	if (inet_pton(AF_INET, text.data(), &ipv4) == 1) {
		std::array<std::uint8_t, 4> bytes{};
		std::memcpy(bytes.data(), &ipv4, bytes.size());
		output = NumericIpAddress::from_ipv4(bytes);
		return true;
	}

	in6_addr ipv6{};
	if (inet_pton(AF_INET6, text.data(), &ipv6) != 1) {
		return false;
	}
	std::array<std::uint8_t, 16> bytes{};
	std::memcpy(bytes.data(), &ipv6, bytes.size());
	if (is_ipv4_mapped(bytes)) {
		return false;
	}
	output = NumericIpAddress::from_ipv6(bytes);
	return true;
}

bool parse_prefix(std::string_view text, std::uint16_t maximum, std::uint8_t& prefix) noexcept
{
	if (text.empty() || (text.size() > 1U && text.front() == '0')) {
		return false;
	}
	std::uint16_t value = 0;
	for (const auto character : text) {
		if (character < '0' || character > '9') {
			return false;
		}
		value = static_cast<std::uint16_t>(value * 10U + static_cast<std::uint16_t>(character - '0'));
		if (value > maximum) {
			return false;
		}
	}
	prefix = static_cast<std::uint8_t>(value);
	return true;
}

bool parse_cidr(const json_t* value, protocol::IpCidr& output) noexcept
{
	if (!json_is_string(value)) {
		return false;
	}
	const auto length = json_string_length(value);
	const auto* raw_text = json_string_value(value);
	if (length == 0U || length >= INET6_ADDRSTRLEN + 4U || std::memchr(raw_text, '\0', length) != nullptr) {
		return false;
	}
	const std::string_view text{raw_text, length};
	const auto slash = text.find('/');
	if (slash == std::string_view::npos || slash == 0U || slash + 1U >= text.size() ||
		text.find('/', slash + 1U) != std::string_view::npos) {
		return false;
	}

	std::array<char, INET6_ADDRSTRLEN> address_text{};
	if (slash >= address_text.size()) {
		return false;
	}
	std::memcpy(address_text.data(), text.data(), slash);
	address_text[slash] = '\0';

	in_addr ipv4{};
	if (inet_pton(AF_INET, address_text.data(), &ipv4) == 1) {
		std::uint8_t prefix = 0;
		if (!parse_prefix(text.substr(slash + 1U), 32U, prefix)) {
			return false;
		}
		std::array<std::uint8_t, 4> bytes{};
		std::memcpy(bytes.data(), &ipv4, bytes.size());
		protocol::IpCidr candidate;
		if (!protocol::IpCidr::from_ipv4(bytes, prefix, candidate) ||
			!std::equal(bytes.begin(), bytes.end(), candidate.network().begin())) {
			return false;
		}
		output = candidate;
		return true;
	}

	in6_addr ipv6{};
	if (inet_pton(AF_INET6, address_text.data(), &ipv6) != 1) {
		return false;
	}
	std::uint8_t prefix = 0;
	if (!parse_prefix(text.substr(slash + 1U), 128U, prefix)) {
		return false;
	}
	std::array<std::uint8_t, 16> bytes{};
	std::memcpy(bytes.data(), &ipv6, bytes.size());
	protocol::IpCidr candidate;
	if (!protocol::IpCidr::from_ipv6(bytes, prefix, candidate) || candidate.network() != bytes) {
		return false;
	}
	output = candidate;
	return true;
}

bool parse_bind_addresses(const json_t* root, TelemetryConfig& config, ConfigError& error) noexcept
{
	const auto* value = json_object_get(root, "bindAddresses");
	if (value == nullptr) {
		return true;
	}
	if (!json_is_array(value)) {
		error = ConfigError::InvalidType;
		return false;
	}
	const auto count = json_array_size(value);
	if (count == 0U || count > MaximumBindAddresses) {
		error = ConfigError::OutOfRange;
		return false;
	}

	BindAddressList parsed;
	for (std::size_t index = 0; index < count; ++index) {
		NumericIpAddress address;
		if (!parse_numeric_address(json_array_get(value, index), address)) {
			error = ConfigError::InvalidAddress;
			return false;
		}
		switch (parsed.add(address)) {
		case BindAddressAddResult::Added:
			break;
		case BindAddressAddResult::Duplicate:
			error = ConfigError::DuplicateAddress;
			return false;
		case BindAddressAddResult::Invalid:
			error = ConfigError::InvalidAddress;
			return false;
		case BindAddressAddResult::CapacityExceeded:
			error = ConfigError::OutOfRange;
			return false;
		}
	}
	config.bind_addresses = parsed;
	return true;
}

bool parse_allowed_clients(const json_t* root,
	TelemetryConfig& config,
	bool& explicitly_configured,
	ConfigError& error) noexcept
{
	const auto* value = json_object_get(root, "allowedClients");
	if (value == nullptr) {
		return true;
	}
	explicitly_configured = true;
	if (!json_is_array(value)) {
		error = ConfigError::InvalidType;
		return false;
	}
	const auto count = json_array_size(value);
	if (count == 0U || count > MaximumAllowedClients) {
		error = ConfigError::OutOfRange;
		return false;
	}

	protocol::SourceAllowlist parsed;
	for (std::size_t index = 0; index < count; ++index) {
		protocol::IpCidr cidr;
		if (!parse_cidr(json_array_get(value, index), cidr)) {
			error = ConfigError::InvalidCidr;
			return false;
		}
		switch (parsed.add(cidr)) {
		case protocol::AllowlistAddResult::Added:
			break;
		case protocol::AllowlistAddResult::AlreadyPresent:
			error = ConfigError::DuplicateCidr;
			return false;
		case protocol::AllowlistAddResult::InvalidCidr:
			error = ConfigError::InvalidCidr;
			return false;
		case protocol::AllowlistAddResult::CapacityExceeded:
			error = ConfigError::OutOfRange;
			return false;
		}
	}
	config.allowed_clients = parsed;
	return true;
}

bool contains_catch_all(const protocol::SourceAllowlist& allowlist) noexcept
{
	for (std::size_t index = 0; index < allowlist.size(); ++index) {
		if (allowlist[index].prefix_length() == 0U) {
			return true;
		}
	}
	return false;
}

ConfigLoadResult parse_config_json(std::string_view input) noexcept
{
	if (input.size() > MaximumTelemetryConfigBytes) {
		return invalid_result(ConfigError::FileTooLarge);
	}
	if (input.find('\0') != std::string_view::npos) {
		return invalid_result(ConfigError::InvalidJson);
	}
	if (detail::preflight_json_container_depth(input, MaximumJsonContainerDepth) ==
		detail::JsonDepthPreflightResult::MaximumDepthExceeded) {
		return invalid_result(ConfigError::MaximumDepthExceeded);
	}

	json_error_t parse_error{};
	const auto* data = input.empty() ? "" : input.data();
	std::unique_ptr<json_t> root(json_loadb(data, input.size(), JSON_REJECT_DUPLICATES, &parse_error));
	if (!root) {
		return invalid_result(ConfigError::InvalidJson);
	}
	if (!json_container_depth_is_valid(root.get(), 1U)) {
		return invalid_result(ConfigError::MaximumDepthExceeded);
	}
	if (!json_is_object(root.get())) {
		return invalid_result(ConfigError::RootNotObject);
	}

	void* iterator = json_object_iter(root.get());
	while (iterator != nullptr) {
		if (!is_known_key(json_object_iter_key(iterator))) {
			return invalid_result(ConfigError::UnknownKey);
		}
		iterator = json_object_iter_next(root.get(), iterator);
	}

	const auto* schema_version = json_object_get(root.get(), "schemaVersion");
	if (schema_version == nullptr) {
		return invalid_result(ConfigError::MissingSchemaVersion);
	}
	if (!json_is_integer(schema_version)) {
		return invalid_result(ConfigError::InvalidType);
	}
	if (json_integer_value(schema_version) != 1) {
		return invalid_result(ConfigError::OutOfRange);
	}

	TelemetryConfig config;
	ConfigError error = ConfigError::None;
	if (!read_boolean(root.get(), "enabled", config.enabled, error) ||
		!parse_bind_addresses(root.get(), config, error)) {
		return invalid_result(error);
	}

	std::uint64_t integer = config.bind_port;
	if (!read_bounded_integer(root.get(), "bindPort", 1024U, 65535U, integer, error)) {
		return invalid_result(error);
	}
	config.bind_port = static_cast<std::uint16_t>(integer);

	bool allowed_clients_explicit = false;
	if (!parse_allowed_clients(root.get(), config, allowed_clients_explicit, error) ||
		!read_boolean(root.get(), "discoveryEnabled", config.discovery_enabled, error)) {
		return invalid_result(error);
	}
	if (config.discovery_enabled) {
		return invalid_result(ConfigError::OutOfRange);
	}

	const auto* visibility = json_object_get(root.get(), "visibilityMode");
	if (visibility != nullptr) {
		if (!json_is_string(visibility)) {
			return invalid_result(ConfigError::InvalidType);
		}
		constexpr std::string_view Cockpit{"Cockpit"};
		if (json_string_length(visibility) != Cockpit.size() ||
			std::memcmp(json_string_value(visibility), Cockpit.data(), Cockpit.size()) != 0) {
			return invalid_result(ConfigError::OutOfRange);
		}
	}

	if (!read_boolean(root.get(), "trustedFullState", config.trusted_full_state, error)) {
		return invalid_result(error);
	}
	if (config.trusted_full_state) {
		return invalid_result(ConfigError::OutOfRange);
	}

	integer = config.max_clients;
	if (!read_bounded_integer(root.get(), "maxClients", 1U, 4U, integer, error)) {
		return invalid_result(error);
	}
	config.max_clients = static_cast<std::uint8_t>(integer);

	integer = config.flight_hz;
	if (!read_bounded_integer(root.get(), "flightHz", 1U, 60U, integer, error)) {
		return invalid_result(error);
	}
	config.flight_hz = static_cast<std::uint8_t>(integer);

	integer = config.keyframe_seconds;
	if (!read_bounded_integer(root.get(), "keyframeSeconds", 1U, 5U, integer, error)) {
		return invalid_result(error);
	}
	config.keyframe_seconds = static_cast<std::uint8_t>(integer);

	integer = config.mission_heartbeat_ms;
	if (!read_bounded_integer(root.get(), "missionHeartbeatMs", 200U, 5000U, integer, error)) {
		return invalid_result(error);
	}
	config.mission_heartbeat_ms = static_cast<std::uint16_t>(integer);

	integer = config.idle_heartbeat_ms;
	if (!read_bounded_integer(root.get(), "idleHeartbeatMs", 200U, 5000U, integer, error)) {
		return invalid_result(error);
	}
	config.idle_heartbeat_ms = static_cast<std::uint16_t>(integer);

	integer = config.max_datagrams_per_tick;
	if (!read_bounded_integer(root.get(), "maxDatagramsPerTick", 1U, 256U, integer, error)) {
		return invalid_result(error);
	}
	config.max_datagrams_per_tick = static_cast<std::uint16_t>(integer);

	bool has_non_loopback_bind = false;
	bool has_wildcard_bind = false;
	for (std::size_t index = 0; index < config.bind_addresses.size(); ++index) {
		has_non_loopback_bind = has_non_loopback_bind || !config.bind_addresses[index].is_loopback();
		has_wildcard_bind = has_wildcard_bind || config.bind_addresses[index].is_wildcard();
	}
	if (has_non_loopback_bind &&
		(!config.enabled || !allowed_clients_explicit || config.allowed_clients.empty())) {
		return invalid_result(ConfigError::UnsafeExposure);
	}
	if (has_wildcard_bind && contains_catch_all(config.allowed_clients)) {
		return invalid_result(ConfigError::UnsafeExposure);
	}

	return valid_result(config);
}

} // namespace

NumericIpAddress NumericIpAddress::from_ipv4(const std::array<std::uint8_t, 4>& address) noexcept
{
	NumericIpAddress result;
	result.m_family = protocol::IpAddressFamily::Ipv4;
	std::copy(address.begin(), address.end(), result.m_bytes.begin());
	return result;
}

NumericIpAddress NumericIpAddress::from_ipv6(const std::array<std::uint8_t, 16>& address) noexcept
{
	NumericIpAddress result;
	result.m_family = protocol::IpAddressFamily::Ipv6;
	result.m_bytes = address;
	return result;
}

bool NumericIpAddress::is_valid() const noexcept
{
	if (m_family == protocol::IpAddressFamily::Ipv4) {
		return std::all_of(m_bytes.begin() + 4, m_bytes.end(), [](std::uint8_t byte) { return byte == 0U; });
	}
	return m_family == protocol::IpAddressFamily::Ipv6 && !is_ipv4_mapped(m_bytes);
}

bool NumericIpAddress::is_loopback() const noexcept
{
	if (!is_valid()) {
		return false;
	}
	if (m_family == protocol::IpAddressFamily::Ipv4) {
		return m_bytes[0] == 127U;
	}
	return std::all_of(m_bytes.begin(), m_bytes.end() - 1, [](std::uint8_t byte) { return byte == 0U; }) &&
		m_bytes.back() == 1U;
}

bool NumericIpAddress::is_wildcard() const noexcept
{
	return is_valid() && std::all_of(m_bytes.begin(), m_bytes.end(), [](std::uint8_t byte) { return byte == 0U; });
}

bool operator==(const NumericIpAddress& lhs, const NumericIpAddress& rhs) noexcept
{
	return lhs.m_family == rhs.m_family && lhs.m_bytes == rhs.m_bytes;
}

BindAddressAddResult BindAddressList::add(const NumericIpAddress& address) noexcept
{
	if (!address.is_valid()) {
		return BindAddressAddResult::Invalid;
	}
	for (std::size_t index = 0; index < m_size; ++index) {
		if (m_entries[index] == address) {
			return BindAddressAddResult::Duplicate;
		}
	}
	if (m_size >= m_entries.size()) {
		return BindAddressAddResult::CapacityExceeded;
	}
	m_entries[m_size++] = address;
	return BindAddressAddResult::Added;
}

void BindAddressList::clear() noexcept
{
	m_entries = {};
	m_size = 0;
}

const NumericIpAddress& BindAddressList::operator[](std::size_t index) const noexcept
{
	static const NumericIpAddress Invalid;
	return index < m_size ? m_entries[index] : Invalid;
}

TelemetryConfig::TelemetryConfig() noexcept
{
	const std::array<std::uint8_t, 4> ipv4_loopback{127U, 0U, 0U, 1U};
	std::array<std::uint8_t, 16> ipv6_loopback{};
	ipv6_loopback.back() = 1U;
	bind_addresses.add(NumericIpAddress::from_ipv4(ipv4_loopback));
	bind_addresses.add(NumericIpAddress::from_ipv6(ipv6_loopback));

	protocol::IpCidr ipv4_cidr;
	protocol::IpCidr ipv6_cidr;
	protocol::IpCidr::from_ipv4(ipv4_loopback, 32U, ipv4_cidr);
	protocol::IpCidr::from_ipv6(ipv6_loopback, 128U, ipv6_cidr);
	allowed_clients.add(ipv4_cidr);
	allowed_clients.add(ipv6_cidr);
}

ConfigLoadResult load_telemetry_config() noexcept
{
	try {
		const auto location = cf_find_file_location(ConfigFilename, CF_TYPE_CONFIG, CF_LOCATION_ALL);
		if (!location.found || location.offset != 0U) {
			return absent_result();
		}
		if (location.size > MaximumTelemetryConfigBytes) {
			return invalid_result(ConfigError::FileTooLarge);
		}

		std::unique_ptr<CFILE, CFileCloser> file(cfopen_special(location, "rb", CF_TYPE_CONFIG));
		if (!file) {
			return invalid_result(ConfigError::ReadFailure);
		}
		std::array<char, MaximumTelemetryConfigBytes> bytes{};
		const auto bytes_read = location.size == 0U
			? 0
			: cfread(bytes.data(), 1, static_cast<int>(location.size), file.get());
		const auto close_result = cfclose(file.release());
		if (bytes_read != static_cast<int>(location.size) || close_result != 0) {
			return invalid_result(ConfigError::ReadFailure);
		}
		return parse_config_json(std::string_view{bytes.data(), location.size});
	} catch (...) {
		return invalid_result(ConfigError::ReadFailure);
	}
}

namespace detail {

ConfigLoadResult parse_telemetry_config_json(std::string_view input) noexcept
{
	return parse_config_json(input);
}

ConfigLoadResult load_telemetry_config_from_observation(const ConfigLocationObservation& observation) noexcept
{
	switch (observation.kind) {
	case ConfigLocationKind::LooseUserRoot:
	case ConfigLocationKind::LooseGameRoot:
	case ConfigLocationKind::LooseActiveMod:
		return observation.offset == 0U ? parse_config_json(observation.bytes) : absent_result();
	case ConfigLocationKind::Absent:
	case ConfigLocationKind::VpOnly:
	default:
		return absent_result();
	}
}

} // namespace detail

} // namespace telemetry
