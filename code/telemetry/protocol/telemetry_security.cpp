#include "telemetry/protocol/telemetry_security.h"

#include "telemetry/protocol/telemetry_rate_limiter.h"

#include <algorithm>
#include <limits>

namespace telemetry::protocol {

static_assert(MaximumConfiguredClients == RateLimitMaximumSessions,
	"security and rate-limit client ceilings must remain identical");

namespace {

void increment_saturated(std::uint64_t& value) noexcept
{
	if (value != std::numeric_limits<std::uint64_t>::max()) {
		++value;
	}
}

void add_saturated(std::uint64_t& value, std::size_t amount) noexcept
{
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	if (amount > maximum || static_cast<std::uint64_t>(amount) > maximum - value) {
		value = maximum;
	} else {
		value += static_cast<std::uint64_t>(amount);
	}
}

bool checked_multiply(std::size_t lhs, std::size_t rhs, std::size_t& product) noexcept
{
	product = 0;
	if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
		return false;
	}
	product = lhs * rhs;
	return true;
}

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t& sum) noexcept
{
	sum = 0;
	if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
		return false;
	}
	sum = lhs + rhs;
	return true;
}

void canonicalize_network(std::array<std::uint8_t, 16>& network,
	std::size_t address_bytes,
	std::uint8_t prefix_length) noexcept
{
	const auto full_bytes = static_cast<std::size_t>(prefix_length / 8U);
	const auto remaining_bits = static_cast<std::uint8_t>(prefix_length % 8U);
	if (remaining_bits != 0 && full_bytes < address_bytes) {
		const auto mask = static_cast<std::uint8_t>(0xffU << (8U - remaining_bits));
		network[full_bytes] &= mask;
	}
	const auto first_zero = full_bytes + static_cast<std::size_t>(remaining_bits != 0);
	std::fill(network.begin() + static_cast<std::ptrdiff_t>(first_zero),
		network.begin() + static_cast<std::ptrdiff_t>(address_bytes),
		0U);
	std::fill(network.begin() + static_cast<std::ptrdiff_t>(address_bytes), network.end(), 0U);
}

bool prefix_matches(const std::array<std::uint8_t, 16>& address,
	const std::array<std::uint8_t, 16>& network,
	std::uint8_t prefix_length) noexcept
{
	const auto full_bytes = static_cast<std::size_t>(prefix_length / 8U);
	for (std::size_t index = 0; index < full_bytes; ++index) {
		if (address[index] != network[index]) {
			return false;
		}
	}
	const auto remaining_bits = static_cast<std::uint8_t>(prefix_length % 8U);
	if (remaining_bits == 0) {
		return true;
	}
	const auto mask = static_cast<std::uint8_t>(0xffU << (8U - remaining_bits));
	return (address[full_bytes] & mask) == network[full_bytes];
}

bool is_valid_source_address(const EndpointKey& endpoint) noexcept
{
	if (!endpoint.is_valid()) {
		return false;
	}
	const auto& address = endpoint.address();
	if (endpoint.family() == IpAddressFamily::Ipv4) {
		// Unspecified, limited broadcast and multicast cannot be a valid peer.
		return !(address[0] == 0U || address[0] >= 224U ||
			(address[0] == 255U && address[1] == 255U && address[2] == 255U && address[3] == 255U));
	}
	if (endpoint.family() != IpAddressFamily::Ipv6) {
		return false;
	}
	const auto all_zero = std::all_of(address.begin(), address.end(), [](std::uint8_t byte) { return byte == 0U; });
	return !all_zero && address[0] != 0xffU;
}

bool is_ipv4_mapped_address(const std::array<std::uint8_t, 16>& address) noexcept
{
	for (std::size_t index = 0; index < 10U; ++index) {
		if (address[index] != 0U) {
			return false;
		}
	}
	return address[10] == 0xffU && address[11] == 0xffU;
}

} // namespace

