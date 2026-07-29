#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string read_source(std::string_view relative_path)
{
	const auto path = std::filesystem::path{FSO_PHASE2_SOURCE_ROOT} / relative_path;
	std::ifstream input(path, std::ios::binary);
	EXPECT_TRUE(input.good()) << path.string();
	return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string function_body(const std::string& source, std::string_view signature)
{
	const auto signature_position = source.find(signature);
	if (signature_position == std::string::npos) {
		return {};
	}
	const auto open = source.find('{', signature_position + signature.size());
	if (open == std::string::npos) {
		return {};
	}

	std::size_t depth = 0U;
	for (auto position = open; position < source.size(); ++position) {
		if (source[position] == '{') {
			++depth;
		} else if (source[position] == '}' && --depth == 0U) {
			return source.substr(signature_position, position - signature_position + 1U);
		}
	}
	return {};
}

std::string nth_function_body(const std::string& source,
	std::string_view signature,
	std::size_t occurrence)
{
	std::size_t position = 0U;
	for (std::size_t index = 0U; index <= occurrence; ++index) {
		position = source.find(signature, position);
		if (position == std::string::npos) {
			return {};
		}
		if (index != occurrence) {
			position += signature.size();
		}
	}
	return function_body(source.substr(position), signature);
}

std::size_t occurrence_count(const std::string& source, std::string_view needle)
{
	std::size_t count = 0U;
	std::size_t position = 0U;
	while ((position = source.find(needle, position)) != std::string::npos) {
		++count;
		position += needle.size();
	}
	return count;
}

void expect_ordered(const std::string& body,
	std::string_view before,
	std::string_view hook,
	std::string_view after)
{
	const auto before_position = body.find(before);
	const auto hook_position = body.find(hook);
	const auto after_position = body.find(after);
	ASSERT_NE(std::string::npos, before_position) << before;
	ASSERT_NE(std::string::npos, hook_position) << hook;
	ASSERT_NE(std::string::npos, after_position) << after;
	EXPECT_LT(before_position, hook_position);
	EXPECT_LT(hook_position, after_position);
}

void expect_sequence(const std::string& body,
	std::string_view first,
	std::string_view second,
	std::string_view third)
{
	const auto first_position = body.find(first);
	ASSERT_NE(std::string::npos, first_position) << first;
	const auto second_position = body.find(second, first_position + first.size());
	ASSERT_NE(std::string::npos, second_position) << second;
	const auto third_position = body.find(third, second_position + second.size());
	EXPECT_NE(std::string::npos, third_position) << third;
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ShipCleanupLatchesBeforeRegistryAndShipStateMutation)
{
	const auto source = read_source("code/ship/ship.cpp");
	const auto body = function_body(source, "void ship_cleanup(int shipnum, int cleanup_mode)");
	ASSERT_FALSE(body.empty());
	EXPECT_EQ(1U, occurrence_count(body, "telemetry::OnShipCleanup("));
	expect_ordered(body, "object *objp =", "telemetry::OnShipCleanup(", "entry->status =");
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, SupportTransitionLatchesBeforeRepairStateDecision)
{
	const auto source = read_source("code/ai/aicode.cpp");
	const auto body =
		function_body(source, "void ai_do_objects_repairing_stuff( object *repaired_objp, object *repair_objp, int how )");
	ASSERT_FALSE(body.empty());
	EXPECT_EQ(1U, occurrence_count(body, "telemetry::OnSupportTransition("));
	expect_ordered(body, "Assert( repaired_objp->type == OBJ_SHIP)",
		"telemetry::OnSupportTransition(", "switch( how )");
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ControlAuthorityLatchesBeforePhotoModeEarlyReturnAndPhysics)
{
	const auto source = read_source("code/playerman/playercontrol.cpp");
	const auto body = function_body(source, "void read_player_controls(object *objp, float frametime)");
	ASSERT_FALSE(body.empty());
	EXPECT_EQ(1U, occurrence_count(body, "telemetry::OnControlTarget("));
	expect_ordered(body, "{", "telemetry::OnControlTarget(", "if (game_is_photo_mode_active())");
	EXPECT_LT(body.find("telemetry::OnControlTarget("), body.find("physics_read_flying_controls("));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoAuthorityLatchesAfterInspectionBeforeHudSideEffects)
{
	const auto source = read_source("code/hud/hudtargetbox.cpp");
	const auto body = function_body(source, "void hud_cargo_scan_update(object *targetp, float frametime)");
	ASSERT_FALSE(body.empty());
	EXPECT_EQ(1U, occurrence_count(body, "telemetry::OnCargoAuthority("));
	expect_ordered(body, "Target_display_cargo = player_inspect_cargo",
		"telemetry::OnCargoAuthority(", "hud_targetbox_start_flash(");
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S10TST007HooksAreNoexceptObservationOnlyAndPrecedeMutation)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	for (const auto declaration :
		{"void OnShipCleanup(std::uint32_t object_signature, ShipCleanupMode mode) noexcept;",
			"SupportTransitionReason reason,\n\tstd::uint64_t sample_time) noexcept;",
			"void OnControlTarget(ControlTargetAuthority authority) noexcept;",
			"void OnCargoAuthority(const CargoAuthorityFact& fact) noexcept;"}) {
		EXPECT_NE(std::string::npos, header.find(declaration)) << declaration;
	}

	const auto observation = read_source("code/telemetry/phase2_observation.cpp");
	const std::array<std::string, 4> hooks{{
		function_body(observation, "void OnShipCleanup("),
		function_body(observation, "void OnSupportTransition("),
		function_body(observation, "void OnControlTarget("),
		function_body(observation, "void OnCargoAuthority("),
	}};
	for (const auto& hook : hooks) {
		ASSERT_FALSE(hook.empty());
		for (const auto forbidden :
			{"socket", "send(", "recv(", "new ", "malloc(", "realloc(", "player_inspect_cargo("}) {
			EXPECT_EQ(std::string::npos, hook.find(forbidden)) << forbidden;
		}
	}

	const auto ship =
		function_body(read_source("code/ship/ship.cpp"), "void ship_cleanup(int shipnum, int cleanup_mode)");
	expect_ordered(ship, "object *objp =", "telemetry::OnShipCleanup(", "entry->status =");
	const auto support = function_body(read_source("code/ai/aicode.cpp"),
		"void ai_do_objects_repairing_stuff( object *repaired_objp, object *repair_objp, int how )");
	expect_ordered(support, "Assert( repaired_objp->type == OBJ_SHIP)",
		"telemetry::OnSupportTransition(", "switch( how )");
	const auto control = function_body(
		read_source("code/playerman/playercontrol.cpp"),
		"void read_player_controls(object *objp, float frametime)");
	expect_ordered(control, "{", "telemetry::OnControlTarget(", "if (game_is_photo_mode_active())");
	const auto cargo = function_body(
		read_source("code/hud/hudtargetbox.cpp"),
		"void hud_cargo_scan_update(object *targetp, float frametime)");
	EXPECT_EQ(1U, occurrence_count(cargo, "player_inspect_cargo("));
	expect_ordered(cargo, "player_inspect_cargo(", "telemetry::OnCargoAuthority(",
		"hud_targetbox_start_flash(");
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReopenedStaticCaptureRemainsBehindTheObservationBoundary)
{
	const auto observation_header =
		read_source("code/telemetry/phase2_observation.h");
	const auto adapter_header = read_source("code/telemetry/engine_adapter.h");
	for (const auto& source : {observation_header, adapter_header}) {
		EXPECT_EQ(std::string::npos,
			source.find("phase2_manifest_builder.h"));
		EXPECT_EQ(std::string::npos, source.find("phase2_closure.h"));
	}

	const auto groups = read_source("code/source_groups.cmake");
	for (const auto* required : {
			 "telemetry/phase2_observation.cpp",
			 "telemetry/phase2_observation.h",
			 "telemetry/engine_adapter.cpp",
			 "telemetry/engine_adapter.h"}) {
		EXPECT_NE(std::string::npos, groups.find(required)) << required;
	}
	EXPECT_EQ(std::string::npos, groups.find("telemetry/phase2_*.cpp"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReopenedFsoExtractorSeamIsTestGuarded)
{
	const auto adapter_header = read_source("code/telemetry/engine_adapter.h");
	const auto seam =
		adapter_header.find("extract_static_authorities_for_test");
	ASSERT_NE(std::string::npos, seam)
		<< "WP02 needs a typed extractor seam over the real FsoEngineReadView.";
	const auto if_defined =
		adapter_header.rfind(
			"#if defined(FSO_TELEMETRY_TEST_SEAMS)", seam);
	const auto ifdef =
		adapter_header.rfind("#ifdef FSO_TELEMETRY_TEST_SEAMS", seam);
	const auto guard =
		if_defined == std::string::npos ? ifdef :
		ifdef == std::string::npos ? if_defined :
		std::max(if_defined, ifdef);
	const auto prior_end = adapter_header.rfind("#endif", seam);
	ASSERT_NE(std::string::npos, guard)
		<< "The extractor seam must exist only in test builds.";
	EXPECT_TRUE(prior_end == std::string::npos || prior_end < guard)
		<< "The extractor wrapper is outside its exact test-seam guard.";
	const auto end = adapter_header.find("#endif", seam);
	EXPECT_NE(std::string::npos, end)
		<< "The FSO_TELEMETRY_TEST_SEAMS guard must close after the seam.";

	const auto adapter_source =
		read_source("code/telemetry/engine_adapter.cpp");
	const auto seam_body = function_body(adapter_header,
		"SourceReadResult extract_static_authorities_for_test(");
	const auto production_body = function_body(adapter_source,
		"SourceReadResult extract_production_static_authorities(");
	ASSERT_FALSE(seam_body.empty());
	ASSERT_FALSE(production_body.empty());
	EXPECT_EQ(std::string::npos,
		production_body.find("preserve_prepared_catalog = true"))
		<< "The real extractor and test seam must execute the same mapping "
		   "helper; the seam cannot select a private prepared-catalog flag.";
	std::string delegate;
	for (std::size_t open = seam_body.find('(');
		 open != std::string::npos;
		 open = seam_body.find('(', open + 1U)) {
		auto begin = open;
		while (begin > 0U) {
			const auto value =
				static_cast<unsigned char>(seam_body[begin - 1U]);
			if (std::isalnum(value) == 0 && value != '_') {
				break;
			}
			--begin;
		}
		const auto candidate = seam_body.substr(begin, open - begin);
		if (!candidate.empty() &&
			production_body.find(candidate + "(") != std::string::npos) {
			delegate = candidate;
			break;
		}
	}
	ASSERT_FALSE(delegate.empty())
		<< "Seam and production must call one shared mapping interface; "
		   "the seam need not return it directly.";
	const auto production_delegate =
		production_body.find(delegate + "(");
	ASSERT_NE(std::string::npos, production_delegate)
		<< "Seam and production must execute the same shared mapping interface.";
	const auto after_delegate =
		production_body.substr(production_delegate + delegate.size() + 1U);
	for (const auto continued_private_mapping : {
			 "raw_class.",
			 "raw_static_catalog.weapon_definitions",
			 "raw_static_catalog.auxiliary_entries"}) {
		EXPECT_EQ(
			std::string::npos, after_delegate.find(continued_private_mapping))
			<< "The shared seam call covers only a subset; production continues "
			   "private mapping after it: " << continued_private_mapping;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReopenedRuntimeUsesOnlyTheProvisionedBufferCapturePath)
{
	const auto runtime = read_source("code/telemetry/native_session_runtime.cpp");
	const auto tick = function_body(runtime,
		"NativeSessionTickStatus NativeSessionRuntime::service_tick(");
	ASSERT_FALSE(tick.empty());
	const auto capture = tick.find("collect_phase2_observation(");
	ASSERT_NE(std::string::npos, capture);
	EXPECT_NE(std::string::npos,
		tick.find("m_phase2_observation", capture))
		<< "Ready runtime capture must use its startup-provisioned scratch owner.";
	EXPECT_EQ(1U, occurrence_count(tick, "collect_phase2_observation("));
	EXPECT_EQ(std::string::npos,
		tick.find("Phase2ObservationDto observation", capture))
		<< "Runtime must not select the allocating direct-DTO overload.";
	EXPECT_EQ(std::string::npos,
		tick.find("std::make_unique<Phase2Observation", capture));
	EXPECT_EQ(std::string::npos,
		tick.find("new Phase2Observation", capture));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReviewB3UsesOneExhaustiveMapperWithoutCatalogShapeBifurcation)
{
	const auto observation =
		read_source("code/telemetry/phase2_observation.cpp");
	const auto mapper = function_body(observation,
		"SourceReadResult map_phase2_static_authorities(");
	const auto adapter = read_source("code/telemetry/engine_adapter.cpp");
	const auto production = function_body(adapter,
		"SourceReadResult extract_production_static_authorities(");
	const auto seam = function_body(
		read_source("code/telemetry/engine_adapter.h"),
		"SourceReadResult extract_static_authorities_for_test(");
	ASSERT_FALSE(mapper.empty());
	ASSERT_FALSE(production.empty());
	ASSERT_FALSE(seam.empty());
	EXPECT_EQ(std::string::npos,
		mapper.find("has_complete_typed_catalog"))
		<< "Production and seam must not choose different mapper behavior "
		   "from whether a catalog was pre-populated.";
	EXPECT_EQ(1U,
		occurrence_count(production, "map_phase2_static_authorities("));
	EXPECT_EQ(1U,
		occurrence_count(seam, "map_phase2_static_authorities("));
	for (const auto hostile_authority : {
			 "fire_wait_seconds", "num_slots"}) {
		EXPECT_NE(std::string::npos, mapper.find(hostile_authority))
			<< hostile_authority;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReviewB3ProductionDoesNotPrefillOutputCatalogBeforePureMapper)
{
	const auto adapter = read_source("code/telemetry/engine_adapter.cpp");
	const auto production = function_body(adapter,
		"SourceReadResult extract_production_static_authorities(");
	ASSERT_FALSE(production.empty());
	const auto mapper =
		production.find("map_phase2_static_authorities(");
	ASSERT_NE(std::string::npos, mapper);
	const auto before_mapper = production.substr(0U, mapper);
	EXPECT_EQ(std::string::npos,
		before_mapper.find("output.raw_static_catalog"))
		<< "Production must collect raw authorities only; the shared mapper "
		   "must be the sole owner of the output catalog shape.";
	EXPECT_EQ(std::string::npos,
		before_mapper.find("auto& catalog = output.raw_static_catalog"))
		<< "An output-catalog alias before the mapper permits private "
		   "production prefill and catalog-shape bifurcation.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReviewB3SharedMapperConsumesExhaustiveRawInputAndFreshOutput)
{
	const auto observation =
		read_source("code/telemetry/phase2_observation.cpp");
	const auto mapper = function_body(observation,
		"SourceReadResult map_phase2_static_authorities(");
	const auto production = function_body(
		read_source("code/telemetry/engine_adapter.cpp"),
		"SourceReadResult extract_production_static_authorities(");
	ASSERT_FALSE(mapper.empty());
	ASSERT_FALSE(production.empty());
	for (const auto authority : {
			 "input.ship_info",
			 "input.weapon_info",
			 "input.model",
			 "input.subsystems",
			 "input.banks",
			 "input.registries"}) {
		EXPECT_NE(std::string::npos, production.find(authority))
			<< "Production does not populate exhaustive raw authority: "
			<< authority;
		EXPECT_NE(std::string::npos, mapper.find(authority))
			<< "The shared mapper does not consume raw authority: "
			<< authority;
	}
	EXPECT_EQ(std::string::npos,
		mapper.find("validate_raw_static_catalog_bounds(catalog)"))
		<< "A pure mapper must not validate or branch on caller-prefilled "
		   "output before constructing its own fresh catalog.";
	EXPECT_EQ(std::string::npos, mapper.find("std::max(catalog.class_count"))
		<< "Class shape must derive from raw input, not prior output state.";
	EXPECT_EQ(std::string::npos, mapper.find("std::max(catalog.weapon_count"))
		<< "Weapon shape must derive from referenced raw authorities, not "
		   "prior output state.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReviewB3BuildDependencyProducesCurrentAdapterObjectWithoutLinkingCode)
{
#if !defined(FSO_PHASE2_MSVC_LINK_EVIDENCE) || !FSO_PHASE2_MSVC_LINK_EVIDENCE
	GTEST_SKIP()
		<< "The current adapter-object dependency contract is MSVC-only.";
#else
	const auto root = std::filesystem::path{FSO_PHASE2_SOURCE_ROOT};
	const auto adapter_object =
		std::filesystem::path{FSO_PHASE2_ENGINE_ADAPTER_OBJECT};
	ASSERT_TRUE(std::filesystem::is_regular_file(adapter_object))
		<< "The source-contract target must depend on `code` so this exact "
		   "configured adapter object exists for inspection.";
	EXPECT_GE(std::filesystem::last_write_time(adapter_object),
		std::filesystem::last_write_time(
			root / "code/telemetry/engine_adapter.cpp"))
		<< "The inspected production object is stale.";

	const auto cmake = read_source("test/src/CMakeLists.txt");
	EXPECT_NE(std::string::npos,
		cmake.find(
			"add_dependencies(telemetry_phase2_engine_integration_source_contract_tests code)"));
	const auto link = cmake.find(
		"target_link_libraries(telemetry_phase2_engine_integration_source_contract_tests PRIVATE");
	ASSERT_NE(std::string::npos, link);
	const auto link_end = cmake.find(')', link);
	ASSERT_NE(std::string::npos, link_end);
	EXPECT_EQ(std::string::npos,
		cmake.substr(link, link_end - link).find(" code"))
		<< "The test may inspect the current adapter object but must not link "
		   "the monolithic engine library or claim adapter execution.";
#endif
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02SourceNonMsvcCompileGuardsEveryAdapterObjectTestReference)
{
	const auto source = read_source(
		"test/src/telemetry/producer/"
		"test_phase2_engine_integration_source_contract.cpp");
	const auto object_macro =
		std::string{"FSO_PHASE2_ENGINE_"} + "ADAPTER_OBJECT";
	std::size_t reference_count = 0U;
	for (auto reference = source.find(object_macro);
		 reference != std::string::npos;
		 reference = source.find(object_macro, reference + object_macro.size())) {
		++reference_count;
		const auto test_start = source.rfind("\nTEST(", reference);
		const auto next_test = source.find("\nTEST(", reference);
		ASSERT_NE(std::string::npos, test_start);
		const auto test_end =
			next_test == std::string::npos ? source.size() : next_test;
		const auto test_block =
			source.substr(test_start, test_end - test_start);
		const auto relative_reference = reference - test_start;
		const auto guard =
			test_block.find("FSO_PHASE2_MSVC_LINK_EVIDENCE");
		const auto skip = test_block.find("GTEST_SKIP()");
		EXPECT_NE(std::string::npos, guard)
			<< "Every executable test that names the MSVC adapter object "
			   "must compile on non-MSVC through an explicit evidence guard.";
		EXPECT_NE(std::string::npos, skip)
			<< "Every guarded MSVC-only object test needs an explicit skip.";
		if (guard != std::string::npos) {
			EXPECT_LT(guard, relative_reference)
				<< "The MSVC evidence guard must precede the object macro.";
		}
	}
	EXPECT_GT(reference_count, 0U);

	const auto cmake = read_source("test/src/CMakeLists.txt");
	const auto evidence_target =
		cmake.find("if(MSVC)\n\tadd_library("
			"telemetry_phase2_engine_adapter_evidence_object");
	const auto non_msvc = cmake.find("else()", evidence_target);
	const auto evidence_end = cmake.find("endif()", non_msvc);
	const auto object_definition =
		cmake.find("FSO_PHASE2_ENGINE_ADAPTER_OBJECT=", evidence_target);
	ASSERT_NE(std::string::npos, evidence_target);
	ASSERT_NE(std::string::npos, non_msvc);
	ASSERT_NE(std::string::npos, evidence_end);
	ASSERT_NE(std::string::npos, object_definition);
	EXPECT_LT(object_definition, non_msvc)
		<< "The MSVC object path must not be defined in the non-MSVC branch.";
	const auto non_msvc_block =
		cmake.substr(non_msvc, evidence_end - non_msvc);
	EXPECT_NE(std::string::npos,
		non_msvc_block.find("FSO_PHASE2_MSVC_LINK_EVIDENCE=0"));
	EXPECT_EQ(std::string::npos,
		non_msvc_block.find("FSO_PHASE2_ENGINE_ADAPTER_OBJECT="));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReviewB3ProductionResetsOwnedAuthorityScratchInPlaceWithoutAggregateTemporary)
{
	const auto adapter = read_source("code/telemetry/engine_adapter.cpp");
	const auto production = function_body(adapter,
		"SourceReadResult extract_production_static_authorities(");
	ASSERT_FALSE(production.empty());
	EXPECT_NE(std::string::npos,
		production.find("auto& input = output.static_authority_input"))
		<< "Production must reuse the Phase2ShipSource-owned authority scratch.";
	for (const auto forbidden_local_or_allocation : {
			 "Phase2StaticAuthorityInput input",
			 "Phase2StaticAuthorityInput{",
			 "make_unique<Phase2StaticAuthorityInput",
			 "new Phase2StaticAuthorityInput",
			 "input = {};",
			 "input = Phase2StaticAuthorityInput{}",
			 "input = Phase2StaticAuthorityInput()"}) {
		EXPECT_EQ(std::string::npos,
			production.find(forbidden_local_or_allocation))
			<< "The exhaustive static-authority fixture is maximum-sized; "
			   "production must map directly into provisioned owned scratch, "
			   "never create/reset it through a giant aggregate temporary: "
			<< forbidden_local_or_allocation;
	}
	EXPECT_EQ(1U,
		occurrence_count(
			production, "reset_phase2_static_authority_input(input)"))
		<< "The owned scratch needs one explicit named in-place reset.";

	const auto observation_header =
		read_source("code/telemetry/phase2_observation.h");
	const auto observation_source =
		read_source("code/telemetry/phase2_observation.cpp");
	EXPECT_NE(std::string::npos,
		observation_header.find("reset_phase2_static_authority_input("))
		<< "The in-place reset contract must be named and shared.";
	const auto reset = function_body(
		observation_source, "void reset_phase2_static_authority_input(");
	ASSERT_FALSE(reset.empty())
		<< "The named reset must have an inspectable production implementation.";
	for (const auto forbidden_aggregate_reset : {
			 "input = {};",
			 "input = Phase2StaticAuthorityInput{}",
			 "input = Phase2StaticAuthorityInput()"}) {
		EXPECT_EQ(std::string::npos, reset.find(forbidden_aggregate_reset))
			<< forbidden_aggregate_reset;
	}
	for (const auto owned_member : {
			 "input.guards_valid",
			 "input.ship_info",
			 "input.weapon_info",
			 "input.model",
			 "input.subsystem_count",
			 "input.subsystems",
			 "input.bank_count",
			 "input.banks",
			 "input.registries"}) {
		EXPECT_NE(std::string::npos, reset.find(owned_member))
			<< "The in-place reset does not visibly clear owned member: "
			<< owned_member;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	Wp02ReviewB3ProductionNeverPlacesFullStaticAuthorityInputOnStack)
{
	const auto adapter = read_source("code/telemetry/engine_adapter.cpp");
	const auto production = function_body(adapter,
		"SourceReadResult extract_production_static_authorities(");
	ASSERT_FALSE(production.empty());
	for (const auto forbidden_local_or_allocation : {
			 "Phase2StaticAuthorityInput input",
			 "Phase2StaticAuthorityInput{",
			 "make_unique<Phase2StaticAuthorityInput",
			 "new Phase2StaticAuthorityInput"}) {
		EXPECT_EQ(std::string::npos,
			production.find(forbidden_local_or_allocation))
			<< "The exhaustive static-authority fixture is maximum-sized; "
			   "production must map directly into provisioned owned scratch, "
			   "never put it on the engine-thread stack or allocate per capture: "
			<< forbidden_local_or_allocation;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S11TST008RuntimeSelectsProfileBeforeDtoTransportAndWelcome)
{
	const auto header = read_source("code/telemetry/native_session_runtime.h");
	const auto request = function_body(header, "struct NativeSessionStartRequest");
	ASSERT_FALSE(request.empty());
	EXPECT_NE(std::string::npos, request.find("Phase2ProfileEligibility")) <<
		"RED S11: runtime start has no engine-eligibility input.";
	EXPECT_NE(std::string::npos, request.find("Phase2Profile")) <<
		"RED S11: runtime start has no requested Phase 2 profile.";

	const auto source = read_source("code/telemetry/native_session_runtime.cpp");
	const auto start =
		function_body(source, "NativeSessionStartStatus NativeSessionRuntime::start(");
	ASSERT_FALSE(start.empty());
	const auto selection = start.find("select_phase2_profile(");
	const auto dto = start.find("std::unique_ptr<Phase2ObservationBuffer>");
	const auto publication = start.find("std::move(*phase2_observation)");
	const auto transport = start.find("m_transport.open(");
	ASSERT_NE(std::string::npos, selection) <<
		"RED S11: the profile gate is not integrated into NativeSessionRuntime::start.";
	ASSERT_NE(std::string::npos, dto);
	ASSERT_NE(std::string::npos, publication);
	ASSERT_NE(std::string::npos, transport);
	EXPECT_LT(selection, dto) << "Profile rejection must precede Phase 2 DTO allocation.";
	EXPECT_LT(dto, publication) << "The unique_ptr candidate must exist before ownership is published.";
	EXPECT_LT(selection, transport) << "Profile rejection must precede transport and any WELCOME.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, RealFsoViewImplementsPhase2GuardsBeforeShipReads)
{
	const auto header = read_source("code/telemetry/engine_adapter.h");
	EXPECT_NE(std::string::npos, header.find("public Phase2EngineReadView"));
	EXPECT_NE(std::string::npos, header.find("current_thread_is_main() const noexcept override"));
	EXPECT_NE(std::string::npos,
		header.find("read_player_root_key(EngineEntityKey& output) const noexcept override"));
	EXPECT_NE(std::string::npos, header.find("read_discovery_node("));
	EXPECT_NE(std::string::npos, header.find("SourceReadResult read_ship("));

	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read_ship =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read_ship.empty());
	EXPECT_NE(std::string::npos, read_ship.find("OBJ_SHIP"));
	EXPECT_NE(std::string::npos, read_ship.find("MAX_SHIPS"));
	EXPECT_NE(std::string::npos, read_ship.find("MAX_OBJECTS"));
	EXPECT_NE(std::string::npos, read_ship.find("object_signature"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, FsoViewNeverClaimsAuthorityFromItsConstructingWorker)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto constructor =
		source.find("FsoEngineReadView::FsoEngineReadView() noexcept = default;");
	ASSERT_NE(std::string::npos, constructor);
	const auto authority = function_body(
		source, "bool FsoEngineReadView::current_thread_is_main() const noexcept");
	ASSERT_FALSE(authority.empty());
	EXPECT_NE(std::string::npos, authority.find("phase2_current_thread_is_main()"));
	EXPECT_EQ(std::string::npos,
		source.find("FsoEngineReadView::FsoEngineReadView() noexcept\n{\n\tcapture_phase2_main_thread_authority"));

	const auto observation = read_source("code/telemetry/phase2_observation.cpp");
	const auto query =
		function_body(observation, "bool phase2_current_thread_is_main() noexcept");
	ASSERT_FALSE(query.empty());
	EXPECT_NE(std::string::npos, query.find("Phase2MainThreadCaptured &&"));
	EXPECT_NE(std::string::npos,
		query.find("std::this_thread::get_id() == Phase2MainThread"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CleanupAndSupportMappingsCoverEveryClosedEngineReason)
{
	const auto ship_source = read_source("code/ship/ship.cpp");
	const auto ship_body = function_body(ship_source, "void ship_cleanup(int shipnum, int cleanup_mode)");
	for (const auto token : {"case SHIP_DESTROYED:", "case SHIP_DEPARTED:",
			 "case SHIP_DEPARTED_WARP:", "case SHIP_DEPARTED_BAY:",
			 "case SHIP_DEPARTED_REDALERT:", "case SHIP_DESTROYED_REDALERT:",
			 "case SHIP_VANISHED:", "ShipCleanupMode::Destroyed",
			 "ShipCleanupMode::Departed", "ShipCleanupMode::Vanished"}) {
		EXPECT_NE(std::string::npos, ship_body.find(token)) << token;
	}

	const auto ai_source = read_source("code/ai/aicode.cpp");
	const auto mapping =
		function_body(ai_source, "static telemetry::SupportTransitionReason telemetry_support_transition_reason(int how) noexcept");
	for (const auto token : {"case REPAIR_INFO_QUEUE:", "case REPAIR_INFO_ONWAY:",
			 "case REPAIR_INFO_BEGIN:", "case REPAIR_INFO_BROKEN:", "case REPAIR_INFO_END:",
			 "case REPAIR_INFO_ABORT:", "case REPAIR_INFO_KILLED:",
			 "case REPAIR_INFO_COMPLETE:", "SupportTransitionReason::Queue",
			 "SupportTransitionReason::OnWay", "SupportTransitionReason::Begin",
			 "SupportTransitionReason::Broken", "SupportTransitionReason::End",
			 "SupportTransitionReason::Abort", "SupportTransitionReason::Killed",
			 "SupportTransitionReason::Complete", "SupportTransitionReason::Count"}) {
		EXPECT_NE(std::string::npos, mapping.find(token)) << token;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, RealFsoReadClearsOutputAndChecksAllIndicesBeforeDataCopy)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto consistency =
		function_body(source, "bool FsoEngineReadView::player_source_is_consistent() const noexcept");
	for (const auto token : {"GM_IN_MISSION", "Player != nullptr", "Player_obj != nullptr",
			 "Player_ship != nullptr", "Player_obj->type == OBJ_SHIP",
			 "Player_obj->instance >= 0", "Player_obj->instance < MAX_SHIPS",
			 "Player->objnum >= 0", "Player->objnum < MAX_OBJECTS",
			 "&Objects[Player->objnum] == Player_obj",
			 "&Ships[Player_obj->instance] == Player_ship",
			 "Player_ship->objnum >= 0", "Player_ship->objnum < MAX_OBJECTS",
			 "&Objects[Player_ship->objnum] == Player_obj", "Player_obj->signature > 0",
			 "Player_ship->ship_info_index >= 0", "Player_ship->ship_info_index <"}) {
		EXPECT_NE(std::string::npos, consistency.find(token)) << token;
	}

	const auto read_ship =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	const auto reset = read_ship.find("output = {};");
	const auto guard = read_ship.find("key.object_signature !=");
	const auto copy = read_ship.find("output.internal_name =");
	ASSERT_NE(std::string::npos, reset);
	ASSERT_NE(std::string::npos, guard);
	ASSERT_NE(std::string::npos, copy);
	EXPECT_LT(reset, guard);
	EXPECT_LT(guard, copy);
	EXPECT_EQ(std::string::npos, read_ship.find("output.object_signature"))
		<< "Engine signatures remain lookup-only and never enter Phase2ShipSource.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, RealFsoReadPopulatesRootFlightDamageAndShieldSources)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read_ship =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read_ship.empty());
	for (const auto token : {"output.flight.position_world",
			 "output.flight.orientation_local_to_world", "output.flight.velocity_world",
			 "output.flight.rotational_velocity_local", "output.flight.radius",
			 "output.damage.hull_current", "output.damage.hull_maximum",
			 "output.damage.guardian_threshold", "output.shields.has_shields",
			 "output.shields.segment_count", "output.shields.segment_current_hits",
			 "output.shields.segment_maximum_hits"}) {
		EXPECT_NE(std::string::npos, read_ship.find(token)) << token;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, RealFsoReadClampsOnlyEngineCurrentHitSources)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read_ship =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read_ship.empty());
	const auto hull_copy = read_ship.find("output.damage.hull_current =");
	const auto hull_clamp = read_ship.find("std::max(0.0F, std::min(Player_obj->hull_strength");
	const auto shield_copy = read_ship.find("output.shields.segment_current_hits[segment] =");
	const auto shield_clamp =
		read_ship.find("std::max(0.0F, std::min(segment_current, segment_maximum))");
	ASSERT_NE(std::string::npos, hull_copy);
	ASSERT_NE(std::string::npos, hull_clamp);
	ASSERT_NE(std::string::npos, shield_copy);
	ASSERT_NE(std::string::npos, shield_clamp);
	EXPECT_LT(hull_copy, hull_clamp);
	EXPECT_LT(shield_copy, shield_clamp);
	EXPECT_EQ(std::string::npos, read_ship.find("std::min(output.damage.hull_maximum"));
	EXPECT_EQ(std::string::npos, read_ship.find("std::min(output.damage.guardian_threshold"));
	EXPECT_EQ(std::string::npos, read_ship.find("std::min(output.shields.recharge_maximum"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, RealFsoReadPopulatesIdentityLifecycleEnergyAndPropulsionWithoutRatios)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read_ship =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read_ship.empty());
	for (const auto token : {"output.identity.presence", "output.identity.class_source_key",
			 "output.lifecycle.presence", "output.lifecycle.state",
			 "output.lifecycle.lifecycle_flags", "output.energy.presence",
			 "output.energy.weapon_energy_current", "output.energy.weapon_energy_maximum",
			 "output.energy.shield_recharge_index", "output.energy.weapon_recharge_index",
			 "output.energy.engine_recharge_index", "output.energy.ets_available",
			 "output.propulsion.presence", "output.propulsion.propulsion_flags",
			 "output.propulsion.afterburner_fuel", "output.propulsion.afterburner_capacity",
			 "output.propulsion.burn_rate", "output.propulsion.recovery_rate",
			 "output.propulsion.cooldown_remaining_us",
			 "output.propulsion.time_since_last_stop_us"}) {
		EXPECT_NE(std::string::npos, read_ship.find(token)) << token;
	}
	for (const auto forbidden :
		{"energy_ratio", "fuel_ratio", "hull_ratio", "shield_ratio", "percentage"}) {
		EXPECT_EQ(std::string::npos, read_ship.find(forbidden)) << forbidden;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, FsoAfterburnerHistoryRejectsTimerUnderflowAndOneDayPlusOne)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read_ship =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read_ship.empty());
	for (const auto token :
		{"elapsed_ms < 0", "elapsed_ms > 86'400'000",
			"cooldown_ms < 0.0", "cooldown_ms > 86'400'000.0"}) {
		EXPECT_NE(std::string::npos, read_ship.find(token)) << token;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, FsoControlsUseAllowedCiFieldsAndLatchedControlAuthority)
{
	const auto header = read_source("code/telemetry/engine_adapter.h");
	EXPECT_NE(std::string::npos,
		header.find("read_player_controls(PlayerControlObservation& output) const noexcept override"));
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto body = function_body(source,
		"bool FsoEngineReadView::read_player_controls(PlayerControlObservation& output) const noexcept");
	ASSERT_FALSE(body.empty());
	for (const auto token : {"Player->ci.pitch", "Player->ci.heading", "Player->ci.bank",
			 "Player->ci.forward", "Player->ci.sideways", "Player->ci.vertical",
			 "phase2_seam_handoff_snapshot()", "has_control_target", "control_target"}) {
		EXPECT_NE(std::string::npos, body.find(token)) << token;
	}
	EXPECT_EQ(std::string::npos, body.find("Player->ci.control_flags"));
	EXPECT_EQ(std::string::npos, body.find("Viewer_mode"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, FsoCargoConsumesHistoricalHandoffWithoutGameplayReentry)
{
	const auto header = read_source("code/telemetry/engine_adapter.h");
	EXPECT_NE(std::string::npos,
		header.find("read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept override"));
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto body = function_body(source,
		"bool FsoEngineReadView::read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept");
	ASSERT_FALSE(body.empty());
	for (const auto token : {"phase2_seam_handoff_snapshot()", "has_cargo_authority",
			 "cargo_authority", "target_subsystem_source_key", "phase", "presence",
			 "elapsed_us", "required_us", "validity_flags", "cargo_text"}) {
		EXPECT_NE(std::string::npos, body.find(token)) << token;
	}
	EXPECT_EQ(std::string::npos, body.find("player_inspect_cargo"));
	EXPECT_EQ(std::string::npos, body.find("hud_cargo_scan_update"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoGameplayAdvancesOnceBeforeHistoricalLatchAndHudEffects)
{
	const auto hud_source = read_source("code/hud/hudtargetbox.cpp");
	const auto body =
		function_body(hud_source, "void hud_cargo_scan_update(object *targetp, float frametime)");
	ASSERT_FALSE(body.empty());
	EXPECT_EQ(1U, occurrence_count(body, "player_inspect_cargo("));
	EXPECT_EQ(1U, occurrence_count(body, "telemetry::OnCargoAuthority("));
	expect_ordered(body, "player_inspect_cargo(", "telemetry::OnCargoAuthority(",
		"hud_targetbox_start_flash(");

	const auto observation_header = read_source("code/telemetry/phase2_observation.h");
	EXPECT_NE(std::string::npos,
		observation_header.find("void reset_phase2_mission_observation_state() noexcept;"));
	const auto observation_source = read_source("code/telemetry/phase2_observation.cpp");
	const auto cargo_hook =
		function_body(observation_source, "void OnCargoAuthority(const CargoAuthorityFact& fact) noexcept");
	ASSERT_FALSE(cargo_hook.empty());
	expect_ordered(cargo_hook, "Phase2Handoff.cargo_authority = fact",
		"phase2_seam_test_double()", "on_cargo_authority(fact)");
	EXPECT_EQ(std::string::npos, cargo_hook.find("player_inspect_cargo"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CollectorReadsEachPlayerRootExactlyOnceAfterShipClosure)
{
	const auto source = read_source("code/telemetry/phase2_observation.cpp");
	const auto body = function_body(source,
		"static Phase2CaptureResult collect_phase2_observation_with_scratch(");
	ASSERT_FALSE(body.empty());
	EXPECT_EQ(1U, occurrence_count(body, "source.read_player_controls("));
	EXPECT_EQ(1U, occurrence_count(body, "source.read_player_cargo_scan("));
	const auto ship_read = body.find("source.read_ship(");
	const auto control_read = body.find("source.read_player_controls(");
	const auto cargo_read = body.find("source.read_player_cargo_scan(");
	ASSERT_NE(std::string::npos, ship_read);
	ASSERT_NE(std::string::npos, control_read);
	ASSERT_NE(std::string::npos, cargo_read);
	EXPECT_LT(ship_read, control_read);
	EXPECT_LT(control_read, cargo_read);
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoFactUsesGameplayScanGeometrySensorsAndClosedValidity)
{
	const auto gameplay_source = read_source("code/playerman/playercontrol.cpp");
	const auto gameplay = function_body(gameplay_source, "bool player_inspect_cargo(");
	ASSERT_FALSE(gameplay.empty());
	for (const auto token :
		{"hud_sensors_ok(", "current_target_distance", "CARGO_MIN_DOT_TO_REVEAL"}) {
		EXPECT_NE(std::string::npos, gameplay.find(token)) << token;
	}
	EXPECT_NE(std::string::npos,
		gameplay_source.find("CargoAuthorityFact& cargo_fact"));
	for (const auto token : {"CargoScanStatePresenceFlagValidity", "ScanValidityFlagInRange",
			 "ScanValidityFlagInAngle", "ScanValidityFlagLineOfSight",
			 "CargoScanPhaseObservation::Idle"}) {
		EXPECT_NE(std::string::npos, gameplay_source.find(token)) << token;
	}

	const auto hud_source = read_source("code/hud/hudtargetbox.cpp");
	const auto body =
		function_body(hud_source, "void hud_cargo_scan_update(object *targetp, float frametime)");
	ASSERT_FALSE(body.empty());
	EXPECT_NE(std::string::npos,
		body.find("player_inspect_cargo(frametime, Cargo_string, cargo_fact)"));
	EXPECT_EQ(std::string::npos,
		body.find("cargo_fact.elapsed_us = cargo_fact.required_us"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoDisclosureDistinguishesShipAndSubsystemAndUsesRawMissionText)
{
	const auto gameplay_source = read_source("code/playerman/playercontrol.cpp");
	for (const auto token : {"cargo_sp->flags[Ship::Ship_Flags::Cargo_revealed]",
			 "subsys->flags[Ship::Subsystem_Flags::Cargo_revealed]",
			 "Cargo_names[", "cargo_title", "[0] == '#'"}) {
		EXPECT_NE(std::string::npos, gameplay_source.find(token)) << token;
	}
	EXPECT_NE(std::string::npos, gameplay_source.find("cargo_fact.phase"));
	EXPECT_NE(std::string::npos, gameplay_source.find("cargo_fact.cargo_text.assign"));

	const auto hud_source = read_source("code/hud/hudtargetbox.cpp");
	const auto body =
		function_body(hud_source, "void hud_cargo_scan_update(object *targetp, float frametime)");
	ASSERT_FALSE(body.empty());
	EXPECT_EQ(std::string::npos, body.find("cargo_text.assign(Cargo_string)"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoReadRevalidatesTargetAndSubsystemBeforePublishingHandoff)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto body = function_body(source,
		"bool FsoEngineReadView::read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept");
	ASSERT_FALSE(body.empty());
	for (const auto token : {"MAX_OBJECTS", "Objects[", "OBJ_SHIP", "target_signature",
			 "target_subsystem_source_key", "Player_ai->targeted_subsys", "system_info"}) {
		EXPECT_NE(std::string::npos, body.find(token)) << token;
	}
	EXPECT_LT(body.find("target_signature"), body.find("output.target_object_signature"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, MissingCargoAuthorityIsExplicitHiddenNotScannableWithNoGroups)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto body = function_body(source,
		"bool FsoEngineReadView::read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept");
	ASSERT_FALSE(body.empty());
	const auto absent = body.find("if (!handoff.has_cargo_authority)");
	ASSERT_NE(std::string::npos, absent);
	const auto absent_end = body.find('}', absent);
	ASSERT_NE(std::string::npos, absent_end);
	const auto absent_branch = body.substr(absent, absent_end - absent + 1U);
	EXPECT_NE(std::string::npos,
		absent_branch.find("CargoScanPhaseObservation::NotScannable"));
	EXPECT_NE(std::string::npos, absent_branch.find("presence ="));
	EXPECT_NE(std::string::npos, absent_branch.find("validity_flags ="));
	EXPECT_EQ(std::string::npos,
		absent_branch.find("CargoScanStatePresenceFlagCargoText"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, MissionLoadShutdownAndTrueMissionLeaveResetControlToShip)
{
	const auto telemetry_source = read_source("code/telemetry/telemetry.cpp");
	const auto mission_load =
		function_body(telemetry_source, "void on_game_mission_load(const char*) noexcept");
	const auto leave =
		function_body(telemetry_source, "void on_game_leave_state(int old_state, int new_state) noexcept");
	const auto shutdown =
		function_body(telemetry_source, "void on_engine_shutdown() noexcept");
	for (const auto* body : {&mission_load, &shutdown}) {
		ASSERT_FALSE(body->empty());
		EXPECT_NE(std::string::npos,
			body->find("reset_phase2_mission_observation_state()"));
	}
	ASSERT_FALSE(leave.empty());
	EXPECT_EQ(0U, occurrence_count(leave, "reset_phase2_mission_observation_state()"));
	const auto runtime = read_source("code/telemetry/runtime.cpp");
	const auto lifecycle =
		function_body(runtime, "void Runtime::apply_pending_lifecycle() noexcept");
	EXPECT_NE(std::string::npos, lifecycle.find("needs_mission_purge"));
	EXPECT_NE(std::string::npos,
		lifecycle.find("reset_phase2_mission_observation_state()"));

	const auto observation_source = read_source("code/telemetry/phase2_observation.cpp");
	const auto reset = function_body(
		observation_source, "void reset_phase2_mission_observation_state() noexcept");
	ASSERT_FALSE(reset.empty());
	EXPECT_NE(std::string::npos, reset.find("has_control_target = true"));
	EXPECT_NE(std::string::npos,
		reset.find("control_target = ControlTargetAuthority::Ship"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ControlRootCapturesAllRawModeCursorAimAndRequestAuthorities)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	for (const auto token : {"autopilot_engaged", "player_use_ai", "engine_control_mode",
			 "flight_cursor_active", "flight_cursor_pitch", "flight_cursor_heading",
			 "flight_cursor_sensitivity", "effective_aim_extent",
			 "afterburner_requested"}) {
		EXPECT_NE(std::string::npos, header.find(token)) << token;
	}

	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto body = function_body(source,
		"bool FsoEngineReadView::read_player_controls(PlayerControlObservation& output) const noexcept");
	ASSERT_FALSE(body.empty());
	for (const auto token : {"AutoPilotEngaged", "Player_use_ai", "Player->control_mode",
			 "Player_flight_mode", "Player_flight_cursor.p", "Player_flight_cursor.h",
			 "Player_flight_cursor_sensitivity", "aims_at_flight_cursor",
			 "flight_cursor_aim_extent", "continuous_ongoing"}) {
		EXPECT_NE(std::string::npos, body.find(token)) << token;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, Phase2ProducerSliceHasNoSpecializedOrRenderingDependency)
{
	const auto profile_header = read_source("code/telemetry/phase2_profile_gate.h");
	const auto profile_source = read_source("code/telemetry/phase2_profile_gate.cpp");
	const auto observation_header = read_source("code/telemetry/phase2_observation.h");
	const auto observation_source = read_source("code/telemetry/phase2_observation.cpp");
	const auto combined = profile_header + profile_source + observation_header + observation_source;
	for (const auto forbidden :
		{"CommView", "TargetVideo", "FFmpeg", "ffmpeg", "OpenGL", "glad/", "graphics/"}) {
		EXPECT_EQ(std::string::npos, combined.find(forbidden)) << forbidden;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S12TST009ProducerAndLinkedInputsExcludeSpecializedCommandVideoAndRendering)
{
	const std::array<std::string_view, 8> producer_files{{
		"code/telemetry/phase2_profile_gate.h",
		"code/telemetry/phase2_profile_gate.cpp",
		"code/telemetry/phase2_observation.h",
		"code/telemetry/phase2_observation.cpp",
		"code/telemetry/engine_adapter.h",
		"code/telemetry/engine_adapter.cpp",
		"code/telemetry/native_session_runtime.h",
		"code/telemetry/native_session_runtime.cpp",
	}};
	std::string combined;
	for (const auto file : producer_files) {
		combined += read_source(file);
	}
	for (const auto forbidden :
		{"FFmpeg", "ffmpeg", "avcodec", "avformat", "OpenGL", "glad/",
			"TargetVideo", "COMM_", "CommView", "execute_command", "system(",
			"popen(", "placeholder"}) {
		EXPECT_EQ(std::string::npos, combined.find(forbidden)) << forbidden;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoTelemetryInvalidityNeverChangesGameplayReturnOrMutation)
{
	const auto source = read_source("code/playerman/playercontrol.cpp");
	const auto ship = function_body(source, "bool player_inspect_cargo(");
	const auto subsystem =
		nth_function_body(source, "bool player_inspect_cap_subsys_cargo(", 1U);
	ASSERT_FALSE(ship.empty());
	ASSERT_FALSE(subsystem.empty());
	EXPECT_EQ(std::string::npos, ship.find("!set_cargo_timing("));
	EXPECT_EQ(std::string::npos, subsystem.find("!set_cargo_timing("));
	EXPECT_EQ(std::string::npos, subsystem.find("if (!subsystem_key_found"));
	EXPECT_EQ(std::string::npos, ship.find("if (!assign_raw_cargo_text("));
	EXPECT_EQ(std::string::npos, subsystem.find("if (!assign_raw_cargo_text("));
	for (const auto token : {"ship_do_cargo_revealed(", "Player->cargo_inspect_time = 0",
			 "return true;"}) {
		EXPECT_NE(std::string::npos, ship.find(token)) << token;
	}
	for (const auto token : {"ship_do_cap_subsys_cargo_revealed(",
			 "Player->cargo_inspect_time = 0", "return true;"}) {
		EXPECT_NE(std::string::npos, subsystem.find(token)) << token;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoReadRequiresExactCurrentTargetAndSubsystem)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto body = function_body(source,
		"bool FsoEngineReadView::read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept");
	ASSERT_FALSE(body.empty());
	for (const auto token : {"Player_ai == nullptr", "Player_ai->target_objnum",
			 "&Objects[Player_ai->target_objnum]", "Player_ai->targeted_subsys",
			 "Player_ai->targeted_subsys->system_info"}) {
		EXPECT_NE(std::string::npos, body.find(token)) << token;
	}
	const auto current_target = body.find("Player_ai->target_objnum");
	if (current_target != std::string::npos) {
		EXPECT_LT(current_target, body.find("output.presence = cargo_authority.presence"));
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoCompletionPublishesElapsedAfterGameplayReset)
{
	const auto source = read_source("code/playerman/playercontrol.cpp");
	const auto ship = function_body(source, "bool player_inspect_cargo(");
	const auto subsystem =
		nth_function_body(source, "bool player_inspect_cap_subsys_cargo(", 1U);
	ASSERT_FALSE(ship.empty());
	ASSERT_FALSE(subsystem.empty());
	EXPECT_GE(occurrence_count(ship, "cargo_fact.elapsed_us = 0U;"), 2U);
	EXPECT_GE(occurrence_count(subsystem, "cargo_fact.elapsed_us = 0U;"), 2U);
	expect_sequence(ship, "ship_do_cargo_revealed(", "Player->cargo_inspect_time = 0",
		"cargo_fact.elapsed_us = 0U;");
	expect_sequence(subsystem, "ship_do_cap_subsys_cargo_revealed(",
		"Player->cargo_inspect_time = 0", "cargo_fact.elapsed_us = 0U;");
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, CargoTelemetryBoundsUseLiveCargoAndSubsystemCardinality)
{
	const auto source = read_source("code/playerman/playercontrol.cpp");
	const auto assign =
		function_body(source, "bool assign_raw_cargo_text(");
	const auto subsystem =
		nth_function_body(source, "bool player_inspect_cap_subsys_cargo(", 1U);
	ASSERT_FALSE(assign.empty());
	ASSERT_FALSE(subsystem.empty());
	EXPECT_NE(std::string::npos, assign.find("Num_cargo"));
	EXPECT_EQ(std::string::npos, assign.find("cargo_index >= MAX_CARGO"));
	EXPECT_NE(std::string::npos, subsystem.find("cargo_sip->n_subsystems < 0"));
	EXPECT_NE(std::string::npos, subsystem.find("cargo_sip->subsystems.size()"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8AdapterReadsOnlyBoundedAuthorizedClosure)
{
	const auto header = read_source("code/telemetry/engine_adapter.h");
	const auto observation_header = read_source("code/telemetry/phase2_observation.h");
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	for (const auto token : {"read_player_root_key(", "read_discovery_node(",
			 "read_ship("}) {
		EXPECT_NE(std::string::npos, header.find(token)) << token;
	}
	EXPECT_NE(std::string::npos, observation_header.find("Phase2ObservationSelection"));
	EXPECT_NE(std::string::npos, observation_header.find("collect_phase2_observation("));
	EXPECT_NE(std::string::npos, source.find("CoreGate"));
	EXPECT_NE(std::string::npos, source.find("Player_obj"));
	for (const auto forbidden : {"closure_ship_count", "Ship_obj_list", "allowlist",
			 "visited"}) {
		EXPECT_TRUE(header.find(forbidden) == std::string::npos &&
			source.find(forbidden) == std::string::npos) << forbidden;
	}
	const auto runtime = read_source("code/telemetry/runtime_adapter.cpp");
	EXPECT_NE(std::string::npos, runtime.find("selection"));
	EXPECT_NE(std::string::npos, runtime.find("player_root"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8SupportCopiesRawAuthorityWithoutInventedProgress)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto adapter_header =
		read_source("code/telemetry/engine_adapter.h");
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read.empty());
	EXPECT_NE(std::string::npos,
		adapter_header.find("evaluate_support_work("));
	EXPECT_NE(std::string::npos, read.find("evaluate_support_work("));
	for (const auto phase : {"ShipSupportPhase::Docking",
			 "ShipSupportPhase::Repairing",
			 "ShipSupportPhase::Rearming"})
		EXPECT_NE(std::string::npos, read.find(phase)) << phase;
	for (const auto token : {"raw_support_flags", "support_ship_objnum",
			 "support_ship_signature", "ai_index"}) {
		EXPECT_TRUE(header.find(token) != std::string::npos ||
			read.find(token) != std::string::npos) << token;
	}
	for (const auto forbidden : {"repair_progress = 0.0F",
			 "rearm_progress = 0.0F"}) {
		EXPECT_EQ(std::string::npos, read.find(forbidden)) << forbidden;
	}
	for (const auto token : {"MAX_AI_INFO", "support_ship_objnum", "MAX_OBJECTS",
			 "support_ship_signature", "Objects["}) {
		EXPECT_NE(std::string::npos, read.find(token)) << token;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8AdapterPreservesLimitStatusInsteadOfBoolCollapse)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto adapter_header = read_source("code/telemetry/engine_adapter.h");
	const auto adapter_source = read_source("code/telemetry/engine_adapter.cpp");
	EXPECT_NE(std::string::npos, header.find("SourceReadResult"));
	EXPECT_NE(std::string::npos, header.find("SourceLimitExceeded"));
	EXPECT_NE(std::string::npos,
		header.find("virtual SourceReadResult read_ship("));
	EXPECT_NE(std::string::npos,
		adapter_header.find("SourceReadResult read_ship("));
	const auto read = function_body(adapter_source,
		"SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read.empty());
	EXPECT_NE(std::string::npos, read.find("SourceLimitExceeded"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8DockingUsesEngineRangeAndBoundedCycleSafeTraversal)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto docking =
		function_body(source, "SourceReadResult read_direct_docking_facts(");
	const auto discovery =
		function_body(source, "SourceReadResult FsoEngineReadView::read_discovery_node(");
	ASSERT_FALSE(docking.empty());
	ASSERT_FALSE(discovery.empty());
	EXPECT_NE(std::string::npos, docking.find("4095"));
	EXPECT_EQ(std::string::npos, docking.find("65535"));
	EXPECT_NE(std::string::npos, docking.find("MaximumPhase2DockRelationsPerShip"));
	EXPECT_NE(std::string::npos, docking.find("docked_objp->instance"));
	EXPECT_NE(std::string::npos, docking.find("Ships["));
	EXPECT_NE(std::string::npos, docking.find("&Objects["));
	EXPECT_NE(std::string::npos, docking.find("inverse_relation_steps"));
	EXPECT_NE(std::string::npos, discovery.find("read_direct_docking_facts("));
	EXPECT_EQ(std::string::npos, docking.find("topology"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8SubsystemFactsKeepRadarDistinctAndExposeFutureRawInputs)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read.empty());
	const auto begin = header.find("enum class ShipSubsystemKind");
	const auto end = header.find("struct ShipSubsystemStorage", begin);
	ASSERT_NE(std::string::npos, begin);
	ASSERT_NE(std::string::npos, end);
	const auto subsystem = header.substr(begin, end - begin);
	EXPECT_NE(std::string::npos, subsystem.find("Radar"));
	for (const auto token : {"raw_flags", "armor_source_key", "disruption_remaining_us",
			 "position_local", "orientation_local", "aggregate", "turret",
			 "primary_bank", "secondary_bank", "turret_next_fire_pos",
			 "turret_beam_free", "turret_locked"}) {
		EXPECT_NE(std::string::npos, subsystem.find(token)) << token;
	}
	EXPECT_NE(std::string::npos, read.find("case SUBSYSTEM_RADAR:"));
	EXPECT_NE(std::string::npos, read.find("ShipSubsystemKind::Radar"));
	EXPECT_NE(std::string::npos, read.find("Ship::Weapon_Flags::Beam_Free"));
	EXPECT_NE(std::string::npos, read.find("Ship::Weapon_Flags::Turret_Lock"));
	for (const auto forbidden : {"target_object_signature", "target_subsystem",
			 "turret_enemy_objnum", "turret_enemy_sig", "next_fire_point"}) {
		EXPECT_EQ(std::string::npos, subsystem.find(forbidden)) << forbidden;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8MissionResetOccursOnlyAtExplicitMissionExit)
{
	const auto telemetry = read_source("code/telemetry/telemetry.cpp");
	const auto load = function_body(telemetry, "void on_game_mission_load(const char*) noexcept");
	const auto leave = function_body(telemetry, "void on_game_leave_state(int old_state, int new_state) noexcept");
	const auto shutdown = function_body(telemetry, "void on_engine_shutdown() noexcept");
	const auto runtime = read_source("code/telemetry/runtime.cpp");
	const auto lifecycle = function_body(runtime, "void Runtime::apply_pending_lifecycle() noexcept");
	ASSERT_FALSE(load.empty());
	ASSERT_FALSE(leave.empty());
	ASSERT_FALSE(shutdown.empty());
	ASSERT_FALSE(lifecycle.empty());
	EXPECT_NE(std::string::npos, load.find("reset_phase2_mission_observation_state()"));
	EXPECT_EQ(std::string::npos, leave.find("reset_phase2_mission_observation_state()"));
	EXPECT_NE(std::string::npos, shutdown.find("reset_phase2_mission_observation_state()"));
	EXPECT_NE(std::string::npos, lifecycle.find("needs_mission_purge"));
	EXPECT_NE(std::string::npos,
		lifecycle.find("reset_phase2_mission_observation_state()"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8CaptureComplexityIsLinearInAuthorizedCaps)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto collect = read_source("code/telemetry/phase2_observation.cpp");
	const auto discovery =
		function_body(source, "SourceReadResult FsoEngineReadView::read_discovery_node(");
	const auto read =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	const auto docking =
		function_body(source, "SourceReadResult read_direct_docking_facts(");
	ASSERT_FALSE(read.empty());
	ASSERT_FALSE(docking.empty());
	EXPECT_EQ(std::string::npos, source.find("closure_ship_count"));
	EXPECT_EQ(std::string::npos, source.find("Ship_obj_list"));
	EXPECT_EQ(std::string::npos, source.find("allowlist"));
	EXPECT_EQ(std::string::npos, source.find("visited"));
	EXPECT_NE(std::string::npos, docking.find("for (auto* inverse"));
	EXPECT_NE(std::string::npos, docking.find("inverse_relation_steps"));
	EXPECT_EQ(std::string::npos, read.find("for (std::size_t candidate"));
	EXPECT_EQ(std::string::npos, collect.find("signature_is_discovered"));
	EXPECT_NE(std::string::npos, docking.find("MaximumPhase2DockRelationsPerShip"));
	EXPECT_NE(std::string::npos, read.find("MaximumPhase2SubsystemsPerShip"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V3KeysResolveEveryAuthorizedSelectionAndExposeLeader)
{
	SCOPED_TRACE("REVIEW-S8V3-01 REQ-009 REQ-013 AC-004 D2-009");
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto discovery =
		function_body(source, "SourceReadResult FsoEngineReadView::read_discovery_node(");
	const auto ship =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	EXPECT_NE(std::string::npos, header.find("std::int32_t object_index"));
	EXPECT_NE(std::string::npos, header.find("object_signature"));
	for (const auto* body : {&discovery, &ship}) {
		ASSERT_FALSE(body->empty());
		EXPECT_NE(std::string::npos, body->find("key.object_index"));
		EXPECT_NE(std::string::npos, body->find("Objects["));
		EXPECT_EQ(std::string::npos, body->find("key.object_index != Player->objnum"));
	}
	for (const auto token : {
			 "Phase2CaptureLocalKey group_leader_capture_key",
			 "Phase2CaptureLocalKey support_capture_key"}) {
		EXPECT_NE(std::string::npos, header.find(token)) << token;
	}
	EXPECT_NE(std::string::npos,
		discovery.find("output.group_leader_capture_key.value"));
	EXPECT_NE(std::string::npos,
		discovery.find("output.support_capture_key.value"));
	EXPECT_EQ(std::string::npos, header.find("leader_object_index"));
	EXPECT_EQ(std::string::npos, header.find("leader_object_signature"));
	EXPECT_EQ(std::string::npos, ship.find("Ship_obj_list"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V3RawFactsAreExhaustiveWithoutInventedSupportPhase)
{
	SCOPED_TRACE("REVIEW-S8V3-03 REQ-012 REQ-013 D2-010 D2-020");
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto adapter_header =
		read_source("code/telemetry/engine_adapter.h");
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	for (const auto token : {"current_primary_bank", "current_secondary_bank",
			 "raw_weapon_flags", "tertiary_bank", "primary_slot", "secondary_slot",
			 "burst_counter", "weapon_animation", "turret_ammunition_current",
			 "turret_ammunition_capacity", "turret_cooldown_remaining_us",
			 "turret_current_direction", "turret_firing_point_count",
			 "turret_rof_scaler", "turret_animation", "dock_leader",
			 "local_dock_bay_name", "remote_dock_bay_name",
			 "raw_hull_repair_work", "raw_shield_repair_work",
			 "raw_subsystem_repair_work", "raw_weapon_energy_rearm_work",
			 "raw_ammunition_rearm_work"}) {
		EXPECT_NE(std::string::npos, header.find(token)) << token;
		EXPECT_NE(std::string::npos, source.find(token)) << token;
	}
	const auto ship =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	EXPECT_NE(std::string::npos,
		adapter_header.find("evaluate_support_work("));
	EXPECT_NE(std::string::npos, ship.find("evaluate_support_work("));
	for (const auto phase : {"ShipSupportPhase::Docking",
			 "ShipSupportPhase::Repairing",
			 "ShipSupportPhase::Rearming"})
		EXPECT_NE(std::string::npos, ship.find(phase)) << phase;
	for (const auto forbidden : {"repair_progress = 0.0F",
			 "rearm_progress = 0.0F"}) {
		EXPECT_EQ(std::string::npos, ship.find(forbidden)) << forbidden;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V3UsesExplicitScratchAndNoLargeStackDto)
{
	SCOPED_TRACE("REVIEW-S8V3-04/05 REQ-014 AC-004 D2-019");
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto source = read_source("code/telemetry/phase2_observation.cpp");
	EXPECT_EQ(std::string::npos, source.find("static Phase2ShipSource"));
	EXPECT_EQ(std::string::npos, source.find("static ShipObservationDto"));
	EXPECT_EQ(std::string::npos, source.find("ShipObservationDto ship;"));
	EXPECT_NE(std::string::npos, header.find("m_source_scratch"));
	EXPECT_NE(std::string::npos, source.find("emplace_back("));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V3DockInverseWalkIsExplicitlyBoundedAndLinear)
{
	SCOPED_TRACE("REVIEW-S8V3-06 REQ-013 AC-004 D2-010");
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto docking =
		function_body(source, "SourceReadResult read_direct_docking_facts(");
	ASSERT_FALSE(docking.empty());
	EXPECT_EQ(std::string::npos, docking.find("dock_find_dockpoint_used_by_object"));
	EXPECT_EQ(std::string::npos, docking.find("dock_check_find_direct_docked_object"));
	EXPECT_NE(std::string::npos, docking.find("inverse_relation_steps"));
	EXPECT_NE(std::string::npos, docking.find("MaximumPhase2DockRelationsPerShip"));
	EXPECT_EQ(std::string::npos, docking.find("visited"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V3ProjectionKeepsCoreGateIndependentFromDiscoveryExtension)
{
	SCOPED_TRACE("REVIEW-S8V3-07 REQ-009 REQ-012 AC-004 D2-020");
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto source = read_source("code/telemetry/phase2_observation.cpp");
	for (const auto token : {"Phase2ObservationProjection", "CoreGate",
			 "DiscoveryExtension"}) {
		EXPECT_NE(std::string::npos, header.find(token)) << token;
	}
	const auto collect = function_body(source,
		"static Phase2CaptureResult collect_phase2_observation_with_scratch(");
	ASSERT_FALSE(collect.empty());
	EXPECT_NE(std::string::npos, collect.find("projection"));
	EXPECT_NE(std::string::npos, collect.find("DiscoveryExtension"));
	EXPECT_NE(std::string::npos, collect.find("read_discovery_node("));
	EXPECT_LT(collect.find("read_discovery_node("), collect.find("read_ship("))
		<< "Opaque capture-local topology must be discovered before related "
		   "engine keys are resolved for ship reads.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S8AdapterFeedsEveryPreIdShipBlockWithoutDownstreamIds)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto collect = read_source("code/telemetry/phase2_observation.cpp");
	const auto read =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	const auto discovery =
		function_body(source, "SourceReadResult FsoEngineReadView::read_discovery_node(");
	ASSERT_FALSE(read.empty());
	for (const auto token : {"ShipWeaponsObservation weapons", "ShipSupportObservation support",
			 "ShipDockingObservation docking", "ShipSubsystemStorage subsystems"}) {
		EXPECT_NE(std::string::npos, header.find(token)) << token;
	}
	for (const auto token : {"output.weapons", "primary_bank_count", "secondary_bank_count",
			 "output.support", "support_capture_key", "output.docking",
			 "output.subsystems", "subsys_list"}) {
		EXPECT_NE(std::string::npos, read.find(token)) << token;
	}
	EXPECT_NE(std::string::npos, discovery.find("direct_docking_count"));
	for (const auto token : {"ship.weapons = source_ship.weapons", "ship.support = source_ship.support",
			 "ship.docking = source_ship.docking", "ship.subsystems = source_ship.subsystems"}) {
		EXPECT_NE(std::string::npos, collect.find(token)) << token;
	}
	for (const auto forbidden :
		{"entity_id", "manifest_id", "RecordType::", "FullSnapshot", "DeltaSnapshot"}) {
		EXPECT_EQ(std::string::npos, read.find(forbidden)) << forbidden;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S8SubsystemAndTurretSourcesHideTargetLockAndAwacs)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto begin = header.find("struct ShipSubsystemObservation");
	const auto end = header.find("struct ShipSubsystemStorage", begin);
	ASSERT_NE(std::string::npos, begin);
	ASSERT_NE(std::string::npos, end);
	const auto subsystem = header.substr(begin, end - begin);
	for (const auto forbidden : {"target_object_signature", "target_subsystem",
			 "awacs", "aim_point", "next_fire_point"}) {
		EXPECT_EQ(std::string::npos, subsystem.find(forbidden)) << forbidden;
	}

	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	for (const auto forbidden : {"turret_enemy_objnum", "turret_enemy_sig",
			 "awacs_intensity", "turret_pick_big_attack_point"}) {
		EXPECT_EQ(std::string::npos, read.find(forbidden)) << forbidden;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S9RuntimeOwnsAndProvisionsPhase2BeforeReady)
{
	const auto runtime_header = read_source("code/telemetry/runtime.h");
	const auto native_header = read_source("code/telemetry/native_session_runtime.h");
	const auto native_source = read_source("code/telemetry/native_session_runtime.cpp");
	const auto adapter_source = read_source("code/telemetry/runtime_adapter.cpp");
	const auto ownership = runtime_header + native_header + adapter_source;
	EXPECT_NE(std::string::npos, ownership.find("Phase2ObservationBuffer"));
	EXPECT_NE(std::string::npos, ownership.find("phase2_observation"));

	const auto start =
		function_body(native_source, "NativeSessionStartStatus NativeSessionRuntime::start(");
	ASSERT_FALSE(start.empty());
	EXPECT_NE(std::string::npos,
		start.find("Phase2ProvisioningMode::ValidEnabled"));
	const auto provision = start.find("phase2_observation->provision(");
	const auto enter_ready = start.find("phase2_observation->enter_ready()");
	EXPECT_NE(std::string::npos, provision);
	EXPECT_NE(std::string::npos, enter_ready);
	const auto bind = start.find("m_transport.open(");
	const auto ready = start.find("m_state = State::Started");
	if (provision != std::string::npos && enter_ready != std::string::npos) {
		EXPECT_LT(provision, enter_ready);
	}
	if (provision != std::string::npos && bind != std::string::npos) {
		EXPECT_LT(provision, bind);
	}
	if (provision != std::string::npos && ready != std::string::npos) {
		EXPECT_LT(provision, ready);
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S9DisabledAndAbsentExitBeforePhase2AllocationSocketOrCapture)
{
	const auto runtime_source = read_source("code/telemetry/runtime.cpp");
	const auto update = function_body(runtime_source, "void Runtime::on_engine_update() noexcept");
	ASSERT_FALSE(update.empty());
	const auto absent = update.find("case RuntimeConfigStatus::Absent:");
	const auto invalid = update.find("case RuntimeConfigStatus::Invalid:", absent);
	const auto disabled = update.find("case RuntimeConfigStatus::Disabled:", invalid);
	const auto enabled = update.find("case RuntimeConfigStatus::Enabled:", disabled);
	ASSERT_NE(std::string::npos, absent);
	ASSERT_NE(std::string::npos, invalid);
	ASSERT_NE(std::string::npos, disabled);
	ASSERT_NE(std::string::npos, enabled);
	const auto absent_branch = update.substr(absent, invalid - absent);
	const auto disabled_branch = update.substr(disabled, enabled - disabled);
	for (const auto* branch : {&absent_branch, &disabled_branch}) {
		EXPECT_NE(std::string::npos, branch->find("enter_disabled("));
		EXPECT_NE(std::string::npos, branch->find("return;"));
		for (const auto forbidden : {"Phase2", "provision(", "allocate_session_registry",
				 "start_transport", "service_tick", "socket"}) {
			EXPECT_EQ(std::string::npos, branch->find(forbidden)) << forbidden;
		}
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S9EngineUpdateRejectsWrongThreadBeforePhase2Read)
{
	const auto runtime_source = read_source("code/telemetry/runtime.cpp");
	const auto update = function_body(runtime_source, "void Runtime::on_engine_update() noexcept");
	ASSERT_FALSE(update.empty());
	expect_ordered(update, "callback_is_on_captured_thread()", "m_services.service_tick(context)",
		"case RuntimeTickStatus::PermanentTransportFailure:");

	const auto adapter_source = read_source("code/telemetry/runtime_adapter.cpp");
	const auto service = function_body(adapter_source,
		"RuntimeTickStatus RuntimeAdapterPlayerTestAccess::service_tick(");
	ASSERT_FALSE(service.empty());
	EXPECT_NE(std::string::npos, service.find("make_fso_engine_read_view()"));
	EXPECT_NE(std::string::npos, service.find("phase2"));
	EXPECT_NE(std::string::npos, service.find("current_thread_is_main"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S9OrdinaryCadencesKeepBlockFamiliesAtomicAndKeyframeForcesAll)
{
	const auto native_header = read_source("code/telemetry/native_session_runtime.h");
	const auto native_source = read_source("code/telemetry/native_session_runtime.cpp");
	const auto combined = native_header + native_source;
	for (const auto token : {"capture_flight_controls", "capture_systems",
			 "force_complete_keyframe", "producer_sample_time_us"}) {
		EXPECT_NE(std::string::npos, combined.find(token)) << token;
	}
	const auto tick = function_body(native_source,
		"NativeSessionTickStatus NativeSessionRuntime::service_tick(");
	ASSERT_FALSE(tick.empty());
	EXPECT_NE(std::string::npos, tick.find("collect_phase2_observation("));
	EXPECT_EQ(1U, occurrence_count(tick, "collect_phase2_observation("));
	EXPECT_NE(std::string::npos, tick.find("capture_flight_controls"));
	EXPECT_NE(std::string::npos, tick.find("capture_systems"));
	EXPECT_NE(std::string::npos, tick.find("force_complete_keyframe"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S9CaptureFailureClearsEverythingBeforeAnyPublication)
{
	const auto native_source = read_source("code/telemetry/native_session_runtime.cpp");
	const auto tick = function_body(native_source,
		"NativeSessionTickStatus NativeSessionRuntime::service_tick(");
	ASSERT_FALSE(tick.empty());
	for (const auto token : {"Phase2CaptureStatus::InvalidSource",
			 "Phase2CaptureStatus::SourceLimitExceeded",
			 "Phase2CaptureStatus::UnsupportedEngineState", "reset_observation",
			 "clear_phase2", "PermanentCaptureFailure"}) {
		EXPECT_NE(std::string::npos, tick.find(token)) << token;
	}
	const auto failure = tick.find("Phase2CaptureStatus::InvalidSource");
	const auto publish = tick.find("publish_phase2");
	if (failure != std::string::npos && publish != std::string::npos) {
		EXPECT_LT(failure, publish);
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, S9IntroducesNoWorkerSpscOrDownstreamPhase)
{
	const auto runtime_header = read_source("code/telemetry/runtime.h");
	const auto runtime_source = read_source("code/telemetry/runtime.cpp");
	const auto native_header = read_source("code/telemetry/native_session_runtime.h");
	const auto native_source = read_source("code/telemetry/native_session_runtime.cpp");
	const auto adapter_source = read_source("code/telemetry/runtime_adapter.cpp");
	const auto combined =
		runtime_header + runtime_source + native_header + native_source + adapter_source;
	for (const auto forbidden : {"Phase2CaptureWorker", "Phase2Spsc", "phase2_worker",
			 "phase2_spsc", "std::async", "systems_hz_phase2_final"}) {
		EXPECT_EQ(std::string::npos, combined.find(forbidden)) << forbidden;
	}
	const auto phase2_header = read_source("code/telemetry/phase2_observation.h");
	for (const auto forbidden :
		{"SessionController", "manifest_id", "baseline_id", "RecordType::"}) {
		EXPECT_EQ(std::string::npos, phase2_header.find(forbidden)) << forbidden;
	}
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V4PhysicalBankBoundsPrecedeDtoBounds)
{
	const auto source = read_source("code/telemetry/engine_adapter.cpp");
	const auto read =
		function_body(source, "SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read.empty());
	for (const auto physical :
		{"MAX_SHIP_PRIMARY_BANKS", "MAX_SHIP_SECONDARY_BANKS"}) {
		EXPECT_NE(std::string::npos, read.find(physical)) << physical;
	}
	const auto ship_counts = read.find("source_weapons.num_primary_banks");
	const auto ship_dto_limit = read.find("MaximumPhase2WeaponBanksPerFamily", ship_counts);
	const auto ship_physical_limit = read.find("MAX_SHIP_PRIMARY_BANKS", ship_counts);
	ASSERT_NE(std::string::npos, ship_counts);
	ASSERT_NE(std::string::npos, ship_dto_limit);
	ASSERT_NE(std::string::npos, ship_physical_limit);
	EXPECT_LT(ship_physical_limit, ship_dto_limit);

	const auto turret_counts = read.find("turret_weapons.num_primary_banks");
	const auto turret_dto_limit =
		read.find("MaximumPhase2WeaponBanksPerFamily", turret_counts);
	const auto turret_physical_limit = read.find("MAX_SHIP_PRIMARY_BANKS", turret_counts);
	ASSERT_NE(std::string::npos, turret_counts);
	ASSERT_NE(std::string::npos, turret_dto_limit);
	ASSERT_NE(std::string::npos, turret_physical_limit);
	EXPECT_LT(turret_physical_limit, turret_dto_limit);
	EXPECT_NE(std::string::npos, read.find("Phase2SourceReadStatus::UnsupportedEngineState"));
	EXPECT_NE(std::string::npos, read.find("Phase2SourceReadStatus::SourceLimitExceeded"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V4TurretDirectionUsesCurrentSubmodelTransform)
{
	const auto read = function_body(read_source("code/telemetry/engine_adapter.cpp"),
		"SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read.empty());
	EXPECT_NE(std::string::npos, read.find("model_instance_local_to_global_dir("));
	EXPECT_NE(std::string::npos, read.find("system_info->turret_norm"));
	EXPECT_NE(std::string::npos, read.find("turret_current_direction"));
	EXPECT_EQ(std::string::npos, read.find("turret_last_fire_direction"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V4SupportWorkUsesRealRepairAndRearmAuthorities)
{
	const auto read = function_body(read_source("code/telemetry/engine_adapter.cpp"),
		"SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read.empty());
	for (const auto fact :
		{"hull", "shield", "subsys", "secondary_bank_ammo",
			"secondary_bank_start_ammo", "weapon_energy"}) {
		EXPECT_NE(std::string::npos, read.find(fact)) << fact;
	}
	EXPECT_EQ(std::string::npos,
		read.find("ship_ai.ai_flags[AI::AI_Flags::Being_repaired] ? 1U : 0U"));
	EXPECT_EQ(std::string::npos,
		read.find("ship_ai.ai_flags[AI::AI_Flags::Awaiting_repair] ? 1U : 0U"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V4DiscoveryOwnsExactOptionalDockLeaderKey)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto discovery = function_body(read_source("code/telemetry/engine_adapter.cpp"),
		"SourceReadResult FsoEngineReadView::read_discovery_node(");
	ASSERT_FALSE(discovery.empty());
	EXPECT_NE(std::string::npos,
		header.find("Phase2CaptureLocalKey group_leader_capture_key"));
	EXPECT_NE(std::string::npos,
		discovery.find("output.group_leader_capture_key.value"));
	EXPECT_EQ(std::string::npos, header.find("EngineEntityKey dock_leader_key"));
	EXPECT_EQ(std::string::npos, header.find("bool has_dock_leader"));
	EXPECT_EQ(std::string::npos,
		discovery.find("output.leader_object_index = key.object_index"));
	EXPECT_EQ(std::string::npos,
		discovery.find("flags[Ship::Ship_Flags::Dock_leader])"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V4InverseDockBudgetAllowsIndependentFortyAndThirtyLists)
{
	const auto docking = function_body(read_source("code/telemetry/engine_adapter.cpp"),
		"SourceReadResult read_direct_docking_facts(");
	ASSERT_FALSE(docking.empty());
	const auto direct_loop = docking.find("for (auto* relation");
	const auto inverse_budget = docking.find("inverse_relation_steps = 0U", direct_loop);
	const auto inverse_loop = docking.find("for (auto* inverse_relation", direct_loop);
	ASSERT_NE(std::string::npos, direct_loop);
	ASSERT_NE(std::string::npos, inverse_budget) <<
		"Each direct relation needs an independent inverse-list budget; 40+30 is valid.";
	ASSERT_NE(std::string::npos, inverse_loop);
	EXPECT_LT(inverse_budget, inverse_loop);
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V4TertiaryDomainErrorsAreUnsupportedNotCapacityLimits)
{
	const auto read = function_body(read_source("code/telemetry/engine_adapter.cpp"),
		"SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read.empty());
	const auto tertiary = read.find("source_weapons.num_tertiary_banks < 0");
	const auto dto_limit = read.find("Phase2SourceReadStatus::SourceLimitExceeded", tertiary);
	const auto unsupported =
		read.find("Phase2SourceReadStatus::UnsupportedEngineState", tertiary);
	ASSERT_NE(std::string::npos, tertiary);
	ASSERT_NE(std::string::npos, unsupported);
	EXPECT_TRUE(dto_limit == std::string::npos || unsupported < dto_limit) <<
		"Negative tertiary values and ammo above capacity are invalid engine state.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V4DiscoveryFailureIsObservableWithoutPoisoningCoreGate)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto collect = function_body(read_source("code/telemetry/phase2_observation.cpp"),
		"static Phase2CaptureResult collect_phase2_observation_with_scratch(");
	ASSERT_FALSE(collect.empty());
	EXPECT_NE(std::string::npos, header.find("discovery_capture"));
	EXPECT_NE(std::string::npos, header.find("discovery_status"));
	EXPECT_EQ(std::string::npos, collect.find("(void)source.read_discovery_node("));
	EXPECT_NE(std::string::npos, collect.find("const auto discovery_result"));
	EXPECT_NE(std::string::npos, collect.find("Phase2ObservationProjection::CoreGate"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS8V4StartupOwnedBudgetIsMeasuredAndRejectedBeforeTransport)
{
	const auto budget_header = read_source("code/telemetry/startup_budget.h");
	const auto native = read_source("code/telemetry/native_session_runtime.cpp");
	const auto start =
		function_body(native, "NativeSessionStartStatus NativeSessionRuntime::start(");
	ASSERT_FALSE(start.empty());
	for (const auto cap : {"Phase2SharedOwnedCapBytes",
			 "Phase2ClientOwnedCapBytes",
			 "Phase2ProcessOwnedCapBytes"})
		EXPECT_NE(std::string::npos, budget_header.find(cap)) << cap;
	EXPECT_NE(std::string::npos, start.find("phase2_observation->owned_bytes()"));
	EXPECT_NE(std::string::npos, start.find("checked_add_size("));
	EXPECT_NE(std::string::npos,
		start.find("calculate_phase2_owned_budget("));
	const auto measured = start.find("phase2_observation->owned_bytes()");
	const auto calculated = start.find("calculate_phase2_owned_budget(");
	const auto transport = start.find("m_transport.open(");
	ASSERT_NE(std::string::npos, measured);
	ASSERT_NE(std::string::npos, calculated);
	ASSERT_NE(std::string::npos, transport);
	EXPECT_LT(measured, transport);
	EXPECT_LT(calculated, transport);
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS9V4Phase2ActivationAndCadencesAreProvisionalAndSeparated)
{
	const auto header = read_source("code/telemetry/native_session_runtime.h");
	const auto source = read_source("code/telemetry/native_session_runtime.cpp");
	const auto start =
		function_body(source, "NativeSessionStartStatus NativeSessionRuntime::start(");
	const auto tick = function_body(source,
		"NativeSessionTickStatus NativeSessionRuntime::service_tick(");
	ASSERT_FALSE(start.empty());
	ASSERT_FALSE(tick.empty());
	EXPECT_NE(std::string::npos, header.find("Phase2Profile selected_phase2_profile"));
	EXPECT_NE(std::string::npos, start.find("selected_phase2_profile"));
	EXPECT_NE(std::string::npos, start.find("Phase2ProvisioningMode::ValidDisabled"));
	EXPECT_NE(std::string::npos, tick.find("m_phase2_enabled"));
	EXPECT_NE(std::string::npos, tick.find("flight_controls_cadence"));
	EXPECT_NE(std::string::npos, tick.find("systems_cadence"));
	EXPECT_LT(tick.find("m_phase2_enabled"), tick.find("collect_phase2_observation("));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract, ReviewerS9V4KeyframePreparationHasExplicitTestSeamWithoutFinalClaim)
{
	const auto header = read_source("code/telemetry/native_session_runtime.h");
	const auto source = read_source("code/telemetry/native_session_runtime.cpp");
	const auto combined = header + source;
	EXPECT_NE(std::string::npos, combined.find("prepare_phase2_keyframe"));
	EXPECT_NE(std::string::npos, combined.find("phase2_keyframe_test_seam"));
	EXPECT_NE(std::string::npos, combined.find("force_complete_keyframe"));
	EXPECT_EQ(std::string::npos, combined.find("phase2_keyframe_final_certified"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerS8V5TurretCurrentDirectionIsCanonicalShipLocalAuthority)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto observation = read_source("code/telemetry/phase2_observation.cpp");
	const auto read = function_body(read_source("code/telemetry/engine_adapter.cpp"),
		"SourceReadResult FsoEngineReadView::read_ship(");
	ASSERT_FALSE(read.empty());
	EXPECT_NE(std::string::npos, header.find("turret_current_direction_local"))
		<< "The coordinate frame is part of the owned DTO contract.";
	EXPECT_NE(std::string::npos, read.find("model_instance_local_to_global_dir("));
	EXPECT_EQ(std::string::npos, read.find("&Player_obj->orient"))
		<< "World orientation must not leak into the ship-local turret direction.";
	EXPECT_TRUE(read.find("&vmd_identity_matrix") != std::string::npos ||
		read.find("vm_vec_unrotate") != std::string::npos)
		<< "Use an identity transform or explicitly remove world orientation.";
	EXPECT_NE(std::string::npos, read.find("vm_vec_normalize"))
		<< "The local direction authority must be normalized before DTO storage.";
	EXPECT_NE(std::string::npos, observation.find("turret_current_direction_local"));
	EXPECT_NE(std::string::npos,
		observation.find("canonicalize_array"))
		<< "The local fixture oracle is finite, normalized and component-canonicalized.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerS8V5SupportRawAuthoritiesAreExhaustiveAndDimensionallySeparate)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto read = function_body(read_source("code/telemetry/engine_adapter.cpp"),
		"SourceReadResult FsoEngineReadView::read_ship(");
	const auto shared_support = read_source("code/ship/ship.cpp");
	const auto support_authorities = read + shared_support;
	ASSERT_FALSE(read.empty());
	for (const auto authority : {
			 "Mission::Mission_Flags::Support_repairs_hull",
			 "The_mission.support_ships.max_hull_repair_val",
			 "The_mission.support_ships.max_subsys_repair_val",
			 "sup_hull_repair_rate",
			 "sup_shield_repair_rate",
			 "sup_subsys_repair_rate",
			 "SecondaryNoAmmo",
			 "num_tertiary_banks",
			 "tertiary_bank_ammo",
			 "turret_primary_banks",
			 "turret_secondary_banks"}) {
		EXPECT_NE(std::string::npos, support_authorities.find(authority)) << authority;
	}
	for (const auto applicability : {
			 "raw_hull_repair_applicable",
			 "raw_shield_repair_applicable",
			 "raw_subsystem_repair_applicable",
			 "raw_weapon_energy_rearm_applicable",
			 "raw_ammunition_rearm_applicable"}) {
		EXPECT_NE(std::string::npos, header.find(applicability)) << applicability;
		EXPECT_NE(std::string::npos, read.find(applicability)) << applicability;
	}
	EXPECT_EQ(std::string::npos, header.find("float raw_repair_work"))
		<< "Hull/shield/subsystem work cannot be collapsed into one heterogeneous float.";
	EXPECT_EQ(std::string::npos, header.find("float raw_rearm_work"))
		<< "Energy and ammunition cannot be collapsed into one heterogeneous float.";
	EXPECT_EQ(std::string::npos, read.find("static_cast<float>(output.support.raw_ammunition"))
		<< "Integer ammunition work must never be folded into a float aggregate.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerS8V5SelectionProjectionMakesCompleteShipMultiKeyFailureDecidable)
{
	const auto header = read_source("code/telemetry/phase2_observation.h");
	const auto collect = function_body(read_source("code/telemetry/phase2_observation.cpp"),
		"static Phase2CaptureResult collect_phase2_observation_with_scratch(");
	ASSERT_FALSE(collect.empty());
	const auto selection_api =
		header.find("const Phase2ObservationSelection& selection");
	ASSERT_NE(std::string::npos, selection_api);
	EXPECT_NE(std::string::npos,
		header.find("Phase2ObservationDto& output,\n\tPhase2ObservationProjection projection",
			selection_api))
		<< "The owned WP03 selection must be executable under CompleteShip, not only DiscoveryExtension.";
	EXPECT_TRUE(collect.find("first_discovery_failure") != std::string::npos ||
		collect.find("worst_discovery") != std::string::npos)
		<< "Multi-key discovery aggregation must preserve the first or worst failure.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerS8V5OwnedBudgetFormulaIsInclusiveSingleCountAndPreBind)
{
	const auto observation = read_source("code/telemetry/phase2_observation.cpp");
	const auto owned = function_body(observation,
		"std::size_t Phase2ObservationBuffer::owned_bytes() const noexcept");
	const auto runtime_header = read_source("code/telemetry/native_session_runtime.h");
	const auto start = function_body(read_source("code/telemetry/native_session_runtime.cpp"),
		"NativeSessionStartStatus NativeSessionRuntime::start(");
	ASSERT_FALSE(owned.empty());
	ASSERT_FALSE(start.empty());
	EXPECT_NE(std::string::npos, owned.find("sizeof(Phase2ObservationBuffer)"))
		<< "The inline buffer and inline DTO are owned storage too.";
	EXPECT_NE(std::string::npos, owned.find(".capacity() * sizeof(ShipObservationDto)"));
	EXPECT_NE(std::string::npos, owned.find("sizeof(Phase2ShipSource)"));
	EXPECT_NE(std::string::npos, runtime_header.find("startup_owned_bytes"))
		<< "A test seam must expose the exact inclusive total used at the cap boundary.";
	EXPECT_NE(std::string::npos,
		runtime_header.find("Phase2OwnedBudget m_phase2_owned_budget"));
	const auto measured = start.find("phase2_observation->owned_bytes()");
	const auto checked = start.find("checked_add_size(", measured);
	const auto budget = start.find("calculate_phase2_owned_budget(", checked);
	const auto bind = start.find("m_transport.open(");
	ASSERT_NE(std::string::npos, measured);
	ASSERT_NE(std::string::npos, checked);
	ASSERT_NE(std::string::npos, budget);
	ASSERT_NE(std::string::npos, bind);
	EXPECT_LT(measured, checked);
	EXPECT_LT(checked, budget);
	EXPECT_LT(budget, bind);
	EXPECT_EQ(start.find("phase2_observation->owned_bytes()"),
		start.rfind("phase2_observation->owned_bytes()"))
		<< "Measure a shared owner once, then reuse the measured value.";
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerS9V5CompleteShipRequiresOwnedWp03ClosureBeforeAllocationOrTransport)
{
	const auto header = read_source("code/telemetry/native_session_runtime.h");
	const auto start = function_body(read_source("code/telemetry/native_session_runtime.cpp"),
		"NativeSessionStartStatus NativeSessionRuntime::start(");
	ASSERT_FALSE(start.empty());
	EXPECT_TRUE(header.find("Phase2ObservationSelection phase2_selection") != std::string::npos ||
		header.find("const Phase2ObservationSelection* phase2_selection") != std::string::npos)
		<< "CompleteShip activation requires an explicitly owned WP03 closure.";
	const auto complete = start.find("Phase2Profile::CompleteShip");
	const auto allocate = start.find("SessionController controller");
	const auto bind = start.find("m_transport.open(");
	ASSERT_NE(std::string::npos, complete)
		<< "Pre-WP03 runtime must explicitly reject CompleteShip.";
	ASSERT_NE(std::string::npos, allocate);
	ASSERT_NE(std::string::npos, bind);
	EXPECT_LT(complete, allocate);
	EXPECT_LT(complete, bind);
	EXPECT_NE(std::string::npos,
		start.find("NativeSessionStartStatus::InvalidConfiguration", complete));
	EXPECT_EQ(std::string::npos, start.find("systems_hz_phase2_final"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerFinalBSupportWorkUsesOneSharedEngineOracle)
{
	const auto header = read_source("code/telemetry/engine_adapter.h");
	const auto adapter = read_source("code/telemetry/engine_adapter.cpp");
	const auto ship = read_source("code/ship/ship.cpp");
	const auto dto = read_source("code/telemetry/phase2_observation.h");
	EXPECT_NE(std::string::npos, header.find("evaluate_support_work("));
	EXPECT_NE(std::string::npos, adapter.find("evaluate_support_work("));
	const auto rearm = function_body(ship, "ship_do_rearm_frame(");
	ASSERT_FALSE(rearm.empty());
	EXPECT_NE(std::string::npos, rearm.find("evaluate_support_work("))
		<< "Gameplay and telemetry must execute the same support-work oracle.";
	EXPECT_EQ(std::string::npos,
		rearm.find("(void)telemetry::detail::evaluate_support_work("))
		<< "Gameplay must consume the evaluation instead of observing and discarding it.";
	for (const auto consumed : {
			 "observed_support_work.mission_rearm_disallowed",
			 "observed_support_work.weapon_rearm_disallowed",
			 "observed_support_work.countermeasure_rearm_applicable",
			 "observed_support_work.hull_repair_applicable",
			 "observed_support_work.subsystem_repair_applicable"}) {
		EXPECT_NE(std::string::npos, rearm.find(consumed)) << consumed;
	}
	for (const auto authority : {
			 "Support_rearm", "disallow_rearm", "rearm_pool", "team",
			 "countermeasure", "max_hull_repair_val", "max_subsys_repair_val",
			 "sup_hull_repair_rate", "sup_shield_repair_rate",
			 "sup_subsys_repair_rate"}) {
		EXPECT_TRUE(adapter.find(authority) != std::string::npos ||
			ship.find(authority) != std::string::npos) << authority;
	}
	for (const auto field : {
			 "raw_countermeasure_rearm_work",
			 "raw_countermeasure_capacity",
			 "raw_countermeasure_rearm_pool",
			 "raw_mission_rearm_disallowed",
			 "raw_weapon_rearm_disallowed"}) {
		EXPECT_NE(std::string::npos, dto.find(field)) << field;
	}
	EXPECT_EQ(std::string::npos, adapter.find("std::clamp(max_hull_repair_val"));
	EXPECT_EQ(std::string::npos, adapter.find("std::clamp(max_subsys_repair_val"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerFinalBSupportOracleHasExactTableDrivenFixture)
{
	const auto fixture = read_source(
		"test/src/telemetry/producer/test_support_work_contract.cpp");
	for (const auto branch : {
			 "mission_disallow_rearm",
			 "weapon_info_disallow_rearm",
			 "mission_rearm_pool_class_team",
			 "countermeasure_current_below_maximum",
			 "max_hull_repair_val_minus_one",
			 "max_hull_repair_val_101",
			 "max_subsys_repair_val_minus_one",
			 "max_subsys_repair_val_101",
			 "rate_and_applicability",
			 "shared_oracle_equals_adapter_capture"}) {
		EXPECT_NE(std::string::npos, fixture.find(branch)) << branch;
	}
	EXPECT_NE(std::string::npos, fixture.find("UnsupportedEngineState"));
	EXPECT_NE(std::string::npos, fixture.find("INSTANTIATE_TEST_SUITE_P"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerFinalTst007RequiresExecutableGameplayAbHarness)
{
	const auto root = std::filesystem::path{FSO_PHASE2_SOURCE_ROOT};
	const auto seam_path =
		root / "code/telemetry/phase2_gameplay_ab_test_seam.h";
	ASSERT_TRUE(std::filesystem::is_regular_file(seam_path))
		<< "RED TST007: production must expose a test-only seam that invokes the "
		   "real gameplay entry points and snapshots their globals.";

	const auto seam = read_source("code/telemetry/phase2_gameplay_ab_test_seam.h");
	const auto ship = read_source("code/ship/ship.cpp");
	const auto harness = read_source(
		"test/src/telemetry/producer/test_support_work_contract.cpp");
	for (const auto seam_contract : {
			 "GameplayAbInvoke",
			 "GameplayAbCapture",
			 "GameplayAbSnapshot",
			 "run_phase2_gameplay_ab"}) {
		EXPECT_NE(std::string::npos, seam.find(seam_contract)) << seam_contract;
	}
	for (const auto proof : {
			 "RealPlayerControlsGameplayAbHarnessUsesEngineGlobalOffOnOff",
			 "read_player_controls(nullptr",
			 "ai_do_objects_repairing_stuff(",
			 "REPAIR_INFO_BROKEN",
			 "hud_cargo_scan_update(&target_object",
			 "Ship::Ship_Flags::Scannable",
			 "Target_display_cargo",
			 "state.support_return = ship_do_rearm_frame(",
			 "::ship_cleanup(state.cleanup_ship_index",
			 "game_is_photo_mode_active()",
			 "Photo_mode_active",
			 "run_phase2_gameplay_ab(invoke, capture",
			 "run.snapshots[0]",
			 "run.snapshots[1]",
			 "run.snapshots[2]"}) {
		EXPECT_NE(std::string::npos, harness.find(proof)) << proof;
	}
	EXPECT_EQ(std::string::npos, harness.find("same_gameplay_return"));
	EXPECT_EQ(std::string::npos, harness.find("same_gameplay_mutations"));
	for (const auto forbidden_runtime_seam : {
			 "register_test_ship_in_ship_obj_list",
			 "unregister_test_ship_from_ship_obj_list"}) {
		EXPECT_EQ(std::string::npos, seam.find(forbidden_runtime_seam))
			<< forbidden_runtime_seam;
		EXPECT_EQ(std::string::npos, ship.find(forbidden_runtime_seam))
			<< forbidden_runtime_seam;
	}
	EXPECT_NE(std::string::npos,
		harness.find("register_fixture_ship_in_ship_obj_list"));
	EXPECT_NE(std::string::npos,
		harness.find("unregister_fixture_ship_from_ship_obj_list"));
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerFinalTst008EligibilityRejectionPrecedesEveryOwnedAllocationAndBind)
{
	const auto header = read_source("code/telemetry/native_session_runtime_test_seam.h");
	const auto start = function_body(read_source("code/telemetry/native_session_runtime.cpp"),
		"NativeSessionStartStatus NativeSessionRuntime::start(");
	ASSERT_FALSE(start.empty());
	EXPECT_NE(std::string::npos, header.find("startup_allocation_count"));
	const auto gate = start.find("select_phase2_profile(");
	const auto cadence = start.find("Capture30Hz capture_cadence");
	const auto controller = start.find("SessionController controller");
	const auto bind = start.find("m_transport.open(");
	ASSERT_NE(std::string::npos, gate);
	ASSERT_NE(std::string::npos, cadence);
	ASSERT_NE(std::string::npos, controller);
	ASSERT_NE(std::string::npos, bind);
	EXPECT_LT(gate, cadence);
	EXPECT_LT(gate, controller);
	EXPECT_LT(gate, bind);
}

TEST(TelemetryPhase2EngineIntegrationSourceContract,
	ReviewerFinalTst009ScansBuiltAdapterArtifactForSpecializedLeakage)
{
#if !defined(FSO_PHASE2_MSVC_LINK_EVIDENCE) || !FSO_PHASE2_MSVC_LINK_EVIDENCE
	GTEST_SKIP()
		<< "TST009 linker-visible object evidence is an explicit MSVC-only "
		   "contract.";
#else
	const auto root = std::filesystem::path{FSO_PHASE2_SOURCE_ROOT};
	const auto adapter = std::filesystem::path{FSO_PHASE2_ENGINE_ADAPTER_OBJECT};
	ASSERT_TRUE(std::filesystem::is_regular_file(adapter))
		<< "TST009 must scan the engine_adapter.obj from this exact configured build.";

	const auto artifact_time = std::filesystem::last_write_time(adapter);
	for (const auto relative_source : {
			 "code/telemetry/engine_adapter.cpp",
			 "code/telemetry/engine_adapter.h",
			 "code/telemetry/phase2_observation.h",
			 "code/ship/support_work.h"}) {
		const auto source = root / relative_source;
		ASSERT_TRUE(std::filesystem::is_regular_file(source)) << source.string();
		EXPECT_GE(artifact_time, std::filesystem::last_write_time(source))
			<< "stale artifact: " << adapter.string()
			<< " predates " << source.string();
	}

	const auto sha_path = adapter.string() + ".sha256";
	ASSERT_TRUE(std::filesystem::is_regular_file(sha_path))
		<< "RED TST009: the exact build object needs a recorded SHA-256 sidecar.";
	EXPECT_GE(std::filesystem::last_write_time(sha_path), artifact_time)
		<< "RED TST009: recorded SHA-256 predates the exact object.";
	std::ifstream sha_input(sha_path, std::ios::binary);
	const std::string recorded_sha{std::istreambuf_iterator<char>{sha_input},
		std::istreambuf_iterator<char>{}};
	EXPECT_EQ(64U, recorded_sha.size())
		<< "RED TST009: SHA-256 sidecar must contain exactly 64 hex digits.";
	if (!recorded_sha.empty()) {
		EXPECT_TRUE(std::all_of(recorded_sha.begin(), recorded_sha.end(),
			[](unsigned char value) { return std::isxdigit(value) != 0; }));
	}

	const auto linkable_path = adapter.string() + ".linkable.txt";
	ASSERT_TRUE(std::filesystem::is_regular_file(linkable_path))
		<< "RED TST009: record LINK /DUMP /SYMBOLS /IMPORTS output for this exact object.";
	EXPECT_GE(std::filesystem::last_write_time(linkable_path), artifact_time)
		<< "RED TST009: recorded linkable-symbol proof predates the exact object.";
	std::ifstream linkable_input(linkable_path, std::ios::binary);
	const std::string linkable{std::istreambuf_iterator<char>{linkable_input},
		std::istreambuf_iterator<char>{}};
	ASSERT_FALSE(linkable.empty());
	EXPECT_EQ(std::string::npos, linkable.find("ANONYMOUS OBJECT"))
		<< "RED TST009: an MSVC LTCG placeholder exposes no linker-visible "
		   "symbols and is therefore vacuous evidence.";
	const std::array<std::string_view, 3U> required_adapter_markers{{
		"FsoEngineReadView",
		"extract_production_static_authorities",
		"map_phase2_static_authorities"}};
	EXPECT_TRUE(std::any_of(required_adapter_markers.begin(),
		required_adapter_markers.end(),
		[&linkable](const auto marker) {
			return linkable.find(marker) != std::string::npos;
		}))
		<< "RED TST009: linker-visible evidence must identify the engine "
		   "adapter or its shared static-authority mapper.";
	// Only linker-visible symbols, directives, imports, and dependencies are
	// scanned. CodeView type-name substrings in the raw OBJ are intentionally
	// excluded from this proof.
	for (const auto forbidden : {
			 "telemetry_specialized_lifecycle",
			 "validate_specialized_lifecycle",
			 "encode_command",
			 "decode_command",
			 "encode_comm_view_state",
			 "decode_comm_view_state",
			 "comm_view_",
			 "encode_target_video",
			 "decode_target_video",
			 "render_target_video",
			 "target_video_",
			 "avcodec_",
			 "avformat_",
			 "avutil_",
			 "sws_",
			 "avcodec.lib",
			 "avformat.lib",
			 "avutil.lib",
			 "swscale.lib",
			 "opengl32.lib",
			 "glBind",
			 "glDraw"}) {
		EXPECT_EQ(std::string::npos, linkable.find(forbidden))
			<< forbidden << " leaked into " << adapter.string();
	}
#endif
}

} // namespace
