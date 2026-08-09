#include "telemetry/identity.h"

#include "telemetry/json_preflight.h"

#include "cfile/cfile.h"
#include "cfile/cfilesystem.h"
#include "libs/jansson.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <stdlib.h>
#endif
#endif

#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace telemetry::detail {

namespace {

constexpr char ProducerProfileFilename[] = "telemetry-profile.json";
constexpr std::size_t MaximumProfileJsonContainerDepth = 2U;
constexpr std::size_t MaximumTemporaryCreateAttempts = 32U;

struct CFileCloser {
	void operator()(CFILE* file) const noexcept
	{
		if (file != nullptr) {
			cfclose(file);
		}
	}
};

IdentityResult identity_failure(IdentityError error) noexcept
{
	IdentityResult result;
	result.error = error;
	return result;
}

bool is_known_profile_key(const char* key) noexcept
{
	return std::strcmp(key, "schemaVersion") == 0 || std::strcmp(key, "producerId") == 0;
}

bool parse_canonical_producer_id(const json_t* value, std::uint64_t& producer_id) noexcept
{
	producer_id = 0;
	if (!json_is_string(value)) {
		return false;
	}
	const auto length = json_string_length(value);
	const auto* text = json_string_value(value);
	if (length == 0U || length > 20U || (length > 1U && text[0] == '0')) {
		return false;
	}

	std::uint64_t parsed = 0;
	for (std::size_t index = 0; index < length; ++index) {
		const auto character = text[index];
		if (character < '0' || character > '9') {
			return false;
		}
		const auto digit = static_cast<std::uint64_t>(character - '0');
		if (parsed > (UINT64_MAX - digit) / 10U) {
			return false;
		}
		parsed = parsed * 10U + digit;
	}
	if (parsed == 0U) {
		return false;
	}
	producer_id = parsed;
	return true;
}

bool serialize_producer_profile(std::uint64_t producer_id,
	std::array<char, 96>& output,
	std::size_t& output_size) noexcept
{
	output_size = 0;
	constexpr std::string_view Prefix{"{\"schemaVersion\":1,\"producerId\":\""};
	constexpr std::string_view Suffix{"\"}"};
	std::memcpy(output.data(), Prefix.data(), Prefix.size());
	auto* first = output.data() + Prefix.size();
	auto* last = output.data() + output.size() - Suffix.size();
	const auto conversion = std::to_chars(first, last, producer_id);
	if (conversion.ec != std::errc{}) {
		return false;
	}
	std::memcpy(conversion.ptr, Suffix.data(), Suffix.size());
	output_size = static_cast<std::size_t>(conversion.ptr - output.data()) + Suffix.size();
	return true;
}

std::uint32_t location_flags_for_root(ProfileStorageRoot root) noexcept
{
	return root == ProfileStorageRoot::WritableGameRoot
		? static_cast<std::uint32_t>(CF_LOCATION_ROOT_GAME | CF_LOCATION_TYPE_ROOT)
		: static_cast<std::uint32_t>(CF_LOCATION_ROOT_USER | CF_LOCATION_TYPE_ROOT);
}

bool is_unambiguous_absolute_profile_path(const std::string& path) noexcept
{
	try {
		const std::filesystem::path parsed(path);
		return !path.empty() && parsed.is_absolute() && parsed.has_parent_path() && parsed.has_filename();
	} catch (...) {
		return false;
	}
}

ProfileReadStatus read_native_profile(const std::string& path, std::string& bytes) noexcept
{
	errno = 0;
	auto* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		return errno == ENOENT ? ProfileReadStatus::Absent : ProfileReadStatus::IoError;
	}

	std::array<char, MaximumProducerProfileBytes + 1U> buffer{};
	const auto read_count = std::fread(buffer.data(), 1U, buffer.size(), file);
	const auto read_failed = std::ferror(file) != 0;
	const auto close_failed = std::fclose(file) != 0;
	if (read_failed || close_failed) {
		return ProfileReadStatus::IoError;
	}
	try {
		bytes.assign(buffer.data(), read_count);
	} catch (...) {
		bytes.clear();
		return ProfileReadStatus::IoError;
	}
	return ProfileReadStatus::Present;
}

