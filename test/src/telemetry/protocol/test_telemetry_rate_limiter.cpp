#include "telemetry/protocol/telemetry_rate_limiter.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>

namespace {

using namespace telemetry::protocol;

EndpointKey endpoint(std::uint8_t host = 1U, std::uint16_t port = 7808U)
{
	return EndpointKey::from_ipv4({{192U, 0U, 2U, host}}, port);
}

RateLimitTargetKey target(std::uint8_t message_type, std::uint32_t message_id, std::uint32_t message_crc32)
{
	RateLimitTargetKey result;
	result.message_type = message_type;
	result.message_id = message_id;
	result.message_crc32 = message_crc32;
	return result;
}

RateLimitTargetKey target(MessageType message_type, std::uint32_t message_id, std::uint32_t message_crc32)
{
	return target(static_cast<std::uint8_t>(message_type), message_id, message_crc32);
}

std::unique_ptr<ProtocolRateLimiter> configured_limiter(const ProtocolRateLimiterConfig& config,
	std::uint64_t initial_time_us = 0U)
{
	auto limiter = std::make_unique<ProtocolRateLimiter>();
	EXPECT_EQ(ValidationError::None, ProtocolRateLimiter::configure(config, initial_time_us, *limiter));
	return limiter;
}

TEST(TelemetryProtocolRateLimiter, SevenProtocolProfilesAreExactAndRespectMaximumHelper)
{
	struct ExpectedProfile {
		RateLimitClass limit_class;
		std::uint32_t rate;
		std::uint32_t burst;
	};
	const std::array<ExpectedProfile, 7> expected{{
		{RateLimitClass::Hello, 4U, 8U},
		{RateLimitClass::HeartbeatRequest, 10U, 20U},
		{RateLimitClass::ResyncRequest, 1U, 2U},
		{RateLimitClass::CapabilityUpdate, 2U, 2U},
		{RateLimitClass::AckNack, 100U, 200U},
		{RateLimitClass::UnsupportedMessageNack, 10U, 10U},
		{RateLimitClass::SessionCreation, 1U, 4U},
	}};
	for (const auto& test_case : expected) {
		SCOPED_TRACE(static_cast<unsigned>(test_case.limit_class));
		const auto profile = rate_limit_profile(test_case.limit_class);
		EXPECT_EQ(test_case.rate, profile.tokens_per_second);
		EXPECT_EQ(test_case.burst, profile.burst_tokens);
		EXPECT_TRUE(rate_limit_profile_is_at_or_below(profile, profile));
		EXPECT_TRUE(rate_limit_profile_is_at_or_below(
			RateLimitProfile{profile.tokens_per_second - 1U, profile.burst_tokens - 1U},
			profile));
		EXPECT_FALSE(
			rate_limit_profile_is_at_or_below(RateLimitProfile{profile.tokens_per_second + 1U, profile.burst_tokens},
				profile));
		EXPECT_FALSE(
			rate_limit_profile_is_at_or_below(RateLimitProfile{profile.tokens_per_second, profile.burst_tokens + 1U},
				profile));
	}
	const auto invalid = rate_limit_profile(static_cast<RateLimitClass>(0xffU));
	EXPECT_EQ(0U, invalid.tokens_per_second);
	EXPECT_EQ(0U, invalid.burst_tokens);
}

TEST(TelemetryProtocolRateLimiter, EveryProtocolBucketStartsBurstFull)
{
	const std::array<RateLimitClass, 7> classes{{
		RateLimitClass::Hello,
		RateLimitClass::HeartbeatRequest,
		RateLimitClass::ResyncRequest,
		RateLimitClass::CapabilityUpdate,
		RateLimitClass::AckNack,
		RateLimitClass::UnsupportedMessageNack,
		RateLimitClass::SessionCreation,
	}};
	for (const auto limit_class : classes) {
		SCOPED_TRACE(static_cast<unsigned>(limit_class));
		const auto profile = rate_limit_profile(limit_class);
		TokenBucket bucket(profile, 123456U);
		EXPECT_EQ(profile.tokens_per_second, bucket.profile().tokens_per_second);
		EXPECT_EQ(profile.burst_tokens, bucket.profile().burst_tokens);
		EXPECT_EQ(123456U, bucket.last_refill_time_us());
		EXPECT_EQ(profile.burst_tokens, bucket.whole_tokens_available());
		EXPECT_EQ(0U, bucket.fractional_credit_units());
		EXPECT_FALSE(bucket.clock_regressed());
		ASSERT_EQ(TokenBucketResult::Allowed, bucket.try_consume(123456U, profile.burst_tokens));
		EXPECT_EQ(0U, bucket.whole_tokens_available());
		EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(123456U));
	}
}

