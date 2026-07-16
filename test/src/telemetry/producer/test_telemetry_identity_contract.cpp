#include "telemetry/identity.h"
#include "telemetry/startup_budget.h"

#include <gtest/gtest.h>
#include <jansson.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;

constexpr std::size_t MaxProfileBytes = 1024U;
constexpr std::size_t MaxSessionIdsPerProcess = 65'536U;
constexpr std::size_t MaxSessionIdDraws = 16U;

template <typename Enum>
constexpr auto enum_value(Enum value) noexcept
{
	static_assert(std::is_enum_v<Enum>, "Identity diagnostics must remain a closed enum.");
	return static_cast<std::underlying_type_t<Enum>>(value);
}

enum class StoreCall {
	Read,
	CreateTemporary,
	WriteTemporary,
	FlushTemporary,
	CloseTemporary,
	InstallIfAbsent,
	AbortTemporary,
};

enum class StoreFailure {
	None,
	InitialRead,
	CreateTemporary,
	WriteTemporary,
	FlushTemporary,
	CloseTemporary,
	Install,
	FinalRead,
};

class ScriptedRandomSource final : public detail::RandomSource {
  public:
	struct Draw {
		bool success;
		std::uint64_t value;
	};

	explicit ScriptedRandomSource(std::vector<Draw> draws) : m_draws(std::move(draws)) {}

	bool next_u64(std::uint64_t& value) noexcept override
	{
		++calls;
		if (m_index >= m_draws.size() || !m_draws[m_index].success) {
			if (m_index < m_draws.size()) {
				++m_index;
			}
			return false;
		}

		value = m_draws[m_index].value;
		++m_index;
		return true;
	}

	std::size_t calls = 0U;

  private:
	std::vector<Draw> m_draws;
	std::size_t m_index = 0U;
};

class SequentialRandomSource final : public detail::RandomSource {
  public:
	bool next_u64(std::uint64_t& value) noexcept override
	{
		++calls;
		value = next_value++;
		return true;
	}

	std::uint64_t next_value = 1U;
	std::size_t calls = 0U;
};

class ScriptedProfileStore final : public detail::ProducerProfileStore {
  public:
	detail::ProfileReadStatus read(std::string& bytes) noexcept override
	{
		calls.push_back(StoreCall::Read);
		++read_count;

		if ((read_count == 1U && failure == StoreFailure::InitialRead) ||
			(read_count > 1U && failure == StoreFailure::FinalRead)) {
			return detail::ProfileReadStatus::IoError;
		}

		if (read_count == 1U) {
			if (initial_status == detail::ProfileReadStatus::Present) {
				bytes = initial_bytes;
			}
			return initial_status;
		}

		if (final_status != detail::ProfileReadStatus::Present) {
			return final_status;
		}

		if (!final_bytes.empty()) {
			bytes = final_bytes;
		} else if (install_status == detail::ProfileInstallStatus::DestinationExists) {
			bytes = destination_bytes;
		} else {
			bytes = installed_bytes;
		}
		return detail::ProfileReadStatus::Present;
	}

	bool create_temporary_in_profile_directory(detail::ProfileTempToken& token) noexcept override
	{
		calls.push_back(StoreCall::CreateTemporary);
		token = {};
		temporary_exists = failure != StoreFailure::CreateTemporary;
		return temporary_exists;
	}

	bool write_temporary(detail::ProfileTempToken, std::string_view bytes) noexcept override
	{
		calls.push_back(StoreCall::WriteTemporary);
		if (failure == StoreFailure::WriteTemporary) {
			return false;
		}
		written_bytes.assign(bytes.data(), bytes.size());
		return true;
	}

	bool flush_temporary_durably(detail::ProfileTempToken) noexcept override
	{
		calls.push_back(StoreCall::FlushTemporary);
		return failure != StoreFailure::FlushTemporary;
	}

	bool close_temporary(detail::ProfileTempToken) noexcept override
	{
		calls.push_back(StoreCall::CloseTemporary);
		return failure != StoreFailure::CloseTemporary;
	}

	detail::ProfileInstallStatus install_if_absent_atomically(detail::ProfileTempToken) noexcept override
	{
		calls.push_back(StoreCall::InstallIfAbsent);
		if (failure == StoreFailure::Install) {
			return detail::ProfileInstallStatus::Error;
		}

		if (install_status == detail::ProfileInstallStatus::Published) {
			installed_bytes = written_bytes;
			temporary_exists = false;
		}
		return install_status;
	}

