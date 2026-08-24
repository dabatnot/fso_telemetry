#include "telemetry/config.h"

#include "cfile/cfilesystem.h"
#include "util/FSTestFixture.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using telemetry::ConfigLoadResult;
using telemetry::ConfigStatus;

constexpr std::size_t MaxConfigBytes = 16U * 1024U;

template <typename Enum>
constexpr auto enum_value(Enum value) noexcept
{
	static_assert(std::is_enum_v<Enum>, "Configuration diagnostics must remain a closed enum.");
	return static_cast<std::underlying_type_t<Enum>>(value);
}

ConfigLoadResult parse(std::string_view json) noexcept
{
	return telemetry::detail::parse_telemetry_config_json(json);
}

std::string object_with(std::string_view key, std::string_view value)
{
	if (key == "schemaVersion") {
		return std::string{"{\"schemaVersion\":"} + std::string{value} + "}";
	}

	return std::string{"{\"schemaVersion\":4,\""} + std::string{key} + "\":" + std::string{value} + "}";
}

std::string quoted_array(const std::vector<std::string>& values)
{
	std::string result{"["};
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i != 0U) {
			result += ',';
		}
		result += '"';
		result += values[i];
		result += '"';
	}
	result += ']';
	return result;
}

void expect_safe_defaults(const ConfigLoadResult& result)
{
	std::array<std::uint8_t, 16> ipv4_loopback{};
	ipv4_loopback[0] = 127U;
	ipv4_loopback[3] = 1U;
	std::array<std::uint8_t, 16> ipv6_loopback{};
	ipv6_loopback[15] = 1U;

	EXPECT_FALSE(result.effective.enabled);
	EXPECT_EQ(4U, result.effective.schema_version);
	EXPECT_EQ(2U, result.effective.bind_addresses.size());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv4, result.effective.bind_addresses[0].family());
	EXPECT_EQ(ipv4_loopback, result.effective.bind_addresses[0].bytes());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv6, result.effective.bind_addresses[1].family());
	EXPECT_EQ(ipv6_loopback, result.effective.bind_addresses[1].bytes());
	EXPECT_EQ(42042U, result.effective.bind_port);
	EXPECT_EQ(2U, result.effective.allowed_clients.size());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv4, result.effective.allowed_clients[0].family());
	EXPECT_EQ(32U, result.effective.allowed_clients[0].prefix_length());
	EXPECT_EQ(ipv4_loopback, result.effective.allowed_clients[0].network());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv6, result.effective.allowed_clients[1].family());
	EXPECT_EQ(128U, result.effective.allowed_clients[1].prefix_length());
	EXPECT_EQ(ipv6_loopback, result.effective.allowed_clients[1].network());
	EXPECT_FALSE(result.effective.discovery_enabled);
	EXPECT_EQ(telemetry::VisibilityMode::Cockpit, result.effective.visibility_mode);
	EXPECT_EQ(1U, result.effective.max_clients);
	EXPECT_EQ(30U, result.effective.flight_hz);
	EXPECT_EQ(10U, result.effective.systems_hz);
	EXPECT_EQ(2U, result.effective.keyframe_seconds);
	EXPECT_EQ(500U, result.effective.mission_heartbeat_ms);
	EXPECT_EQ(1000U, result.effective.idle_heartbeat_ms);
	EXPECT_EQ(64U, result.effective.max_datagrams_per_tick);
	EXPECT_EQ('\0', result.effective.communication_bundle_path[0]);
}

void expect_invalid(std::string_view json)
{
	const auto result = parse(json);
	EXPECT_EQ(ConfigStatus::Invalid, result.status);
	EXPECT_NE(0, enum_value(result.error));
	expect_safe_defaults(result);
}

void expect_valid_disabled(std::string_view json)
{
	const auto result = parse(json);
	EXPECT_EQ(ConfigStatus::ValidDisabled, result.status);
	EXPECT_EQ(0, enum_value(result.error));
}