TEST(TelemetryProtocolRateLimiter, ContinuousPartialRefillHasNoPartitionRoundingDrift)
{
	struct RefillCase {
		std::uint32_t rate;
		std::uint32_t burst;
		std::uint64_t elapsed;
	};
	const std::array<RefillCase, 3> cases{{
		{1U, 4U, 54321U},
		{3U, 4U, 54321U},
		{137U, 10U, 54321U},
	}};
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.rate);
		const RateLimitProfile profile{test_case.rate, test_case.burst};
		TokenBucket one_call(profile, 0U);
		TokenBucket many_calls(profile, 0U);
		ASSERT_EQ(TokenBucketResult::Allowed, one_call.try_consume(0U, test_case.burst));
		ASSERT_EQ(TokenBucketResult::Allowed, many_calls.try_consume(0U, test_case.burst));

		EXPECT_EQ(TokenBucketResult::RateLimited,
			one_call.try_consume(test_case.elapsed, std::numeric_limits<std::uint32_t>::max()));
		for (const auto timestamp : {1U, 17U, 111U, 1000U, 12345U, 40000U, 54321U}) {
			EXPECT_EQ(TokenBucketResult::RateLimited,
				many_calls.try_consume(timestamp, std::numeric_limits<std::uint32_t>::max()));
		}
		const auto expected_units = test_case.elapsed * test_case.rate;
		EXPECT_EQ(expected_units / RateLimitTimeUnitsPerSecond, one_call.whole_tokens_available());
		EXPECT_EQ(expected_units % RateLimitTimeUnitsPerSecond, one_call.fractional_credit_units());
		EXPECT_EQ(one_call.whole_tokens_available(), many_calls.whole_tokens_available());
		EXPECT_EQ(one_call.fractional_credit_units(), many_calls.fractional_credit_units());
	}
}

TEST(TelemetryProtocolRateLimiter, FractionalCreditSurvivesRepeatedNearBoundaryConsumption)
{
	TokenBucket bucket(RateLimitProfile{3U, 10U}, 0U);
	ASSERT_EQ(TokenBucketResult::Allowed, bucket.try_consume(0U, 10U));
	EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(333333U));
	EXPECT_EQ(0U, bucket.whole_tokens_available());
	EXPECT_EQ(999999U, bucket.fractional_credit_units());
	EXPECT_EQ(TokenBucketResult::Allowed, bucket.try_consume(333334U));
	EXPECT_EQ(0U, bucket.whole_tokens_available());
	EXPECT_EQ(2U, bucket.fractional_credit_units());
	EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(666666U));
	EXPECT_EQ(999998U, bucket.fractional_credit_units());
	EXPECT_EQ(TokenBucketResult::Allowed, bucket.try_consume(666667U));
	EXPECT_EQ(1U, bucket.fractional_credit_units());
}

TEST(TelemetryProtocolRateLimiter, ExactTokenBoundaryAllowsAndOneMicrosecondBeforeDenies)
{
	TokenBucket bucket(RateLimitProfile{4U, 1U}, 100000U);
	ASSERT_EQ(TokenBucketResult::Allowed, bucket.try_consume(100000U));
	EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(349999U));
	EXPECT_EQ(0U, bucket.whole_tokens_available());
	EXPECT_EQ(999996U, bucket.fractional_credit_units());
	EXPECT_EQ(349999U, bucket.last_refill_time_us());
	EXPECT_EQ(TokenBucketResult::Allowed, bucket.try_consume(350000U));
	EXPECT_EQ(0U, bucket.whole_tokens_available());
	EXPECT_EQ(0U, bucket.fractional_credit_units());
	EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(350000U));
}

TEST(TelemetryProtocolRateLimiter, MultiTokenCostsAreAtomicAndDeniedCostsDoNotConsumeCredit)
{
	TokenBucket bucket(RateLimitProfile{10U, 20U}, 100U);
	EXPECT_EQ(TokenBucketResult::Allowed, bucket.try_consume(100U, 7U));
	EXPECT_EQ(13U, bucket.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(100U, 14U));
	EXPECT_EQ(13U, bucket.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::Allowed, bucket.try_consume(100U, 13U));
	EXPECT_EQ(0U, bucket.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(100U, std::numeric_limits<std::uint32_t>::max()));
	EXPECT_EQ(0U, bucket.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::Allowed, bucket.try_consume(200100U, 2U));
	EXPECT_EQ(0U, bucket.whole_tokens_available());
	EXPECT_EQ(0U, bucket.fractional_credit_units());
}