bool IpCidr::from_ipv4(const std::array<std::uint8_t, 4>& address,
	std::uint8_t prefix_length,
	IpCidr& cidr) noexcept
{
	if (prefix_length > 32U) {
		return false;
	}
	IpCidr candidate;
	candidate.m_family = IpAddressFamily::Ipv4;
	candidate.m_prefix_length = prefix_length;
	std::copy(address.begin(), address.end(), candidate.m_network.begin());
	canonicalize_network(candidate.m_network, 4U, prefix_length);
	cidr = candidate;
	return true;
}

bool IpCidr::from_ipv6(const std::array<std::uint8_t, 16>& address,
	std::uint8_t prefix_length,
	IpCidr& cidr) noexcept
{
	if (prefix_length > 128U || is_ipv4_mapped_address(address)) {
		return false;
	}
	IpCidr candidate;
	candidate.m_family = IpAddressFamily::Ipv6;
	candidate.m_prefix_length = prefix_length;
	candidate.m_network = address;
	canonicalize_network(candidate.m_network, 16U, prefix_length);
	cidr = candidate;
	return true;
}

bool IpCidr::is_valid() const noexcept
{
	if (m_family == IpAddressFamily::Ipv4) {
		if (m_prefix_length > 32U) {
			return false;
		}
		return std::all_of(m_network.begin() + 4, m_network.end(), [](std::uint8_t byte) { return byte == 0U; });
	}
	return m_family == IpAddressFamily::Ipv6 && m_prefix_length <= 128U;
}

bool IpCidr::contains(const EndpointKey& endpoint) const noexcept
{
	return is_valid() && endpoint.is_valid() && endpoint.family() == m_family &&
		prefix_matches(endpoint.address(), m_network, m_prefix_length);
}

bool operator==(const IpCidr& lhs, const IpCidr& rhs) noexcept
{
	return lhs.m_family == rhs.m_family && lhs.m_prefix_length == rhs.m_prefix_length &&
		lhs.m_network == rhs.m_network;
}

AllowlistAddResult SourceAllowlist::add(const IpCidr& cidr) noexcept
{
	if (!cidr.is_valid()) {
		return AllowlistAddResult::InvalidCidr;
	}
	for (std::size_t index = 0; index < m_size; ++index) {
		if (m_entries[index] == cidr) {
			return AllowlistAddResult::AlreadyPresent;
		}
	}
	if (m_size >= m_entries.size()) {
		return AllowlistAddResult::CapacityExceeded;
	}
	m_entries[m_size++] = cidr;
	return AllowlistAddResult::Added;
}

bool SourceAllowlist::contains(const EndpointKey& endpoint) const noexcept
{
	for (std::size_t index = 0; index < m_size; ++index) {
		if (m_entries[index].contains(endpoint)) {
			return true;
		}
	}
	return false;
}

void SourceAllowlist::clear() noexcept
{
	for (auto& entry : m_entries) {
		entry = IpCidr{};
	}
	m_size = 0;
}

const IpCidr& SourceAllowlist::operator[](std::size_t index) const noexcept
{
	static const IpCidr Invalid;
	return index < m_size ? m_entries[index] : Invalid;
}

