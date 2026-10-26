#pragma once
// maya/core/executor.hpp — how maya's view code asks for parallel work
// without owning a thread.
//
// The view library (libmaya) doesn't link a runtime: a renderer must work
// with no loop at all. But two widgets want parallelism — Pixels::fill_rows
// (a parallel-for over rows) and StreamingMarkdown's big-document parse.
// They used to start std::threads. Now they ask this hook, and maya::app
// installs a jaal-backed executor at startup (<maya/runtime.hpp>), so every
// thread in a maya program is jaal's.
//
// With nothing installed (a bare renderer, a unit test), work runs inline on
// the caller. That's slower, never wrong: both callers are correct serially.

#include <cstddef>
#include <functional>
#include <memory>

namespace maya::exec {

struct Executor {
    // Run fn(i) for i in [0, n) and return when every call has finished.
    // Calls may run concurrently; fn must be safe for that.
    std::function<void(std::size_t n, const std::function<void(std::size_t)>& fn)>
        parallel_for;
    // Run `job` in the background. It must not outlive the process: the
    // executor joins (or cancels and joins) outstanding jobs at shutdown.
    std::function<void(std::function<void()> job)> post;
};

// Install the process executor. Called once by maya::app before the program
// starts; passing {} uninstalls (jobs then run inline).
void install(Executor e);

// The installed executor's operations, or inline fallbacks.
void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn);

// The type-erased form post() wraps. Not for direct use.
void post_erased(std::function<void()> job);

// Run body(*state) in the background. The body is a plain function pointer,
// so it can't capture `this` or a reference into the caller's frame: the
// only thing it reaches is `state`, which the job co-owns, so the caller may
// be destroyed mid-job. Whatever in S both sides touch must synchronise
// itself (an atomic flag published after the writes, say); that one rule is
// what remains the caller's to keep.
template <class S>
void post(void (*body)(S&), std::shared_ptr<S> state) {
    post_erased([body, s = std::move(state)] { body(*s); });
}

}  // namespace maya::exec