TEST(TelemetryProtocolRateLimiter, RefillSaturatesWithoutOverflowForHugeElapsedAndProfiles)
{
	TokenBucket ordinary(RateLimitProfile{100U, 200U}, 0U);
	ASSERT_EQ(TokenBucketResult::Allowed, ordinary.try_consume(0U, 200U));
	EXPECT_EQ(TokenBucketResult::Allowed, ordinary.try_consume(std::numeric_limits<std::uint64_t>::max(), 200U));
	EXPECT_EQ(0U, ordinary.whole_tokens_available());
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), ordinary.last_refill_time_us());

	const auto maximum = std::numeric_limits<std::uint32_t>::max();
	TokenBucket maximum_bucket(RateLimitProfile{maximum, maximum}, 0U);
	EXPECT_EQ(maximum, maximum_bucket.whole_tokens_available());
	ASSERT_EQ(TokenBucketResult::Allowed, maximum_bucket.try_consume(0U, maximum));
	EXPECT_EQ(0U, maximum_bucket.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::Allowed,
		maximum_bucket.try_consume(std::numeric_limits<std::uint64_t>::max(), maximum));
	EXPECT_EQ(0U, maximum_bucket.whole_tokens_available());
	EXPECT_EQ(0U, maximum_bucket.fractional_credit_units());
}

TEST(TelemetryProtocolRateLimiter, InvalidCostDoesNotRefillConsumeOrAlterClockState)
{
	TokenBucket bucket(RateLimitProfile{10U, 2U}, 100U);
	ASSERT_EQ(TokenBucketResult::Allowed, bucket.try_consume(100U, 2U));
	EXPECT_EQ(TokenBucketResult::InvalidCost, bucket.try_consume(200U, 0U));
	EXPECT_EQ(100U, bucket.last_refill_time_us());
	EXPECT_EQ(0U, bucket.whole_tokens_available());
	EXPECT_EQ(0U, bucket.fractional_credit_units());
	EXPECT_FALSE(bucket.clock_regressed());

	EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(200U));
	EXPECT_EQ(1000U, bucket.fractional_credit_units());
	EXPECT_EQ(TokenBucketResult::InvalidCost, bucket.try_consume(99U, 0U));
	EXPECT_FALSE(bucket.clock_regressed());
	EXPECT_EQ(200U, bucket.last_refill_time_us());
}

TEST(TelemetryProtocolRateLimiter, BackwardsClockLatchesUntilExplicitReset)
{
	TokenBucket bucket(RateLimitProfile{2U, 3U}, 100U);
	ASSERT_EQ(TokenBucketResult::Allowed, bucket.try_consume(100U));
	EXPECT_EQ(2U, bucket.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::ClockRegressed, bucket.try_consume(99U));
	EXPECT_TRUE(bucket.clock_regressed());
	EXPECT_EQ(100U, bucket.last_refill_time_us());
	EXPECT_EQ(2U, bucket.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::ClockRegressed, bucket.try_consume(std::numeric_limits<std::uint64_t>::max()));
	EXPECT_EQ(2U, bucket.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::InvalidCost, bucket.try_consume(100U, 0U));
	EXPECT_TRUE(bucket.clock_regressed());

	bucket.reset(500U);
	EXPECT_FALSE(bucket.clock_regressed());
	EXPECT_EQ(500U, bucket.last_refill_time_us());
	EXPECT_EQ(3U, bucket.whole_tokens_available());
	EXPECT_EQ(0U, bucket.fractional_credit_units());
	EXPECT_EQ(TokenBucketResult::Allowed, bucket.try_consume(500U, 3U));
	EXPECT_EQ(TokenBucketResult::RateLimited, bucket.try_consume(500U));
}

