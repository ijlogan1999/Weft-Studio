# The JUCE modules, compiled once into one static library that the platform layer,
# the app and their tests share (two copies of a module in one binary would clash).
add_library(weft_juce_modules STATIC)
target_link_libraries(weft_juce_modules
    PRIVATE juce::juce_audio_utils juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags)
target_compile_definitions(weft_juce_modules PUBLIC
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_WASAPI=1 JUCE_DIRECTSOUND=0
    JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=0)
target_compile_definitions(weft_juce_modules INTERFACE $<TARGET_PROPERTY:weft_juce_modules,COMPILE_DEFINITIONS>)
target_include_directories(weft_juce_modules INTERFACE $<TARGET_PROPERTY:weft_juce_modules,INCLUDE_DIRECTORIES>)
set_target_properties(weft_juce_modules PROPERTIES POSITION_INDEPENDENT_CODE TRUE VISIBILITY_INLINES_HIDDEN TRUE
                      C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
add_library(weft::juce_modules ALIAS weft_juce_modules)

# The JUCE-backed platform layer: devices (WASAPI, ASIO) and file decoders. Its
# public headers have no JUCE types.
add_library(weft_engine_juce STATIC src/JuceAudioDevice.cpp src/JuceDecoders.cpp)
add_library(weft::engine_juce ALIAS weft_engine_juce)
target_include_directories(weft_engine_juce PUBLIC include)
target_link_libraries(weft_engine_juce PUBLIC weft::engine weft::juce_modules)

# ASIO needs Steinberg's ASIO SDK, which isn't fetched (D-ARCH-14). Point
# WEFT_ASIO_SDK_DIR at an unpacked SDK to build the ASIO driver type.
set(WEFT_ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK folder (enables the ASIO driver on Windows)")
if(WIN32 AND WEFT_ASIO_SDK_DIR)
    target_include_directories(weft_juce_modules PRIVATE "${WEFT_ASIO_SDK_DIR}/common")
    target_compile_definitions(weft_juce_modules PUBLIC JUCE_ASIO=1)
endif()
