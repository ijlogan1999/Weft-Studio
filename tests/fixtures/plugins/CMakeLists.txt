# Fixture VST3 plugins from spec 06 ("Fixture plugins"). Written from scratch for tests; never shipped.

function(weft_fixture_plugin name code)
    cmake_parse_arguments(arg "SYNTH" "" "" ${ARGN})
    if(arg_SYNTH)
        set(synth TRUE)
        set(midi_in TRUE)
    else()
        set(synth FALSE)
        set(midi_in FALSE)
    endif()
    juce_add_plugin(${name}
        COMPANY_NAME "Weft Test Fixtures"
        PLUGIN_MANUFACTURER_CODE Wftx
        PLUGIN_CODE ${code}
        FORMATS VST3
        PRODUCT_NAME "${name}"
        BUNDLE_ID "org.weftstudio.fixtures.${name}"
        IS_SYNTH ${synth}
        NEEDS_MIDI_INPUT ${midi_in}
        COPY_PLUGIN_AFTER_BUILD FALSE
        ${arg_UNPARSED_ARGUMENTS})
    target_sources(${name} PRIVATE ${name}.cpp)
    target_compile_definitions(${name} PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(${name} PRIVATE juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    get_target_property(artefacts ${name}_VST3 JUCE_PLUGIN_ARTEFACT_FILE)
    set(WEFT_FIXTURE_${name} "${artefacts}" PARENT_SCOPE)
endfunction()

weft_fixture_plugin(TestGain TGai)
weft_fixture_plugin(TestLatency TLat)
weft_fixture_plugin(TestSynth TSyn SYNTH)
# Crashes when instantiated, so the build must not load it to generate a manifest.
weft_fixture_plugin(TestCrashScan TCra VST3_AUTO_MANIFEST FALSE)

set(WEFT_FIXTURE_TestGain "${WEFT_FIXTURE_TestGain}" PARENT_SCOPE)
set(WEFT_FIXTURE_TestLatency "${WEFT_FIXTURE_TestLatency}" PARENT_SCOPE)
set(WEFT_FIXTURE_TestSynth "${WEFT_FIXTURE_TestSynth}" PARENT_SCOPE)
set(WEFT_FIXTURE_TestCrashScan "${WEFT_FIXTURE_TestCrashScan}" PARENT_SCOPE)