TEST(TelemetryProtocolRateLimiter, ZeroRateAndZeroBurstProfilesHaveDefinedSafeBehavior)
{
	TokenBucket no_refill(RateLimitProfile{0U, 3U}, 7U);
	EXPECT_EQ(3U, no_refill.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::Allowed, no_refill.try_consume(7U, 3U));
	EXPECT_EQ(TokenBucketResult::RateLimited, no_refill.try_consume(std::numeric_limits<std::uint64_t>::max()));
	EXPECT_EQ(0U, no_refill.whole_tokens_available());
	no_refill.reset(9U);
	EXPECT_EQ(3U, no_refill.whole_tokens_available());

	TokenBucket no_capacity(RateLimitProfile{3U, 0U}, 0U);
	EXPECT_EQ(0U, no_capacity.whole_tokens_available());
	EXPECT_EQ(TokenBucketResult::RateLimited, no_capacity.try_consume(1000000U));
	EXPECT_EQ(0U, no_capacity.whole_tokens_available());
	EXPECT_EQ(0U, no_capacity.fractional_credit_units());

	TokenBucket inert(RateLimitProfile{0U, 0U}, 42U);
	EXPECT_EQ(TokenBucketResult::RateLimited, inert.try_consume(42U));
	EXPECT_EQ(TokenBucketResult::InvalidCost, inert.try_consume(42U, 0U));
	EXPECT_EQ(0U, inert.whole_tokens_available());
	EXPECT_EQ(0U, inert.fractional_credit_units());
}

TEST(TelemetryProtocolRateLimiter, RegistryConfigurationAcceptsOnlyBoundedProfilesAndFixedCapacities)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 2U;
	config.limits.max_sessions = 2U;
	config.limits.max_targets = 3U;
	config.limits.idle_expiry_us = 100U;
	auto limiter = configured_limiter(config, 10U);
	ASSERT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_pre_session(RateLimitClass::Hello, endpoint(), 10U));
	EXPECT_EQ(1U, limiter->pre_session_entry_count());
	EXPECT_EQ(1U, limiter->counters().allowed);

	const std::array<RateLimitProfile ProtocolRateLimitProfiles::*, 8> profile_members{{
		&ProtocolRateLimitProfiles::hello,
		&ProtocolRateLimitProfiles::heartbeat_request,
		&ProtocolRateLimitProfiles::resync_request,
		&ProtocolRateLimitProfiles::capability_update,
		&ProtocolRateLimitProfiles::ack_nack,
		&ProtocolRateLimitProfiles::unsupported_message_nack,
		&ProtocolRateLimitProfiles::session_creation,
		&ProtocolRateLimitProfiles::target_ack_nack,
	}};
	const std::array<RateLimitProfile, 8> maxima{{
		HelloRateLimit,
		HeartbeatRequestRateLimit,
		ResyncRequestRateLimit,
		CapabilityUpdateRateLimit,
		AckNackRateLimit,
		UnsupportedMessageNackRateLimit,
		SessionCreationRateLimit,
		AckNackRateLimit,
	}};
	for (std::size_t index = 0; index < profile_members.size(); ++index) {
		SCOPED_TRACE(index);
		auto invalid = config;
		(invalid.profiles.*profile_members[index]) = maxima[index];
		++(invalid.profiles.*profile_members[index]).tokens_per_second;
		EXPECT_EQ(ValidationError::OutOfRange, ProtocolRateLimiter::configure(invalid, 20U, *limiter));
		invalid = config;
		(invalid.profiles.*profile_members[index]) = maxima[index];
		++(invalid.profiles.*profile_members[index]).burst_tokens;
		EXPECT_EQ(ValidationError::OutOfRange, ProtocolRateLimiter::configure(invalid, 20U, *limiter));
		EXPECT_EQ(1U, limiter->pre_session_entry_count());
		EXPECT_EQ(1U, limiter->counters().allowed);
	}

	for (std::size_t field = 0; field < 7U; ++field) {
		SCOPED_TRACE(field);
		auto invalid = config;
		switch (field) {
		case 0:
			invalid.limits.max_pre_session_sources = 0U;
			break;
		case 1:
			invalid.limits.max_pre_session_sources = RateLimitMaximumPreSessionSources + 1U;
			break;
		case 2:
			invalid.limits.max_sessions = 0U;
			break;
		case 3:
			invalid.limits.max_sessions = RateLimitMaximumSessions + 1U;
			break;
		case 4:
			invalid.limits.max_targets = 0U;
			break;
		case 5:
			invalid.limits.max_targets = RateLimitMaximumTargets + 1U;
			break;
		case 6:
			invalid.limits.idle_expiry_us = 0U;
			break;
		}
		EXPECT_EQ(ValidationError::OutOfRange, ProtocolRateLimiter::configure(invalid, 20U, *limiter));
		EXPECT_EQ(1U, limiter->pre_session_entry_count());
	}

	auto disabled = config;
	disabled.profiles = ProtocolRateLimitProfiles{};
	for (const auto member : profile_members) {
		(disabled.profiles.*member) = RateLimitProfile{};
	}
	EXPECT_EQ(ValidationError::None, ProtocolRateLimiter::configure(disabled, 30U, *limiter));
	EXPECT_EQ(0U, limiter->pre_session_entry_count());
	EXPECT_EQ(0U, limiter->counters().allowed);
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited,
		limiter->consume_pre_session(RateLimitClass::Hello, endpoint(), 30U));
	EXPECT_EQ(0U, limiter->pre_session_entry_count());
}

