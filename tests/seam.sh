#!/bin/sh
# tests/seam.sh — the layering rule, as a test.
#
# maya is the view layer and the terminal device; jaal is the runtime;
# include/maya/host/ and include/maya/runtime.hpp are where they meet. The
# rule that makes "maya is to jaal what Ink is to React" a fact instead of a
# slogan:
#
#   NOTHING outside include/maya/host/ and maya/runtime.hpp may depend on jaal.
#
# A prose mention in a comment is fine — that's how a reader finds the seam.
# An #include or a jaal:: name is not. If this fails, someone reached across
# the seam, and the view layer just stopped being usable without a runtime.
#
#   sh tests/seam.sh
#
# Runs in CI. No build needed: it reads the tree.

cd "$(dirname "$0")/.." || exit 1
status=0

fail() { echo "  FAIL  $1"; status=1; }

# 1. The compiled view layer knows nothing of jaal, so libmaya.a links
#    without it and a non-terminal host could reuse the whole renderer.
#    Comments may NAME jaal (that's how a reader finds the seam); code may
#    not depend on it.
hits=""
for f in $(grep -rl 'jaal' src/ 2>/dev/null); do
    if sed 's://.*::' "$f" | grep -qE '#[[:space:]]*include[[:space:]]*<jaal|jaal::'; then
        hits="$hits $f"
    fi
done
if [ -n "$hits" ]; then
    fail "src/ depends on jaal:"
    for f in $hits; do echo "          $f"; done
else
    echo "  ok    src/ is jaal-free"
fi

# 2. Headers outside the seam may NAME jaal in prose, but must not include
#    it or use its types.
leaks=""
for f in $(grep -rl 'jaal' include/maya --include='*.hpp' 2>/dev/null); do
    case "$f" in
        include/maya/host/*) continue ;;      # the seam itself
        include/maya/runtime.hpp) continue ;; # jaal re-exported for apps
        include/maya/capability.hpp) continue ;; # lock-order types only
        include/maya/core/target.hpp) continue ;; # jaal's platform facts, re-exported
    esac
    # Strip // comments, then look for real dependencies.
    if sed 's://.*::' "$f" | grep -qE '#[[:space:]]*include[[:space:]]*<jaal|jaal::'; then
        leaks="$leaks $f"
    fi
done
if [ -n "$leaks" ]; then
    fail "these headers depend on jaal outside the seam:"
    for f in $leaks; do echo "          $f"; done
else
    echo "  ok    only maya/host/, runtime.hpp, capability.hpp and core/target.hpp depend on jaal"
fi

# 3. The seam is where we say it is.
for f in interop effects sources device terminal run; do
    [ -f "include/maya/host/$f.hpp" ] || fail "missing include/maya/host/$f.hpp"
done
[ $status -eq 0 ] && echo "  ok    the seam is six files"

# 4. maya has no runtime of its own. `Runtime` was the terminal DEVICE, a
#    name that contradicted the whole design; it is `Device` now.
if grep -rn 'detail::Runtime' include/ src/ 2>/dev/null | grep -qv '^Binary'; then
    fail "detail::Runtime is back — the device is not the runtime (jaal is)"
else
    echo "  ok    no 'Runtime' in maya: jaal is the runtime"
fi

# 5. "app" is what someone WRITES with maya, not a layer inside it. The
#    layers are the device, the renderer, the view layer and the host; a
#    directory called app/ means one of them lost its name again.
if [ -d include/maya/app ] || [ -d src/app ]; then
    fail "an app/ directory is back — name the layer, not the product"
else
    echo "  ok    no app/ layer: device, renderer, view, host"
fi

# 6. No detached threads. The loop and the threads are jaal's (rule 1); the
#    two maya owns are joined before they can outlive their caller. A
#    detached thread is one nobody can wait for, so at exit it runs on
#    inside a process destroying the statics under it.
detached=$(grep -rn '\.detach()' src/ include/ --include='*.cpp' --include='*.hpp' 2>/dev/null)
if [ -n "$detached" ]; then
    fail "a detached thread is back — own it and join it:"
    echo "$detached" | sed 's/^/          /'
else
    echo "  ok    no detached threads: every one is owned and joined"
fi

# 7. Signals are the runtime's. jaal watches SIGWINCH and delivers
#    jaal::sig::resize; maya used to install a SECOND handler whose
#    self-pipe nobody read, and whoever installs last wins — jaal carries a
#    special case to this day because maya's handler ate resizes. Only the
#    emergency tty-restore may touch sigaction, and it chains to the prior
#    handler and re-raises rather than keeping the signal.
sigwinch=""
for f in $(grep -rl 'SIGWINCH' src/ include/ --include='*.cpp' --include='*.hpp' 2>/dev/null); do
    # Normalise the path before comparing. BSD grep (macOS) echoes the root
    # back verbatim, so `grep -r include/` yields `include//maya/...` with a
    # doubled slash while GNU grep yields `include/maya/...`. An exact-string
    # exclusion then silently misses on macOS only, and the emergency
    # tty-restore below reads as a re-installed SIGWINCH handler.
    f=$(printf '%s\n' "$f" | sed 's://*:/:g')
    [ "$f" = "include/maya/terminal/terminal.hpp" ] && continue
    # Code (not comments) that INSTALLS a handler for it. `::sigaction(` /
    # `std::signal(` — not `on_signal(`, which is how the host RECEIVES
    # jaal's delivery.
    if sed 's://.*::' "$f" | grep -qE '(^|[^_[:alnum:]])(::)?(sigaction|signal)[[:space:]]*\(' ; then
        sigwinch="$sigwinch $f"
    fi
done
if [ -n "$sigwinch" ]; then
    fail "maya installs a SIGWINCH handler again — that is jaal's, via on_signal:"
    for f in $sigwinch; do echo "          $f"; done
else
    echo "  ok    SIGWINCH is jaal's: one owner, delivered as sig::resize"
fi

# 8. Quitting is a Cmd, not a flag. The device used to carry running_ /
#    request_quit() from the old loop; a program ends by returning
#    Cmd::quit(n) and jaal unwinds. A flag in the device means the device
#    is deciding when the program is over, which is the runtime's call.
if grep -rn 'request_quit\|is_running' src/ include/ --include='*.cpp' --include='*.hpp' 2>/dev/null \
     | grep -v '^[^:]*:[0-9]*:[[:space:]]*//' | grep -q .; then
    fail "a quit flag is back in maya — a program quits with Cmd::quit(n)"
else
    echo "  ok    no quit flag: a program ends with Cmd::quit"
fi

[ $status -eq 0 ] && echo "seam: ok" || echo "seam: FAILED"
exit $status
