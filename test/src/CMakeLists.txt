
include(source_groups.cmake)

add_executable(unittests ${source_files})

target_compile_features(unittests PUBLIC cxx_std_17)

target_link_libraries(unittests PRIVATE gtest)
target_link_libraries(unittests PRIVATE code)

target_include_directories(unittests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(unittests PROPERTIES FOLDER "tests")
if(MSVC)
	# The Phase 3 integration case nests the complete engine, session and
	# projection call stack. Product projections remain heap-backed; reserve
	# virtual stack for the monolithic Windows test runner itself.
  target_link_options(unittests PRIVATE /STACK:8388608)
endif()

file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../test_data" TEST_DATA_PATH)
string(REPLACE "\\" "\\\\" TEST_DATA_PATH "${TEST_DATA_PATH}")
target_compile_definitions(unittests PRIVATE "TEST_DATA_PATH=\"${TEST_DATA_PATH}\"")

file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/test/telemetry/protocol" FSTL_PROTOCOL_TEST_ASSET_PATH)
string(REPLACE "\\" "\\\\" FSTL_PROTOCOL_TEST_ASSET_PATH "${FSTL_PROTOCOL_TEST_ASSET_PATH}")
target_compile_definitions(unittests PRIVATE "FSTL_PROTOCOL_TEST_ASSET_PATH=\"${FSTL_PROTOCOL_TEST_ASSET_PATH}\"")
target_compile_definitions(unittests PRIVATE
	FSO_PHASE2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
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
target_link_libraries(telemetry_initialize_contract_tests PRIVATE gtest_main sdl3)

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
target_link_libraries(telemetry_lifecycle_callback_contract_tests PRIVATE gtest_main sdl3)

set_target_properties(telemetry_lifecycle_callback_contract_tests PROPERTIES FOLDER "tests")

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
target_link_libraries(telemetry_initialize_failure_contract_tests PRIVATE sdl3)

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

add_executable(telemetry_native_runtime_loopback_contract_tests
	telemetry/producer/test_telemetry_native_runtime_loopback_contract.cpp
	test_stubs.cpp
)

target_compile_features(telemetry_native_runtime_loopback_contract_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_native_runtime_loopback_contract_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_native_runtime_loopback_contract_tests PRIVATE
	gtest_main
	code
)
if(WIN32)
	target_link_libraries(telemetry_native_runtime_loopback_contract_tests PRIVATE ws2_32)
endif()
set_target_properties(telemetry_native_runtime_loopback_contract_tests
	PROPERTIES FOLDER "tests")
if(MSVC)
	# This in-process test nests a complete server, client and engine callback
	# stack. Production trackers remain heap-backed; reserve virtual stack for
	# the Windows-only harness nesting itself.
	target_link_options(telemetry_native_runtime_loopback_contract_tests
		PRIVATE /STACK:4194304)
endif()
add_test(NAME telemetry_native_runtime_loopback_contract_tests
	COMMAND telemetry_native_runtime_loopback_contract_tests)

add_executable(telemetry_phase2_profile_tests
	telemetry/protocol/phase2_profile_tests.cpp
	test_stubs.cpp
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
	test_stubs.cpp
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
	test_stubs.cpp
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

add_executable(telemetry_phase2_closure_tests
	telemetry/producer/phase2_closure_tests.cpp
	telemetry/phase2_allocation_tracker.cpp
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
	telemetry/phase2_allocation_tracker.cpp
	telemetry/phase2_allocation_tracker.h
	telemetry/phase2_manifest_test_support.h
)
add_test(NAME telemetry_phase2_closure_tests COMMAND telemetry_phase2_closure_tests)

add_executable(telemetry_phase2_manifest_tests
	telemetry/producer/phase2_manifest_tests.cpp
	telemetry/phase2_allocation_tracker.cpp
	telemetry/phase2_manifest_test_support.cpp
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
	telemetry/phase2_allocation_tracker.cpp
	telemetry/phase2_allocation_tracker.h
	telemetry/phase2_manifest_test_support.cpp
	telemetry/phase2_manifest_test_support.h
)
add_test(NAME telemetry_phase2_manifest_tests COMMAND telemetry_phase2_manifest_tests)

add_executable(telemetry_phase2_manifest_codec_tests
	telemetry/protocol/phase2_manifest_codec_tests.cpp
	telemetry/phase2_manifest_test_support.cpp
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
	telemetry/phase2_manifest_test_support.cpp
	telemetry/phase2_manifest_test_support.h
)
add_test(NAME telemetry_phase2_manifest_codec_tests COMMAND telemetry_phase2_manifest_codec_tests)

add_executable(telemetry_phase2_core_state_tests
	telemetry/producer/phase2_state_image_tests.cpp
	telemetry/phase2_allocation_tracker.cpp
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
	telemetry/phase2_allocation_tracker.cpp
	telemetry/phase2_allocation_tracker.h
)
add_test(NAME telemetry_phase2_core_state_tests COMMAND telemetry_phase2_core_state_tests)

add_executable(telemetry_phase2_control_turret_tests
	telemetry/producer/phase2_control_turret_tests.cpp
	telemetry/phase2_allocation_tracker.cpp
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
	telemetry/phase2_allocation_tracker.cpp
	telemetry/phase2_allocation_tracker.h
)
add_test(NAME telemetry_phase2_control_turret_tests
	COMMAND telemetry_phase2_control_turret_tests)

add_executable(telemetry_phase2_weapon_state_tests
	telemetry/producer/phase2_weapon_state_tests.cpp
	telemetry/phase2_allocation_tracker.cpp
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
	telemetry/phase2_allocation_tracker.cpp
	telemetry/phase2_allocation_tracker.h
)
add_test(NAME telemetry_phase2_weapon_state_tests
	COMMAND telemetry_phase2_weapon_state_tests)

add_executable(telemetry_phase2_lifecycle_support_tests
	telemetry/producer/phase2_lifecycle_support_tests.cpp
	telemetry/phase2_allocation_tracker.cpp
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
	telemetry/phase2_allocation_tracker.cpp
	telemetry/phase2_allocation_tracker.h
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
	${CMAKE_SOURCE_DIR}/code/telemetry/logging.cpp
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

add_executable(telemetry_phase2_security_bounds_tests
	telemetry/producer/phase2_security_bounds_tests.cpp
	telemetry/producer/phase2_security_bounds_cfile_test_support.cpp
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
target_compile_features(telemetry_phase2_security_bounds_tests PRIVATE cxx_std_17)
target_compile_definitions(telemetry_phase2_security_bounds_tests PRIVATE
	FSO_PHASE2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
target_include_directories(telemetry_phase2_security_bounds_tests PRIVATE
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase2_security_bounds_tests PRIVATE
	gtest_main
	fstl_protocol
	jansson
	sdl3
)
if(WIN32)
	target_link_libraries(telemetry_phase2_security_bounds_tests PRIVATE ws2_32)
endif()
set_target_properties(telemetry_phase2_security_bounds_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase2_security_bounds_tests.cpp
	telemetry/producer/phase2_security_bounds_cfile_test_support.cpp
	telemetry/producer/phase2_session_identity_test_support.cpp
)
add_test(NAME telemetry_phase2_security_bounds_tests
	COMMAND telemetry_phase2_security_bounds_tests)

add_executable(telemetry_phase3_identity_tests
	telemetry/producer/phase3_identity_tests.cpp
	telemetry/phase2_allocation_tracker.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase3_identity_registry.cpp
)
target_compile_features(telemetry_phase3_identity_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase3_identity_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase3_identity_tests PRIVATE gtest_main)
set_target_properties(telemetry_phase3_identity_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase3_identity_tests.cpp
	telemetry/phase2_allocation_tracker.cpp
	telemetry/phase2_allocation_tracker.h
)
add_test(NAME telemetry_phase3_identity_tests COMMAND telemetry_phase3_identity_tests)

add_executable(telemetry_phase4_entity_registry_tests
	telemetry/producer/phase4_entity_registry_tests.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase4_entity_registry.cpp
)
target_compile_features(telemetry_phase4_entity_registry_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase4_entity_registry_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CODE_HEADERS}
	${FREESPACE_HEADERS}
	${GENERATED_SOURCE_DIR}
	${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(telemetry_phase4_entity_registry_tests PRIVATE gtest_main)
set_target_properties(telemetry_phase4_entity_registry_tests PROPERTIES FOLDER "tests")
source_group("Telemetry\\Producer" FILES
	telemetry/producer/phase4_entity_registry_tests.cpp
)
add_test(NAME telemetry_phase4_entity_registry_tests COMMAND telemetry_phase4_entity_registry_tests)

add_executable(telemetry_phase4_entity_projection_tests
	telemetry/producer/phase4_entity_projection_tests.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase4_entity_projection.cpp
)
target_compile_features(telemetry_phase4_entity_projection_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase4_entity_projection_tests PRIVATE
	${CMAKE_SOURCE_DIR}/code
	${CMAKE_SOURCE_DIR}/test/src
)
target_link_libraries(telemetry_phase4_entity_projection_tests PRIVATE
	gtest_main fstl_protocol)
set_target_properties(telemetry_phase4_entity_projection_tests PROPERTIES FOLDER "tests")
add_test(NAME telemetry_phase4_entity_projection_tests
	COMMAND telemetry_phase4_entity_projection_tests)

add_executable(telemetry_phase3_state_image_tests
	telemetry/producer/phase3_state_image_tests.cpp
	${CMAKE_SOURCE_DIR}/code/telemetry/phase3_state_image.cpp)
target_compile_features(telemetry_phase3_state_image_tests PRIVATE cxx_std_17)
target_include_directories(telemetry_phase3_state_image_tests PRIVATE
	${CMAKE_SOURCE_DIR}/code
	${CMAKE_SOURCE_DIR}/test/src)
target_link_libraries(telemetry_phase3_state_image_tests PRIVATE
	gtest_main fstl_protocol)
set_target_properties(telemetry_phase3_state_image_tests PROPERTIES FOLDER "tests")
add_test(NAME telemetry_phase3_state_image_tests
	COMMAND telemetry_phase3_state_image_tests)

if(FSO_TELEMETRY_FUZZ_STANDALONE
	AND TARGET telemetry_phase2_packet_reader_fuzz
	AND TARGET telemetry_phase2_state_validator_fuzz)
	set(FSTL_PHASE2_REPLAY_CORPUS_SOURCE_DIR
		"${CMAKE_SOURCE_DIR}/test/telemetry/producer/phase2/fuzz/corpus")
	set(FSTL_PHASE2_REPLAY_CORPUS_DIR
		"${CMAKE_CURRENT_BINARY_DIR}/telemetry-phase2-replay-corpus")

	foreach(FSTL_PHASE2_REPLAY_CORPUS_NAME packet_reader state_validator)
		set(FSTL_PHASE2_REPLAY_SOURCE_DIR
			"${FSTL_PHASE2_REPLAY_CORPUS_SOURCE_DIR}/${FSTL_PHASE2_REPLAY_CORPUS_NAME}")
		set(FSTL_PHASE2_REPLAY_OUTPUT_DIR
			"${FSTL_PHASE2_REPLAY_CORPUS_DIR}/${FSTL_PHASE2_REPLAY_CORPUS_NAME}")
		file(MAKE_DIRECTORY "${FSTL_PHASE2_REPLAY_OUTPUT_DIR}")
		file(GLOB FSTL_PHASE2_REPLAY_SEEDS CONFIGURE_DEPENDS
			"${FSTL_PHASE2_REPLAY_SOURCE_DIR}/*.seed")
		foreach(FSTL_PHASE2_REPLAY_SEED IN LISTS FSTL_PHASE2_REPLAY_SEEDS)
			get_filename_component(FSTL_PHASE2_REPLAY_SEED_NAME
				"${FSTL_PHASE2_REPLAY_SEED}" NAME)
			configure_file(
				"${FSTL_PHASE2_REPLAY_SEED}"
				"${FSTL_PHASE2_REPLAY_OUTPUT_DIR}/${FSTL_PHASE2_REPLAY_SEED_NAME}"
				COPYONLY)
		endforeach()
	endforeach()

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
	telemetry_phase2_security_bounds_tests
)
set_target_properties(telemetry_phase2_unit_tests PROPERTIES FOLDER "tests")

add_custom_target(telemetry_phase2_protocol_tests DEPENDS
	telemetry_phase2_manifest_codec_tests
	telemetry_phase2_replication_tests
)
set_target_properties(telemetry_phase2_protocol_tests PROPERTIES FOLDER "tests")

add_custom_target(telemetry_phase2_integration_tests DEPENDS
	telemetry_phase2_observation_contract_tests
	telemetry_phase2_runtime_tests
)
set_target_properties(telemetry_phase2_integration_tests PROPERTIES FOLDER "tests")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME telemetry_no_legacy_certification
	COMMAND ${Python3_EXECUTABLE}
		"${CMAKE_SOURCE_DIR}/test/telemetry/tools/check_no_legacy_certification.py"
		"${CMAKE_SOURCE_DIR}")

set(TELEMETRY_SHORT_TESTS
	telemetry_native_runtime_loopback_contract_tests
	telemetry_phase2_profile_tests
	telemetry_phase2_session_transition_tests
	telemetry_phase2_business_state_tests
	telemetry_phase2_observation_contract_tests
	telemetry_phase2_closure_tests
	telemetry_phase2_manifest_tests
	telemetry_phase2_manifest_codec_tests
	telemetry_phase2_core_state_tests
	telemetry_phase2_control_turret_tests
	telemetry_phase2_weapon_state_tests
	telemetry_phase2_lifecycle_support_tests
	telemetry_phase2_replication_tests
	telemetry_phase2_runtime_tests
	telemetry_phase2_security_bounds_tests
	telemetry_phase3_identity_tests
	telemetry_phase3_state_image_tests
	telemetry_no_legacy_certification)
set_tests_properties(${TELEMETRY_SHORT_TESTS} PROPERTIES
	LABELS "telemetry-short"
	TIMEOUT 60)