SecurityConfigurationError validate_security_configuration(const TelemetryOperationalConfig& config,
	TelemetryResourceBudgetTotals& totals) noexcept
{
	totals = TelemetryResourceBudgetTotals{};
	if (config.port == 0U) {
		return SecurityConfigurationError::InvalidPort;
	}
	if (config.bind_mode != NetworkBindMode::LoopbackOnly &&
		config.bind_mode != NetworkBindMode::LoopbackAndAllowlisted) {
		return SecurityConfigurationError::InvalidBindMode;
	}
	if (config.bind_mode == NetworkBindMode::LoopbackAndAllowlisted && config.source_allowlist.empty()) {
		return SecurityConfigurationError::NonLoopbackRequiresAllowlist;
	}
	if (config.discovery_destination_count > MaximumDiscoveryDestinations) {
		return SecurityConfigurationError::TooManyDiscoveryDestinations;
	}
	if (config.discovery_enabled && config.discovery_destination_count == 0U) {
		return SecurityConfigurationError::DiscoveryRequiresDestination;
	}
	if (config.trusted_full_state_enabled && config.source_allowlist.empty()) {
		return SecurityConfigurationError::TrustedFullStateRequiresAllowlist;
	}
	if (config.target_video_enabled &&
		(!config.target_video_renderer_validated || !config.target_video_async_readback_validated ||
			!config.target_video_encoder_validated)) {
		return SecurityConfigurationError::TargetVideoPrerequisitesMissing;
	}
	if (config.resources.max_clients == 0U) {
		return SecurityConfigurationError::InvalidClientLimit;
	}
	if (!checked_multiply(config.resources.max_clients,
			MaxStateReassemblyBytesPerClient,
			totals.required_state_reassembly_bytes) ||
		(config.target_video_enabled &&
			!checked_multiply(config.resources.max_clients,
				MaxVideoReassemblyBytesPerClient,
				totals.required_video_reassembly_bytes)) ||
		!checked_add(totals.required_state_reassembly_bytes,
			totals.required_video_reassembly_bytes,
			totals.required_total_reassembly_bytes) ||
		!checked_add(config.resources.global_state_reassembly_bytes,
			config.target_video_enabled ? config.resources.global_video_reassembly_bytes : 0U,
			totals.configured_total_reassembly_bytes)) {
		totals = TelemetryResourceBudgetTotals{};
		return SecurityConfigurationError::ArithmeticOverflow;
	}
	if (config.resources.max_clients > MaximumConfiguredClients) {
		return SecurityConfigurationError::ClientLimitExceeded;
	}
	if (config.resources.global_state_reassembly_bytes < totals.required_state_reassembly_bytes) {
		return SecurityConfigurationError::GlobalStateBudgetTooSmall;
	}
	if (config.target_video_enabled &&
		config.resources.global_video_reassembly_bytes < totals.required_video_reassembly_bytes) {
		return SecurityConfigurationError::GlobalVideoBudgetTooSmall;
	}
	return SecurityConfigurationError::None;
}

bool telemetry_module_can_start(const TelemetryOperationalConfig& config,
	TelemetryResourceBudgetTotals& totals) noexcept
{
	const auto error = validate_security_configuration(config, totals);
	return config.enabled && error == SecurityConfigurationError::None;
}

bool is_loopback_source(const EndpointKey& endpoint) noexcept
{
	if (!endpoint.is_valid()) {
		return false;
	}
	const auto& address = endpoint.address();
	if (endpoint.family() == IpAddressFamily::Ipv4) {
		return address[0] == 127U;
	}
	if (endpoint.family() != IpAddressFamily::Ipv6) {
		return false;
	}
	for (std::size_t index = 0; index < address.size() - 1U; ++index) {
		if (address[index] != 0U) {
			return false;
		}
	}
	return address.back() == 1U;
}

bool source_is_allowed(const TelemetryOperationalConfig& config, const EndpointKey& source_endpoint) noexcept
{
	TelemetryResourceBudgetTotals ignored;
	if (!config.enabled ||
		validate_security_configuration(config, ignored) != SecurityConfigurationError::None ||
		!is_valid_source_address(source_endpoint)) {
		return false;
	}
	if (is_loopback_source(source_endpoint)) {
		return true;
	}
	return config.bind_mode == NetworkBindMode::LoopbackAndAllowlisted &&
		config.source_allowlist.contains(source_endpoint);
}

bool trusted_full_state_is_authorized(const TelemetryOperationalConfig& config,
	const EndpointKey& source_endpoint) noexcept
{
	TelemetryResourceBudgetTotals ignored;
	return config.enabled && config.trusted_full_state_enabled &&
		validate_security_configuration(config, ignored) == SecurityConfigurationError::None &&
		source_is_allowed(config, source_endpoint) && config.source_allowlist.contains(source_endpoint);
}