void expect_valid_enabled(std::string_view json)
{
	const auto result = parse(json);
	EXPECT_EQ(ConfigStatus::ValidEnabled, result.status);
	EXPECT_EQ(0, enum_value(result.error));
	EXPECT_TRUE(result.effective.enabled);
}

class TelemetryConfigCFileIntegrationTest : public test::FSTestFixture {
  public:
	TelemetryConfigCFileIntegrationTest() : FSTestFixture(test::INIT_CFILE) {}
};

TEST(TelemetryConfigContract, MinimalObjectAppliesEveryFailClosedDefault)
{
	const auto result = parse(R"({"schemaVersion":4})");
	ASSERT_EQ(ConfigStatus::ValidDisabled, result.status);
	EXPECT_EQ(0, enum_value(result.error));
	expect_safe_defaults(result);
}

TEST(TelemetryConfigContract, VersionFourHasNoProducerProfileSelection)
{
	for (const auto input : {
			 R"({"schemaVersion":4,"profile":"CockpitSensors"})",
			 R"({"schemaVersion":4,"phase2Profile":"CompleteShip"})"}) {
		SCOPED_TRACE(input);
		const auto result = parse(input);
		EXPECT_EQ(ConfigStatus::Invalid, result.status);
		EXPECT_EQ(telemetry::ConfigError::UnknownKey, result.error);
	}
}

TEST(TelemetryConfigContract, VersionsOneThroughThreeAreRejected)
{
	for (const auto version : {1, 2, 3}) {
		const auto input = std::string{"{\"schemaVersion\":"} +
			std::to_string(version) + "}";
		SCOPED_TRACE(input);
		const auto result = parse(input);
		EXPECT_EQ(ConfigStatus::Invalid, result.status);
		EXPECT_EQ(telemetry::ConfigError::OutOfRange, result.error);
	}
}

TEST(TelemetryConfigContract, EnabledMinimalObjectUsesTheLoopbackOnlyProfile)
{
	const auto result = parse(R"({"schemaVersion":4,"enabled":true})");
	ASSERT_EQ(ConfigStatus::ValidEnabled, result.status);
	EXPECT_TRUE(result.effective.enabled);
	EXPECT_EQ(2U, result.effective.bind_addresses.size());
	EXPECT_EQ(2U, result.effective.allowed_clients.size());
	EXPECT_FALSE(result.effective.discovery_enabled);
}

TEST(TelemetryConfigContract, CommunicationBundlePathIsOptionalAndBounded)
{
	const auto configured = parse(
		R"({"schemaVersion":4,"enabled":true,"communicationBundlePath":"D:/bundles/comm"})");
	ASSERT_EQ(ConfigStatus::ValidEnabled, configured.status);
	EXPECT_STREQ("D:/bundles/comm", configured.effective.communication_bundle_path.data());

	expect_invalid(R"({"schemaVersion":4,"communicationBundlePath":7})");
	std::string too_long(telemetry::MaximumCommunicationBundlePathBytes + 1U, 'a');
	expect_invalid(object_with("communicationBundlePath", '"' + too_long + '"'));
}

TEST(TelemetryConfigContract, StrictJsonRejectsMissingSchemaSyntaxAndNonObjectRoots)
{
	const std::vector<std::string> invalid_inputs{
		"",
		" ",
		"null",
		"true",
		"1",
		"[]",
		R"({})",
		R"({"enabled":false})",
		R"({"schemaVersion":)",
		R"({"schemaVersion":4)",
		R"({"schemaVersion":4,})",
		R"(/*comment*/{"schemaVersion":4})",
		R"({"schemaVersion":4//comment
})",
	};

	for (const auto& input : invalid_inputs) {
		SCOPED_TRACE(input);
		expect_invalid(input);
	}
}

