cmake_minimum_required(VERSION 3.22)

project(FsoSimpitRadar VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network Test)
find_package(Qt6 6.2 QUIET COMPONENTS LinguistTools)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_subdirectory("${REPOSITORY_ROOT}/code/telemetry/protocol"
                 "${CMAKE_CURRENT_BINARY_DIR}/fstl_protocol")

add_library(radar_core STATIC
    src/radar_image.h
    src/radar_image.cpp
    src/radar_client.h
    src/radar_client.cpp
    src/radar_widget.h
    src/radar_widget.cpp
)
target_include_directories(radar_core PUBLIC src)
target_link_libraries(radar_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network fstl_protocol)
target_compile_definitions(radar_core PUBLIC QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)

add_executable(fso-simpit-radar WIN32
    src/main.cpp
    src/main_window.h
    src/main_window.cpp
    src/settings_dialog.h
    src/settings_dialog.cpp
)
target_link_libraries(fso-simpit-radar PRIVATE radar_core)
set_target_properties(fso-simpit-radar PROPERTIES
    OUTPUT_NAME "FsoSimpitRadar"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

if(Qt6LinguistTools_FOUND)
    qt_add_translations(fso-simpit-radar
        TS_FILES translations/radar_fr.ts translations/radar_en.ts
        RESOURCE_PREFIX "/i18n")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(radar_model_tests tests/radar_model_tests.cpp)
    target_link_libraries(radar_model_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME radar_model_tests COMMAND radar_model_tests)

    add_executable(radar_widget_tests tests/radar_widget_tests.cpp)
    target_link_libraries(radar_widget_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME radar_widget_tests COMMAND radar_widget_tests)
    set_tests_properties(radar_widget_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(radar_client_tests tests/radar_client_tests.cpp)
    target_link_libraries(radar_client_tests PRIVATE radar_core Qt6::Test)
    add_test(NAME radar_client_tests COMMAND radar_client_tests)
endif()

install(TARGETS fso-simpit-radar RUNTIME DESTINATION . BUNDLE DESTINATION .)
install(FILES README.md ../../Copying.md ../../Unlicense.md DESTINATION licenses)

