#pragma once
// maya::platform::detect - Compile-time platform identity
//
// Every platform fact is a constexpr value, DERIVED from maya/core/target.hpp
// (which re-exports jaal's one decision of what the platform is). Nothing in
// this file reads a compiler's raw target macro; the compiler-portability
// spellings at the bottom (restrict, force-inline) ask MAYA_CC_* too.
// All other maya code branches on the constexpr values or uses concept
// dispatch.
//
// Usage:
//   if constexpr (platform::is_posix) { ... }        // compile-time branch
//   using T = std::conditional_t<platform::is_win32,  // type-level branch
//                                Win32Impl, PosixImpl>;

#include <cstdint>
#include <type_traits>

#include "../core/target.hpp"

namespace maya::platform {

// ============================================================================
// Operating system
// ============================================================================

enum class Os : uint8_t {
    Linux,
    MacOS,
    Windows,
    FreeBSD,
    OpenBSD,
    Unknown,
};

inline constexpr Os os =
#if MAYA_OS_LINUX
    Os::Linux;
#elif MAYA_OS_APPLE
    Os::MacOS;
#elif MAYA_OS_WINDOWS
    Os::Windows;
#elif MAYA_OS_FREEBSD
    Os::FreeBSD;
#elif MAYA_OS_OPENBSD
    Os::OpenBSD;
#else
    Os::Unknown;
#endif

// ============================================================================
// Architecture
// ============================================================================

enum class Arch : uint8_t {
    X86_64,
    Aarch64,
    Riscv64,
    Wasm32,
    Unknown,
};

inline constexpr Arch arch =
#if MAYA_CPU_X86_64
    Arch::X86_64;
#elif MAYA_CPU_ARM64
    Arch::Aarch64;
#elif defined(__riscv) && (__riscv_xlen == 64)
    Arch::Riscv64;
#elif defined(__wasm32__) || MAYA_OS_EMSCRIPTEN
    Arch::Wasm32;
#else
    Arch::Unknown;
#endif

// ============================================================================
// OS family — tag types for type-level dispatch
// ============================================================================

struct Posix {};
struct MacOS {};
struct Win32 {};

using OsFamily = std::conditional_t<os == Os::Windows, Win32,
                 std::conditional_t<os == Os::MacOS,   MacOS, Posix>>;

inline constexpr bool is_posix = std::is_same_v<OsFamily, Posix>;
inline constexpr bool is_macos = std::is_same_v<OsFamily, MacOS>;
inline constexpr bool is_win32 = std::is_same_v<OsFamily, Win32>;

// ============================================================================
// SIMD capability flags (compile-time)
// ============================================================================

struct SimdCaps {
    bool avx512f  : 1 = false;
    bool avx2     : 1 = false;
    bool sse2     : 1 = false;
    bool neon     : 1 = false;
    bool sve      : 1 = false;    // ARM SVE (future)
    bool rvv      : 1 = false;    // RISC-V V (future)
    bool crc32_hw : 1 = false;    // Hardware CRC32 (ARM or SSE4.2)
};

inline constexpr SimdCaps simd_caps = {
#if MAYA_ISA_AVX512F
    .avx512f = true,
#endif
#if MAYA_ISA_AVX2
    .avx2 = true,
#endif
#if MAYA_CPU_X86_64
    .sse2 = true,                  // x86-64 baseline
#endif
#if MAYA_CPU_ARM64 || MAYA_ISA_NEON
    .neon = true,                  // AArch64 baseline, ARMv7+NEON build
#endif
#if defined(__ARM_FEATURE_SVE)
    .sve = true,
#endif
#if defined(__riscv_v)
    .rvv = true,
#endif
#if defined(__ARM_FEATURE_CRC32) || MAYA_ISA_SSE42
    .crc32_hw = true,
#endif
};

// ============================================================================
// Compile-time platform assertions
// ============================================================================

static_assert(os != Os::Unknown || arch == Arch::Wasm32,
    "maya: unsupported operating system. "
    "Supported: Linux, macOS, Windows, FreeBSD, OpenBSD");

} // namespace maya::platform

// ============================================================================
// Platform macros — ONLY for #if include guards and type definitions.
// All behavioral branching uses constexpr values above.
// ============================================================================

#if MAYA_OS_WINDOWS
    #define MAYA_PLATFORM_WIN32 1
    #define MAYA_PLATFORM_MACOS 0
    #define MAYA_PLATFORM_POSIX 0
#elif MAYA_OS_APPLE
    #define MAYA_PLATFORM_WIN32 0
    #define MAYA_PLATFORM_MACOS 1
    #define MAYA_PLATFORM_POSIX 0
#else
    #define MAYA_PLATFORM_WIN32 0
    #define MAYA_PLATFORM_MACOS 0
    #define MAYA_PLATFORM_POSIX 1
#endif

// ============================================================================
// Compiler portability macros
// ============================================================================

// restrict qualifier — optimizer hint for non-aliasing pointers.
// MSVC spells it __restrict, GCC/Clang spell it __restrict__.
#if MAYA_CC_MSVC_ABI
    #define MAYA_RESTRICT __restrict
#else
    #define MAYA_RESTRICT __restrict__
#endif

// __builtin_unreachable / __builtin_expect — GCC/Clang intrinsics the rest
// of maya relies on. MSVC has __assume(0) for unreachability and no direct
// equivalent for branch hinting (identity is the correct fallback — the
// surrounding [[likely]]/[[unlikely]] attributes handle hot-path hinting
// on C++20 compilers).
#if MAYA_CC_MSVC
    #ifndef __builtin_unreachable
        #define __builtin_unreachable() __assume(0)
    #endif
    #ifndef __builtin_expect
        #define __builtin_expect(expr, val) (expr)
    #endif
#endif

// Force-inline hint — stronger than [[nodiscard]], used in hot paths.
// Use MAYA_FORCEINLINE in place of 'inline' for free functions.
// Use MAYA_ALWAYS_INLINE as an attribute on member functions.
#if MAYA_CC_MSVC_ABI
    #define MAYA_FORCEINLINE __forceinline
    #define MAYA_ALWAYS_INLINE __forceinline
#elif MAYA_CC_GNU_EXT
    #define MAYA_FORCEINLINE inline __attribute__((always_inline))
    #define MAYA_ALWAYS_INLINE __attribute__((always_inline))
#else
    #define MAYA_FORCEINLINE inline
    #define MAYA_ALWAYS_INLINE
#endif
