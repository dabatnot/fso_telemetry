#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliable_receive.h"
#include "telemetry/protocol/telemetry_security.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using namespace telemetry::protocol;

EndpointKey ipv4(std::uint8_t a,
	std::uint8_t b,
	std::uint8_t c,
	std::uint8_t d,
	std::uint16_t port = DefaultTelemetryPort)
{
	return EndpointKey::from_ipv4({{a, b, c, d}}, port);
}

EndpointKey ipv6(const std::array<std::uint8_t, 16>& address,
	std::uint16_t port = DefaultTelemetryPort)
{
	return EndpointKey::from_ipv6(address, port);
}

IpCidr ipv4_cidr(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint8_t prefix)
{
	IpCidr result;
	EXPECT_TRUE(IpCidr::from_ipv4({{a, b, c, d}}, prefix, result));
	return result;
}

struct OwnedFragment {
	TelemetryDatagramHeader header;
	std::vector<std::uint8_t> payload;

	DatagramView view() const noexcept
	{
		return DatagramView{header, ByteView{payload.data(), payload.size()}};
	}
};

OwnedFragment first_state_fragment(std::uint32_t message_id, std::uint64_t session_id = 42U)
{
	OwnedFragment result;
	result.header.message_type = MessageType::Delta;
	result.header.flags = MessageFlagFragmented;
	result.header.session_id = session_id;
	result.header.message_id = message_id;
	result.header.message_size = static_cast<std::uint32_t>(MaxStateMessageSize);
	result.header.message_crc32 = 0x12345678U;
	EXPECT_EQ(ValidationError::None,
		expected_fragment_count(result.header.message_type, result.header.message_size, result.header.fragment_count));
	result.payload.assign(MaxFragmentPayload, 0x5aU);
	result.header.payload_size = static_cast<std::uint16_t>(result.payload.size());
	return result;
}

TEST(TelemetryProtocolSecurity, DefaultsAreDisabledLoopbackOnlyCockpitOnlyAndBudgeted)
{
	const TelemetryOperationalConfig config;
	EXPECT_FALSE(config.enabled);
	EXPECT_EQ(DefaultTelemetryPort, config.port);
	EXPECT_EQ(NetworkBindMode::LoopbackOnly, config.bind_mode);
	EXPECT_FALSE(config.discovery_enabled);
	EXPECT_EQ(0U, config.discovery_destination_count);
	EXPECT_FALSE(config.trusted_full_state_enabled);
	EXPECT_FALSE(config.target_video_enabled);
	EXPECT_FALSE(config.target_video_renderer_validated);
	EXPECT_FALSE(config.target_video_async_readback_validated);
	EXPECT_FALSE(config.target_video_encoder_validated);
	EXPECT_TRUE(config.source_allowlist.empty());
	EXPECT_EQ(1U, config.resources.max_clients);
	EXPECT_EQ(48U, ValidationErrorCount);

	TelemetryResourceBudgetTotals totals;
	EXPECT_EQ(SecurityConfigurationError::None, validate_security_configuration(config, totals));
	EXPECT_FALSE(telemetry_module_can_start(config, totals));
	EXPECT_EQ(MaxStateReassemblyBytesPerClient, totals.required_state_reassembly_bytes);
	EXPECT_EQ(0U, totals.required_video_reassembly_bytes);
	EXPECT_EQ(MaxStateReassemblyBytesPerClient, totals.required_total_reassembly_bytes);

	GlobalReassemblyBudget budget;
	EXPECT_EQ(SecurityConfigurationError::ModuleDisabled, GlobalReassemblyBudget::configure(config, budget));
	EXPECT_EQ(0U, budget.capacity_bytes(MessageSizeClass::State));
	EXPECT_EQ(0U, budget.capacity_bytes(MessageSizeClass::Video));
}

