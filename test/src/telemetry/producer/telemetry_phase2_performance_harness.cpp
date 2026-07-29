#define main telemetry_native_performance_embedded_main
#include "telemetry_native_performance_runner.cpp"
#undef main

#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/startup_budget.h"

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>

namespace phase2 {

struct Series {
	std::vector<double> tick_ms;
	std::vector<double> keyframe_ms;
	std::uint64_t allocations = 0U;
	std::uint64_t frames = 0U;
	std::uint64_t keyframes = 0U;
};

using Summary = std::map<std::string, std::string>;

Summary load_summary(std::filesystem::path path) {
	path.replace_extension(".summary.txt");
	std::ifstream input(path);
	Summary result;
	std::string line;
	while (std::getline(input, line)) {
		const auto equals = line.find('=');
		if (equals != std::string::npos)
			result.emplace(line.substr(0U, equals), line.substr(equals + 1U));
	}
	return result;
}

bool observed(const Summary& summary, const char* key) {
	const auto found = summary.find(key);
	return found != summary.end() && found->second == "1";
}

bool parse_u64_arg(const char* text, std::uint64_t& value) {
	char* end = nullptr;
	value = std::strtoull(text, &end, 10);
	return text && *text && end && *end == '\0';
}

std::string escape(const std::string& value) {
	std::ostringstream out;
	for (const auto ch : value) {
		if (ch == '\\' || ch == '"') out << '\\';
		out << ch;
	}
	return out.str();
}

std::string file_sha(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	std::vector<std::uint8_t> bytes{
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	telemetry::protocol::Sha256Digest digest{};
	if (input.bad() || !telemetry::protocol::sha256({bytes.data(), bytes.size()}, digest)) return {};
	std::ostringstream output;
	for (const auto byte : digest)
		output << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
	return output.str();
}

std::vector<std::string> split(const std::string& line) {
	std::vector<std::string> fields;
	std::size_t begin = 0U;
	while (begin <= line.size()) {
		const auto comma = line.find(',', begin);
		fields.push_back(line.substr(begin, comma == std::string::npos ? comma : comma - begin));
		if (comma == std::string::npos) break;
		begin = comma + 1U;
	}
	return fields;
}

bool load_series(const std::filesystem::path& path, Series& result) {
	std::ifstream input(path);
	std::string line;
	if (!std::getline(input, line)) return false;
	while (std::getline(input, line)) {
		const auto fields = split(line);
		if (fields.size() != 17U) return false;
		const auto tick = std::strtoull(fields[1].c_str(), nullptr, 10);
		const auto allocations = std::strtoull(fields[12].c_str(), nullptr, 10);
		const auto keyframe = fields[16] == "1";
		result.tick_ms.push_back(static_cast<double>(tick) / 1000000.0);
		if (keyframe) result.keyframe_ms.push_back(static_cast<double>(tick) / 1000000.0);
		result.allocations = std::max(result.allocations, allocations);
		++result.frames;
		result.keyframes += keyframe ? 1U : 0U;
	}
	return !result.tick_ms.empty();
}

double nearest_rank(std::vector<double> values, double percentile) {
	if (values.empty()) return std::numeric_limits<double>::infinity();
	std::sort(values.begin(), values.end());
	const auto rank = std::max<std::size_t>(1U,
		static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(values.size()))));
	return values[std::min(rank, values.size()) - 1U];
}

double maximum(const std::vector<double>& values) {
	return values.empty() ? std::numeric_limits<double>::infinity()
		: *std::max_element(values.begin(), values.end());
}

double median(const std::vector<double>& values) { return nearest_rank(values, 0.5); }

std::uint64_t derived_seed(std::uint64_t base, const std::string& name) {
	std::uint64_t value = base ^ 1469598103934665603ULL;
	for (const auto ch : name) {
		value ^= static_cast<unsigned char>(ch);
		value *= 1099511628211ULL;
	}
	return value;
}

