cmake_minimum_required(VERSION 3.20)

project(gtaiv_xr_host LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "gtaiv_xr_host is Windows-only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "gtaiv_xr_host must be configured for x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OPENXR_SDK_ROOT "" CACHE PATH "Pinned Khronos OpenXR-SDK checkout")
if(NOT EXISTS "${OPENXR_SDK_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "OPENXR_SDK_ROOT is missing. Run scripts/fetch-openxr-sdk.ps1 first.")
endif()

set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(INSTALL_SYMBOLS OFF CACHE BOOL "" FORCE)

add_subdirectory(
  "${OPENXR_SDK_ROOT}"
  "${CMAKE_CURRENT_BINARY_DIR}/openxr-sdk"
  EXCLUDE_FROM_ALL)

set(GTAIV_XR_BUILD_ID "dev" CACHE STRING "Build identity written to the host log")

add_executable(gtaiv_xr_host
  main.cpp
  game_bridge.cpp
  pose_bridge.cpp)
target_compile_definitions(gtaiv_xr_host PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  XR_USE_PLATFORM_WIN32
  XR_USE_GRAPHICS_API_D3D11
  GTAIV_XR_BUILD_ID="${GTAIV_XR_BUILD_ID}")
target_compile_options(gtaiv_xr_host PRIVATE /W4 /WX /permissive- /EHsc /fp:strict)
target_link_libraries(gtaiv_xr_host PRIVATE
  openxr_loader
  d3d11
  d3dcompiler
  dxgi)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(HOST_OUTPUT_DIR "${REPOSITORY_ROOT}/out-openxr")
set_target_properties(gtaiv_xr_host PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${HOST_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${HOST_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${HOST_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${HOST_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${HOST_OUTPUT_DIR}")

add_custom_command(TARGET gtaiv_xr_host POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${OPENXR_SDK_ROOT}/LICENSE"
    "${HOST_OUTPUT_DIR}/THIRD-PARTY-OpenXR-LICENSE.txt"
  VERBATIM)
