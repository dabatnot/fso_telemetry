#include "telemetry/protocol/telemetry_sha256.h"

#include <algorithm>
#include <array>
#include <limits>

namespace telemetry::protocol {

namespace {

constexpr std::array<std::uint32_t, 8> InitialState{
	0x6a09e667U,
	0xbb67ae85U,
	0x3c6ef372U,
	0xa54ff53aU,
	0x510e527fU,
	0x9b05688cU,
	0x1f83d9abU,
	0x5be0cd19U,
};

constexpr std::array<std::uint32_t, 64> RoundConstants{
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
	0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
	0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
	0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
	0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
	0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
	0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
	0xc67178f2U,
};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned int count) noexcept {
	return (value >> count) | (value << (32U - count));
}

std::uint32_t load_big_endian_u32(const std::uint8_t* bytes) noexcept {
	return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
	       (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

void store_big_endian_u32(std::uint32_t value, std::uint8_t* output) noexcept {
	output[0] = static_cast<std::uint8_t>(value >> 24U);
	output[1] = static_cast<std::uint8_t>(value >> 16U);
	output[2] = static_cast<std::uint8_t>(value >> 8U);
	output[3] = static_cast<std::uint8_t>(value);
}

} // namespace

Sha256::Sha256() noexcept {
	reset();
}

bool Sha256::update(ByteView bytes) noexcept {
	if (!m_ok || m_finalized || (bytes.size != 0 && bytes.data == nullptr)) {
		m_ok = false;
		return false;
	}

	const auto byte_count = static_cast<std::uint64_t>(bytes.size);
	constexpr auto MaximumMessageBytes = std::numeric_limits<std::uint64_t>::max() / 8U;
	if (static_cast<std::size_t>(byte_count) != bytes.size || byte_count > MaximumMessageBytes - m_total_size) {
		m_ok = false;
		return false;
	}
	m_total_size += byte_count;
	if (bytes.empty()) {
		return true;
	}

	std::size_t consumed = 0;
	if (m_buffer_size != 0) {
		const auto copied = std::min(BlockSize - m_buffer_size, bytes.size);
		std::copy_n(bytes.data, copied, m_buffer.data() + m_buffer_size);
		m_buffer_size += copied;
		consumed += copied;
		if (m_buffer_size == BlockSize) {
			transform(m_buffer.data());
			m_buffer_size = 0;
		}
	}

	while (bytes.size - consumed >= BlockSize) {
		transform(bytes.data + consumed);
		consumed += BlockSize;
	}

	const auto remaining = bytes.size - consumed;
	if (remaining != 0) {
		std::copy_n(bytes.data + consumed, remaining, m_buffer.data());
		m_buffer_size = remaining;
	}

	return true;
}

bool Sha256::finalize(Sha256Digest& digest) noexcept {
	digest.fill(0);
	if (!m_ok) {
		return false;
	}
	if (m_finalized) {
		digest = m_digest;
		return true;
	}

	m_buffer[m_buffer_size++] = 0x80U;
	if (m_buffer_size > 56) {
		std::fill(m_buffer.begin() + static_cast<std::ptrdiff_t>(m_buffer_size), m_buffer.end(), std::uint8_t{0});
		transform(m_buffer.data());
		m_buffer_size = 0;
	}
	std::fill(m_buffer.begin() + static_cast<std::ptrdiff_t>(m_buffer_size),
	          m_buffer.begin() + 56,
	          std::uint8_t{0});

	const auto bit_length = m_total_size * 8U;
	for (std::size_t index = 0; index < 8; ++index) {
		m_buffer[56 + index] = static_cast<std::uint8_t>(bit_length >> (56U - static_cast<unsigned int>(index) * 8U));
	}
	transform(m_buffer.data());
	m_buffer_size = 0;

	for (std::size_t index = 0; index < m_state.size(); ++index) {
		store_big_endian_u32(m_state[index], m_digest.data() + index * sizeof(std::uint32_t));
	}
	m_finalized = true;
	digest = m_digest;
	return true;
}

void Sha256::reset() noexcept {
	m_state = InitialState;
	m_buffer.fill(0);
	m_digest.fill(0);
	m_total_size = 0;
	m_buffer_size = 0;
	m_ok = true;
	m_finalized = false;
}

void Sha256::transform(const std::uint8_t* block) noexcept {
	std::array<std::uint32_t, 64> schedule{};
	for (std::size_t index = 0; index < 16; ++index) {
		schedule[index] = load_big_endian_u32(block + index * sizeof(std::uint32_t));
	}
	for (std::size_t index = 16; index < schedule.size(); ++index) {
		const auto s0 = rotate_right(schedule[index - 15], 7U) ^ rotate_right(schedule[index - 15], 18U) ^
		                (schedule[index - 15] >> 3U);
		const auto s1 = rotate_right(schedule[index - 2], 17U) ^ rotate_right(schedule[index - 2], 19U) ^
		                (schedule[index - 2] >> 10U);
		schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
	}

	auto a = m_state[0];
	auto b = m_state[1];
	auto c = m_state[2];
	auto d = m_state[3];
	auto e = m_state[4];
	auto f = m_state[5];
	auto g = m_state[6];
	auto h = m_state[7];

	for (std::size_t index = 0; index < schedule.size(); ++index) {
		const auto sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
		const auto choice = (e & f) ^ (~e & g);
		const auto temporary1 = h + sum1 + choice + RoundConstants[index] + schedule[index];
		const auto sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
		const auto majority = (a & b) ^ (a & c) ^ (b & c);
		const auto temporary2 = sum0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}

	m_state[0] += a;
	m_state[1] += b;
	m_state[2] += c;
	m_state[3] += d;
	m_state[4] += e;
	m_state[5] += f;
	m_state[6] += g;
	m_state[7] += h;
}

bool sha256(ByteView bytes, Sha256Digest& digest) noexcept {
	digest.fill(0);
	Sha256 calculator;
	if (!calculator.update(bytes)) {
		return false;
	}
	return calculator.finalize(digest);
}

} // namespace telemetry::protocol
