#pragma once

#include "telemetry/protocol/telemetry_session_context.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::uint64_t RateLimitTimeUnitsPerSecond = 1'000'000ULL;

struct RateLimitProfile {
	std::uint32_t tokens_per_second = 0;
	std::uint32_t burst_tokens = 0;
};

enum class RateLimitClass : std::uint8_t {
	Hello,
	HeartbeatRequest,
	ResyncRequest,
	CapabilityUpdate,
	AckNack,
	UnsupportedMessageNack,
	SessionCreation,
};

constexpr RateLimitProfile HelloRateLimit{4, 8};
constexpr RateLimitProfile HeartbeatRequestRateLimit{10, 20};
constexpr RateLimitProfile ResyncRequestRateLimit{1, 2};
constexpr RateLimitProfile CapabilityUpdateRateLimit{2, 2};
constexpr RateLimitProfile AckNackRateLimit{100, 200};
constexpr RateLimitProfile UnsupportedMessageNackRateLimit{10, 10};
constexpr RateLimitProfile SessionCreationRateLimit{1, 4};

// The profiles are protocol maxima. Bucket ownership supplies the required
// scope: source address before session, session/endpoint afterwards, and an
// additional target-message bucket for ACK/NACK.
constexpr bool rate_limit_profile_is_at_or_below(RateLimitProfile configured, RateLimitProfile maximum) noexcept
{
	return configured.tokens_per_second <= maximum.tokens_per_second && configured.burst_tokens <= maximum.burst_tokens;
}

constexpr RateLimitProfile rate_limit_profile(RateLimitClass limit_class) noexcept
{
	switch (limit_class) {
	case RateLimitClass::Hello:
		return HelloRateLimit;
	case RateLimitClass::HeartbeatRequest:
		return HeartbeatRequestRateLimit;
	case RateLimitClass::ResyncRequest:
		return ResyncRequestRateLimit;
	case RateLimitClass::CapabilityUpdate:
		return CapabilityUpdateRateLimit;
	case RateLimitClass::AckNack:
		return AckNackRateLimit;
	case RateLimitClass::UnsupportedMessageNack:
		return UnsupportedMessageNackRateLimit;
	case RateLimitClass::SessionCreation:
		return SessionCreationRateLimit;
	}
	return RateLimitProfile{};
}

enum class TokenBucketResult : std::uint8_t {
	Allowed,
	RateLimited,
	ClockRegressed,
	InvalidCost,
};

// Integer continuous-refill token bucket. Credit is retained in
// token-microseconds, so sub-token refill is never discarded and repeated
// calls do not accumulate rounding drift. The injected clock is never read
// internally. A backwards timestamp latches a safe failure until reset() is
// explicitly called; reset starts the bucket full as required by FSTL 1.0.
class TokenBucket {
  public:
	TokenBucket() noexcept = default;
	TokenBucket(RateLimitProfile profile, std::uint64_t initial_time_us) noexcept;

	TokenBucketResult try_consume(std::uint64_t now_us, std::uint32_t token_cost = 1) noexcept;

	void reset(std::uint64_t now_us) noexcept;

	RateLimitProfile profile() const noexcept
	{
		return m_profile;
	}
	std::uint64_t last_refill_time_us() const noexcept
	{
		return m_last_refill_time_us;
	}
	std::uint32_t whole_tokens_available() const noexcept;
	std::uint64_t fractional_credit_units() const noexcept;
	bool clock_regressed() const noexcept
	{
		return m_clock_regressed;
	}

  private:
	friend class ProtocolRateLimiter;

	// Advances time and verifies credit without consuming it. This is the
	// transaction primitive used when one received datagram must debit several
	// buckets atomically.
	TokenBucketResult prepare_consume(std::uint64_t now_us, std::uint32_t token_cost) noexcept;
	void commit_prepared(std::uint32_t token_cost) noexcept;
	bool refill(std::uint64_t now_us) noexcept;
	std::uint64_t capacity_units() const noexcept;

	RateLimitProfile m_profile;
	std::uint64_t m_credit_units = 0;
	std::uint64_t m_last_refill_time_us = 0;
	bool m_clock_regressed = false;
};

// Fixed implementation ceilings. Configuration may lower them, but peer
// input can never grow a registry or allocate a bucket dynamically.
constexpr std::size_t RateLimitMaximumPreSessionSources = 256;
constexpr std::size_t RateLimitMaximumSessions = 64;
constexpr std::size_t RateLimitMaximumTargets = 4096;
constexpr std::uint64_t RateLimitDefaultIdleExpiryUs = 10'000'000ULL;