	void abort_temporary(detail::ProfileTempToken) noexcept override
	{
		calls.push_back(StoreCall::AbortTemporary);
		temporary_exists = false;
	}

	detail::ProfileReadStatus initial_status = detail::ProfileReadStatus::Absent;
	std::string initial_bytes;
	detail::ProfileReadStatus final_status = detail::ProfileReadStatus::Present;
	std::string final_bytes;
	detail::ProfileInstallStatus install_status = detail::ProfileInstallStatus::Published;
	std::string destination_bytes;
	StoreFailure failure = StoreFailure::None;
	std::vector<StoreCall> calls;
	std::size_t read_count = 0U;
	bool temporary_exists = false;
	std::string written_bytes;
	std::string installed_bytes;
};

detail::IdentityResult parse_profile(std::string_view json) noexcept
{
	return detail::parse_producer_profile_json(json);
}

void expect_invalid_profile(std::string_view json)
{
	const auto result = parse_profile(json);
	EXPECT_EQ(0U, result.producer_id);
	EXPECT_NE(0, enum_value(result.error));
}

void expect_identity_failure(const detail::IdentityResult& result)
{
	EXPECT_EQ(0U, result.producer_id);
	EXPECT_NE(0, enum_value(result.error));
}

TEST(TelemetryProducerProfileContract, ClosedProfileSchemaAcceptsTheFullUint64RangeAsCanonicalDecimalStrings)
{
	const auto minimum = parse_profile(R"({"schemaVersion":1,"producerId":"1"})");
	EXPECT_EQ(1U, minimum.producer_id);
	EXPECT_EQ(0, enum_value(minimum.error));

	const auto maximum = parse_profile(
		R"({"schemaVersion":1,"producerId":"18446744073709551615"})");
	EXPECT_EQ(UINT64_MAX, maximum.producer_id);
	EXPECT_EQ(0, enum_value(maximum.error));
}

TEST(TelemetryProducerProfileContract, ProfileParserIsStrictBoundedAndDepthTwo)
{
	const std::vector<std::string> invalid_profiles{
		"",
		"[]",
		R"({})",
		R"({"schemaVersion":1})",
		R"({"producerId":"1"})",
		R"({"schemaVersion":1,"producerId":"1","unknown":0})",
		R"({"schemaVersion":1,"schemaVersion":1,"producerId":"1"})",
		R"({"schemaVersion":1,"producerId":"1","producerId":"2"})",
		R"({"schemaVersion":1,"producerId":"1"}null)",
		R"({"schemaVersion":1,"producerId":{"nested":"1"}})",
		R"({"schemaVersion":1,"producerId":{"nested":{"tooDeep":"1"}}})",
	};

	for (const auto& profile : invalid_profiles) {
		SCOPED_TRACE(profile);
		expect_invalid_profile(profile);
	}

	std::string exact_limit{R"({"schemaVersion":1,"producerId":"1"})"};
	exact_limit.append(MaxProfileBytes - exact_limit.size(), ' ');
	ASSERT_EQ(MaxProfileBytes, exact_limit.size());
	EXPECT_EQ(1U, parse_profile(exact_limit).producer_id);

	auto over_limit = exact_limit;
	over_limit.push_back(' ');
	ASSERT_EQ(MaxProfileBytes + 1U, over_limit.size());
	expect_invalid_profile(over_limit);
}

TEST(TelemetryProducerProfileContract, DepthPreflightRejectsTruncatedDepthThreeBeforeCallingJansson)
{
	const auto result = parse_profile(R"({"schemaVersion":1,"producerId":[[)");

	EXPECT_EQ(0U, result.producer_id);
	EXPECT_EQ(detail::IdentityError::MaximumDepthExceeded, result.error)
		<< "A truncated over-depth profile must be stopped before the recursive JSON parser.";
}

TEST(TelemetryProducerProfileContract, DepthPreflightIgnoresStructuralCharactersAndEscapesInsideStrings)
{
	const auto result = parse_profile(R"json({"schemaVersion":1,"producerId":"[[{{}}]]\"\\end"})json");

	EXPECT_EQ(0U, result.producer_id);
	EXPECT_EQ(detail::IdentityError::InvalidProducerId, result.error)
		<< "Containers, an escaped quote and an escaped backslash inside producerId are string bytes, not depth.";
}