TEST(TelemetryProtocolSecurity, LanDiscoveryAndTrustedVisibilityRequireIndependentExplicitOptIns)
{
	TelemetryOperationalConfig config;
	config.enabled = true;
	TelemetryResourceBudgetTotals totals;
	EXPECT_TRUE(telemetry_module_can_start(config, totals));
	config.resources.global_video_reassembly_bytes = 0U;
	EXPECT_TRUE(telemetry_module_can_start(config, totals));

	config.bind_mode = NetworkBindMode::LoopbackAndAllowlisted;
	EXPECT_EQ(SecurityConfigurationError::NonLoopbackRequiresAllowlist,
		validate_security_configuration(config, totals));
	ASSERT_EQ(AllowlistAddResult::Added, config.source_allowlist.add(ipv4_cidr(192U, 0U, 2U, 0U, 24U)));
	EXPECT_EQ(SecurityConfigurationError::None, validate_security_configuration(config, totals));

	config.discovery_enabled = true;
	EXPECT_EQ(SecurityConfigurationError::DiscoveryRequiresDestination,
		validate_security_configuration(config, totals));
	config.discovery_destination_count = 1U;
	EXPECT_EQ(SecurityConfigurationError::None, validate_security_configuration(config, totals));
	config.discovery_destination_count = MaximumDiscoveryDestinations + 1U;
	EXPECT_EQ(SecurityConfigurationError::TooManyDiscoveryDestinations,
		validate_security_configuration(config, totals));

	config = TelemetryOperationalConfig{};
	config.enabled = true;
	config.trusted_full_state_enabled = true;
	EXPECT_EQ(SecurityConfigurationError::TrustedFullStateRequiresAllowlist,
		validate_security_configuration(config, totals));
	ASSERT_EQ(AllowlistAddResult::Added, config.source_allowlist.add(ipv4_cidr(127U, 0U, 0U, 1U, 32U)));
	EXPECT_EQ(SecurityConfigurationError::None, validate_security_configuration(config, totals));
	EXPECT_TRUE(trusted_full_state_is_authorized(config, ipv4(127U, 0U, 0U, 1U)));
	EXPECT_FALSE(trusted_full_state_is_authorized(config, ipv4(127U, 0U, 0U, 2U)));
	ASSERT_EQ(AllowlistAddResult::Added, config.source_allowlist.add(ipv4_cidr(192U, 0U, 2U, 9U, 32U)));
	EXPECT_FALSE(trusted_full_state_is_authorized(config, ipv4(192U, 0U, 2U, 9U)));
	config.bind_mode = NetworkBindMode::LoopbackAndAllowlisted;
	EXPECT_TRUE(trusted_full_state_is_authorized(config, ipv4(192U, 0U, 2U, 9U)));

	config.target_video_enabled = true;
	EXPECT_EQ(SecurityConfigurationError::TargetVideoPrerequisitesMissing,
		validate_security_configuration(config, totals));
	config.target_video_renderer_validated = true;
	config.target_video_async_readback_validated = true;
	config.target_video_encoder_validated = true;
	EXPECT_EQ(SecurityConfigurationError::None, validate_security_configuration(config, totals));
	config.port = 0U;
	EXPECT_EQ(SecurityConfigurationError::InvalidPort, validate_security_configuration(config, totals));
	config.port = DefaultTelemetryPort;
	config.bind_mode = static_cast<NetworkBindMode>(0xffU);
	EXPECT_EQ(SecurityConfigurationError::InvalidBindMode, validate_security_configuration(config, totals));
}

