#include "cfile/cfile.h"

CFileLocation cf_find_file_location(
	const char*, int, std::uint32_t)
{
	return CFileLocation{};
}

CFILE* _cfopen_special(
	const char*, int, const CFileLocation&, const char*, int)
{
	return nullptr;
}

int cfread(void*, int, int, CFILE*)
{
	return 0;
}

int cfclose(CFILE*)
{
	return 0;
}
