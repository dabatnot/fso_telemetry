#include "telemetry/protocol/telemetry_rate_limiter.h"

#include <limits>

namespace telemetry::protocol {

namespace {

constexpr std::size_t NoEntry = std::numeric_limits<std::size_t>::max();

void increment_saturated(std::uint64_t& value) noexcept
{
	if (value != std::numeric_limits<std::uint64_t>::max()) {
		++value;
	}
}

bool profiles_are_valid(const ProtocolRateLimitProfiles& profiles) noexcept
{
	return rate_limit_profile_is_at_or_below(profiles.hello, HelloRateLimit) &&
		   rate_limit_profile_is_at_or_below(profiles.heartbeat_request, HeartbeatRequestRateLimit) &&
		   rate_limit_profile_is_at_or_below(profiles.resync_request, ResyncRequestRateLimit) &&
		   rate_limit_profile_is_at_or_below(profiles.capability_update, CapabilityUpdateRateLimit) &&
		   rate_limit_profile_is_at_or_below(profiles.ack_nack, AckNackRateLimit) &&
		   rate_limit_profile_is_at_or_below(profiles.unsupported_message_nack, UnsupportedMessageNackRateLimit) &&
		   rate_limit_profile_is_at_or_below(profiles.session_creation, SessionCreationRateLimit) &&
		   rate_limit_profile_is_at_or_below(profiles.target_ack_nack, AckNackRateLimit);
}

ProtocolRateLimitResult protocol_result(TokenBucketResult result) noexcept
{
	switch (result) {
	case TokenBucketResult::Allowed:
		return ProtocolRateLimitResult::Allowed;
	case TokenBucketResult::RateLimited:
		return ProtocolRateLimitResult::RateLimited;
	case TokenBucketResult::ClockRegressed:
		return ProtocolRateLimitResult::ClockRegressed;
	case TokenBucketResult::InvalidCost:
	default:
		return ProtocolRateLimitResult::InvalidClass;
	}
}

ProtocolRateLimitResult combine(TokenBucketResult first, TokenBucketResult second) noexcept
{
	if (first == TokenBucketResult::ClockRegressed || second == TokenBucketResult::ClockRegressed) {
		return ProtocolRateLimitResult::ClockRegressed;
	}
	if (first == TokenBucketResult::InvalidCost || second == TokenBucketResult::InvalidCost) {
		return ProtocolRateLimitResult::InvalidClass;
	}
	if (first == TokenBucketResult::RateLimited || second == TokenBucketResult::RateLimited) {
		return ProtocolRateLimitResult::RateLimited;
	}
	return ProtocolRateLimitResult::Allowed;
}

ProtocolRateLimitResult combine(TokenBucketResult first, TokenBucketResult second, TokenBucketResult third) noexcept
{
	const auto first_two = combine(first, second);
	if (first_two == ProtocolRateLimitResult::ClockRegressed || third == TokenBucketResult::ClockRegressed) {
		return ProtocolRateLimitResult::ClockRegressed;
	}
	if (first_two == ProtocolRateLimitResult::InvalidClass || third == TokenBucketResult::InvalidCost) {
		return ProtocolRateLimitResult::InvalidClass;
	}
	if (first_two == ProtocolRateLimitResult::RateLimited || third == TokenBucketResult::RateLimited) {
		return ProtocolRateLimitResult::RateLimited;
	}
	return ProtocolRateLimitResult::Allowed;
}

} // namespace

TokenBucket::TokenBucket(RateLimitProfile profile, std::uint64_t initial_time_us) noexcept
	: m_profile(profile), m_last_refill_time_us(initial_time_us)
{
	m_credit_units = capacity_units();
}

TokenBucketResult TokenBucket::try_consume(std::uint64_t now_us, std::uint32_t token_cost) noexcept
{
	const auto result = prepare_consume(now_us, token_cost);
	if (result == TokenBucketResult::Allowed) {
		commit_prepared(token_cost);
	}
	return result;
}

TokenBucketResult TokenBucket::prepare_consume(std::uint64_t now_us, std::uint32_t token_cost) noexcept
{
	if (token_cost == 0) {
		return TokenBucketResult::InvalidCost;
	}
	if (!refill(now_us)) {
		return TokenBucketResult::ClockRegressed;
	}

	const auto cost_units = static_cast<std::uint64_t>(token_cost) * RateLimitTimeUnitsPerSecond;
	if (cost_units > m_credit_units) {
		return TokenBucketResult::RateLimited;
	}
	return TokenBucketResult::Allowed;
}

void TokenBucket::commit_prepared(std::uint32_t token_cost) noexcept
{
	const auto cost_units = static_cast<std::uint64_t>(token_cost) * RateLimitTimeUnitsPerSecond;
	// The method is private and is called only after prepare_consume succeeds.
	m_credit_units -= cost_units;
}

void TokenBucket::reset(std::uint64_t now_us) noexcept
{
	m_last_refill_time_us = now_us;
	m_credit_units = capacity_units();
	m_clock_regressed = false;
}

std::uint32_t TokenBucket::whole_tokens_available() const noexcept
{
	return static_cast<std::uint32_t>(m_credit_units / RateLimitTimeUnitsPerSecond);
}

std::uint64_t TokenBucket::fractional_credit_units() const noexcept
{
	return m_credit_units % RateLimitTimeUnitsPerSecond;
}

bool TokenBucket::refill(std::uint64_t now_us) noexcept
{
	if (m_clock_regressed) {
		return false;
	}
	if (now_us < m_last_refill_time_us) {
		m_clock_regressed = true;
		return false;
	}

	const auto elapsed_us = now_us - m_last_refill_time_us;
	m_last_refill_time_us = now_us;
	const auto capacity = capacity_units();
	if (m_credit_units >= capacity) {
		m_credit_units = capacity;
		return true;
	}
	if (elapsed_us == 0 || m_profile.tokens_per_second == 0) {
		return true;
	}

	const auto missing_units = capacity - m_credit_units;
	const auto rate = static_cast<std::uint64_t>(m_profile.tokens_per_second);
	const auto whole_us_to_full = missing_units / rate;
	const auto needs_partial_us = (missing_units % rate) != 0;
	const auto us_to_full = whole_us_to_full + static_cast<std::uint64_t>(needs_partial_us);
	if (elapsed_us >= us_to_full) {
		m_credit_units = capacity;
	} else {
		// elapsed_us < ceil(missing_units / rate), therefore this product is
		// strictly below missing_units and cannot overflow uint64_t.
		m_credit_units += elapsed_us * rate;
	}
	return true;
}

std::uint64_t TokenBucket::capacity_units() const noexcept
{
	// burst_tokens is u32, so this multiplication is bounded by about 2^52.
	return static_cast<std::uint64_t>(m_profile.burst_tokens) * RateLimitTimeUnitsPerSecond;
}

ValidationError ProtocolRateLimiter::configure(const ProtocolRateLimiterConfig& config,
	std::uint64_t initial_time_us,
	ProtocolRateLimiter& limiter) noexcept
{
	if (!profiles_are_valid(config.profiles) || config.limits.max_pre_session_sources == 0 ||
		config.limits.max_pre_session_sources > RateLimitMaximumPreSessionSources || config.limits.max_sessions == 0 ||
		config.limits.max_sessions > RateLimitMaximumSessions || config.limits.max_targets == 0 ||
		config.limits.max_targets > RateLimitMaximumTargets || config.limits.idle_expiry_us == 0) {
		return ValidationError::OutOfRange;
	}

	limiter.m_config = config;
	limiter.reset(initial_time_us);
	return ValidationError::None;
}

ProtocolRateLimitResult ProtocolRateLimiter::consume_pre_session(RateLimitClass limit_class,
	const EndpointKey& source_endpoint,
	std::uint64_t now_us) noexcept
{
	if (!source_endpoint.is_valid()) {
		return record(ProtocolRateLimitResult::InvalidIdentity);
	}
	if (limit_class != RateLimitClass::Hello && limit_class != RateLimitClass::SessionCreation) {
		return record(ProtocolRateLimitResult::InvalidClass);
	}
	if (!time_is_accepted(now_us)) {
		return record(ProtocolRateLimitResult::ClockRegressed);
	}
	purge_expired(now_us);

	SourceAddressKey source;
	source.family = source_endpoint.family();
	source.address = source_endpoint.address();
	const auto existing_index = find_pre_session(source);
	const auto index = existing_index != NoEntry ? existing_index : free_pre_session_slot();
	if (index == NoEntry) {
		return record(ProtocolRateLimitResult::ResourceLimit);
	}

	auto candidate = existing_index != NoEntry ? m_pre_session_entries[index] : make_pre_session_entry(source, now_us);
	auto& bucket = limit_class == RateLimitClass::Hello ? candidate.hello : candidate.session_creation;
	const auto bucket_result = bucket.prepare_consume(now_us, 1U);
	const auto result = protocol_result(bucket_result);
	if (result == ProtocolRateLimitResult::Allowed) {
		bucket.commit_prepared(1U);
		candidate.last_seen_us = now_us;
		m_pre_session_entries[index] = candidate;
		if (existing_index == NoEntry) {
			++m_pre_session_entry_count;
		}
	} else if (existing_index != NoEntry) {
		if (result != ProtocolRateLimitResult::ClockRegressed) {
			candidate.last_seen_us = now_us;
		}
		m_pre_session_entries[index] = candidate;
	}
	return record(result);
}

ProtocolRateLimitResult ProtocolRateLimiter::consume_session(RateLimitClass limit_class,
	std::uint64_t session_id,
	const EndpointKey& endpoint,
	std::uint64_t now_us) noexcept
{
	if (session_id == 0 || !endpoint.is_valid()) {
		return record(ProtocolRateLimitResult::InvalidIdentity);
	}
	if (limit_class != RateLimitClass::HeartbeatRequest && limit_class != RateLimitClass::ResyncRequest &&
		limit_class != RateLimitClass::CapabilityUpdate) {
		return record(ProtocolRateLimitResult::InvalidClass);
	}
	if (!time_is_accepted(now_us)) {
		return record(ProtocolRateLimitResult::ClockRegressed);
	}
	purge_expired(now_us);

	const auto existing_index = find_session(session_id, endpoint);
	const auto index = existing_index != NoEntry ? existing_index : free_session_slot();
	if (index == NoEntry) {
		return record(ProtocolRateLimitResult::ResourceLimit);
	}

	auto candidate =
		existing_index != NoEntry ? m_session_entries[index] : make_session_entry(session_id, endpoint, now_us);
	TokenBucket* bucket = nullptr;
	switch (limit_class) {
	case RateLimitClass::HeartbeatRequest:
		bucket = &candidate.heartbeat_request;
		break;
	case RateLimitClass::ResyncRequest:
		bucket = &candidate.resync_request;
		break;
	case RateLimitClass::CapabilityUpdate:
		bucket = &candidate.capability_update;
		break;
	default:
		return record(ProtocolRateLimitResult::InvalidClass);
	}

	const auto bucket_result = bucket->prepare_consume(now_us, 1U);
	const auto result = protocol_result(bucket_result);
	if (result == ProtocolRateLimitResult::Allowed) {
		bucket->commit_prepared(1U);
		candidate.last_seen_us = now_us;
		m_session_entries[index] = candidate;
		if (existing_index == NoEntry) {
			++m_session_entry_count;
		}
	} else if (existing_index != NoEntry) {
		if (result != ProtocolRateLimitResult::ClockRegressed) {
			candidate.last_seen_us = now_us;
		}
		m_session_entries[index] = candidate;
	}
	return record(result);
}

ProtocolRateLimitResult ProtocolRateLimiter::consume_ack_nack(std::uint64_t session_id,
	const EndpointKey& endpoint,
	const RateLimitTargetKey& target,
	std::uint64_t now_us) noexcept
{
	if (session_id == 0 || !endpoint.is_valid() || !target.is_valid()) {
		return record(ProtocolRateLimitResult::InvalidIdentity);
	}
	if (target.message_type >= FirstReservedMessageType) {
		return record(ProtocolRateLimitResult::InvalidClass);
	}
	if (!time_is_accepted(now_us)) {
		return record(ProtocolRateLimitResult::ClockRegressed);
	}
	purge_expired(now_us);

	const auto existing_target = find_target(session_id, endpoint, target);
	const auto existing_session = find_session(session_id, endpoint);
	const auto session_index = existing_session != NoEntry ? existing_session : free_session_slot();
	if (session_index == NoEntry) {
		return record(ProtocolRateLimitResult::ResourceLimit);
	}
	const auto target_index = existing_target != NoEntry ? existing_target : free_target_slot();
	if (target_index == NoEntry) {
		return record(ProtocolRateLimitResult::ResourceLimit);
	}

	auto session_candidate = existing_session != NoEntry ? m_session_entries[session_index]
														 : make_session_entry(session_id, endpoint, now_us);
	auto target_candidate = existing_target != NoEntry ? m_target_entries[target_index]
													   : make_target_entry(session_id, endpoint, target, now_us);
	const auto common_result = session_candidate.ack_nack.prepare_consume(now_us, 1U);
	const auto target_result = target_candidate.ack_nack.prepare_consume(now_us, 1U);
	const auto result = combine(common_result, target_result);
	if (result == ProtocolRateLimitResult::Allowed) {
		session_candidate.ack_nack.commit_prepared(1U);
		target_candidate.ack_nack.commit_prepared(1U);
		session_candidate.last_seen_us = now_us;
		target_candidate.last_seen_us = now_us;
		m_session_entries[session_index] = session_candidate;
		m_target_entries[target_index] = target_candidate;
		if (existing_session == NoEntry) {
			++m_session_entry_count;
		}
		if (existing_target == NoEntry) {
			++m_target_entry_count;
		}
	} else {
		if (existing_session != NoEntry) {
			if (result != ProtocolRateLimitResult::ClockRegressed) {
				session_candidate.last_seen_us = now_us;
			}
			m_session_entries[session_index] = session_candidate;
		}
		if (existing_target != NoEntry) {
			if (result != ProtocolRateLimitResult::ClockRegressed) {
				target_candidate.last_seen_us = now_us;
			}
			m_target_entries[target_index] = target_candidate;
		}
	}
	return record(result);
}

ProtocolRateLimitResult ProtocolRateLimiter::consume_unsupported_message_nack(std::uint64_t session_id,
	const EndpointKey& endpoint,
	const RateLimitTargetKey& target,
	std::uint64_t now_us) noexcept
{
	if (session_id == 0 || !endpoint.is_valid() || !target.is_valid()) {
		return record(ProtocolRateLimitResult::InvalidIdentity);
	}
	if (target.message_type < FirstReservedMessageType) {
		return record(ProtocolRateLimitResult::InvalidClass);
	}
	if (!time_is_accepted(now_us)) {
		return record(ProtocolRateLimitResult::ClockRegressed);
	}
	purge_expired(now_us);

	const auto existing_target = find_target(session_id, endpoint, target);
	if (existing_target != NoEntry && m_target_entries[existing_target].unsupported_message_sent) {
		return record(ProtocolRateLimitResult::RateLimited);
	}
	const auto existing_session = find_session(session_id, endpoint);
	const auto session_index = existing_session != NoEntry ? existing_session : free_session_slot();
	if (session_index == NoEntry) {
		return record(ProtocolRateLimitResult::ResourceLimit);
	}
	const auto target_index = existing_target != NoEntry ? existing_target : free_target_slot();
	if (target_index == NoEntry) {
		return record(ProtocolRateLimitResult::ResourceLimit);
	}

	auto session_candidate = existing_session != NoEntry ? m_session_entries[session_index]
														 : make_session_entry(session_id, endpoint, now_us);
	auto target_candidate = existing_target != NoEntry ? m_target_entries[target_index]
													   : make_target_entry(session_id, endpoint, target, now_us);
	const auto common_result = session_candidate.ack_nack.prepare_consume(now_us, 1U);
	const auto unsupported_result = session_candidate.unsupported_message_nack.prepare_consume(now_us, 1U);
	const auto target_result = target_candidate.ack_nack.prepare_consume(now_us, 1U);
	const auto result = combine(common_result, unsupported_result, target_result);
	if (result == ProtocolRateLimitResult::Allowed) {
		session_candidate.ack_nack.commit_prepared(1U);
		session_candidate.unsupported_message_nack.commit_prepared(1U);
		target_candidate.ack_nack.commit_prepared(1U);
		target_candidate.unsupported_message_sent = true;
		session_candidate.last_seen_us = now_us;
		target_candidate.last_seen_us = now_us;
		m_session_entries[session_index] = session_candidate;
		m_target_entries[target_index] = target_candidate;
		if (existing_session == NoEntry) {
			++m_session_entry_count;
		}
		if (existing_target == NoEntry) {
			++m_target_entry_count;
		}
	} else {
		if (existing_session != NoEntry) {
			if (result != ProtocolRateLimitResult::ClockRegressed) {
				session_candidate.last_seen_us = now_us;
			}
			m_session_entries[session_index] = session_candidate;
		}
		if (existing_target != NoEntry) {
			if (result != ProtocolRateLimitResult::ClockRegressed) {
				target_candidate.last_seen_us = now_us;
			}
			m_target_entries[target_index] = target_candidate;
		}
	}
	return record(result);
}

std::size_t ProtocolRateLimiter::purge_expired(std::uint64_t now_us) noexcept
{
	if (!time_is_accepted(now_us)) {
		return 0;
	}
	std::size_t purged = 0;
	for (std::size_t index = 0; index < m_config.limits.max_pre_session_sources; ++index) {
		auto& entry = m_pre_session_entries[index];
		if (entry.active && is_expired(entry.last_seen_us, now_us)) {
			entry = PreSessionEntry{};
			--m_pre_session_entry_count;
			++purged;
		}
	}
	for (std::size_t index = 0; index < m_config.limits.max_sessions; ++index) {
		auto& entry = m_session_entries[index];
		if (entry.active && is_expired(entry.last_seen_us, now_us)) {
			entry = SessionEntry{};
			--m_session_entry_count;
			++purged;
		}
	}
	for (std::size_t index = 0; index < m_config.limits.max_targets; ++index) {
		auto& entry = m_target_entries[index];
		if (entry.active && !entry.unsupported_message_sent && is_expired(entry.last_seen_us, now_us)) {
			entry = TargetEntry{};
			--m_target_entry_count;
			++purged;
		}
	}
	return purged;
}

std::size_t ProtocolRateLimiter::purge_source(const EndpointKey& source_endpoint) noexcept
{
	if (!source_endpoint.is_valid()) {
		return 0;
	}
	SourceAddressKey source;
	source.family = source_endpoint.family();
	source.address = source_endpoint.address();
	const auto index = find_pre_session(source);
	if (index == NoEntry) {
		return 0;
	}
	m_pre_session_entries[index] = PreSessionEntry{};
	--m_pre_session_entry_count;
	return 1;
}

std::size_t ProtocolRateLimiter::purge_session(std::uint64_t session_id, const EndpointKey& endpoint) noexcept
{
	if (session_id == 0 || !endpoint.is_valid()) {
		return 0;
	}
	std::size_t purged = 0;
	const auto session_index = find_session(session_id, endpoint);
	if (session_index != NoEntry) {
		m_session_entries[session_index] = SessionEntry{};
		--m_session_entry_count;
		++purged;
	}
	for (std::size_t index = 0; index < m_config.limits.max_targets; ++index) {
		auto& entry = m_target_entries[index];
		if (entry.active && entry.session_id == session_id && entry.endpoint == endpoint) {
			entry = TargetEntry{};
			--m_target_entry_count;
			++purged;
		}
	}
	return purged;
}

void ProtocolRateLimiter::reset(std::uint64_t now_us) noexcept
{
	for (auto& entry : m_pre_session_entries) {
		entry = PreSessionEntry{};
	}
	for (auto& entry : m_session_entries) {
		entry = SessionEntry{};
	}
	for (auto& entry : m_target_entries) {
		entry = TargetEntry{};
	}
	m_pre_session_entry_count = 0;
	m_session_entry_count = 0;
	m_target_entry_count = 0;
	m_last_time_us = now_us;
	m_clock_regressed = false;
	m_counters = ProtocolRateLimiterCounters{};
}

bool ProtocolRateLimiter::time_is_accepted(std::uint64_t now_us) noexcept
{
	if (m_clock_regressed) {
		return false;
	}
	if (now_us < m_last_time_us) {
		m_clock_regressed = true;
		return false;
	}
	m_last_time_us = now_us;
	return true;
}

bool ProtocolRateLimiter::is_expired(std::uint64_t last_seen_us, std::uint64_t now_us) const noexcept
{
	return now_us >= last_seen_us && now_us - last_seen_us >= m_config.limits.idle_expiry_us;
}

ProtocolRateLimitResult ProtocolRateLimiter::record(ProtocolRateLimitResult result) noexcept
{
	switch (result) {
	case ProtocolRateLimitResult::Allowed:
		increment_saturated(m_counters.allowed);
		break;
	case ProtocolRateLimitResult::RateLimited:
		increment_saturated(m_counters.rate_limited);
		break;
	case ProtocolRateLimitResult::ResourceLimit:
		increment_saturated(m_counters.resource_limited);
		break;
	case ProtocolRateLimitResult::InvalidIdentity:
		increment_saturated(m_counters.invalid_identity);
		break;
	case ProtocolRateLimitResult::InvalidClass:
		increment_saturated(m_counters.invalid_class);
		break;
	case ProtocolRateLimitResult::ClockRegressed:
		increment_saturated(m_counters.clock_regressed);
		break;
	}
	return result;
}

std::size_t ProtocolRateLimiter::find_pre_session(const SourceAddressKey& source) const noexcept
{
	for (std::size_t index = 0; index < m_config.limits.max_pre_session_sources; ++index) {
		const auto& entry = m_pre_session_entries[index];
		if (entry.active && entry.source == source) {
			return index;
		}
	}
	return NoEntry;
}

std::size_t ProtocolRateLimiter::find_session(std::uint64_t session_id, const EndpointKey& endpoint) const noexcept
{
	for (std::size_t index = 0; index < m_config.limits.max_sessions; ++index) {
		const auto& entry = m_session_entries[index];
		if (entry.active && entry.session_id == session_id && entry.endpoint == endpoint) {
			return index;
		}
	}
	return NoEntry;
}

std::size_t ProtocolRateLimiter::find_target(std::uint64_t session_id,
	const EndpointKey& endpoint,
	const RateLimitTargetKey& target) const noexcept
{
	for (std::size_t index = 0; index < m_config.limits.max_targets; ++index) {
		const auto& entry = m_target_entries[index];
		if (entry.active && entry.session_id == session_id && entry.endpoint == endpoint && entry.target == target) {
			return index;
		}
	}
	return NoEntry;
}

std::size_t ProtocolRateLimiter::free_pre_session_slot() const noexcept
{
	for (std::size_t index = 0; index < m_config.limits.max_pre_session_sources; ++index) {
		if (!m_pre_session_entries[index].active) {
			return index;
		}
	}
	return NoEntry;
}

std::size_t ProtocolRateLimiter::free_session_slot() const noexcept
{
	for (std::size_t index = 0; index < m_config.limits.max_sessions; ++index) {
		if (!m_session_entries[index].active) {
			return index;
		}
	}
	return NoEntry;
}

std::size_t ProtocolRateLimiter::free_target_slot() const noexcept
{
	for (std::size_t index = 0; index < m_config.limits.max_targets; ++index) {
		if (!m_target_entries[index].active) {
			return index;
		}
	}
	return NoEntry;
}

ProtocolRateLimiter::PreSessionEntry ProtocolRateLimiter::make_pre_session_entry(const SourceAddressKey& source,
	std::uint64_t now_us) const noexcept
{
	PreSessionEntry entry;
	entry.active = true;
	entry.source = source;
	entry.hello = TokenBucket(m_config.profiles.hello, now_us);
	entry.session_creation = TokenBucket(m_config.profiles.session_creation, now_us);
	entry.last_seen_us = now_us;
	return entry;
}

ProtocolRateLimiter::SessionEntry ProtocolRateLimiter::make_session_entry(std::uint64_t session_id,
	const EndpointKey& endpoint,
	std::uint64_t now_us) const noexcept
{
	SessionEntry entry;
	entry.active = true;
	entry.session_id = session_id;
	entry.endpoint = endpoint;
	entry.heartbeat_request = TokenBucket(m_config.profiles.heartbeat_request, now_us);
	entry.resync_request = TokenBucket(m_config.profiles.resync_request, now_us);
	entry.capability_update = TokenBucket(m_config.profiles.capability_update, now_us);
	entry.ack_nack = TokenBucket(m_config.profiles.ack_nack, now_us);
	entry.unsupported_message_nack = TokenBucket(m_config.profiles.unsupported_message_nack, now_us);
	entry.last_seen_us = now_us;
	return entry;
}

ProtocolRateLimiter::TargetEntry ProtocolRateLimiter::make_target_entry(std::uint64_t session_id,
	const EndpointKey& endpoint,
	const RateLimitTargetKey& target,
	std::uint64_t now_us) const noexcept
{
	TargetEntry entry;
	entry.active = true;
	entry.session_id = session_id;
	entry.endpoint = endpoint;
	entry.target = target;
	entry.ack_nack = TokenBucket(m_config.profiles.target_ack_nack, now_us);
	entry.last_seen_us = now_us;
	return entry;
}

} // namespace telemetry::protocol
