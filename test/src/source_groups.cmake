
set(source_files)

add_file_folder(""
    main.cpp
    test_stubs.cpp
)

add_file_folder("Actions"
)

add_file_folder("Actions\\\\Expression"
	actions/expression/test_ExpressionParser.cpp
)

add_file_folder("CFile"
    cfile/cfile.cpp
)

add_file_folder("Globalincs"
    globalincs/test_flagset.cpp
    globalincs/test_safe_strings.cpp
    globalincs/test_version.cpp
)

add_file_folder("Graphics"
	   graphics/test_font.cpp
)

if (FSO_BUILD_WITH_VULKAN)
	add_file_folder("Graphics\\\\Vulkan"
		graphics/vulkan/test_vulkan_raytracing.cpp
	)
endif()

add_file_folder("Math"
    math/test_vecmat.cpp
)

add_file_folder("menuui"
    menuui/test_intel_parse.cpp
)

add_file_folder("mod"
    mod/test_mod_table.cpp
)

add_file_folder("model"
    model/test_modelread.cpp
)

add_file_folder("Parse"
    parse/test_parselo.cpp
    parse/test_replace.cpp
)

add_file_folder("Pilotfile"
    pilotfile/plr.cpp
)

add_file_folder("Scripting"
    scripting/ade_args.cpp
    scripting/doc_parser.cpp
    scripting/require.cpp
    scripting/script_state.cpp
    scripting/ScriptingTestFixture.h
    scripting/ScriptingTestFixture.cpp
)

add_file_folder("Scripting\\\\API"
    scripting/api/async.cpp
    scripting/api/base.cpp
    scripting/api/bitops.cpp
    scripting/api/enums.cpp
    scripting/api/hookvars.cpp
)

add_file_folder("Scripting\\\\Lua"
    scripting/lua/Args.cpp
    scripting/lua/Convert.cpp
    scripting/lua/Function.cpp
    scripting/lua/Reference.cpp
    scripting/lua/Table.cpp
    scripting/lua/TestUtil.h
    scripting/lua/Thread.cpp
    scripting/lua/Util.cpp
    scripting/lua/Value.cpp
)

add_file_folder("Telemetry\\\\Protocol"
	telemetry/protocol/test_packet_io.cpp
	telemetry/protocol/test_protocol_constants.cpp
	telemetry/protocol/test_telemetry_capabilities.cpp
	telemetry/protocol/test_telemetry_business_records_1_10.cpp
	telemetry/protocol/test_telemetry_business_records_11_18.cpp
	telemetry/protocol/test_telemetry_business_records_19_24.cpp
	telemetry/protocol/test_telemetry_business_records_28.cpp
	telemetry/protocol/test_telemetry_business_state_validation.cpp
	telemetry/protocol/test_telemetry_clock.cpp
	telemetry/protocol/test_telemetry_comm_manifest_transaction.cpp
	telemetry/protocol/test_telemetry_control_messages.cpp
	telemetry/protocol/test_telemetry_counters.cpp
	telemetry/protocol/test_telemetry_datagram.cpp
	telemetry/protocol/test_telemetry_event_messages.cpp
	telemetry/protocol/test_telemetry_fragmentation.cpp
	telemetry/protocol/test_telemetry_rate_limiter.cpp
	telemetry/protocol/test_telemetry_records.cpp
	telemetry/protocol/test_telemetry_replication.cpp
	telemetry/protocol/test_telemetry_replication_harness.cpp
	telemetry/protocol/test_telemetry_reliability_harness.cpp
	telemetry/protocol/test_telemetry_reliability_messages.cpp
	telemetry/protocol/test_telemetry_reliable_receive.cpp
	telemetry/protocol/test_telemetry_reliable_window.cpp
	telemetry/protocol/test_telemetry_preallocated_reliable_parity.cpp
	telemetry/protocol/test_telemetry_session.cpp
	telemetry/protocol/test_telemetry_session_context.cpp
	telemetry/protocol/test_telemetry_security.cpp
	telemetry/protocol/test_telemetry_sha256.cpp
	telemetry/protocol/test_telemetry_specialized_lifecycle.cpp
	telemetry/protocol/test_telemetry_specialized_views.cpp
	telemetry/protocol/test_telemetry_state_messages.cpp
	telemetry/protocol/test_telemetry_transaction.cpp
	telemetry/protocol/test_telemetry_vectors.cpp
)

add_file_folder("Telemetry\\\\Producer"
	telemetry/producer/telemetry_native_session_runtime_player_test_access.h
	telemetry/producer/telemetry_runtime_adapter_player_test_access.h
	telemetry/producer/telemetry_session_controller_player_test_access.h
	telemetry/producer/phase2_gameplay_ab_test_support.h
	telemetry/producer/test_telemetry_capture_scheduler_contract.cpp
	telemetry/producer/test_telemetry_config_contract.cpp
	telemetry/producer/test_telemetry_datagram_scheduler_contract.cpp
	telemetry/producer/test_telemetry_engine_adapter_contract.cpp
	telemetry/producer/test_telemetry_entity_id_registry_contract.cpp
	telemetry/producer/test_telemetry_identity_contract.cpp
	telemetry/producer/test_telemetry_identity_native_contract.cpp
	telemetry/producer/test_telemetry_initialize.cpp
	telemetry/producer/test_telemetry_logging_contract.cpp
	telemetry/producer/test_telemetry_metrics_contract.cpp
	telemetry/producer/test_telemetry_native_runtime_integration_contract.cpp
	telemetry/producer/test_support_work_contract.cpp
	telemetry/producer/test_telemetry_runtime_adapter_player_contract.cpp
	telemetry/producer/test_telemetry_player_observation_slot_contract.cpp
	telemetry/producer/test_telemetry_phase1_snapshot_slot_contract.cpp
	telemetry/producer/test_telemetry_phase1_snapshot_egress_contract.cpp
	telemetry/producer/test_telemetry_runtime_lifecycle_contract.cpp
	telemetry/producer/test_telemetry_runtime_startup_contract.cpp
	telemetry/producer/test_telemetry_session_controller_contract.cpp
	telemetry/producer/test_telemetry_session_controller_heartbeat_contract.cpp
	telemetry/producer/test_telemetry_startup_budget_contract.cpp
	telemetry/producer/test_telemetry_transport_contract.cpp
	telemetry/producer/test_telemetry_wp04_startup_budget_contract.cpp
)

add_file_folder("Test Util"
    util/FSTestFixture.cpp
    util/FSTestFixture.h
    util/test_util.h
)

add_file_folder("Utils"
    utils/HeapAllocatorTest.cpp
)

add_file_folder("Weapon"
    weapon/weapons.cpp
)