TEST(TelemetryProtocolSecurity, BinaryAddressAndCidrAllowlistIsCanonicalBoundedAndPortIndependent)
{
	SourceAllowlist allowlist;
	const auto subnet = ipv4_cidr(192U, 168U, 7U, 199U, 24U);
	EXPECT_EQ(0U, subnet.network()[3]);
	ASSERT_EQ(AllowlistAddResult::Added, allowlist.add(subnet));
	EXPECT_EQ(AllowlistAddResult::AlreadyPresent, allowlist.add(subnet));
	EXPECT_TRUE(allowlist.contains(ipv4(192U, 168U, 7U, 1U, 1U)));
	EXPECT_TRUE(allowlist.contains(ipv4(192U, 168U, 7U, 254U, 65535U)));
	EXPECT_FALSE(allowlist.contains(ipv4(192U, 168U, 8U, 1U)));

	std::array<std::uint8_t, 16> network6{{0x20U, 0x01U, 0x0dU, 0xb8U, 0x12U, 0x34U}};
	IpCidr subnet6;
	ASSERT_TRUE(IpCidr::from_ipv6(network6, 64U, subnet6));
	ASSERT_EQ(AllowlistAddResult::Added, allowlist.add(subnet6));
	auto peer6 = network6;
	peer6[15] = 42U;
	EXPECT_TRUE(allowlist.contains(ipv6(peer6)));
	peer6[7] = 1U;
	EXPECT_FALSE(allowlist.contains(ipv6(peer6)));

	std::array<std::uint8_t, 16> mapped{};
	mapped[10] = 0xffU;
	mapped[11] = 0xffU;
	mapped[12] = 192U;
	mapped[13] = 168U;
	mapped[14] = 7U;
	mapped[15] = 20U;
	EXPECT_EQ(IpAddressFamily::Ipv4, ipv6(mapped).family());
	EXPECT_TRUE(allowlist.contains(ipv6(mapped)));

	IpCidr sentinel = subnet;
	EXPECT_FALSE(IpCidr::from_ipv6(mapped, 128U, sentinel));
	EXPECT_EQ(subnet, sentinel);
	EXPECT_FALSE(IpCidr::from_ipv4({{1U, 2U, 3U, 4U}}, 33U, sentinel));
	EXPECT_EQ(subnet, sentinel);
	EXPECT_FALSE(IpCidr::from_ipv6(network6, 129U, sentinel));
	EXPECT_EQ(subnet, sentinel);
	EXPECT_EQ(AllowlistAddResult::InvalidCidr, allowlist.add(IpCidr{}));

	allowlist.clear();
	for (std::size_t index = 0; index < MaximumSourceAllowlistEntries; ++index) {
		ASSERT_EQ(AllowlistAddResult::Added,
			allowlist.add(ipv4_cidr(10U, 0U, 0U, static_cast<std::uint8_t>(index), 32U)));
	}
	EXPECT_EQ(MaximumSourceAllowlistEntries, allowlist.size());
	EXPECT_EQ(AllowlistAddResult::CapacityExceeded, allowlist.add(ipv4_cidr(10U, 0U, 1U, 1U, 32U)));
	EXPECT_FALSE(allowlist[MaximumSourceAllowlistEntries].is_valid());
}

TEST(TelemetryProtocolSecurity, SourcePolicyRejectsBeforeReadingAndContextPrecedesFragmentLayout)
{
	TelemetryOperationalConfig config;
	config.enabled = true;
	TelemetrySessionContext context;
	context.local_role = LocalEndpointRole::Producer;
	context.peer_endpoint = ipv4(127U, 0U, 0U, 1U);
	context.active_session_id = 42U;
	DatagramView decoded;
	TelemetryIngressCounters counters;

	EXPECT_EQ(ValidationError::SourceNotAllowed,
		decode_and_validate_ingress_datagram(
			config, ipv4(203U, 0U, 113U, 9U), ByteView{}, context, decoded, counters));
	EXPECT_EQ(ValidationError::DatagramTooShort,
		decode_and_validate_ingress_datagram(config, context.peer_endpoint, ByteView{}, context, decoded, counters));

	TelemetryDatagramHeader header;
	header.message_type = MessageType::Heartbeat;
	header.session_id = 42U;
	header.message_id = 7U;
	header.message_size = 0U;
	header.message_crc32 = 0U;
	std::array<std::uint8_t, MaxDatagramSize> bytes{};
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None,
		encode_datagram(header, ByteView{}, MutableByteView{bytes.data(), bytes.size()}, written));
	ASSERT_EQ(HeaderSizeV1, written);
	EXPECT_EQ(ValidationError::None,
		decode_and_validate_ingress_datagram(config,
			context.peer_endpoint,
			ByteView{bytes.data(), written},
			context,
			decoded,
			counters));
	EXPECT_EQ(MessageType::Heartbeat, decoded.header.message_type);

	// A second loopback source passes source policy but fails exact endpoint
	// binding before the static fragment layout is allowed to allocate state.
	EXPECT_EQ(ValidationError::EndpointMismatch,
		decode_and_validate_ingress_datagram(config,
			ipv4(127U, 0U, 0U, 2U),
			ByteView{bytes.data(), written},
			context,
			decoded,
			counters));

	config.enabled = false;
	EXPECT_EQ(ValidationError::SourceNotAllowed,
		decode_and_validate_ingress_datagram(
			config, context.peer_endpoint, ByteView{bytes.data(), written}, context, decoded, counters));
	EXPECT_EQ(MessageType::Invalid, decoded.header.message_type);
	EXPECT_EQ(5U, counters.datagrams_received());
	EXPECT_EQ(3U * HeaderSizeV1, counters.bytes_received());
	EXPECT_EQ(1U, counters.datagrams_accepted());
	EXPECT_EQ(HeaderSizeV1, counters.bytes_accepted());
	EXPECT_EQ(4U, counters.datagrams_dropped());
	EXPECT_EQ(2U * HeaderSizeV1, counters.bytes_dropped());
	EXPECT_EQ(2U, counters.rejected(ValidationError::SourceNotAllowed));
	EXPECT_EQ(1U, counters.rejected(ValidationError::DatagramTooShort));
	EXPECT_EQ(1U, counters.rejected(ValidationError::EndpointMismatch));
	EXPECT_EQ(0U, counters.rejected(ValidationError::None));
}