enum class ExclusiveOpenResult : std::uint8_t {
	Opened = 0,
	AlreadyExists,
	Error,
};

ExclusiveOpenResult open_exclusive_temporary(const std::string& path, std::FILE*& file) noexcept
{
	file = nullptr;
	errno = 0;
#ifdef _WIN32
	const auto descriptor = _open(path.c_str(),
		_O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
		_S_IREAD | _S_IWRITE);
#else
	int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	const auto descriptor = open(path.c_str(), flags, S_IRUSR | S_IWUSR);
#endif
	if (descriptor < 0) {
		return errno == EEXIST ? ExclusiveOpenResult::AlreadyExists : ExclusiveOpenResult::Error;
	}

#ifdef _WIN32
	file = _fdopen(descriptor, "wb");
#else
	file = fdopen(descriptor, "wb");
#endif
	if (file != nullptr) {
		return ExclusiveOpenResult::Opened;
	}

#ifdef _WIN32
	_close(descriptor);
	_unlink(path.c_str());
#else
	close(descriptor);
	unlink(path.c_str());
#endif
	return ExclusiveOpenResult::Error;
}

bool durable_flush(std::FILE* file) noexcept
{
	if (file == nullptr || std::fflush(file) != 0) {
		return false;
	}
#ifdef _WIN32
	return _commit(_fileno(file)) == 0;
#else
	return fsync(fileno(file)) == 0;
#endif
}

void remove_native_file(const std::string& path) noexcept
{
	if (path.empty()) {
		return;
	}
#ifdef _WIN32
	_unlink(path.c_str());
#else
	unlink(path.c_str());
#endif
}

std::uint64_t process_id() noexcept
{
#ifdef _WIN32
	return static_cast<std::uint64_t>(_getpid());
#else
	return static_cast<std::uint64_t>(getpid());
#endif
}

ProfileInstallStatus install_native_no_clobber(const std::string& temporary,
	const std::string& destination) noexcept
{
#ifdef _WIN32
	if (MoveFileExA(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
		return ProfileInstallStatus::Published;
	}
	const auto error = GetLastError();
	return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
		? ProfileInstallStatus::DestinationExists
		: ProfileInstallStatus::Error;
#else
	if (link(temporary.c_str(), destination.c_str()) == 0) {
		return unlink(temporary.c_str()) == 0 ? ProfileInstallStatus::Published : ProfileInstallStatus::Error;
	}
	return errno == EEXIST ? ProfileInstallStatus::DestinationExists : ProfileInstallStatus::Error;
#endif
}

#ifdef _WIN32
using BCryptGenRandomFunction = LONG(WINAPI*)(void*, unsigned char*, ULONG, ULONG);

BCryptGenRandomFunction load_bcrypt_gen_random() noexcept
{
	constexpr wchar_t BcryptFilename[] = L"bcrypt.dll";
	constexpr std::size_t BcryptFilenameLength = sizeof(BcryptFilename) / sizeof(BcryptFilename[0]);
	std::array<wchar_t, MAX_PATH> system_path{};
	auto length = GetSystemDirectoryW(system_path.data(), static_cast<UINT>(system_path.size()));
	if (length == 0U || length >= system_path.size()) {
		return nullptr;
	}
	if (system_path[length - 1U] != L'\\') {
		if (length + 1U >= system_path.size()) {
			return nullptr;
		}
		system_path[length++] = L'\\';
	}
	if (length + BcryptFilenameLength > system_path.size()) {
		return nullptr;
	}
	std::memcpy(system_path.data() + length, BcryptFilename, sizeof(BcryptFilename));

	const auto module = LoadLibraryW(system_path.data());
	if (module == nullptr) {
		return nullptr;
	}
	const auto function = reinterpret_cast<BCryptGenRandomFunction>(GetProcAddress(module, "BCryptGenRandom"));
	if (function == nullptr) {
		FreeLibrary(module);
	}
	// On success the module reference intentionally lives for the process. This
	// avoids a loader call for every future session ID and keeps the function
	// pointer valid without adding a bcrypt import-library dependency.
	return function;
}
#endif

std::size_t hash_session_id(std::uint64_t value) noexcept
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
	value ^= value >> 31U;
	return static_cast<std::size_t>(value);
}

} // namespace

