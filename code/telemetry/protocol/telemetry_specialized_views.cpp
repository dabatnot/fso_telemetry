#include "telemetry/protocol/telemetry_specialized_views.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <set>
#include <string_view>

namespace telemetry::protocol {

namespace {

ValidationError validate_exact_input(ByteView input, std::size_t expected_size) noexcept
{
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < expected_size) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > expected_size) {
		return ValidationError::TrailingBytes;
	}
	return ValidationError::None;
}

template <std::size_t Size>
ValidationError publish_fixed(const std::array<std::uint8_t, Size>& encoded,
	MutableByteView output,
	std::size_t& written) noexcept
{
	if (output.data == nullptr || output.size < encoded.size()) {
		return ValidationError::InternalSerializationError;
	}
	std::memcpy(output.data, encoded.data(), encoded.size());
	written = encoded.size();
	return ValidationError::None;
}

template <std::size_t PrefixSize>
ValidationError publish_with_suffix(const std::array<std::uint8_t, PrefixSize>& prefix,
	ByteView suffix,
	MutableByteView output,
	std::size_t& written) noexcept
{
	if (suffix.size > std::numeric_limits<std::size_t>::max() - PrefixSize) {
		return ValidationError::InternalSerializationError;
	}
	const auto total_size = PrefixSize + suffix.size;
	if (output.data == nullptr || output.size < total_size) {
		return ValidationError::InternalSerializationError;
	}
	if (suffix.size != 0) {
		std::memmove(output.data + PrefixSize, suffix.data, suffix.size);
	}
	std::memcpy(output.data, prefix.data(), prefix.size());
	written = total_size;
	return ValidationError::None;
}

template <std::size_t Size>
bool finish_writer(const PacketWriter& writer) noexcept
{
	return writer.ok() && writer.size() == Size;
}

bool digest_is_zero(const Sha256Digest& digest) noexcept
{
	for (const auto byte : digest) {
		if (byte != 0) {
			return false;
		}
	}
	return true;
}

bool is_known_comm_negotiation_result(CommNegotiationResult result) noexcept
{
	switch (result) {
	case CommNegotiationResult::NotRequested:
	case CommNegotiationResult::Accepted:
	case CommNegotiationResult::SourceUnavailable:
	case CommNegotiationResult::BundleAbsent:
	case CommNegotiationResult::BundleVersionMismatch:
	case CommNegotiationResult::BundleHashMismatch:
	case CommNegotiationResult::NoCommonFormat:
	case CommNegotiationResult::ManifestInvalid:
		return true;
	}
	return false;
}

bool comm_selection_requires_bundle_details(CommNegotiationResult result) noexcept
{
	return result == CommNegotiationResult::Accepted || result == CommNegotiationResult::BundleAbsent ||
		   result == CommNegotiationResult::BundleVersionMismatch ||
		   result == CommNegotiationResult::BundleHashMismatch || result == CommNegotiationResult::NoCommonFormat;
}

bool is_known_comm_playback_mode(CommPlaybackMode mode) noexcept
{
	return mode == CommPlaybackMode::Once || mode == CommPlaybackMode::Loop;
}

bool is_known_comm_color_mode(CommColorMode mode) noexcept
{
	return mode == CommColorMode::HudTint || mode == CommColorMode::FullColor;
}

bool is_known_comm_stop_reason(CommStopReason reason) noexcept
{
	switch (reason) {
	case CommStopReason::None:
	case CommStopReason::Completed:
	case CommStopReason::Interrupted:
	case CommStopReason::Replaced:
	case CommStopReason::HudDisabled:
	case CommStopReason::MissionChanged:
	case CommStopReason::SessionStopped:
		return true;
	}
	return false;
}

bool is_known_comm_event_kind(CommEventKind kind) noexcept
{
	return kind == CommEventKind::Start || kind == CommEventKind::Stop;
}

ValidationError validate_comm_modes(CommPlaybackMode playback_mode,
	CommColorMode color_mode,
	CommStopReason stop_reason) noexcept
{
	if (!is_known_comm_playback_mode(playback_mode) || !is_known_comm_color_mode(color_mode) ||
		!is_known_comm_stop_reason(stop_reason)) {
		return ValidationError::UnknownEnum;
	}
	return ValidationError::None;
}

ValidationError validate_active_comm_values(std::uint64_t playback_id,
	std::uint64_t head_asset_id,
	std::uint64_t animation_time_us,
	std::uint64_t duration_us,
	float playback_rate) noexcept
{
	if (playback_id == 0 || head_asset_id == 0 || duration_us == 0 || duration_us > MaximumCommDurationUs ||
		animation_time_us > duration_us) {
		return ValidationError::OutOfRange;
	}
	if (!std::isfinite(playback_rate)) {
		return ValidationError::NonFiniteFloat;
	}
	if (playback_rate < MinimumCommPlaybackRate || playback_rate > MaximumCommPlaybackRate) {
		return ValidationError::OutOfRange;
	}
	return ValidationError::None;
}

bool is_canonical_zero(float value) noexcept
{
	return value == 0.0F;
}

bool has_four_byte_start_code(ByteView bytes, std::size_t offset) noexcept
{
	return offset <= bytes.size && bytes.size - offset >= 4 && bytes.data[offset] == 0 &&
		   bytes.data[offset + 1] == 0 && bytes.data[offset + 2] == 0 && bytes.data[offset + 3] == 1;
}

bool has_three_byte_start_code(ByteView bytes, std::size_t offset) noexcept
{
	return offset <= bytes.size && bytes.size - offset >= 3 && bytes.data[offset] == 0 &&
		   bytes.data[offset + 1] == 0 && bytes.data[offset + 2] == 1;
}

// Reads an H.264 RBSP directly from an EBSP without allocating a second copy.
// The raw zero counter makes emulation-prevention validation local and bounded:
// 00 00 03 xx is accepted only for xx in 00..03, while unescaped 00..02 is
// rejected before it can be interpreted as syntax.
class RbspBitReader final {
  public:
	explicit RbspBitReader(ByteView ebsp) noexcept : m_ebsp(ebsp) {}

	bool read_bits(std::uint8_t count, std::uint32_t& value) noexcept
	{
		value = 0;
		if (count > 32U) {
			m_malformed = true;
			return false;
		}
		for (std::uint8_t index = 0; index < count; ++index) {
			std::uint32_t bit = 0;
			if (!read_bit(bit)) {
				return false;
			}
			value = static_cast<std::uint32_t>((value << 1U) | bit);
		}
		return true;
	}

	bool read_flag(bool& value) noexcept
	{
		std::uint32_t raw = 0;
		if (!read_bit(raw)) {
			return false;
		}
		value = raw != 0;
		return true;
	}

	bool read_ue(std::uint32_t& value) noexcept
	{
		std::uint32_t leading_zero_bits = 0;
		for (;;) {
			std::uint32_t bit = 0;
			if (!read_bit(bit)) {
				return false;
			}
			if (bit != 0) {
				break;
			}
			if (++leading_zero_bits > 31U) {
				m_malformed = true;
				return false;
			}
		}

		std::uint32_t suffix = 0;
		if (leading_zero_bits != 0 && !read_bits(static_cast<std::uint8_t>(leading_zero_bits), suffix)) {
			return false;
		}
		const auto decoded = ((std::uint64_t{1} << leading_zero_bits) - 1U) + suffix;
		if (decoded > std::numeric_limits<std::uint32_t>::max()) {
			m_malformed = true;
			return false;
		}
		value = static_cast<std::uint32_t>(decoded);
		return true;
	}

	bool read_se(std::int32_t& value) noexcept
	{
		std::uint32_t code_num = 0;
		if (!read_ue(code_num)) {
			return false;
		}
		const std::int64_t magnitude = (static_cast<std::int64_t>(code_num) + 1) / 2;
		const std::int64_t decoded = (code_num & 1U) != 0 ? magnitude : -magnitude;
		if (decoded < std::numeric_limits<std::int32_t>::min() ||
			decoded > std::numeric_limits<std::int32_t>::max()) {
			m_malformed = true;
			return false;
		}
		value = static_cast<std::int32_t>(decoded);
		return true;
	}

	bool read_rbsp_trailing_bits() noexcept
	{
		std::uint32_t stop_bit = 0;
		if (!read_bit(stop_bit)) {
			return false;
		}
		if (stop_bit != 1U) {
			m_malformed = true;
			return false;
		}
		while (m_bits_remaining != 0) {
			std::uint32_t padding_bit = 0;
			if (!read_bit(padding_bit)) {
				return false;
			}
			if (padding_bit != 0) {
				m_malformed = true;
				return false;
			}
		}
		if (m_position != m_ebsp.size) {
			m_malformed = true;
			return false;
		}
		return true;
	}

	bool malformed() const noexcept
	{
		return m_malformed;
	}

  private:
	bool read_bit(std::uint32_t& value) noexcept
	{
		if (m_bits_remaining == 0 && !load_byte()) {
			return false;
		}
		value = (m_current_byte >> (m_bits_remaining - 1U)) & 1U;
		--m_bits_remaining;
		return true;
	}

	bool load_byte() noexcept
	{
		while (m_position < m_ebsp.size) {
			const auto byte = m_ebsp.data[m_position++];
			if (m_raw_zero_count >= 2U) {
				if (byte == 0x03U) {
					if (m_position >= m_ebsp.size || m_ebsp.data[m_position] > 0x03U) {
						m_malformed = true;
						return false;
					}
					m_raw_zero_count = 0;
					continue;
				}
				if (byte <= 0x02U) {
					m_malformed = true;
					return false;
				}
			}

			m_raw_zero_count = byte == 0 ? std::min<std::uint8_t>(2U, m_raw_zero_count + 1U) : 0U;
			m_current_byte = byte;
			m_bits_remaining = 8;
			return true;
		}
		return false;
	}

	ByteView m_ebsp;
	std::size_t m_position = 0;
	std::uint8_t m_current_byte = 0;
	std::uint8_t m_bits_remaining = 0;
	std::uint8_t m_raw_zero_count = 0;
	bool m_malformed = false;
};

ValidationError rbsp_read_error(const RbspBitReader& reader) noexcept
{
	return reader.malformed() ? ValidationError::InvalidStateTransition : ValidationError::TruncatedPayload;
}

ValidationError parse_scaling_list(RbspBitReader& reader, std::size_t size) noexcept
{
	std::int32_t last_scale = 8;
	std::int32_t next_scale = 8;
	for (std::size_t index = 0; index < size; ++index) {
		if (next_scale != 0) {
			std::int32_t delta_scale = 0;
			if (!reader.read_se(delta_scale)) {
				return rbsp_read_error(reader);
			}
			const auto sum = static_cast<std::int64_t>(last_scale) + delta_scale;
			next_scale = static_cast<std::int32_t>((sum % 256 + 256) % 256);
		}
		last_scale = next_scale == 0 ? last_scale : next_scale;
	}
	return ValidationError::None;
}

ValidationError parse_hrd_parameters(RbspBitReader& reader) noexcept
{
	std::uint32_t cpb_count_minus_one = 0;
	std::uint32_t ignored = 0;
	if (!reader.read_ue(cpb_count_minus_one)) {
		return rbsp_read_error(reader);
	}
	if (cpb_count_minus_one > 31U) {
		return ValidationError::OutOfRange;
	}
	if (!reader.read_bits(4, ignored) || !reader.read_bits(4, ignored)) {
		return rbsp_read_error(reader);
	}
	for (std::uint32_t index = 0; index <= cpb_count_minus_one; ++index) {
		bool cbr_flag = false;
		if (!reader.read_ue(ignored) || !reader.read_ue(ignored) || !reader.read_flag(cbr_flag)) {
			return rbsp_read_error(reader);
		}
	}
	for (std::size_t index = 0; index < 4U; ++index) {
		if (!reader.read_bits(5, ignored)) {
			return rbsp_read_error(reader);
		}
	}
	return ValidationError::None;
}

