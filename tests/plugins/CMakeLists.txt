# Hosts the fixture plugins through JUCE's VST3 host to prove they build and load.
# Full plugin-hosting acceptance tests (AT-PLG-*) arrive with the mixer in Milestone 6.
add_executable(weft_plugin_tests FixturePluginTests.cpp)
target_link_libraries(weft_plugin_tests PRIVATE
    juce::juce_audio_processors Catch2::Catch2
    juce::juce_recommended_config_flags)
target_compile_definitions(weft_plugin_tests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_STANDALONE_APPLICATION=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    "WEFT_FIXTURE_TESTGAIN=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestGain_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\""
    "WEFT_FIXTURE_TESTLATENCY=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestLatency_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\""
    "WEFT_FIXTURE_TESTSYNTH=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\""
    "WEFT_FIXTURE_TESTCRASHSCAN=\"$<GENEX_EVAL:$<TARGET_PROPERTY:TestCrashScan_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>\"")
add_dependencies(weft_plugin_tests TestGain_VST3 TestLatency_VST3 TestSynth_VST3 TestCrashScan_VST3)
catch_discover_tests(weft_plugin_tests DISCOVERY_MODE PRE_TEST)

add_test(NAME fixtures.TestCrashScan-crashes
         COMMAND "${CMAKE_COMMAND}" "-DCMD=$<TARGET_FILE:weft_plugin_tests>;[.crash]" -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectCrash.cmake")
