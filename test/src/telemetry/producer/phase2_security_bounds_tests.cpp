#include "telemetry/capture_scheduler.h"
#include "telemetry/config.h"
#include "telemetry/logging.h"
#include "telemetry/metrics.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/phase2_runtime.h"
#include "telemetry/session_controller.h"
#include "telemetry/startup_budget.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

namespace detail = telemetry::detail;

constexpr std::size_t index(detail::TelemetryPhase2Block value)
{
	return static_cast<std::size_t>(value);
}

constexpr std::size_t index(detail::TelemetryPhase2MemoryScope value)
{
	return static_cast<std::size_t>(value);
}

constexpr std::size_t index(detail::TelemetryCockpitProducerRejection value)
{
	return static_cast<std::size_t>(value);
}

constexpr std::size_t index(detail::TelemetryPhase2CaptureFailure value)
{
	return static_cast<std::size_t>(value);
}

std::string read_source_file(const char* relative)
{
	std::ifstream stream(std::string{FSO_PHASE2_SOURCE_ROOT} + "/" + relative,
		std::ios::binary);
	std::ostringstream contents;
	contents << stream.rdbuf();
	return contents.str();
}

std::size_t occurrence_count(
	const std::string& source, const std::string& needle)
{
	std::size_t count = 0U;
	for (auto offset = source.find(needle);
		 offset != std::string::npos;
		 offset = source.find(needle, offset + needle.size()))
		++count;
	return count;
}

struct Phase2SupportTrackerAbiEntry {
	detail::Phase2CaptureLocalKey capture_key{};
	std::uint32_t signature = 0U;
	detail::ShipSupportPhase previous =
		detail::ShipSupportPhase::None;
	bool active = false;
};

TEST(Phase2SecurityBounds, P2TST001LegacyConfigurationDefaultsSystemsToTenWithoutChangingOtherCadences)
{
	const auto result = detail::parse_telemetry_config_json(
		R"({"schemaVersion":4,"enabled":true,"flightHz":17,"keyframeSeconds":4})");
	ASSERT_EQ(telemetry::ConfigStatus::ValidEnabled, result.status);
	EXPECT_EQ(10U, result.effective.systems_hz);
	EXPECT_EQ(17U, result.effective.flight_hz);
	EXPECT_EQ(4U, result.effective.keyframe_seconds);
	EXPECT_EQ(64U, result.effective.max_datagrams_per_tick);
	EXPECT_FALSE(result.effective.discovery_enabled);
}

