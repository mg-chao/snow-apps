# Runs a timing-sensitive test up to MAX_ATTEMPTS times and reports success
# as soon as one attempt passes, so intermittent failures under machine load
# are tolerated without weakening genuine failures: every attempt has to fail
# before the test is reported as failed.
#
# Required: -DTEST_COMMAND=<executable>[;<arg>...] (semicolon-separated list)
# Optional: -DMAX_ATTEMPTS=<n> (default 3)

if(NOT DEFINED TEST_COMMAND)
    message(FATAL_ERROR "TEST_COMMAND is required")
endif()
if(NOT DEFINED MAX_ATTEMPTS)
    set(MAX_ATTEMPTS 3)
endif()

set(_attempts 0)
set(_result 1)
while(_attempts LESS MAX_ATTEMPTS AND NOT _result EQUAL 0)
    math(EXPR _attempts "${_attempts} + 1")
    execute_process(
        COMMAND ${TEST_COMMAND}
        RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0 AND _attempts LESS MAX_ATTEMPTS)
        message(STATUS "flaky test attempt ${_attempts}/${MAX_ATTEMPTS} failed; retrying")
    endif()
endwhile()

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "flaky test still failing after ${_attempts} attempt(s)")
endif()