struct ProtocolRateLimitProfiles {
	RateLimitProfile hello = HelloRateLimit;
	RateLimitProfile heartbeat_request = HeartbeatRequestRateLimit;
	RateLimitProfile resync_request = ResyncRequestRateLimit;
	RateLimitProfile capability_update = CapabilityUpdateRateLimit;
	RateLimitProfile ack_nack = AckNackRateLimit;
	RateLimitProfile unsupported_message_nack = UnsupportedMessageNackRateLimit;
	RateLimitProfile session_creation = SessionCreationRateLimit;
	// ACK/NACK also consume a bucket scoped to their exact logical target.
	RateLimitProfile target_ack_nack = AckNackRateLimit;
};

struct ProtocolRateLimiterLimits {
	std::size_t max_pre_session_sources = RateLimitMaximumPreSessionSources;
	std::size_t max_sessions = RateLimitMaximumSessions;
	std::size_t max_targets = RateLimitMaximumTargets;
	std::uint64_t idle_expiry_us = RateLimitDefaultIdleExpiryUs;
};

struct ProtocolRateLimiterConfig {
	ProtocolRateLimitProfiles profiles;
	ProtocolRateLimiterLimits limits;
};

// message_type is the raw one-byte registry value so an unknown type can be
// retained in an UnsupportedMessage target without pretending it is known.
struct RateLimitTargetKey {
	std::uint8_t message_type = 0;
	std::uint32_t message_id = 0;
	std::uint32_t message_crc32 = 0;

	bool is_valid() const noexcept
	{
		return message_type != 0 && message_id != 0;
	}

	friend bool operator==(const RateLimitTargetKey& lhs, const RateLimitTargetKey& rhs) noexcept
	{
		return lhs.message_type == rhs.message_type && lhs.message_id == rhs.message_id &&
			   lhs.message_crc32 == rhs.message_crc32;
	}
	friend bool operator!=(const RateLimitTargetKey& lhs, const RateLimitTargetKey& rhs) noexcept
	{
		return !(lhs == rhs);
	}
};

enum class ProtocolRateLimitResult : std::uint8_t {
	Allowed,
	RateLimited,
	ResourceLimit,
	InvalidIdentity,
	InvalidClass,
	ClockRegressed,
};

struct ProtocolRateLimiterCounters {
	std::uint64_t allowed = 0;
	std::uint64_t rate_limited = 0;
	std::uint64_t resource_limited = 0;
	std::uint64_t invalid_identity = 0;
	std::uint64_t invalid_class = 0;
	std::uint64_t clock_regressed = 0;
};

// A fixed-capacity registry for all Phase 0 receive-side rate limits. Source
// address keys deliberately ignore UDP ports before session establishment;
// session keys require the exact canonical endpoint. UnsupportedMessage uses
// one atomic transaction across the common ACK/NACK, UnsupportedMessage and
// exact-target buckets.
class ProtocolRateLimiter final {
  public:
	ProtocolRateLimiter() noexcept = default;
	ProtocolRateLimiter(const ProtocolRateLimiter&) = delete;
	ProtocolRateLimiter& operator=(const ProtocolRateLimiter&) = delete;

	// On validation failure, limiter is unchanged. A successful configuration
	// clears all entries and counters and establishes the earliest accepted
	// monotonic timestamp.
	static ValidationError configure(const ProtocolRateLimiterConfig& config,
		std::uint64_t initial_time_us,
		ProtocolRateLimiter& limiter) noexcept;

	ProtocolRateLimitResult
	consume_pre_session(RateLimitClass limit_class, const EndpointKey& source_endpoint, std::uint64_t now_us) noexcept;

	ProtocolRateLimitResult consume_session(RateLimitClass limit_class,
		std::uint64_t session_id,
		const EndpointKey& endpoint,
		std::uint64_t now_us) noexcept;

	ProtocolRateLimitResult consume_ack_nack(std::uint64_t session_id,
		const EndpointKey& endpoint,
		const RateLimitTargetKey& target,
		std::uint64_t now_us) noexcept;

	ProtocolRateLimitResult consume_unsupported_message_nack(std::uint64_t session_id,
		const EndpointKey& endpoint,
		const RateLimitTargetKey& target,
		std::uint64_t now_us) noexcept;

