// maya::exec — see include/maya/core/executor.hpp.
#include "maya/core/executor.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace maya::exec {

namespace {
// Set by maya::run before the program starts and cleared after it ends; read
// by widgets during view(). The lock covers install vs. a read and is held
// only to copy a shared_ptr, so a read never copies the functions themselves.
std::mutex                            g_mu;
std::shared_ptr<const Executor>       g_exec;

std::shared_ptr<const Executor> current() {
    std::lock_guard lk(g_mu);
    return g_exec;
}
}  // namespace

void install(Executor e) {
    auto next = (e.parallel_for || e.post) ? std::make_shared<const Executor>(std::move(e)) : nullptr;
    std::lock_guard lk(g_mu);
    g_exec = std::move(next);
}

void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) {
    if (auto e = current(); e && e->parallel_for) { e->parallel_for(n, fn); return; }
    for (std::size_t i = 0; i < n; ++i) fn(i);   // no runtime: run inline
}

void post_erased(std::function<void()> job) {
    if (auto e = current(); e && e->post) { e->post(std::move(job)); return; }
    job();                                        // no runtime: run inline
}

}  // namespace maya::exec
