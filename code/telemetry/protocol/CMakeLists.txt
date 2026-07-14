set(FSTL_PROTOCOL_SOURCES
	packet_reader.cpp
	packet_writer.cpp
	telemetry_crc32.cpp
)

set(FSTL_PROTOCOL_HEADERS
	packet_reader.h
	packet_writer.h
	telemetry_crc32.h
	telemetry_protocol_constants.h
	telemetry_protocol_types.h
)

add_library(fstl_protocol STATIC ${FSTL_PROTOCOL_SOURCES} ${FSTL_PROTOCOL_HEADERS})
target_compile_features(fstl_protocol PUBLIC cxx_std_17)
target_include_directories(fstl_protocol PUBLIC "${CMAKE_SOURCE_DIR}/code")
set_target_properties(fstl_protocol PROPERTIES FOLDER "code/telemetry")

source_group("Protocol" FILES ${FSTL_PROTOCOL_SOURCES} ${FSTL_PROTOCOL_HEADERS})

enable_clang_tidy(fstl_protocol)
