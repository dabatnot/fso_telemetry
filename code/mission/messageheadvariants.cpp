#include "mission/messageheadvariants.h"

#include <algorithm>
#include <cctype>

namespace {

bool starts_with_ci(const SCP_string& value, const char* prefix)
{
	const auto length = std::char_traits<char>::length(prefix);
	if (value.size() < length) return false;
	for (std::size_t index = 0; index < length; ++index) {
		if (std::tolower(static_cast<unsigned char>(value[index])) !=
			std::tolower(static_cast<unsigned char>(prefix[index]))) return false;
	}
	return true;
}

} // namespace

MessageHeadVariant select_message_head_variant(const SCP_string& base_name,
	bool base_exists,
	bool suffix_eligible,
	bool use_new_suffixes,
	MessageHeadPersonaClass persona_class,
	bool death_scream,
	int selector)
{
	MessageHeadVariant result{base_name, false, false};
	if (base_exists || !suffix_eligible) return result;
	selector = std::max(0, selector);
	if (use_new_suffixes) {
		const bool command = persona_class == MessageHeadPersonaClass::Command;
		result.filename += !command && death_scream ? "-death" : "-reg";
		result.subhead_selected = true;
		return result;
	}

	int variant = selector % 2;
	if (persona_class == MessageHeadPersonaClass::WingmanSupport) {
		if (death_scream) {
			variant = 2;
			result.death_scream = true;
		}
		result.subhead_selected = true;
	} else if (persona_class == MessageHeadPersonaClass::Command || persona_class == MessageHeadPersonaClass::Large) {
		variant = selector % ((starts_with_ci(base_name, "head-tp") || starts_with_ci(base_name, "head-vp")) ? 2 : 3);
		result.subhead_selected = true;
	}
	result.filename.push_back(static_cast<char>('a' + variant));
	return result;
}

SCP_vector<SCP_string> enumerate_message_head_variants(const SCP_string& base_name,
	bool base_exists,
	bool suffix_eligible)
{
	if (base_exists || !suffix_eligible) return {base_name};
	SCP_vector<SCP_string> result;
	const MessageHeadPersonaClass personas[] = {
		MessageHeadPersonaClass::None,
		MessageHeadPersonaClass::WingmanSupport,
		MessageHeadPersonaClass::Command,
		MessageHeadPersonaClass::Large,
		MessageHeadPersonaClass::Other,
	};
	for (const auto persona : personas) {
		for (const auto newer : {false, true}) {
			for (const auto death : {false, true}) {
				for (int selector = 0; selector < 3; ++selector) {
					result.push_back(select_message_head_variant(base_name,
						false, true, newer, persona, death, selector).filename);
				}
			}
		}
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}