void TelemetryIngressCounters::record(std::size_t datagram_bytes, ValidationError result) noexcept
{
	increment_saturated(m_datagrams_received);
	add_saturated(m_bytes_received, datagram_bytes);
	if (result == ValidationError::None) {
		increment_saturated(m_datagrams_accepted);
		add_saturated(m_bytes_accepted, datagram_bytes);
		return;
	}
	increment_saturated(m_datagrams_dropped);
	add_saturated(m_bytes_dropped, datagram_bytes);
	const auto index = static_cast<std::size_t>(result);
	if (index < m_rejected_by_error.size()) {
		increment_saturated(m_rejected_by_error[index]);
	}
}

void TelemetryIngressCounters::clear() noexcept
{
	*this = TelemetryIngressCounters{};
}

std::uint64_t TelemetryIngressCounters::rejected(ValidationError error) const noexcept
{
	const auto index = static_cast<std::size_t>(error);
	return error != ValidationError::None && index < m_rejected_by_error.size() ? m_rejected_by_error[index] : 0U;
}

ValidationError decode_and_validate_ingress_datagram(const TelemetryOperationalConfig& config,
	const EndpointKey& source_endpoint,
	ByteView datagram,
	const TelemetrySessionContext& context,
	DatagramView& decoded,
	TelemetryIngressCounters& counters) noexcept
{
	decoded = DatagramView{};
	const auto finish = [&counters, datagram](ValidationError result) noexcept {
		counters.record(datagram.size, result);
		return result;
	};
	if (!source_is_allowed(config, source_endpoint)) {
		return finish(ValidationError::SourceNotAllowed);
	}
	DatagramView envelope;
	if (const auto error = decode_and_validate_datagram_envelope(datagram, envelope);
		error != ValidationError::None) {
		return finish(error);
	}
	if (const auto error = validate_received_datagram_context(envelope.header, source_endpoint, context);
		error != ValidationError::None) {
		return finish(error);
	}
	if (const auto error = validate_fragment_layout(envelope.header); error != ValidationError::None) {
		return finish(error);
	}
	decoded = envelope;
	return finish(ValidationError::None);
}

ProtocolAuthority message_authority(MessageType type) noexcept
{
	switch (type) {
	case MessageType::Discovery:
	case MessageType::Hello:
	case MessageType::Welcome:
	case MessageType::SessionBegin:
	case MessageType::Manifest:
	case MessageType::FullSnapshot:
	case MessageType::Delta:
	case MessageType::EventBatch:
	case MessageType::Heartbeat:
	case MessageType::Ack:
	case MessageType::Nack:
	case MessageType::ResyncRequest:
	case MessageType::SessionEnd:
	case MessageType::TargetVideoSubscribe:
	case MessageType::TargetVideoConfig:
	case MessageType::TargetVideoFrame:
	case MessageType::TargetVideoKeyframeRequest:
	case MessageType::TargetVideoStop:
	case MessageType::TargetVideoStats:
	case MessageType::CapabilityUpdate:
		return ProtocolAuthority::ReadOnlyTelemetry;
	default:
		return ProtocolAuthority::Invalid;
	}
}

SecurityConfigurationError GlobalReassemblyBudget::configure(const TelemetryOperationalConfig& config,
	GlobalReassemblyBudget& budget) noexcept
{
	if (budget.m_active_clients != 0U || budget.m_state_reserved != 0U || budget.m_video_reserved != 0U) {
		return SecurityConfigurationError::GlobalBudgetInUse;
	}
	TelemetryResourceBudgetTotals totals;
	const auto error = validate_security_configuration(config, totals);
	if (error != SecurityConfigurationError::None) {
		budget.m_client_capacity = 0U;
		budget.m_state_capacity = 0U;
		budget.m_video_capacity = 0U;
		return error;
	}
	if (!config.enabled) {
		budget.m_client_capacity = 0U;
		budget.m_state_capacity = 0U;
		budget.m_video_capacity = 0U;
		return SecurityConfigurationError::ModuleDisabled;
	}
	budget.m_client_capacity = config.resources.max_clients;
	budget.m_state_capacity = config.resources.global_state_reassembly_bytes;
	budget.m_video_capacity = config.target_video_enabled ? config.resources.global_video_reassembly_bytes : 0U;
	budget.m_counters = GlobalReassemblyBudgetCounters{};
	return SecurityConfigurationError::None;
}

