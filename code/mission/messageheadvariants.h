#pragma once

#include "globalincs/pstypes.h"

enum class MessageHeadPersonaClass {
	None,
	WingmanSupport,
	Command,
	Large,
	Other,
};

struct MessageHeadVariant {
	SCP_string filename;
	bool subhead_selected = false;
	bool death_scream = false;
};

MessageHeadVariant select_message_head_variant(const SCP_string& base_name,
	bool base_exists,
	bool suffix_eligible,
	bool use_new_suffixes,
	MessageHeadPersonaClass persona_class,
	bool death_scream,
	int selector);

SCP_vector<SCP_string> enumerate_message_head_variants(const SCP_string& base_name,
	bool base_exists,
	bool suffix_eligible);
