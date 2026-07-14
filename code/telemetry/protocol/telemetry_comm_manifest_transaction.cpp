#include "telemetry/protocol/telemetry_comm_manifest_transaction.h"

#include <algorithm>
#include <iterator>
#include <new>
#include <utility>

namespace telemetry::protocol {

namespace {

std::string byte_string(ByteView bytes)
{
	return std::string(reinterpret_cast<const char*>(bytes.data), bytes.size);
}

bool byte_equals_string(ByteView bytes, const std::string& value) noexcept
{
	if (bytes.size != value.size()) {
		return false;
	}
	for (std::size_t index = 0; index < bytes.size; ++index) {
		if (bytes.data[index] != static_cast<std::uint8_t>(value[index])) {
			return false;
		}
	}
	return true;
}

std::string ascii_casefolded(ByteView bytes)
{
	std::string result;
	result.resize(bytes.size);
	for (std::size_t index = 0; index < bytes.size; ++index) {
		auto value = bytes.data[index];
		if (value >= static_cast<std::uint8_t>('A') && value <= static_cast<std::uint8_t>('Z')) {
			value = static_cast<std::uint8_t>(value - static_cast<std::uint8_t>('A') +
				static_cast<std::uint8_t>('a'));
		}
		result[index] = static_cast<char>(value);
	}
	return result;
}

} // namespace

void CommManifestTransaction::reset() noexcept
{
	m_state = CommManifestTransactionState::Empty;
	m_bundle_version = 0;
	m_manifest_flags = ManifestFlagNone;
	m_bundle_hash = {};
	m_frame_asset_id = 0;
	m_placeholder_asset_id = 0;
	m_total_asset_count = 0;
	m_next_asset_index = 0;
	m_converter_id.clear();
	m_converter_version.clear();
	m_asset_ids.clear();
	m_casefolded_paths.clear();
}

ValidationError CommManifestTransaction::fail(ValidationError error) noexcept
{
	m_state = CommManifestTransactionState::Failed;
	return error;
}

bool CommManifestTransaction::common_header_matches(const CommAssetManifestPayloadView& payload) const noexcept
{
	return payload.bundle_version == m_bundle_version && payload.manifest_flags == m_manifest_flags &&
		   payload.bundle_hash == m_bundle_hash && payload.frame_asset_id == m_frame_asset_id &&
		   payload.placeholder_asset_id == m_placeholder_asset_id &&
		   payload.total_asset_count == m_total_asset_count &&
		   byte_equals_string(payload.converter_id, m_converter_id) &&
		   byte_equals_string(payload.converter_version, m_converter_version);
}

ValidationError CommManifestTransaction::ingest_record(ByteView input) noexcept
{
	if (m_state == CommManifestTransactionState::Failed || m_state == CommManifestTransactionState::Complete) {
		return ValidationError::InvalidStateTransition;
	}
	CommAssetManifestPayloadView payload;
	if (const auto error = decode_comm_asset_manifest_payload(input, payload); error != ValidationError::None) {
		return fail(error);
	}

	try {
		if (m_state == CommManifestTransactionState::Empty) {
			if (payload.first_asset_index != 0 || payload.total_asset_count == 0 ||
				payload.total_asset_count > MaximumCommAssetCount) {
				return fail(ValidationError::InvalidStateTransition);
			}
			m_bundle_version = payload.bundle_version;
			m_manifest_flags = payload.manifest_flags;
			m_bundle_hash = payload.bundle_hash;
			m_frame_asset_id = payload.frame_asset_id;
			m_placeholder_asset_id = payload.placeholder_asset_id;
			m_total_asset_count = payload.total_asset_count;
			m_converter_id = byte_string(payload.converter_id);
			m_converter_version = byte_string(payload.converter_version);
			m_asset_ids.reserve(m_total_asset_count);
			m_state = CommManifestTransactionState::Collecting;
		} else if (!common_header_matches(payload)) {
			return fail(ValidationError::InvalidStateTransition);
		}

		if (payload.first_asset_index != m_next_asset_index ||
			payload.entry_count > m_total_asset_count - m_next_asset_index) {
			return fail(ValidationError::InvalidStateTransition);
		}

		std::vector<EntryIdentity> additions;
		additions.reserve(payload.entry_count);
		std::size_t offset = 0;
		for (std::uint16_t index = 0; index < payload.entry_count; ++index) {
			CommAssetEntryView entry;
			std::size_t consumed = 0;
			if (const auto error = decode_comm_asset_entry(
					payload.encoded_entries.subview(offset, payload.encoded_entries.size - offset), entry, consumed);
				error != ValidationError::None) {
				return fail(error);
			}
			EntryIdentity identity;
			identity.asset_id = entry.asset_id;
			identity.casefolded_path = ascii_casefolded(entry.file_path);

			// The record decoder has already established strict ordering and
			// uniqueness inside this chunk. Binary/tree lookup preserves the
			// cross-chunk DuplicateItemKey priority without rescanning the catalog.
			if (std::binary_search(m_asset_ids.begin(), m_asset_ids.end(), identity.asset_id) ||
				m_casefolded_paths.find(identity.casefolded_path) != m_casefolded_paths.end()) {
				return fail(ValidationError::DuplicateItemKey);
			}
			if ((!m_asset_ids.empty() && additions.empty() && identity.asset_id <= m_asset_ids.back()) ||
				(!additions.empty() && identity.asset_id <= additions.back().asset_id)) {
				return fail(ValidationError::InvalidStateTransition);
			}
			additions.push_back(std::move(identity));
			offset += consumed;
		}
		if (offset != payload.encoded_entries.size) {
			return fail(ValidationError::TrailingBytes);
		}
		for (auto& addition : additions) {
			const auto inserted = m_casefolded_paths.insert(std::move(addition.casefolded_path));
			if (!inserted.second) {
				return fail(ValidationError::DuplicateItemKey);
			}
			m_asset_ids.push_back(addition.asset_id);
		}
		m_next_asset_index += payload.entry_count;
		return ValidationError::None;
	} catch (const std::bad_alloc&) {
		return fail(ValidationError::ResourceLimit);
	}
}

ValidationError CommManifestTransaction::finalize(CommAssetCatalogDescriptor& catalog) noexcept
{
	if (m_state == CommManifestTransactionState::Failed) {
		return ValidationError::InvalidStateTransition;
	}
	if (m_state != CommManifestTransactionState::Collecting || m_next_asset_index != m_total_asset_count ||
		m_asset_ids.size() != m_total_asset_count || m_casefolded_paths.size() != m_total_asset_count) {
		return ValidationError::MissingManifest;
	}
	const auto contains = [&](std::uint64_t asset_id) {
		return asset_id == 0 || std::binary_search(m_asset_ids.begin(), m_asset_ids.end(), asset_id);
	};
	if (!contains(m_frame_asset_id) || !contains(m_placeholder_asset_id)) {
		return fail(ValidationError::UnknownEntity);
	}

	CommAssetCatalogDescriptor candidate;
	candidate.bundle_version = m_bundle_version;
	candidate.manifest_flags = m_manifest_flags;
	candidate.bundle_hash = m_bundle_hash;
	candidate.frame_asset_id = m_frame_asset_id;
	candidate.placeholder_asset_id = m_placeholder_asset_id;
	candidate.asset_count = m_total_asset_count;
	catalog = candidate;
	m_state = CommManifestTransactionState::Complete;
	return ValidationError::None;
}

} // namespace telemetry::protocol
