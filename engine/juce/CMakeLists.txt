# The JUCE-backed device layer (WASAPI, ASIO). JUCE stays private to this target:
# its public header has no JUCE types.
add_library(weft_engine_juce STATIC src/JuceAudioDevice.cpp)
add_library(weft::engine_juce ALIAS weft_engine_juce)
target_include_directories(weft_engine_juce PUBLIC include)
target_link_libraries(weft_engine_juce
    PUBLIC weft::engine
    PRIVATE juce::juce_audio_devices juce::juce_recommended_config_flags)
target_compile_definitions(weft_engine_juce PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=0
    JUCE_WASAPI=1 JUCE_DIRECTSOUND=0)

# ASIO needs Steinberg's ASIO SDK, which isn't fetched (D-ARCH-14). Point
# WEFT_ASIO_SDK_DIR at an unpacked SDK to build the ASIO driver type.
set(WEFT_ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK folder (enables the ASIO driver on Windows)")
if(WIN32 AND WEFT_ASIO_SDK_DIR)
    target_include_directories(weft_engine_juce PRIVATE "${WEFT_ASIO_SDK_DIR}/common")
    target_compile_definitions(weft_engine_juce PRIVATE JUCE_ASIO=1)
endif()