bool OsRandomSource::next_u64(std::uint64_t& value) noexcept
{
	value = 0;
#ifdef _WIN32
	static const auto bcrypt_gen_random = load_bcrypt_gen_random();
	constexpr ULONG UseSystemPreferredRng = 0x00000002UL;
	return bcrypt_gen_random != nullptr &&
		bcrypt_gen_random(nullptr,
			reinterpret_cast<unsigned char*>(&value),
			static_cast<ULONG>(sizeof(value)),
			UseSystemPreferredRng) >= 0;
#else
	// `getentropy` is unavailable in older macOS SDK/deployment combinations,
	// while arc4random_buf is a system CSPRNG available across supported macOS.
	// Keep getentropy on the other Unix targets, where it remains the native
	// no-allocation entropy source.
#ifdef __APPLE__
	arc4random_buf(&value, sizeof(value));
	return true;
#else
	return getentropy(&value, sizeof(value)) == 0;
#endif
#endif
}

ProfileStorageRoot profile_storage_root_for_mode(bool portable_mode) noexcept
{
	return portable_mode ? ProfileStorageRoot::WritableGameRoot : ProfileStorageRoot::WritableUserRoot;
}

NativeProducerProfileStore::NativeProducerProfileStore(ProfileStorageRoot root) noexcept : m_root(root) {}

NativeProducerProfileStore::NativeProducerProfileStore(std::string exact_profile_path)
	: m_uses_exact_path(true), m_exact_profile_path(std::move(exact_profile_path))
{
}

NativeProducerProfileStore::~NativeProducerProfileStore()
{
	if (m_active_token != 0U) {
		abort_temporary(ProfileTempToken{m_active_token});
	}
}

bool NativeProducerProfileStore::resolve_profile_path(std::string& path, bool create_directory) noexcept
{
	try {
		if (m_uses_exact_path) {
			path = m_exact_profile_path;
			return is_unambiguous_absolute_profile_path(path);
		}

		const auto flags = location_flags_for_root(m_root);
		if (create_directory) {
			cf_create_directory(CF_TYPE_CONFIG, flags);
		}
		SCP_string resolved;
		if (cf_create_default_path_string(resolved, CF_TYPE_CONFIG, ProducerProfileFilename, flags) == 0) {
			return false;
		}
		path.assign(resolved.data(), resolved.size());
		return is_unambiguous_absolute_profile_path(path);
	} catch (...) {
		path.clear();
		return false;
	}
}

ProfileReadStatus NativeProducerProfileStore::read(std::string& bytes) noexcept
{
	try {
		bytes.clear();
		if (m_uses_exact_path) {
			std::string path;
			return resolve_profile_path(path, false) ? read_native_profile(path, bytes) : ProfileReadStatus::IoError;
		}

		const auto location = cf_find_file_location(
			ProducerProfileFilename, CF_TYPE_CONFIG, location_flags_for_root(m_root));
		if (!location.found || location.offset != 0U || location.data_ptr != nullptr) {
			return ProfileReadStatus::Absent;
		}
		if (location.size > MaximumProducerProfileBytes) {
			bytes.assign(MaximumProducerProfileBytes + 1U, ' ');
			return ProfileReadStatus::Present;
		}

		std::unique_ptr<CFILE, CFileCloser> file(cfopen_special(location, "rb", CF_TYPE_CONFIG));
		if (!file) {
			return ProfileReadStatus::IoError;
		}
		std::array<char, MaximumProducerProfileBytes> buffer{};
		const auto read_count = location.size == 0U
			? 0
			: cfread(buffer.data(), 1, static_cast<int>(location.size), file.get());
		const auto close_result = cfclose(file.release());
		if (read_count != static_cast<int>(location.size) || close_result != 0) {
			return ProfileReadStatus::IoError;
		}
		bytes.assign(buffer.data(), location.size);
		return ProfileReadStatus::Present;
	} catch (...) {
		bytes.clear();
		return ProfileReadStatus::IoError;
	}
}

