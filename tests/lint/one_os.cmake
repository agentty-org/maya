# one_os.cmake — maya's door to jaal's one_os rule, for maya and above.
#
#   cmake -DROOTS="<dir>;..." [-DSELF=ON] -P one_os.cmake
#
# jaal decides the platform once (jaal/meta/target.hpp); maya re-exports it
# (maya/core/target.hpp: MAYA_OS_* / MAYA_CC_* / MAYA_CPU_* / MAYA_ISA_*).
# This runs jaal's lint over ROOTS with maya's prefix, so a project above
# maya (agentty) holds every file to the same rule without naming jaal —
# agentty → maya → jaal, strictly. SELF=ON is maya checking itself (it may
# also include core/target.hpp by a relative path, and test JAAL_*).

set(_jaal ${CMAKE_CURRENT_LIST_DIR}/../../third_party/jaal)
get_filename_component(_jaal ${_jaal} ABSOLUTE)
if(NOT EXISTS ${_jaal}/tests/lint/one_os.cmake)
    message(FATAL_ERROR "one_os: maya's third_party/jaal is missing")
endif()
if(SELF)
    set(_derived "JAAL|MAYA")
    set(_headers "maya/core/target\\.hpp|[./]*core/target\\.hpp|target\\.hpp")
else()
    set(_derived "MAYA")
    set(_headers "maya/core/target\\.hpp")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND}
            "-DROOTS=${ROOTS}"
            -DSOURCE=${_jaal}/include/jaal/meta/target.hpp
            "-DDERIVED=${_derived}"
            "-DHEADERS=${_headers}"
            -P ${_jaal}/tests/lint/one_os.cmake
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "one_os: a raw target macro outside jaal/meta/target.hpp (see above)")
endif()