TEST(TelemetryProtocolRateLimiter, PreSessionIdentityIsCanonicalSourceIpAndIgnoresUdpPort)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 2U;
	config.limits.max_sessions = 1U;
	config.limits.max_targets = 1U;
	config.profiles.hello = RateLimitProfile{0U, 2U};
	config.profiles.session_creation = RateLimitProfile{0U, 1U};
	auto limiter = configured_limiter(config);
	const auto first_port = endpoint(1U, 7000U);
	const auto second_port = endpoint(1U, 7001U);
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_pre_session(RateLimitClass::Hello, first_port, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_pre_session(RateLimitClass::Hello, second_port, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited,
		limiter->consume_pre_session(RateLimitClass::Hello, first_port, 0U));
	EXPECT_EQ(1U, limiter->pre_session_entry_count());

	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_pre_session(RateLimitClass::SessionCreation, second_port, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited,
		limiter->consume_pre_session(RateLimitClass::SessionCreation, first_port, 0U));

	std::array<std::uint8_t, 16> mapped{};
	mapped[10U] = 0xffU;
	mapped[11U] = 0xffU;
	mapped[12U] = 192U;
	mapped[13U] = 0U;
	mapped[14U] = 2U;
	mapped[15U] = 1U;
	const auto mapped_endpoint = EndpointKey::from_ipv6(mapped, 9000U);
	ASSERT_EQ(IpAddressFamily::Ipv4, mapped_endpoint.family());
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited,
		limiter->consume_pre_session(RateLimitClass::Hello, mapped_endpoint, 0U));
	EXPECT_EQ(1U, limiter->pre_session_entry_count());

	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_pre_session(RateLimitClass::Hello, endpoint(2U), 0U));
	EXPECT_EQ(2U, limiter->pre_session_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::ResourceLimit,
		limiter->consume_pre_session(RateLimitClass::Hello, endpoint(3U), 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidIdentity,
		limiter->consume_pre_session(RateLimitClass::Hello, EndpointKey{}, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidClass,
		limiter->consume_pre_session(RateLimitClass::AckNack, first_port, 0U));
	EXPECT_EQ(2U, limiter->pre_session_entry_count());
}

TEST(TelemetryProtocolRateLimiter, SessionBucketsUseExactSessionAndEndpointAndRemainIndependentByClass)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 1U;
	config.limits.max_sessions = 3U;
	config.limits.max_targets = 1U;
	config.profiles.heartbeat_request = RateLimitProfile{0U, 1U};
	config.profiles.resync_request = RateLimitProfile{0U, 1U};
	config.profiles.capability_update = RateLimitProfile{0U, 1U};
	auto limiter = configured_limiter(config);
	const auto peer = endpoint(1U, 7000U);
	const auto other_port = endpoint(1U, 7001U);

	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 1U, peer, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 1U, peer, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_session(RateLimitClass::ResyncRequest, 1U, peer, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_session(RateLimitClass::CapabilityUpdate, 1U, peer, 0U));
	EXPECT_EQ(1U, limiter->session_entry_count());

	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 1U, other_port, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 2U, peer, 0U));
	EXPECT_EQ(3U, limiter->session_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::ResourceLimit,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 3U, peer, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidIdentity,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 0U, peer, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidIdentity,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 1U, EndpointKey{}, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidClass, limiter->consume_session(RateLimitClass::Hello, 1U, peer, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidClass, limiter->consume_session(RateLimitClass::AckNack, 1U, peer, 0U));
	EXPECT_EQ(3U, limiter->session_entry_count());
}

