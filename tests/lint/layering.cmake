# tests/lint/layering.cmake — maya sits between agentty and jaal.
#
#   agentty → maya → jaal
#
# maya may use jaal (it is built on it), but never anything above it: no
# agentty includes, no agentty:: names, no AGENTTY_ macros in code. Comments
# and string literals may mention agentty (an env-var alias kept for old
# hosts lives in a string, which is fine).
#
# Run with: cmake -DROOT=<maya source dir> -P layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(hits "")
foreach(dir include src tests examples)
    file(GLOB_RECURSE files ${ROOT}/${dir}/*.cpp ${ROOT}/${dir}/*.hpp ${ROOT}/${dir}/*.h)
    foreach(f IN LISTS files)
        file(STRINGS ${f} lines REGEX "agentty|AGENTTY")
        if(NOT lines)
            continue()
        endif()
        file(RELATIVE_PATH rel ${ROOT} ${f})
        foreach(l IN LISTS lines)
            string(REGEX REPLACE "//.*$" "" code "${l}")
            string(REGEX REPLACE "\"[^\"]*\"" "\"\"" code "${code}")
            if(code MATCHES "#[ \t]*include[ \t]*[<\"]agentty/"
               OR code MATCHES "(^|[^A-Za-z0-9_])agentty::"
               OR code MATCHES "(^|[^A-Za-z0-9_])AGENTTY_[A-Z]")
                string(STRIP "${l}" s)
                list(APPEND hits "${rel}: ${s}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR
        "maya reaches up into agentty:\n  ${msg}\n\n"
        "maya is a library agentty builds on. Expose a hook or an option "
        "and let agentty pass the policy down.")
endif()

# Inside maya, only the adapter to the runtime may name jaal: host/ (where
# maya plugs into jaal as a terminal host) and the re-export seams for apps:
# runtime.hpp (everything) and capability.hpp (just the lock-order
# capability types, for headers that only pass a capability through), and
# core/target.hpp (jaal's one decision of what the platform is, re-exported
# as MAYA_OS_* / MAYA_CC_* / MAYA_CPU_* / MAYA_ISA_*). The
# view layer — elements, widgets, render, style, terminal — stays a pure
# library that knows nothing about the runtime.
set(jhits "")
foreach(dir include src)
    file(GLOB_RECURSE files ${ROOT}/${dir}/*.cpp ${ROOT}/${dir}/*.hpp ${ROOT}/${dir}/*.h)
    foreach(f IN LISTS files)
        file(RELATIVE_PATH rel ${ROOT} ${f})
        if(rel MATCHES "^include/maya/host/" OR rel MATCHES "^src/host/"
           OR rel STREQUAL "include/maya/runtime.hpp"
           OR rel STREQUAL "include/maya/capability.hpp"
           OR rel STREQUAL "include/maya/core/target.hpp")
            continue()
        endif()
        file(STRINGS ${f} lines REGEX "jaal")
        foreach(l IN LISTS lines)
            string(REGEX REPLACE "//.*$" "" code "${l}")
            string(REGEX REPLACE "\"[^\"]*\"" "\"\"" code "${code}")
            if(code MATCHES "#[ \t]*include[ \t]*[<\"]jaal/"
               OR code MATCHES "(^|[^A-Za-z0-9_])jaal::")
                string(STRIP "${l}" s)
                list(APPEND jhits "${rel}: ${s}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(jhits)
    list(JOIN jhits "\n  " msg)
    message(FATAL_ERROR
        "maya's view layer names jaal directly:\n  ${msg}\n\n"
        "Only maya/host/ and maya/runtime.hpp adapt to the runtime. Move "
        "the runtime part into host/, or take what you need as a parameter.")
endif()
message(STATUS "layering ok: maya depends on nothing above it, and only host/ touches jaal")
