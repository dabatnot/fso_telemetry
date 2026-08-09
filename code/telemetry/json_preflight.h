#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace telemetry::detail {

enum class JsonDepthPreflightResult : std::uint8_t {
	WithinLimit = 0,
	MaximumDepthExceeded,
};

// This bounded lexical pass runs before Jansson's recursive descent parser.
// It deliberately handles only the property needed for stack safety; Jansson
// remains authoritative for JSON syntax, balanced delimiters, duplicates and EOF.
inline JsonDepthPreflightResult preflight_json_container_depth(std::string_view input,
	std::size_t maximum_depth) noexcept
{
	std::size_t depth = 0;
	bool in_string = false;
	bool escaped = false;

	for (const auto character : input) {
		if (in_string) {
			if (escaped) {
				escaped = false;
				continue;
			}
			if (character == '\\') {
				escaped = true;
			} else if (character == '"') {
				in_string = false;
			}
			continue;
		}

		if (character == '"') {
			in_string = true;
		} else if (character == '{' || character == '[') {
			if (depth >= maximum_depth) {
				return JsonDepthPreflightResult::MaximumDepthExceeded;
			}
			++depth;
		} else if ((character == '}' || character == ']') && depth != 0U) {
			--depth;
		}
	}

	return JsonDepthPreflightResult::WithinLimit;
}

} // namespace telemetry::detail
