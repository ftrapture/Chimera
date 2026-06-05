cmake_minimum_required(VERSION 3.28)

project(
    ProjectChimera
    VERSION 0.1.0
    DESCRIPTION "Project Chimera: NTSR/FI Windows Runtime"
    LANGUAGES CXX)

include(CTest)
enable_testing()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)

add_library(
    chimera_common
    src/common/log.cpp
    src/config/config_loader.cpp
    src/platform/win32/process_utils.cpp)

target_include_directories(chimera_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(chimera_common PUBLIC nlohmann_json::nlohmann_json)
target_compile_definitions(chimera_common PUBLIC CHIMERA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
chimera_apply_project_options(chimera_common)

add_library(
    chimera_loader
    src/loader/policy_gate.cpp
    src/loader/attach_session.cpp)

target_include_directories(chimera_loader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(chimera_loader PUBLIC chimera_common)
chimera_apply_project_options(chimera_loader)

add_library(
    chimera_d3d12
    src/d3d12/device_context.cpp
    src/d3d12/sample_scene.cpp
    src/sr/analytic_tsr.cpp
    src/ml/neural_sr.cpp
    src/overlay/overlay_renderer.cpp
    src/ofa/optical_flow.cpp
    src/fg/frame_interpolation.cpp
    src/pacing/frame_pacer.cpp
    src/capture/d3d12_capture.cpp)

target_include_directories(chimera_d3d12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
    chimera_d3d12
    PUBLIC
        chimera_common
        d3d12
        dxgi
        d3dcompiler
        user32
        gdi32
        shcore)
target_compile_definitions(chimera_d3d12 PUBLIC CHIMERA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
chimera_apply_project_options(chimera_d3d12)

add_library(
    chimera_resource_inspection
    src/resource_inspection/signal_inspector.cpp)

target_include_directories(chimera_resource_inspection PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(chimera_resource_inspection PUBLIC chimera_common)
chimera_apply_project_options(chimera_resource_inspection)

add_library(
    chimera_runtime
    SHARED
    src/loader/runtime_entry.cpp
    src/loader/runtime_hooks.cpp
    src/loader/runtime_d3d12_overlay.cpp)

target_include_directories(chimera_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
    chimera_runtime
    PRIVATE
        chimera_d3d12
        chimera_resource_inspection
        minhook::minhook)
chimera_apply_project_options(chimera_runtime)

add_executable(chimera_sample_app WIN32 src/d3d12/sample_app_main.cpp)
target_link_libraries(chimera_sample_app PRIVATE chimera_d3d12)
chimera_apply_project_options(chimera_sample_app)

add_executable(chimera_launcher src/loader/chimera_launcher_main.cpp)
target_link_libraries(chimera_launcher PRIVATE chimera_loader)
chimera_apply_project_options(chimera_launcher)

add_executable(
    chimera_unit_tests
    tests/unit/config_tests.cpp
    tests/unit/confidence_tests.cpp
    tests/unit/policy_gate_tests.cpp
    tests/unit/signal_inspector_tests.cpp)
target_link_libraries(chimera_unit_tests PRIVATE chimera_loader chimera_resource_inspection Catch2::Catch2WithMain)
chimera_apply_project_options(chimera_unit_tests)

add_executable(
    chimera_integration_tests
    tests/integration/d3d12_harness_smoke.cpp)
target_link_libraries(chimera_integration_tests PRIVATE chimera_d3d12 Catch2::Catch2WithMain)
chimera_apply_project_options(chimera_integration_tests)

add_executable(chimera_dataset_gen tools/dataset_gen/dataset_gen_main.cpp)
target_link_libraries(chimera_dataset_gen PRIVATE chimera_d3d12)
chimera_apply_project_options(chimera_dataset_gen)

add_executable(chimera_capture_replay tools/capture_replay/capture_replay_main.cpp)
target_link_libraries(chimera_capture_replay PRIVATE chimera_d3d12)
chimera_apply_project_options(chimera_capture_replay)

add_executable(chimera_benchmark tools/benchmark/benchmark_main.cpp)
target_link_libraries(chimera_benchmark PRIVATE chimera_d3d12)
chimera_apply_project_options(chimera_benchmark)

add_executable(chimera_image_quality_tests tests/image_quality/image_quality_tests.cpp)
target_link_libraries(chimera_image_quality_tests PRIVATE chimera_d3d12 Catch2::Catch2WithMain)
chimera_apply_project_options(chimera_image_quality_tests)

include(Catch)
catch_discover_tests(chimera_unit_tests)
catch_discover_tests(chimera_integration_tests)
catch_discover_tests(chimera_image_quality_tests)