TEST(TelemetryConfigContract, StrictJsonRejectsDuplicateKeysAtRoot)
{
	expect_invalid(R"({"schemaVersion":4,"enabled":false,"enabled":true})");
	expect_invalid(R"({"schemaVersion":4,"schemaVersion":4})");
}

TEST(TelemetryConfigContract, StrictJsonRequiresEofButAllowsTrailingJsonWhitespace)
{
	expect_valid_disabled("{\"schemaVersion\":4}\r\n\t ");
	expect_invalid(R"({"schemaVersion":4}{"schemaVersion":4})");
	expect_invalid(R"({"schemaVersion":4}null)");
	expect_invalid(R"({"schemaVersion":4}x)");

	std::string trailing_nul{R"({"schemaVersion":4})"};
	trailing_nul.push_back('\0');
	expect_invalid(std::string_view{trailing_nul.data(), trailing_nul.size()});
}

TEST(TelemetryConfigContract, StrictJsonEnforcesTheSixteenKibibyteLimitBeforeParsing)
{
	std::string exact_limit{R"({"schemaVersion":4})"};
	exact_limit.append(MaxConfigBytes - exact_limit.size(), ' ');
	ASSERT_EQ(MaxConfigBytes, exact_limit.size());
	expect_valid_disabled(exact_limit);

	std::string over_limit = exact_limit;
	over_limit.push_back(' ');
	ASSERT_EQ(MaxConfigBytes + 1U, over_limit.size());
	expect_invalid(over_limit);
}

TEST(TelemetryConfigContract, DepthPreflightRejectsTruncatedDepthFiveBeforeCallingJansson)
{
	const auto result = parse(R"({"schemaVersion":4,"bindAddresses":[[[[)");

	ASSERT_EQ(ConfigStatus::Invalid, result.status);
	EXPECT_EQ(telemetry::ConfigError::MaximumDepthExceeded, result.error)
		<< "A truncated over-depth input must be rejected by the bounded lexical preflight, not the recursive parser.";
	expect_safe_defaults(result);
}

