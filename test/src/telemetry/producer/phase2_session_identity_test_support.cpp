#include "telemetry/identity.h"

#include <new>

namespace telemetry::detail {

SessionIdCandidateResult draw_session_id_candidate(
	RandomSource& random) noexcept
{
	std::uint64_t candidate = 0U;
	if (!random.next_u64(candidate) || candidate == 0U)
		return {};
	return {SessionIdCandidateStatus::Ready, candidate};
}

bool SessionIdRegistry::allocate_storage() noexcept
{
	if (storage_ready())
		return true;
	auto storage =
		std::unique_ptr<Storage>(new (std::nothrow) Storage{});
	if (!storage)
		return false;
	m_used_ids = std::move(storage);
	m_used_count = 0U;
	return true;
}

void SessionIdRegistry::release_storage() noexcept
{
	m_used_ids.reset();
	m_used_count = 0U;
}

SessionIdRegistrationStatus
SessionIdRegistry::register_candidate(
	std::uint64_t candidate) noexcept
{
	if (candidate == 0U)
		return SessionIdRegistrationStatus::InvalidCandidate;
	if (!storage_ready())
		return SessionIdRegistrationStatus::StorageUnavailable;
	if (m_used_count >= MaximumSessionIdsPerProcess)
		return SessionIdRegistrationStatus::Capacity;
	for (const auto used : *m_used_ids)
		if (used == candidate)
			return SessionIdRegistrationStatus::Duplicate;
	for (auto& slot : *m_used_ids)
		if (slot == 0U) {
			slot = candidate;
			++m_used_count;
			return SessionIdRegistrationStatus::Registered;
		}
	return SessionIdRegistrationStatus::Capacity;
}

SessionIdAllocator::SessionIdAllocator(
	RandomSource& random, SessionIdRegistry& registry) noexcept :
	m_random(random), m_registry(registry)
{
}

SessionIdResult SessionIdAllocator::allocate() noexcept
{
	if (!m_registry.storage_ready())
		return {SessionIdStatus::StorageUnavailable, 0U};
	for (std::size_t attempt = 0U;
		 attempt < MaximumSessionIdCollisionDraws; ++attempt) {
		const auto candidate =
			draw_session_id_candidate(m_random);
		if (candidate.status !=
			SessionIdCandidateStatus::Ready)
			return {SessionIdStatus::EntropyFailure, 0U};
		const auto registration =
			m_registry.register_candidate(candidate.session_id);
		if (registration ==
			SessionIdRegistrationStatus::Registered)
			return {SessionIdStatus::Allocated,
				candidate.session_id};
		if (registration !=
			SessionIdRegistrationStatus::Duplicate)
			return {registration ==
					SessionIdRegistrationStatus::Capacity
				? SessionIdStatus::CapacityExhausted
				: SessionIdStatus::StorageUnavailable,
				0U};
	}
	return {SessionIdStatus::RetryLimit, 0U};
}

} // namespace telemetry::detail