bool NativeProducerProfileStore::create_temporary_in_profile_directory(ProfileTempToken& token) noexcept
{
	token = {};
	if (m_active_token != 0U || m_temporary_file != nullptr || !m_temporary_path.empty()) {
		return false;
	}

	try {
		std::string destination;
		if (!resolve_profile_path(destination, true)) {
			return false;
		}

		for (std::size_t attempt = 0; attempt < MaximumTemporaryCreateAttempts; ++attempt) {
			auto candidate_token = m_next_token++;
			if (candidate_token == 0U) {
				candidate_token = m_next_token++;
				if (candidate_token == 0U) {
					return false;
				}
			}
			const auto temporary = destination + ".tmp." + std::to_string(process_id()) + "." +
				std::to_string(candidate_token);
			std::FILE* temporary_file = nullptr;
			const auto open_result = open_exclusive_temporary(temporary, temporary_file);
			if (open_result == ExclusiveOpenResult::AlreadyExists) {
				continue;
			}
			if (open_result != ExclusiveOpenResult::Opened) {
				return false;
			}

			m_temporary_path = temporary;
			m_temporary_file = temporary_file;
			m_active_token = candidate_token;
			m_temporary_flushed = false;
			token.value = candidate_token;
			return true;
		}
	} catch (...) {
		return false;
	}
	return false;
}

bool NativeProducerProfileStore::write_temporary(ProfileTempToken token, std::string_view bytes) noexcept
{
	if (!token_is_active(token) || m_temporary_file == nullptr || bytes.empty()) {
		return false;
	}
	const auto written = std::fwrite(bytes.data(), 1U, bytes.size(), m_temporary_file);
	m_temporary_flushed = false;
	return written == bytes.size();
}

bool NativeProducerProfileStore::flush_temporary_durably(ProfileTempToken token) noexcept
{
	if (!token_is_active(token) || m_temporary_file == nullptr) {
		return false;
	}
	m_temporary_flushed = durable_flush(m_temporary_file);
	return m_temporary_flushed;
}

bool NativeProducerProfileStore::close_temporary(ProfileTempToken token) noexcept
{
	if (!token_is_active(token) || m_temporary_file == nullptr) {
		return false;
	}
	auto* file = m_temporary_file;
	m_temporary_file = nullptr;
	return std::fclose(file) == 0;
}

ProfileInstallStatus NativeProducerProfileStore::install_if_absent_atomically(ProfileTempToken token) noexcept
{
	if (!token_is_active(token) || m_temporary_file != nullptr || !m_temporary_flushed ||
		m_temporary_path.empty()) {
		return ProfileInstallStatus::Error;
	}
	std::string destination;
	if (!resolve_profile_path(destination, false)) {
		return ProfileInstallStatus::Error;
	}
	const auto result = install_native_no_clobber(m_temporary_path, destination);
	if (result == ProfileInstallStatus::Published) {
		clear_temporary_state();
	}
	return result;
}

void NativeProducerProfileStore::abort_temporary(ProfileTempToken token) noexcept
{
	if (!token_is_active(token)) {
		return;
	}
	if (m_temporary_file != nullptr) {
		std::fclose(m_temporary_file);
		m_temporary_file = nullptr;
	}
	remove_native_file(m_temporary_path);
	clear_temporary_state();
}

bool NativeProducerProfileStore::token_is_active(ProfileTempToken token) const noexcept
{
	return token.value != 0U && token.value == m_active_token;
}

void NativeProducerProfileStore::clear_temporary_state() noexcept
{
	m_temporary_file = nullptr;
	m_temporary_path.clear();
	m_active_token = 0;
	m_temporary_flushed = false;
}