ValidationError parse_and_validate_vui(RbspBitReader& reader) noexcept
{
	bool present = false;
	std::uint32_t value = 0;
	if (!reader.read_flag(present)) {
		return rbsp_read_error(reader);
	}
	if (present) {
		if (!reader.read_bits(8, value)) {
			return rbsp_read_error(reader);
		}
		if (value == 255U) {
			std::uint32_t sar_width = 0;
			std::uint32_t sar_height = 0;
			if (!reader.read_bits(16, sar_width) || !reader.read_bits(16, sar_height)) {
				return rbsp_read_error(reader);
			}
			if (sar_width == 0 || sar_height == 0) {
				return ValidationError::OutOfRange;
			}
		}
	}

	if (!reader.read_flag(present)) {
		return rbsp_read_error(reader);
	}
	if (present) {
		bool ignored_flag = false;
		if (!reader.read_flag(ignored_flag)) {
			return rbsp_read_error(reader);
		}
	}

	bool video_signal_type_present = false;
	if (!reader.read_flag(video_signal_type_present)) {
		return rbsp_read_error(reader);
	}
	if (!video_signal_type_present) {
		return ValidationError::InvalidAbsence;
	}
	bool full_range = false;
	bool colour_description_present = false;
	if (!reader.read_bits(3, value) || !reader.read_flag(full_range) ||
		!reader.read_flag(colour_description_present)) {
		return rbsp_read_error(reader);
	}
	if (full_range || !colour_description_present) {
		return ValidationError::InvalidStateTransition;
	}
	std::uint32_t colour_primaries = 0;
	std::uint32_t transfer_characteristics = 0;
	std::uint32_t matrix_coefficients = 0;
	if (!reader.read_bits(8, colour_primaries) || !reader.read_bits(8, transfer_characteristics) ||
		!reader.read_bits(8, matrix_coefficients)) {
		return rbsp_read_error(reader);
	}
	if (colour_primaries != 1U || transfer_characteristics != 1U || matrix_coefficients != 1U) {
		return ValidationError::InvalidStateTransition;
	}

	if (!reader.read_flag(present)) {
		return rbsp_read_error(reader);
	}
	if (present) {
		std::uint32_t top = 0;
		std::uint32_t bottom = 0;
		if (!reader.read_ue(top) || !reader.read_ue(bottom)) {
			return rbsp_read_error(reader);
		}
		if (top > 5U || bottom > 5U) {
			return ValidationError::OutOfRange;
		}
	}

	if (!reader.read_flag(present)) {
		return rbsp_read_error(reader);
	}
	if (present) {
		std::uint32_t num_units_in_tick = 0;
		std::uint32_t time_scale = 0;
		bool fixed_frame_rate = false;
		if (!reader.read_bits(32, num_units_in_tick) || !reader.read_bits(32, time_scale) ||
			!reader.read_flag(fixed_frame_rate)) {
			return rbsp_read_error(reader);
		}
		if (num_units_in_tick == 0 || time_scale == 0) {
			return ValidationError::OutOfRange;
		}
	}

	bool nal_hrd_present = false;
	bool vcl_hrd_present = false;
	if (!reader.read_flag(nal_hrd_present)) {
		return rbsp_read_error(reader);
	}
	if (nal_hrd_present) {
		if (const auto error = parse_hrd_parameters(reader); error != ValidationError::None) {
			return error;
		}
	}
	if (!reader.read_flag(vcl_hrd_present)) {
		return rbsp_read_error(reader);
	}
	if (vcl_hrd_present) {
		if (const auto error = parse_hrd_parameters(reader); error != ValidationError::None) {
			return error;
		}
	}
	if (nal_hrd_present || vcl_hrd_present) {
		bool low_delay_hrd = false;
		if (!reader.read_flag(low_delay_hrd)) {
			return rbsp_read_error(reader);
		}
	}

	bool pic_struct_present = false;
	if (!reader.read_flag(pic_struct_present)) {
		return rbsp_read_error(reader);
	}
	bool bitstream_restriction_present = false;
	if (!reader.read_flag(bitstream_restriction_present)) {
		return rbsp_read_error(reader);
	}
	if (bitstream_restriction_present) {
		bool motion_vectors_over_picture_boundaries = false;
		if (!reader.read_flag(motion_vectors_over_picture_boundaries)) {
			return rbsp_read_error(reader);
		}
		for (std::size_t index = 0; index < 6U; ++index) {
			if (!reader.read_ue(value)) {
				return rbsp_read_error(reader);
			}
		}
	}
	return ValidationError::None;
}

struct ParsedSps {
	std::uint16_t displayed_width = 0;
	std::uint16_t displayed_height = 0;
};

ValidationError parse_and_validate_sps(ByteView ebsp,
	const TargetVideoConfigPayload& config,
	ParsedSps& parsed) noexcept
{
	parsed = ParsedSps{};
	if (ebsp.data == nullptr || ebsp.size == 0) {
		return ValidationError::TruncatedPayload;
	}
	RbspBitReader reader(ebsp);
	std::uint32_t profile_idc = 0;
	std::uint32_t constraint_flags = 0;
	std::uint32_t level_idc = 0;
	std::uint32_t sps_id = 0;
	if (!reader.read_bits(8, profile_idc) || !reader.read_bits(8, constraint_flags) ||
		!reader.read_bits(8, level_idc) || !reader.read_ue(sps_id)) {
		return rbsp_read_error(reader);
	}
	if ((constraint_flags & 0x03U) != 0) {
		return ValidationError::ReservedFlag;
	}
	if (profile_idc != static_cast<std::uint8_t>(config.codec_profile) ||
		level_idc != static_cast<std::uint8_t>(config.codec_level)) {
		return ValidationError::InvalidStateTransition;
	}
	if (profile_idc == static_cast<std::uint8_t>(H264Profile::ConstrainedBaseline) &&
		(constraint_flags & 0x40U) == 0) {
		return ValidationError::InvalidStateTransition;
	}
	if (sps_id > 31U) {
		return ValidationError::OutOfRange;
	}

	std::uint32_t chroma_format_idc = 1;
	if (profile_idc == static_cast<std::uint8_t>(H264Profile::High)) {
		std::uint32_t bit_depth_luma_minus8 = 0;
		std::uint32_t bit_depth_chroma_minus8 = 0;
		bool qpprime_y_zero_transform_bypass = false;
		bool scaling_matrix_present = false;
		if (!reader.read_ue(chroma_format_idc) || !reader.read_ue(bit_depth_luma_minus8) ||
			!reader.read_ue(bit_depth_chroma_minus8) ||
			!reader.read_flag(qpprime_y_zero_transform_bypass) ||
			!reader.read_flag(scaling_matrix_present)) {
			return rbsp_read_error(reader);
		}
		if (chroma_format_idc != 1U || bit_depth_luma_minus8 != 0 || bit_depth_chroma_minus8 != 0) {
			return ValidationError::InvalidStateTransition;
		}
		if (scaling_matrix_present) {
			for (std::size_t index = 0; index < 8U; ++index) {
				bool list_present = false;
				if (!reader.read_flag(list_present)) {
					return rbsp_read_error(reader);
				}
				if (list_present) {
					if (const auto error = parse_scaling_list(reader, index < 6U ? 16U : 64U);
						error != ValidationError::None) {
						return error;
					}
				}
			}
		}
	} else if (profile_idc != static_cast<std::uint8_t>(H264Profile::ConstrainedBaseline) &&
		profile_idc != static_cast<std::uint8_t>(H264Profile::Main)) {
		return ValidationError::UnknownEnum;
	}

	std::uint32_t log2_max_frame_num_minus4 = 0;
	std::uint32_t pic_order_count_type = 0;
	if (!reader.read_ue(log2_max_frame_num_minus4) || !reader.read_ue(pic_order_count_type)) {
		return rbsp_read_error(reader);
	}
	if (log2_max_frame_num_minus4 > 12U || pic_order_count_type > 2U) {
		return ValidationError::OutOfRange;
	}
	if (pic_order_count_type == 0) {
		std::uint32_t log2_max_pic_order_count_lsb_minus4 = 0;
		if (!reader.read_ue(log2_max_pic_order_count_lsb_minus4)) {
			return rbsp_read_error(reader);
		}
		if (log2_max_pic_order_count_lsb_minus4 > 12U) {
			return ValidationError::OutOfRange;
		}
	} else if (pic_order_count_type == 1) {
		bool delta_pic_order_always_zero = false;
		std::int32_t ignored_offset = 0;
		std::uint32_t cycle_count = 0;
		if (!reader.read_flag(delta_pic_order_always_zero) || !reader.read_se(ignored_offset) ||
			!reader.read_se(ignored_offset) || !reader.read_ue(cycle_count)) {
			return rbsp_read_error(reader);
		}
		if (cycle_count > 255U) {
			return ValidationError::OutOfRange;
		}
		for (std::uint32_t index = 0; index < cycle_count; ++index) {
			if (!reader.read_se(ignored_offset)) {
				return rbsp_read_error(reader);
			}
		}
	}

	std::uint32_t max_num_ref_frames = 0;
	bool gaps_in_frame_num_allowed = false;
	std::uint32_t width_in_mbs_minus_one = 0;
	std::uint32_t height_in_map_units_minus_one = 0;
	bool frame_mbs_only = false;
	bool direct_8x8_inference = false;
	bool cropping_present = false;
	if (!reader.read_ue(max_num_ref_frames) || !reader.read_flag(gaps_in_frame_num_allowed) ||
		!reader.read_ue(width_in_mbs_minus_one) || !reader.read_ue(height_in_map_units_minus_one) ||
		!reader.read_flag(frame_mbs_only)) {
		return rbsp_read_error(reader);
	}
	if (!frame_mbs_only) {
		return ValidationError::InvalidStateTransition;
	}
	if (!reader.read_flag(direct_8x8_inference) || !reader.read_flag(cropping_present)) {
		return rbsp_read_error(reader);
	}

	std::uint32_t crop_left = 0;
	std::uint32_t crop_right = 0;
	std::uint32_t crop_top = 0;
	std::uint32_t crop_bottom = 0;
	if (cropping_present &&
		(!reader.read_ue(crop_left) || !reader.read_ue(crop_right) || !reader.read_ue(crop_top) ||
			!reader.read_ue(crop_bottom))) {
		return rbsp_read_error(reader);
	}

	bool vui_present = false;
	if (!reader.read_flag(vui_present)) {
		return rbsp_read_error(reader);
	}
	if (!vui_present) {
		return ValidationError::InvalidAbsence;
	}
	if (const auto error = parse_and_validate_vui(reader); error != ValidationError::None) {
		return error;
	}
	if (!reader.read_rbsp_trailing_bits()) {
		return rbsp_read_error(reader);
	}

	const auto coded_width = (static_cast<std::uint64_t>(width_in_mbs_minus_one) + 1U) * 16U;
	const auto coded_height = (static_cast<std::uint64_t>(height_in_map_units_minus_one) + 1U) * 16U;
	const auto horizontal_crop = (static_cast<std::uint64_t>(crop_left) + crop_right) * 2U;
	const auto vertical_crop = (static_cast<std::uint64_t>(crop_top) + crop_bottom) * 2U;
	if (coded_width > std::numeric_limits<std::uint16_t>::max() ||
		coded_height > std::numeric_limits<std::uint16_t>::max() || horizontal_crop >= coded_width ||
		vertical_crop >= coded_height) {
		return ValidationError::OutOfRange;
	}
	const auto displayed_width = coded_width - horizontal_crop;
	const auto displayed_height = coded_height - vertical_crop;
	if (displayed_width != config.width || displayed_height != config.height) {
		return ValidationError::InvalidStateTransition;
	}
	parsed.displayed_width = static_cast<std::uint16_t>(displayed_width);
	parsed.displayed_height = static_cast<std::uint16_t>(displayed_height);
	return ValidationError::None;
}