TEST(Phase2SecurityBounds, P2TST002SystemsBoundsTypesAndIndependentSchedulersAreFailClosed)
{
	for (const auto rate : {1U, 20U}) {
		const auto json = std::string{
			R"({"schemaVersion":4,"systemsHz":)"} +
			std::to_string(rate) + "}";
		const auto result = detail::parse_telemetry_config_json(json);
		ASSERT_NE(telemetry::ConfigStatus::Invalid, result.status);
		EXPECT_EQ(rate, result.effective.systems_hz);
	}
	for (const auto* json : {
			 R"({"schemaVersion":4,"systemsHz":0})",
			 R"({"schemaVersion":4,"systemsHz":21})",
			 R"({"schemaVersion":4,"systemsHz":10.0})",
			 R"({"schemaVersion":4,"systemsHz":"10"})",
			 R"({"schemaVersion":4,"enabled":true,"flightHz":11,"systemsHz":21})"}) {
		const auto result = detail::parse_telemetry_config_json(json);
		ASSERT_EQ(telemetry::ConfigStatus::Invalid, result.status);
		EXPECT_FALSE(result.effective.enabled);
		EXPECT_EQ(30U, result.effective.flight_hz);
		EXPECT_EQ(10U, result.effective.systems_hz);
	}

	detail::Capture30Hz flight;
	detail::Capture30Hz systems;
	ASSERT_TRUE(flight.configure(7U));
	ASSERT_TRUE(systems.configure(20U, 20U));
	EXPECT_EQ(142'858U, flight.period_us());
	EXPECT_EQ(50'000U, systems.period_us());
	EXPECT_FALSE(systems.configure(21U, 20U));
	EXPECT_TRUE(flight.configure(1U));
	EXPECT_EQ(1'000'000U, flight.period_us());
}

TEST(Phase2SecurityBounds, P2TST003UnknownDuplicateAndDeepInputsNeverPartiallyApply)
{
	for (const auto* json : {
			 R"({"schemaVersion":4,"enabled":true,"futureSystemsHz":10})",
			 R"({"schemaVersion":4,"enabled":true,"systemsHz":10,"systemsHz":11})",
			 R"({"schemaVersion":4,"enabled":true,"bindAddresses":[[[["127.0.0.1"]]]]})"}) {
		const auto result = detail::parse_telemetry_config_json(json);
		ASSERT_EQ(telemetry::ConfigStatus::Invalid, result.status);
		EXPECT_FALSE(result.effective.enabled);
		EXPECT_EQ(10U, result.effective.systems_hz);
		ASSERT_EQ(2U, result.effective.bind_addresses.size());
		for (std::size_t index = 0U;
			 index < result.effective.bind_addresses.size(); ++index)
			EXPECT_TRUE(result.effective.bind_addresses[index].is_loopback());
	}
}

TEST(Phase2SecurityBounds, P2TST004DisabledConfigurationRetainsThePhase1FastPathDefaults)
{
	const auto absent = detail::load_telemetry_config_from_observation({});
	ASSERT_EQ(telemetry::ConfigStatus::Absent, absent.status);
	EXPECT_FALSE(absent.effective.enabled);
	EXPECT_EQ(30U, absent.effective.flight_hz);
	EXPECT_EQ(10U, absent.effective.systems_hz);

	const auto disabled =
		detail::parse_telemetry_config_json(R"({"schemaVersion":4,"enabled":false})");
	ASSERT_EQ(telemetry::ConfigStatus::ValidDisabled, disabled.status);
	EXPECT_FALSE(disabled.effective.enabled);
	EXPECT_EQ(30U, disabled.effective.flight_hz);
	EXPECT_EQ(10U, disabled.effective.systems_hz);
}

TEST(TelemetryConfigContract, VersionFourHasNoProfileAndRejectsLegacySchemas)
{
	const auto cockpit = detail::parse_telemetry_config_json(
		R"({"schemaVersion":4})");
	ASSERT_EQ(telemetry::ConfigStatus::ValidDisabled, cockpit.status);
	EXPECT_EQ(4U, cockpit.effective.schema_version);

	for (const auto* invalid : {
			 R"({"schemaVersion":1})",
			 R"({"schemaVersion":2,"phase2Profile":"CoreGate"})",
			 R"({"schemaVersion":3,"profile":"CockpitSensors"})",
			 R"({"schemaVersion":4,"profile":"CockpitSensors"})",
			 R"({"schemaVersion":4,"phase2Profile":"CompleteShip"})"}) {
		SCOPED_TRACE(invalid);
		EXPECT_EQ(telemetry::ConfigStatus::Invalid,
			detail::parse_telemetry_config_json(invalid).status);
	}
}

TEST(Phase2SecurityBounds, P2TST005And006BuildSurfaceAndCMakeInventoryAreExplicit)
{
#if defined(_MSC_VER)
	SUCCEED() << "MSVC Debug/Release compiles this exact contract target.";
#else
	SUCCEED() << "The non-Windows build compiles this exact contract target.";
#endif
	const auto cmake = read_source_file("test/src/CMakeLists.txt");
	ASSERT_FALSE(cmake.empty());
	const std::string source =
		"telemetry/producer/phase2_security_bounds_tests.cpp";
	const auto target_begin = cmake.find(
		"add_executable(telemetry_phase2_security_bounds_tests");
	const auto target_end = cmake.find(
		"target_compile_features(telemetry_phase2_security_bounds_tests",
		target_begin);
	ASSERT_NE(std::string::npos, target_begin);
	ASSERT_NE(std::string::npos, target_end);
	const auto target_sources =
		cmake.substr(target_begin, target_end - target_begin);
	EXPECT_NE(std::string::npos, target_sources.find(source));
	EXPECT_EQ(std::string::npos, target_sources.find("GLOB"));
}

TEST(Phase2SecurityBounds, P2TST049ExactOwnedCapsPassAndEveryPlusOneFailsBeforeBind)
{
	const detail::Phase2OwnedBudgetRequest shared_and_process_exact{
		detail::Phase2SharedOwnedCapBytes,
		(detail::Phase2ProcessOwnedCapBytes -
			detail::Phase2SharedOwnedCapBytes) /
			detail::TelemetryMetricsMaxClients,
		detail::TelemetryMetricsMaxClients};
	const auto shared_accepted =
		detail::calculate_phase2_owned_budget(shared_and_process_exact);
	ASSERT_EQ(detail::StartupBudgetError::None, shared_accepted.error);
	EXPECT_EQ(134'217'728U, shared_accepted.shared_owned_bytes);
	EXPECT_EQ(67'108'864U, shared_accepted.client_owned_bytes);
	EXPECT_EQ(402'653'184U, shared_accepted.process_owned_bytes);

	const detail::Phase2OwnedBudgetRequest client_and_process_exact{
		detail::Phase2ProcessOwnedCapBytes -
			detail::TelemetryMetricsMaxClients *
				detail::Phase2ClientOwnedCapBytes,
		detail::Phase2ClientOwnedCapBytes,
		detail::TelemetryMetricsMaxClients};
	const auto client_accepted =
		detail::calculate_phase2_owned_budget(client_and_process_exact);
	ASSERT_EQ(detail::StartupBudgetError::None, client_accepted.error);
	EXPECT_EQ(67'108'864U, client_accepted.shared_owned_bytes);
	EXPECT_EQ(83'886'080U, client_accepted.client_owned_bytes);
	EXPECT_EQ(402'653'184U, client_accepted.process_owned_bytes);

	for (const auto scope : {
			 detail::TelemetryPhase2MemoryScope::Shared,
			 detail::TelemetryPhase2MemoryScope::ClientTotal,
			 detail::TelemetryPhase2MemoryScope::ProcessTotal}) {
		const auto cap =
			scope == detail::TelemetryPhase2MemoryScope::Shared
			? detail::Phase2SharedOwnedCapBytes
			: scope ==
					detail::TelemetryPhase2MemoryScope::ClientTotal
			? detail::Phase2ClientOwnedCapBytes
			: detail::Phase2ProcessOwnedCapBytes;
		EXPECT_TRUE(detail::phase2_owned_scope_within_cap(scope, cap));
		EXPECT_FALSE(detail::phase2_owned_scope_within_cap(
			scope, cap + 1U))
			<< "Each +1 is isolated through the arithmetic-only scope seam.";
	}
	EXPECT_FALSE(detail::phase2_owned_scope_within_cap(
		detail::TelemetryPhase2MemoryScope::Count, 0U));

	const detail::Phase2OwnedBudgetRequest overflow{
		1U, std::numeric_limits<std::size_t>::max(),
		detail::TelemetryMetricsMaxClients};
	EXPECT_EQ(detail::StartupBudgetError::ArithmeticOverflow,
		detail::calculate_phase2_owned_budget(overflow).error);
}

TEST(Phase3SecurityBounds, ExactOwnedCapsAndReplicationScratchCoverFourThousandNinetySixContacts)
{
	static_assert(detail::Phase3SharedOwnedCapBytes == 167'772'160U);
	static_assert(detail::Phase3ClientOwnedCapBytes == 92'274'688U);
	static_assert(detail::Phase3ProcessOwnedCapBytes == 536'870'912U);
	static_assert(detail::Phase3CockpitSensorsDeltaBytes ==
		telemetry::protocol::MaxStateMessageSize);
	static_assert(telemetry::protocol::MaxIncrementalDirtyStateAtomCount >=
		10U + 10U * 64U + 4096U + 4096U);

	const detail::Phase2OwnedBudgetRequest exact{
		detail::Phase3SharedOwnedCapBytes,
		detail::Phase3ClientOwnedCapBytes,
		detail::TelemetryMetricsMaxClients};
	const auto accepted = detail::calculate_phase3_owned_budget(exact);
	ASSERT_EQ(detail::StartupBudgetError::None, accepted.error);
	EXPECT_EQ(167'772'160U, accepted.shared_owned_bytes);
	EXPECT_EQ(92'274'688U, accepted.client_owned_bytes);
	EXPECT_EQ(369'098'752U, accepted.clients_owned_bytes);
	EXPECT_EQ(536'870'912U, accepted.process_owned_bytes);

	for (const auto scope : {
			 detail::TelemetryPhase2MemoryScope::Shared,
			 detail::TelemetryPhase2MemoryScope::ClientTotal,
			 detail::TelemetryPhase2MemoryScope::ProcessTotal}) {
		const auto cap =
			scope == detail::TelemetryPhase2MemoryScope::Shared
			? detail::Phase3SharedOwnedCapBytes
			: scope == detail::TelemetryPhase2MemoryScope::ClientTotal
			? detail::Phase3ClientOwnedCapBytes
			: detail::Phase3ProcessOwnedCapBytes;
		EXPECT_TRUE(detail::phase3_owned_scope_within_cap(scope, cap));
		EXPECT_FALSE(detail::phase3_owned_scope_within_cap(
			scope, cap + 1U));
	}
	EXPECT_FALSE(detail::phase3_owned_scope_within_cap(
		detail::TelemetryPhase2MemoryScope::Count, 0U));

	const detail::Phase2OwnedBudgetRequest invalid_clients{
		0U, 0U, detail::TelemetryMetricsMaxClients + 1U};
	EXPECT_EQ(detail::StartupBudgetError::InvalidClientCount,
		detail::calculate_phase3_owned_budget(invalid_clients).error);
	const detail::Phase2OwnedBudgetRequest overflow{
		1U, std::numeric_limits<std::size_t>::max(),
		detail::TelemetryMetricsMaxClients};
	EXPECT_EQ(detail::StartupBudgetError::ArithmeticOverflow,
		detail::calculate_phase3_owned_budget(overflow).error);
}

TEST(Phase2SecurityBounds, P2AC012Phase2MetricsAreClosedBoundedResettableAndHighWatered)
{
	static_assert(std::is_trivially_copyable_v<detail::TelemetryMetricsSnapshot>);
	detail::TelemetryMetrics metrics;
	ASSERT_TRUE(metrics.provision());
	metrics.activate_session(0U, 7U);
	metrics.activate_session(1U, 8U);
	metrics.activate_session(2U, 9U);
	for (std::size_t block = 0U;
		 block < static_cast<std::size_t>(detail::TelemetryPhase2Block::Count);
		 ++block) {
		metrics.observe_phase2_capture(
			static_cast<detail::TelemetryPhase2Block>(block),
			block + 1U);
		metrics.set_phase2_sample_age(0U,
			static_cast<detail::TelemetryPhase2Block>(block),
			100U + block);
	}
	metrics.record_cockpit_producer_rejection(
		detail::TelemetryCockpitProducerRejection::UnsupportedVisibility);
	metrics.record_phase2_capture_failure(
		detail::TelemetryPhase2Block::Identity,
		detail::TelemetryPhase2CaptureFailure::Guard);
	metrics.record_phase2_closure(
		detail::TelemetryPhase2ClosureResult::Created);
	metrics.set_phase2_closure(1U, 2U, 3U, 4U);

	const std::array<std::uint8_t, 9U> records{{
		0xfeU, 0x7fU, 1U, 0U, 3U, 0U, 'a', 'b', 'c'}};
	telemetry::protocol::Sha256Digest digest{};
	ASSERT_TRUE(telemetry::protocol::sha256(
		{records.data(), records.size()}, digest));
	telemetry::protocol::ManifestPartPayload manifest{};
	manifest.manifest_id = 1U;
	manifest.part_count = 1U;
	manifest.transaction_size =
		static_cast<std::uint32_t>(records.size());
	manifest.transaction_sha256 = digest;
	manifest.producer_sample_time_us = 10U;
	manifest.manifest_kind =
		telemetry::protocol::ManifestKind::FullRequired;
	manifest.record_count = 1U;
	manifest.records = {records.data(), records.size()};
	std::array<std::uint8_t,
		telemetry::protocol::ManifestPartPayloadPrefixSize +
			records.size()> encoded_manifest{};
	std::size_t manifest_written = 0U;
	ASSERT_EQ(telemetry::protocol::ValidationError::None,
		telemetry::protocol::encode_manifest_part_payload(
			manifest,
			{encoded_manifest.data(), encoded_manifest.size()},
			manifest_written));
	telemetry::protocol::ManifestPartPayload decoded_manifest{};
	ASSERT_EQ(telemetry::protocol::ValidationError::None,
		telemetry::protocol::decode_manifest_part_payload(
			{encoded_manifest.data(), manifest_written},
			decoded_manifest));
	metrics.record_phase2_manifest(0U,
		detail::TelemetryPhase2ManifestResult::Built,
		manifest_written, decoded_manifest.part_count, 17U);
	metrics.record_phase2_manifest(1U,
		detail::TelemetryPhase2ManifestResult::Reused,
		manifest_written, decoded_manifest.part_count, 19U);

	telemetry::protocol::FullSnapshotPartPayload image{};
	image.snapshot_id = 1U;
	image.part_count = 1U;
	image.transaction_size =
		static_cast<std::uint32_t>(records.size());
	image.transaction_sha256 = digest;
	image.producer_sample_time_us = 11U;
	image.required_manifest_id = 1U;
	image.snapshot_flags =
		telemetry::protocol::SnapshotFlagInitial;
	image.record_count = 1U;
	image.records = {records.data(), records.size()};
	std::array<std::uint8_t,
		telemetry::protocol::FullSnapshotPartPayloadPrefixSize +
			records.size()> encoded_image{};
	std::size_t image_written = 0U;
	ASSERT_EQ(telemetry::protocol::ValidationError::None,
		telemetry::protocol::encode_full_snapshot_part_payload(
			image, {encoded_image.data(), encoded_image.size()},
			image_written));
	telemetry::protocol::FullSnapshotPartPayload decoded_image{};
	ASSERT_EQ(telemetry::protocol::ValidationError::None,
		telemetry::protocol::decode_full_snapshot_part_payload(
			{encoded_image.data(), image_written}, decoded_image));
	metrics.set_phase2_session_state(0U,
		decoded_image.record_count, decoded_image.records.size,
		3U, 2U);
	metrics.observe_phase2_image(23U);
	metrics.record_phase2_lifecycle(
		detail::TelemetryPhase2LifecycleKind::Destroyed);
	metrics.record_phase2_support(
		detail::TelemetryPhase2SupportKind::CompletedPrivate);
	metrics.record_phase2_support_coalesced(0U,
		detail::TelemetryPhase2SupportCoalescedKind::
			CompleteThenEndSameEpisode);
	metrics.set_phase2_ring(
		detail::TelemetryPhase2Ring::Cleanup, 64U);
	metrics.record_phase2_ring_overflow(
		detail::TelemetryPhase2Ring::Cleanup);
	metrics.set_phase2_support_latches(0U, 64U);
	metrics.record_phase2_forced_keyframe(0U,
		detail::TelemetryPhase2KeyframeReason::Lifecycle);
	metrics.set_phase2_manifest_generations(0U, 2U);
	metrics.record_phase2_manifest_rebuild_coalesced(0U);
	metrics.record_phase2_source_limit(
		detail::TelemetryPhase2SourceLimit::Subsystems);
	metrics.record_phase2_allocation_after_ready(
		detail::TelemetryPhase2AllocationKind::VectorGrowth);
	metrics.set_phase2_memory(
		detail::TelemetryPhase2MemoryScope::Shared, 100U);
	metrics.set_phase2_memory(
		detail::TelemetryPhase2MemoryScope::Shared, 40U);
	metrics.set_phase2_memory(
		detail::TelemetryPhase2MemoryScope::ClientTotal, 200U);
	metrics.set_phase2_memory(
		detail::TelemetryPhase2MemoryScope::ProcessTotal, 240U);

	auto snapshot = metrics.snapshot();
	for (std::size_t block = 0U;
		 block < static_cast<std::size_t>(detail::TelemetryPhase2Block::Count);
		 ++block) {
		EXPECT_EQ(1U, snapshot.phase2_capture_duration[block].count);
		EXPECT_EQ(snapshot.phase2_capture_duration[block].count,
			snapshot.mission_phase2_capture_duration[block].count);
		EXPECT_EQ(block + 1U,
			snapshot.phase2_capture_duration[block].sum_us);
	}
	EXPECT_EQ(1U, snapshot.cockpit_producer_rejections[
		index(detail::TelemetryCockpitProducerRejection::
			UnsupportedVisibility)]);
	EXPECT_EQ(1U, snapshot.phase2_capture_failures[
		index(detail::TelemetryPhase2Block::Identity)]
		[index(detail::TelemetryPhase2CaptureFailure::Guard)]);
	EXPECT_EQ(40U, snapshot.phase2_memory_bytes[
		index(detail::TelemetryPhase2MemoryScope::Shared)]);
	EXPECT_EQ(100U, snapshot.phase2_memory_high_water[
		index(detail::TelemetryPhase2MemoryScope::Shared)]);
	EXPECT_EQ(decoded_image.record_count,
		snapshot.sessions[0].phase2_image_records);
	EXPECT_EQ(decoded_image.records.size,
		snapshot.sessions[0].phase2_image_bytes);
	EXPECT_EQ(3U, snapshot.sessions[0].phase2_dirty_atoms);
	EXPECT_EQ(decoded_manifest.part_count,
		snapshot.sessions[0].phase2_manifest_parts);
	EXPECT_EQ(manifest_written,
		snapshot.sessions[0].phase2_manifest_bytes);
	EXPECT_EQ(2U, snapshot.sessions[0].phase2_manifest_generations);
	EXPECT_EQ(1U, snapshot.phase2_ring_overflows[
		static_cast<std::size_t>(
			detail::TelemetryPhase2Ring::Cleanup)]);
	EXPECT_EQ(1U, snapshot.phase2_forced_keyframes[
		static_cast<std::size_t>(
			detail::TelemetryPhase2KeyframeReason::Lifecycle)]);
	EXPECT_EQ(snapshot.phase2_lifecycle_events,
		snapshot.mission_phase2_lifecycle_events);
	EXPECT_EQ(snapshot.phase2_support_transitions,
		snapshot.mission_phase2_support_transitions);
	EXPECT_EQ(snapshot.phase2_closure_results,
		snapshot.mission_phase2_closure_results);
	EXPECT_EQ(240U, snapshot.phase2_memory_bytes[
		index(detail::TelemetryPhase2MemoryScope::ProcessTotal)]);

	metrics.reset_session(0U);
	metrics.reset_mission();
	metrics.set_phase2_memory(
		detail::TelemetryPhase2MemoryScope::Shared, 0U);
	snapshot = metrics.snapshot();
	EXPECT_FALSE(snapshot.sessions[0].active);
	EXPECT_EQ(0U, snapshot.sessions[0].phase2_image_records);
	EXPECT_EQ(0U, snapshot.phase2_closure_ships);
	EXPECT_EQ(0U, snapshot.mission_phase2_capture_duration[
		index(detail::TelemetryPhase2Block::Identity)].count);
	EXPECT_EQ(1U, snapshot.phase2_capture_duration[
		index(detail::TelemetryPhase2Block::Identity)].count)
		<< "Mission reset never erases process aggregates.";
	EXPECT_EQ(0U, snapshot.phase2_memory_bytes[
		index(detail::TelemetryPhase2MemoryScope::Shared)]);
	EXPECT_EQ(100U, snapshot.phase2_memory_high_water[
		index(detail::TelemetryPhase2MemoryScope::Shared)]);
}

TEST(Phase2SecurityBounds, P2AC011DiagnosticsContainOnlyClosedNumericLabels)
{
	static_assert(std::is_trivially_copyable_v<detail::TelemetryMetricsSnapshot>);
	static_assert(std::is_standard_layout_v<detail::TelemetryMetricsSnapshot>);
	EXPECT_EQ(8U,
		static_cast<std::size_t>(detail::TelemetryCockpitProducerRejection::Count));
	EXPECT_EQ(7U,
		static_cast<std::size_t>(detail::TelemetryPhase2CaptureFailure::Count));
	EXPECT_EQ(5U,
		static_cast<std::size_t>(detail::TelemetryPhase2ClosureResult::Count));
	EXPECT_EQ(3U,
		static_cast<std::size_t>(detail::TelemetryPhase2ManifestResult::Count));
	EXPECT_EQ(5U,
		static_cast<std::size_t>(
			detail::TelemetryPhase2AllocationKind::Count));
	constexpr std::array<detail::TelemetryPhase2SourceLimit, 10U>
		source_limits{{
			detail::TelemetryPhase2SourceLimit::Ships,
			detail::TelemetryPhase2SourceLimit::Classes,
			detail::TelemetryPhase2SourceLimit::Weapons,
			detail::TelemetryPhase2SourceLimit::Subsystems,
			detail::TelemetryPhase2SourceLimit::ClassBanks,
			detail::TelemetryPhase2SourceLimit::ImageRecords,
			detail::TelemetryPhase2SourceLimit::RecordBytes,
			detail::TelemetryPhase2SourceLimit::TransactionBytes,
			detail::TelemetryPhase2SourceLimit::Parts,
			detail::TelemetryPhase2SourceLimit::Memory,
		}};
	for (std::size_t index = 0U; index < source_limits.size(); ++index)
		EXPECT_EQ(index, static_cast<std::size_t>(source_limits[index]));
	EXPECT_EQ(source_limits.size(),
		static_cast<std::size_t>(detail::TelemetryPhase2SourceLimit::Count));
	EXPECT_EQ(3U,
		static_cast<std::size_t>(detail::TelemetryPhase2MemoryScope::Count));

	detail::TelemetryStructuredLog log;
	log.cockpit_producer_rejected(
		detail::TelemetryCockpitProducerRejection::InvalidSource,
		0x07CBU);
	log.phase2_manifest(0U,
		detail::TelemetryLogEvent::Phase2ManifestBuilt,
		1U, 3U, 1U, 99U, 7U);
	log.phase2_manifest(0U,
		detail::TelemetryLogEvent::Phase2ManifestInstalled,
		2U, 3U, 1U, 99U, 7U);
	log.phase2_manifest(0U,
		detail::TelemetryLogEvent::Phase2ManifestRejected,
		3U, 0U, 0U, 0U, 2U);
	log.phase2_lifecycle(0U,
		detail::TelemetryPhase2LifecycleKind::Destroyed);
	log.phase2_support_terminal(0U,
		detail::TelemetryPhase2SupportKind::CompletedPrivate, true);
	log.phase2_resync(0U,
		detail::TelemetryLogPhase2ResyncResult::AcceptedNewCandidate);
	log.phase2_resync(1U,
		detail::TelemetryLogPhase2ResyncResult::AcceptedCoalesced);
	log.phase2_source_rejected(
		detail::TelemetryPhase2Block::Subsystems,
		detail::TelemetryPhase2CaptureFailure::OutOfRange,
		1'000'000U);
	log.phase2_summary(0U, 8U, 4'096U);
	const auto logs = log.snapshot();
	ASSERT_GE(logs.count, 8U)
		<< "Forbidden-data scanning is meaningful only after normative events were emitted.";
	for (const auto event : {
			 detail::TelemetryLogEvent::CockpitProducerRejected,
			 detail::TelemetryLogEvent::Phase2ManifestBuilt,
			 detail::TelemetryLogEvent::Phase2ManifestInstalled,
			 detail::TelemetryLogEvent::Phase2ManifestRejected,
			 detail::TelemetryLogEvent::Phase2Lifecycle,
			 detail::TelemetryLogEvent::Phase2SupportTerminal,
			 detail::TelemetryLogEvent::Phase2Resync,
			 detail::TelemetryLogEvent::Phase2SourceRejected,
			 detail::TelemetryLogEvent::Phase2Summary}) {
		EXPECT_NE(logs.records.begin() + logs.count,
			std::find_if(logs.records.begin(),
				logs.records.begin() + logs.count,
				[event](const auto& record) {
					return record.event == event;
				}));
	}
	const auto first_resync = std::find_if(logs.records.begin(),
		logs.records.begin() + logs.count, [](const auto& record) {
			return record.event ==
					detail::TelemetryLogEvent::Phase2Resync &&
				record.correlation_slot == 1U;
		});
	const auto coalesced_resync = std::find_if(logs.records.begin(),
		logs.records.begin() + logs.count, [](const auto& record) {
			return record.event ==
					detail::TelemetryLogEvent::Phase2Resync &&
				record.correlation_slot == 2U;
		});
	ASSERT_NE(logs.records.begin() + logs.count, first_resync);
	ASSERT_NE(logs.records.begin() + logs.count, coalesced_resync);
	EXPECT_EQ(detail::TelemetryLogPhase2ResyncResult::
			AcceptedNewCandidate,
		first_resync->phase2_resync);
	EXPECT_EQ(detail::TelemetryLogPhase2ResyncResult::
			AcceptedCoalesced,
		coalesced_resync->phase2_resync);
	const auto* bytes = reinterpret_cast<const std::uint8_t*>(
		logs.records.data());
	const auto byte_count =
		logs.count * sizeof(detail::TelemetryLogRecord);
	for (const std::string forbidden : {
			 "HIDDEN_CALLSIGN_P2",
			 "HIDDEN_CARGO_TEXT_P2",
			 "PAYLOAD_FRAGMENT_P2",
			 "C:\\secret\\mission.fs2",
			 "203.0.113.77",
			 "objnum=-1",
			 "instance=-1",
			 "0x7ffdeadbeef"}) {
		EXPECT_EQ(bytes + byte_count,
			std::search(bytes, bytes + byte_count,
				forbidden.begin(), forbidden.end()))
			<< forbidden;
	}
}

TEST(Phase2SecurityBounds,
	P2AC011And012ProductionWiringUsesAcceptedEventsAndOwnedFixedScratch)
{
	static_assert(std::is_trivially_copyable_v<
		detail::Phase2Wp07GlobalEventBatch>);
	static_assert(std::is_trivially_copyable_v<
		detail::Phase2GlobalFanoutResult>);
	static_assert(detail::Phase2Wp07CleanupRing::Capacity == 64U);
	static_assert(detail::Phase2Wp07SupportTerminalRing::Capacity == 64U);
	static_assert(detail::Phase2GlobalFanoutResult::Capacity == 4U);
	static_assert(sizeof(void*) == 4U || sizeof(void*) == 8U);
	constexpr std::size_t ExpectedEventBatchSize =
		sizeof(void*) == 4U ? 4'632U : 4'640U;
	constexpr std::size_t ExpectedFanoutResultSize =
		sizeof(void*) == 4U ? 28U : 32U;
	constexpr std::size_t ExpectedEventScratchSize =
		sizeof(void*) == 4U ? 4'660U : 4'672U;
	constexpr std::size_t ExpectedOwnedScratchSize =
		sizeof(void*) == 4U ? 5'492U : 5'504U;
	EXPECT_EQ(ExpectedEventBatchSize,
		sizeof(detail::Phase2Wp07GlobalEventBatch));
	EXPECT_EQ(ExpectedFanoutResultSize,
		sizeof(detail::Phase2GlobalFanoutResult));
	EXPECT_EQ(ExpectedEventScratchSize,
		sizeof(detail::Phase2Wp07GlobalEventBatch) +
			sizeof(detail::Phase2GlobalFanoutResult));
	EXPECT_EQ(768U, sizeof(std::array<Phase2SupportTrackerAbiEntry,
		detail::MaximumPhase2ObservationShips>));
	EXPECT_EQ(64U, sizeof(std::array<bool,
		detail::MaximumPhase2ObservationShips>));
	EXPECT_EQ(ExpectedOwnedScratchSize,
		sizeof(detail::Phase2Wp07GlobalEventBatch) +
			sizeof(detail::Phase2GlobalFanoutResult) +
			sizeof(std::array<Phase2SupportTrackerAbiEntry,
				detail::MaximumPhase2ObservationShips>) +
			sizeof(std::array<bool,
				detail::MaximumPhase2ObservationShips>));

	const auto runtime =
		read_source_file("code/telemetry/native_session_runtime.cpp");
	const auto runtime_header =
		read_source_file("code/telemetry/native_session_runtime.h");
	ASSERT_FALSE(runtime.empty());
	ASSERT_FALSE(runtime_header.empty());
	EXPECT_NE(std::string::npos,
		runtime_header.find(
			"Phase2Wp07GlobalEventBatch m_phase2_event_batch_scratch{}"));
	EXPECT_NE(std::string::npos,
		runtime_header.find(
			"Phase2GlobalFanoutResult m_phase2_fanout_scratch{}"));
	EXPECT_NE(std::string::npos,
		runtime_header.find("m_phase2_support_tracker{}"));
	EXPECT_NE(std::string::npos,
		runtime_header.find("m_phase2_support_seen_scratch{}"));
	EXPECT_NE(std::string::npos,
		runtime.find("sizeof(m_phase2_event_batch_scratch) +"));
	EXPECT_NE(std::string::npos,
		runtime.find("sizeof(m_phase2_fanout_scratch)"));
	EXPECT_NE(std::string::npos,
		runtime.find("sizeof(m_phase2_support_tracker)"));
	EXPECT_NE(std::string::npos,
		runtime.find("sizeof(m_phase2_support_seen_scratch)"));
	EXPECT_NE(std::string::npos,
		runtime.find("m_phase2_event_batch_scratch = {}"));
	EXPECT_NE(std::string::npos,
		runtime.find("m_phase2_fanout_scratch = {}"));
	EXPECT_EQ(0U, occurrence_count(runtime_header,
		"std::vector<Phase2Wp07GlobalEventBatch"));
	EXPECT_EQ(0U, occurrence_count(runtime_header,
		"std::vector<Phase2GlobalFanoutResult"));
	EXPECT_EQ(0U, occurrence_count(runtime_header,
		"std::vector<Phase2SupportTrackerEntry"));
	EXPECT_NE(std::string::npos,
		runtime.find("m_phase2_support_seen_scratch = {};"));
	EXPECT_NE(std::string::npos,
		runtime.find("m_phase2_support_tracker = {};"));

	EXPECT_EQ(3U,
		occurrence_count(runtime, "m_log->phase2_source_rejected("));
	const auto unsupported_case = runtime.find(
		"case Phase2CaptureStatus::UnsupportedEngineState:");
	ASSERT_NE(std::string::npos, unsupported_case);
	const auto unsupported_log_guard = runtime.find(
		"m_log != nullptr", unsupported_case);
	const auto unsupported_log = runtime.find(
		"m_log->phase2_source_rejected(primary_block,",
		unsupported_case);
	ASSERT_NE(std::string::npos, unsupported_log_guard);
	ASSERT_NE(std::string::npos, unsupported_log);
	EXPECT_LT(unsupported_log_guard, unsupported_log);
	EXPECT_NE(std::string::npos,
		runtime.find("TelemetryPhase2CaptureFailure::InvalidEnum",
			unsupported_log));
	EXPECT_NE(std::string::npos,
		runtime.find("diagnostics.primary_failed_block"));
	EXPECT_NE(std::string::npos,
		runtime.find("m_metrics->set_phase2_sample_age(index"));
	EXPECT_NE(std::string::npos,
		runtime.find("active_snapshot_id() != 0U"));
	EXPECT_NE(std::string::npos,
		runtime.find("active_block_sample(block)"));
	EXPECT_NE(std::string::npos,
		runtime.find("!rebuild_was_pending &&"));
	EXPECT_NE(std::string::npos,
		runtime.find("manifest_state().rebuild_intent &&"));
	EXPECT_EQ(1U, occurrence_count(runtime,
		"record_phase2_manifest_rebuild_coalesced("));
	EXPECT_NE(std::string::npos,
		runtime.find("started_snapshot_sequence()"));
	EXPECT_NE(std::string::npos,
		runtime.find("last_started_snapshot_cause()"));
	EXPECT_EQ(1U, occurrence_count(runtime,
		"record_phase2_forced_keyframe("));
	for (const auto cause : {
			 "Phase2RuntimeSnapshotCause::Periodic",
			 "Phase2RuntimeSnapshotCause::Topology",
			 "Phase2RuntimeSnapshotCause::Catalog",
			 "Phase2RuntimeSnapshotCause::Lifecycle",
			 "Phase2RuntimeSnapshotCause::SupportTerminal",
			 "Phase2RuntimeSnapshotCause::Resync"})
		EXPECT_NE(std::string::npos, runtime.find(cause)) << cause;
	EXPECT_NE(std::string::npos,
		runtime.find("record_phase2_support_coalesced("));
	const auto docking =
		runtime.find("case ShipSupportPhase::Docking:");
	const auto repairing =
		runtime.find("case ShipSupportPhase::Repairing:");
	const auto rearming =
		runtime.find("case ShipSupportPhase::Rearming:");
	ASSERT_NE(std::string::npos, docking);
	ASSERT_NE(std::string::npos, repairing);
	ASSERT_NE(std::string::npos, rearming);
	EXPECT_LT(docking, repairing);
	EXPECT_LT(repairing, rearming);
	EXPECT_NE(std::string::npos,
		runtime.find(
			"if (entry.previous == ship.support.phase) continue;"))
		<< "Repeated support observations are no-op transitions.";
	const auto begin =
		runtime.find("case SupportTransitionReason::Begin:");
	const auto observed_mapper =
		runtime.find("TelemetryPhase2SupportKind telemetry_observed_support_kind(");
	ASSERT_NE(std::string::npos, begin);
	ASSERT_NE(std::string::npos, observed_mapper);
	EXPECT_LT(begin, observed_mapper)
		<< "Begin is handled only by the terminal-event mapper and cannot double-count Repairing.";
	for (const auto kind : {
			 "TelemetryPhase2LifecycleKind::Appeared",
			 "TelemetryPhase2LifecycleKind::Disabled",
			 "TelemetryPhase2LifecycleKind::DyingStarted",
			 "TelemetryPhase2LifecycleKind::Destroyed",
			 "TelemetryPhase2LifecycleKind::Disappeared",
			 "TelemetryPhase2SupportKind::Requested",
			 "TelemetryPhase2SupportKind::Approaching",
			 "TelemetryPhase2SupportKind::Docking",
			 "TelemetryPhase2SupportKind::Repairing",
			 "TelemetryPhase2SupportKind::Rearming",
			 "TelemetryPhase2SupportKind::Obstructed",
			 "TelemetryPhase2SupportKind::Aborted",
			 "TelemetryPhase2SupportKind::CompletedPrivate",
			 "TelemetryPhase2SupportKind::EndedPrivate"})
		EXPECT_NE(std::string::npos, runtime.find(kind)) << kind;

	detail::TelemetryMetrics metrics;
	ASSERT_TRUE(metrics.provision());
	metrics.activate_session(0U, 1U);
	const auto clean_metrics = metrics.snapshot();
	metrics.record_phase2_lifecycle(
		detail::TelemetryPhase2LifecycleKind::Count);
	metrics.record_phase2_support(
		detail::TelemetryPhase2SupportKind::Count);
	metrics.record_phase2_support_coalesced(0U,
		detail::TelemetryPhase2SupportCoalescedKind::Count);
	metrics.record_phase2_forced_keyframe(0U,
		detail::TelemetryPhase2KeyframeReason::Count);
	EXPECT_EQ(clean_metrics.phase2_lifecycle_events,
		metrics.snapshot().phase2_lifecycle_events);
	EXPECT_EQ(clean_metrics.phase2_support_transitions,
		metrics.snapshot().phase2_support_transitions);
	EXPECT_EQ(clean_metrics.phase2_support_coalesced,
		metrics.snapshot().phase2_support_coalesced);
	EXPECT_EQ(clean_metrics.phase2_forced_keyframes,
		metrics.snapshot().phase2_forced_keyframes);

	detail::TelemetryStructuredLog log;
	const auto clean_logs = log.snapshot();
	log.phase2_lifecycle(0U,
		detail::TelemetryPhase2LifecycleKind::Count);
	log.phase2_support_terminal(0U,
		detail::TelemetryPhase2SupportKind::Count, true);
	log.phase2_source_rejected(
		detail::TelemetryPhase2Block::Count,
		detail::TelemetryPhase2CaptureFailure::Count, 1U);
	EXPECT_EQ(clean_logs.count, log.snapshot().count)
		<< "Rejected enum ordinals never become structured events.";
}

TEST(Phase2SecurityBounds,
	P2TST043FullSupportTrackerTurnsOverWithoutDroppingTheNewTransition)
{
	const auto runtime =
		read_source_file("code/telemetry/native_session_runtime.cpp");
	ASSERT_FALSE(runtime.empty());
	const auto tracker_begin = runtime.find(
		"void NativeSessionRuntime::observe_phase2_support_transitions()");
	const auto tracker_end = runtime.find(
		"void NativeSessionRuntime::reset_phase2_support_tracker()",
		tracker_begin);
	ASSERT_NE(std::string::npos, tracker_begin);
	ASSERT_NE(std::string::npos, tracker_end);
	const auto tracker =
		runtime.substr(tracker_begin, tracker_end - tracker_begin);
	const auto release_absent =
		tracker.find("if (!present) entry = {};");
	const auto accept_sample =
		tracker.find("for (std::size_t ship_index = 0U;",
			release_absent);
	ASSERT_NE(std::string::npos, release_absent);
	ASSERT_NE(std::string::npos, accept_sample);
	EXPECT_LT(release_absent, accept_sample)
		<< "Absent members are released before a full tracker accepts replacements.";
	const auto full_tracker_guard = tracker.find(
		"if (free_index == m_phase2_support_tracker.size())");
	ASSERT_NE(std::string::npos, full_tracker_guard);
	const auto skip_untracked = tracker.find("continue;", full_tracker_guard);
	ASSERT_NE(std::string::npos, skip_untracked);
	EXPECT_LT(skip_untracked - full_tracker_guard, 128U)
		<< "A full tracker skips the untracked sample immediately.";
	EXPECT_EQ(0U, occurrence_count(tracker,
		"m_log->phase2_support_terminal("))
		<< "Observed active phases are metrics-only.";
	EXPECT_EQ(1U, occurrence_count(runtime,
		"m_log->phase2_support_terminal("))
		<< "Only the terminal event path writes Phase2SupportTerminal.";

	std::array<Phase2SupportTrackerAbiEntry,
		detail::MaximumPhase2ObservationShips> entries{};
	for (std::size_t index = 0U; index < entries.size(); ++index) {
		entries[index].signature =
			static_cast<std::uint32_t>(1'000U + index);
		entries[index].capture_key.value =
			static_cast<std::uint32_t>(index + 1U);
		entries[index].previous =
			detail::ShipSupportPhase::Docking;
		entries[index].active = true;
	}
	auto apply_sample = [&entries](
			const std::array<std::uint32_t,
				detail::MaximumPhase2ObservationShips>& signatures,
			detail::ShipSupportPhase phase) {
		std::array<bool,
			detail::MaximumPhase2ObservationShips> seen{};
		for (auto& entry : entries) {
			if (!entry.active) continue;
			const auto present =
				std::find(signatures.begin(), signatures.end(),
					entry.signature) != signatures.end();
			if (!present) entry = {};
		}
		std::size_t transitions = 0U;
		for (std::size_t ship = 0U; ship < signatures.size(); ++ship) {
			std::size_t found = entries.size();
			std::size_t free = entries.size();
			for (std::size_t index = 0U; index < entries.size(); ++index) {
				if (entries[index].active &&
					entries[index].signature == signatures[ship]) {
					found = index;
					break;
				}
				if (!entries[index].active && free == entries.size())
					free = index;
			}
			if (found == entries.size()) {
				if (free == entries.size()) continue;
				found = free;
				entries[found] = {};
				entries[found].signature = signatures[ship];
				entries[found].capture_key.value =
					static_cast<std::uint32_t>(ship + 1U);
				entries[found].active = true;
			}
			seen[found] = true;
			if (entries[found].previous == phase) continue;
			entries[found].previous = phase;
			++transitions;
		}
		for (std::size_t index = 0U; index < entries.size(); ++index)
			if (entries[index].active && !seen[index])
				entries[index] = {};
		return transitions;
	};

	std::array<std::uint32_t,
		detail::MaximumPhase2ObservationShips> turnover{};
	for (std::size_t index = 0U; index + 1U < turnover.size(); ++index)
		turnover[index] = static_cast<std::uint32_t>(1'001U + index);
	turnover.back() = 2'000U;
	EXPECT_EQ(1U, apply_sample(
		turnover, detail::ShipSupportPhase::Docking));
	EXPECT_EQ(entries.size(),
		std::count_if(entries.begin(), entries.end(),
			[](const auto& entry) { return entry.active; }));
	EXPECT_NE(entries.end(),
		std::find_if(entries.begin(), entries.end(),
			[](const auto& entry) {
				return entry.active && entry.signature == 2'000U;
			}))
		<< "The replacement transition is observed at full capacity.";
	EXPECT_EQ(0U, apply_sample(
		turnover, detail::ShipSupportPhase::Docking))
		<< "The retained replacement is reusable and repeated samples are no-op.";
	turnover[0] = 3'000U;
	EXPECT_EQ(1U, apply_sample(
		turnover, detail::ShipSupportPhase::Docking))
		<< "A second turnover proves the released tracker slot is reusable.";
	EXPECT_EQ(entries.size(),
		std::count_if(entries.begin(), entries.end(),
			[](const auto& entry) { return entry.active; }));

	detail::TelemetryMetrics metrics;
	ASSERT_TRUE(metrics.provision());
	const auto before = metrics.snapshot();
	metrics.record_phase2_support(
		detail::TelemetryPhase2SupportKind::Docking);
	metrics.record_phase2_support(
		detail::TelemetryPhase2SupportKind::Rearming);
	const auto after = metrics.snapshot();
	EXPECT_EQ(before.phase2_support_transitions[
			static_cast<std::size_t>(
				detail::TelemetryPhase2SupportKind::Docking)] + 1U,
		after.phase2_support_transitions[
			static_cast<std::size_t>(
				detail::TelemetryPhase2SupportKind::Docking)]);
	EXPECT_EQ(before.phase2_support_transitions[
			static_cast<std::size_t>(
				detail::TelemetryPhase2SupportKind::Rearming)] + 1U,
		after.phase2_support_transitions[
			static_cast<std::size_t>(
				detail::TelemetryPhase2SupportKind::Rearming)]);

	detail::TelemetryStructuredLog log;
	EXPECT_EQ(0U, log.snapshot().count);
	detail::reset_phase2_mission_observation_state();
	telemetry::OnSupportTransition(11U, 12U, 1U,
		detail::SupportTransitionReason::Complete, 100U);
	detail::Phase2CaptureDiagnostics accepted{};
	accepted.source_count = 2U;
	accepted.source_signatures[0] = 11U;
	accepted.source_signatures[1] = 12U;
	detail::Phase2Wp07GlobalEventBatch terminal{};
	ASSERT_EQ(detail::Phase2Wp07DrainStatus::Drained,
		detail::prepare_phase2_global_events(accepted, terminal));
	ASSERT_EQ(1U, terminal.support_count);
	ASSERT_EQ(detail::SupportTransitionReason::Complete,
		terminal.support[0].reason);
	log.phase2_support_terminal(0U,
		detail::TelemetryPhase2SupportKind::CompletedPrivate, false);
	const auto logs = log.snapshot();
	ASSERT_EQ(1U, logs.count);
	EXPECT_EQ(detail::TelemetryLogEvent::Phase2SupportTerminal,
		logs.records[0].event);
	EXPECT_EQ(detail::TelemetryPhase2SupportKind::CompletedPrivate,
		logs.records[0].phase2_support);
	detail::reset_phase2_mission_observation_state();
}

TEST(Phase2SecurityBounds,
	CockpitProducerHasNoProfileOrCompleteDomainBuilderDependency)
{
	const auto runtime =
		read_source_file("code/telemetry/native_session_runtime.cpp");
	const auto runtime_header =
		read_source_file("code/telemetry/native_session_runtime.h");
	const auto controller =
		read_source_file("code/telemetry/session_controller.cpp");
	const auto controller_header =
		read_source_file("code/telemetry/session_controller.h");
	const auto slot =
		read_source_file("code/telemetry/phase2_runtime.cpp");
	const auto slot_header =
		read_source_file("code/telemetry/phase2_runtime.h");
	const auto cockpit_header =
		read_source_file("code/telemetry/cockpit_sensors_state_image.h");
	const auto state_image =
		read_source_file("code/telemetry/cockpit_sensors_state_image.cpp");
	for (const auto* source : {&runtime, &runtime_header,
			 &controller, &controller_header, &slot, &slot_header,
			 &cockpit_header, &state_image}) {
		ASSERT_FALSE(source->empty());
		EXPECT_EQ(std::string::npos, source->find("Phase2" "Profile"));
		EXPECT_EQ(std::string::npos,
			source->find("Phase2" "CompleteDomainPool"));
		EXPECT_EQ(std::string::npos,
			source->find("build_phase2_" "complete_domain"));
	}
	EXPECT_EQ(std::string::npos,
		runtime_header.find("phase2_" "profile_gate.h"));
	EXPECT_NE(std::string::npos, state_image.find(
		"build_cockpit_sensors_state_image_preallocated("));
}

} // namespace