TEST(TelemetryProtocolRateLimiter, AckNackTargetIdentityIncludesEveryNormativeTupleField)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 1U;
	config.limits.max_sessions = 4U;
	config.limits.max_targets = 8U;
	config.profiles.ack_nack = RateLimitProfile{0U, 20U};
	config.profiles.target_ack_nack = RateLimitProfile{0U, 1U};
	auto limiter = configured_limiter(config);
	const auto peer = endpoint();
	const auto base = target(MessageType::FullSnapshot, 7U, 0x12345678U);
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(1U, peer, base, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited, limiter->consume_ack_nack(1U, peer, base, 0U));

	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::Delta, 7U, 0x12345678U), 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::FullSnapshot, 8U, 0x12345678U), 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::FullSnapshot, 7U, 0x87654321U), 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::FullSnapshot, 9U, 0U), 0U));
	EXPECT_EQ(5U, limiter->target_entry_count());

	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(1U, endpoint(1U, 7809U), base, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(2U, peer, base, 0U));
	EXPECT_EQ(7U, limiter->target_entry_count());
	EXPECT_EQ(3U, limiter->session_entry_count());

	EXPECT_EQ(ProtocolRateLimitResult::InvalidIdentity, limiter->consume_ack_nack(0U, peer, base, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidIdentity, limiter->consume_ack_nack(1U, EndpointKey{}, base, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidIdentity, limiter->consume_ack_nack(1U, peer, target(0U, 1U, 1U), 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidIdentity,
		limiter->consume_ack_nack(1U, peer, target(MessageType::Ack, 0U, 1U), 0U));
	EXPECT_EQ(7U, limiter->target_entry_count());
}

TEST(TelemetryProtocolRateLimiter, CommonAckNackBucketIsSharedAndDeniedTargetsAreNotReserved)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 1U;
	config.limits.max_sessions = 1U;
	config.limits.max_targets = 4U;
	config.profiles.ack_nack = RateLimitProfile{2U, 2U};
	config.profiles.target_ack_nack = RateLimitProfile{0U, 4U};
	auto limiter = configured_limiter(config);
	const auto peer = endpoint();
	const auto first = target(MessageType::Ack, 1U, 1U);
	const auto second = target(MessageType::Nack, 2U, 2U);
	const auto third = target(MessageType::FullSnapshot, 3U, 3U);
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(1U, peer, first, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(1U, peer, second, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited, limiter->consume_ack_nack(1U, peer, third, 0U));
	EXPECT_EQ(2U, limiter->target_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited, limiter->consume_ack_nack(1U, peer, third, 499'999U));
	EXPECT_EQ(2U, limiter->target_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(1U, peer, third, 500'000U));
	EXPECT_EQ(3U, limiter->target_entry_count());
}

TEST(TelemetryProtocolRateLimiter, UnsupportedDenialConsumesNoneOfCommonUnsupportedOrNewTarget)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 1U;
	config.limits.max_sessions = 1U;
	config.limits.max_targets = 4U;
	config.profiles.ack_nack = RateLimitProfile{0U, 2U};
	config.profiles.unsupported_message_nack = RateLimitProfile{0U, 1U};
	config.profiles.target_ack_nack = RateLimitProfile{0U, 2U};
	auto limiter = configured_limiter(config);
	const auto peer = endpoint();
	const auto first = target(FirstReservedMessageType, 1U, 1U);
	const auto second = target(static_cast<std::uint8_t>(FirstReservedMessageType + 1U), 2U, 2U);
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_unsupported_message_nack(1U, peer, first, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited, limiter->consume_unsupported_message_nack(1U, peer, second, 0U));
	EXPECT_EQ(1U, limiter->target_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::InvalidClass, limiter->consume_ack_nack(1U, peer, second, 0U));
	EXPECT_EQ(1U, limiter->target_entry_count());

	// The denied Unsupported attempt must not have consumed the last common
	// ACK/NACK token, and its new target must not have been installed.
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::FullSnapshot, 2U, 2U), 0U));
	EXPECT_EQ(2U, limiter->target_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited,
		limiter->consume_ack_nack(1U, peer, target(MessageType::Ack, 3U, 3U), 0U));
	EXPECT_EQ(2U, limiter->target_entry_count());
}

TEST(TelemetryProtocolRateLimiter, UnsupportedTombstoneSurvivesIdleExpiryAndConsumesNoTokens)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 1U;
	config.limits.max_sessions = 1U;
	config.limits.max_targets = 3U;
	config.limits.idle_expiry_us = 100U;
	config.profiles.ack_nack = RateLimitProfile{0U, 2U};
	config.profiles.unsupported_message_nack = RateLimitProfile{0U, 2U};
	config.profiles.target_ack_nack = RateLimitProfile{0U, 1U};
	auto limiter = configured_limiter(config);
	const auto peer = endpoint();
	const auto first = target(FirstReservedMessageType, 1U, 1U);
	const auto second = target(static_cast<std::uint8_t>(FirstReservedMessageType + 1U), 2U, 2U);
	const auto third = target(static_cast<std::uint8_t>(FirstReservedMessageType + 2U), 3U, 3U);
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_unsupported_message_nack(1U, peer, first, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited, limiter->consume_unsupported_message_nack(1U, peer, first, 0U));
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_unsupported_message_nack(1U, peer, second, 0U));
	EXPECT_EQ(2U, limiter->target_entry_count());
	EXPECT_EQ(1U, limiter->session_entry_count());

	// Only the idle session entry expires. UnsupportedMessage tombstones remain
	// live until the exact session is explicitly purged or the limiter resets.
	EXPECT_EQ(1U, limiter->purge_expired(100U));
	EXPECT_EQ(2U, limiter->target_entry_count());
	EXPECT_EQ(0U, limiter->session_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited, limiter->consume_unsupported_message_nack(1U, peer, first, 100U));
	EXPECT_EQ(0U, limiter->session_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_unsupported_message_nack(1U, peer, third, 100U));
	EXPECT_EQ(3U, limiter->target_entry_count());
	EXPECT_EQ(1U, limiter->session_entry_count());
	EXPECT_EQ(3U, limiter->counters().allowed);
	EXPECT_EQ(2U, limiter->counters().rate_limited);
	EXPECT_EQ(4U, limiter->purge_session(1U, peer));
	EXPECT_EQ(0U, limiter->target_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_unsupported_message_nack(1U, peer, first, 100U));
}

