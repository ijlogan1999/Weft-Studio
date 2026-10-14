# [U] unit tests
add_executable(weft_unit_tests
    unit/VersionTests.cpp
    unit/DocumentTests.cpp
)
target_link_libraries(weft_unit_tests PRIVATE weft::core Catch2::Catch2WithMain)
catch_discover_tests(weft_unit_tests)

# [E] engine tests (NullDevice)
add_executable(weft_engine_tests
    engine/NullDeviceTests.cpp
)
target_link_libraries(weft_engine_tests PRIVATE weft::engine Catch2::Catch2WithMain)
catch_discover_tests(weft_engine_tests)

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

add_test(NAME cli.version COMMAND weft-cli version)
set_tests_properties(cli.version PROPERTIES PASS_REGULAR_EXPRESSION "Weft Studio ${PROJECT_VERSION}")

if(WEFT_WITH_JUCE)
    add_subdirectory(fixtures/plugins)
    add_subdirectory(plugins)
endif()
