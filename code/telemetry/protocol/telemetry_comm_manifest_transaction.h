#pragma once

#include "telemetry/protocol/telemetry_specialized_views.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace telemetry::protocol {

struct CommAssetCatalogDescriptor {
	std::uint16_t bundle_version = 0;
	std::uint16_t manifest_flags = ManifestFlagNone;
	Sha256Digest bundle_hash{};
	std::uint64_t frame_asset_id = 0;
	std::uint64_t placeholder_asset_id = 0;
	std::uint32_t asset_count = 0;
};

enum class CommManifestTransactionState : std::uint8_t {
	Empty = 0,
	Collecting = 1,
	Complete = 2,
	Failed = 3,
};

// Validates the invariants that span several COMM_ASSET_MANIFEST records.
// The candidate is never observable as a catalog until finalize() succeeds.
// Memory is reserved only after total_asset_count has been validated against
// the protocol maximum, and retained paths are bounded by that count and the
// 1024-byte entry limit.
class CommManifestTransaction final {
  public:
	CommManifestTransaction() = default;
	CommManifestTransaction(const CommManifestTransaction&) = delete;
	CommManifestTransaction& operator=(const CommManifestTransaction&) = delete;

	void reset() noexcept;
	ValidationError ingest_record(ByteView payload) noexcept;
	ValidationError finalize(CommAssetCatalogDescriptor& catalog) noexcept;

	CommManifestTransactionState state() const noexcept
	{
		return m_state;
	}
	std::uint32_t received_asset_count() const noexcept
	{
		return m_next_asset_index;
	}
	std::uint32_t expected_asset_count() const noexcept
	{
		return m_total_asset_count;
	}

  private:
	struct EntryIdentity {
		std::uint64_t asset_id = 0;
		std::string casefolded_path;
	};

	ValidationError fail(ValidationError error) noexcept;
	bool common_header_matches(const CommAssetManifestPayloadView& payload) const noexcept;

	CommManifestTransactionState m_state = CommManifestTransactionState::Empty;
	std::uint16_t m_bundle_version = 0;
	std::uint16_t m_manifest_flags = ManifestFlagNone;
	Sha256Digest m_bundle_hash{};
	std::uint64_t m_frame_asset_id = 0;
	std::uint64_t m_placeholder_asset_id = 0;
	std::uint32_t m_total_asset_count = 0;
	std::uint32_t m_next_asset_index = 0;
	std::string m_converter_id;
	std::string m_converter_version;
	std::vector<std::uint64_t> m_asset_ids;
	std::set<std::string, std::less<>> m_casefolded_paths;
};

} // namespace telemetry::protocol
