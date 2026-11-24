#pragma once
// maya::core::target — the platform this build is FOR, as maya and every
// project above it (agentty) asks for it.
//
// Not maya's own decision: jaal decides the platform once
// (jaal/meta/target.hpp), and this file only re-exports those answers under
// maya's prefix, so a project above maya never names jaal (agentty →
// maya → jaal, strictly) and still reads the same answer. Re-deriving them
// here from _WIN32 / __APPLE__ would be a second copy free to drift from
// the one the kernel was built against.
//
//   #if MAYA_OS_WINDOWS                         a preprocessor decision
//   if constexpr (maya::platform::os == ...)    everything that compiles
//                                               everywhere (prefer it)
//
// Every name is ALWAYS defined, 0 or 1, so `#if` (never `#ifdef`) reads it
// and a missing include fails the one_os lint instead of reading as 0.
//
// THE one file in maya and agentty that may test the compiler's raw target
// macros is jaal's; the one_os lint (tests/lint, run by both projects)
// holds every other file to these names.

#include <jaal/meta/target.hpp>

// ── the operating system ─────────────────────────────────────────────────
#define MAYA_OS_WINDOWS     JAAL_OS_WINDOWS
#define MAYA_OS_APPLE       JAAL_OS_APPLE
#define MAYA_OS_LINUX       JAAL_OS_LINUX
#define MAYA_OS_ANDROID     JAAL_OS_ANDROID
#define MAYA_OS_FREEBSD     JAAL_OS_FREEBSD
#define MAYA_OS_OPENBSD     JAAL_OS_OPENBSD
#define MAYA_OS_NETBSD      JAAL_OS_NETBSD
#define MAYA_OS_EMSCRIPTEN  JAAL_OS_EMSCRIPTEN
#define MAYA_OS_UNIX        JAAL_OS_UNIX        // __unix__: Linux, BSDs; NOT macOS
#define MAYA_OS_POSIX       JAAL_OS_POSIX       // not Windows

// ── the compiler ─────────────────────────────────────────────────────────
#define MAYA_CC_CLANG        JAAL_CC_CLANG
#define MAYA_CC_GCC          JAAL_CC_GCC
#define MAYA_CC_MSVC         JAAL_CC_MSVC        // cl, not clang-cl
#define MAYA_CC_MSVC_ABI     JAAL_CC_MSVC_ABI    // cl or clang-cl
#define MAYA_CC_APPLE_CLANG  JAAL_CC_APPLE_CLANG
#define MAYA_CC_GNU_EXT      JAAL_CC_GNU_EXT     // GCC or any clang
#define MAYA_CC_INT128       JAAL_CC_INT128

// ── the CPU and what the compiler may emit for it ────────────────────────
#define MAYA_CPU_ARM64    JAAL_CPU_ARM64
#define MAYA_CPU_X86_64   JAAL_CPU_X86_64
#define MAYA_CPU_X86_32   JAAL_CPU_X86_32
#define MAYA_CPU_X86      (JAAL_CPU_X86_64 || JAAL_CPU_X86_32)
#define MAYA_ISA_NEON     JAAL_ISA_NEON
#define MAYA_ISA_SSE2     JAAL_ISA_SSE2
#define MAYA_ISA_SSE41    JAAL_ISA_SSE41
#define MAYA_ISA_SSE42    JAAL_ISA_SSE42
#define MAYA_ISA_AVX2     JAAL_ISA_AVX2
#define MAYA_ISA_AVX512F  JAAL_ISA_AVX512F

namespace maya::platform {
using os_kind = ::jaal::platform::os_kind;
inline constexpr os_kind target_os = ::jaal::platform::os;
}  // namespace maya::platform