// Opt-in process-isolation oracle for the same reason as the 16 KiB config
// bomb: the pre-fix vendored parser is recursive and must never see this input.
TEST(TelemetryProducerProfileDepthIsolationContract, DISABLED_DeepTruncatedContainerBombIsRejectedByPreflight)
{
	std::string bomb{R"({"schemaVersion":1,"producerId":)"};
	bomb.append(900U, '[');
	ASSERT_LT(bomb.size(), MaxProfileBytes);

	const auto result = parse_profile(bomb);
	EXPECT_EQ(0U, result.producer_id);
	EXPECT_EQ(detail::IdentityError::MaximumDepthExceeded, result.error);
}

TEST(TelemetryProducerProfileContract, ProducerIdRejectsNonStringsNonCanonicalDecimalZeroAndOverflow)
{
	const std::vector<std::string> invalid_values{
		"0",
		"1.0",
		R"("")",
		R"("0")",
		R"("00")",
		R"("01")",
		R"("+1")",
		R"("-1")",
		R"(" 1")",
		R"("1 ")",
		R"("1.0")",
		R"("1e3")",
		R"("18446744073709551616")",
		R"("999999999999999999999999999999999999")",
	};

	for (const auto& value : invalid_values) {
		SCOPED_TRACE(value);
		expect_invalid_profile(std::string{"{\"schemaVersion\":1,\"producerId\":"} + value + "}");
	}
}

TEST(TelemetryProducerIdentityContract, ExistingValidProfileAvoidsEntropyAndEveryWriteOperation)
{
	ScriptedProfileStore store;
	store.initial_status = detail::ProfileReadStatus::Present;
	store.initial_bytes = R"({"schemaVersion":1,"producerId":"1844674407370955161"})";
	ScriptedRandomSource random({{true, 9U}});

	const auto result = detail::load_or_create_producer_identity(store, random);

	EXPECT_EQ(1844674407370955161ULL, result.producer_id);
	EXPECT_EQ(0, enum_value(result.error));
	EXPECT_EQ(0U, random.calls);
	EXPECT_EQ(std::vector<StoreCall>{StoreCall::Read}, store.calls);
	EXPECT_TRUE(store.written_bytes.empty());
}

TEST(TelemetryProducerIdentityContract, ExistingInvalidProfileIsNeverOverwrittenOrRegenerated)
{
	const std::vector<std::string> invalid_existing{
		R"({"schemaVersion":1,"producerId":"0"})",
		R"({"schemaVersion":1,"producerId":"01"})",
		R"({"schemaVersion":1,"producerId":"18446744073709551616"})",
		R"({"schemaVersion":1,"producerId":"1","unknown":true})",
	};

	for (const auto& bytes : invalid_existing) {
		SCOPED_TRACE(bytes);
		ScriptedProfileStore store;
		store.initial_status = detail::ProfileReadStatus::Present;
		store.initial_bytes = bytes;
		ScriptedRandomSource random({{true, 9U}});

		const auto result = detail::load_or_create_producer_identity(store, random);

		expect_identity_failure(result);
		EXPECT_EQ(0U, random.calls);
		EXPECT_EQ(std::vector<StoreCall>{StoreCall::Read}, store.calls);
		EXPECT_TRUE(store.written_bytes.empty());
	}
}

TEST(TelemetryProducerIdentityContract, InitialProfileIoErrorFailsBeforeEntropyOrTemporaryCreation)
{
	ScriptedProfileStore store;
	store.failure = StoreFailure::InitialRead;
	ScriptedRandomSource random({{true, 9U}});

	const auto result = detail::load_or_create_producer_identity(store, random);

	expect_identity_failure(result);
	EXPECT_EQ(0U, random.calls);
	EXPECT_EQ(std::vector<StoreCall>{StoreCall::Read}, store.calls);
}

TEST(TelemetryProducerIdentityContract, EntropyFailureAndZeroProducerIdEachFailAfterExactlyOneDraw)
{
	for (const auto draw : std::vector<ScriptedRandomSource::Draw>{{false, 0U}, {true, 0U}}) {
		ScriptedProfileStore store;
		ScriptedRandomSource random({draw, {true, 7U}});

		const auto result = detail::load_or_create_producer_identity(store, random);

		expect_identity_failure(result);
		EXPECT_EQ(1U, random.calls);
		EXPECT_EQ(std::vector<StoreCall>{StoreCall::Read}, store.calls);
	}
}