TEST(TelemetryProtocolSecurity, SourcePolicyMatchesOnlyLoopbackOrExplicitUnicastCidrs)
{
	TelemetryOperationalConfig config;
	config.enabled = true;
	EXPECT_TRUE(source_is_allowed(config, ipv4(127U, 255U, 2U, 3U)));
	std::array<std::uint8_t, 16> loopback6{};
	loopback6[15] = 1U;
	EXPECT_TRUE(is_loopback_source(ipv6(loopback6)));
	EXPECT_TRUE(source_is_allowed(config, ipv6(loopback6)));
	EXPECT_FALSE(source_is_allowed(config, ipv4(192U, 0U, 2U, 1U)));

	config.bind_mode = NetworkBindMode::LoopbackAndAllowlisted;
	ASSERT_EQ(AllowlistAddResult::Added, config.source_allowlist.add(ipv4_cidr(0U, 0U, 0U, 0U, 0U)));
	EXPECT_TRUE(source_is_allowed(config, ipv4(192U, 0U, 2U, 1U)));
	EXPECT_FALSE(source_is_allowed(config, ipv4(0U, 0U, 0U, 1U)));
	EXPECT_FALSE(source_is_allowed(config, ipv4(239U, 1U, 2U, 3U)));
	std::array<std::uint8_t, 16> multicast6{};
	multicast6[0] = 0xffU;
	multicast6[15] = 1U;
	EXPECT_FALSE(source_is_allowed(config, ipv6(multicast6)));
}

TEST(TelemetryProtocolSecurity, StartupBudgetArithmeticRejectsOverflowAndImpossibleGlobalBudgets)
{
	TelemetryOperationalConfig config;
	config.enabled = true;
	TelemetryResourceBudgetTotals totals;

	config.resources.max_clients = 0U;
	EXPECT_EQ(SecurityConfigurationError::InvalidClientLimit,
		validate_security_configuration(config, totals));
	config.resources.max_clients = std::numeric_limits<std::size_t>::max();
	EXPECT_EQ(SecurityConfigurationError::ArithmeticOverflow,
		validate_security_configuration(config, totals));
	config.resources.max_clients = MaximumConfiguredClients + 1U;
	EXPECT_EQ(SecurityConfigurationError::ClientLimitExceeded,
		validate_security_configuration(config, totals));

	config.resources.max_clients = 2U;
	config.target_video_enabled = true;
	config.target_video_renderer_validated = true;
	config.target_video_async_readback_validated = true;
	config.target_video_encoder_validated = true;
	config.resources.global_state_reassembly_bytes = 2U * MaxStateReassemblyBytesPerClient;
	config.resources.global_video_reassembly_bytes = 2U * MaxVideoReassemblyBytesPerClient;
	ASSERT_EQ(SecurityConfigurationError::None, validate_security_configuration(config, totals));
	EXPECT_EQ(2U * MaxStateReassemblyBytesPerClient, totals.required_state_reassembly_bytes);
	EXPECT_EQ(2U * MaxVideoReassemblyBytesPerClient, totals.required_video_reassembly_bytes);

	--config.resources.global_state_reassembly_bytes;
	EXPECT_EQ(SecurityConfigurationError::GlobalStateBudgetTooSmall,
		validate_security_configuration(config, totals));
	++config.resources.global_state_reassembly_bytes;
	--config.resources.global_video_reassembly_bytes;
	EXPECT_EQ(SecurityConfigurationError::GlobalVideoBudgetTooSmall,
		validate_security_configuration(config, totals));

	config.resources.max_clients = 1U;
	config.resources.global_state_reassembly_bytes = std::numeric_limits<std::size_t>::max();
	config.resources.global_video_reassembly_bytes = 1U;
	EXPECT_EQ(SecurityConfigurationError::ArithmeticOverflow,
		validate_security_configuration(config, totals));
}

