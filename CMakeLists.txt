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

# Rust: weft-ffi bundles the CRDT shim, the MP3 codec and the relay transport into the one static library the
# client links (the relay builds with plain cargo).
weft_fetch(Corrosion)
corrosion_import_crate(MANIFEST_PATH "${PROJECT_SOURCE_DIR}/Cargo.toml" CRATES weft-ffi CRATE_TYPES staticlib LOCKED)
if(WEFT_BUILD_TESTS)
    # The relay binary, for the [S] sync tests (tests/sync).
    corrosion_import_crate(MANIFEST_PATH "${PROJECT_SOURCE_DIR}/Cargo.toml" CRATES weft-relay LOCKED)
endif()
# getrandom (pulled in by the relay transport's TLS) calls BCryptGenRandom, which Corrosion doesn't add on its own.
if(WIN32)
    target_link_libraries(weft_ffi INTERFACE bcrypt)
endif()
add_library(weft_crdt_c INTERFACE)
target_link_libraries(weft_crdt_c INTERFACE weft_ffi)
target_include_directories(weft_crdt_c INTERFACE "${PROJECT_SOURCE_DIR}/crates/weft-crdt/include")
add_library(weft::crdt ALIAS weft_crdt_c)
add_library(weft_codec_c INTERFACE)
target_link_libraries(weft_codec_c INTERFACE weft_ffi)
target_include_directories(weft_codec_c INTERFACE "${PROJECT_SOURCE_DIR}/crates/weft-codec/include")
add_library(weft::codec ALIAS weft_codec_c)
add_library(weft_sync_c INTERFACE)
target_link_libraries(weft_sync_c INTERFACE weft_crdt_c)
target_include_directories(weft_sync_c INTERFACE "${PROJECT_SOURCE_DIR}/crates/weft-sync/include")
add_library(weft::sync ALIAS weft_sync_c)

if(WEFT_WITH_JUCE)
    weft_fetch(JUCE)
endif()

add_subdirectory(core)
add_subdirectory(engine)
add_subdirectory(collab)
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