TEST(TelemetryConfigContract, DepthPreflightIgnoresStructuralCharactersAndEscapesInsideStrings)
{
	const auto structural_string =
		parse(R"json({"schemaVersion":4,"bindAddresses":["[[[[{}]]]]\"\\"]})json");
	ASSERT_EQ(ConfigStatus::Invalid, structural_string.status);
	EXPECT_EQ(telemetry::ConfigError::InvalidAddress, structural_string.error)
		<< "Braces, brackets, an escaped quote and an escaped backslash inside a JSON string are not containers.";

	const auto structural_key = parse(R"json({"schemaVersion":4,"[[[[{\"}]]]]":0})json");
	ASSERT_EQ(ConfigStatus::Invalid, structural_key.status);
	EXPECT_EQ(telemetry::ConfigError::UnknownKey, structural_key.error)
		<< "The lexical preflight must also ignore structural characters in object keys.";
}

// This adversarial input is intentionally opt-in. It must be launched as a
// dedicated process so that the pre-fix recursive Jansson parser cannot take
// down the normal unit-test run with a stack overflow.
TEST(TelemetryConfigDepthIsolationContract, DISABLED_DeepTruncatedContainerBombIsRejectedByPreflight)
{
	std::string bomb{R"({"schemaVersion":4,"bindAddresses":)"};
	bomb.append(8000U, '[');
	ASSERT_LT(bomb.size(), MaxConfigBytes);

	const auto result = parse(bomb);
	ASSERT_EQ(ConfigStatus::Invalid, result.status);
	EXPECT_EQ(telemetry::ConfigError::MaximumDepthExceeded, result.error);
}

TEST(TelemetryConfigContract, StrictJsonDistinguishesDepthFiveFromAValidlyParsedShallowTypeError)
{
	const auto depth_four = parse(R"({"schemaVersion":4,"bindAddresses":[[["127.0.0.1"]]]})");
	const auto depth_five = parse(R"({"schemaVersion":4,"bindAddresses":[[[["127.0.0.1"]]]]})");

	ASSERT_EQ(ConfigStatus::Invalid, depth_four.status);
	ASSERT_EQ(ConfigStatus::Invalid, depth_five.status);
	EXPECT_NE(0, enum_value(depth_four.error));
	EXPECT_NE(0, enum_value(depth_five.error));
	EXPECT_NE(depth_four.error, depth_five.error)
		<< "Depth five must be rejected by the parser limit, not merely by later schema validation.";
}

TEST(TelemetryConfigContract, ClosedSchemaRejectsEveryUnknownKeyWithoutPartialApplication)
{
	const auto result = parse(R"({"schemaVersion":4,"enabled":true,"futureOption":1})");
	ASSERT_EQ(ConfigStatus::Invalid, result.status);
	EXPECT_NE(0, enum_value(result.error));
	expect_safe_defaults(result);
}

TEST(TelemetryConfigContract, EveryConfigurationKeyHasAnExactJsonType)
{
	const std::vector<std::pair<std::string, std::string>> wrong_types{
		{"schemaVersion", "true"},
		{"schemaVersion", "1.0"},
		{"schemaVersion", R"("1")"},
		{"enabled", "1"},
		{"enabled", R"("true")"},
		{"bindAddresses", R"("127.0.0.1")"},
		{"bindAddresses", R"([127])"},
		{"bindPort", "42042.0"},
		{"bindPort", R"("42042")"},
		{"allowedClients", R"("127.0.0.1/32")"},
		{"allowedClients", R"([127])"},
		{"discoveryEnabled", "0"},
		{"discoveryEnabled", R"("false")"},
		{"visibilityMode", "false"},
		{"visibilityMode", "1"},
		{"maxClients", "1.0"},
		{"maxClients", R"("1")"},
		{"flightHz", "30.0"},
		{"flightHz", R"("30")"},
		{"keyframeSeconds", "2.0"},
		{"keyframeSeconds", R"("2")"},
		{"missionHeartbeatMs", "500.0"},
		{"missionHeartbeatMs", R"("500")"},
		{"idleHeartbeatMs", "1000.0"},
		{"idleHeartbeatMs", R"("1000")"},
		{"maxDatagramsPerTick", "64.0"},
		{"maxDatagramsPerTick", R"("64")"},
	};

	for (const auto& [key, value] : wrong_types) {
		SCOPED_TRACE(key + "=" + value);
		expect_invalid(object_with(key, value));
	}
}

TEST(TelemetryConfigContract, IntegerBoundsAcceptMinAndMaxAndRejectTheirNeighbours)
{
	struct BoundaryCase {
		const char* key;
		std::uint64_t minimum;
		std::uint64_t maximum;
	};

	const BoundaryCase cases[]{
		{"bindPort", 1024U, 65535U},
		{"maxClients", 1U, 4U},
		{"flightHz", 1U, 60U},
		{"keyframeSeconds", 1U, 5U},
		{"missionHeartbeatMs", 200U, 5000U},
		{"idleHeartbeatMs", 200U, 5000U},
		{"maxDatagramsPerTick", 1U, 256U},
	};

	for (const auto& item : cases) {
		SCOPED_TRACE(item.key);
		expect_invalid(object_with(item.key, std::to_string(item.minimum - 1U)));
		expect_valid_disabled(object_with(item.key, std::to_string(item.minimum)));
		expect_valid_disabled(object_with(item.key, std::to_string(item.maximum)));
		expect_invalid(object_with(item.key, std::to_string(item.maximum + 1U)));
		expect_invalid(object_with(item.key, "-1"));
	}
}

TEST(TelemetryConfigContract, ConfigurationValuesAreClosed)
{
	expect_valid_disabled(object_with("discoveryEnabled", "false"));
	expect_invalid(object_with("discoveryEnabled", "true"));
	expect_valid_disabled(object_with("visibilityMode", R"("Cockpit")"));
	expect_invalid(object_with("visibilityMode", R"("cockpit")"));
	expect_invalid(object_with("schemaVersion", "0"));
	expect_invalid(object_with("schemaVersion", "5"));
}

TEST(TelemetryConfigContract, BindAddressArrayIsBoundedNumericAndUniqueAfterBinaryCanonicalization)
{
	std::array<std::uint8_t, 16> ipv4_loopback{};
	ipv4_loopback[0] = 127U;
	ipv4_loopback[3] = 1U;
	std::array<std::uint8_t, 16> ipv6_loopback{};
	ipv6_loopback[15] = 1U;

	const auto one = parse(object_with("bindAddresses", R"(["127.0.0.1"])"));
	ASSERT_EQ(ConfigStatus::ValidDisabled, one.status);
	EXPECT_EQ(1U, one.effective.bind_addresses.size());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv4, one.effective.bind_addresses[0].family());
	EXPECT_EQ(ipv4_loopback, one.effective.bind_addresses[0].bytes());

	const auto two = parse(object_with("bindAddresses", R"(["127.0.0.1","::1"])"));
	ASSERT_EQ(ConfigStatus::ValidDisabled, two.status);
	EXPECT_EQ(2U, two.effective.bind_addresses.size());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv4, two.effective.bind_addresses[0].family());
	EXPECT_EQ(ipv4_loopback, two.effective.bind_addresses[0].bytes());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv6, two.effective.bind_addresses[1].family());
	EXPECT_EQ(ipv6_loopback, two.effective.bind_addresses[1].bytes());

	const std::vector<std::string> invalid_arrays{
		R"([])",
		R"(["127.0.0.1","::1","192.0.2.1"])",
		R"(["127.0.0.1","127.0.0.1"])",
		R"(["::1","0:0:0:0:0:0:0:1"])",
		R"(["localhost"])",
		R"(["example.invalid"])",
		R"(["fe80::1%3"])",
		R"(["fe80::1%eth0"])",
		R"(["127.0.0.1/32"])",
		R"([" 127.0.0.1"])",
		R"(["127.000.000.001"])",
		R"(["::ffff:127.0.0.1"])",
		R"(["[::1]"])",
		R"(["127.0.0.1:42042"])",
		R"(["[::1]:42042"])",
	};

	for (const auto& array : invalid_arrays) {
		SCOPED_TRACE(array);
		expect_invalid(object_with("bindAddresses", array));
	}
}

