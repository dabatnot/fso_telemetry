#include "telemetry/protocol/telemetry_fragmenter.h"

#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <algorithm>
#include <limits>

namespace telemetry::protocol {

namespace {

bool class_limits(MessageSizeClass message_class, std::size_t& max_message_size) noexcept {
	switch (message_class) {
	case MessageSizeClass::State:
		max_message_size = MaxStateMessageSize;
		return true;
	case MessageSizeClass::Video:
		max_message_size = MaxVideoMessageSize;
		return true;
	default:
		return false;
	}
}

} // namespace

bool TelemetryFragmenter::create(ByteView message,
                                 MessageSizeClass message_class,
                                 TelemetryFragmenter& result) noexcept {
	result = TelemetryFragmenter{};

	std::size_t max_message_size = 0;
	if (!class_limits(message_class, max_message_size) ||
	    (message.size != 0 && message.data == nullptr) ||
	    message.size > max_message_size ||
	    message.size > std::numeric_limits<std::uint32_t>::max()) {
		return false;
	}

	const auto fragment_count = message.size == 0 ? std::size_t{1}
	                                              : (message.size + MaxFragmentPayload - 1) / MaxFragmentPayload;
	const auto max_fragment_count = message_class == MessageSizeClass::Video ? MaxVideoFragments : MaxStateFragments;
	if (fragment_count == 0 || fragment_count > max_fragment_count ||
	    fragment_count > std::numeric_limits<std::uint16_t>::max()) {
		return false;
	}

	std::uint32_t message_crc32 = 0;
	if (!crc32_iso_hdlc(message, message_crc32)) {
		return false;
	}

	TelemetryFragmenter candidate;
	candidate.m_message = message;
	candidate.m_message_class = message_class;
	candidate.m_message_size = static_cast<std::uint32_t>(message.size);
	candidate.m_message_crc32 = message_crc32;
	candidate.m_fragment_count = static_cast<std::uint16_t>(fragment_count);
	candidate.m_ok = true;
	result = candidate;
	return true;
}

bool TelemetryFragmenter::fragment(std::size_t index, FragmentSlice& result) const noexcept {
	result = FragmentSlice{};
	if (!m_ok || index >= m_fragment_count) {
		return false;
	}

	const auto offset = index * MaxFragmentPayload;
	const auto payload_size = m_message.size == 0 ? std::size_t{0}
	                                              : std::min(MaxFragmentPayload, m_message.size - offset);

	result.fragment_index = static_cast<std::uint16_t>(index);
	result.fragment_count = m_fragment_count;
	result.fragment_offset = static_cast<std::uint32_t>(offset);
	result.message_size = m_message_size;
	result.message_crc32 = m_message_crc32;
	result.payload = payload_size == 0 ? ByteView{} : ByteView{m_message.data + offset, payload_size};
	return true;
}

} // namespace telemetry::protocol
