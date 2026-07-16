
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
