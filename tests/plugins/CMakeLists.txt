# Hosts the fixture plugins through Weft's VST3 host (AT-PLG-2, 3, 6, AT-MIX-5, 6, 12 with
# the real fixtures) and the out-of-process scanner.
add_executable(weft_plugin_tests FixturePluginTests.cpp PluginHostTests.cpp)
target_link_libraries(weft_plugin_tests PRIVATE weft::engine_juce Catch2::Catch2)
add_dependencies(weft_plugin_tests weft-plugin-scanner)
target_compile_definitions(weft_plugin_tests PRIVATE
    "WEFT_SCANNER=\"$<TARGET_FILE:weft-plugin-scanner>\""
    "WEFT_FIXTURE_TESTGAIN=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestGain_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\""
    "WEFT_FIXTURE_TESTLATENCY=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestLatency_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\""
    "WEFT_FIXTURE_TESTSYNTH=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\""
    "WEFT_FIXTURE_TESTCRASHSCAN=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestCrashScan_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\"")
add_dependencies(weft_plugin_tests TestGain_VST3 TestLatency_VST3 TestSynth_VST3 TestCrashScan_VST3)
catch_discover_tests(weft_plugin_tests DISCOVERY_MODE PRE_TEST)

add_test(NAME fixtures.TestCrashScan-crashes
         COMMAND "${CMAKE_COMMAND}" "-DCMD=$<TARGET_FILE:weft_plugin_tests>;[.crash]" -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectCrash.cmake")