bool is_even_video_dimension(std::uint16_t value) noexcept
{
	return value >= MinimumVideoDimension && value <= MaximumVideoDimension && (value & 1U) == 0;
}

bool is_known_video_config_result(VideoConfigResult result) noexcept
{
	switch (result) {
	case VideoConfigResult::Accepted:
	case VideoConfigResult::UnsupportedCapability:
	case VideoConfigResult::InvalidRequest:
	case VideoConfigResult::NoCommonProfile:
	case VideoConfigResult::NoCommonLevel:
	case VideoConfigResult::NoCommonRenderProfile:
	case VideoConfigResult::RequiredOverlaysMissing:
	case VideoConfigResult::ResourceLimit:
	case VideoConfigResult::TemporarilyUnavailable:
		return true;
	}
	return false;
}

std::uint32_t profile_bit(H264Profile profile) noexcept
{
	switch (profile) {
	case H264Profile::ConstrainedBaseline:
		return H264ProfileBitConstrainedBaseline;
	case H264Profile::Main:
		return H264ProfileBitMain;
	case H264Profile::High:
		return H264ProfileBitHigh;
	}
	return 0;
}

std::uint32_t level_bit(H264Level level) noexcept
{
	switch (level) {
	case H264Level::Level3_1:
		return H264LevelBitLevel3_1;
	case H264Level::Level3_2:
		return H264LevelBitLevel3_2;
	case H264Level::Level4_0:
		return H264LevelBitLevel4_0;
	case H264Level::Level4_1:
		return H264LevelBitLevel4_1;
	case H264Level::Level4_2:
		return H264LevelBitLevel4_2;
	case H264Level::Level5_0:
		return H264LevelBitLevel5_0;
	case H264Level::Level5_1:
		return H264LevelBitLevel5_1;
	case H264Level::Level5_2:
		return H264LevelBitLevel5_2;
	}
	return 0;
}

std::uint32_t render_profile_bit(RenderProfile profile) noexcept
{
	switch (profile) {
	case RenderProfile::MfdHigh:
		return RenderProfileBitMfdHigh;
	case RenderProfile::HudExact:
		return RenderProfileBitHudExact;
	default:
		return 0;
	}
}

bool is_known_video_keyframe_reason(VideoKeyframeReason reason) noexcept
{
	switch (reason) {
	case VideoKeyframeReason::PacketLoss:
	case VideoKeyframeReason::DecoderError:
	case VideoKeyframeReason::LateJoin:
	case VideoKeyframeReason::ConfigChanged:
	case VideoKeyframeReason::IdrRecoveryExpired:
		return true;
	}
	return false;
}

bool is_known_video_stop_reason(VideoStopReason reason) noexcept
{
	switch (reason) {
	case VideoStopReason::ClientUnsubscribe:
	case VideoStopReason::NoTarget:
	case VideoStopReason::UnsupportedTarget:
	case VideoStopReason::MissionChanged:
	case VideoStopReason::SessionStopped:
	case VideoStopReason::CapabilityWithdrawn:
	case VideoStopReason::EncoderFailed:
	case VideoStopReason::RendererUnavailable:
	case VideoStopReason::ResourceLimit:
		return true;
	}
	return false;
}

std::uint64_t integer_square_root(std::uint64_t value) noexcept
{
	std::uint64_t low = 0;
	std::uint64_t high = value < 0xffffffffULL ? value : 0xffffffffULL;
	while (low < high) {
		const auto middle = low + (high - low + 1U) / 2U;
		if (middle <= value / middle) {
			low = middle;
		} else {
			high = middle - 1U;
		}
	}
	return low;
}

bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result) noexcept
{
	if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
		return false;
	}
	result = lhs * rhs;
	return true;
}

bool is_ascii_letter(std::uint8_t value) noexcept
{
	return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool is_ascii_digit(std::uint8_t value) noexcept
{
	return value >= '0' && value <= '9';
}

bool is_portable_segment_edge(std::uint8_t value) noexcept
{
	return is_ascii_letter(value) || is_ascii_digit(value) || value == '_' || value == '-';
}

bool is_portable_segment_middle(std::uint8_t value) noexcept
{
	return is_portable_segment_edge(value) || value == '.';
}

std::uint8_t ascii_casefold(std::uint8_t value) noexcept
{
	return value >= 'A' && value <= 'Z' ? static_cast<std::uint8_t>(value + ('a' - 'A')) : value;
}

struct AsciiCasefoldLess {
	bool operator()(ByteView lhs, ByteView rhs) const noexcept
	{
		const auto common_size = std::min(lhs.size, rhs.size);
		for (std::size_t index = 0; index < common_size; ++index) {
			const auto lhs_byte = ascii_casefold(lhs.data[index]);
			const auto rhs_byte = ascii_casefold(rhs.data[index]);
			if (lhs_byte != rhs_byte) {
				return lhs_byte < rhs_byte;
			}
		}
		return lhs.size < rhs.size;
	}
};

bool segment_has_reserved_windows_stem(ByteView segment) noexcept
{
	std::size_t stem_length = 0;
	while (stem_length < segment.size && segment.data[stem_length] != '.') {
		++stem_length;
	}
	const ByteView stem{segment.data, stem_length};
	const auto equals_literal = [stem](const char* literal, std::size_t length) noexcept {
		if (stem.size != length) {
			return false;
		}
		for (std::size_t index = 0; index < length; ++index) {
			if (ascii_casefold(stem.data[index]) != static_cast<std::uint8_t>(literal[index])) {
				return false;
			}
		}
		return true;
	};
	if (equals_literal("con", 3) || equals_literal("prn", 3) || equals_literal("aux", 3) ||
		equals_literal("nul", 3)) {
		return true;
	}
	if (stem.size == 4 && stem.data[3] >= '1' && stem.data[3] <= '9') {
		return (ascii_casefold(stem.data[0]) == 'c' && ascii_casefold(stem.data[1]) == 'o' &&
				ascii_casefold(stem.data[2]) == 'm') ||
			   (ascii_casefold(stem.data[0]) == 'l' && ascii_casefold(stem.data[1]) == 'p' &&
				ascii_casefold(stem.data[2]) == 't');
	}
	return false;
}

bool is_known_source_format(SourceFormat format) noexcept
{
	return format == SourceFormat::Ani || format == SourceFormat::Eff || format == SourceFormat::Apng ||
		format == SourceFormat::StaticImage;
}

bool is_known_delivered_format(DeliveredFormat format) noexcept
{
	switch (format) {
	case DeliveredFormat::Ani:
	case DeliveredFormat::Eff:
	case DeliveredFormat::Apng:
	case DeliveredFormat::WebmNoAudio:
	case DeliveredFormat::Rgba8Atlas:
	case DeliveredFormat::Png:
		return true;
	default:
		return false;
	}
}

bool is_known_alpha_mode(AlphaMode mode) noexcept
{
	return mode == AlphaMode::None || mode == AlphaMode::Straight || mode == AlphaMode::Premultiplied;
}

bool is_known_asset_timing_mode(AssetTimingMode mode) noexcept
{
	return mode == AssetTimingMode::FormatIntrinsic || mode == AssetTimingMode::Constant ||
		mode == AssetTimingMode::PerFrame;
}

bool byte_view_ends_with(ByteView value, const char* suffix, std::size_t suffix_size) noexcept
{
	if (value.size < suffix_size) {
		return false;
	}
	for (std::size_t index = 0; index < suffix_size; ++index) {
		if (value.data[value.size - suffix_size + index] != static_cast<std::uint8_t>(suffix[index])) {
			return false;
		}
	}
	return true;
}

} // namespace

std::uint64_t comm_asset_id_from_content_hash(const Sha256Digest& content_hash) noexcept
{
	std::uint64_t asset_id = 0;
	for (std::size_t index = 0; index < sizeof(asset_id); ++index) {
		asset_id = (asset_id << 8U) | content_hash[index];
	}
	return asset_id;
}

ValidationError validate_portable_bundle_path(ByteView path) noexcept
{
	if (path.data == nullptr || path.size == 0 || path.size > MaximumCommFilePathLength) {
		return ValidationError::StringTooLong;
	}
	std::size_t segment_start = 0;
	while (segment_start < path.size) {
		std::size_t segment_end = segment_start;
		while (segment_end < path.size && path.data[segment_end] != '/') {
			++segment_end;
		}
		const auto segment_length = segment_end - segment_start;
		if (segment_length == 0 || segment_length > 255) {
			return ValidationError::OutOfRange;
		}
		const ByteView segment{path.data + segment_start, segment_length};
		if (!is_portable_segment_edge(segment.data[0]) ||
			!is_portable_segment_edge(segment.data[segment.size - 1])) {
			return ValidationError::OutOfRange;
		}
		for (const auto byte : segment) {
			if (byte > 0x7fU || !is_portable_segment_middle(byte)) {
				return ValidationError::InvalidUtf8;
			}
		}
		if (segment_has_reserved_windows_stem(segment)) {
			return ValidationError::OutOfRange;
		}
		if (segment_end == path.size) {
			return ValidationError::None;
		}
		segment_start = segment_end + 1U;
		if (segment_start == path.size) {
			return ValidationError::OutOfRange;
		}
	}
	return ValidationError::OutOfRange;
}

