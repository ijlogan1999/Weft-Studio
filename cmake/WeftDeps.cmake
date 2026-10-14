# Fetches C++ dependencies. The only place a dependency can come from is
# third_party/cpp-deps.json, which is also the license manifest (AT-SYS-1).
include(FetchContent)

file(READ "${PROJECT_SOURCE_DIR}/third_party/cpp-deps.json" WEFT_DEPS_JSON)
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${PROJECT_SOURCE_DIR}/third_party/cpp-deps.json")

# weft_fetch(<name>) declares and populates a dependency listed in cpp-deps.json.
# Sets WEFT_SRC_<name> and records the source dir for the license check.
function(weft_fetch name)
    string(JSON git ERROR_VARIABLE err GET "${WEFT_DEPS_JSON}" "${name}" git)
    if(err)
        message(FATAL_ERROR "Dependency '${name}' is not in third_party/cpp-deps.json (AT-SYS-1)")
    endif()
    string(JSON tag GET "${WEFT_DEPS_JSON}" "${name}" tag)
    FetchContent_Declare(${name} GIT_REPOSITORY "${git}" GIT_TAG "${tag}" GIT_SHALLOW TRUE ${ARGN})
    FetchContent_MakeAvailable(${name})
    string(TOLOWER "${name}" lname)
    set(WEFT_SRC_${name} "${${lname}_SOURCE_DIR}" PARENT_SCOPE)
    set_property(GLOBAL APPEND PROPERTY WEFT_FETCHED_DEPS "${name}=${${lname}_SOURCE_DIR}")
endfunction()