TEST(TelemetryProtocolSecurity, SharedGlobalBudgetIsTransactionalBoundedAndClassSeparated)
{
	TelemetryOperationalConfig config;
	config.enabled = true;
	config.target_video_enabled = true;
	config.target_video_renderer_validated = true;
	config.target_video_async_readback_validated = true;
	config.target_video_encoder_validated = true;
	GlobalReassemblyBudget budget;
	ASSERT_EQ(SecurityConfigurationError::None, GlobalReassemblyBudget::configure(config, budget));
	EXPECT_EQ(MaxStateReassemblyBytesPerClient, budget.capacity_bytes(MessageSizeClass::State));
	EXPECT_EQ(MaxVideoReassemblyBytesPerClient, budget.capacity_bytes(MessageSizeClass::Video));
	EXPECT_EQ(GlobalBudgetResult::NoClient, budget.try_reserve(MessageSizeClass::State, 1U));
	ASSERT_TRUE(budget.try_register_client());
	EXPECT_EQ(GlobalBudgetResult::InvalidClass, budget.try_reserve(MessageSizeClass::Invalid, 1U));
	EXPECT_EQ(GlobalBudgetResult::InvalidAmount, budget.try_reserve(MessageSizeClass::State, 0U));

	ASSERT_EQ(GlobalBudgetResult::Reserved,
		budget.try_reserve(MessageSizeClass::State, MaxStateReassemblyBytesPerClient));
	EXPECT_EQ(GlobalBudgetResult::Exhausted, budget.try_reserve(MessageSizeClass::State, 1U));
	EXPECT_EQ(MaxStateReassemblyBytesPerClient, budget.reserved_bytes(MessageSizeClass::State));
	EXPECT_EQ(0U, budget.reserved_bytes(MessageSizeClass::Video));
	EXPECT_EQ(1U, budget.counters().state_reservations);
	EXPECT_EQ(1U, budget.counters().state_quota_rejections);
	EXPECT_EQ(MaxStateReassemblyBytesPerClient, budget.counters().peak_state_reserved_bytes);
	EXPECT_EQ(0U, budget.counters().peak_video_reserved_bytes);
	EXPECT_EQ(SecurityConfigurationError::GlobalBudgetInUse, GlobalReassemblyBudget::configure(config, budget));
	EXPECT_FALSE(budget.release(MessageSizeClass::State, MaxStateReassemblyBytesPerClient + 1U));
	EXPECT_TRUE(budget.release(MessageSizeClass::State, MaxStateReassemblyBytesPerClient));
	EXPECT_FALSE(budget.release(MessageSizeClass::State, 1U));
	EXPECT_TRUE(budget.release_client());
	EXPECT_EQ(SecurityConfigurationError::None, GlobalReassemblyBudget::configure(config, budget));
}

