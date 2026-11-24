# MayaTarget.cmake — a build asks the C++ headers what the platform is.
#
#   include(<maya>/cmake/MayaTarget.cmake)
#   maya_target_is(<C++ expression over MAYA_*> <out-var>)
#   maya_target_facts()        # sets MAYA_TARGET_WINDOWS / _APPLE / _LINUX /
#                              # _ANDROID / _POSIX (ON/OFF) in the caller
#
# jaal decides the platform once (jaal/meta/target.hpp) and maya re-exports
# it (maya/core/target.hpp). A build script that re-derived the answer from
# WIN32 / APPLE / UNIX / CMAKE_SYSTEM_NAME would be a second copy, free to
# drift from the one the code is compiled against: a link line for one OS
# beside sources compiled for another. So the build doesn't decide; it asks,
# exactly the way jaal's own CMake does (cmake/JaalTarget.cmake), through
# maya's re-export so a project above maya never names jaal.
#
# A COMPILE check, never a run: right when cross-compiling, and cached (one
# tiny compile per question, at the first configure). Usable before
# add_subdirectory(maya): it needs only the headers.

include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

set(_MAYA_TARGET_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/../include;${CMAKE_CURRENT_LIST_DIR}/../third_party/jaal/include"
    CACHE INTERNAL "")

function(maya_target_is expr out)
    string(MAKE_C_IDENTIFIER "MAYA_TARGET_IS_${expr}" key)
    set(CMAKE_REQUIRED_INCLUDES "${_MAYA_TARGET_INCLUDES}")
    set(CMAKE_REQUIRED_QUIET ON)
    # The stack's dialect, not the compiler's default: gnu++NN predefines
    # `linux` / `unix` as 1 -- a different preprocessor than the one the
    # code is built with (jaal's JaalTarget.cmake does the same).
    set(CMAKE_REQUIRED_FLAGS "")
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    check_cxx_source_compiles("
#include <maya/core/target.hpp>
#if !(${expr})
#error no
#endif
int main() { return 0; }" ${key})
    if(${key})
        set(${out} ON PARENT_SCOPE)
    else()
        set(${out} OFF PARENT_SCOPE)
    endif()
endfunction()

# A check that cannot COMPILE reads as "no", silently: every platform answer
# would come back OFF. Prove the headers compile here first; stop otherwise.
maya_target_is(1 _maya_target_works)
if(NOT _maya_target_works)
    message(FATAL_ERROR "maya: cannot compile maya/core/target.hpp with this compiler, "
                        "so no platform question can be answered; see "
                        "CMakeConfigureLog.yaml (check MAYA_TARGET_IS_1)")
endif()

# The answers most build scripts want, asked once each.
macro(maya_target_facts)
    maya_target_is("MAYA_OS_WINDOWS" MAYA_TARGET_WINDOWS)
    maya_target_is("MAYA_OS_APPLE"   MAYA_TARGET_APPLE)
    maya_target_is("MAYA_OS_LINUX"   MAYA_TARGET_LINUX)
    maya_target_is("MAYA_OS_ANDROID" MAYA_TARGET_ANDROID)
    maya_target_is("MAYA_OS_POSIX"   MAYA_TARGET_POSIX)
    maya_target_is("MAYA_CPU_X86_64" MAYA_TARGET_X86_64)
    # MinGW: Windows without MSVC's ABI -- GCC or clang on the MinGW runtime,
    # what CMake's MINGW means (its own link and static-runtime quirks).
    maya_target_is("MAYA_OS_WINDOWS && !MAYA_CC_MSVC_ABI" MAYA_TARGET_MINGW)
    # Linux in the GNU/Linux sense (not Android): glibc, ELF, /proc.
    if(MAYA_TARGET_LINUX AND NOT MAYA_TARGET_ANDROID)
        set(MAYA_TARGET_GNU_LINUX ON)
    else()
        set(MAYA_TARGET_GNU_LINUX OFF)
    endif()
    # The free unixes: POSIX, not Apple (ELF, a separate libdl/librt...).
    if(MAYA_TARGET_POSIX AND NOT MAYA_TARGET_APPLE)
        set(MAYA_TARGET_ELF_UNIX ON)
    else()
        set(MAYA_TARGET_ELF_UNIX OFF)
    endif()
endmacro()