int run_native(const std::string& workload, const std::filesystem::path& output,
	std::uint64_t samples, std::uint64_t warmup, std::uint64_t seed,
	std::uint64_t wall_seconds, std::uint64_t warmup_wall_seconds) {
	auto summary = output;
	summary.replace_extension(".summary.txt");
	std::vector<std::string> args{
		"telemetry_native_performance_runner", "--workload", workload,
		"--output", output.string(), "--summary", summary.string(),
		"--samples", std::to_string(samples),
		"--warmup", std::to_string(warmup), "--flight-hz", "30",
		"--seed", std::to_string(seed), "--diagnostic", "0",
		"--wall-seconds", std::to_string(wall_seconds),
		"--warmup-wall-seconds", std::to_string(warmup_wall_seconds)};
	std::vector<char*> raw;
	for (auto& arg : args) raw.push_back(arg.data());
	return telemetry_native_performance_embedded_main(static_cast<int>(raw.size()), raw.data());
}

const char* environment(const char* name) {
	const auto* value = std::getenv(name);
	return value && *value ? value : "unavailable";
}

} // namespace phase2

int main(int argc, char** argv) {
	std::uint64_t warmup = 0U, measure = 0U, frames = 0U, flight = 0U, systems = 0U,
		keyframes = 0U, seed = 0U, soak_seconds = 0U, soak_seed = 0U;
	const char *report = nullptr, *soak_dir = nullptr;
	bool smoke = false;
	for (int index = 1; index < argc; ++index) {
		if (!std::strcmp(argv[index], "--smoke")) { smoke = true; continue; }
		if (index + 1 >= argc) return 2;
		const auto* key = argv[index];
		const auto* value = argv[++index];
		auto numeric = [&](std::uint64_t& target) { return phase2::parse_u64_arg(value, target); };
		if (!std::strcmp(key, "--warmup-seconds")) { if (!numeric(warmup)) return 2; }
		else if (!std::strcmp(key, "--measure-seconds")) { if (!numeric(measure)) return 2; }
		else if (!std::strcmp(key, "--minimum-frames")) { if (!numeric(frames)) return 2; }
		else if (!std::strcmp(key, "--minimum-flight-ticks")) { if (!numeric(flight)) return 2; }
		else if (!std::strcmp(key, "--minimum-system-ticks")) { if (!numeric(systems)) return 2; }
		else if (!std::strcmp(key, "--minimum-keyframes")) { if (!numeric(keyframes)) return 2; }
		else if (!std::strcmp(key, "--seed")) { if (!numeric(seed)) return 2; }
		else if (!std::strcmp(key, "--soak-seconds")) { if (!numeric(soak_seconds)) return 2; }
		else if (!std::strcmp(key, "--soak-seed")) { if (!numeric(soak_seed)) return 2; }
		else if (!std::strcmp(key, "--soak-report-dir")) soak_dir = value;
		else if (!std::strcmp(key, "--report")) report = value;
		else return 2;
	}
	if (smoke) {
		warmup = 0U; measure = 0U; frames = 64U; flight = systems = keyframes = 1U;
		seed = soak_seed = 4242U; soak_seconds = 0U;
	}
	if (!report || !soak_dir) return 2;
	if (!smoke && (warmup != 60U || measure < 1800U || frames < 100000U ||
		flight < 54000U || systems < 18000U || keyframes < 900U ||
		seed != 4242U || soak_seconds != 1800U || soak_seed != 4242U))
		return 2;

	const std::filesystem::path report_path(report);
	const std::filesystem::path soak_path(soak_dir);
	const auto raw_dir = report_path.parent_path() / "raw";
	std::filesystem::create_directories(raw_dir);
	std::filesystem::create_directories(soak_path);
	const auto started = std::chrono::steady_clock::now();
	const auto sample_count = smoke ? 640U : std::max<std::uint64_t>(frames, measure * 60U);
	const auto warmup_samples = smoke ? 8U : warmup * 30U;
	struct Workload { const char* name; const char* native; };
	const std::array<Workload, 9> workloads{{
		{"noModule", "no-module"},
		{"configAbsent", "config-absent"},
		{"disabledBaseline", "enabled-false"},
		{"flightControl", "active-one-client"},
		{"systemsNominal", "active-one-client"},
		{"maximumBounds", "maximum-bounds"},
		{"fourClientsSystems", "active-four-clients"},
		{"wouldBlockLossResync", "would-block-loss-resync"},
		{"manifestChanged", "manifest-changed"}}};
	std::map<std::string, phase2::Series> series;
	std::map<std::string, phase2::Summary> summaries;
	bool ok = true;
	for (const auto& workload : workloads) {
		const auto path = raw_dir / (std::string(workload.name) + ".csv");
		ok = phase2::run_native(workload.native, path, sample_count, warmup_samples, seed,
			smoke ? 0U : measure, smoke ? 0U : warmup) == 0 && ok;
		ok = phase2::load_series(path, series[workload.name]) && ok;
		summaries.emplace(workload.name, phase2::load_summary(path));
		ok = !summaries[workload.name].empty() && ok;
	}

	const std::array<const char*, 5> soak_names{{
		"continuous-all-blocks", "mission-reentry-respawn", "client-stop-restart",
		"p2-loss-gate-resync", "four-clients-one-slow"}};
	const std::array<const char*, 5> soak_workloads{{
		"endurance", "mission-reentry-respawn", "client-stop-restart",
		"would-block-loss-resync", "four-clients-one-slow"}};
	std::vector<std::string> soak_hashes;
	std::vector<phase2::Series> soak_series(soak_names.size());
	std::vector<phase2::Summary> soak_summaries(soak_names.size());
	std::vector<bool> soak_run_ok(soak_names.size(), false);
	for (std::size_t index = 0U; index < soak_names.size(); ++index) {
		const auto path = soak_path / (std::string(soak_names[index]) + "-raw.csv");
		const auto derived = phase2::derived_seed(soak_seed, soak_names[index]);
		soak_run_ok[index] = phase2::run_native(soak_workloads[index], path,
			smoke ? 640U : soak_seconds * 30U, smoke ? 8U : warmup_samples,
			derived, smoke ? 0U : soak_seconds, smoke ? 0U : warmup) == 0;
		ok = soak_run_ok[index] && ok;
		ok = phase2::load_series(path, soak_series[index]) && ok;
		soak_summaries[index] = phase2::load_summary(path);
		ok = !soak_summaries[index].empty() && ok;
		soak_hashes.push_back(phase2::file_sha(path));
		ok = !soak_hashes.back().empty() && ok;
	}

	const auto p99_flight = phase2::nearest_rank(series["flightControl"].tick_ms, 0.99);
	const auto p99_systems = phase2::nearest_rank(series["systemsNominal"].tick_ms, 0.99);
	const auto p99_four = phase2::nearest_rank(series["fourClientsSystems"].tick_ms, 0.99);
	const auto p99_keyframe = phase2::nearest_rank(series["systemsNominal"].keyframe_ms, 0.99);
	const auto max_keyframe = phase2::maximum(series["systemsNominal"].keyframe_ms);
	const auto baseline_median = phase2::median(series["disabledBaseline"].tick_ms);
	const auto active_median = phase2::median(series["flightControl"].tick_ms);
	const auto regression = baseline_median == 0.0 ? 100.0
		: ((active_median - baseline_median) / baseline_median) * 100.0;
	std::uint64_t allocations = 0U;
	for (const auto& item : series)
		allocations = std::max(allocations, item.second.allocations);
	for (const auto& item : soak_series)
		allocations = std::max(allocations, item.allocations);
	bool observed_no_growth = true;
	for (const auto& item : summaries)
		observed_no_growth = phase2::observed(item.second, "noGrowth") && observed_no_growth;
	for (const auto& item : soak_summaries)
		observed_no_growth = phase2::observed(item, "noGrowth") && observed_no_growth;
	const auto summary_value = [](const phase2::Summary& summary,
								   const char* key) -> std::uint64_t {
		const auto found = summary.find(key);
		if (found == summary.end()) return 0U;
		try {
			return static_cast<std::uint64_t>(std::stoull(found->second));
		} catch (...) {
			return 0U;
		}
	};
	bool soak_contracts_pass = true;
	for (std::size_t index = 0U; index < soak_names.size(); ++index) {
		const auto& summary = soak_summaries[index];
		soak_contracts_pass =
			phase2::observed(summary, "finalStateExact") &&
			phase2::observed(summary, "hashEqual") &&
			summary_value(summary, "oracleStateHash") != 0U &&
			summary_value(summary, "finalStateHash") != 0U &&
			soak_contracts_pass;
		if (index == 0U)
			soak_contracts_pass =
				phase2::observed(summary, "blockCoverageComplete") &&
				summary_value(summary, "blockCoverageCount") == 11U &&
				soak_contracts_pass;
		if (index == 1U || index == 2U)
			soak_contracts_pass =
				summary_value(summary, "events") == 3U &&
				summary_value(summary, "cycles") == 3U &&
				phase2::observed(summary, "idsNotReused") &&
				soak_contracts_pass;
		if (index == 1U)
			soak_contracts_pass =
				summary_value(summary, "respawns") == 3U &&
				summary_value(summary, "reentries") == 3U &&
				soak_contracts_pass;
		if (index == 2U)
			soak_contracts_pass =
				summary_value(summary, "restarts") == 3U &&
				soak_contracts_pass;
	}
	using detail::NativeSessionRuntimeTestAccess;
	using detail::TelemetryPhase2MemoryScope;
	const bool shared_exact = NativeSessionRuntimeTestAccess::phase2_owned_scope_within_cap(
		TelemetryPhase2MemoryScope::Shared, detail::Phase2SharedOwnedCapBytes);
	const bool shared_plus_one = !NativeSessionRuntimeTestAccess::phase2_owned_scope_within_cap(
		TelemetryPhase2MemoryScope::Shared, detail::Phase2SharedOwnedCapBytes + 1U);
	const bool client_exact = NativeSessionRuntimeTestAccess::phase2_owned_scope_within_cap(
		TelemetryPhase2MemoryScope::ClientTotal, detail::Phase2ClientOwnedCapBytes);
	const bool client_plus_one = !NativeSessionRuntimeTestAccess::phase2_owned_scope_within_cap(
		TelemetryPhase2MemoryScope::ClientTotal, detail::Phase2ClientOwnedCapBytes + 1U);
	const bool process_exact = NativeSessionRuntimeTestAccess::phase2_owned_scope_within_cap(
		TelemetryPhase2MemoryScope::ProcessTotal, detail::Phase2ProcessOwnedCapBytes);
	const bool process_plus_one = !NativeSessionRuntimeTestAccess::phase2_owned_scope_within_cap(
		TelemetryPhase2MemoryScope::ProcessTotal, detail::Phase2ProcessOwnedCapBytes + 1U);
	const bool budgets_pass = shared_exact && shared_plus_one && client_exact &&
		client_plus_one && process_exact && process_plus_one;
	const auto elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - started).count();
	const auto minimum_elapsed = smoke ? 0.0
		: static_cast<double>(workloads.size() * (warmup + measure) +
			soak_names.size() * (warmup + soak_seconds));
	const bool timing_pass = p99_flight <= 0.25 && p99_systems <= 0.75 &&
		p99_keyframe <= 2.0 && max_keyframe <= 5.0 && p99_four <= 1.50 &&
		regression < 2.0;
	const bool metadata_complete =
		std::strcmp(phase2::environment("FSO_PHASE2_POWER_MODE"), "unavailable") != 0 &&
		std::strcmp(phase2::environment("FSO_PHASE2_TEMPERATURE_LOG"), "unavailable") != 0 &&
		std::strcmp(phase2::environment("FSO_PHASE2_THROTTLE_LOG"), "unavailable") != 0;
	const bool evidence_pass = ok && timing_pass && allocations == 0U &&
		observed_no_growth && budgets_pass && soak_contracts_pass;
	const bool smoke_pass = ok && allocations == 0U &&
		observed_no_growth && budgets_pass && soak_contracts_pass;
	const bool certified = !smoke && evidence_pass &&
		elapsed >= minimum_elapsed && metadata_complete;

	std::filesystem::create_directories(report_path.parent_path());
	std::ofstream out(report_path, std::ios::out | std::ios::trunc);
	if (!out) return 5;
	out << std::setprecision(9)
		<< "{\n\"schema\":\"fs2open.telemetry.phase2.performance-harness.v1\","
		<< "\n\"status\":\"" << (smoke ? "smoke-only" : (certified ? "passed" : "failed")) << "\","
		<< "\n\"certificationEligible\":" << (!smoke ? "true" : "false") << ','
		<< "\n\"measurementProtocol\":{\"configuration\":\"Release\",\"clock\":\"steady_clock\","
		<< "\"wallElapsedSeconds\":" << elapsed << ",\"requiredWallElapsedSeconds\":" << minimum_elapsed
		<< ",\"warmupSeconds\":" << warmup << ",\"measuredSeconds\":" << measure
		<< ",\"minimumFrames\":" << frames << ",\"minimumFlightTicks\":" << flight
		<< ",\"minimumSystemTicks\":" << systems << ",\"minimumKeyframes\":" << keyframes
		<< ",\"observedFrames\":" << series["flightControl"].frames
		<< ",\"observedFlightTicks\":" << series["flightControl"].frames
		<< ",\"observedSystemTicks\":" << series["systemsNominal"].frames
		<< ",\"observedKeyframes\":" << series["systemsNominal"].keyframes
		<< ",\"percentile\":\"nearest-rank\",\"outliersRemoved\":0},"
		<< "\n\"host\":{\"compiler\":\"" << phase2::escape(
#if defined(_MSC_VER)
			"MSVC-" + std::to_string(_MSC_VER)
#else
			"non-MSVC"
#endif
		) << "\",\"powerMode\":\"" << phase2::escape(phase2::environment("FSO_PHASE2_POWER_MODE"))
		<< "\",\"temperatureLog\":\"" << phase2::escape(phase2::environment("FSO_PHASE2_TEMPERATURE_LOG"))
		<< "\",\"throttleLog\":\"" << phase2::escape(phase2::environment("FSO_PHASE2_THROTTLE_LOG")) << "\"},"
		<< "\n\"timingsMs\":{\"flightControl\":{\"p99\":" << p99_flight
		<< "},\"systemsNominal\":{\"p99\":" << p99_systems
		<< "},\"keyframe\":{\"p99\":" << p99_keyframe << ",\"max\":" << max_keyframe
		<< "},\"fourClientsSystems\":{\"p99\":" << p99_four
		<< "},\"activeControlMedianRegressionPercent\":" << regression << "},"
		<< "\n\"rawSamples\":[";
	for (std::size_t index = 0U; index < workloads.size(); ++index) {
		const auto path = raw_dir / (std::string(workloads[index].name) + ".csv");
		out << (index ? "," : "") << "{\"workload\":\"" << workloads[index].name
			<< "\",\"path\":\"" << phase2::escape(path.string()) << "\",\"sha256\":\""
			<< phase2::file_sha(path) << "\",\"count\":" << series[workloads[index].name].frames << "}";
	}
	out << "],\n\"allocations\":{\"afterReady\":" << allocations
		<< ",\"steadyStateGrowthBytes\":" << (observed_no_growth ? 0 : 1) << "},"
		<< "\n\"budgets\":{\"sharedBytes\":{\"limit\":" << detail::Phase2SharedOwnedCapBytes
		<< ",\"exactAccepted\":" << (shared_exact ? "true" : "false")
		<< ",\"plusOneRejectedBeforeBind\":" << (shared_plus_one ? "true" : "false")
		<< "},\"perClientBytes\":{\"limit\":" << detail::Phase2ClientOwnedCapBytes
		<< ",\"exactAccepted\":" << (client_exact ? "true" : "false")
		<< ",\"plusOneRejectedBeforeBind\":" << (client_plus_one ? "true" : "false")
		<< "},\"processFourClientsBytes\":{\"limit\":" << detail::Phase2ProcessOwnedCapBytes
		<< ",\"exactAccepted\":" << (process_exact ? "true" : "false")
		<< ",\"plusOneRejectedBeforeBind\":" << (process_plus_one ? "true" : "false")
		<< "}},\n\"soaks\":[";
	for (std::size_t index = 0U; index < soak_names.size(); ++index) {
		const auto& summary = soak_summaries[index];
		out << (index ? "," : "") << "{\"name\":\"" << soak_names[index]
			<< "\",\"workload\":\"" << soak_workloads[index]
			<< "\",\"baseSeed\":" << soak_seed
			<< ",\"seedDerivation\":\"(4242,canonical-scenario-name)\",\"derivedStream\":\""
			<< std::hex << phase2::derived_seed(soak_seed, soak_names[index]) << std::dec
			<< "\",\"durationSeconds\":" << soak_seconds << ",\"rawSha256\":\"" << soak_hashes[index]
			<< "\",\"observedEvents\":" << (summary.count("events") ? summary.at("events") : "0")
			<< ",\"cycles\":" << (summary.count("cycles") ? summary.at("cycles") : "0")
			<< ",\"respawns\":" << (summary.count("respawns") ? summary.at("respawns") : "0")
			<< ",\"reentries\":" << (summary.count("reentries") ? summary.at("reentries") : "0")
			<< ",\"restarts\":" << (summary.count("restarts") ? summary.at("restarts") : "0")
			<< ",\"blockCoverage\":{\"mask\":"
			<< (summary.count("blockCoverageMask") ? summary.at("blockCoverageMask") : "0")
			<< ",\"count\":"
			<< (summary.count("blockCoverageCount") ? summary.at("blockCoverageCount") : "0")
			<< ",\"complete\":"
			<< (phase2::observed(summary, "blockCoverageComplete") ? "true" : "false")
			<< "},\"stateHash\":{\"oracle\":\""
			<< (summary.count("oracleStateHash") ? summary.at("oracleStateHash") : "0")
			<< "\",\"final\":\""
			<< (summary.count("finalStateHash") ? summary.at("finalStateHash") : "0")
			<< "\",\"equal\":"
			<< (phase2::observed(summary, "hashEqual") ? "true" : "false")
			<< "}"
			<< ",\"noLeak\":" << (soak_series[index].allocations == 0U ? "true" : "false")
			<< ",\"noGrowth\":" << (phase2::observed(summary, "noGrowth") ? "true" : "false")
			<< ",\"noDeadlock\":" << (soak_run_ok[index] ? "true" : "false")
			<< ",\"purgedToZero\":" << (phase2::observed(summary, "shutdownPurged") ? "true" : "false")
			<< ",\"idsNotReused\":" << (phase2::observed(summary, "idsNotReused") ? "true" : "false")
			<< ",\"finalStateExact\":" << (phase2::observed(summary, "finalStateExact") ? "true" : "false")
			<< ",\"shutdownBounded\":" << (phase2::observed(summary, "shutdownPurged") ? "true" : "false") << "}";
	}
	out << "]}\n";
	return (smoke ? smoke_pass : certified) ? 0 : 4;
}
