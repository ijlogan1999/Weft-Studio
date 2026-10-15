# [U] unit tests
add_executable(weft_unit_tests
    unit/VersionTests.cpp
    unit/DocumentTests.cpp
    unit/ProjectTests.cpp
    unit/RepairTests.cpp
    unit/TimeTests.cpp
    unit/TrackTests.cpp
    unit/TransportTests.cpp
    unit/UndoTests.cpp
    unit/ValueTests.cpp
)
target_link_libraries(weft_unit_tests PRIVATE weft::core Catch2::Catch2WithMain)
catch_discover_tests(weft_unit_tests)

# CRDT load benchmark (M1); its own executable so unit test runs stay fast.
add_executable(weft_bench_tests bench/LoadBenchmarkTests.cpp)
target_link_libraries(weft_bench_tests PRIVATE weft::core Catch2::Catch2WithMain)
if(CMAKE_BUILD_TYPE MATCHES "^Rel")
    add_test(NAME bench.crdt-load COMMAND weft_bench_tests)
endif()

# [E] engine tests (NullDevice)
add_executable(weft_engine_tests
    engine/AllocationCounter.cpp
    engine/NullDeviceTests.cpp
    engine/RecordingTests.cpp
    engine/StreamingTests.cpp
    engine/TrackEngineTests.cpp
    engine/TransportEngineTests.cpp
)
target_link_libraries(weft_engine_tests PRIVATE weft::engine Catch2::Catch2WithMain)
catch_discover_tests(weft_engine_tests)

# AT-SYS-1 (build check part): every linked dependency is in the license manifest
# and THIRD_PARTY.md is up to date.
get_property(fetched GLOBAL PROPERTY WEFT_FETCHED_DEPS)
set(cpp_src_args "")
foreach(entry IN LISTS fetched)
    list(APPEND cpp_src_args "--cpp-src" "${entry}")
endforeach()
add_test(NAME AT-SYS-1.license-manifest
         COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/third_party.py" --check ${cpp_src_args}
         WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

# AT-PRJ-11 / AT-PRJ-12 [E]: kill the CLI after unsaved edits, then recover or discard.
foreach(mode IN ITEMS recover discard)
    if(mode STREQUAL "recover")
        set(at AT-PRJ-11)
    else()
        set(at AT-PRJ-12)
    endif()
    add_test(NAME ${at}.kill-${mode}
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/kill_recovery.py"
                     $<TARGET_FILE:weft-cli> "${CMAKE_CURRENT_BINARY_DIR}/kill-${mode}" ${mode})
endforeach()

add_test(NAME cli.render
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cli_render.py"
                 $<TARGET_FILE:weft-cli> "${CMAKE_CURRENT_BINARY_DIR}/cli-render")

add_test(NAME cli.version COMMAND weft-cli version)
set_tests_properties(cli.version PROPERTIES PASS_REGULAR_EXPRESSION "Weft Studio ${PROJECT_VERSION}")

if(WEFT_WITH_JUCE)
    add_executable(weft_juce_device_tests engine/JuceDeviceTests.cpp)
    target_link_libraries(weft_juce_device_tests PRIVATE weft::engine_juce Catch2::Catch2WithMain)
    catch_discover_tests(weft_juce_device_tests)

    add_subdirectory(fixtures/plugins)
    add_subdirectory(plugins)
endif()
