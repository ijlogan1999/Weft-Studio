add_library(weft_engine STATIC
    src/arrange/Import.cpp
    src/arrange/OfflineRender.cpp
    src/render/Bounce.cpp
    src/devices/NullDevice.cpp
    src/dsp/WeftKeys.cpp
    src/dsp/BuiltinEffects.cpp
    src/dsp/BuiltinProcessor.cpp
    src/graph/Instrument.cpp
    src/graph/Mixer.cpp
    src/graph/SnapshotCompiler.cpp
    src/media/MediaPool.cpp
    src/media/MediaSource.cpp
    src/media/MediaStreamer.cpp
    src/midi/KeyboardMidi.cpp
    src/midi/MidiInputs.cpp
    src/record/RecordLayout.cpp
    src/record/RecordPass.cpp
    src/record/RecordWriter.cpp
    src/transport/Engine.cpp
    src/transport/LiveAutomation.cpp
    src/transport/Metronome.cpp
)
add_library(weft::engine ALIAS weft_engine)
target_include_directories(weft_engine PUBLIC include)
target_link_libraries(weft_engine PUBLIC weft::core PRIVATE weft::codec)
weft_set_warnings(weft_engine)

if(WEFT_WITH_JUCE)
    add_subdirectory(juce)
endif()
