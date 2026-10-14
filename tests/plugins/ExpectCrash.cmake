# Runs CMD (a ;-list) and passes only if the process fails, e.g. by crashing.
# ctest's WILL_FAIL does not treat a crash as an expected failure.
execute_process(COMMAND ${CMD} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(result EQUAL 0)
    message(FATAL_ERROR "expected a crash, but the process exited cleanly:\n${out}${err}")
endif()
if(out MATCHES "No test cases matched")
    message(FATAL_ERROR "the crash test case was not found:\n${out}")
endif()
message(STATUS "process failed as expected (${result})")
