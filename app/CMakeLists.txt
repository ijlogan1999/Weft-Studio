juce_add_gui_app(WeftStudio
    PRODUCT_NAME "Weft Studio"
    COMPANY_NAME "Weft Studio contributors"
    BUNDLE_ID "org.weftstudio.app"
    VERSION ${PROJECT_VERSION})

target_sources(WeftStudio PRIVATE src/Main.cpp src/MainComponent.cpp src/AboutPanel.cpp)

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

target_compile_definitions(WeftStudio PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:WeftStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:WeftStudio,JUCE_VERSION>"
    WEFT_DONATE_URL="${WEFT_DONATE_URL}")

target_link_libraries(WeftStudio PRIVATE
    WeftStudioData weft::engine_juce weft::juce_modules
    juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
