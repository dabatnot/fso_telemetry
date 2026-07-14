#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

constexpr std::uintmax_t MaximumReplayInputSize = 2'097'220U;

bool replay_file(const std::filesystem::path& path)
{
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || size > MaximumReplayInputSize) {
		std::cerr << "Skipping unreadable or oversized fuzz seed: " << path.string() << '\n';
		return false;
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		std::cerr << "Cannot open fuzz seed: " << path.string() << '\n';
		return false;
	}

	std::vector<std::uint8_t> input(static_cast<std::size_t>(size));
	if (!input.empty()) {
		stream.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(input.size()));
		if (!stream) {
			std::cerr << "Cannot read fuzz seed: " << path.string() << '\n';
			return false;
		}
	}
	LLVMFuzzerTestOneInput(input.empty() ? nullptr : input.data(), input.size());
	return true;
}

} // namespace

int main(int argc, char** argv)
{
	std::vector<std::filesystem::path> files;
	for (int index = 1; index < argc; ++index) {
		const std::filesystem::path input(argv[index]);
		std::error_code error;
		if (std::filesystem::is_regular_file(input, error)) {
			files.push_back(input);
		} else if (!error && std::filesystem::is_directory(input, error)) {
			for (std::filesystem::recursive_directory_iterator iterator(input, error), end;
				 !error && iterator != end;
				 iterator.increment(error)) {
				if (iterator->is_regular_file(error) && !error) {
					files.push_back(iterator->path());
				}
			}
		}
	}

	std::sort(files.begin(), files.end());
	if (files.empty()) {
		LLVMFuzzerTestOneInput(nullptr, 0);
		return 0;
	}

	for (const auto& file : files) {
		if (!replay_file(file)) {
			return 1;
		}
	}
	std::cout << "Replayed " << files.size() << " telemetry fuzz seeds.\n";
	return 0;
}