ValidationError decode_comm_asset_entry(ByteView input,
	CommAssetEntryView& entry,
	std::size_t& consumed) noexcept
{
	entry = CommAssetEntryView{};
	consumed = 0;
	if (input.data == nullptr || input.size < CommAssetEntryPrefixSize) {
		return ValidationError::TruncatedPayload;
	}
	const auto declared_length = static_cast<std::uint16_t>(
		static_cast<std::uint16_t>(input.data[0]) | (static_cast<std::uint16_t>(input.data[1]) << 8U));
	if (declared_length < CommAssetEntryPrefixSize) {
		return ValidationError::BadRecordLength;
	}
	if (declared_length > input.size) {
		return ValidationError::TruncatedPayload;
	}

	const ByteView exact_input{input.data, declared_length};
	CommAssetEntryView candidate;
	std::uint8_t raw_source_format = 0;
	std::uint8_t raw_delivered_format = 0;
	std::uint8_t raw_alpha_mode = 0;
	std::uint8_t raw_timing_mode = 0;
	std::uint16_t reserved = 0;
	std::uint16_t logical_name_length = 0;
	std::uint16_t file_path_length = 0;
	ByteView hash;
	PacketReader reader(exact_input);
	const bool prefix_ok = reader.read_u16(candidate.entry_length) && reader.read_u64(candidate.asset_id) &&
		reader.read_bytes(candidate.content_hash.size(), hash) && reader.read_u8(raw_source_format) &&
		reader.read_u8(raw_delivered_format) && reader.read_u8(raw_alpha_mode) && reader.read_u8(raw_timing_mode) &&
		reader.read_u16(reserved) && reader.read_u16(candidate.width) && reader.read_u16(candidate.height) &&
		reader.read_u32(candidate.frame_count) && reader.read_u64(candidate.duration_us) &&
		reader.read_u16(logical_name_length) && reader.read_u16(file_path_length) &&
		reader.read_u32(candidate.frame_duration_count);
	if (!prefix_ok) {
		return ValidationError::TruncatedPayload;
	}
	std::copy(hash.begin(), hash.end(), candidate.content_hash.begin());
	candidate.source_format = static_cast<SourceFormat>(raw_source_format);
	candidate.delivered_format = static_cast<DeliveredFormat>(raw_delivered_format);
	candidate.alpha_mode = static_cast<AlphaMode>(raw_alpha_mode);
	candidate.timing_mode = static_cast<AssetTimingMode>(raw_timing_mode);

	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	if (!is_known_source_format(candidate.source_format) || !is_known_delivered_format(candidate.delivered_format) ||
		!is_known_alpha_mode(candidate.alpha_mode) || !is_known_asset_timing_mode(candidate.timing_mode)) {
		return ValidationError::UnknownEnum;
	}
	if (candidate.asset_id == 0 || candidate.asset_id != comm_asset_id_from_content_hash(candidate.content_hash)) {
		return ValidationError::InvalidStateTransition;
	}
	if (candidate.width == 0 || candidate.width > 4096 || candidate.height == 0 || candidate.height > 4096 ||
		candidate.frame_count == 0 || candidate.frame_count > 65'535 || candidate.duration_us == 0 ||
		candidate.duration_us > MaximumCommDurationUs) {
		return ValidationError::OutOfRange;
	}
	if (logical_name_length == 0 || logical_name_length > MaximumCommLogicalNameLength || file_path_length == 0 ||
		file_path_length > MaximumCommFilePathLength) {
		return ValidationError::StringTooLong;
	}
	if (candidate.frame_duration_count > candidate.frame_count ||
		candidate.frame_duration_count > (MaximumCommAssetRecordPayloadSize - CommAssetEntryPrefixSize) / 4U) {
		return ValidationError::OutOfRange;
	}
	const std::uint64_t expected_length = CommAssetEntryPrefixSize + logical_name_length + file_path_length +
		static_cast<std::uint64_t>(candidate.frame_duration_count) * 4U;
	if (expected_length != candidate.entry_length) {
		return ValidationError::BadRecordLength;
	}
	if (!reader.read_bytes(logical_name_length, candidate.logical_name) ||
		!reader.read_bytes(file_path_length, candidate.file_path) ||
		!reader.read_bytes(static_cast<std::size_t>(candidate.frame_duration_count) * 4U,
			candidate.frame_durations_le) ||
		!reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	const auto logical_name = std::string_view{reinterpret_cast<const char*>(candidate.logical_name.data),
		candidate.logical_name.size};
	if (!is_valid_utf8(logical_name)) {
		return ValidationError::InvalidUtf8;
	}
	if (const auto error = validate_portable_bundle_path(candidate.file_path); error != ValidationError::None) {
		return error;
	}

	switch (candidate.timing_mode) {
	case AssetTimingMode::FormatIntrinsic:
		if (candidate.frame_duration_count != 0) {
			return ValidationError::InvalidStateTransition;
		}
		break;
	case AssetTimingMode::Constant:
		if (candidate.frame_duration_count != 1) {
			return ValidationError::InvalidStateTransition;
		}
		break;
	case AssetTimingMode::PerFrame:
		if (candidate.frame_duration_count != candidate.frame_count) {
			return ValidationError::InvalidStateTransition;
		}
		break;
	}

	if (candidate.frame_duration_count != 0) {
		PacketReader durations(candidate.frame_durations_le);
		std::uint64_t duration_sum = 0;
		for (std::uint32_t index = 0; index < candidate.frame_duration_count; ++index) {
			std::uint32_t duration = 0;
			if (!durations.read_u32(duration)) {
				return ValidationError::TruncatedPayload;
			}
			if (duration == 0 || duration_sum > std::numeric_limits<std::uint64_t>::max() - duration) {
				return ValidationError::OutOfRange;
			}
			duration_sum += duration;
		}
		if (!durations.at_end()) {
			return ValidationError::TrailingBytes;
		}
		if (candidate.timing_mode == AssetTimingMode::Constant) {
			std::uint64_t total_duration = 0;
			if (!checked_multiply(candidate.frame_count, duration_sum, total_duration) ||
				total_duration != candidate.duration_us) {
				return ValidationError::InvalidStateTransition;
			}
		} else if (duration_sum != candidate.duration_us) {
			return ValidationError::InvalidStateTransition;
		}
	}

	if (candidate.delivered_format == DeliveredFormat::Eff &&
		(candidate.timing_mode != AssetTimingMode::PerFrame ||
			!byte_view_ends_with(candidate.file_path, ".eff", 4))) {
		return ValidationError::InvalidStateTransition;
	}

	entry = candidate;
	consumed = candidate.entry_length;
	return ValidationError::None;
}

ValidationError validate_comm_asset_entry_payload(ByteView input) noexcept
{
	CommAssetEntryView entry;
	std::size_t consumed = 0;
	if (const auto error = decode_comm_asset_entry(input, entry, consumed); error != ValidationError::None) {
		return error;
	}
	return consumed == input.size ? ValidationError::None : ValidationError::TrailingBytes;
}

ValidationError decode_comm_asset_manifest_payload(ByteView input, CommAssetManifestPayloadView& payload) noexcept
{
	payload = CommAssetManifestPayloadView{};
	if (input.size > MaximumCommAssetRecordPayloadSize) {
		return ValidationError::MessageTooLarge;
	}
	if (input.data == nullptr || input.size < CommAssetManifestPrefixSize) {
		return ValidationError::TruncatedPayload;
	}

	CommAssetManifestPayloadView candidate;
	std::uint8_t converter_id_length = 0;
	std::uint8_t converter_version_length = 0;
	ByteView hash;
	PacketReader reader(input);
	const bool prefix_ok = reader.read_u16(candidate.bundle_version) && reader.read_u16(candidate.manifest_flags) &&
		reader.read_bytes(candidate.bundle_hash.size(), hash) && reader.read_u8(converter_id_length) &&
		reader.read_u8(converter_version_length) && reader.read_u64(candidate.frame_asset_id) &&
		reader.read_u64(candidate.placeholder_asset_id) && reader.read_u32(candidate.total_asset_count) &&
		reader.read_u32(candidate.first_asset_index) && reader.read_u16(candidate.entry_count);
	if (!prefix_ok) {
		return ValidationError::TruncatedPayload;
	}
	std::copy(hash.begin(), hash.end(), candidate.bundle_hash.begin());
	if (candidate.bundle_version != CommBundleVersionV1) {
		return ValidationError::OutOfRange;
	}
	if ((candidate.manifest_flags & ReservedManifestFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	if (digest_is_zero(candidate.bundle_hash)) {
		return ValidationError::InvalidAbsence;
	}
	if (converter_id_length == 0 || converter_id_length > 63 || converter_version_length == 0 ||
		converter_version_length > 63) {
		return ValidationError::StringTooLong;
	}
	const bool has_frame_asset = (candidate.manifest_flags & ManifestFlagFrameAsset) != 0;
	const bool has_placeholder_asset = (candidate.manifest_flags & ManifestFlagPlaceholderAsset) != 0;
	if (has_frame_asset != (candidate.frame_asset_id != 0) ||
		has_placeholder_asset != (candidate.placeholder_asset_id != 0)) {
		return ValidationError::InvalidAbsence;
	}
	if (candidate.total_asset_count > MaximumCommAssetCount || candidate.entry_count > MaximumCommAssetCount) {
		return ValidationError::ResourceLimit;
	}
	if (candidate.total_asset_count == 0 || candidate.first_asset_index >= candidate.total_asset_count ||
		candidate.entry_count == 0 ||
		candidate.entry_count > candidate.total_asset_count - candidate.first_asset_index) {
		return ValidationError::OutOfRange;
	}
	if (!reader.read_bytes(converter_id_length, candidate.converter_id) ||
		!reader.read_bytes(converter_version_length, candidate.converter_version)) {
		return ValidationError::TruncatedPayload;
	}
	const auto converter_id = std::string_view{reinterpret_cast<const char*>(candidate.converter_id.data),
		candidate.converter_id.size};
	const auto converter_version = std::string_view{reinterpret_cast<const char*>(candidate.converter_version.data),
		candidate.converter_version.size};
	if (!is_valid_utf8(converter_id) || !is_valid_utf8(converter_version)) {
		return ValidationError::InvalidUtf8;
	}
	candidate.encoded_entries = reader.unread();

	std::size_t offset = 0;
	std::uint64_t previous_asset_id = 0;
	bool found_frame_asset = false;
	bool found_placeholder_asset = false;
	try {
		// Quotas and declared ranges have already been validated above. Retain
		// only ordered views into the input instead of decoding all preceding
		// entries again for every path comparison.
		std::set<ByteView, AsciiCasefoldLess> casefold_ordered_paths;
		for (std::uint16_t entry_index = 0; entry_index < candidate.entry_count; ++entry_index) {
			CommAssetEntryView entry;
			std::size_t consumed = 0;
			if (offset > candidate.encoded_entries.size) {
				return ValidationError::TruncatedPayload;
			}
			if (const auto error = decode_comm_asset_entry(
					candidate.encoded_entries.subview(offset, candidate.encoded_entries.size - offset), entry, consumed);
				error != ValidationError::None) {
				return error;
			}
			if (entry_index != 0 && entry.asset_id <= previous_asset_id) {
				return entry.asset_id == previous_asset_id ? ValidationError::DuplicateItemKey
															 : ValidationError::OutOfRange;
			}
			previous_asset_id = entry.asset_id;
			found_frame_asset = found_frame_asset || entry.asset_id == candidate.frame_asset_id;
			found_placeholder_asset = found_placeholder_asset || entry.asset_id == candidate.placeholder_asset_id;
			if (!casefold_ordered_paths.insert(entry.file_path).second) {
				return ValidationError::DuplicateItemKey;
			}
			offset += consumed;
		}
	} catch (const std::bad_alloc&) {
		return ValidationError::ResourceLimit;
	}
	if (offset < candidate.encoded_entries.size) {
		return ValidationError::TrailingBytes;
	}
	if (offset > candidate.encoded_entries.size) {
		return ValidationError::TruncatedPayload;
	}
	if (candidate.first_asset_index == 0 && candidate.entry_count == candidate.total_asset_count &&
		((has_frame_asset && !found_frame_asset) || (has_placeholder_asset && !found_placeholder_asset))) {
		return ValidationError::UnknownEntity;
	}

	payload = candidate;
	return ValidationError::None;
}

ValidationError validate_comm_asset_manifest_record_payload(ByteView input) noexcept
{
	CommAssetManifestPayloadView payload;
	return decode_comm_asset_manifest_payload(input, payload);
}

ValidationError validate_comm_bundle_offer(const CommBundleOffer& payload) noexcept
{
	const bool hash_is_zero = digest_is_zero(payload.bundle_hash);
	if (payload.bundle_version == 0) {
		return hash_is_zero ? ValidationError::None : ValidationError::InvalidAbsence;
	}
	return hash_is_zero ? ValidationError::InvalidAbsence : ValidationError::None;
}

ValidationError validate_comm_bundle_selection(const CommBundleSelection& payload) noexcept
{
	if (!is_known_comm_negotiation_result(payload.result)) {
		return ValidationError::UnknownEnum;
	}
	const bool has_details = payload.bundle_version != 0 || payload.required_delivered_formats != 0 ||
						 !digest_is_zero(payload.required_bundle_hash);
	if (comm_selection_requires_bundle_details(payload.result)) {
		if (payload.bundle_version != CommBundleVersionV1 || payload.required_delivered_formats == 0 ||
			digest_is_zero(payload.required_bundle_hash)) {
			return ValidationError::InvalidAbsence;
		}
		return ValidationError::None;
	}
	return has_details ? ValidationError::InvalidStateTransition : ValidationError::None;
}

ValidationError encode_comm_bundle_offer(const CommBundleOffer& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_comm_bundle_offer(payload); error != ValidationError::None) {
		return error;
	}
	if ((payload.supported_delivered_formats & ReservedDeliveredFormatBits) != 0) {
		return ValidationError::ReservedFlag;
	}
	std::array<std::uint8_t, CommBundleOfferSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u16(payload.bundle_version) && writer.write_u16(0) &&
					writer.write_u32(payload.supported_delivered_formats) &&
					writer.write_bytes(ByteView{payload.bundle_hash.data(), payload.bundle_hash.size()});
	if (!ok || !finish_writer<CommBundleOfferSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_comm_bundle_offer(ByteView input, CommBundleOffer& payload) noexcept
{
	payload = CommBundleOffer{};
	if (const auto error = validate_exact_input(input, CommBundleOfferSize); error != ValidationError::None) {
		return error;
	}
	CommBundleOffer candidate;
	std::uint16_t reserved = 0;
	ByteView hash;
	PacketReader reader(input);
	const bool ok = reader.read_u16(candidate.bundle_version) && reader.read_u16(reserved) &&
					reader.read_u32(candidate.supported_delivered_formats) &&
					reader.read_bytes(candidate.bundle_hash.size(), hash);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	std::copy(hash.begin(), hash.end(), candidate.bundle_hash.begin());
	if (const auto error = validate_comm_bundle_offer(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_comm_bundle_selection(const CommBundleSelection& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_comm_bundle_selection(payload); error != ValidationError::None) {
		return error;
	}
	if ((payload.required_delivered_formats & ReservedDeliveredFormatBits) != 0) {
		return ValidationError::ReservedFlag;
	}
	std::array<std::uint8_t, CommBundleSelectionSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u8(static_cast<std::uint8_t>(payload.result)) && writer.write_u8(0) &&
					writer.write_u16(payload.bundle_version) &&
					writer.write_u32(payload.required_delivered_formats) &&
					writer.write_bytes(ByteView{payload.required_bundle_hash.data(), payload.required_bundle_hash.size()});
	if (!ok || !finish_writer<CommBundleSelectionSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_comm_bundle_selection(ByteView input, CommBundleSelection& payload) noexcept
{
	payload = CommBundleSelection{};
	if (const auto error = validate_exact_input(input, CommBundleSelectionSize); error != ValidationError::None) {
		return error;
	}
	CommBundleSelection candidate;
	std::uint8_t raw_result = 0;
	std::uint8_t reserved = 0;
	ByteView hash;
	PacketReader reader(input);
	const bool ok = reader.read_u8(raw_result) && reader.read_u8(reserved) &&
					reader.read_u16(candidate.bundle_version) &&
					reader.read_u32(candidate.required_delivered_formats) &&
					reader.read_bytes(candidate.required_bundle_hash.size(), hash);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	candidate.result = static_cast<CommNegotiationResult>(raw_result);
	std::copy(hash.begin(), hash.end(), candidate.required_bundle_hash.begin());
	if (const auto error = validate_comm_bundle_selection(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError validate_comm_view_state_payload(const CommViewStatePayload& payload) noexcept
{
	if (const auto error = validate_comm_modes(payload.playback_mode, payload.color_mode, payload.stop_reason);
		error != ValidationError::None) {
		return error;
	}
	if (payload.active) {
		if (payload.stop_reason != CommStopReason::None) {
			return ValidationError::InvalidStateTransition;
		}
		return validate_active_comm_values(payload.playback_id,
			payload.head_asset_id,
			payload.animation_time_us,
			payload.duration_us,
			payload.playback_rate);
	}
	if (payload.playback_mode != CommPlaybackMode::Once || payload.color_mode != CommColorMode::HudTint ||
		payload.playback_id != 0 || payload.engine_message_id != 0 || payload.sender_entity_id != 0 ||
		payload.head_asset_id != 0 || payload.producer_sample_time_us != 0 || payload.animation_time_us != 0 ||
		payload.duration_us != 0 || !is_canonical_zero(payload.playback_rate)) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError validate_comm_view_event_payload(const CommViewEventPayload& payload) noexcept
{
	if (!is_known_comm_event_kind(payload.event_kind)) {
		return ValidationError::UnknownEnum;
	}
	if (const auto error = validate_comm_modes(payload.playback_mode, payload.color_mode, payload.stop_reason);
		error != ValidationError::None) {
		return error;
	}
	if (payload.event_id == 0 || payload.playback_id == 0) {
		return ValidationError::OutOfRange;
	}
	if (payload.event_kind == CommEventKind::Start) {
		if (payload.stop_reason != CommStopReason::None) {
			return ValidationError::InvalidStateTransition;
		}
		return validate_active_comm_values(payload.playback_id,
			payload.head_asset_id,
			payload.animation_time_us,
			payload.duration_us,
			payload.playback_rate);
	}
	if (payload.stop_reason == CommStopReason::None) {
		return ValidationError::InvalidStateTransition;
	}
	if (payload.playback_mode != CommPlaybackMode::Once || payload.color_mode != CommColorMode::HudTint ||
		payload.engine_message_id != 0 || payload.sender_entity_id != 0 || payload.head_asset_id != 0 ||
		payload.animation_time_us != 0 || payload.duration_us != 0 || !is_canonical_zero(payload.playback_rate)) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError encode_comm_view_state_payload(const CommViewStatePayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_comm_view_state_payload(payload); error != ValidationError::None) {
		return error;
	}
	std::array<std::uint8_t, CommViewStatePayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_bool8(payload.active) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.playback_mode)) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.color_mode)) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.stop_reason)) &&
					writer.write_u64(payload.playback_id) && writer.write_u32(payload.engine_message_id) &&
					writer.write_u64(payload.sender_entity_id) && writer.write_u64(payload.head_asset_id) &&
					writer.write_u64(payload.producer_sample_time_us) && writer.write_u64(payload.animation_time_us) &&
					writer.write_u64(payload.duration_us) && writer.write_f32(payload.playback_rate);
	if (!ok || !finish_writer<CommViewStatePayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_comm_view_state_payload(ByteView input, CommViewStatePayload& payload) noexcept
{
	payload = CommViewStatePayload{};
	if (const auto error = validate_exact_input(input, CommViewStatePayloadSize); error != ValidationError::None) {
		return error;
	}
	CommViewStatePayload candidate;
	std::uint8_t raw_mode = 0;
	std::uint8_t raw_color = 0;
	std::uint8_t raw_stop = 0;
	PacketReader reader(input);
	const bool ok = reader.read_bool8(candidate.active) && reader.read_u8(raw_mode) && reader.read_u8(raw_color) &&
					reader.read_u8(raw_stop) && reader.read_u64(candidate.playback_id) &&
					reader.read_u32(candidate.engine_message_id) && reader.read_u64(candidate.sender_entity_id) &&
					reader.read_u64(candidate.head_asset_id) && reader.read_u64(candidate.producer_sample_time_us) &&
					reader.read_u64(candidate.animation_time_us) && reader.read_u64(candidate.duration_us) &&
					reader.read_f32(candidate.playback_rate);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	candidate.playback_mode = static_cast<CommPlaybackMode>(raw_mode);
	candidate.color_mode = static_cast<CommColorMode>(raw_color);
	candidate.stop_reason = static_cast<CommStopReason>(raw_stop);
	if (const auto error = validate_comm_view_state_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError validate_comm_view_state_record_payload(ByteView input) noexcept
{
	CommViewStatePayload payload;
	return decode_comm_view_state_payload(input, payload);
}

ValidationError encode_comm_view_event_payload(const CommViewEventPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_comm_view_event_payload(payload); error != ValidationError::None) {
		return error;
	}
	std::array<std::uint8_t, CommViewEventPayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u64(payload.event_id) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.event_kind)) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.playback_mode)) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.color_mode)) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.stop_reason)) &&
					writer.write_u64(payload.playback_id) && writer.write_u32(payload.engine_message_id) &&
					writer.write_u64(payload.sender_entity_id) && writer.write_u64(payload.head_asset_id) &&
					writer.write_u64(payload.producer_sample_time_us) && writer.write_u64(payload.animation_time_us) &&
					writer.write_u64(payload.duration_us) && writer.write_f32(payload.playback_rate);
	if (!ok || !finish_writer<CommViewEventPayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_comm_view_event_payload(ByteView input, CommViewEventPayload& payload) noexcept
{
	payload = CommViewEventPayload{};
	if (const auto error = validate_exact_input(input, CommViewEventPayloadSize); error != ValidationError::None) {
		return error;
	}
	CommViewEventPayload candidate;
	std::uint8_t raw_kind = 0;
	std::uint8_t raw_mode = 0;
	std::uint8_t raw_color = 0;
	std::uint8_t raw_stop = 0;
	PacketReader reader(input);
	const bool ok = reader.read_u64(candidate.event_id) && reader.read_u8(raw_kind) && reader.read_u8(raw_mode) &&
					reader.read_u8(raw_color) && reader.read_u8(raw_stop) && reader.read_u64(candidate.playback_id) &&
					reader.read_u32(candidate.engine_message_id) && reader.read_u64(candidate.sender_entity_id) &&
					reader.read_u64(candidate.head_asset_id) && reader.read_u64(candidate.producer_sample_time_us) &&
					reader.read_u64(candidate.animation_time_us) && reader.read_u64(candidate.duration_us) &&
					reader.read_f32(candidate.playback_rate);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	candidate.event_kind = static_cast<CommEventKind>(raw_kind);
	candidate.playback_mode = static_cast<CommPlaybackMode>(raw_mode);
	candidate.color_mode = static_cast<CommColorMode>(raw_color);
	candidate.stop_reason = static_cast<CommStopReason>(raw_stop);
	if (const auto error = validate_comm_view_event_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError validate_comm_view_event_record_payload(ByteView input) noexcept
{
	CommViewEventPayload payload;
	return decode_comm_view_event_payload(input, payload);
}

ValidationError avc_level_limits(H264Level level, AvcLevelLimits& limits) noexcept
{
	limits = AvcLevelLimits{};
	switch (level) {
	case H264Level::Level3_1:
		limits = AvcLevelLimits{3600, 108'000, 14'000};
		break;
	case H264Level::Level3_2:
		limits = AvcLevelLimits{5120, 216'000, 20'000};
		break;
	case H264Level::Level4_0:
		limits = AvcLevelLimits{8192, 245'760, 20'000};
		break;
	case H264Level::Level4_1:
		limits = AvcLevelLimits{8192, 245'760, 50'000};
		break;
	case H264Level::Level4_2:
		limits = AvcLevelLimits{8704, 522'240, 50'000};
		break;
	case H264Level::Level5_0:
		limits = AvcLevelLimits{22'080, 589'824, 135'000};
		break;
	case H264Level::Level5_1:
		limits = AvcLevelLimits{36'864, 983'040, 240'000};
		break;
	case H264Level::Level5_2:
		limits = AvcLevelLimits{36'864, 2'073'600, 240'000};
		break;
	default:
		return ValidationError::UnknownEnum;
	}
	return ValidationError::None;
}

ValidationError validate_avc_level_configuration(H264Level level,
	std::uint16_t width,
	std::uint16_t height,
	std::uint16_t fps_num,
	std::uint16_t fps_den,
	std::uint32_t bitrate_kbps) noexcept
{
	AvcLevelLimits limits;
	if (const auto error = avc_level_limits(level, limits); error != ValidationError::None) {
		return error;
	}
	if (width == 0 || height == 0 || fps_num == 0 || fps_den == 0 || bitrate_kbps == 0) {
		return ValidationError::OutOfRange;
	}

	const std::uint64_t width_in_mbs = (static_cast<std::uint64_t>(width) + 15U) / 16U;
	const std::uint64_t height_in_mbs = (static_cast<std::uint64_t>(height) + 15U) / 16U;
	std::uint64_t picture_size_in_mbs = 0;
	if (!checked_multiply(width_in_mbs, height_in_mbs, picture_size_in_mbs)) {
		return ValidationError::OutOfRange;
	}
	const auto maximum_dimension_in_mbs = integer_square_root(static_cast<std::uint64_t>(limits.max_fs) * 8U);
	if (picture_size_in_mbs > limits.max_fs || width_in_mbs > maximum_dimension_in_mbs ||
		height_in_mbs > maximum_dimension_in_mbs) {
		return ValidationError::OutOfRange;
	}

	std::uint64_t macroblocks_per_second_numerator = 0;
	std::uint64_t maximum_macroblocks_per_second_numerator = 0;
	if (!checked_multiply(picture_size_in_mbs, fps_num, macroblocks_per_second_numerator) ||
		!checked_multiply(limits.max_mbps, fps_den, maximum_macroblocks_per_second_numerator) ||
		macroblocks_per_second_numerator > maximum_macroblocks_per_second_numerator ||
		bitrate_kbps > limits.max_br_kbps) {
		return ValidationError::OutOfRange;
	}
	return ValidationError::None;
}

namespace {

ValidationError validate_h264_annex_b_access_unit_impl(ByteView access_unit,
	bool declared_idr,
	const TargetVideoConfigPayload* config,
	AnnexBValidationSummary& summary) noexcept
{
	summary = AnnexBValidationSummary{};
	if (access_unit.data == nullptr || access_unit.size < 5 || !has_four_byte_start_code(access_unit, 0)) {
		return ValidationError::InvalidStateTransition;
	}

	bool has_non_idr_vcl_slice = false;
	std::size_t start = 0;
	while (start < access_unit.size) {
		if (!has_four_byte_start_code(access_unit, start) || start + 4U >= access_unit.size) {
			return ValidationError::InvalidStateTransition;
		}
		const auto nal_header = access_unit.data[start + 4U];
		if ((nal_header & 0x80U) != 0) {
			return ValidationError::ReservedFlag;
		}
		const auto nal_unit_type = static_cast<std::uint8_t>(nal_header & 0x1fU);
		if (nal_unit_type == 0 || nal_unit_type > 23) {
			return ValidationError::UnknownEnum;
		}

		std::size_t next = access_unit.size;
		for (std::size_t index = start + 5U; index < access_unit.size; ++index) {
			if (has_four_byte_start_code(access_unit, index)) {
				next = index;
				break;
			}
			if (has_three_byte_start_code(access_unit, index)) {
				return ValidationError::InvalidStateTransition;
			}
		}

		++summary.nal_unit_count;
		switch (nal_unit_type) {
		case 1:
		case 2:
		case 3:
		case 4:
			has_non_idr_vcl_slice = true;
			break;
		case 5:
			if (!summary.has_sps || !summary.has_pps) {
				return ValidationError::InvalidStateTransition;
			}
			summary.has_idr_slice = true;
			break;
		case 7:
			if (config != nullptr) {
				ParsedSps parsed;
				const auto sps_begin = start + 5U;
				if (const auto error = parse_and_validate_sps(
						ByteView{access_unit.data + sps_begin, next - sps_begin}, *config, parsed);
					error != ValidationError::None) {
					return error;
				}
				++summary.validated_sps_count;
				summary.displayed_width = parsed.displayed_width;
				summary.displayed_height = parsed.displayed_height;
				summary.has_bt709_limited_range_vui = true;
			}
			summary.has_sps = true;
			break;
		case 8:
			if (!summary.has_sps) {
				return ValidationError::InvalidStateTransition;
			}
			summary.has_pps = true;
			break;
		default:
			break;
		}
		start = next;
	}

	if (declared_idr) {
		return summary.has_sps && summary.has_pps && summary.has_idr_slice ? ValidationError::None
																  : ValidationError::InvalidAbsence;
	}
	if (summary.has_idr_slice) {
		return ValidationError::InvalidStateTransition;
	}
	return has_non_idr_vcl_slice ? ValidationError::None : ValidationError::InvalidAbsence;
}

} // namespace

ValidationError validate_h264_annex_b_access_unit(ByteView access_unit,
	bool declared_idr,
	AnnexBValidationSummary& summary) noexcept
{
	return validate_h264_annex_b_access_unit_impl(access_unit, declared_idr, nullptr, summary);
}

ValidationError validate_h264_annex_b_access_unit(ByteView access_unit,
	bool declared_idr,
	const TargetVideoConfigPayload& config,
	AnnexBValidationSummary& summary) noexcept
{
	if (const auto error = validate_target_video_config_payload(config); error != ValidationError::None) {
		return error;
	}
	if (config.result != VideoConfigResult::Accepted) {
		return ValidationError::InvalidStateTransition;
	}
	return validate_h264_annex_b_access_unit_impl(access_unit, declared_idr, &config, summary);
}

ValidationError validate_target_video_subscribe_payload(const TargetVideoSubscribePayload& payload) noexcept
{
	if (payload.request_id == 0 || !is_even_video_dimension(payload.max_width) ||
		!is_even_video_dimension(payload.max_height) || !is_even_video_dimension(payload.preferred_width) ||
		!is_even_video_dimension(payload.preferred_height) || payload.preferred_width > payload.max_width ||
		payload.preferred_height > payload.max_height || payload.max_fps < MinimumVideoFps ||
		payload.max_fps > MaximumVideoFps || payload.preferred_fps < MinimumVideoFps ||
		payload.preferred_fps > payload.max_fps || payload.max_bitrate_kbps < MinimumVideoBitrateKbps ||
		payload.max_bitrate_kbps > MaximumVideoBitrateKbps ||
		payload.preferred_bitrate_kbps < MinimumVideoBitrateKbps ||
		payload.preferred_bitrate_kbps > payload.max_bitrate_kbps) {
		return ValidationError::OutOfRange;
	}
	if ((payload.supported_h264_profiles & KnownH264ProfileBits) == 0 ||
		(payload.supported_h264_levels & KnownH264LevelBits) == 0 ||
		(payload.overlay_capabilities & RequiredOverlayCapabilityBits) != RequiredOverlayCapabilityBits ||
		(payload.acceptable_render_profiles & KnownRenderProfileBits) == 0) {
		return ValidationError::InvalidAbsence;
	}
	const auto preferred_bit = render_profile_bit(payload.preferred_render_profile);
	if (preferred_bit == 0) {
		return ValidationError::UnknownEnum;
	}
	if ((payload.acceptable_render_profiles & preferred_bit) == 0) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError validate_target_video_config_payload(const TargetVideoConfigPayload& payload) noexcept
{
	if (!is_known_video_config_result(payload.result)) {
		return ValidationError::UnknownEnum;
	}
	if (payload.result != VideoConfigResult::Accepted) {
		const bool has_nonzero_field = payload.codec != Codec::Invalid ||
			static_cast<std::uint8_t>(payload.codec_profile) != 0 ||
			static_cast<std::uint8_t>(payload.codec_level) != 0 || payload.stream_id != 0 ||
			payload.config_generation != 0 || payload.pixel_format != PixelFormat::Invalid ||
			payload.render_profile != RenderProfile::Invalid || payload.overlay_mode != OverlayMode::Invalid ||
			payload.recovery_mode != RecoveryMode::Invalid || payload.width != 0 || payload.height != 0 ||
			payload.fps_num != 0 || payload.fps_den != 0 || payload.bitrate_kbps != 0 ||
			payload.gop_duration_ms != 0 || payload.idr_recovery_window_ms != 0;
		return has_nonzero_field ? ValidationError::InvalidStateTransition : ValidationError::None;
	}

	if (payload.codec != Codec::H264AnnexB || profile_bit(payload.codec_profile) == 0 ||
		level_bit(payload.codec_level) == 0 || payload.pixel_format != PixelFormat::Yuv420p8 ||
		render_profile_bit(payload.render_profile) == 0 ||
		payload.overlay_mode != OverlayMode::Client || payload.recovery_mode != RecoveryMode::IdrSelectiveRetransmit) {
		return ValidationError::UnknownEnum;
	}
	if (payload.stream_id == 0 || payload.config_generation == 0) {
		return ValidationError::OutOfRange;
	}
	if (!is_even_video_dimension(payload.width) || !is_even_video_dimension(payload.height) || payload.fps_num == 0 ||
		payload.fps_den == 0 || payload.fps_num < payload.fps_den ||
		static_cast<std::uint64_t>(payload.fps_num) >
			static_cast<std::uint64_t>(MaximumVideoFps) * payload.fps_den ||
		std::gcd(payload.fps_num, payload.fps_den) != 1 || payload.bitrate_kbps < MinimumVideoBitrateKbps ||
		payload.bitrate_kbps > MaximumVideoBitrateKbps || payload.gop_duration_ms == 0 ||
		payload.gop_duration_ms > MaximumVideoGopDurationMs || payload.idr_recovery_window_ms == 0 ||
		payload.idr_recovery_window_ms > MaximumVideoIdrRecoveryWindowMs) {
		return ValidationError::OutOfRange;
	}
	return validate_avc_level_configuration(payload.codec_level,
		payload.width,
		payload.height,
		payload.fps_num,
		payload.fps_den,
		payload.bitrate_kbps);
}

ValidationError validate_target_video_config_for_subscribe(const TargetVideoConfigPayload& config,
	const TargetVideoSubscribePayload& subscribe) noexcept
{
	if (const auto error = validate_target_video_subscribe_payload(subscribe); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_target_video_config_payload(config); error != ValidationError::None) {
		return error;
	}
	if (config.result != VideoConfigResult::Accepted || config.request_id != subscribe.request_id) {
		return ValidationError::InvalidStateTransition;
	}
	if ((subscribe.supported_h264_profiles & profile_bit(config.codec_profile)) == 0 ||
		(subscribe.supported_h264_levels & level_bit(config.codec_level)) == 0 ||
		(subscribe.acceptable_render_profiles & render_profile_bit(config.render_profile)) == 0 ||
		config.width > subscribe.max_width || config.height > subscribe.max_height ||
		config.bitrate_kbps > subscribe.max_bitrate_kbps ||
		static_cast<std::uint64_t>(config.fps_num) >
			static_cast<std::uint64_t>(subscribe.max_fps) * config.fps_den) {
		return ValidationError::CapabilityNotNegotiated;
	}
	return ValidationError::None;
}

ValidationError validate_target_video_config_for_subscribe(const TargetVideoConfigPayload& config,
	const TargetVideoSubscribePayload& subscribe,
	bool h264_codec_capability_negotiated) noexcept
{
	if (const auto error = validate_target_video_config_for_subscribe(config, subscribe);
		error != ValidationError::None) {
		return error;
	}
	return h264_codec_capability_negotiated ? ValidationError::None
									  : ValidationError::CapabilityNotNegotiated;
}

ValidationError validate_target_video_frame_payload(const TargetVideoFramePayload& payload) noexcept
{
	if (payload.stream_id == 0 || payload.config_generation == 0 || payload.video_frame_id == 0 ||
		payload.target_entity_id == 0) {
		return ValidationError::OutOfRange;
	}
	if ((payload.video_flags & ReservedVideoFrameFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	if (payload.encoded_frame_size == 0 || payload.encoded_frame_size != payload.annex_b_access_unit.size ||
		payload.annex_b_access_unit.data == nullptr) {
		return ValidationError::InvalidAbsence;
	}
	if (payload.encoded_frame_size > MaxVideoMessageSize - TargetVideoFramePrefixSize) {
		return ValidationError::MessageTooLarge;
	}
	const bool declared_idr = (payload.video_flags & VideoFrameFlagIdr) != 0;
	if ((payload.video_flags & (VideoFrameFlagDiscontinuity | VideoFrameFlagTargetChanged)) != 0 && !declared_idr) {
		return ValidationError::InvalidStateTransition;
	}
	AnnexBValidationSummary summary;
	return validate_h264_annex_b_access_unit(payload.annex_b_access_unit, declared_idr, summary);
}

ValidationError validate_target_video_frame_payload(const TargetVideoFramePayload& payload,
	const TargetVideoConfigPayload& config) noexcept
{
	if (payload.stream_id == 0 || payload.config_generation == 0 || payload.video_frame_id == 0 ||
		payload.target_entity_id == 0) {
		return ValidationError::OutOfRange;
	}
	if ((payload.video_flags & ReservedVideoFrameFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	if (payload.encoded_frame_size == 0 || payload.encoded_frame_size != payload.annex_b_access_unit.size ||
		payload.annex_b_access_unit.data == nullptr) {
		return ValidationError::InvalidAbsence;
	}
	if (payload.encoded_frame_size > MaxVideoMessageSize - TargetVideoFramePrefixSize) {
		return ValidationError::MessageTooLarge;
	}
	const bool declared_idr = (payload.video_flags & VideoFrameFlagIdr) != 0;
	if ((payload.video_flags & (VideoFrameFlagDiscontinuity | VideoFrameFlagTargetChanged)) != 0 &&
		!declared_idr) {
		return ValidationError::InvalidStateTransition;
	}
	AnnexBValidationSummary summary;
	return validate_h264_annex_b_access_unit(payload.annex_b_access_unit, declared_idr, config, summary);
}

ValidationError
validate_target_video_keyframe_request_payload(const TargetVideoKeyframeRequestPayload& payload) noexcept
{
	if (payload.request_id == 0 || payload.stream_id == 0 || payload.config_generation == 0) {
		return ValidationError::OutOfRange;
	}
	return is_known_video_keyframe_reason(payload.reason) ? ValidationError::None : ValidationError::UnknownEnum;
}

ValidationError validate_target_video_keyframe_request_clock(const TargetVideoKeyframeRequestPayload& payload,
	bool clock_filter_valid,
	std::uint64_t estimated_producer_now_us) noexcept
{
	bool idr_required = false;
	return validate_target_video_keyframe_request_clock(
		payload, clock_filter_valid, estimated_producer_now_us, idr_required);
}

ValidationError validate_target_video_keyframe_request_clock(const TargetVideoKeyframeRequestPayload& payload,
	bool clock_filter_valid,
	std::uint64_t estimated_producer_now_us,
	bool& idr_required) noexcept
{
	idr_required = false;
	if (const auto error = validate_target_video_keyframe_request_payload(payload); error != ValidationError::None) {
		return error;
	}
	if (!clock_filter_valid) {
		if (payload.needed_before_producer_time_us != 0) {
			return ValidationError::InvalidStateTransition;
		}
		idr_required = true;
		return ValidationError::None;
	}
	if (payload.needed_before_producer_time_us == 0) {
		return ValidationError::InvalidStateTransition;
	}
	if (payload.needed_before_producer_time_us <= estimated_producer_now_us) {
		// The reliable request is valid and must be acknowledged, but its
		// producer-clock deadline has elapsed so it does not mandate an IDR.
		return ValidationError::None;
	}
	if (payload.needed_before_producer_time_us - estimated_producer_now_us >
		MaximumVideoKeyframeDeadlineLeadUs) {
		return ValidationError::OutOfRange;
	}
	idr_required = true;
	return ValidationError::None;
}

ValidationError validate_target_video_stop_payload(const TargetVideoStopPayload& payload) noexcept
{
	if (payload.request_id == 0 || payload.stream_id == 0 || payload.config_generation == 0) {
		return ValidationError::OutOfRange;
	}
	if (!is_known_video_stop_reason(payload.reason)) {
		return ValidationError::UnknownEnum;
	}
	if ((payload.stop_flags & ReservedVideoStopFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	const bool is_confirmation = (payload.stop_flags & VideoStopFlagConfirmation) != 0;
	if (is_confirmation && payload.reason != VideoStopReason::ClientUnsubscribe) {
		return ValidationError::InvalidStateTransition;
	}
	if (!is_confirmation && payload.reason == VideoStopReason::ClientUnsubscribe && payload.producer_time_us != 0) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError validate_target_video_stats_payload(const TargetVideoStatsPayload& payload) noexcept
{
	if (payload.stream_id == 0 || payload.config_generation == 0 || payload.report_id == 0) {
		return ValidationError::OutOfRange;
	}
	if (payload.estimated_loss_ppm > MaximumVideoEstimatedLossPpm ||
		payload.report_interval_ms < MinimumVideoStatsIntervalMs ||
		payload.report_interval_ms > MaximumVideoStatsIntervalMs) {
		return ValidationError::OutOfRange;
	}
	return (payload.stats_flags & ReservedVideoStatsFlags) == 0 ? ValidationError::None
																		 : ValidationError::ReservedFlag;
}

ValidationError encode_target_video_subscribe_payload(const TargetVideoSubscribePayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_target_video_subscribe_payload(payload); error != ValidationError::None) {
		return error;
	}
	if ((payload.supported_h264_profiles & ReservedH264ProfileBits) != 0 ||
		(payload.supported_h264_levels & ReservedH264LevelBits) != 0 ||
		(payload.overlay_capabilities & ReservedOverlayCapabilityBits) != 0 ||
		(payload.acceptable_render_profiles & ReservedRenderProfileBits) != 0) {
		return ValidationError::ReservedFlag;
	}
	std::array<std::uint8_t, TargetVideoSubscribePayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u32(payload.request_id) && writer.write_u16(payload.max_width) &&
		writer.write_u16(payload.max_height) && writer.write_u16(payload.preferred_width) &&
		writer.write_u16(payload.preferred_height) && writer.write_u16(payload.max_fps) &&
		writer.write_u16(payload.preferred_fps) && writer.write_u32(payload.max_bitrate_kbps) &&
		writer.write_u32(payload.preferred_bitrate_kbps) && writer.write_u32(payload.supported_h264_profiles) &&
		writer.write_u32(payload.supported_h264_levels) && writer.write_u32(payload.overlay_capabilities) &&
		writer.write_u32(payload.acceptable_render_profiles) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.preferred_render_profile)) && writer.write_zeroes(3);
	if (!ok || !finish_writer<TargetVideoSubscribePayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_target_video_subscribe_payload(ByteView input, TargetVideoSubscribePayload& payload) noexcept
{
	payload = TargetVideoSubscribePayload{};
	if (const auto error = validate_exact_input(input, TargetVideoSubscribePayloadSize); error != ValidationError::None) {
		return error;
	}
	TargetVideoSubscribePayload candidate;
	std::uint8_t raw_render_profile = 0;
	ByteView reserved;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.request_id) && reader.read_u16(candidate.max_width) &&
		reader.read_u16(candidate.max_height) && reader.read_u16(candidate.preferred_width) &&
		reader.read_u16(candidate.preferred_height) && reader.read_u16(candidate.max_fps) &&
		reader.read_u16(candidate.preferred_fps) && reader.read_u32(candidate.max_bitrate_kbps) &&
		reader.read_u32(candidate.preferred_bitrate_kbps) && reader.read_u32(candidate.supported_h264_profiles) &&
		reader.read_u32(candidate.supported_h264_levels) && reader.read_u32(candidate.overlay_capabilities) &&
		reader.read_u32(candidate.acceptable_render_profiles) && reader.read_u8(raw_render_profile) &&
		reader.read_bytes(3, reserved);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved.data[0] != 0 || reserved.data[1] != 0 || reserved.data[2] != 0) {
		return ValidationError::ReservedFlag;
	}
	candidate.preferred_render_profile = static_cast<RenderProfile>(raw_render_profile);
	if (const auto error = validate_target_video_subscribe_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_target_video_config_payload(const TargetVideoConfigPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_target_video_config_payload(payload); error != ValidationError::None) {
		return error;
	}
	std::array<std::uint8_t, TargetVideoConfigPayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u32(payload.request_id) && writer.write_u8(static_cast<std::uint8_t>(payload.result)) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.codec)) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.codec_profile)) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.codec_level)) && writer.write_u32(payload.stream_id) &&
		writer.write_u32(payload.config_generation) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.pixel_format)) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.render_profile)) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.overlay_mode)) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.recovery_mode)) && writer.write_u16(payload.width) &&
		writer.write_u16(payload.height) && writer.write_u16(payload.fps_num) && writer.write_u16(payload.fps_den) &&
		writer.write_u32(payload.bitrate_kbps) && writer.write_u16(payload.gop_duration_ms) &&
		writer.write_u16(payload.idr_recovery_window_ms);
	if (!ok || !finish_writer<TargetVideoConfigPayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_target_video_config_payload(ByteView input, TargetVideoConfigPayload& payload) noexcept
{
	payload = TargetVideoConfigPayload{};
	if (const auto error = validate_exact_input(input, TargetVideoConfigPayloadSize); error != ValidationError::None) {
		return error;
	}
	TargetVideoConfigPayload candidate;
	std::uint8_t raw_result = 0;
	std::uint8_t raw_codec = 0;
	std::uint8_t raw_profile = 0;
	std::uint8_t raw_level = 0;
	std::uint8_t raw_pixel_format = 0;
	std::uint8_t raw_render_profile = 0;
	std::uint8_t raw_overlay_mode = 0;
	std::uint8_t raw_recovery_mode = 0;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.request_id) && reader.read_u8(raw_result) && reader.read_u8(raw_codec) &&
		reader.read_u8(raw_profile) && reader.read_u8(raw_level) && reader.read_u32(candidate.stream_id) &&
		reader.read_u32(candidate.config_generation) && reader.read_u8(raw_pixel_format) &&
		reader.read_u8(raw_render_profile) && reader.read_u8(raw_overlay_mode) && reader.read_u8(raw_recovery_mode) &&
		reader.read_u16(candidate.width) && reader.read_u16(candidate.height) && reader.read_u16(candidate.fps_num) &&
		reader.read_u16(candidate.fps_den) && reader.read_u32(candidate.bitrate_kbps) &&
		reader.read_u16(candidate.gop_duration_ms) && reader.read_u16(candidate.idr_recovery_window_ms);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	candidate.result = static_cast<VideoConfigResult>(raw_result);
	candidate.codec = static_cast<Codec>(raw_codec);
	candidate.codec_profile = static_cast<H264Profile>(raw_profile);
	candidate.codec_level = static_cast<H264Level>(raw_level);
	candidate.pixel_format = static_cast<PixelFormat>(raw_pixel_format);
	candidate.render_profile = static_cast<RenderProfile>(raw_render_profile);
	candidate.overlay_mode = static_cast<OverlayMode>(raw_overlay_mode);
	candidate.recovery_mode = static_cast<RecoveryMode>(raw_recovery_mode);
	if (const auto error = validate_target_video_config_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_target_video_frame_payload(const TargetVideoFramePayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_target_video_frame_payload(payload); error != ValidationError::None) {
		return error;
	}
	std::array<std::uint8_t, TargetVideoFramePrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u32(payload.stream_id) && writer.write_u32(payload.config_generation) &&
		writer.write_u32(payload.video_frame_id) && writer.write_u64(payload.target_entity_id) &&
		writer.write_u64(payload.presentation_time_us) && writer.write_u32(payload.encoded_frame_size) &&
		writer.write_u16(payload.video_flags) && writer.write_u16(0);
	if (!ok || !finish_writer<TargetVideoFramePrefixSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_with_suffix(prefix, payload.annex_b_access_unit, output, written);
}

ValidationError decode_target_video_frame_payload(ByteView input, TargetVideoFramePayload& payload) noexcept
{
	payload = TargetVideoFramePayload{};
	if (input.size > MaxVideoMessageSize) {
		return ValidationError::MessageTooLarge;
	}
	if (input.data == nullptr || input.size < TargetVideoFramePrefixSize) {
		return ValidationError::TruncatedPayload;
	}
	TargetVideoFramePayload candidate;
	std::uint16_t reserved = 0;
	PacketReader reader(input);
	const bool prefix_ok = reader.read_u32(candidate.stream_id) && reader.read_u32(candidate.config_generation) &&
		reader.read_u32(candidate.video_frame_id) && reader.read_u64(candidate.target_entity_id) &&
		reader.read_u64(candidate.presentation_time_us) && reader.read_u32(candidate.encoded_frame_size) &&
		reader.read_u16(candidate.video_flags) && reader.read_u16(reserved);
	if (!prefix_ok) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	if (candidate.encoded_frame_size > MaxVideoMessageSize - TargetVideoFramePrefixSize) {
		return ValidationError::MessageTooLarge;
	}
	if (reader.remaining() < candidate.encoded_frame_size) {
		return ValidationError::TruncatedPayload;
	}
	if (reader.remaining() > candidate.encoded_frame_size) {
		return ValidationError::TrailingBytes;
	}
	if (!reader.read_bytes(candidate.encoded_frame_size, candidate.annex_b_access_unit) || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (const auto error = validate_target_video_frame_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_target_video_keyframe_request_payload(const TargetVideoKeyframeRequestPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_target_video_keyframe_request_payload(payload); error != ValidationError::None) {
		return error;
	}
	std::array<std::uint8_t, TargetVideoKeyframeRequestPayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u32(payload.request_id) && writer.write_u32(payload.stream_id) &&
		writer.write_u32(payload.config_generation) && writer.write_u32(payload.last_decodable_frame_id) &&
		writer.write_u64(payload.needed_before_producer_time_us) &&
		writer.write_u8(static_cast<std::uint8_t>(payload.reason)) && writer.write_zeroes(7);
	if (!ok || !finish_writer<TargetVideoKeyframeRequestPayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_target_video_keyframe_request_payload(ByteView input,
	TargetVideoKeyframeRequestPayload& payload) noexcept
{
	payload = TargetVideoKeyframeRequestPayload{};
	if (const auto error = validate_exact_input(input, TargetVideoKeyframeRequestPayloadSize);
		error != ValidationError::None) {
		return error;
	}
	TargetVideoKeyframeRequestPayload candidate;
	std::uint8_t raw_reason = 0;
	ByteView reserved;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.request_id) && reader.read_u32(candidate.stream_id) &&
		reader.read_u32(candidate.config_generation) && reader.read_u32(candidate.last_decodable_frame_id) &&
		reader.read_u64(candidate.needed_before_producer_time_us) && reader.read_u8(raw_reason) &&
		reader.read_bytes(7, reserved);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	for (const auto byte : reserved) {
		if (byte != 0) {
			return ValidationError::ReservedFlag;
		}
	}
	candidate.reason = static_cast<VideoKeyframeReason>(raw_reason);
	if (const auto error = validate_target_video_keyframe_request_payload(candidate);
		error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_target_video_stop_payload(const TargetVideoStopPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_target_video_stop_payload(payload); error != ValidationError::None) {
		return error;
	}
	std::array<std::uint8_t, TargetVideoStopPayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u32(payload.request_id) && writer.write_u32(payload.stream_id) &&
		writer.write_u32(payload.config_generation) && writer.write_u8(static_cast<std::uint8_t>(payload.reason)) &&
		writer.write_u8(payload.stop_flags) && writer.write_u16(0) && writer.write_u64(payload.producer_time_us);
	if (!ok || !finish_writer<TargetVideoStopPayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_target_video_stop_payload(ByteView input, TargetVideoStopPayload& payload) noexcept
{
	payload = TargetVideoStopPayload{};
	if (const auto error = validate_exact_input(input, TargetVideoStopPayloadSize); error != ValidationError::None) {
		return error;
	}
	TargetVideoStopPayload candidate;
	std::uint8_t raw_reason = 0;
	std::uint16_t reserved = 0;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.request_id) && reader.read_u32(candidate.stream_id) &&
		reader.read_u32(candidate.config_generation) && reader.read_u8(raw_reason) &&
		reader.read_u8(candidate.stop_flags) && reader.read_u16(reserved) &&
		reader.read_u64(candidate.producer_time_us);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	candidate.reason = static_cast<VideoStopReason>(raw_reason);
	if (const auto error = validate_target_video_stop_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_target_video_stats_payload(const TargetVideoStatsPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_target_video_stats_payload(payload); error != ValidationError::None) {
		return error;
	}
	std::array<std::uint8_t, TargetVideoStatsPayloadSize> encoded{};
	PacketWriter writer(MutableByteView{encoded.data(), encoded.size()});
	const bool ok = writer.write_u32(payload.stream_id) && writer.write_u32(payload.config_generation) &&
		writer.write_u32(payload.report_id) && writer.write_u32(payload.highest_received_frame_id) &&
		writer.write_u32(payload.highest_decoded_frame_id) && writer.write_u32(payload.highest_presented_frame_id) &&
		writer.write_u32(payload.received_frames) && writer.write_u32(payload.decoded_frames) &&
		writer.write_u32(payload.presented_frames) && writer.write_u32(payload.dropped_frames) &&
		writer.write_u32(payload.missing_fragments) && writer.write_u32(payload.decoder_errors) &&
		writer.write_u32(payload.estimated_loss_ppm) && writer.write_u32(payload.jitter_us) &&
		writer.write_u32(payload.decode_latency_us) && writer.write_u32(payload.presentation_latency_us) &&
		writer.write_u32(payload.buffered_duration_us) && writer.write_u16(payload.report_interval_ms) &&
		writer.write_u16(payload.stats_flags);
	if (!ok || !finish_writer<TargetVideoStatsPayloadSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_fixed(encoded, output, written);
}

ValidationError decode_target_video_stats_payload(ByteView input, TargetVideoStatsPayload& payload) noexcept
{
	payload = TargetVideoStatsPayload{};
	if (const auto error = validate_exact_input(input, TargetVideoStatsPayloadSize); error != ValidationError::None) {
		return error;
	}
	TargetVideoStatsPayload candidate;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.stream_id) && reader.read_u32(candidate.config_generation) &&
		reader.read_u32(candidate.report_id) && reader.read_u32(candidate.highest_received_frame_id) &&
		reader.read_u32(candidate.highest_decoded_frame_id) && reader.read_u32(candidate.highest_presented_frame_id) &&
		reader.read_u32(candidate.received_frames) && reader.read_u32(candidate.decoded_frames) &&
		reader.read_u32(candidate.presented_frames) && reader.read_u32(candidate.dropped_frames) &&
		reader.read_u32(candidate.missing_fragments) && reader.read_u32(candidate.decoder_errors) &&
		reader.read_u32(candidate.estimated_loss_ppm) && reader.read_u32(candidate.jitter_us) &&
		reader.read_u32(candidate.decode_latency_us) && reader.read_u32(candidate.presentation_latency_us) &&
		reader.read_u32(candidate.buffered_duration_us) && reader.read_u16(candidate.report_interval_ms) &&
		reader.read_u16(candidate.stats_flags);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (const auto error = validate_target_video_stats_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

std::uint64_t video_bucket_capacity_bytes(std::uint32_t bitrate_kbps) noexcept
{
	const auto rate_bits_per_s = static_cast<std::uint64_t>(bitrate_kbps) * 1000U;
	const auto raw_burst_bytes = (rate_bits_per_s * 500U) / 8000U;
	return std::min(MaximumVideoBucketCapacityBytes,
		std::max(MinimumVideoBucketCapacityBytes, raw_burst_bytes));
}

ValidationError VideoTokenBucket::configure(std::uint32_t bitrate_kbps,
	std::uint64_t initial_time_us,
	VideoTokenBucket& bucket) noexcept
{
	if (bitrate_kbps < MinimumVideoBitrateKbps || bitrate_kbps > MaximumVideoBitrateKbps) {
		return ValidationError::OutOfRange;
	}
	VideoTokenBucket candidate;
	candidate.m_bitrate_kbps = bitrate_kbps;
	candidate.m_rate_bits_per_s = static_cast<std::uint64_t>(bitrate_kbps) * 1000U;
	candidate.m_capacity_bytes = video_bucket_capacity_bytes(bitrate_kbps);
	candidate.m_tokens_bytes = candidate.m_capacity_bytes;
	candidate.m_last_refill_time_us = initial_time_us;
	candidate.m_configured = true;
	bucket = candidate;
	return ValidationError::None;
}

VideoTokenBucketResult VideoTokenBucket::advance(std::uint64_t now_us) noexcept
{
	if (!m_configured) {
		return VideoTokenBucketResult::Unconfigured;
	}
	if (m_clock_regressed) {
		return VideoTokenBucketResult::ClockRegressed;
	}
	if (now_us < m_last_refill_time_us) {
		m_clock_regressed = true;
		return VideoTokenBucketResult::ClockRegressed;
	}
	const auto elapsed_us = now_us - m_last_refill_time_us;
	if (elapsed_us == 0) {
		return VideoTokenBucketResult::Allowed;
	}

	const auto maximum_elapsed_us =
		(m_capacity_bytes * VideoBucketRefillDenominator + m_rate_bits_per_s - 1U) / m_rate_bits_per_s;
	m_last_refill_time_us = now_us;
	if (elapsed_us >= maximum_elapsed_us) {
		m_tokens_bytes = m_capacity_bytes;
		m_refill_remainder = 0;
		return VideoTokenBucketResult::Allowed;
	}

	const auto refill_numerator = m_rate_bits_per_s * elapsed_us + m_refill_remainder;
	const auto added_bytes = refill_numerator / VideoBucketRefillDenominator;
	const auto next_remainder = refill_numerator % VideoBucketRefillDenominator;
	if (added_bytes >= m_capacity_bytes - m_tokens_bytes) {
		m_tokens_bytes = m_capacity_bytes;
		m_refill_remainder = 0;
	} else {
		m_tokens_bytes += added_bytes;
		m_refill_remainder = next_remainder;
	}
	return VideoTokenBucketResult::Allowed;
}

VideoTokenBucketResult VideoTokenBucket::try_consume(std::uint64_t now_us, std::size_t datagram_bytes) noexcept
{
	if (!m_configured) {
		return VideoTokenBucketResult::Unconfigured;
	}
	if (datagram_bytes == 0 || datagram_bytes > MaxDatagramSize) {
		return VideoTokenBucketResult::InvalidDatagramSize;
	}
	if (const auto result = advance(now_us); result != VideoTokenBucketResult::Allowed) {
		return result;
	}
	if (datagram_bytes > m_tokens_bytes) {
		return VideoTokenBucketResult::InsufficientTokens;
	}
	m_tokens_bytes -= datagram_bytes;
	return VideoTokenBucketResult::Allowed;
}

VideoTokenBucketResult VideoTokenBucket::reconfigure(std::uint32_t bitrate_kbps, std::uint64_t now_us) noexcept
{
	if (!m_configured) {
		return VideoTokenBucketResult::Unconfigured;
	}
	if (bitrate_kbps < MinimumVideoBitrateKbps || bitrate_kbps > MaximumVideoBitrateKbps) {
		return VideoTokenBucketResult::InvalidBitrate;
	}
	if (const auto result = advance(now_us); result != VideoTokenBucketResult::Allowed) {
		return result;
	}
	m_bitrate_kbps = bitrate_kbps;
	m_rate_bits_per_s = static_cast<std::uint64_t>(bitrate_kbps) * 1000U;
	m_capacity_bytes = video_bucket_capacity_bytes(bitrate_kbps);
	m_tokens_bytes = std::min(m_tokens_bytes, m_capacity_bytes);
	if (m_tokens_bytes == m_capacity_bytes) {
		m_refill_remainder = 0;
	}
	return VideoTokenBucketResult::Allowed;
}

} // namespace telemetry::protocol
