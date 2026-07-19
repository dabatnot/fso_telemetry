if(NOT TARGET fstl_protocol)
	message(FATAL_ERROR "Telemetry fuzzers require the fstl_protocol target")
endif()

set(FSTL_FUZZ_TARGETS
	fuzz_packet_reader
	fuzz_datagram
	fuzz_reassembler
	fuzz_control_payloads
	fuzz_records
	fuzz_transactions
	fuzz_comm_views
	fuzz_video_payloads
)

set(FSTL_FUZZ_SANITIZER_COMPILE_OPTIONS)
set(FSTL_FUZZ_SANITIZER_LINK_OPTIONS)
set(FSTL_FUZZ_DRIVER_COMPILE_OPTIONS)
set(FSTL_FUZZ_DRIVER_LINK_OPTIONS)

if(MSVC)
	if(NOT FSO_TELEMETRY_FUZZ_STANDALONE)
		message(FATAL_ERROR
			"MSVC telemetry fuzz builds are corpus-replay sanitizer jobs; set "
			"FSO_TELEMETRY_FUZZ_STANDALONE=ON")
	endif()
	if(FSO_TELEMETRY_FUZZ_SANITIZERS)
		set(FSTL_FUZZ_SANITIZER_COMPILE_OPTIONS /fsanitize=address /Zi /GL-)
		set(FSTL_FUZZ_SANITIZER_LINK_OPTIONS /INCREMENTAL:NO)
	else()
		message(WARNING
			"Telemetry fuzz sanitizer instrumentation is disabled. Corpus replay remains useful "
			"as a deterministic smoke test, but is not sanitizer evidence for the Phase 0 gate.")
	endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
	if(FSO_TELEMETRY_FUZZ_SANITIZERS)
		set(FSTL_FUZZ_SANITIZER_COMPILE_OPTIONS
			-fsanitize=address,undefined
			-fno-sanitize-recover=all
			-fno-omit-frame-pointer)
		set(FSTL_FUZZ_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
	endif()
	if(NOT FSO_TELEMETRY_FUZZ_STANDALONE)
		set(FSTL_FUZZ_DRIVER_COMPILE_OPTIONS -fsanitize=fuzzer-no-link)
		set(FSTL_FUZZ_DRIVER_LINK_OPTIONS -fsanitize=fuzzer)
	endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	if(NOT FSO_TELEMETRY_FUZZ_STANDALONE)
		message(FATAL_ERROR
			"GCC has no bundled libFuzzer driver; use Clang or set "
			"FSO_TELEMETRY_FUZZ_STANDALONE=ON")
	endif()
	if(FSO_TELEMETRY_FUZZ_SANITIZERS)
		set(FSTL_FUZZ_SANITIZER_COMPILE_OPTIONS
			-fsanitize=address,undefined
			-fno-sanitize-recover=all
			-fno-omit-frame-pointer)
		set(FSTL_FUZZ_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
	endif()
else()
	message(FATAL_ERROR "Telemetry fuzzers require Clang, AppleClang, GCC replay mode, or MSVC replay mode")
endif()

# Compile a fuzz-only copy of the engine-independent protocol library. This
# instruments the parsers themselves without changing fstl_protocol or any
# ordinary FS2Open target in a fuzz-enabled build tree.
get_target_property(FSTL_PROTOCOL_SOURCE_DIR fstl_protocol SOURCE_DIR)
get_target_property(FSTL_PROTOCOL_TARGET_SOURCES fstl_protocol SOURCES)
set(FSTL_PROTOCOL_FUZZ_SOURCES)
foreach(source IN LISTS FSTL_PROTOCOL_TARGET_SOURCES)
	if(IS_ABSOLUTE "${source}")
		list(APPEND FSTL_PROTOCOL_FUZZ_SOURCES "${source}")
	else()
		list(APPEND FSTL_PROTOCOL_FUZZ_SOURCES "${FSTL_PROTOCOL_SOURCE_DIR}/${source}")
	endif()
endforeach()

add_library(fstl_protocol_fuzz STATIC ${FSTL_PROTOCOL_FUZZ_SOURCES})
target_compile_features(fstl_protocol_fuzz PUBLIC cxx_std_17)
target_include_directories(fstl_protocol_fuzz PUBLIC "${CMAKE_SOURCE_DIR}/code")
target_compile_options(fstl_protocol_fuzz PRIVATE ${FSTL_FUZZ_SANITIZER_COMPILE_OPTIONS})
set_target_properties(fstl_protocol_fuzz PROPERTIES
	FOLDER "tests/telemetry/fuzz"
	INTERPROCEDURAL_OPTIMIZATION OFF)

foreach(target IN LISTS FSTL_FUZZ_TARGETS)
	set(target_sources "${target}.cpp")
	if(FSO_TELEMETRY_FUZZ_STANDALONE)
		list(APPEND target_sources fuzz_standalone_main.cpp)
	endif()

	add_executable(${target} ${target_sources})
	target_compile_features(${target} PRIVATE cxx_std_17)
	target_link_libraries(${target} PRIVATE fstl_protocol_fuzz)
	target_compile_options(${target} PRIVATE
		${FSTL_FUZZ_SANITIZER_COMPILE_OPTIONS}
		${FSTL_FUZZ_DRIVER_COMPILE_OPTIONS})
	target_link_options(${target} PRIVATE
		${FSTL_FUZZ_SANITIZER_LINK_OPTIONS}
		${FSTL_FUZZ_DRIVER_LINK_OPTIONS})
	set_target_properties(${target} PROPERTIES
		FOLDER "tests/telemetry/fuzz"
		INTERPROCEDURAL_OPTIMIZATION OFF)
endforeach()

add_custom_target(telemetry_fuzzers DEPENDS ${FSTL_FUZZ_TARGETS})
set_target_properties(telemetry_fuzzers PROPERTIES FOLDER "tests/telemetry/fuzz")

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(FSTL_FUZZ_CORPUS_DIR "${CMAKE_BINARY_DIR}/telemetry-fuzz-corpus")

add_custom_target(telemetry_fuzz_corpus
	COMMAND "${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/prepare_fuzz_corpus.py"
		--vectors "${CMAKE_SOURCE_DIR}/test/telemetry/protocol/vectors"
		--vectors "${CMAKE_SOURCE_DIR}/test/telemetry/protocol/vectors-v1.1"
		--output "${FSTL_FUZZ_CORPUS_DIR}"
	BYPRODUCTS "${FSTL_FUZZ_CORPUS_DIR}/manifest.json"
	COMMENT "Seeding telemetry fuzz corpora from FSTL golden vectors"
	VERBATIM)
set_target_properties(telemetry_fuzz_corpus PROPERTIES FOLDER "tests/telemetry/fuzz")

set(FSTL_FUZZ_SMOKE_ARGS)
if(FSO_TELEMETRY_FUZZ_STANDALONE)
	list(APPEND FSTL_FUZZ_SMOKE_ARGS --standalone)
endif()

add_custom_target(telemetry_fuzz_smoke
	COMMAND "${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/run_fuzz_smoke.py"
		--binary-dir "$<TARGET_FILE_DIR:fuzz_packet_reader>"
		--corpus "${FSTL_FUZZ_CORPUS_DIR}"
		--artifacts "${CMAKE_BINARY_DIR}/telemetry-fuzz-artifacts"
		--runs 2000
		--max-len 2097220
		--dictionary "${CMAKE_CURRENT_SOURCE_DIR}/fstl.dict"
		${FSTL_FUZZ_SMOKE_ARGS}
	DEPENDS telemetry_fuzzers telemetry_fuzz_corpus
	USES_TERMINAL
	VERBATIM)
set_target_properties(telemetry_fuzz_smoke PROPERTIES FOLDER "tests/telemetry/fuzz")
