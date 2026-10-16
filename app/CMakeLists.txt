juce_add_gui_app(WeftStudio
    PRODUCT_NAME "Weft Studio"
    COMPANY_NAME "Weft Studio contributors"
    BUNDLE_ID "org.weftstudio.app"
    VERSION ${PROJECT_VERSION})

target_sources(WeftStudio PRIVATE src/Main.cpp)

# The window and everything in it, as a library the UI tests link too.
add_library(weft_app_lib STATIC
    src/ui/AboutPanel.cpp
    src/ui/BottomArea.cpp
    src/ui/BounceDialog.cpp
    src/ui/PianoRoll.cpp
    src/ui/Commands.cpp
    src/ui/ControlBar.cpp
    src/ui/Inspector.cpp
    src/ui/MainComponent.cpp
    src/ui/Mixer.cpp
    src/ui/PluginWindows.cpp
    src/ui/NewTrackDialog.cpp
    src/ui/SidePanel.cpp
    src/ui/Studio.cpp
    src/ui/Theme.cpp
    src/ui/TracksArea.cpp)
add_library(weft::app_lib ALIAS weft_app_lib)
target_include_directories(weft_app_lib PUBLIC src)

# Third-party notices for the About box (F-SYS-1), generated from the license manifest.
get_property(fetched GLOBAL PROPERTY WEFT_FETCHED_DEPS)
set(cpp_src_args "")
foreach(entry IN LISTS fetched)
    list(APPEND cpp_src_args "--cpp-src" "${entry}")
endforeach()
set(notices "${CMAKE_CURRENT_BINARY_DIR}/ThirdPartyNotices.txt")
add_custom_command(OUTPUT "${notices}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/third_party.py" --notices "${notices}" ${cpp_src_args}
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/third_party.py" "${PROJECT_SOURCE_DIR}/third_party/cpp-deps.json"
            "${PROJECT_SOURCE_DIR}/Cargo.lock"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Generating third-party notices")
juce_add_binary_data(WeftStudioData SOURCES "${notices}" "${PROJECT_SOURCE_DIR}/LICENSE")

target_compile_definitions(weft_app_lib PUBLIC
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:WeftStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:WeftStudio,JUCE_VERSION>"
    WEFT_DONATE_URL="${WEFT_DONATE_URL}")
target_link_libraries(weft_app_lib
    PUBLIC WeftStudioData weft::engine_juce weft::juce_modules
    PRIVATE juce::juce_recommended_warning_flags)
set_target_properties(weft_app_lib PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

target_link_libraries(WeftStudio PRIVATE
    weft::app_lib juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# The plug-in scanner runs next to the app (F-PLG-2): Studio looks for it beside the executable.
add_dependencies(WeftStudio weft-plugin-scanner)
add_custom_command(TARGET WeftStudio POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:weft-plugin-scanner>" "$<TARGET_FILE_DIR:WeftStudio>"
    COMMENT "Copying the plug-in scanner next to the app")
