set(FSTL_PROTOCOL_SOURCES
	packet_reader.cpp
	packet_writer.cpp
	telemetry_comm_manifest_transaction.cpp
	telemetry_business_records.cpp
	telemetry_business_records_1_10.cpp
	telemetry_business_records_11_18.cpp
	telemetry_business_records_19_24.cpp
	telemetry_business_records_28.cpp
	telemetry_business_records_29.cpp
	telemetry_business_state_validation.cpp
	telemetry_rate_limiter.cpp
	telemetry_capabilities.cpp
	telemetry_clock.cpp
	telemetry_control_messages.cpp
	telemetry_counters.cpp
	telemetry_datagram.cpp
	telemetry_event_messages.cpp
	telemetry_crc32.cpp
	telemetry_fragmenter.cpp
	telemetry_manifest_metadata.cpp
	telemetry_reassembler.cpp
	telemetry_records.cpp
	telemetry_replication.cpp
	telemetry_reliability_messages.cpp
	telemetry_reliable_receive.cpp
	telemetry_reliable_window.cpp
	telemetry_session.cpp
	telemetry_session_context.cpp
	telemetry_security.cpp
	telemetry_sha256.cpp
	telemetry_specialized_lifecycle.cpp
	telemetry_specialized_views.cpp
	telemetry_state_messages.cpp
	telemetry_transaction.cpp
)

set(FSTL_PROTOCOL_HEADERS
	packet_reader.h
	packet_writer.h
	telemetry_comm_manifest_transaction.h
	telemetry_business_records.h
	telemetry_business_records_internal.h
	telemetry_business_state_validation.h
	telemetry_rate_limiter.h
	telemetry_capabilities.h
	telemetry_clock.h
	telemetry_control_messages.h
	telemetry_counters.h
	telemetry_datagram.h
	telemetry_event_messages.h
	telemetry_crc32.h
	telemetry_fragmenter.h
	telemetry_manifest_metadata.h
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
	telemetry_security.h
	telemetry_sha256.h
	telemetry_specialized_lifecycle.h
	telemetry_specialized_views.h
	telemetry_state_messages.h
	telemetry_transaction.h
)

add_library(fstl_protocol STATIC ${FSTL_PROTOCOL_SOURCES} ${FSTL_PROTOCOL_HEADERS})
target_compile_features(fstl_protocol PUBLIC cxx_std_17)
get_filename_component(FSTL_CODE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_include_directories(fstl_protocol PUBLIC "${FSTL_CODE_ROOT}")
set_target_properties(fstl_protocol PROPERTIES FOLDER "code/telemetry")

source_group("Protocol" FILES ${FSTL_PROTOCOL_SOURCES} ${FSTL_PROTOCOL_HEADERS})

if(COMMAND enable_clang_tidy)
	enable_clang_tidy(fstl_protocol)
endif()