TEST(TelemetryProducerIdentityContract, AbsentProfileUsesExclusiveSameDirectoryAtomicPublishAndRereads)
{
	ScriptedProfileStore store;
	ScriptedRandomSource random({{true, 1844674407370955161ULL}});

	const auto result = detail::load_or_create_producer_identity(store, random);

	EXPECT_EQ(1844674407370955161ULL, result.producer_id);
	EXPECT_EQ(0, enum_value(result.error));
	EXPECT_EQ(1U, random.calls);
	EXPECT_EQ((std::vector<StoreCall>{StoreCall::Read,
			StoreCall::CreateTemporary,
			StoreCall::WriteTemporary,
			StoreCall::FlushTemporary,
			StoreCall::CloseTemporary,
			StoreCall::InstallIfAbsent,
			StoreCall::Read}),
		store.calls);
	EXPECT_FALSE(store.temporary_exists);

	json_error_t error{};
	json_t* root = json_loadb(store.written_bytes.data(), store.written_bytes.size(), JSON_REJECT_DUPLICATES, &error);
	ASSERT_NE(nullptr, root) << error.text;
	ASSERT_TRUE(json_is_object(root));
	EXPECT_EQ(2U, json_object_size(root));
	EXPECT_TRUE(json_is_integer(json_object_get(root, "schemaVersion")));
	EXPECT_EQ(1, json_integer_value(json_object_get(root, "schemaVersion")));
	json_t* producer_id = json_object_get(root, "producerId");
	ASSERT_TRUE(json_is_string(producer_id));
	EXPECT_STREQ("1844674407370955161", json_string_value(producer_id));
	json_decref(root);
}

TEST(TelemetryProducerIdentityContract, EveryTemporaryPersistenceFailureFailsClosedAndCleansUp)
{
	struct FailureCase {
		StoreFailure failure;
		std::vector<StoreCall> expected_calls;
	};

	const FailureCase cases[]{
		{StoreFailure::CreateTemporary, {StoreCall::Read, StoreCall::CreateTemporary}},
		{StoreFailure::WriteTemporary,
			{StoreCall::Read, StoreCall::CreateTemporary, StoreCall::WriteTemporary, StoreCall::AbortTemporary}},
		{StoreFailure::FlushTemporary,
			{StoreCall::Read,
				StoreCall::CreateTemporary,
				StoreCall::WriteTemporary,
				StoreCall::FlushTemporary,
				StoreCall::AbortTemporary}},
		{StoreFailure::CloseTemporary,
			{StoreCall::Read,
				StoreCall::CreateTemporary,
				StoreCall::WriteTemporary,
				StoreCall::FlushTemporary,
				StoreCall::CloseTemporary,
				StoreCall::AbortTemporary}},
		{StoreFailure::Install,
			{StoreCall::Read,
				StoreCall::CreateTemporary,
				StoreCall::WriteTemporary,
				StoreCall::FlushTemporary,
				StoreCall::CloseTemporary,
				StoreCall::InstallIfAbsent,
				StoreCall::AbortTemporary,
				StoreCall::Read}},
	};

	for (const auto& item : cases) {
		SCOPED_TRACE(enum_value(item.failure));
		ScriptedProfileStore store;
		store.failure = item.failure;
		store.final_status = detail::ProfileReadStatus::Absent;
		ScriptedRandomSource random({{true, 7U}});

		const auto result = detail::load_or_create_producer_identity(store, random);

		expect_identity_failure(result);
		EXPECT_EQ(1U, random.calls);
		EXPECT_EQ(item.expected_calls, store.calls);
		EXPECT_FALSE(store.temporary_exists);
	}
}

TEST(TelemetryProducerIdentityContract, PublishedProfileMustBeRereadAndMatchTheGeneratedIdentity)
{
	for (const auto& mismatch : std::vector<std::string>{
			R"({"schemaVersion":1,"producerId":"8"})", R"({"schemaVersion":1,"producerId":"invalid"})"}) {
		ScriptedProfileStore store;
		store.final_bytes = mismatch;
		ScriptedRandomSource random({{true, 7U}});

		const auto result = detail::load_or_create_producer_identity(store, random);

		expect_identity_failure(result);
		EXPECT_EQ(StoreCall::Read, store.calls.back());
		EXPECT_EQ(2U, store.read_count);
	}

	ScriptedProfileStore read_error;
	read_error.failure = StoreFailure::FinalRead;
	ScriptedRandomSource random({{true, 7U}});
	expect_identity_failure(detail::load_or_create_producer_identity(read_error, random));
	EXPECT_EQ(StoreCall::Read, read_error.calls.back());
}