bool GlobalReassemblyBudget::try_register_client() noexcept
{
	if (m_client_capacity == 0U || m_active_clients >= m_client_capacity) {
		increment_saturated(m_counters.client_admission_rejections);
		return false;
	}
	++m_active_clients;
	increment_saturated(m_counters.client_admissions);
	m_counters.peak_active_clients = std::max(m_counters.peak_active_clients, m_active_clients);
	return true;
}

bool GlobalReassemblyBudget::release_client() noexcept
{
	if (m_active_clients == 0U) {
		return false;
	}
	--m_active_clients;
	return true;
}

GlobalBudgetResult GlobalReassemblyBudget::try_reserve(MessageSizeClass message_class,
	std::size_t byte_count) noexcept
{
	if (m_active_clients == 0U) {
		return GlobalBudgetResult::NoClient;
	}
	if (byte_count == 0U) {
		return GlobalBudgetResult::InvalidAmount;
	}
	std::size_t* reserved = nullptr;
	std::size_t capacity = 0;
	switch (message_class) {
	case MessageSizeClass::State:
		reserved = &m_state_reserved;
		capacity = m_state_capacity;
		break;
	case MessageSizeClass::Video:
		reserved = &m_video_reserved;
		capacity = m_video_capacity;
		break;
	default:
		return GlobalBudgetResult::InvalidClass;
	}
	if (*reserved > capacity || byte_count > capacity - *reserved) {
		if (message_class == MessageSizeClass::State) {
			increment_saturated(m_counters.state_quota_rejections);
		} else {
			increment_saturated(m_counters.video_quota_rejections);
		}
		return GlobalBudgetResult::Exhausted;
	}
	*reserved += byte_count;
	if (message_class == MessageSizeClass::State) {
		increment_saturated(m_counters.state_reservations);
		m_counters.peak_state_reserved_bytes = std::max(m_counters.peak_state_reserved_bytes, *reserved);
	} else {
		increment_saturated(m_counters.video_reservations);
		m_counters.peak_video_reserved_bytes = std::max(m_counters.peak_video_reserved_bytes, *reserved);
	}
	return GlobalBudgetResult::Reserved;
}

bool GlobalReassemblyBudget::release(MessageSizeClass message_class, std::size_t byte_count) noexcept
{
	if (byte_count == 0U) {
		return false;
	}
	std::size_t* reserved = nullptr;
	switch (message_class) {
	case MessageSizeClass::State:
		reserved = &m_state_reserved;
		break;
	case MessageSizeClass::Video:
		reserved = &m_video_reserved;
		break;
	default:
		return false;
	}
	if (byte_count > *reserved) {
		return false;
	}
	*reserved -= byte_count;
	return true;
}

std::size_t GlobalReassemblyBudget::capacity_bytes(MessageSizeClass message_class) const noexcept
{
	switch (message_class) {
	case MessageSizeClass::State:
		return m_state_capacity;
	case MessageSizeClass::Video:
		return m_video_capacity;
	default:
		return 0U;
	}
}

std::size_t GlobalReassemblyBudget::reserved_bytes(MessageSizeClass message_class) const noexcept
{
	switch (message_class) {
	case MessageSizeClass::State:
		return m_state_reserved;
	case MessageSizeClass::Video:
		return m_video_reserved;
	default:
		return 0U;
	}
}

} // namespace telemetry::protocol
