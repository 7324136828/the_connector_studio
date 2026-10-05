cmake_minimum_required(VERSION 3.25)
project(ConnectorStudio VERSION 1.2.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Connector Studio requires Windows and the MSVC C++ toolchain.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/x64/$<CONFIG>")
endif()

set(LATTICE_CORE_SOURCES
    src/AppController.cpp
    src/Direct2DContext.cpp
    src/UIComponents.cpp
    src/SessionPersistence.cpp
    src/ItemTypeRegistry.cpp
    src/WorkProjectStore.cpp
    src/NewItemDialog.cpp
    src/ConnectorClient.cpp
    src/ProjectResources.cpp
    src/MarkdownRenderer.cpp
    src/McpClient.cpp
    src/ChatCopy.cpp
    src/SettingsDialog.cpp)
set(LATTICE_WINDOWS_LIBRARIES
    d2d1 dwrite windowscodecs comctl32 shlwapi shell32 ole32 user32 gdi32 comdlg32 dwmapi winhttp)

function(lattice_target_defaults target)
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /sdl /guard:cf $<$<CONFIG:Release>:/Zi>)
    target_link_options(${target} PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf /DEBUG)
    target_link_libraries(${target} PRIVATE ${LATTICE_WINDOWS_LIBRARIES})
endfunction()

add_library(lattice_core STATIC ${LATTICE_CORE_SOURCES})
lattice_target_defaults(lattice_core)

add_executable(ConnectorStudio WIN32 src/main.cpp resources/app.rc resources/app.manifest)
lattice_target_defaults(ConnectorStudio)
target_link_libraries(ConnectorStudio PRIVATE lattice_core)
target_link_options(ConnectorStudio PRIVATE "/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/resources/app.manifest")
install(TARGETS ConnectorStudio RUNTIME DESTINATION .)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.txt DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    add_executable(persistence_tests tests/persistence_tests.cpp)
    add_executable(controller_tests tests/controller_tests.cpp)
    add_executable(ui_layout_tests tests/ui_layout_tests.cpp)
    # This source includes main.cpp with LATTICE_TESTING, exposing the real
    # window/edit message handlers without a second application entrypoint.
    add_executable(interaction_tests tests/interaction_tests.cpp)
    add_executable(registry_tests tests/registry_tests.cpp)
    add_executable(project_tests tests/project_tests.cpp)
    add_executable(new_dialog_tests tests/new_dialog_tests.cpp)
    add_executable(connector_tests tests/connector_tests.cpp)
    add_executable(connection_controller_tests tests/connection_controller_tests.cpp)
    add_executable(settings_dialog_tests tests/settings_dialog_tests.cpp)
    add_executable(project_resource_tests tests/project_resource_tests.cpp)
    add_executable(markdown_tests tests/markdown_tests.cpp)
    add_executable(mcp_tests tests/mcp_tests.cpp)
    set(LATTICE_TEST_TARGETS
        persistence_tests controller_tests ui_layout_tests interaction_tests
        registry_tests project_tests new_dialog_tests
        connector_tests connection_controller_tests settings_dialog_tests
        project_resource_tests markdown_tests mcp_tests)
    foreach(test_target IN LISTS LATTICE_TEST_TARGETS)
        lattice_target_defaults(${test_target})
        target_link_libraries(${test_target} PRIVATE lattice_core ws2_32)
        set(test_workspace "${CMAKE_BINARY_DIR}/test-workspaces/${test_target}")
        file(MAKE_DIRECTORY "${test_workspace}")
        add_test(NAME ${test_target} COMMAND ${test_target})
        set_tests_properties(${test_target} PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${test_workspace}"
            ENVIRONMENT "CONNECTOR_STUDIO_TEST_ROOT=${test_workspace}")
    endforeach()
    add_custom_target(native-tests DEPENDS ${LATTICE_TEST_TARGETS})
endif()
