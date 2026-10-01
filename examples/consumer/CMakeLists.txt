cmake_minimum_required(VERSION 3.25)

# Independent out-of-tree consumer of the installed FacilityEfficiencyLedger
# package. This project is deliberately not part of the main build: it exists to
# prove that the installed artefact, its exported targets, and its public headers
# are usable by a downstream project that only has an install prefix.

project(fel_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FacilityEfficiencyLedger 1.0 REQUIRED CONFIG)

add_executable(fel_consumer main.cpp)
target_link_libraries(fel_consumer PRIVATE FacilityEfficiencyLedger::fel)
