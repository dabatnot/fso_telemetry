#pragma once

#include "telemetry/protocol/telemetry_protocol_types.h"

#include <array>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::size_t Sha256DigestSize = 32;
using Sha256Digest = std::array<std::uint8_t, Sha256DigestSize>;

class Sha256 {
  public:
	Sha256() noexcept;

	bool update(ByteView bytes) noexcept;
	bool finalize(Sha256Digest& digest) noexcept;
	void reset() noexcept;

	bool ok() const noexcept { return m_ok; }
	bool finalized() const noexcept { return m_finalized; }

  private:
	static constexpr std::size_t BlockSize = 64;

	void transform(const std::uint8_t* block) noexcept;

	std::array<std::uint32_t, 8> m_state{};
	std::array<std::uint8_t, BlockSize> m_buffer{};
	Sha256Digest m_digest{};
	std::uint64_t m_total_size = 0;
	std::size_t m_buffer_size = 0;
	bool m_ok = true;
	bool m_finalized = false;
};

bool sha256(ByteView bytes, Sha256Digest& digest) noexcept;

} // namespace telemetry::protocol
