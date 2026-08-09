#include "telemetry/identity.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace {

namespace detail = telemetry::detail;

class FixedRandomSource final : public detail::RandomSource {
  public:
	explicit FixedRandomSource(std::uint64_t value) : m_value(value) {}

	bool next_u64(std::uint64_t& value) noexcept override
	{
		++calls;
		value = m_value;
		return true;
	}

	std::size_t calls = 0U;

  private:
	std::uint64_t m_value;
};

class IsolatedTemporaryDirectory final {
  public:
	IsolatedTemporaryDirectory()
	{
		static std::atomic<std::uint64_t> ordinal{0U};
		std::error_code error;
		const auto base = std::filesystem::temp_directory_path(error);
		if (error) {
			return;
		}

		const auto clock_value = static_cast<std::uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count());
		for (std::uint64_t attempt = 0; attempt < 64U; ++attempt) {
			m_path = base / ("fs2open-telemetry-wp03-" + std::to_string(clock_value) + "-" +
				std::to_string(ordinal.fetch_add(1U, std::memory_order_relaxed)));
			error.clear();
			if (std::filesystem::create_directory(m_path, error)) {
				m_ready = true;
				return;
			}
			if (error) {
				return;
			}
		}
	}

	~IsolatedTemporaryDirectory()
	{
		if (m_ready) {
			std::error_code ignored;
			std::filesystem::remove_all(m_path, ignored);
		}
	}

	bool ready() const noexcept
	{
		return m_ready;
	}

	const std::filesystem::path& path() const noexcept
	{
		return m_path;
	}

	std::string profile_path() const
	{
		return (m_path / "telemetry-profile.json").string();
	}

  private:
	std::filesystem::path m_path;
	bool m_ready = false;
};

std::size_t directory_entry_count(const std::filesystem::path& directory) noexcept
{
	std::error_code error;
	std::size_t count = 0U;
	std::filesystem::directory_iterator iterator(directory, error);
	const std::filesystem::directory_iterator end;
	while (!error && iterator != end) {
		++count;
		iterator.increment(error);
	}
	return error ? static_cast<std::size_t>(-1) : count;
}

bool write_file(const std::string& path, const std::string& bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	output.flush();
	return output.good();
}

std::string read_file(const std::string& path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(TelemetryProducerProfileNativeContract, ExactPathCreatesDurablyPublishesAndRereadsWithoutTemporaryLeak)
{
	IsolatedTemporaryDirectory directory;
	ASSERT_TRUE(directory.ready());
	const auto profile_path = directory.profile_path();
	FixedRandomSource first_random(7U);

	{
		detail::NativeProducerProfileStore store(profile_path);
		const auto created = detail::load_or_create_producer_identity(store, first_random);
		ASSERT_EQ(detail::IdentityError::None, created.error);
		EXPECT_EQ(7U, created.producer_id);
	}

	EXPECT_EQ(1U, first_random.calls);
	EXPECT_EQ(1U, directory_entry_count(directory.path()));
	EXPECT_EQ(R"({"schemaVersion":1,"producerId":"7"})", read_file(profile_path));

	FixedRandomSource second_random(99U);
	detail::NativeProducerProfileStore reopened(profile_path);
	const auto reread = detail::load_or_create_producer_identity(reopened, second_random);
	EXPECT_EQ(detail::IdentityError::None, reread.error);
	EXPECT_EQ(7U, reread.producer_id);
	EXPECT_EQ(0U, second_random.calls);
	EXPECT_EQ(1U, directory_entry_count(directory.path()));
}

TEST(TelemetryProducerProfileNativeContract, AtomicInstallNeverClobbersConcurrentDestinationAndAbortCleansSameDirectoryTemp)
{
	IsolatedTemporaryDirectory directory;
	ASSERT_TRUE(directory.ready());
	const auto profile_path = directory.profile_path();
	detail::NativeProducerProfileStore store(profile_path);
	detail::ProfileTempToken token;
	const std::string losing_bytes{R"({"schemaVersion":1,"producerId":"7"})"};
	const std::string winning_bytes{R"({"schemaVersion":1,"producerId":"99"})"};

	ASSERT_TRUE(store.create_temporary_in_profile_directory(token));
	EXPECT_EQ(1U, directory_entry_count(directory.path()))
		<< "The exclusive temporary must be created beside the destination.";
	ASSERT_TRUE(store.write_temporary(token, losing_bytes));
	ASSERT_TRUE(store.flush_temporary_durably(token));
	ASSERT_TRUE(store.close_temporary(token));
	ASSERT_TRUE(write_file(profile_path, winning_bytes));
	ASSERT_EQ(2U, directory_entry_count(directory.path()));

	EXPECT_EQ(detail::ProfileInstallStatus::DestinationExists, store.install_if_absent_atomically(token));
	EXPECT_EQ(winning_bytes, read_file(profile_path));
	store.abort_temporary(token);
	EXPECT_EQ(1U, directory_entry_count(directory.path()));
	EXPECT_EQ(winning_bytes, read_file(profile_path));
}

TEST(TelemetryProducerProfileNativeContract, DestructorRemovesAnAbandonedSameDirectoryTemporary)
{
	IsolatedTemporaryDirectory directory;
	ASSERT_TRUE(directory.ready());
	{
		detail::NativeProducerProfileStore store(directory.profile_path());
		detail::ProfileTempToken token;
		ASSERT_TRUE(store.create_temporary_in_profile_directory(token));
		ASSERT_EQ(1U, directory_entry_count(directory.path()));
	}
	EXPECT_EQ(0U, directory_entry_count(directory.path()));
}

TEST(TelemetryProducerProfileNativeContract, ExactPathInjectionRejectsRelativeDestinations)
{
	detail::NativeProducerProfileStore store("telemetry-profile.json");
	std::string bytes;
	detail::ProfileTempToken token;
	EXPECT_EQ(detail::ProfileReadStatus::IoError, store.read(bytes));
	EXPECT_FALSE(store.create_temporary_in_profile_directory(token));
}

TEST(TelemetryProducerOsRandomContract, OsEntropySucceedsAndProducesANonzeroWordWithoutLoggingIt)
{
	detail::OsRandomSource random;
	std::uint64_t value = 0U;
	ASSERT_TRUE(random.next_u64(value));
	EXPECT_TRUE(value != 0U);
}

} // namespace
