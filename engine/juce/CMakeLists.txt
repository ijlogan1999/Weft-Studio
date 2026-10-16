# The JUCE modules, compiled once into one static library that the platform layer,
# the app and their tests share (two copies of a module in one binary would clash).
add_library(weft_juce_modules STATIC)
target_link_libraries(weft_juce_modules
    PRIVATE juce::juce_audio_utils juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags)
target_compile_definitions(weft_juce_modules PUBLIC
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_WASAPI=1 JUCE_DIRECTSOUND=0
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_AU=0
    JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=0)
target_compile_definitions(weft_juce_modules INTERFACE $<TARGET_PROPERTY:weft_juce_modules,COMPILE_DEFINITIONS>)
target_include_directories(weft_juce_modules INTERFACE $<TARGET_PROPERTY:weft_juce_modules,INCLUDE_DIRECTORIES>)
set_target_properties(weft_juce_modules PROPERTIES POSITION_INDEPENDENT_CODE TRUE VISIBILITY_INLINES_HIDDEN TRUE
                      C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
add_library(weft::juce_modules ALIAS weft_juce_modules)

# The JUCE-backed platform layer: devices (WASAPI, ASIO), file decoders and VST3
# hosting. Its public headers have no JUCE types, except plugins/HostedPlugin.h.
add_library(weft_engine_juce STATIC
    src/HostedPlugin.cpp
    src/JuceAudioDevice.cpp
    src/JuceDecoders.cpp
    src/JuceMidiInputs.cpp
    src/PluginCatalog.cpp
    src/Vst3Host.cpp)
add_library(weft::engine_juce ALIAS weft_engine_juce)
target_include_directories(weft_engine_juce PUBLIC include)
# Reading class IDs from a VST3 factory uses the VST3 interface headers that ship with JUCE.
target_include_directories(weft_engine_juce PRIVATE "${WEFT_SRC_JUCE}/modules/juce_audio_processors/format_types/VST3_SDK")
target_link_libraries(weft_engine_juce PUBLIC weft::engine weft::juce_modules)

# The out-of-process plug-in scanner (F-PLG-2), shipped next to the app.
add_executable(weft-plugin-scanner "${PROJECT_SOURCE_DIR}/tools/scanner/ScannerMain.cpp")
target_link_libraries(weft-plugin-scanner PRIVATE weft::engine_juce)
weft_set_warnings(weft-plugin-scanner)

# ASIO needs Steinberg's ASIO SDK, which isn't fetched (D-ARCH-14). Point
# WEFT_ASIO_SDK_DIR at an unpacked SDK to build the ASIO driver type.
set(WEFT_ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK folder (enables the ASIO driver on Windows)")
if(WIN32 AND WEFT_ASIO_SDK_DIR)
    target_include_directories(weft_juce_modules PRIVATE "${WEFT_ASIO_SDK_DIR}/common")
    target_compile_definitions(weft_juce_modules PUBLIC JUCE_ASIO=1)
endif()
