cmake_minimum_required(VERSION 3.25)

project(WeftStudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WEFT_WITH_JUCE "Build the JUCE-based targets (app, fixture plugins, plugin hosting tests)" ON)
option(WEFT_BUILD_TESTS "Build the test suites" ON)
set(WEFT_DONATE_URL "" CACHE STRING "Donation link shown in the app and README (empty hides the button)")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

include(cmake/WeftDeps.cmake)
include(cmake/WeftWarnings.cmake)

# Rust: the CRDT shim linked into the client (the relay builds with plain cargo).
weft_fetch(Corrosion)
corrosion_import_crate(MANIFEST_PATH "${PROJECT_SOURCE_DIR}/Cargo.toml" CRATES weft-crdt CRATE_TYPES staticlib LOCKED)
add_library(weft_crdt_c INTERFACE)
target_link_libraries(weft_crdt_c INTERFACE weft_crdt)
target_include_directories(weft_crdt_c INTERFACE "${PROJECT_SOURCE_DIR}/crates/weft-crdt/include")
add_library(weft::crdt ALIAS weft_crdt_c)

if(WEFT_WITH_JUCE)
    weft_fetch(JUCE)
endif()

add_subdirectory(core)
add_subdirectory(engine)
add_subdirectory(cli)

if(WEFT_WITH_JUCE)
    add_subdirectory(app)
endif()

if(WEFT_BUILD_TESTS)
    enable_testing()
    weft_fetch(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${WEFT_SRC_Catch2}/extras")
    include(Catch)
    add_subdirectory(tests)
endif()