TEST(TelemetryConfigContract, AllowedClientArrayIsBoundedCanonicalAndUnique)
{
	std::array<std::uint8_t, 16> ipv4_loopback{};
	ipv4_loopback[0] = 127U;
	ipv4_loopback[3] = 1U;

	const auto one = parse(object_with("allowedClients", R"(["127.0.0.1/32"])"));
	ASSERT_EQ(ConfigStatus::ValidDisabled, one.status);
	EXPECT_EQ(1U, one.effective.allowed_clients.size());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv4, one.effective.allowed_clients[0].family());
	EXPECT_EQ(32U, one.effective.allowed_clients[0].prefix_length());
	EXPECT_EQ(ipv4_loopback, one.effective.allowed_clients[0].network());

	std::vector<std::string> maximum;
	for (std::uint32_t i = 0; i < 32U; ++i) {
		maximum.push_back("192.0.2." + std::to_string(i) + "/32");
	}
	const auto max_result = parse(object_with("allowedClients", quoted_array(maximum)));
	ASSERT_EQ(ConfigStatus::ValidDisabled, max_result.status);
	EXPECT_EQ(32U, max_result.effective.allowed_clients.size());
	EXPECT_EQ(telemetry::protocol::IpAddressFamily::Ipv4, max_result.effective.allowed_clients[0].family());
	EXPECT_EQ(32U, max_result.effective.allowed_clients[0].prefix_length());
	EXPECT_EQ(192U, max_result.effective.allowed_clients[0].network()[0]);
	EXPECT_EQ(2U, max_result.effective.allowed_clients[0].network()[2]);
	EXPECT_EQ(0U, max_result.effective.allowed_clients[0].network()[3]);
	EXPECT_EQ(31U, max_result.effective.allowed_clients[31].network()[3]);

	auto over_maximum = maximum;
	over_maximum.push_back("198.51.100.1/32");
	expect_invalid(object_with("allowedClients", quoted_array(over_maximum)));

	const std::vector<std::string> invalid_arrays{
		R"([])",
		R"(["127.0.0.1"])",
		R"(["localhost/32"])",
		R"(["127.0.0.1/-1"])",
		R"(["127.0.0.1/33"])",
		R"(["::1/129"])",
		R"(["fe80::1%eth0/128"])",
		R"(["::ffff:127.0.0.1/128"])",
		R"(["[::1]/128"])",
		R"(["127.0.0.1:42042/32"])",
		R"(["[::1]:42042/128"])",
		R"(["192.0.2.1/24"])",
		R"(["2001:db8::1/64"])",
		R"(["127.0.0.1/32","127.0.0.1/32"])",
		R"(["::1/128","0:0:0:0:0:0:0:1/128"])",
	};

	for (const auto& array : invalid_arrays) {
		SCOPED_TRACE(array);
		expect_invalid(object_with("allowedClients", array));
	}
}

