# [U] unit tests
add_executable(weft_unit_tests
    unit/VersionTests.cpp
    unit/DocumentTests.cpp
    unit/ProjectTests.cpp
    unit/RepairTests.cpp
    unit/ArrangeTests.cpp
    unit/CollabModelTests.cpp
    unit/MediaImportTests.cpp
    unit/TimeTests.cpp
    unit/TrackTests.cpp
    unit/TransportTests.cpp
    unit/UndoTests.cpp
    unit/AutomationTests.cpp
    unit/PianoRollTests.cpp
    unit/ValueTests.cpp
)
target_link_libraries(weft_unit_tests PRIVATE weft::core Catch2::Catch2WithMain)
target_compile_definitions(weft_unit_tests PRIVATE WEFT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
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
    engine/ArrangeEngineTests.cpp
    engine/AutomationEngineTests.cpp
    engine/BounceTests.cpp
    engine/ImportTests.cpp
    engine/MixerEngineTests.cpp
    engine/NullDeviceTests.cpp
    engine/PianoRollEngineTests.cpp
    engine/ProjectFileTests.cpp
    engine/RecordingTests.cpp
    engine/StreamingTests.cpp
    engine/TrackEngineTests.cpp
    engine/TransportEngineTests.cpp
)
target_link_libraries(weft_engine_tests PRIVATE weft::engine weft::codec Catch2::Catch2WithMain)
catch_discover_tests(weft_engine_tests)

# [S] sync tests: clients with real Sessions against a weft-relay process on localhost.
add_executable(weft_sync_tests sync/SyncTests.cpp sync/MediaTests.cpp sync/PresenceTests.cpp sync/CollabTests.cpp)
target_link_libraries(weft_sync_tests PRIVATE weft::collab weft::engine Catch2::Catch2WithMain)
target_compile_definitions(weft_sync_tests PRIVATE WEFT_RELAY_EXE="$<TARGET_FILE:weft-relay>")
add_dependencies(weft_sync_tests weft-relay)
catch_discover_tests(weft_sync_tests PROPERTIES TIMEOUT 600)

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
    add_executable(weft_juce_device_tests engine/JuceDeviceTests.cpp engine/JuceImportTests.cpp engine/JuceBounceTests.cpp)
    target_link_libraries(weft_juce_device_tests PRIVATE weft::engine_juce Catch2::Catch2WithMain)
    target_compile_definitions(weft_juce_device_tests PRIVATE WEFT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    catch_discover_tests(weft_juce_device_tests)

    add_subdirectory(fixtures/plugins)
    add_subdirectory(plugins)
endif()

# [UI] tests: the arrange window, headless. Linux needs a virtual X server (xvfb-run).
if(WEFT_WITH_JUCE)
    add_executable(weft_ui_tests ui/UiMain.cpp ui/PanelTests.cpp ui/ArrangeUiTests.cpp ui/EditorTests.cpp ui/PianoRollUiTests.cpp
                                 ui/MixerUiTests.cpp ui/AutomationUiTests.cpp ui/BounceUiTests.cpp ui/ProjectUiTests.cpp ui/ShareUiTests.cpp
                                 ui/CollabUiTests.cpp)
    target_link_libraries(weft_ui_tests PRIVATE weft::app_lib Catch2::Catch2)
    # The mixer and plug-in manager tests host the fixture plug-ins through the real scanner.
    add_dependencies(weft_ui_tests weft-plugin-scanner TestGain_VST3 TestSynth_VST3 weft-relay)
    target_compile_definitions(weft_ui_tests PRIVATE
        "WEFT_SCANNER=\"$<TARGET_FILE:weft-plugin-scanner>\""
        "WEFT_RELAY_EXE=\"$<TARGET_FILE:weft-relay>\""
        "WEFT_FIXTURE_TESTGAIN=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestGain_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\""
        "WEFT_FIXTURE_TESTSYNTH=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\"")
    if(WIN32 OR APPLE)
        add_test(NAME ui-tests COMMAND weft_ui_tests)
    else()
        find_program(XVFB_RUN xvfb-run)
        if(XVFB_RUN)
            add_test(NAME ui-tests COMMAND "${XVFB_RUN}" -a $<TARGET_FILE:weft_ui_tests>)
        else()
            message(WARNING "xvfb-run not found: the [UI] tests won't run under ctest")
        endif()
    endif()
endif()