TEST(TelemetryProducerIdentityContract, AtomicDestinationRaceNeverClobbersAndUsesTheValidatedWinner)
{
	ScriptedProfileStore store;
	store.install_status = detail::ProfileInstallStatus::DestinationExists;
	store.destination_bytes = R"({"schemaVersion":1,"producerId":"99"})";
	ScriptedRandomSource random({{true, 7U}});

	const auto result = detail::load_or_create_producer_identity(store, random);

	EXPECT_EQ(99U, result.producer_id);
	EXPECT_EQ(0, enum_value(result.error));
	EXPECT_EQ((std::vector<StoreCall>{StoreCall::Read,
			StoreCall::CreateTemporary,
			StoreCall::WriteTemporary,
			StoreCall::FlushTemporary,
			StoreCall::CloseTemporary,
			StoreCall::InstallIfAbsent,
			StoreCall::AbortTemporary,
			StoreCall::Read}),
		store.calls);
	EXPECT_FALSE(store.temporary_exists);
	EXPECT_NE(store.written_bytes, store.destination_bytes);

	ScriptedProfileStore invalid_winner;
	invalid_winner.install_status = detail::ProfileInstallStatus::DestinationExists;
	invalid_winner.destination_bytes = R"({"schemaVersion":1,"producerId":"0"})";
	ScriptedRandomSource second_random({{true, 7U}});
	expect_identity_failure(detail::load_or_create_producer_identity(invalid_winner, second_random));
	EXPECT_FALSE(invalid_winner.temporary_exists);
}

TEST(TelemetryProducerProfileContract, NormalAndPortableModesSelectOnlyTheirWritableInstallationRoots)
{
	EXPECT_EQ(detail::ProfileStorageRoot::WritableUserRoot, detail::profile_storage_root_for_mode(false));
	EXPECT_EQ(detail::ProfileStorageRoot::WritableGameRoot, detail::profile_storage_root_for_mode(true));
}

TEST(TelemetrySessionIdentityContract, InitialCandidateDrawNeedsNoRegistryAndConsumesExactlyOneEntropyWord)
{
	ScriptedRandomSource successful_random({{true, 42U}, {true, 99U}});
	const auto candidate = detail::draw_session_id_candidate(successful_random);
	EXPECT_EQ(detail::SessionIdCandidateStatus::Ready, candidate.status);
	EXPECT_EQ(42U, candidate.session_id);
	EXPECT_EQ(1U, successful_random.calls);

	for (const auto draw : std::vector<ScriptedRandomSource::Draw>{{false, 0U}, {true, 0U}}) {
		ScriptedRandomSource failing_random({draw, {true, 7U}});
		const auto failure = detail::draw_session_id_candidate(failing_random);
		EXPECT_EQ(detail::SessionIdCandidateStatus::EntropyFailure, failure.status);
		EXPECT_EQ(0U, failure.session_id);
		EXPECT_EQ(1U, failing_random.calls);
	}
}

