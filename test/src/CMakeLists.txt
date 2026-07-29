
include(source_groups.cmake)

add_executable(unittests ${source_files})

target_compile_features(unittests PUBLIC cxx_std_17)

target_link_libraries(unittests PRIVATE gtest)
target_link_libraries(unittests PRIVATE code)

target_include_directories(unittests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(unittests PROPERTIES FOLDER "tests")

file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../test_data" TEST_DATA_PATH)
string(REPLACE "\\" "\\\\" TEST_DATA_PATH "${TEST_DATA_PATH}")
target_compile_definitions(unittests PRIVATE "TEST_DATA_PATH=\"${TEST_DATA_PATH}\"")

file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/test/telemetry/protocol" FSTL_PROTOCOL_TEST_ASSET_PATH)
string(REPLACE "\\" "\\\\" FSTL_PROTOCOL_TEST_ASSET_PATH "${FSTL_PROTOCOL_TEST_ASSET_PATH}")
target_compile_definitions(unittests PRIVATE "FSTL_PROTOCOL_TEST_ASSET_PATH=\"${FSTL_PROTOCOL_TEST_ASSET_PATH}\"")
target_compile_definitions(unittests PRIVATE
	FSO_PHASE2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
	FSO_PHASE2_ENGINE_ADAPTER_OBJECT="${CMAKE_BINARY_DIR}/code/code.dir/$<CONFIG>/engine_adapter.obj"
)

INCLUDE(util)
COPY_FILES_TO_TARGET(unittests)

set(TELEMETRY_ISOLATED_RUNTIME_SOURCES
	${CMAKE_SOURCE_DIR}/code/telemetry/runtime.cpp
	telemetry/producer/telemetry_runtime_disabled_adapter.cpp
)

add_executable(telemetry_initialize_contract_tests
	telemetry/producer/test_telemetry_initialize_instrumented.cpp
	${TELEMETRY_ISOLATED_RUNTIME_SOURCES}
	${CMAKE_SOURCE_DIR}/code/telemetry/telemetry.cpp
	${CMAKE_SOURCE_DIR}/code/events/events.cpp
)

target_compile_features(telemetry_initialize_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_initialize_contract_tests PRIVATE FSO_TELEMETRY_TEST_SEAMS=1)
target_include_directories(telemetry_initialize_contract_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_initialize_contract_tests PRIVATE gtest_main sdl2)

set_target_properties(telemetry_initialize_contract_tests PROPERTIES FOLDER "tests")

add_executable(telemetry_lifecycle_callback_contract_tests
	telemetry/producer/test_telemetry_lifecycle_callbacks_instrumented.cpp
	telemetry/producer/telemetry_runtime_ready_adapter.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/runtime.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/telemetry.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
	${CMAKE_SOURCE_DIR}/code/events/events.cpp
)

target_compile_features(telemetry_lifecycle_callback_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_lifecycle_callback_contract_tests PRIVATE FSO_TELEMETRY_TEST_SEAMS=1)
target_include_directories(telemetry_lifecycle_callback_contract_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_lifecycle_callback_contract_tests PRIVATE gtest_main sdl2)

set_target_properties(telemetry_lifecycle_callback_contract_tests PROPERTIES FOLDER "tests")

add_executable(telemetry_disabled_benchmark
	telemetry/producer/telemetry_disabled_benchmark.cpp
	${TELEMETRY_ISOLATED_RUNTIME_SOURCES}
	${CMAKE_SOURCE_DIR}/code/telemetry/telemetry.cpp
	${CMAKE_SOURCE_DIR}/code/events/events.cpp
)

target_compile_features(telemetry_disabled_benchmark PRIVATE cxx_std_17)
target_include_directories(telemetry_disabled_benchmark PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_disabled_benchmark PRIVATE sdl2)

set_target_properties(telemetry_disabled_benchmark PROPERTIES FOLDER "tests")

add_executable(telemetry_native_performance_runner
	telemetry/producer/telemetry_native_performance_runner.cpp
	test_stubs.cpp
)

target_compile_features(telemetry_native_performance_runner PRIVATE cxx_std_17)
target_compile_definitions(telemetry_native_performance_runner PRIVATE FSO_TELEMETRY_TEST_SEAMS=1)
target_include_directories(telemetry_native_performance_runner PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_native_performance_runner PRIVATE code)

set_target_properties(telemetry_native_performance_runner PROPERTIES
	FOLDER "tests"
	OUTPUT_NAME "telemetry_native_performance_runner_p93"
	# Keep the executable next to the engine's runtime DLLs.  A test/bin output
	# directory produces a runner that cannot be launched without a manual PATH
	# workaround and caused the P9.3 campaign to pick up a stale binary.
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

# WP11 evidence harness.  It remains test-only and deliberately separate from
# P9.3 so profile injection/lifecycle proof cannot be mistaken for a benchmark.
add_executable(telemetry_phase1_reliability_harness
	telemetry/producer/telemetry_phase1_reliability_harness.cpp
	test_stubs.cpp
)
target_compile_features(telemetry_phase1_reliability_harness PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase1_reliability_harness PRIVATE FSO_TELEMETRY_TEST_SEAMS=1)
target_include_directories(telemetry_phase1_reliability_harness PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase1_reliability_harness PRIVATE code)
set_target_properties(telemetry_phase1_reliability_harness PROPERTIES
	FOLDER "tests"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

add_executable(telemetry_phase2_loss_harness
	telemetry/producer/telemetry_phase2_loss_harness.cpp
	test_stubs.cpp
)
target_compile_features(telemetry_phase2_loss_harness PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_loss_harness PRIVATE
	FSO_TELEMETRY_TEST_SEAMS=1)
target_include_directories(telemetry_phase2_loss_harness PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_loss_harness PRIVATE code)
if(MSVC)
	target_link_options(telemetry_phase2_loss_harness PRIVATE "/STACK:8388608")
endif()
set_target_properties(telemetry_phase2_loss_harness PROPERTIES
	FOLDER "tests/telemetry/phase2"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
add_test(NAME telemetry_phase2_loss_harness_smoke
	COMMAND telemetry_phase2_loss_harness
		--smoke
		--profile complete-ship
		--loss-rate 20
		--mode burst
		--seed 1345474380
		--warmup-seconds 0
		--duration-seconds 4
		--decision-log "${CMAKE_BINARY_DIR}/Testing/Temporary/phase2-loss-smoke.jsonl"
		--report "${CMAKE_BINARY_DIR}/Testing/Temporary/phase2-loss-smoke.json")
if(WIN32)
	set_tests_properties(telemetry_phase2_loss_harness_smoke PROPERTIES
		ENVIRONMENT_MODIFICATION
			"PATH=path_list_prepend:${CMAKE_BINARY_DIR}/lib/prebuilt/openal/bin;PATH=path_list_prepend:${CMAKE_BINARY_DIR}/lib/prebuilt/ffmpeg/bin;PATH=path_list_prepend:${CMAKE_BINARY_DIR}/lib/prebuilt/freetype/lib;PATH=path_list_prepend:${CMAKE_BINARY_DIR}/lib/prebuilt/sdl2/lib")
endif()

add_executable(telemetry_phase2_performance_harness
	telemetry/producer/telemetry_phase2_performance_harness.cpp
	test_stubs.cpp
)
target_compile_features(telemetry_phase2_performance_harness PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_performance_harness PRIVATE
	FSO_TELEMETRY_TEST_SEAMS=1)
target_include_directories(telemetry_phase2_performance_harness PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_performance_harness PRIVATE code)
set_target_properties(telemetry_phase2_performance_harness PROPERTIES
	FOLDER "tests/telemetry/phase2"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
add_test(NAME telemetry_phase2_performance_harness_smoke
	COMMAND telemetry_phase2_performance_harness
		--smoke
		--soak-report-dir "${CMAKE_BINARY_DIR}/Testing/Temporary/phase2-soak-smoke"
		--report "${CMAKE_BINARY_DIR}/Testing/Temporary/phase2-performance-smoke.json")
if(WIN32)
	set_tests_properties(telemetry_phase2_performance_harness_smoke PROPERTIES
		ENVIRONMENT_MODIFICATION
			"PATH=path_list_prepend:${CMAKE_BINARY_DIR}/lib/prebuilt/openal/bin;PATH=path_list_prepend:${CMAKE_BINARY_DIR}/lib/prebuilt/ffmpeg/bin;PATH=path_list_prepend:${CMAKE_BINARY_DIR}/lib/prebuilt/freetype/lib;PATH=path_list_prepend:${CMAKE_BINARY_DIR}/lib/prebuilt/sdl2/lib")
endif()

add_executable(telemetry_initialize_failure_contract_tests
	telemetry/producer/telemetry_initialize_failure_contract_tests.cpp
	${TELEMETRY_ISOLATED_RUNTIME_SOURCES}
	${CMAKE_SOURCE_DIR}/code/telemetry/telemetry.cpp
	${CMAKE_SOURCE_DIR}/code/events/events.cpp
)

target_compile_features(telemetry_initialize_failure_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_initialize_failure_contract_tests PRIVATE FSO_TELEMETRY_TEST_SEAMS=1)
target_include_directories(telemetry_initialize_failure_contract_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_initialize_failure_contract_tests PRIVATE sdl2)

set_target_properties(telemetry_initialize_failure_contract_tests PROPERTIES FOLDER "tests")

add_executable(telemetry_session_controller_allocation_contract_tests
	telemetry/producer/test_telemetry_session_controller_allocations.cpp
	test_stubs.cpp
)

target_compile_features(telemetry_session_controller_allocation_contract_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_session_controller_allocation_contract_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_session_controller_allocation_contract_tests PRIVATE gtest_main code)

set_target_properties(telemetry_session_controller_allocation_contract_tests PROPERTIES FOLDER "tests")

add_executable(telemetry_phase2_profile_tests
	telemetry/protocol/phase2_profile_tests.cpp
)

target_compile_features(telemetry_phase2_profile_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_profile_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_profile_tests PRIVATE gtest_main code)

set_target_properties(telemetry_phase2_profile_tests PROPERTIES FOLDER "tests")

add_test(NAME telemetry_phase2_profile_tests COMMAND telemetry_phase2_profile_tests)

add_executable(telemetry_phase2_session_transition_tests
	telemetry/protocol/phase2_session_transition_tests.cpp
)

target_compile_features(telemetry_phase2_session_transition_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_session_transition_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_session_transition_tests PRIVATE gtest_main code)

set_target_properties(telemetry_phase2_session_transition_tests PROPERTIES FOLDER "tests")

add_test(NAME telemetry_phase2_session_transition_tests COMMAND telemetry_phase2_session_transition_tests)

add_executable(telemetry_phase2_business_state_tests
	telemetry/protocol/test_telemetry_business_state_validation.cpp
)

target_compile_features(telemetry_phase2_business_state_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_business_state_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_business_state_tests PRIVATE gtest_main code)

set_target_properties(telemetry_phase2_business_state_tests PROPERTIES FOLDER "tests")

add_test(NAME telemetry_phase2_business_state_tests
	COMMAND telemetry_phase2_business_state_tests
		--gtest_filter=TelemetryProtocolBusinessStateValidation.CompleteShipK1HasExactlyFourPlusTenKPlusNRecords:TelemetryProtocolBusinessStateValidation.CompleteShipK1RejectsMandatoryRecordAndManifestOmissions:TelemetryProtocolBusinessStateValidation.CompleteShipWaitsForAtomicClassAndWeaponManifestTransaction:TelemetryProtocolBusinessStateValidation.CoreGateRejectsEveryMandatoryCoreRecordOmission:TelemetryProtocolBusinessStateValidation.Phase2RecordsCannotAppearWithoutTheirDomains:TelemetryProtocolBusinessStateValidation.CompleteShipRejectsSpecializedRecordsAndCapabilities:TelemetryProtocolBusinessStateValidation.CompleteShipAcceptsK2AndK64ButRejectsK65:TelemetryProtocolBusinessStateValidation.CompleteShipCoverageCannotChangeByDelta:TelemetryProtocolBusinessStateValidation.GenericCargoDomainDoesNotRequirePerShipDockingOrSupport
)

add_executable(telemetry_phase2_observation_contract_tests
	telemetry/producer/test_phase2_observation_contract.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
)

target_compile_features(telemetry_phase2_observation_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_observation_contract_tests PRIVATE FSO_TELEMETRY_TEST_SEAMS=1)
target_include_directories(telemetry_phase2_observation_contract_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_observation_contract_tests PRIVATE gtest_main)

set_target_properties(telemetry_phase2_observation_contract_tests PROPERTIES FOLDER "tests")

add_test(NAME telemetry_phase2_observation_contract_tests
	COMMAND telemetry_phase2_observation_contract_tests
)

add_executable(telemetry_phase2_engine_integration_source_contract_tests
	telemetry/producer/test_phase2_engine_integration_source_contract.cpp
)

target_compile_features(telemetry_phase2_engine_integration_source_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_engine_integration_source_contract_tests PRIVATE
	FSO_PHASE2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(telemetry_phase2_engine_integration_source_contract_tests PRIVATE gtest_main)

add_dependencies(telemetry_phase2_engine_integration_source_contract_tests code)

if(MSVC)
	add_library(telemetry_phase2_engine_adapter_evidence_object OBJECT
		${CMAKE_SOURCE_DIR}/code/telemetry/engine_adapter.cpp
	)
	target_compile_features(telemetry_phase2_engine_adapter_evidence_object PRIVATE
		cxx_std_17
		"$<TARGET_PROPERTY:code,COMPILE_FEATURES>"
	)
	target_include_directories(telemetry_phase2_engine_adapter_evidence_object PRIVATE
		"$<TARGET_PROPERTY:code,INCLUDE_DIRECTORIES>"
	)
	target_compile_definitions(telemetry_phase2_engine_adapter_evidence_object PRIVATE
		"$<TARGET_PROPERTY:code,COMPILE_DEFINITIONS>"
	)
	target_compile_options(telemetry_phase2_engine_adapter_evidence_object PRIVATE
		"$<TARGET_PROPERTY:code,COMPILE_OPTIONS>"
		/GL-
	)
	target_link_libraries(telemetry_phase2_engine_adapter_evidence_object PRIVATE
		"$<TARGET_PROPERTY:code,LINK_LIBRARIES>"
	)
	set_target_properties(telemetry_phase2_engine_adapter_evidence_object PROPERTIES
		INTERPROCEDURAL_OPTIMIZATION FALSE
		FOLDER "tests"
	)

	set(PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT
		"$<TARGET_OBJECTS:telemetry_phase2_engine_adapter_evidence_object>")
	target_compile_definitions(telemetry_phase2_engine_integration_source_contract_tests PRIVATE
		FSO_PHASE2_MSVC_LINK_EVIDENCE=1
		FSO_PHASE2_ENGINE_ADAPTER_OBJECT="${PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT}"
	)
	add_custom_target(telemetry_phase2_engine_adapter_evidence
		COMMAND "${CMAKE_COMMAND}"
			"-DINPUT_FILE=${PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT}"
			"-DOUTPUT_FILE=${PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT}.sha256"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/write_phase2_sha256_sidecar.cmake"
		COMMAND "${CMAKE_COMMAND}" -E rm -f
			"${PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT}.linkable.txt.tmp"
		COMMAND "${CMAKE_LINKER}" /DUMP /SYMBOLS /IMPORTS
			"/OUT:${PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT}.linkable.txt.tmp"
			"${PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT}"
		COMMAND "${CMAKE_COMMAND}" -E rename
			"${PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT}.linkable.txt.tmp"
			"${PHASE2_ENGINE_ADAPTER_EVIDENCE_OBJECT}.linkable.txt"
		VERBATIM
	)
	add_dependencies(telemetry_phase2_engine_adapter_evidence
		code
		telemetry_phase2_engine_adapter_evidence_object)
	add_dependencies(telemetry_phase2_engine_integration_source_contract_tests
		telemetry_phase2_engine_adapter_evidence)
else()
	target_compile_definitions(telemetry_phase2_engine_integration_source_contract_tests PRIVATE
		FSO_PHASE2_MSVC_LINK_EVIDENCE=0
	)
endif()

set_target_properties(telemetry_phase2_engine_integration_source_contract_tests PROPERTIES FOLDER "tests")

add_test(NAME telemetry_phase2_engine_integration_source_contract_tests
	COMMAND telemetry_phase2_engine_integration_source_contract_tests
)

add_executable(telemetry_phase2_closure_tests
	telemetry/producer/phase2_closure_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_closure.cpp
)
target_compile_features(telemetry_phase2_closure_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_closure_tests PRIVATE
	FSO_PHASE2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
target_include_directories(telemetry_phase2_closure_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_closure_tests PRIVATE gtest_main fstl_protocol)
set_target_properties(telemetry_phase2_closure_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\\\Producer" FILES
	telemetry/producer/phase2_closure_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	telemetry/phase2_wp03_allocation_tracker.h
	telemetry/phase2_wp03_test_support.h
)
add_test(NAME telemetry_phase2_closure_tests COMMAND telemetry_phase2_closure_tests)

add_executable(telemetry_phase2_manifest_tests
	telemetry/producer/phase2_manifest_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	telemetry/phase2_wp03_test_support.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/entity_id_registry.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_manifest_builder.cpp
)
target_compile_features(telemetry_phase2_manifest_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_manifest_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_manifest_tests PRIVATE gtest_main fstl_protocol)
set_target_properties(telemetry_phase2_manifest_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\\\Producer" FILES
	telemetry/producer/phase2_manifest_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	telemetry/phase2_wp03_allocation_tracker.h
	telemetry/phase2_wp03_test_support.cpp
	telemetry/phase2_wp03_test_support.h
)
add_test(NAME telemetry_phase2_manifest_tests COMMAND telemetry_phase2_manifest_tests)

add_executable(telemetry_phase2_manifest_codec_tests
	telemetry/protocol/phase2_manifest_codec_tests.cpp
	telemetry/phase2_wp03_test_support.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_manifest_builder.cpp
)
target_compile_features(telemetry_phase2_manifest_codec_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_manifest_codec_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_manifest_codec_tests PRIVATE gtest_main fstl_protocol)
set_target_properties(telemetry_phase2_manifest_codec_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\\\Protocol" FILES
	telemetry/protocol/phase2_manifest_codec_tests.cpp
	telemetry/phase2_wp03_test_support.cpp
	telemetry/phase2_wp03_test_support.h
)
add_test(NAME telemetry_phase2_manifest_codec_tests COMMAND telemetry_phase2_manifest_codec_tests)

add_executable(telemetry_phase2_core_state_tests
	telemetry/producer/phase2_state_image_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_manifest_builder.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_state_image.cpp
)
target_compile_features(telemetry_phase2_core_state_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_core_state_tests PRIVATE
	FSTL_PROTOCOL_TEST_ASSET_PATH="${FSTL_PROTOCOL_TEST_ASSET_PATH}"
)
target_include_directories(telemetry_phase2_core_state_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_core_state_tests PRIVATE gtest_main fstl_protocol)
set_target_properties(telemetry_phase2_core_state_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\\\Producer" FILES
	telemetry/producer/phase2_state_image_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	telemetry/phase2_wp03_allocation_tracker.h
)
add_test(NAME telemetry_phase2_core_state_tests COMMAND telemetry_phase2_core_state_tests)

add_executable(telemetry_phase2_control_turret_tests
	telemetry/producer/phase2_control_turret_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_manifest_builder.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_state_image.cpp
)
target_compile_features(telemetry_phase2_control_turret_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_control_turret_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_control_turret_tests PRIVATE gtest_main fstl_protocol)
set_target_properties(telemetry_phase2_control_turret_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\\\Producer" FILES
	telemetry/producer/phase2_control_turret_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	telemetry/phase2_wp03_allocation_tracker.h
)
add_test(NAME telemetry_phase2_control_turret_tests
	COMMAND telemetry_phase2_control_turret_tests)

add_executable(telemetry_phase2_weapon_state_tests
	telemetry/producer/phase2_weapon_state_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_manifest_builder.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_state_image.cpp
)
target_compile_features(telemetry_phase2_weapon_state_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_weapon_state_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_weapon_state_tests PRIVATE
	gtest_main
	fstl_protocol
)
set_target_properties(telemetry_phase2_weapon_state_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase2_weapon_state_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	telemetry/phase2_wp03_allocation_tracker.h
)
add_test(NAME telemetry_phase2_weapon_state_tests
	COMMAND telemetry_phase2_weapon_state_tests)

add_executable(telemetry_phase2_lifecycle_support_tests
	telemetry/producer/phase2_lifecycle_support_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_closure.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_manifest_builder.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_state_image.cpp
)
target_compile_features(telemetry_phase2_lifecycle_support_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_lifecycle_support_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_lifecycle_support_tests PRIVATE
	gtest_main
	fstl_protocol
)
set_target_properties(telemetry_phase2_lifecycle_support_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase2_lifecycle_support_tests.cpp
	telemetry/phase2_wp03_allocation_tracker.cpp
	telemetry/phase2_wp03_allocation_tracker.h
)
add_test(NAME telemetry_phase2_lifecycle_support_tests
	COMMAND telemetry_phase2_lifecycle_support_tests)

add_executable(telemetry_phase2_replication_tests
	telemetry/producer/phase2_replication_tests.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase1_snapshot_slot.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase1_state_image.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_runtime.cpp
)
target_compile_features(telemetry_phase2_replication_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase2_replication_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_replication_tests PRIVATE
	gtest_main
	fstl_protocol
)
set_target_properties(telemetry_phase2_replication_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase2_replication_tests.cpp
)
add_test(NAME telemetry_phase2_replication_tests
	COMMAND telemetry_phase2_replication_tests)

add_executable(telemetry_phase2_runtime_tests
	telemetry/producer/phase2_runtime_tests.cpp
	telemetry/producer/phase2_session_identity_test_support.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/entity_id_registry.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase1_snapshot_egress.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase1_snapshot_slot.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase1_state_image.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_manifest_builder.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_profile_gate.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_runtime.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_session_transition.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/session_controller.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/startup_budget.cpp
)
target_compile_features(telemetry_phase2_runtime_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_runtime_tests PRIVATE
	FSO_TELEMETRY_TEST_SEAMS=1
)
target_include_directories(telemetry_phase2_runtime_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_runtime_tests PRIVATE
	gtest_main
	fstl_protocol
)
set_target_properties(telemetry_phase2_runtime_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase2_runtime_tests.cpp
	telemetry/producer/phase2_session_identity_test_support.cpp
)
add_test(NAME telemetry_phase2_runtime_tests
	COMMAND telemetry_phase2_runtime_tests)

add_executable(telemetry_phase2_wp09_contract_tests
	telemetry/producer/phase2_wp09_contract_tests.cpp
	telemetry/producer/phase2_wp09_cfile_test_support.cpp
	telemetry/producer/phase2_session_identity_test_support.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/capture_scheduler.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/config.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/entity_id_registry.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/logging.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/metrics.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase1_snapshot_egress.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase1_snapshot_slot.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_observation.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_profile_gate.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_runtime.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase2_session_transition.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/session_controller.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/startup_budget.cpp
)
target_compile_features(telemetry_phase2_wp09_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_wp09_contract_tests PRIVATE
	FSO_PHASE2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
target_include_directories(telemetry_phase2_wp09_contract_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_wp09_contract_tests PRIVATE
	gtest_main
	fstl_protocol
	jansson
	sdl2
)
if(WIN32)
	target_link_libraries(telemetry_phase2_wp09_contract_tests PRIVATE ws2_32)
endif()
set_target_properties(telemetry_phase2_wp09_contract_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase2_wp09_contract_tests.cpp
	telemetry/producer/phase2_wp09_cfile_test_support.cpp
	telemetry/producer/phase2_session_identity_test_support.cpp
)
add_test(NAME telemetry_phase2_wp09_contract_tests
	COMMAND telemetry_phase2_wp09_contract_tests)

add_executable(telemetry_phase2_oracle_harness
	telemetry/producer/phase2_oracle_harness.cpp
)
target_compile_features(telemetry_phase2_oracle_harness PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_oracle_harness PRIVATE
	FSO_PHASE2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
target_include_directories(telemetry_phase2_oracle_harness PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_oracle_harness PRIVATE fstl_protocol)
set_target_properties(telemetry_phase2_oracle_harness PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase2_oracle_harness.cpp
)
add_test(NAME telemetry_phase2_oracle_harness_smoke
	COMMAND telemetry_phase2_oracle_harness
		--profiles core-gate complete-ship
		--output-dir "${CMAKE_CURRENT_BINARY_DIR}/phase2-oracle-smoke")

if(FSO_TELEMETRY_FUZZ_STANDALONE
	AND TARGET telemetry_phase2_packet_reader_fuzz
	AND TARGET telemetry_phase2_state_validator_fuzz)
	set(FSTL_PHASE2_REPLAY_CORPUS_DIR
		"${CMAKE_SOURCE_DIR}/test/telemetry/producer/phase2/fuzz/corpus")

	add_test(NAME telemetry_phase2_packet_reader_corpus_replay
		COMMAND telemetry_phase2_packet_reader_fuzz
			"${FSTL_PHASE2_REPLAY_CORPUS_DIR}/packet_reader")
	set_tests_properties(telemetry_phase2_packet_reader_corpus_replay
		PROPERTIES CONFIGURATIONS "Debug;Release")

	add_test(NAME telemetry_phase2_state_validator_corpus_replay
		COMMAND telemetry_phase2_state_validator_fuzz
			"${FSTL_PHASE2_REPLAY_CORPUS_DIR}/state_validator")
	set_tests_properties(telemetry_phase2_state_validator_corpus_replay
		PROPERTIES CONFIGURATIONS "Debug;Release")
endif()

# Contract-level Phase 2 build entry points. Keep the dependencies explicit so
# the portable Clang job and the MSVC matrix build the same bounded test surface.
add_custom_target(telemetry_phase2_unit_tests DEPENDS
	telemetry_phase2_profile_tests
	telemetry_phase2_session_transition_tests
	telemetry_phase2_business_state_tests
	telemetry_phase2_closure_tests
	telemetry_phase2_manifest_tests
	telemetry_phase2_core_state_tests
	telemetry_phase2_control_turret_tests
	telemetry_phase2_weapon_state_tests
	telemetry_phase2_lifecycle_support_tests
	telemetry_phase2_wp09_contract_tests
)
set_target_properties(telemetry_phase2_unit_tests PROPERTIES FOLDER "tests")

add_custom_target(telemetry_phase2_protocol_tests DEPENDS
	telemetry_phase2_manifest_codec_tests
	telemetry_phase2_replication_tests
)
set_target_properties(telemetry_phase2_protocol_tests PROPERTIES FOLDER "tests")

add_custom_target(telemetry_phase2_integration_tests DEPENDS
	telemetry_phase2_observation_contract_tests
	telemetry_phase2_engine_integration_source_contract_tests
	telemetry_phase2_runtime_tests
	telemetry_phase2_loss_harness
	telemetry_phase2_performance_harness
	telemetry_phase2_oracle_harness
)
set_target_properties(telemetry_phase2_integration_tests PROPERTIES FOLDER "tests")
