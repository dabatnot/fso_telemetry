cmake_minimum_required(VERSION 3.22)

project(AvDs VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network Svg Test)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_subdirectory("${REPOSITORY_ROOT}/code/telemetry/protocol"
                 "${CMAKE_CURRENT_BINARY_DIR}/fstl_protocol")

add_library(radar_core STATIC
    src/application_settings.h
    src/application_settings.cpp
    src/display_unit.h
    src/display_unit.cpp
    src/radar_display_settings.h
    src/radar_image.h
    src/radar_image.cpp
    src/radar_icons.h
    src/radar_icons.cpp
    src/radar_manifest.h
    src/radar_manifest.cpp
    src/svg_icon_cache.h
    src/svg_icon_cache.cpp
    src/vfnt_font.h
    src/vfnt_font.cpp
    src/radar_client.h
    src/radar_client.cpp
    src/radar_widget.h
    src/radar_widget.cpp
    radar_icons.qrc
    radar_fonts.qrc
)
target_include_directories(radar_core PUBLIC src)
target_link_libraries(radar_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg fstl_protocol)
target_compile_definitions(radar_core PUBLIC QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)

add_executable(av-ds WIN32
    src/main.cpp
    src/main_window.h
    src/main_window.cpp
    src/settings_dialog.h
    src/settings_dialog.cpp
)
target_link_libraries(av-ds PRIVATE radar_core)
set_target_properties(av-ds PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
if(WIN32)
    set_target_properties(av-ds PROPERTIES OUTPUT_NAME "av-ds")
else()
    set_target_properties(av-ds PROPERTIES OUTPUT_NAME "FsoSimpitRadar")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(av_ds_shell_tests
        tests/av_ds_shell_tests.cpp
        src/main_window.h
        src/main_window.cpp
        src/settings_dialog.h
        src/settings_dialog.cpp)
    target_link_libraries(av_ds_shell_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME av_ds_shell_tests COMMAND av_ds_shell_tests)
    set_tests_properties(av_ds_shell_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(application_settings_tests tests/application_settings_tests.cpp)
    target_link_libraries(application_settings_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME application_settings_tests COMMAND application_settings_tests)

    add_executable(radar_model_tests tests/radar_model_tests.cpp)
    target_link_libraries(radar_model_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME radar_model_tests COMMAND radar_model_tests)

    add_executable(radar_widget_tests
        tests/radar_widget_tests.cpp
        src/settings_dialog.h
        src/settings_dialog.cpp)
    target_link_libraries(radar_widget_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME radar_widget_tests COMMAND radar_widget_tests)
    set_tests_properties(radar_widget_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(radar_client_tests tests/radar_client_tests.cpp)
    target_link_libraries(radar_client_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME radar_client_tests COMMAND radar_client_tests)

    add_executable(radar_manifest_tests
        tests/radar_manifest_tests.cpp
        ../../code/telemetry/phase2_manifest_builder.cpp)
    target_link_libraries(radar_manifest_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME radar_manifest_tests COMMAND radar_manifest_tests)

endif()

install(TARGETS av-ds RUNTIME DESTINATION . BUNDLE DESTINATION .)
install(FILES README.md ../../Copying.md ../../Unlicense.md DESTINATION licenses)
