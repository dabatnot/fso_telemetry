#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace telemetry::detail {

constexpr std::size_t MaximumProducerProfileBytes = 1024U;
constexpr std::size_t MaximumSessionIdsPerProcess = 65'536U;
constexpr std::size_t MaximumSessionIdCollisionDraws = 16U;
constexpr std::size_t SessionIdRegistryStorageSlotCount = MaximumSessionIdsPerProcess * 2U;
using SessionIdRegistryStorage = std::array<std::uint64_t, SessionIdRegistryStorageSlotCount>;
constexpr std::size_t SessionIdRegistryStorageBytes = sizeof(SessionIdRegistryStorage);

class RandomSource {
  public:
	virtual ~RandomSource() = default;
	virtual bool next_u64(std::uint64_t& value) noexcept = 0;
};

class OsRandomSource final : public RandomSource {
  public:
	bool next_u64(std::uint64_t& value) noexcept override;
};

enum class ProfileReadStatus : std::uint8_t {
	Absent = 0,
	Present,
	IoError,
};

enum class ProfileInstallStatus : std::uint8_t {
	Published = 0,
	DestinationExists,
	Error,
};

struct ProfileTempToken {
	std::uint64_t value = 0;
};

class ProducerProfileStore {
  public:
	virtual ~ProducerProfileStore() = default;

	virtual ProfileReadStatus read(std::string& bytes) noexcept = 0;
	virtual bool create_temporary_in_profile_directory(ProfileTempToken& token) noexcept = 0;
	virtual bool write_temporary(ProfileTempToken token, std::string_view bytes) noexcept = 0;
	virtual bool flush_temporary_durably(ProfileTempToken token) noexcept = 0;
	virtual bool close_temporary(ProfileTempToken token) noexcept = 0;
	virtual ProfileInstallStatus install_if_absent_atomically(ProfileTempToken token) noexcept = 0;
	virtual void abort_temporary(ProfileTempToken token) noexcept = 0;
};

enum class ProfileStorageRoot : std::uint8_t {
	WritableUserRoot = 0,
	WritableGameRoot,
};

ProfileStorageRoot profile_storage_root_for_mode(bool portable_mode) noexcept;

// The root constructor is the production path and resolves only the selected
// CFile writable root. The exact-path constructor is an internal injection seam
// for isolated native-store verification; it rejects relative or parentless paths.
class NativeProducerProfileStore final : public ProducerProfileStore {
  public:
	explicit NativeProducerProfileStore(ProfileStorageRoot root) noexcept;
	explicit NativeProducerProfileStore(std::string exact_profile_path);
	~NativeProducerProfileStore() override;

	NativeProducerProfileStore(const NativeProducerProfileStore&) = delete;
	NativeProducerProfileStore& operator=(const NativeProducerProfileStore&) = delete;
	NativeProducerProfileStore(NativeProducerProfileStore&&) = delete;
	NativeProducerProfileStore& operator=(NativeProducerProfileStore&&) = delete;

	ProfileReadStatus read(std::string& bytes) noexcept override;
	bool create_temporary_in_profile_directory(ProfileTempToken& token) noexcept override;
	bool write_temporary(ProfileTempToken token, std::string_view bytes) noexcept override;
	bool flush_temporary_durably(ProfileTempToken token) noexcept override;
	bool close_temporary(ProfileTempToken token) noexcept override;
	ProfileInstallStatus install_if_absent_atomically(ProfileTempToken token) noexcept override;
	void abort_temporary(ProfileTempToken token) noexcept override;

  private:
	bool resolve_profile_path(std::string& path, bool create_directory) noexcept;
	bool token_is_active(ProfileTempToken token) const noexcept;
	void clear_temporary_state() noexcept;

	ProfileStorageRoot m_root = ProfileStorageRoot::WritableUserRoot;
	bool m_uses_exact_path = false;
	std::string m_exact_profile_path;
	std::string m_temporary_path;
	std::FILE* m_temporary_file = nullptr;
	std::uint64_t m_active_token = 0;
	std::uint64_t m_next_token = 1;
	bool m_temporary_flushed = false;
};

enum class IdentityError : std::uint8_t {
	None = 0,
	ProfileTooLarge,
	InvalidJson,
	MaximumDepthExceeded,
	InvalidSchema,
	InvalidProducerId,
	ProfileReadFailure,
	EntropyFailure,
	TemporaryCreateFailure,
	TemporaryWriteFailure,
	TemporaryFlushFailure,
	TemporaryCloseFailure,
	AtomicInstallFailure,
	FinalReadFailure,
	FinalIdentityMismatch,
};

struct IdentityResult {
	std::uint64_t producer_id = 0;
	IdentityError error = IdentityError::None;
};

IdentityResult parse_producer_profile_json(std::string_view input) noexcept;
IdentityResult load_or_create_producer_identity(ProducerProfileStore& store, RandomSource& random) noexcept;

enum class SessionIdCandidateStatus : std::uint8_t {
	Ready = 0,
	EntropyFailure,
};

struct SessionIdCandidateResult {
	SessionIdCandidateStatus status = SessionIdCandidateStatus::EntropyFailure;
	std::uint64_t session_id = 0;
};

SessionIdCandidateResult draw_session_id_candidate(RandomSource& random) noexcept;

enum class SessionIdRegistrationStatus : std::uint8_t {
	Registered = 0,
	Duplicate,
	InvalidCandidate,
	StorageUnavailable,
	Capacity,
};

class SessionIdRegistry final {
  public:
	static constexpr std::size_t StorageSlotCount = SessionIdRegistryStorageSlotCount;
	static constexpr std::size_t StorageBytes = SessionIdRegistryStorageBytes;

	SessionIdRegistry() noexcept = default;

	SessionIdRegistry(const SessionIdRegistry&) = delete;
	SessionIdRegistry& operator=(const SessionIdRegistry&) = delete;
	SessionIdRegistry(SessionIdRegistry&&) = delete;
	SessionIdRegistry& operator=(SessionIdRegistry&&) = delete;

	bool allocate_storage() noexcept;
	void release_storage() noexcept;
	SessionIdRegistrationStatus register_candidate(std::uint64_t candidate) noexcept;

	bool storage_ready() const noexcept
	{
		return m_used_ids != nullptr;
	}
	std::size_t used_count() const noexcept
	{
		return m_used_count;
	}

  private:
	using Storage = SessionIdRegistryStorage;

	std::unique_ptr<Storage> m_used_ids;
	std::size_t m_used_count = 0U;
};

enum class SessionIdStatus : std::uint8_t {
	Allocated = 0,
	StorageUnavailable,
	EntropyFailure,
	RetryLimit,
	CapacityExhausted,
};

struct SessionIdResult {
	SessionIdStatus status = SessionIdStatus::EntropyFailure;
	std::uint64_t session_id = 0;
};

class SessionIdAllocator final {
  public:
	SessionIdAllocator(RandomSource& random, SessionIdRegistry& registry) noexcept;

	SessionIdAllocator(const SessionIdAllocator&) = delete;
	SessionIdAllocator& operator=(const SessionIdAllocator&) = delete;
	SessionIdAllocator(SessionIdAllocator&&) = delete;
	SessionIdAllocator& operator=(SessionIdAllocator&&) = delete;

	SessionIdResult allocate() noexcept;
	std::size_t used_count() const noexcept
	{
		return m_registry.used_count();
	}

  private:
	RandomSource& m_random;
	SessionIdRegistry& m_registry;
};

} // namespace telemetry::detail
