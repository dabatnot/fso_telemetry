add_library(comm_bundle_core STATIC
    comm_bundle_core.cpp
    comm_bundle_core.h
)

target_compile_features(comm_bundle_core PUBLIC cxx_std_17)
target_include_directories(comm_bundle_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CODE_HEADERS}
    ${GENERATED_SOURCE_DIR}
    ${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(comm_bundle_core PUBLIC fstl_protocol ${ZLIB_LIBS})

add_executable(comm_bundle_packager EXCLUDE_FROM_ALL
    comm_bundle_packager.cpp
    # The engine static library expects lifecycle symbols supplied by the game
    # executable. These no-op shims are also used by the native test harness and
    # keep this command-line tool independent from the game loop.
    ${CMAKE_SOURCE_DIR}/test/src/test_stubs.cpp
)

target_compile_features(comm_bundle_packager PUBLIC cxx_std_17)
target_include_directories(comm_bundle_packager PRIVATE
    ${CODE_HEADERS}
    ${GENERATED_SOURCE_DIR}
    ${GENERATED_SOURCE_DIR}/code
)
target_link_libraries(comm_bundle_packager PRIVATE comm_bundle_core code)

set_target_properties(comm_bundle_core comm_bundle_packager PROPERTIES FOLDER "FSOTools")

if (MSVC)
    target_compile_definitions(comm_bundle_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
