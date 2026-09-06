cmake_minimum_required(VERSION 3.20)
project(SLOFabric VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SLOFABRIC_BUILD_TESTS "Build the test suite" ON)
option(SLOFABRIC_BUILD_EXAMPLES "Build runnable examples" ON)
option(SLOFABRIC_BUILD_TOOLS "Build the CLI inspection tool" ON)
option(SLOFABRIC_BUILD_BENCHMARKS "Build benchmarks" ON)
option(SLOFABRIC_BUILD_DISTRIBUTED "Build the distributed proof binaries" ON)
option(SLOFABRIC_BUILD_CUDA "Build the CUDA proof (requires CUDA)" OFF)
option(SLOFABRIC_ASAN "Enable AddressSanitizer (MSVC/clang)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warnings: strict, /W4 /WX on MSVC for all first-party code.
# ---------------------------------------------------------------------------
function(slofabric_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(SLOFABRIC_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
      target_compile_definitions(${target} PRIVATE SLOFABRIC_ASAN=1)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(SLOFABRIC_ASAN)
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
      target_compile_definitions(${target} PRIVATE SLOFABRIC_ASAN=1)
    endif()
  endif()
endfunction()

set(SLOFABRIC_SOURCES
  src/slofabric/version.cpp
  src/slofabric/persistence.cpp
  src/slofabric/fabric.cpp
)

add_library(slofabric STATIC ${SLOFABRIC_SOURCES})
add_library(slofabric::slofabric ALIAS slofabric)
target_include_directories(slofabric PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(slofabric PUBLIC cxx_std_20)
slofabric_apply_warnings(slofabric)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SLOFABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SLOFABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(SLOFABRIC_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(SLOFABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(SLOFABRIC_BUILD_DISTRIBUTED)
  add_subdirectory(distributed)
endif()

if(SLOFABRIC_BUILD_CUDA)
  add_subdirectory(cuda)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS slofabric
  EXPORT SLOFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(SLOFABRIC_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/SLOFabric")
install(EXPORT SLOFabricTargets
  FILE SLOFabricTargets.cmake
  NAMESPACE slofabric::
  DESTINATION ${SLOFABRIC_CMAKE_INSTALL_DIR})

configure_package_config_file(
  cmake/SLOFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/SLOFabricConfig.cmake
  INSTALL_DESTINATION ${SLOFABRIC_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/SLOFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/SLOFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/SLOFabricConfigVersion.cmake
  DESTINATION ${SLOFABRIC_CMAKE_INSTALL_DIR})