	// Ordinary entries expire exactly at idle_expiry_us. UnsupportedMessage
	// tombstones are session-lifetime and survive idle expiry so the same tuple
	// cannot be answered twice; purge_session() or reset() reclaims them. A
	// regressed timestamp never purges newer state. Explicit purges use the same
	// identity semantics as consumption.
	std::size_t purge_expired(std::uint64_t now_us) noexcept;
	std::size_t purge_source(const EndpointKey& source_endpoint) noexcept;
	std::size_t purge_session(std::uint64_t session_id, const EndpointKey& endpoint) noexcept;
	void reset(std::uint64_t now_us) noexcept;

	const ProtocolRateLimiterConfig& config() const noexcept
	{
		return m_config;
	}
	const ProtocolRateLimiterCounters& counters() const noexcept
	{
		return m_counters;
	}
	std::size_t pre_session_entry_count() const noexcept
	{
		return m_pre_session_entry_count;
	}
	std::size_t session_entry_count() const noexcept
	{
		return m_session_entry_count;
	}
	std::size_t target_entry_count() const noexcept
	{
		return m_target_entry_count;
	}

  private:
	struct SourceAddressKey {
		IpAddressFamily family = IpAddressFamily::Invalid;
		std::array<std::uint8_t, 16> address{};

		friend bool operator==(const SourceAddressKey& lhs, const SourceAddressKey& rhs) noexcept
		{
			return lhs.family == rhs.family && lhs.address == rhs.address;
		}
	};

	struct PreSessionEntry {
		bool active = false;
		SourceAddressKey source;
		TokenBucket hello;
		TokenBucket session_creation;
		std::uint64_t last_seen_us = 0;
	};

	struct SessionEntry {
		bool active = false;
		std::uint64_t session_id = 0;
		EndpointKey endpoint;
		TokenBucket heartbeat_request;
		TokenBucket resync_request;
		TokenBucket capability_update;
		TokenBucket ack_nack;
		TokenBucket unsupported_message_nack;
		std::uint64_t last_seen_us = 0;
	};

	struct TargetEntry {
		bool active = false;
		// UnsupportedMessage is emitted at most once for an exact logical target
		// during a session. This tombstone deliberately survives idle expiry and
		// is reclaimed by purge_session() or reset().
		bool unsupported_message_sent = false;
		std::uint64_t session_id = 0;
		EndpointKey endpoint;
		RateLimitTargetKey target;
		TokenBucket ack_nack;
		std::uint64_t last_seen_us = 0;
	};

	bool time_is_accepted(std::uint64_t now_us) noexcept;
	bool is_expired(std::uint64_t last_seen_us, std::uint64_t now_us) const noexcept;
	ProtocolRateLimitResult record(ProtocolRateLimitResult result) noexcept;

	std::size_t find_pre_session(const SourceAddressKey& source) const noexcept;
	std::size_t find_session(std::uint64_t session_id, const EndpointKey& endpoint) const noexcept;
	std::size_t
	find_target(std::uint64_t session_id, const EndpointKey& endpoint, const RateLimitTargetKey& target) const noexcept;
	std::size_t free_pre_session_slot() const noexcept;
	std::size_t free_session_slot() const noexcept;
	std::size_t free_target_slot() const noexcept;

	PreSessionEntry make_pre_session_entry(const SourceAddressKey& source, std::uint64_t now_us) const noexcept;
	SessionEntry
	make_session_entry(std::uint64_t session_id, const EndpointKey& endpoint, std::uint64_t now_us) const noexcept;
	TargetEntry make_target_entry(std::uint64_t session_id,
		const EndpointKey& endpoint,
		const RateLimitTargetKey& target,
		std::uint64_t now_us) const noexcept;

	ProtocolRateLimiterConfig m_config{};
	std::array<PreSessionEntry, RateLimitMaximumPreSessionSources> m_pre_session_entries{};
	std::array<SessionEntry, RateLimitMaximumSessions> m_session_entries{};
	std::array<TargetEntry, RateLimitMaximumTargets> m_target_entries{};
	std::size_t m_pre_session_entry_count = 0;
	std::size_t m_session_entry_count = 0;
	std::size_t m_target_entry_count = 0;
	// Global registry epoch, not merely the reset timestamp. Without this
	// watermark a regressed clock could obtain fresh full buckets by switching
	// to a previously unseen source/session/target identity.
	std::uint64_t m_last_time_us = 0;
	bool m_clock_regressed = false;
	ProtocolRateLimiterCounters m_counters{};
};

} // namespace telemetry::protocol
