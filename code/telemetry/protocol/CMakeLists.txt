set(FSTL_PROTOCOL_SOURCES
	packet_reader.cpp
	packet_writer.cpp
	telemetry_rate_limiter.cpp
	telemetry_capabilities.cpp
	telemetry_clock.cpp
	telemetry_control_messages.cpp
	telemetry_counters.cpp
	telemetry_datagram.cpp
	telemetry_crc32.cpp
	telemetry_fragmenter.cpp
	telemetry_reassembler.cpp
	telemetry_records.cpp
	telemetry_replication.cpp
	telemetry_reliability_messages.cpp
	telemetry_reliable_receive.cpp
	telemetry_reliable_window.cpp
	telemetry_session.cpp
	telemetry_session_context.cpp
	telemetry_sha256.cpp
	telemetry_state_messages.cpp
	telemetry_transaction.cpp
)

set(FSTL_PROTOCOL_HEADERS
	packet_reader.h
	packet_writer.h
	telemetry_rate_limiter.h
	telemetry_capabilities.h
	telemetry_clock.h
	telemetry_control_messages.h
	telemetry_counters.h
	telemetry_datagram.h
	telemetry_crc32.h
	telemetry_fragmenter.h
	telemetry_protocol_constants.h
	telemetry_protocol_types.h
	telemetry_reassembler.h
	telemetry_records.h
	telemetry_replication.h
	telemetry_reliability_messages.h
	telemetry_reliable_receive.h
	telemetry_reliable_window.h
	telemetry_session.h
	telemetry_session_context.h
	telemetry_sha256.h
	telemetry_state_messages.h
	telemetry_transaction.h
)

add_library(fstl_protocol STATIC ${FSTL_PROTOCOL_SOURCES} ${FSTL_PROTOCOL_HEADERS})
target_compile_features(fstl_protocol PUBLIC cxx_std_17)
target_include_directories(fstl_protocol PUBLIC "${CMAKE_SOURCE_DIR}/code")
set_target_properties(fstl_protocol PROPERTIES FOLDER "code/telemetry")

source_group("Protocol" FILES ${FSTL_PROTOCOL_SOURCES} ${FSTL_PROTOCOL_HEADERS})

enable_clang_tidy(fstl_protocol)
