cmake_minimum_required(VERSION 3.28...3.31)
project(maya
    VERSION     0.1.0
    DESCRIPTION "A fast immediate-mode terminal UI + markdown renderer in modern C++"
    LANGUAGES   CXX)

if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fall back to C++23 when the compiler doesn't advertise C++26 (older Termux
# clang < 18, gcc < 14). maya uses no C++26-only library facility, so C++23
# builds cleanly. Matches the same guard on the target_compile_features below.
if(NOT MSVC AND NOT "cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES
            AND "cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    set(CMAKE_CXX_STANDARD 23)
    message(STATUS "maya: C++26 not advertised by "
                   "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
                   "— building at C++23.")
endif()

# Default to Release so out-of-the-box builds get -O3.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# ── Linux-specific aggressive optimization ────────────────────────────────
#
# MAYA_NATIVE_TUNING gates -march=native / -mtune=native. Default ON for
# local dev (the producing host == the running host), but consumers shipping
# a portable binary MUST turn it off — `-march=native` bakes the build
# host's specific microarchitecture into the binary, and the result will
# SIGILL on any chip lacking those features. Particularly visible on
# aarch64, where CI runners (Graviton2/3/4) emit instructions older
# Cortex-A cores can't decode. With this OFF the build relies on the
# compiler's default baseline (`-march=x86-64` / `armv8-a`) which every
# chip of the relevant arch supports.
option(MAYA_NATIVE_TUNING "Bake the host CPU into the binary (-march=native -mtune=native). OFF for portable / standalone releases." ON)

# Treat warnings as errors. OFF by default so day-to-day dev builds aren't
# blocked by a stray warning; CI turns it ON (-DMAYA_WERROR=ON) to keep the
# tree warning-clean. Applies -Werror (GCC/Clang) or /WX (MSVC) to the maya
# target; -Wall -Wextra / -W4 are always on regardless of this flag.
option(MAYA_WERROR "Treat compiler warnings as errors (CI)." OFF)

# ── Compiler cache ────────────────────────────────────────────────────────
# maya is header-heavy: every TU pulls in the DSL, the element tree and a
# pile of widgets, so the SAME work is redone constantly — across the two
# library variants (maya and maya_test), across 55 example binaries, and
# after every `git checkout` or reconfigure. ccache collapses all of it to a
# hash lookup. A full rebuild went 207s -> ~25s warm on an 8-core laptop.
# Silently skipped when ccache isn't installed.
option(MAYA_CCACHE "Compile through ccache when it's installed." ON)
if(MAYA_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(MAYA_CCACHE_BIN NAMES ccache sccache)
    if(MAYA_CCACHE_BIN)
        set(CMAKE_C_COMPILER_LAUNCHER   "${MAYA_CCACHE_BIN}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${MAYA_CCACHE_BIN}")
        message(STATUS "maya: compiling through ${MAYA_CCACHE_BIN}")
    endif()
endif()

# ── Fast test iteration ───────────────────────────────────────────────────
# The production `maya` library compiles every TU at -O3 with LTO + loop
# unrolling + Graphite polyhedral passes (see MAYA_LINUX_CXX_FLAGS). Great for
# the shipped binary, terrible for the test edit-build-run loop: any header
# edit recompiles the whole library at -O3, and every test then LTO-links
# (whole-program optimization) against it.
#
# With MAYA_FAST_TESTS=ON the tests link a SEPARATE `maya_test` library built
# at -O0 with NO LTO and NO heavy flags, and test TUs compile at -O0 too. A
# header edit recompiles only the touched object at -O0 and links instantly.
# The production `maya` target and its flags are completely untouched, so the
# portable-release path is unaffected. Default ON whenever tests are enabled.
option(MAYA_FAST_TESTS "Build tests against a non-LTO, -O0 maya library for fast iteration." ON)

# ── Heavy optimization gate ────────────────────────────────────────────────
# LTO + -O3 + Graphite/unroll/vectorize are for the SHIPPED binary. In a
# Debug build they are pure tax: any maya source or header edit forces a
# whole-library -O3 recompile AND a whole-program LTO relink of everything
# downstream (the agentty binary), turning a one-line change into a multi-
# minute wait. The `debug` preset promises "no LTO, ~1.7s incremental" — this
# is what makes that true. Heavy opt is ON for every build type EXCEPT Debug;
# a caller can still force it either way with -DMAYA_HEAVY_OPT=ON/OFF.
if(NOT DEFINED MAYA_HEAVY_OPT)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(MAYA_HEAVY_OPT OFF)
    else()
        set(MAYA_HEAVY_OPT ON)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND MAYA_HEAVY_OPT)
    # LTO across all translation units — lets GCC inline across .cpp boundaries,
    # devirtualize, and eliminate dead code globally. Respect a caller-supplied
    # value (e.g. distro packagers pass -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
    # so the installed libmaya.a contains real object code, not LTO bytecode
    # tied to the producing GCC version).
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()

    # -march=native asks the COMPILER to inspect the CPU it is running on.
    # When cross-compiling that host is the wrong machine entirely: a Termux
    # package built on an x86_64 CI box for aarch64 would either fail
    # outright or bake x86 tuning into an ARM target. CMAKE_CROSSCOMPILING is
    # set by every toolchain file, so honour it regardless of what the caller
    # asked for — there is no sane reading of "native" across an arch gap.
    if(MAYA_NATIVE_TUNING AND NOT CMAKE_CROSSCOMPILING)
        set(MAYA_ARCH_FLAGS -march=native -mtune=native)
    else()
        set(MAYA_ARCH_FLAGS)   # rely on the compiler's default arch baseline
    endif()

    # Shared flags for library and examples
    set(MAYA_LINUX_CXX_FLAGS
        -O3
        ${MAYA_ARCH_FLAGS}           # native tuning gated on MAYA_NATIVE_TUNING
        -funroll-loops               # unroll small loops
        -ftree-vectorize             # auto-vectorize where SIMD fits
        -fprefetch-loop-arrays       # insert prefetch for sequential access
        -fomit-frame-pointer         # free up RBP as GPR
        -fno-plt                     # direct calls through GOT, skip PLT indirection
        -fno-semantic-interposition  # assume symbols won't be interposed at runtime
        -fvisibility=hidden          # hide all symbols by default, reduce PLT/GOT
        -fvisibility-inlines-hidden  # hide inline function bodies
        -ffunction-sections          # each function in its own section for --gc-sections
        -fdata-sections              # each global in its own section
        -fmerge-all-constants        # merge identical constants across TUs
        -fdevirtualize-speculatively # speculative devirt for indirect calls
        -fipa-pta                    # interprocedural pointer analysis
        -fgraphite-identity          # enable Graphite loop optimizations
        -floop-nest-optimize         # Graphite polyhedral loop nest optimization
        -ftree-loop-distribution     # distribute loops for better vectorization
        -fno-math-errno              # don't set errno on math functions
        -fno-trapping-math           # assume no FP traps
        -fno-stack-protector         # no stack canaries — TUI app, not a network daemon
    )

    set(MAYA_LINUX_LINK_FLAGS
        -Wl,--gc-sections            # discard unreferenced sections
        -Wl,--as-needed              # only link actually-used libraries
        -Wl,-O2                      # linker optimization level
        -Wl,--hash-style=gnu         # faster symbol lookup
        -Wl,--sort-common            # sort common symbols by alignment
        -Wl,-z,now                   # resolve all symbols at load time (RELRO)
        -Wl,-z,relro                 # read-only relocations
    )
elseif(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND MAYA_HEAVY_OPT)
    # macOS + GCC (Homebrew g++). Same intent as the Linux block — wring
    # the rendering hot paths — but Mach-O, not ELF, so the flag set is the
    # subset that's meaningful here. NO LTO (Apple ld can't consume GCC LTO
    # IR; agentty's top-level CMake already forces IPO off for this combo).
    #
    # No -march/-mtune=native: Homebrew GCC on Apple Silicon resolves
    # `native` to `apple-m1`, which its aarch64 backend doesn't accept as an
    # -march value ("unknown value 'apple-m1'"), so it hard-errors the
    # compile. We fall back to GCC's armv8-a baseline — which still has NEON,
    # so -ftree-vectorize keeps emitting SIMD; we just forgo CPU-specific
    # instruction scheduling. (This is a GCC/Apple-Silicon gap, not a choice.)

    # Excluded vs the Linux set, on purpose:
    #   -fno-plt / -fno-semantic-interposition — ELF symbol-interposition
    #     concepts with no Mach-O meaning (GCC warns or silently ignores).
    #   -fgraphite-identity / -floop-nest-optimize — polyhedral passes are
    #     the least-exercised on macOS GCC; the marginal gain isn't worth
    #     the correctness risk on a TUI's render loop. Kept Linux-only.
    set(MAYA_MACOS_CXX_FLAGS
        -O3
        -funroll-loops
        -ftree-vectorize             # auto-vectorize (NEON on arm64)
        -fprefetch-loop-arrays
        -fomit-frame-pointer
        -fvisibility=hidden          # proven safe on the Linux static-link path
        -fvisibility-inlines-hidden
        -ffunction-sections          # pairs with -dead_strip below
        -fdata-sections
        -fmerge-all-constants
        -fdevirtualize-speculatively
        -fipa-pta                    # interprocedural pointer analysis
        -ftree-loop-distribution
        -fno-math-errno
        -fno-trapping-math
        -fno-stack-protector         # TUI app, not a network daemon
    )

    # Apple's ld doesn't speak GNU-ld flags (--gc-sections / --as-needed /
    # -z now / --hash-style all error out). The one equivalent that matters
    # for the -ffunction-sections/-fdata-sections pairing is -dead_strip.
    set(MAYA_MACOS_LINK_FLAGS
        -Wl,-dead_strip              # discard unreferenced sections (≈ --gc-sections)
    )
endif()

set(MAYA_LIB_SOURCES
    src/core/executor.cpp
    src/device/create.cpp
    src/device/input.cpp
    src/device/lifetime.cpp
    src/device/render.cpp
    src/device/render_inline.cpp
    src/device/render_fullscreen.cpp
    src/device/render_grid.cpp
    src/device/host_effects.cpp
    src/device/events.cpp
    src/render/print.cpp
    src/element/builder.cpp
    src/element/text.cpp
    src/terminal/ansi.cpp
    src/terminal/tmux.cpp
    src/terminal/input.cpp
    src/terminal/writer.cpp
    src/layout/yoga.cpp
    src/render/canvas.cpp
    src/render/renderer.cpp
    src/render/layout_build.cpp
    src/render/paint_border.cpp
    src/render/paint_box.cpp
    src/render/paint_text.cpp
    src/render/paint_list.cpp
    src/render/paint_component.cpp
    src/render/diff.cpp
    src/render/serialize.cpp
    src/render/inline_state.cpp
    src/render/compose_inline.cpp
    src/render/grid_emit.cpp
    src/render/frame_bytes.cpp
    src/render/inline_frame.cpp
    src/render/canvas_witness.cpp
    src/render/scrollback_ledger.cpp
    src/widget/panel.cpp
    # markdown widget — three modules: parser / render / streaming
    # (component layout: src/widget/markdown/LAYOUT.md)
    src/widget/markdown/parser/parser_inline.cpp
    src/widget/markdown/parser/parser_block.cpp
    src/widget/markdown/parser/text_transform.cpp
    # CommonMark engine — spec-faithful two-phase core
    src/widget/markdown/engine/cm_block.cpp
    src/widget/markdown/engine/cm_inline.cpp
    src/widget/markdown/render/render_inline.cpp
    src/widget/markdown/render/render_block.cpp
    src/widget/markdown/render/syntax_lang.cpp
    src/widget/markdown/render/syntax_highlight.cpp
    src/widget/markdown/render/highlight.cpp
    # streaming module — one concern per TU
    src/widget/markdown/streaming/markdown_memo.cpp
    src/widget/markdown/streaming/boundary.cpp
    src/widget/markdown/streaming/commit.cpp
    src/widget/markdown/streaming/render_tail.cpp
    src/widget/markdown/streaming/build.cpp
    src/widget/markdown/streaming/reveal_fx.cpp
    src/widget/markdown/streaming/folding.cpp
    src/widget/markdown/streaming/async.cpp

    src/widget/html/tags.cpp
    src/widget/html/tokenizer.cpp
    src/widget/html/parser.cpp
    src/widget/html/css.cpp
    src/widget/html/render.cpp
)

add_library(maya ${MAYA_LIB_SOURCES})
# maya uses no C++20/23 modules (no `export module` / `import` anywhere). With
# CMAKE_CXX_STANDARD 26 and CMake's ninja generator, module dependency scanning
# is enabled by default — which here is pure overhead AND incorrect: the html/*
# TUs' .ddi scan kept them perpetually dirty and reordered their compile to
# AFTER the libmaya.a archive link, so a parallel build could archive stale
# objects or read the archive mid-write (corrupt member -> link failures /
# random std::system_error at runtime). Turning scanning off makes the build
# deterministic and order-correct.
set_target_properties(maya PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(maya PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Namespaced ALIAS so consumers link the canonical `maya::maya` target: a `::`
# name is guaranteed to be a real target (a bare `maya` could silently be a
# system lib), so a rename/removal fails at configure with a clear error.
# Previously agentty synthesised this alias itself as a workaround; maya now
# owns it, for every consumer.
add_library(maya::maya ALIAS maya)

# ── Unicode width table regeneration (manual target) ─────────────────────
# `cmake --build build --target maya_unicode_regen` re-runs the generator
# script over the pinned UCD files in maya/data/ and overwrites
# include/maya/text/unicode_width_table.hpp. The generated header is
# checked in, so the regular build never needs Python — this target is
# only for bumping to a new Unicode revision (drop newer .txt files into
# maya/data/, then run this target, then commit the new .hpp).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(maya_unicode_regen
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_unicode_width.py
        # The script writes the header directly. It used to emit a .hpp.tmp
        # that this target copied into place, but the script's own OUT path
        # kept the .tmp suffix — so running the SCRIPT by hand (the documented
        # way, per the header's own banner) wrote a scratch file and left the
        # real header untouched. Regenerating appeared to be a no-op, which is
        # part of why a wrong table survived so long. One writer, one path.
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        # The generated header is checked into the source tree. Do not list it
        # as a BYPRODUCT: CMake's clean target deletes byproducts, which made a
        # clean build remove the required header before compiling Maya.
        COMMENT "Regenerating maya/include/maya/text/unicode_width_table.hpp from pinned UCD"
        VERBATIM
    )
endif()
if(MSVC)
    # CMake has no /std:c++26 for MSVC yet. Publicly require C++23 and
    # opt PRIVATEly into /std:c++latest — consumers linking maya stay
    # on C++23 but maya's own TUs get the newer library surface.
    target_compile_features(maya PUBLIC cxx_std_23)
    # Force-include the platform detect header so __builtin_unreachable /
    # __builtin_expect polyfills are visible to every maya TU AND every
    # downstream consumer (propagated via PUBLIC /FI).
    target_compile_options(maya PUBLIC
        /FI${CMAKE_CURRENT_SOURCE_DIR}/include/maya/platform/detect.hpp
    )
else()
    # Publicly require C++26 where the compiler advertises it, else fall back
    # to C++23. maya uses no C++26-only LIBRARY facility, so C++23 builds the
    # whole target cleanly. This requirement is PUBLIC — it propagates to
    # every consumer (agentty), so gating it here is what actually lets an
    # older Termux clang (< 18, i.e. no cxx_std_26) compile the tree.
    if("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        target_compile_features(maya PUBLIC cxx_std_26)
    else()
        target_compile_features(maya PUBLIC cxx_std_23)
    endif()
endif()

if(DEFINED MAYA_LINUX_CXX_FLAGS)
    target_compile_options(maya PRIVATE ${MAYA_LINUX_CXX_FLAGS})
    target_link_options(maya PRIVATE ${MAYA_LINUX_LINK_FLAGS})
elseif(DEFINED MAYA_MACOS_CXX_FLAGS)
    target_compile_options(maya PRIVATE ${MAYA_MACOS_CXX_FLAGS})
    target_link_options(maya PRIVATE ${MAYA_MACOS_LINK_FLAGS})
else()
    if(MSVC)
        # These four are not "how maya is built", they are what it takes to
        # PARSE maya's public headers at all, so consumers need them too:
        #
        #   /utf-8            the headers are UTF-8 and contain char32_t box
        #                     glyphs (U'─' in element/builder.hpp). Without
        #                     this MSVC reads them as the system codepage and
        #                     a single glyph becomes a multi-byte character
        #                     constant — C2015, "too many characters in
        #                     constant".
        #   /std:c++latest    maya::app needs C++26; CMake has no cxx_std_26
        #                     for MSVC yet, so the flag can't come from
        #                     target_compile_features.
        #   /Zc:preprocessor  the conforming preprocessor the variadic macros
        #                     in the DSL assume.
        #   /Zc:__cplusplus   without it __cplusplus reports 199711L and the
        #                     headers' own version checks misfire.
        #
        # They were PRIVATE, so libmaya.lib built fine while every example
        # and test including the same headers failed — which is exactly what
        # the examples (windows-msvc) lane was reporting.
        target_compile_options(maya INTERFACE
            /utf-8
            /std:c++latest
            /Zc:preprocessor
            /Zc:__cplusplus
        )
    endif()

    target_compile_options(maya PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            $<$<BOOL:${MAYA_HEAVY_OPT}>:-O3>
            $<$<BOOL:${MAYA_NATIVE_TUNING}>:-march=native>
            $<$<BOOL:${MAYA_HEAVY_OPT}>:-funroll-loops>
            # Warnings. This code is SIMD- and pointer-heavy; -Wall -Wextra
            # is exactly the net that catches the class of bug (unaligned
            # intrinsics, sign/size mismatches) that has bitten this file
            # before. Opt into -Werror via -DMAYA_WERROR=ON in CI only — the
            # default keeps warnings visible without breaking dev builds.
            -Wall
            -Wextra
            # Partial designated-init (`TextElement{.content=…}`) leaves cache
            # fields (cells / cached_lines / kids / cached_render) default-
            # initialized on purpose; unused overload params, dead locals
            # behind #if'd paths, and default-fallthrough switches are all
            # intentional. These are benign under Clang's stricter -Wextra
            # net — silence them so a dev build is warning-clean. CI still
            # gets the full net via -DMAYA_WERROR=ON (which does NOT inherit
            # these, since they're appended after the WERROR generator expr
            # only when Werror is off would be ideal; here we keep them on
            # both, treating the class as known-benign project-wide).
            -Wno-missing-field-initializers
            -Wno-unused-parameter
            -Wno-unused-variable
            -Wno-unused-function
            -Wno-unused-but-set-variable
            -Wno-switch
            $<$<CXX_COMPILER_ID:Clang>:-Wno-missing-designated-field-initializers>
            $<$<CXX_COMPILER_ID:Clang>:-Wno-unused-private-field>
            $<$<BOOL:${MAYA_WERROR}>:-Werror>
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /utf-8
            /std:c++latest
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
            /Zc:inline
            /Zc:throwingNew
            /EHsc
            /bigobj
            /MP
            /W4                # match -Wall -Wextra
            $<$<BOOL:${MAYA_WERROR}>:/WX>
            /wd4100
            /wd4127
            /wd4324
            /wd4244  # conversion, possible loss of data
            /wd4267  # size_t -> int
        >
        # ── MSVC Release-only aggressive optimization ─────────────────
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:
            /O2
            /Ob3               # aggressive inlining
            /Oi                # intrinsics
            /Ot                # favor speed over size
            /Oy                # omit frame pointer
            /GL                # whole-program optimization
            /GF                # string pooling
            /Gy                # function-level linking
            /Gw                # data-level linking
            /GS-               # disable stack cookies (TUI, not a daemon)
            /fp:fast           # relax FP — fine for layout math
            /arch:AVX2         # baseline AVX2/BMI2/FMA (consistent with SIMD caps)
            /Qpar              # auto-parallelizer for hot loops
        >
    )
    target_link_options(maya PRIVATE
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:
            /LTCG
            /OPT:REF
            /OPT:ICF=3
            /INCREMENTAL:NO
            /DEBUG:NONE
        >
    )
    if(MSVC)
        target_compile_definitions(maya PUBLIC
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:NDEBUG>
        )
    endif()
endif()

# ── maya::app: maya on jaal ─────────────────────────────────────────────
# jaal (third_party/jaal) is the runtime; maya is the view layer and the
# terminal device. The `maya` library knows nothing of jaal; `maya::app`
# (maya + jaal + <maya/host/run.hpp>) is what a program links. jaal needs C++26.
# docs/internals/design.md.
option(MAYA_BUILD_APP "Build maya::app (needs C++26 and third_party/jaal)" ON)
if(MAYA_BUILD_APP)
    # A CMake older than the compiler (CMake 3.31 with AppleClang 21) doesn't
    # LIST cxx_std_26 even though the compiler takes -std=c++2c, so asking
    # CMake is the wrong test: probe the flag itself. jaal's own build does
    # the same (third_party/jaal/CMakeLists.txt, JAAL_STD_FLAG).
    set(MAYA_APP_STD_FLAG "")
    if(NOT MSVC AND NOT "cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-std=c++2c" MAYA_HAS_CXX2C)
        if(NOT MAYA_HAS_CXX2C)
            message(FATAL_ERROR "maya::app needs C++26; this compiler "
                                "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) "
                                "accepts neither cxx_std_26 nor -std=c++2c. Configure with -DMAYA_BUILD_APP=OFF.")
        endif()
        set(MAYA_APP_STD_FLAG "-std=c++2c")
    endif()
    if(NOT EXISTS ${PROJECT_SOURCE_DIR}/third_party/jaal/CMakeLists.txt)
        message(FATAL_ERROR "maya::app: third_party/jaal is empty. Run: "
                            "git submodule update --init third_party/jaal")
    endif()
    set(JAAL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/jaal EXCLUDE_FROM_ALL)
    add_library(maya_app INTERFACE)
    add_library(maya::app ALIAS maya_app)
    set_target_properties(maya_app PROPERTIES EXPORT_NAME app)
    target_link_libraries(maya_app INTERFACE maya jaal::jaal)
    # The runtime alone (<maya/runtime.hpp>: guarded, published, pools, the
    # platform layer) with no terminal library. For a small target, a race
    # test say, that must not pull in the whole of maya.
    add_library(maya_runtime INTERFACE)
    add_library(maya::runtime ALIAS maya_runtime)
    target_include_directories(maya_runtime INTERFACE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(maya_runtime INTERFACE jaal::jaal)
    if(MAYA_APP_STD_FLAG)
        target_compile_options(maya_app INTERFACE ${MAYA_APP_STD_FLAG})
        target_compile_options(jaal PRIVATE ${MAYA_APP_STD_FLAG})
    endif()
endif()

option(MAYA_BUILD_EXAMPLES "Build example programs (needs maya::app)" ON)
# The 55 examples are demos, not products: at -O3 with unrolling they were
# the single biggest slice of a full build (and each is rebuilt whenever a
# header moves). Two knobs:
#   MAYA_FAST_EXAMPLES  compile them at -O1, no unroll/native (default ON).
#                       The pixel demos still hit their frame rate; turn it
#                       OFF to measure or ship one.
#   MAYA_EXAMPLES_ALL   put them in the default target (default OFF). Off,
#                       `ninja` builds the library and tests, and you ask for
#                       the demo you want: `ninja maya_fps`. The smoke run
#                       asks for all of them (`ninja maya_examples`).
option(MAYA_FAST_EXAMPLES "Compile examples at -O1 instead of -O3." ON)
option(MAYA_EXAMPLES_ALL  "Build every example as part of the default target." OFF)
if(MAYA_BUILD_EXAMPLES AND MAYA_BUILD_APP)
    file(GLOB MAYA_EXAMPLE_SOURCES CONFIGURE_DEPENDS examples/*.cpp)
    # A few examples demonstrate things that only exist on POSIX — running a
    # child on the real tty (sys/wait.h, std::system) and isatty(). They are
    # about the host OS, not about maya, so there is nothing to port: skip
    # them on MSVC instead of letting the glob hand the compiler a
    # <sys/wait.h> it will never find (C1083).
    if(MSVC)
        foreach(posix_only terminal_fx markup)
            list(FILTER MAYA_EXAMPLE_SOURCES EXCLUDE REGEX "/${posix_only}\\.cpp$")
        endforeach()
    endif()
    add_custom_target(maya_examples COMMENT "every example program")
    foreach(src ${MAYA_EXAMPLE_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        set(tgt maya_${name})
        if(MAYA_EXAMPLES_ALL)
            add_executable(${tgt} ${src})
        else()
            add_executable(${tgt} EXCLUDE_FROM_ALL ${src})
        endif()
        add_dependencies(maya_examples ${tgt})
        target_link_libraries(${tgt} PRIVATE maya_app)
        if(MAYA_FAST_EXAMPLES)
            # -O1, no unrolling, and no LTO *for this example's own object*:
            # the demo is not what anyone profiles, and skipping IPO here is
            # most of the build-time saving.
            #
            # The -fno-lto goes on the COMPILE only. Putting it on the LINK
            # too is what broke every example in a default Release build:
            # maya itself is built with CMAKE_INTERPROCEDURAL_OPTIMIZATION ON
            # (see the top of this file), so libmaya.a holds fat LTO objects,
            # and -fno-lto at link tells the driver not to load the LTO
            # plugin. ld then cannot read those members and reports the whole
            # library as missing:
            #
            #     libmaya.a(create.cpp.o): plugin needed to handle lto object
            #     undefined reference to `maya::detail::Device::read_events()'
            #
            # -- 77 undefined symbols for a library sitting right there on the
            # link line, in the correct order. INTERPROCEDURAL_OPTIMIZATION
            # OFF below already keeps the example's own object out of LTO;
            # the link must stay plugin-capable to consume maya's.
            target_compile_options(${tgt} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:-O1 -g0 -fno-lto>)
            set_target_properties(${tgt} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
        elseif(DEFINED MAYA_LINUX_CXX_FLAGS)
            target_compile_options(${tgt} PRIVATE ${MAYA_LINUX_CXX_FLAGS})
            target_link_options(${tgt} PRIVATE ${MAYA_LINUX_LINK_FLAGS})
        else()
            target_compile_options(${tgt} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:
                    -O3
                    $<$<BOOL:${MAYA_NATIVE_TUNING}>:-march=native>
                    -funroll-loops
                >
                $<$<CXX_COMPILER_ID:MSVC>:/O2 /GL>
            )
        endif()
    endforeach()
endif()

# Default ON only when maya is the top-level project (standalone). Embedded via
# add_subdirectory (e.g. by agentty) it defaults OFF so the parent never pulls
# maya's tests into its build — each repo runs its own tests. (maya requires
# CMake 3.28, so PROJECT_IS_TOP_LEVEL is always defined.)
option(MAYA_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
if(MAYA_BUILD_TESTS)
    enable_testing()

    # ── Which library do tests link against? ──────────────────────────────
    # MAYA_FAST_TESTS builds a parallel `maya_test` library from the SAME
    # sources but with LTO disabled and -O0 — so a header edit recompiles
    # only the touched .o at -O0 and the test links in milliseconds instead
    # of waiting on whole-program LTO. Production `maya` is left alone.
    if(MAYA_FAST_TESTS AND NOT MSVC)
        add_library(maya_test STATIC EXCLUDE_FROM_ALL ${MAYA_LIB_SOURCES})
        set_target_properties(maya_test PROPERTIES CXX_SCAN_FOR_MODULES OFF)
        target_include_directories(maya_test PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
        if("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
            target_compile_features(maya_test PUBLIC cxx_std_26)
        else()
            target_compile_features(maya_test PUBLIC cxx_std_23)
        endif()
        # No LTO, no -O3, no unroll/Graphite. Just enough to be correct + fast
        # to compile. -g0 keeps objects small; debugging a failing test is
        # better done by rebuilding the one TU with -Og if needed.
        set_target_properties(maya_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
        # -O1 (NOT -O0): a handful of tests assert per-frame render latency
        # (test_markdown's streaming cases HUNG at -O0). -O1 has no LTO, no
        # loop unrolling, no Graphite polyhedral passes — so it compiles in a
        # fraction of the production -O3+LTO time — yet runs fast enough for
        # those gates. Native tuning is intentionally omitted (portability +
        # compile speed). Test TUs themselves stay at -O0 (see MAYA_TEST_OPT).
        target_compile_options(maya_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-O1 -g0 -fno-lto>)
        target_link_options(maya_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-lto>)
        set(MAYA_TEST_LIB maya_test)
        set(MAYA_TEST_OPT -O0 -g0 -fno-lto)
    else()
        set(MAYA_TEST_LIB maya)
        set(MAYA_TEST_OPT -O2)
        if(MAYA_NATIVE_TUNING)
            list(APPEND MAYA_TEST_OPT -march=native)
        endif()
    endif()

    # doctest — single maya_tests binary. The pure unit tests are doctest
    # TEST_CASEs auto-registered into one executable (linking maya once) via
    # doctest_discover_tests, instead of one executable per file.
    include(FetchContent)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE
    )
    set(DOCTEST_WITH_TESTS OFF CACHE INTERNAL "")
    set(DOCTEST_NO_INSTALL ON CACHE INTERNAL "")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")
    FetchContent_MakeAvailable(doctest)
    unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)

    set(MAYA_CONSOLIDATED_TESTS
        tests/test_render.cpp
        tests/test_render_scaling.cpp
        tests/test_types.cpp
        tests/test_color.cpp
        tests/test_color_resolution.cpp
        tests/test_markdown_palette.cpp
        tests/test_style.cpp
        tests/test_theme_discipline.cpp
        tests/test_blend_safety.cpp
        tests/test_canvas.cpp
        tests/test_diff.cpp
        tests/test_links.cpp
        tests/test_diff_fidelity.cpp
        tests/test_writer_residue.cpp
        tests/test_grid_emit.cpp
        tests/test_columns.cpp
        tests/test_render_sweep.cpp
        tests/test_framebuffer.cpp
        tests/test_input.cpp
        tests/test_signal.cpp
        tests/test_animation.cpp
        tests/test_motion.cpp
        tests/test_overlay_layer.cpp
        tests/test_scroll_region.cpp
        tests/test_layout.cpp
        tests/test_border.cpp
        tests/test_simd.cpp
        tests/test_ansi.cpp
        tests/test_writer.cpp
        tests/test_coalesce_never_drops.cpp
        tests/test_color_tier_consistency.cpp
        tests/test_widgets.cpp
        tests/tab_strip_test.cpp
        tests/stat_sheet_test.cpp
        tests/test_spacing_invariants.cpp
        tests/test_promoted.cpp
        tests/test_math.cpp
        tests/test_streaming_perf.cpp
        tests/wire_bytes_bench.cpp
        tests/test_wire_coalesce.cpp
        tests/test_streaming_window.cpp
        tests/test_tracked.cpp
        tests/test_unicode_width.cpp
        tests/test_stream_sink.cpp
        tests/test_scroll.cpp
        tests/test_wire_row.cpp
        tests/test_commonmark_spec.cpp
        tests/test_html.cpp
        tests/test_highlight.cpp
        tests/test_highlight_resume.cpp
    )
    add_executable(maya_tests tests/test_main.cpp ${MAYA_CONSOLIDATED_TESTS})
    target_link_libraries(maya_tests PRIVATE ${MAYA_TEST_LIB} doctest::doctest)
    target_compile_definitions(maya_tests PRIVATE MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # The Themed gate in test_theme_discipline.cpp is checked by COMPILING
    # tiny probe TUs — "does this fail to compile" is not a question a
    # static_assert can answer, because the gate is a consteval throw and a
    # throw is well-formed until evaluated. That needs the compiler's path,
    # and a flag saying whether it speaks -fsyntax-only (GCC/Clang) so the
    # probes can be skipped rather than mis-run on MSVC.
    target_compile_definitions(maya_tests PRIVATE
        MAYA_CXX_COMPILER="${CMAKE_CXX_COMPILER}")
    if(NOT MSVC)
        target_compile_definitions(maya_tests PRIVATE MAYA_CXX_GNULIKE=1)
        # The probes run the compiler by hand: on macOS it needs the same
        # SDK the build uses, or <cstddef> isn't found and every probe
        # "refuses". Pass the sysroot through when there is one.
        if(CMAKE_OSX_SYSROOT)
            target_compile_definitions(maya_tests PRIVATE MAYA_CXX_SYSROOT="${CMAKE_OSX_SYSROOT}")
        endif()
    endif()
    target_compile_options(maya_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${MAYA_TEST_OPT}>)
    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
    doctest_discover_tests(maya_tests)
    # Timing-budget cases (render-scaling ratios, warm-frame budgets) measure
    # wall time and flake under parallel ctest load — six test binaries
    # fighting for cores can triple a 100µs baseline. Label them `perf` so
    # `ctest -LE perf` (and CI's parallel lane) skips them; a serial
    # `ctest -L perf` still gates them.
    #
    # doctest tests are discovered at BUILD time (the discovery file is a
    # TEST_INCLUDE_FILES entry), so a configure-time set_tests_properties
    # sees no such test and silently no-ops. Append our OWN include file
    # after the discovery one — include files run in order, so by the time
    # ours executes the discovered tests exist.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/maya_perf_labels.cmake" [=[
# Runs AFTER the doctest discovery include (see TEST_INCLUDE_FILES order),
# so the discovered tests already exist. Unconditional: if a case is
# renamed, this errors loudly instead of silently un-labelling it.
set_tests_properties(
    [==[long session with virtualization]==]
    [==[warm frame time at high turn count]==]
    PROPERTIES LABELS perf)
]=])
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/maya_perf_labels.cmake")

    # These keep their own int main() (multi-phase programs, the streaming
    # reveal/lag probes shared verbatim with agentty's suite, and the
    # benchmark), so they can't be doctest cases inside maya_tests. They do NOT
    # each need their own link though: all 8 are bundled into ONE binary,
    # maya_standalone_tests, argv-dispatched so each still runs in its OWN
    # process per ctest entry — isolation unchanged, 8 links -> 1.
    set(MAYA_TEST_SOURCES
        tests/test_markdown.cpp
        tests/test_streaming_cache.cpp
        tests/reveal_pacing_test.cpp
        tests/reveal_resume_test.cpp
        tests/reveal_smoothness_probe.cpp
        tests/reveal_cost_probe.cpp
        tests/reveal_settled_rows_test.cpp
        tests/stream_md_lag_test.cpp
        tests/stream_async_freeze_test.cpp
        tests/bench_thread_scaling.cpp
    )

    add_executable(maya_standalone_tests
        tests/maya_standalone_tests_main.cpp ${MAYA_TEST_SOURCES})
    target_link_libraries(maya_standalone_tests PRIVATE ${MAYA_TEST_LIB})
    target_include_directories(maya_standalone_tests PRIVATE tests)
    target_compile_definitions(maya_standalone_tests PRIVATE
        MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(maya_standalone_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${MAYA_TEST_OPT}>)
    foreach(src ${MAYA_TEST_SOURCES})
        get_filename_component(tgt ${src} NAME_WE)
        # Rename this TU's main() so the dispatcher can call it; each test still
        # runs in its OWN process via `maya_standalone_tests <name>`.
        set_source_files_properties(${src} PROPERTIES
            COMPILE_DEFINITIONS "main=${tgt}_main")
        add_test(NAME ${tgt} COMMAND maya_standalone_tests ${tgt})
        set_tests_properties(${tgt} PROPERTIES TIMEOUT 120)
    endforeach()

    # POSIX-only tests. These mock a real fd (a pipe / pty), so they need
    # unistd/termios and can't build under MSVC.
    #   • test_inline_frame — routes the Witness Chain through a pipe Writer;
    #     builds on Linux + macOS.
    #   • test_composer_caret_shrink — wire-level repro for the ghost-caret
    #     bug: asserts the painted caret survives a composer shrink exactly
    #     once AND the hardware cursor ends every frame parked at col 0 of
    #     the last wire row, hidden, with the hide re-asserted outside the
    #     ?2026 sync block (pipe Writer + compact VT emulator).
    #   • test_terminal_restore — PTY-driven; verifies maya puts the tty back
    #     (cooked termios + restore escapes) on every exit route, including
    #     SIGINT. Linux-only: it drives POSIX signal/pty surface the macOS
    #     toolchain doesn't expose cleanly from this TU, and the restore
    #     behaviour it guards is platform-identical.
    if(UNIX)
        # The layering rule, as a test: nothing outside include/maya/host/ may
        # depend on jaal. Reads the tree, so it needs no build.
        add_test(NAME seam
                 COMMAND ${CMAKE_COMMAND} -E env sh ${PROJECT_SOURCE_DIR}/tests/seam.sh)
        set_tests_properties(seam PROPERTIES TIMEOUT 60)

        # The generated width table still matches data/ + the generator.
        # unicode_width_table.hpp is the kind of file review skims, so a bad
        # entry can outlive releases (agentty#55: Korean measured 4 columns
        # per syllable instead of 2). Regenerates into a temp dir and diffs;
        # reads the tree, so it needs no build.
        add_test(NAME unicode_table_fresh
                 COMMAND ${CMAKE_COMMAND} -E env sh
                         ${PROJECT_SOURCE_DIR}/tests/unicode_table_fresh.sh)
        set_tests_properties(unicode_table_fresh PROPERTIES TIMEOUT 120)

        # End-to-end key-to-first-byte latency: PTY write -> device drain
        # -> parse -> on(model, KeyEvent) -> Msg -> update -> view -> Element -> the
        # renderer -> PTY back. The microbenches in bench/ hold each stage
        # tight; this one catches a regression that only shows up when the
        # whole pipeline runs together (a stray reconcile per key, a slow
        # renderer walk, a Sub firing too eagerly).
        #
        # Baseline on a workstation: median ~0.2 ms, p95 ~0.4 ms. Thresholds
        # carry a lot of headroom (3 / 5 ms) so a shared CI runner does not
        # trip; if either breaches, something got dramatically slower.
        # Needs a PTY, so POSIX only. maya_counter must be built.
        if(TARGET maya_counter AND Python3_Interpreter_FOUND)
            add_test(NAME input_latency_gate
                     COMMAND ${Python3_EXECUTABLE}
                             ${PROJECT_SOURCE_DIR}/tests/input_latency_gate.py
                             $<TARGET_FILE:maya_counter>)
            set_tests_properties(input_latency_gate PROPERTIES
                TIMEOUT 60
                LABELS "perf"
                RUN_SERIAL TRUE)  # wall-clock probe, do not race other tests
        endif()

        # ── Concurrency banlist ────────────────────────────────────
        # Raw threads / mutexes / atomics / thread_locals are allowed only
        # in files a human signed off on in tests/lint/allowlist.txt.
        #
        # This is jaal's own check pointed at maya. jaal runs it over ITS
        # tree and agentty over agentty's, which left maya — the layer in
        # between, and the one with the most raw primitives by far — as the
        # only unchecked tree in the chain. The render caches, the palette
        # publish and the markdown async worker all live here, so that was
        # the wrong place for the gap: seam.sh catches a `.detach()` and
        # nothing caught anything else.
        #
        # Every primitive in the allowlist today was already deliberate and
        # already correct. The point of the gate is that the NEXT one has to
        # be argued for instead of merely compiling.
        #
        # Two roots because the allowlist keys on paths relative to the root
        # it scans. ROOT must be absolute: it is a file(GLOB_RECURSE) prefix,
        # so a relative path silently matches zero files.
        set(_MAYA_BANLIST ${PROJECT_SOURCE_DIR}/third_party/jaal/tests/lint/banlist.cmake)
        # agentty → maya → jaal: maya names nothing above it, and only
        # host/ + runtime.hpp touch jaal. tests/lint/layering.cmake.
        add_test(NAME layering
                 COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
                         -P ${PROJECT_SOURCE_DIR}/tests/lint/layering.cmake)
        set_tests_properties(layering PROPERTIES TIMEOUT 120 LABELS "static")
        if(EXISTS ${_MAYA_BANLIST})
            add_test(NAME concurrency_banlist_src
                     COMMAND ${CMAKE_COMMAND}
                             -DROOT=${PROJECT_SOURCE_DIR}/src
                             -DALLOW=${PROJECT_SOURCE_DIR}/tests/lint/allowlist.txt
                             -P ${_MAYA_BANLIST})
            add_test(NAME concurrency_banlist_include
                     COMMAND ${CMAKE_COMMAND}
                             -DROOT=${PROJECT_SOURCE_DIR}/include/maya
                             -DALLOW=${PROJECT_SOURCE_DIR}/tests/lint/allow_include.txt
                             -P ${_MAYA_BANLIST})
            set_tests_properties(concurrency_banlist_src concurrency_banlist_include
                                 PROPERTIES TIMEOUT 120 LABELS "static")

            # The banlist only ever LOOKS UP the allowlist, so a grant whose
            # justification is gone is silently ignored rather than
            # reported. That rots one way: the exemption outlives the code,
            # and the next person to add a mutex to that file inherits a
            # pass nobody granted them. This fails the build on a dead or
            # stale entry, using the banlist's own matching rules.
            set(_MAYA_PRUNE ${PROJECT_SOURCE_DIR}/third_party/jaal/tests/lint/prune_allowlist.cmake)
            if(EXISTS ${_MAYA_PRUNE})
                add_test(NAME allowlist_tight_src
                         COMMAND ${CMAKE_COMMAND}
                                 -DROOT=${PROJECT_SOURCE_DIR}/src
                                 -DALLOW=${PROJECT_SOURCE_DIR}/tests/lint/allowlist.txt
                                 -P ${_MAYA_PRUNE})
                add_test(NAME allowlist_tight_include
                         COMMAND ${CMAKE_COMMAND}
                                 -DROOT=${PROJECT_SOURCE_DIR}/include/maya
                                 -DALLOW=${PROJECT_SOURCE_DIR}/tests/lint/allow_include.txt
                                 -P ${_MAYA_PRUNE})
                set_tests_properties(allowlist_tight_src allowlist_tight_include
                                     PROPERTIES TIMEOUT 120 LABELS "static")
            endif()
        endif()

        set(_maya_posix_tests test_inline_frame test_composer_caret_shrink
            test_tmux_capabilities test_fresh_oversized_scrollback)
        if(NOT APPLE)
            list(APPEND _maya_posix_tests test_terminal_restore)
        endif()
        foreach(tgt ${_maya_posix_tests})
            add_executable(${tgt} tests/${tgt}.cpp)
            target_link_libraries(${tgt} PRIVATE ${MAYA_TEST_LIB})
            target_compile_definitions(${tgt} PRIVATE
                MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
            target_compile_options(${tgt} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:${MAYA_TEST_OPT}>)
            add_test(NAME ${tgt} COMMAND ${tgt})
            set_tests_properties(${tgt} PROPERTIES TIMEOUT 120)
        endforeach()
        # forkpty() needs -lutil on Linux/glibc.
        if(NOT APPLE)
            target_link_libraries(test_terminal_restore PRIVATE util)
        endif()

        # test_host drives terminal_host against a fake device, so it needs
        # jaal (maya_app), unlike every other test here. It opens no terminal
        # and runs in microseconds — the scheduling logic that used to be
        # reachable only through a real pty.
        if(TARGET maya_app)
            add_executable(test_host tests/test_host.cpp)
            target_link_libraries(test_host PRIVATE ${MAYA_TEST_LIB} maya_app)
            target_compile_definitions(test_host PRIVATE
                MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
            target_compile_options(test_host PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:${MAYA_TEST_OPT}>)
            if(MAYA_APP_STD_FLAG)
                target_compile_options(test_host PRIVATE ${MAYA_APP_STD_FLAG})
            endif()
            add_test(NAME test_host COMMAND test_host)
            set_tests_properties(test_host PROPERTIES TIMEOUT 120)

            # host_pty_test drives host_probe in a real pty: keys, a timer that
            # comes and goes, SIGWINCH, quit. Needs pyte; skips without it.
            if(NOT WIN32)
                add_executable(host_probe tests/host_probe.cpp)
                target_link_libraries(host_probe PRIVATE maya_app)
                if(MAYA_APP_STD_FLAG)
                    target_compile_options(host_probe PRIVATE ${MAYA_APP_STD_FLAG})
                endif()
                if(Python3_Interpreter_FOUND)
                    add_test(NAME host_pty_test
                             COMMAND ${Python3_EXECUTABLE}
                                     ${PROJECT_SOURCE_DIR}/tests/host_pty_test.py
                                     $<TARGET_FILE:host_probe>)
                    set_tests_properties(host_pty_test PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
                                         SKIP_RETURN_CODE 77)
                    add_test(NAME ctrl_c_test
                             COMMAND ${Python3_EXECUTABLE}
                                     ${PROJECT_SOURCE_DIR}/tests/ctrl_c_test.py
                                     $<TARGET_FILE:host_probe>)
                    set_tests_properties(ctrl_c_test PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
                endif()
            endif()

            # Seam mistakes that must not compile (tests/compile_fail/seam.cpp).
            # Each MATCH is the diagnostic, so a case can't pass by failing for
            # the wrong reason. Case 0 is a control that must compile.
            set(_cf_std "-std=c++2c")
            if(MSVC)
                set(_cf_std "/std:c++latest")
            endif()
            set(_cf_inc "${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/third_party/jaal/include")
            set(_cf_cases
                "0|"
                "1|use SpecialKey::Tab"
                "2|use SpecialKey::Escape"
                "3|arrive as Ctrl\\+letter"
                "4|lists input the terminal never sends"
                "5|no overload takes an event this host sends"
                "6|lists a kind of input with no on\\(\\) for it"
                "7|on_key")
            if(NOT MSVC)
                foreach(_c IN LISTS _cf_cases)
                    string(REPLACE "|" ";" _parts "${_c}")
                    list(GET _parts 0 _n)
                    list(LENGTH _parts _len)
                    set(_m "")
                    if(_len GREATER 1)
                        list(GET _parts 1 _m)
                    endif()
                    set(_ok "")
                    if(_n EQUAL 0)
                        set(_ok 1)
                    endif()
                    add_test(NAME seam_compile_fail_${_n}
                             COMMAND ${CMAKE_COMMAND}
                                     -DCXX=${CMAKE_CXX_COMPILER} -DSTD=${_cf_std}
                                     "-DINC=${_cf_inc}"
                                     -DSRC=${PROJECT_SOURCE_DIR}/tests/compile_fail/seam.cpp
                                     -DCASE=${_n} "-DMATCH=${_m}" -DEXPECT_OK=${_ok}
                                     "-DSYSROOT=${CMAKE_OSX_SYSROOT}"
                                     -P ${PROJECT_SOURCE_DIR}/tests/compile_fail/run_case.cmake)
                    set_tests_properties(seam_compile_fail_${_n} PROPERTIES LABELS "static" TIMEOUT 120)
                endforeach()
            endif()
        endif()
    endif()
endif()

# ── Fuzzers (libFuzzer; Clang only) ────────────────────────────────────────
# Harnesses for the byte-parsing surfaces most exposed to adversarial input:
# the terminal input parser, the writer's safe-break splitter, the OSC-52
# base64 path, and the markdown engine. Build with Clang and a fuzzer
# sanitizer; run via CI (.github/workflows/ci.yml fuzz-smoke job) or a
# scheduled long run.
#
#   CC=clang CXX=clang++ cmake -B fuzzbuild -DMAYA_BUILD_FUZZERS=ON
#   cmake --build fuzzbuild -j
#   ./fuzzbuild/fuzz/fuzz_input -max_total_time=60
option(MAYA_BUILD_FUZZERS "Build libFuzzer harnesses (Clang only)" OFF)
if(MAYA_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MAYA_BUILD_FUZZERS requires Clang (libFuzzer).")
    endif()
    add_subdirectory(fuzz)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/maya DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS maya EXPORT maya-targets)
# maya::app ships with the jaal it was built against (the submodule), in
# maya's own export set: find_package(maya) is enough for an app.
if(TARGET maya_app)
    install(TARGETS maya_app jaal EXPORT maya-targets FILE_SET HEADERS)
endif()
install(EXPORT maya-targets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maya
    NAMESPACE maya::
)

# Generate maya-config.cmake so downstream projects can find_package(maya)
configure_package_config_file(
    cmake/maya-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/maya-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maya
)

# Generate maya-config-version.cmake for version compatibility
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/maya-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/maya-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/maya-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maya
)