TEST(TelemetryConfigContract, NonLoopbackExposureRequiresAnExplicitNonemptyAllowlist)
{
	expect_invalid(
		R"({"schemaVersion":4,"enabled":false,"bindAddresses":["192.0.2.10"],"allowedClients":["192.0.2.0/24"]})");
	expect_invalid(R"({"schemaVersion":4,"enabled":true,"bindAddresses":["192.0.2.10"]})");
	expect_invalid(
		R"({"schemaVersion":4,"enabled":true,"bindAddresses":["192.0.2.10"],"allowedClients":[]})");
	expect_valid_enabled(
		R"({"schemaVersion":4,"enabled":true,"bindAddresses":["192.0.2.10"],"allowedClients":["192.0.2.0/24"]})");

	// A wildcard bind is explicit and valid only with a narrowed allowlist.
	expect_valid_enabled(
		R"({"schemaVersion":4,"enabled":true,"bindAddresses":["0.0.0.0"],"allowedClients":["192.0.2.0/24"]})");
	expect_invalid(
		R"({"schemaVersion":4,"enabled":true,"bindAddresses":["0.0.0.0"],"allowedClients":["0.0.0.0/0"]})");
	expect_invalid(
		R"({"schemaVersion":4,"enabled":true,"bindAddresses":["::"],"allowedClients":["::/0"]})");
}

TEST(TelemetryConfigLocationContract, AbsentAndVpOnlyAreBothFailClosedAbsentResults)
{
	using telemetry::detail::ConfigLocationKind;
	using telemetry::detail::ConfigLocationObservation;

	const auto absent = telemetry::detail::load_telemetry_config_from_observation(
		ConfigLocationObservation{ConfigLocationKind::Absent, 0U, {}});
	ASSERT_EQ(ConfigStatus::Absent, absent.status);
	EXPECT_EQ(0, enum_value(absent.error));
	expect_safe_defaults(absent);

	const auto vp_only = telemetry::detail::load_telemetry_config_from_observation(ConfigLocationObservation{
		ConfigLocationKind::VpOnly, 4096U, R"({"schemaVersion":4,"enabled":true})"});
	ASSERT_EQ(ConfigStatus::Absent, vp_only.status);
	EXPECT_EQ(0, enum_value(vp_only.error));
	expect_safe_defaults(vp_only);
}