IdentityResult parse_producer_profile_json(std::string_view input) noexcept
{
	if (input.size() > MaximumProducerProfileBytes) {
		return identity_failure(IdentityError::ProfileTooLarge);
	}
	if (input.find('\0') != std::string_view::npos) {
		return identity_failure(IdentityError::InvalidJson);
	}
	if (preflight_json_container_depth(input, MaximumProfileJsonContainerDepth) ==
		JsonDepthPreflightResult::MaximumDepthExceeded) {
		return identity_failure(IdentityError::MaximumDepthExceeded);
	}

	json_error_t parse_error{};
	const auto* data = input.empty() ? "" : input.data();
	std::unique_ptr<json_t> root(json_loadb(data, input.size(), JSON_REJECT_DUPLICATES, &parse_error));
	if (!root || !json_is_object(root.get())) {
		return identity_failure(IdentityError::InvalidJson);
	}

	void* iterator = json_object_iter(root.get());
	while (iterator != nullptr) {
		if (!is_known_profile_key(json_object_iter_key(iterator))) {
			return identity_failure(IdentityError::InvalidSchema);
		}
		iterator = json_object_iter_next(root.get(), iterator);
	}

	const auto* schema_version = json_object_get(root.get(), "schemaVersion");
	const auto* producer_id_value = json_object_get(root.get(), "producerId");
	if (!json_is_integer(schema_version) || json_integer_value(schema_version) != 1 || producer_id_value == nullptr) {
		return identity_failure(IdentityError::InvalidSchema);
	}

	std::uint64_t producer_id = 0;
	if (!parse_canonical_producer_id(producer_id_value, producer_id)) {
		return identity_failure(IdentityError::InvalidProducerId);
	}
	return IdentityResult{producer_id, IdentityError::None};
}

IdentityResult load_or_create_producer_identity(ProducerProfileStore& store, RandomSource& random) noexcept
{
	try {
		std::string profile_bytes;
		const auto initial_status = store.read(profile_bytes);
		if (initial_status == ProfileReadStatus::Present) {
			return parse_producer_profile_json(profile_bytes);
		}
		if (initial_status != ProfileReadStatus::Absent) {
			return identity_failure(IdentityError::ProfileReadFailure);
		}

		std::uint64_t generated_id = 0;
		if (!random.next_u64(generated_id) || generated_id == 0U) {
			return identity_failure(IdentityError::EntropyFailure);
		}

		std::array<char, 96> serialized{};
		std::size_t serialized_size = 0;
		if (!serialize_producer_profile(generated_id, serialized, serialized_size)) {
			return identity_failure(IdentityError::InvalidProducerId);
		}

		ProfileTempToken token;
		if (!store.create_temporary_in_profile_directory(token)) {
			return identity_failure(IdentityError::TemporaryCreateFailure);
		}
		if (!store.write_temporary(token, std::string_view{serialized.data(), serialized_size})) {
			store.abort_temporary(token);
			return identity_failure(IdentityError::TemporaryWriteFailure);
		}
		if (!store.flush_temporary_durably(token)) {
			store.abort_temporary(token);
			return identity_failure(IdentityError::TemporaryFlushFailure);
		}
		if (!store.close_temporary(token)) {
			store.abort_temporary(token);
			return identity_failure(IdentityError::TemporaryCloseFailure);
		}

		const auto install_status = store.install_if_absent_atomically(token);
		if (install_status != ProfileInstallStatus::Published) {
			store.abort_temporary(token);
		}

		profile_bytes.clear();
		const auto final_status = store.read(profile_bytes);
		if (install_status == ProfileInstallStatus::Error) {
			return identity_failure(IdentityError::AtomicInstallFailure);
		}
		if (final_status != ProfileReadStatus::Present) {
			return identity_failure(IdentityError::FinalReadFailure);
		}
		const auto persisted = parse_producer_profile_json(profile_bytes);
		if (persisted.error != IdentityError::None) {
			return identity_failure(IdentityError::FinalReadFailure);
		}
		if (install_status == ProfileInstallStatus::Published && persisted.producer_id != generated_id) {
			return identity_failure(IdentityError::FinalIdentityMismatch);
		}
		return persisted;
	} catch (...) {
		return identity_failure(IdentityError::ProfileReadFailure);
	}
}

