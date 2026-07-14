#pragma once

#include "telemetry/protocol/telemetry_datagram.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

struct FragmentSlice {
	std::uint16_t fragment_index = 0;
	std::uint16_t fragment_count = 0;
	std::uint32_t fragment_offset = 0;
	std::uint32_t message_size = 0;
	std::uint32_t message_crc32 = 0;
	ByteView payload;
};

// A bounded, zero-copy view of the canonical FSTL fragmentation of one logical
// payload. The source payload must remain alive while slices are inspected.
class TelemetryFragmenter {
  public:
	TelemetryFragmenter() noexcept = default;

	static bool create(ByteView message, MessageSizeClass message_class, TelemetryFragmenter& result) noexcept;

	bool fragment(std::size_t index, FragmentSlice& result) const noexcept;

	bool ok() const noexcept { return m_ok; }
	MessageSizeClass message_class() const noexcept { return m_message_class; }
	std::uint32_t message_size() const noexcept { return m_message_size; }
	std::uint32_t message_crc32() const noexcept { return m_message_crc32; }
	std::uint16_t fragment_count() const noexcept { return m_fragment_count; }

  private:
	ByteView m_message;
	MessageSizeClass m_message_class = MessageSizeClass::Invalid;
	std::uint32_t m_message_size = 0;
	std::uint32_t m_message_crc32 = 0;
	std::uint16_t m_fragment_count = 0;
	bool m_ok = false;
};

} // namespace telemetry::protocol