TEST(TelemetryProtocolSecurity, PerClientReassemblersShareAndReleaseTheGlobalBudget)
{
	TelemetryOperationalConfig config;
	config.enabled = true;
	GlobalReassemblyBudget budget;
	ASSERT_EQ(SecurityConfigurationError::None, GlobalReassemblyBudget::configure(config, budget));
	ReassembledMessage completed;
	{
		ReliableReceivePipeline operational_pipeline(budget);
		EXPECT_EQ(1U, budget.active_clients());
		EXPECT_EQ(0U, operational_pipeline.reserved_bytes(MessageSizeClass::State));
		EXPECT_EQ(GlobalBudgetResult::Exhausted, budget.try_reserve(MessageSizeClass::Video, 1U));
		EXPECT_EQ(SecurityConfigurationError::GlobalBudgetInUse,
			GlobalReassemblyBudget::configure(config, budget));
	}
	EXPECT_EQ(0U, budget.active_clients());

	{
		TelemetryReassembler first(budget);
		TelemetryReassembler second(budget);
		EXPECT_TRUE(first.global_client_admitted());
		EXPECT_FALSE(second.global_client_admitted());
		for (std::uint32_t index = 0; index < MaxStateReassembliesPerClient; ++index) {
			const auto fragment = first_state_fragment(100U + index);
			ASSERT_EQ(ReassemblyResult::Accepted, first.ingest(fragment.view(), completed));
		}
		EXPECT_EQ(MaxStateReassemblyBytesPerClient, budget.reserved_bytes(MessageSizeClass::State));
		const auto denied = first_state_fragment(200U, 43U);
		EXPECT_EQ(ReassemblyResult::QuotaExceeded, second.ingest(denied.view(), completed));
		EXPECT_EQ(0U, second.reserved_bytes(MessageSizeClass::State));
	}
	EXPECT_EQ(0U, budget.active_clients());
	EXPECT_EQ(0U, budget.reserved_bytes(MessageSizeClass::State));
	{
		TelemetryReassembler replacement(budget);
		EXPECT_TRUE(replacement.global_client_admitted());
		const auto fragment = first_state_fragment(300U, 44U);
		EXPECT_EQ(ReassemblyResult::Accepted, replacement.ingest(fragment.view(), completed));
		EXPECT_EQ(MaxStateMessageSize, budget.reserved_bytes(MessageSizeClass::State));
	}
	EXPECT_EQ(0U, budget.active_clients());
	EXPECT_EQ(0U, budget.reserved_bytes(MessageSizeClass::State));
	EXPECT_EQ(3U, budget.counters().client_admissions);
	EXPECT_EQ(1U, budget.counters().client_admission_rejections);
	EXPECT_EQ(1U, budget.counters().peak_active_clients);
}

TEST(TelemetryProtocolSecurity, EveryKnownWireMessageHasReadOnlyTelemetryAuthority)
{
	constexpr std::array<MessageType, 20> known{{
		MessageType::Discovery,
		MessageType::Hello,
		MessageType::Welcome,
		MessageType::SessionBegin,
		MessageType::Manifest,
		MessageType::FullSnapshot,
		MessageType::Delta,
		MessageType::EventBatch,
		MessageType::Heartbeat,
		MessageType::Ack,
		MessageType::Nack,
		MessageType::ResyncRequest,
		MessageType::SessionEnd,
		MessageType::TargetVideoSubscribe,
		MessageType::TargetVideoConfig,
		MessageType::TargetVideoFrame,
		MessageType::TargetVideoKeyframeRequest,
		MessageType::TargetVideoStop,
		MessageType::TargetVideoStats,
		MessageType::CapabilityUpdate,
	}};
	static_assert(known.size() == FirstReservedMessageType - 1U, "all Phase 0 messages must be classified");
	for (const auto type : known) {
		EXPECT_TRUE(is_known_message_type(type));
		EXPECT_EQ(ProtocolAuthority::ReadOnlyTelemetry, message_authority(type));
	}
	EXPECT_EQ(ProtocolAuthority::Invalid, message_authority(MessageType::Invalid));
	EXPECT_EQ(ProtocolAuthority::Invalid, message_authority(static_cast<MessageType>(FirstReservedMessageType)));
}

} // namespace
