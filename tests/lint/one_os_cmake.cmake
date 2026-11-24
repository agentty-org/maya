# one_os_cmake.cmake — maya's door to jaal's one_os_cmake rule.
#
#   cmake -DFILES="<CMakeLists.txt>;..." [-DALLOW="<file>:<n>;..."] -P one_os_cmake.cmake
#
# A build file branches on the platform by asking the headers
# (cmake/MayaTarget.cmake: maya_target_is / MAYA_TARGET_*), never on WIN32 /
# APPLE / UNIX / CMAKE_SYSTEM_NAME. jaal's lint, so the rule is one rule;
# through maya, so a project above maya never names jaal.

get_filename_component(_jaal ${CMAKE_CURRENT_LIST_DIR}/../../third_party/jaal ABSOLUTE)
execute_process(
    COMMAND ${CMAKE_COMMAND} "-DFILES=${FILES}" "-DALLOW=${ALLOW}"
            -P ${_jaal}/tests/lint/one_os_cmake.cmake
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "one_os_cmake: a build file branches on the OS itself (see above); "
                        "ask the headers: MAYA_TARGET_* (cmake/MayaTarget.cmake)")
endif()