TEST(TelemetrySessionIdentityContract, RegistryStorageLifecycleAndRegistrationStatusesAreExplicitAndIdempotent)
{
	static_assert(std::is_same_v<detail::SessionIdRegistryStorage,
		std::array<std::uint64_t, 131'072U>>,
		"The budget must price the registry's exact storage representation.");
	EXPECT_EQ(131'072U, detail::SessionIdRegistryStorageSlotCount);
	EXPECT_EQ(1'048'576U, detail::SessionIdRegistryStorageBytes);
	EXPECT_EQ(sizeof(detail::SessionIdRegistryStorage), detail::SessionIdRegistryStorageBytes);
	EXPECT_EQ(detail::SessionIdRegistryStorageSlotCount, detail::SessionIdRegistry::StorageSlotCount);
	EXPECT_EQ(detail::SessionIdRegistryStorageBytes, detail::SessionIdRegistry::StorageBytes);

	detail::SessionIdRegistry registry;
	EXPECT_FALSE(registry.storage_ready());
	EXPECT_EQ(0U, registry.used_count());
	EXPECT_EQ(detail::SessionIdRegistrationStatus::StorageUnavailable, registry.register_candidate(7U));

	ASSERT_TRUE(registry.allocate_storage());
	EXPECT_TRUE(registry.storage_ready());
	EXPECT_EQ(detail::SessionIdRegistrationStatus::InvalidCandidate, registry.register_candidate(0U));
	EXPECT_EQ(detail::SessionIdRegistrationStatus::Registered, registry.register_candidate(7U));
	EXPECT_EQ(detail::SessionIdRegistrationStatus::Duplicate, registry.register_candidate(7U));
	EXPECT_EQ(1U, registry.used_count());

	ASSERT_TRUE(registry.allocate_storage()) << "Idempotent allocation must preserve existing registrations.";
	EXPECT_EQ(1U, registry.used_count());
	EXPECT_EQ(detail::SessionIdRegistrationStatus::Duplicate, registry.register_candidate(7U));

	registry.release_storage();
	registry.release_storage();
	EXPECT_FALSE(registry.storage_ready());
	EXPECT_EQ(0U, registry.used_count());
	EXPECT_EQ(detail::SessionIdRegistrationStatus::StorageUnavailable, registry.register_candidate(8U));

	ASSERT_TRUE(registry.allocate_storage());
	EXPECT_EQ(0U, registry.used_count());
	EXPECT_EQ(detail::SessionIdRegistrationStatus::Registered, registry.register_candidate(7U));
}

TEST(TelemetrySessionIdentityContract, InitialCandidateRegistersAfterStorageAllocationWithoutASecondEntropyDraw)
{
	ScriptedRandomSource random({{true, 42U}, {true, 99U}});
	const auto candidate = detail::draw_session_id_candidate(random);
	ASSERT_EQ(detail::SessionIdCandidateStatus::Ready, candidate.status);
	ASSERT_EQ(1U, random.calls);

	detail::SessionIdRegistry registry;
	ASSERT_FALSE(registry.storage_ready());
	const auto budget =
		detail::calculate_wp03_known_budget_subtotal(detail::make_wp03_known_budget_request(1U));
	ASSERT_EQ(detail::StartupBudgetError::None, budget.error);
	EXPECT_FALSE(budget.is_complete);
	EXPECT_EQ(detail::SessionIdRegistryStorageBytes, budget.session_id_registry_bytes);
	EXPECT_FALSE(registry.storage_ready()) << "Budget calculation must not allocate the registry.";
	EXPECT_EQ(1U, random.calls) << "Budget calculation must not redraw the candidate.";

	ASSERT_TRUE(registry.allocate_storage());
	EXPECT_EQ(detail::SessionIdRegistrationStatus::Registered,
		registry.register_candidate(candidate.session_id));
	EXPECT_EQ(1U, random.calls) << "Registration must not consume a second entropy word.";
	EXPECT_EQ(1U, registry.used_count());

	detail::SessionIdAllocator allocator(random, registry);
	const auto next = allocator.allocate();
	EXPECT_EQ(detail::SessionIdStatus::Allocated, next.status);
	EXPECT_EQ(99U, next.session_id);
	EXPECT_EQ(2U, random.calls);
	EXPECT_EQ(2U, allocator.used_count());
}

TEST(TelemetrySessionIdentityContract, AllocatorWithoutRegistryStorageFailsBeforeEntropy)
{
	ScriptedRandomSource random({{true, 42U}});
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator allocator(random, registry);

	const auto result = allocator.allocate();

	EXPECT_EQ(detail::SessionIdStatus::StorageUnavailable, result.status);
	EXPECT_EQ(0U, result.session_id);
	EXPECT_EQ(0U, random.calls);
	EXPECT_EQ(0U, allocator.used_count());
}

TEST(TelemetrySessionIdentityContract, EntropyFailureAndZeroEachFailImmediatelyWithoutRetry)
{
	for (const auto draw : std::vector<ScriptedRandomSource::Draw>{{false, 0U}, {true, 0U}}) {
		ScriptedRandomSource random({draw, {true, 7U}});
		detail::SessionIdRegistry registry;
		ASSERT_TRUE(registry.allocate_storage());
		detail::SessionIdAllocator allocator(random, registry);

		const auto result = allocator.allocate();

		EXPECT_EQ(detail::SessionIdStatus::EntropyFailure, result.status);
		EXPECT_EQ(0U, result.session_id);
		EXPECT_EQ(1U, random.calls);
		EXPECT_EQ(0U, allocator.used_count());
	}
}

TEST(TelemetrySessionIdentityContract, FifteenCollisionsThenUniqueOnDrawSixteenSucceeds)
{
	std::vector<ScriptedRandomSource::Draw> draws{{true, 42U}};
	for (std::size_t i = 0; i < 15U; ++i) {
		draws.push_back({true, 42U});
	}
	draws.push_back({true, 99U});
	ScriptedRandomSource random(std::move(draws));
	detail::SessionIdRegistry registry;
	ASSERT_TRUE(registry.allocate_storage());
	detail::SessionIdAllocator allocator(random, registry);
	ASSERT_EQ(detail::SessionIdStatus::Allocated, allocator.allocate().status);

	const auto result = allocator.allocate();

	EXPECT_EQ(detail::SessionIdStatus::Allocated, result.status);
	EXPECT_EQ(99U, result.session_id);
	EXPECT_EQ(1U + MaxSessionIdDraws, random.calls);
	EXPECT_EQ(2U, allocator.used_count());
}

TEST(TelemetrySessionIdentityContract, SixteenCollisionsStopAtRetryLimitAndNeverConsumeDrawSeventeen)
{
	std::vector<ScriptedRandomSource::Draw> draws{{true, 42U}};
	for (std::size_t i = 0; i < MaxSessionIdDraws; ++i) {
		draws.push_back({true, 42U});
	}
	draws.push_back({true, 99U});
	ScriptedRandomSource random(std::move(draws));
	detail::SessionIdRegistry registry;
	ASSERT_TRUE(registry.allocate_storage());
	detail::SessionIdAllocator allocator(random, registry);
	ASSERT_EQ(detail::SessionIdStatus::Allocated, allocator.allocate().status);

	const auto result = allocator.allocate();

	EXPECT_EQ(detail::SessionIdStatus::RetryLimit, result.status);
	EXPECT_EQ(0U, result.session_id);
	EXPECT_EQ(1U + MaxSessionIdDraws, random.calls);
	EXPECT_EQ(1U, allocator.used_count());
}

TEST(TelemetrySessionIdentityContract, EntropyFailureDuringCollisionRetriesIsDistinct)
{
	ScriptedRandomSource random({{true, 42U}, {true, 42U}, {false, 0U}, {true, 99U}});
	detail::SessionIdRegistry registry;
	ASSERT_TRUE(registry.allocate_storage());
	detail::SessionIdAllocator allocator(random, registry);
	ASSERT_EQ(detail::SessionIdStatus::Allocated, allocator.allocate().status);

	const auto result = allocator.allocate();

	EXPECT_EQ(detail::SessionIdStatus::EntropyFailure, result.status);
	EXPECT_EQ(0U, result.session_id);
	EXPECT_EQ(3U, random.calls);
	EXPECT_EQ(1U, allocator.used_count());
}

TEST(TelemetrySessionIdentityContract, CapacityIsCheckedBeforeEntropyAndNoIdentityIsEvictedOrReused)
{
	SequentialRandomSource random;
	detail::SessionIdRegistry registry;
	ASSERT_TRUE(registry.allocate_storage());
	detail::SessionIdAllocator allocator(random, registry);

	for (std::size_t i = 0; i < MaxSessionIdsPerProcess; ++i) {
		const auto result = allocator.allocate();
		ASSERT_EQ(detail::SessionIdStatus::Allocated, result.status) << i;
		ASSERT_EQ(i + 1U, result.session_id) << i;
	}
	ASSERT_EQ(MaxSessionIdsPerProcess, allocator.used_count());
	ASSERT_EQ(MaxSessionIdsPerProcess, random.calls);

	const auto exhausted = allocator.allocate();
	EXPECT_EQ(detail::SessionIdStatus::CapacityExhausted, exhausted.status);
	EXPECT_EQ(0U, exhausted.session_id);
	EXPECT_EQ(MaxSessionIdsPerProcess, random.calls) << "Capacity must be checked before the RNG call.";
	EXPECT_EQ(MaxSessionIdsPerProcess, allocator.used_count());
	EXPECT_EQ(detail::SessionIdRegistrationStatus::Capacity,
		registry.register_candidate(MaxSessionIdsPerProcess + 1U));
}

} // namespace