TEST(TelemetryProtocolRateLimiter, UnsupportedRequiresAnUnknownTypeAndTargetCapacityIsHard)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 1U;
	config.limits.max_sessions = 1U;
	config.limits.max_targets = 1U;
	config.profiles.ack_nack = RateLimitProfile{0U, 4U};
	config.profiles.unsupported_message_nack = RateLimitProfile{0U, 4U};
	config.profiles.target_ack_nack = RateLimitProfile{0U, 4U};
	auto limiter = configured_limiter(config);
	const auto peer = endpoint();
	EXPECT_EQ(ProtocolRateLimitResult::InvalidClass,
		limiter->consume_unsupported_message_nack(1U, peer, target(MessageType::CapabilityUpdate, 1U, 1U), 0U));
	EXPECT_EQ(ProtocolRateLimitResult::InvalidClass,
		limiter->consume_ack_nack(1U, peer, target(FirstReservedMessageType, 1U, 1U), 0U));
	EXPECT_EQ(0U, limiter->session_entry_count());
	EXPECT_EQ(0U, limiter->target_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_unsupported_message_nack(1U,
			peer,
			target(std::numeric_limits<std::uint8_t>::max(), 1U, 0U),
			0U));
	EXPECT_EQ(1U, limiter->target_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::ResourceLimit,
		limiter->consume_unsupported_message_nack(1U, peer, target(FirstReservedMessageType, 2U, 2U), 0U));
	EXPECT_EQ(1U, limiter->target_entry_count());
	EXPECT_EQ(1U, limiter->counters().allowed);
	EXPECT_EQ(1U, limiter->counters().resource_limited);
	EXPECT_EQ(2U, limiter->counters().invalid_class);
}