TEST(TelemetryConfigLocationContract, LooseUserGameAndActiveModLocationsAreAcceptedAtOffsetZero)
{
	using telemetry::detail::ConfigLocationKind;
	using telemetry::detail::ConfigLocationObservation;

	const ConfigLocationKind loose_locations[]{ConfigLocationKind::LooseUserRoot,
		ConfigLocationKind::LooseGameRoot,
		ConfigLocationKind::LooseActiveMod};

	for (const auto location : loose_locations) {
		SCOPED_TRACE(enum_value(location));
		const auto result = telemetry::detail::load_telemetry_config_from_observation(
			ConfigLocationObservation{location, 0U, R"({"schemaVersion":4,"enabled":true})"});
		EXPECT_EQ(ConfigStatus::ValidEnabled, result.status);
	}
}

TEST(TelemetryConfigLocationContract, AFoundLocationWithNonzeroArchiveOffsetIsNotParsedAsLoose)
{
	using telemetry::detail::ConfigLocationKind;
	using telemetry::detail::ConfigLocationObservation;

	const auto result = telemetry::detail::load_telemetry_config_from_observation(ConfigLocationObservation{
		ConfigLocationKind::LooseActiveMod, 1U, R"({"schemaVersion":4,"enabled":true})"});
	EXPECT_EQ(ConfigStatus::Absent, result.status);
	expect_safe_defaults(result);
}

TEST(TelemetryConfigLocationContract, ALooseFileStillUsesTheSameStrictParser)
{
	using telemetry::detail::ConfigLocationKind;
	using telemetry::detail::ConfigLocationObservation;

	const auto result = telemetry::detail::load_telemetry_config_from_observation(ConfigLocationObservation{
		ConfigLocationKind::LooseUserRoot, 0U, R"({"schemaVersion":4,"unknown":true})"});
	EXPECT_EQ(ConfigStatus::Invalid, result.status);
	EXPECT_NE(0, enum_value(result.error));
	expect_safe_defaults(result);
}

TEST_F(TelemetryConfigCFileIntegrationTest, LooseRootGameTelemetryJsonIsDiscoveredAndActivated)
{
	const auto location =
		cf_find_file_location("telemetry.json", CF_TYPE_CONFIG, CF_LOCATION_ROOT_GAME | CF_LOCATION_TYPE_ROOT);
	ASSERT_TRUE(location.found)
		<< "A loose data/config/telemetry.json in the game root must be discoverable through CF_TYPE_CONFIG.";
	ASSERT_EQ(0U, location.offset);
	ASSERT_EQ(nullptr, location.data_ptr);

	const auto result = telemetry::load_telemetry_config();
	ASSERT_EQ(ConfigStatus::ValidEnabled, result.status);
	EXPECT_EQ(telemetry::ConfigError::None, result.error);
	EXPECT_TRUE(result.effective.enabled);
}

TEST_F(TelemetryConfigCFileIntegrationTest, ActiveModConfigurationPrecedesLooseGameRoot)
{
	const auto active_mod =
		cf_find_file_location("telemetry.json", CF_TYPE_CONFIG, CF_LOCATION_ROOT_GAME | CF_LOCATION_TYPE_PRIMARY_MOD);
	ASSERT_TRUE(active_mod.found);
	ASSERT_EQ(0U, active_mod.offset);
	ASSERT_EQ(nullptr, active_mod.data_ptr);

	const auto game_root =
		cf_find_file_location("telemetry.json", CF_TYPE_CONFIG, CF_LOCATION_ROOT_GAME | CF_LOCATION_TYPE_ROOT);
	ASSERT_TRUE(game_root.found);
	ASSERT_EQ(0U, game_root.offset);
	ASSERT_EQ(nullptr, game_root.data_ptr);

	const auto result = telemetry::load_telemetry_config();
	ASSERT_EQ(ConfigStatus::ValidDisabled, result.status);
	EXPECT_EQ(telemetry::ConfigError::None, result.error);
	EXPECT_FALSE(result.effective.enabled);
}

} // namespace