SessionIdCandidateResult draw_session_id_candidate(RandomSource& random) noexcept
{
	std::uint64_t candidate = 0U;
	if (!random.next_u64(candidate) || candidate == 0U) {
		return SessionIdCandidateResult{SessionIdCandidateStatus::EntropyFailure, 0U};
	}
	return SessionIdCandidateResult{SessionIdCandidateStatus::Ready, candidate};
}

bool SessionIdRegistry::allocate_storage() noexcept
{
	if (storage_ready()) {
		return true;
	}
	auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage{});
	if (!storage) {
		return false;
	}
	m_used_ids = std::move(storage);
	m_used_count = 0U;
	return true;
}

void SessionIdRegistry::release_storage() noexcept
{
	m_used_ids.reset();
	m_used_count = 0U;
}

SessionIdRegistrationStatus SessionIdRegistry::register_candidate(std::uint64_t candidate) noexcept
{
	static_assert((StorageSlotCount & (StorageSlotCount - 1U)) == 0U,
		"Session ID hash table must be a power of two");
	if (candidate == 0U) {
		return SessionIdRegistrationStatus::InvalidCandidate;
	}
	if (!storage_ready()) {
		return SessionIdRegistrationStatus::StorageUnavailable;
	}

	const auto start = hash_session_id(candidate) & (StorageSlotCount - 1U);
	for (std::size_t probe = 0; probe < StorageSlotCount; ++probe) {
		auto& slot = (*m_used_ids)[(start + probe) & (StorageSlotCount - 1U)];
		if (slot == candidate) {
			return SessionIdRegistrationStatus::Duplicate;
		}
		if (slot == 0U) {
			if (m_used_count >= MaximumSessionIdsPerProcess) {
				return SessionIdRegistrationStatus::Capacity;
			}
			slot = candidate;
			++m_used_count;
			return SessionIdRegistrationStatus::Registered;
		}
	}
	return SessionIdRegistrationStatus::Capacity;
}

SessionIdAllocator::SessionIdAllocator(RandomSource& random, SessionIdRegistry& registry) noexcept
	: m_random(random), m_registry(registry)
{
}

SessionIdResult SessionIdAllocator::allocate() noexcept
{
	if (!m_registry.storage_ready()) {
		return SessionIdResult{SessionIdStatus::StorageUnavailable, 0U};
	}
	if (m_registry.used_count() >= MaximumSessionIdsPerProcess) {
		return SessionIdResult{SessionIdStatus::CapacityExhausted, 0U};
	}

	for (std::size_t collision = 0; collision < MaximumSessionIdCollisionDraws; ++collision) {
		const auto candidate = draw_session_id_candidate(m_random);
		if (candidate.status != SessionIdCandidateStatus::Ready) {
			return SessionIdResult{SessionIdStatus::EntropyFailure, 0U};
		}

		switch (m_registry.register_candidate(candidate.session_id)) {
		case SessionIdRegistrationStatus::Registered:
			return SessionIdResult{SessionIdStatus::Allocated, candidate.session_id};
		case SessionIdRegistrationStatus::Duplicate:
			break;
		case SessionIdRegistrationStatus::StorageUnavailable:
			return SessionIdResult{SessionIdStatus::StorageUnavailable, 0U};
		case SessionIdRegistrationStatus::Capacity:
			return SessionIdResult{SessionIdStatus::CapacityExhausted, 0U};
		case SessionIdRegistrationStatus::InvalidCandidate:
			return SessionIdResult{SessionIdStatus::EntropyFailure, 0U};
		}
	}
	return SessionIdResult{SessionIdStatus::RetryLimit, 0U};
}

} // namespace telemetry::detail