TEST(TelemetryProtocolRateLimiter, IdleExpiryIsExactRefreshesOnDenialAndReclaimsCapacity)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 1U;
	config.limits.max_sessions = 1U;
	config.limits.max_targets = 1U;
	config.limits.idle_expiry_us = 100U;
	config.profiles.hello = RateLimitProfile{0U, 1U};
	config.profiles.heartbeat_request = RateLimitProfile{0U, 1U};
	config.profiles.ack_nack = RateLimitProfile{0U, 2U};
	config.profiles.target_ack_nack = RateLimitProfile{0U, 1U};
	auto limiter = configured_limiter(config);
	const auto peer = endpoint();
	ASSERT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_pre_session(RateLimitClass::Hello, peer, 0U));
	ASSERT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 1U, peer, 0U));
	ASSERT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::Ack, 1U, 1U), 0U));
	EXPECT_EQ(ProtocolRateLimitResult::RateLimited, limiter->consume_pre_session(RateLimitClass::Hello, peer, 50U));
	EXPECT_EQ(0U, limiter->purge_expired(99U));
	EXPECT_EQ(2U, limiter->purge_expired(100U));
	EXPECT_EQ(1U, limiter->pre_session_entry_count());
	EXPECT_EQ(0U, limiter->session_entry_count());
	EXPECT_EQ(0U, limiter->target_entry_count());
	EXPECT_EQ(1U, limiter->purge_expired(150U));
	EXPECT_EQ(0U, limiter->pre_session_entry_count());

	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::Ack, 2U, 2U), 150U));
	EXPECT_EQ(1U, limiter->target_entry_count());
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::Ack, 3U, 3U), 250U));
	EXPECT_EQ(1U, limiter->target_entry_count());
}

TEST(TelemetryProtocolRateLimiter, ExplicitPurgeUsesExactIdentityAndResetClearsAllState)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 2U;
	config.limits.max_sessions = 2U;
	config.limits.max_targets = 3U;
	auto limiter = configured_limiter(config, 100U);
	const auto peer = endpoint(1U, 7000U);
	const auto same_ip_other_port = endpoint(1U, 7001U);
	ASSERT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_pre_session(RateLimitClass::Hello, peer, 100U));
	EXPECT_EQ(1U, limiter->purge_source(same_ip_other_port));
	EXPECT_EQ(0U, limiter->purge_source(peer));

	ASSERT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::Ack, 1U, 1U), 100U));
	ASSERT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_ack_nack(1U, peer, target(MessageType::Nack, 2U, 2U), 100U));
	EXPECT_EQ(0U, limiter->purge_session(1U, same_ip_other_port));
	EXPECT_EQ(0U, limiter->purge_session(2U, peer));
	EXPECT_EQ(3U, limiter->purge_session(1U, peer));
	EXPECT_EQ(0U, limiter->session_entry_count());
	EXPECT_EQ(0U, limiter->target_entry_count());

	ASSERT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 1U, peer, 100U));
	EXPECT_GT(limiter->counters().allowed, 0U);
	limiter->reset(500U);
	EXPECT_EQ(0U, limiter->pre_session_entry_count());
	EXPECT_EQ(0U, limiter->session_entry_count());
	EXPECT_EQ(0U, limiter->target_entry_count());
	EXPECT_EQ(0U, limiter->counters().allowed);
	EXPECT_EQ(config.limits.max_targets, limiter->config().limits.max_targets);
	EXPECT_EQ(ProtocolRateLimitResult::ClockRegressed,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 1U, peer, 499U));
	EXPECT_EQ(0U, limiter->session_entry_count());
	// A registry-level regression fails closed until the caller explicitly
	// establishes a new monotonic epoch.
	limiter->reset(500U);
	EXPECT_EQ(ProtocolRateLimitResult::Allowed,
		limiter->consume_session(RateLimitClass::HeartbeatRequest, 1U, peer, 500U));
}

TEST(TelemetryProtocolRateLimiter, ClockRegressionLatchesAcrossAtomicBucketsUntilReset)
{
	ProtocolRateLimiterConfig config;
	config.limits.max_pre_session_sources = 1U;
	config.limits.max_sessions = 1U;
	config.limits.max_targets = 2U;
	auto limiter = configured_limiter(config, 100U);
	const auto peer = endpoint();
	const auto key = target(MessageType::Ack, 1U, 1U);
	ASSERT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(1U, peer, key, 100U));
	ASSERT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(1U, peer, key, 1'000U));
	const auto unseen_after_regression = target(MessageType::Nack, 2U, 2U);
	EXPECT_EQ(ProtocolRateLimitResult::ClockRegressed,
		limiter->consume_ack_nack(1U, peer, unseen_after_regression, 999U));
	EXPECT_EQ(ProtocolRateLimitResult::ClockRegressed,
		limiter->consume_ack_nack(1U, peer, unseen_after_regression, 1'001U));
	EXPECT_EQ(1U, limiter->session_entry_count());
	EXPECT_EQ(1U, limiter->target_entry_count());
	EXPECT_EQ(2U, limiter->counters().clock_regressed);
	limiter->reset(2'000U);
	EXPECT_EQ(ProtocolRateLimitResult::Allowed, limiter->consume_ack_nack(1U, peer, key, 2'000U));
}

} // namespace
