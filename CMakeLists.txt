cmake_minimum_required(VERSION 3.25)

project(FacilityEfficiencyLedger
    VERSION 1.0.0
    DESCRIPTION "Deterministic facility efficiency accounting ledger for a Data Center Control Plane"
    HOMEPAGE_URL "https://github.com/summonlabs/Facility-Efficiency-Ledger"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FEL_BUILD_CLI        "Build the fel command line interface"      ON)
option(FEL_BUILD_TESTS      "Build the test suite"                      ON)
option(FEL_BUILD_BENCHMARKS "Build the benchmark driver"                ON)
option(FEL_WARNINGS_AS_ERRORS "Treat first-party warnings as errors"    ON)
option(FEL_ENABLE_ASAN      "Build with AddressSanitizer where supported" OFF)
option(FEL_ENABLE_UBSAN     "Build with UndefinedBehaviorSanitizer where supported" OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FEL_IS_TOP_LEVEL ON)
else()
    set(FEL_IS_TOP_LEVEL OFF)
endif()

if(FEL_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Keep archives and executables in separate directories so that an executable's
# import library can never shadow the static library of the same name.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warning configuration (first-party code only)
# ---------------------------------------------------------------------------
# Strict warnings are applied to first-party targets only, never exported.
function(fel_apply_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc
            /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546
            /w14547 /w14548 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906
            /w14928 /w15038
            $<$<BOOL:${FEL_WARNINGS_AS_ERRORS}>:/WX>)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            -Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
            -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wnull-dereference
            $<$<BOOL:${FEL_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    if(FEL_ENABLE_ASAN AND MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
    elseif(FEL_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
    if(FEL_ENABLE_UBSAN AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Build identity
# ---------------------------------------------------------------------------
set(FEL_GIT_COMMIT "unknown")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE FEL_GIT_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE FEL_GIT_RESULT)
        if(NOT FEL_GIT_RESULT EQUAL 0)
            set(FEL_GIT_COMMIT "unknown")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(fel
    src/status.cpp
    src/ids.cpp
    src/rational.cpp
    src/unit.cpp
    src/quantity.cpp
    src/time.cpp
    src/digest.cpp
    src/codec.cpp
    src/platform.cpp
    src/journal.cpp
    src/store.cpp
    src/authority.cpp
    src/view.cpp
    src/reconcile.cpp
    src/report.cpp
    src/ledger.cpp
)
add_library(FacilityEfficiencyLedger::fel ALIAS fel)

target_include_directories(fel PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(fel PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    EXPORT_NAME fel)

fel_apply_strict_warnings(fel)
target_compile_definitions(fel PRIVATE FEL_GIT_COMMIT="${FEL_GIT_COMMIT}")
target_link_libraries(fel PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(FEL_BUILD_CLI)
    add_executable(fel_cli apps/fel_main.cpp)
    set_target_properties(fel_cli PROPERTIES OUTPUT_NAME fel)
    fel_apply_strict_warnings(fel_cli)
    target_link_libraries(fel_cli PRIVATE fel)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FEL_BUILD_BENCHMARKS)
    add_executable(fel_bench benchmarks/bench_main.cpp)
    fel_apply_strict_warnings(fel_bench)
    target_link_libraries(fel_bench PRIVATE fel)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS fel
    EXPORT FacilityEfficiencyLedgerTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FEL_BUILD_CLI)
    install(TARGETS fel_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT FacilityEfficiencyLedgerTargets
    FILE FacilityEfficiencyLedgerTargets.cmake
    NAMESPACE FacilityEfficiencyLedger::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FacilityEfficiencyLedger)

configure_package_config_file(
    cmake/FacilityEfficiencyLedgerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/FacilityEfficiencyLedgerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FacilityEfficiencyLedger)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/FacilityEfficiencyLedgerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/FacilityEfficiencyLedgerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/FacilityEfficiencyLedgerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FacilityEfficiencyLedger)

install(FILES README.md LICENSE NOTICE
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
