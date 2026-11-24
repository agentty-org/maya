#pragma once
// maya::core::expected - Monadic error handling
//
// Rust has Result<T, E>. We have std::expected<T, E> (C++23) plus a rich
// Error type with source location tracking. No exceptions. No error codes.
// Just algebraic types composed monadically.

#include "target.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace maya {

// ============================================================================
// ErrorKind - categorized error variants
// ============================================================================

enum class ErrorKind : uint8_t {
    TerminalInit,       // Failed to initialize terminal
    Io,                 // I/O operation failed
    LayoutOverflow,     // Layout computation overflow
    InvalidStyle,       // Invalid style specification
    InvalidUtf8,        // Malformed UTF-8 input
    Unsupported,        // Feature not supported by terminal
    Signal,             // Process signal received
    WouldBlock,         // Write would block (EAGAIN/EWOULDBLOCK) — frame skipped
};

// ============================================================================
// Error - rich error type with source location
// ============================================================================

struct Error {
    ErrorKind           kind;
    std::string         message;
    std::source_location location;

    // Factory methods - named constructors for each error kind
    [[nodiscard]] static Error terminal(
        std::string msg,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::TerminalInit, std::move(msg), loc};
    }

    [[nodiscard]] static Error io(
        std::string msg,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::Io, std::move(msg), loc};
    }

    [[nodiscard]] static Error layout(
        std::string msg,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::LayoutOverflow, std::move(msg), loc};
    }

    [[nodiscard]] static Error unsupported(
        std::string msg,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::Unsupported, std::move(msg), loc};
    }

    [[nodiscard]] static Error would_block(
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::WouldBlock, "write would block", loc};
    }

    [[nodiscard]] static Error from_errno(
        std::string_view context,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::Io, std::string(context) + ": " + std::string(strerror(errno)), loc};
    }
};

// ============================================================================
// Result<T> = std::expected<T, Error>
// ============================================================================

template <typename T>
using Result = std::expected<T, Error>;

// Result<void> shorthand
using Status = Result<void>;

// ============================================================================
// ok() / err() helpers - like Rust's Ok() and Err()
// ============================================================================

template <typename T>
[[nodiscard]] constexpr Result<std::remove_cvref_t<T>> ok(T&& value) {
    return Result<std::remove_cvref_t<T>>(std::in_place, std::forward<T>(value));
}

[[nodiscard]] inline Status ok() {
    return {};
}

template <typename T = void>
[[nodiscard]] constexpr std::unexpected<Error> err(Error e) {
    return std::unexpected{std::move(e)};
}

// ============================================================================
// try_() macro - like Rust's ? operator
// ============================================================================
// Usage: auto val = MAYA_TRY(some_result_returning_fn());
//
// If the result is an error, immediately returns it from the enclosing function.
// If the result is a value, unwraps it.

// Token concatenation for unique variable names
#define MAYA_CAT2_(a, b) a##b
#define MAYA_CAT_(a, b) MAYA_CAT2_(a, b)

// GCC/Clang: expression macro using statement expressions.
// Not available on MSVC — use MAYA_TRY_DECL instead.
#if MAYA_CC_GNU_EXT
#define MAYA_TRY(expr)                                         \
    ({                                                         \
        auto&& _maya_result = (expr);                          \
        if (!_maya_result) [[unlikely]]                        \
            return std::unexpected{_maya_result.error()};       \
        std::move(*_maya_result);                               \
    })
#endif

// Portable version — works on GCC, Clang, AND MSVC.
// Usage: MAYA_TRY_DECL(auto val, some_result_fn());
#define MAYA_TRY_DECL_IMPL_(uniq, decl, expr)                  \
    auto&& uniq = (expr);                                      \
    if (!uniq) [[unlikely]]                                    \
        return std::unexpected{uniq.error()};                   \
    decl = std::move(*uniq)

#define MAYA_TRY_DECL(decl, expr)                              \
    MAYA_TRY_DECL_IMPL_(MAYA_CAT_(_maya_try_, __COUNTER__), decl, expr)

// For Status (Result<void>) - just propagate error, no value to unwrap.
// Portable across all compilers.
#define MAYA_TRY_VOID(expr)                                    \
    do {                                                       \
        auto&& _maya_result = (expr);                          \
        if (!_maya_result) [[unlikely]]                        \
            return std::unexpected{_maya_result.error()};       \
    } while (0)

} // namespace maya
