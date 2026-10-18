# Signals & Reactivity

maya includes a fine-grained reactive signal system inspired by SolidJS and
Leptos. Signals hold mutable values. Computed nodes derive values lazily.
Effects run side-effects. All dependency tracking is automatic.

## Signal\<T\> — Reactive Mutable Value

A `Signal` holds a value and notifies subscribers when it changes:

```cpp
Signal<int> count{0};

count.get();   // Read: returns 0
count();       // Shorthand for get()
count.set(42); // Write: notifies all dependents
count.update([](int& n) { n += 10; }); // Mutate in-place
```

### Construction

```cpp
Signal<int> a{0};                    // int, starts at 0
Signal<std::string> name{"alice"};   // string
Signal<float> ratio{0.5f};          // float
Signal<std::vector<int>> nums{{1, 2, 3}};  // vector
```

### Reading — get() and operator()

```cpp
const int& val = count.get();   // Returns const reference
const int& val = count();       // Same thing
```

**Automatic tracking**: If `get()` is called inside a `Computed` or `Effect`
scope, the signal is automatically registered as a dependency. When the signal
changes, the computed/effect re-evaluates.

### Writing — set()

```cpp
count.set(42);
```

If the new value equals the old value (via `operator==`), no notification is
sent. This prevents unnecessary re-renders.

### Mutating — update()

```cpp
count.update([](int& n) { n *= 2; });
```

`update()` always notifies, since it can't know if the mutation changed the
value. Use `set()` when possible for the equality check.

### Derived Values — map()

```cpp
Signal<int> width{10};
auto doubled = width.map([](int w) { return w * 2; });
// doubled is a Computed<int> that tracks width
```

### Version Tracking

```cpp
uint64_t v = count.version();  // Monotonically increasing on each change
```

## Computed\<T\> — Lazy Derived Value

A `Computed` is a read-only reactive value derived from other signals. It's
memoized — only recomputes when at least one dependency has changed:

```cpp
Signal<int> width{10};
Signal<int> height{20};

auto area = computed([&] {
    return width.get() * height.get();
});

area.get();   // Returns 200
area();       // Same thing

width.set(15);
area.get();   // Returns 300 (recomputed because width changed)
area.get();   // Returns 300 (cached — nothing changed)
```

### How It Works

1. On creation, the compute function runs once to establish initial
   dependencies (via `get()` calls inside the lambda).
2. When any dependency changes, the computed node is marked dirty.
3. On the next `get()`, it re-evaluates, re-establishes dependencies, and
   caches the new value.
4. If the new value equals the old (via `operator==`), downstream subscribers
   are **not** notified — the change stops propagating.

### Chaining

Computed values can depend on other computed values:

```cpp
Signal<float> price{10.0f};
Signal<int> quantity{3};

auto subtotal = computed([&] { return price() * quantity(); });
auto tax      = computed([&] { return subtotal() * 0.08f; });
auto total    = computed([&] { return subtotal() + tax(); });

total.get();  // 32.4
price.set(20.0f);
total.get();  // 64.8 (subtotal, tax, and total all recomputed)
```

## Effect — Reactive Side-Effects

An `Effect` runs a function whenever its dependencies change. It's an RAII
object — alive as long as it exists, disposed when destroyed:

```cpp
Signal<int> count{0};

auto fx = effect([&] {
    std::println("count is now {}", count.get());
});
// Prints: "count is now 0" (runs immediately on creation)

count.set(5);
// Prints: "count is now 5"

count.set(5);
// Nothing printed (value didn't change, no notification)

count.set(10);
// Prints: "count is now 10"
```

### Lifecycle

```cpp
{
    auto fx = effect([&] { /* ... */ });
    // Effect is active
    count.set(1);  // Effect runs
}
// fx destroyed — effect is disposed, no longer runs
count.set(2);  // Effect does NOT run
```

### Manual Disposal

```cpp
auto fx = effect([&] { /* ... */ });
fx.active();   // true
fx.dispose();  // Unsubscribes from all dependencies
fx.active();   // false
```

### Use Cases

- Logging state changes
- Triggering side-effects (network calls, file writes)
- Synchronizing external state with signal values
- Flagging a condition for code outside the signal graph

```cpp
auto alarm_effect = effect([&] {
    if (error_count.get() > 10) {
        std::println(stderr, "too many errors: {}", error_count.get());
    }
});
```

## Batch — Coalescing Updates

When setting multiple signals, each `set()` triggers notifications immediately.
`Batch` defers all notifications until the batch scope ends:

```cpp
Signal<int> x{0}, y{0};

auto fx = effect([&] {
    std::println("x={}, y={}", x(), y());
});
// Prints: "x=0, y=0"

// Without batch:
x.set(10);  // Effect runs: "x=10, y=0"
y.set(20);  // Effect runs: "x=10, y=20"  (two evaluations!)

// With batch:
{
    Batch batch;
    x.set(10);  // Deferred
    y.set(20);  // Deferred
}
// Effect runs ONCE: "x=10, y=20"
```

### Functional Form

```cpp
batch([&] {
    x.set(10);
    y.set(20);
});
// Single notification after lambda completes
```

### Nested Batches

Batches nest correctly — only the outermost batch triggers notifications:

```cpp
{
    Batch outer;
    x.set(1);
    {
        Batch inner;
        y.set(2);
    }  // inner batch ends, but outer is still active — no notification yet
}  // outer batch ends — notifications fire now
```

## Signals in maya Applications

In a maya program (`run<P>()`) the Model is the state: `update()` changes it
and `view()` reads it. Signals are useful for derived/cached computations,
and for reactive state outside a program (tests, one-shot `print()` output,
library code).

### Pattern: Signal-Driven View Fragment

`dyn()` reads signals when the element is rendered, so a view fragment can
follow signal values directly:

```cpp
Signal<int>         count{0};
Signal<std::string> message{"Ready"};

Element status() {
    return (v(
        dyn([&] { return text(message.get()); }),
        dyn([&] { return text(count.get()) | Bold; })
    ) | pad<1>).build();
}

print(status());
```

### Pattern: Program with Plain Model (preferred)

In Program apps, use plain data in the Model instead of signals:

```cpp
struct MyApp {
    struct Model { int count = 0; std::string message = "Ready"; };
    struct Inc {};
    using Msg = std::variant<Inc>;

    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Inc) {
        m.count++;
        m.message = "Count: " + std::to_string(m.count);
        return {};
    }
    static Element view(const Model& m) {
        return v(text(m.message), text(m.count) | Bold) | pad<1>;
    }
    // ...
};
```

### Pattern: Computed Display Values

```cpp
Signal<float> cpu{0.0f};
Signal<float> mem{0.0f};

auto health = computed([&] {
    float c = cpu(), m = mem();
    if (c > 90 || m > 90) return "CRITICAL";
    if (c > 70 || m > 70) return "WARNING";
    return "OK";
});
```

### Pattern: Batch Updates from External Data

```cpp
void on_metrics(const Metrics& m) {
    batch([&] {
        cpu.set(m.cpu);
        mem.set(m.mem);
        disk.set(m.disk);
        net.set(m.net);
    });
    // UI re-renders once with all four values updated
}
```

## Thread-Safety Model

Signals are **thread-local**. Each thread has its own reactive graph with:
- Its own scope stack (`current_scope`)
- Its own batch depth counter
- Its own pending notification queue

Do not share `Signal` objects across threads. For cross-thread communication,
use channels, atomics, or message passing — then update signals on the
receiving thread.

## Reactive Graph Internals

The reactive system uses a simple dependency graph:

```
Signal ──subscribes──→ Computed ──subscribes──→ Effect
  (source)              (derived)               (sink)
```

- **Signals** are pure sources — they have subscribers but no dependencies.
- **Computed** nodes are both — they subscribe to sources and have their own
  subscribers.
- **Effects** are pure sinks — they have dependencies but no subscribers.

When a signal changes:
1. `notify_subscribers()` walks the subscriber list.
2. Each subscriber is `mark_dirty()` + `evaluate()`.
3. `Computed::evaluate()` re-runs the compute function, re-establishes
   dependencies via `track()`, and propagates if the value changed.
4. `Effect::evaluate()` re-runs the effect function.

This is a **push-then-pull** model: changes push "dirty" flags down the graph,
then evaluation pulls fresh values as needed.

## Memory Safety Without a Borrow Checker

A reactive graph is the single hardest data structure to express in safe Rust,
and it is worth understanding *why*, because it is exactly the structure maya
is built on.

The graph is **cyclic and self-referential by construction.** A `Computed`
holds pointers to the `Signal`s it reads (its dependencies) *and* is pointed at
by everything that reads it (its subscribers). Edges run in both directions.
Nodes are created and destroyed in arbitrary order at runtime — a `Signal` may
outlive the `Effect` watching it, or the `Effect` may be destroyed first, or a
`Computed` may be dropped mid-notification while its subscribers are still on
the notification stack.

This is precisely the shape Rust's borrow checker *cannot* model. `&T` requires
a single acyclic ownership tree with statically-provable lifetimes; a graph
with back-edges has neither. Every mature Rust signal library — leptos, sycamore,
dioxus — resolves this the same way: it abandons `&T` and reaches for
`Rc<RefCell<Node>>` (or arena indices, or `unsafe`). At that point the
borrow checker is no longer checking anything about the graph. You have simply
**moved the aliasing check from compile time to run time**, where a violation
is a `RefCell::borrow` panic — a crash — instead of a compiler error. The
reactive re-entrancy that is *normal* in this domain (an effect that writes a
signal that re-runs the effect) is exactly what trips a `RefCell`.

maya expresses the same graph in plain C++ with raw intrusive back-pointers,
no reference counting on the hot path, and no runtime borrow flag. In exchange
for giving up the borrow checker's static guarantee, it takes on the obligation
to get the lifetime bookkeeping right by hand:

- `~ReactiveNode()` calls `unlink_all()`, which severs every incoming *and*
  outgoing edge before the node's storage dies. A destroyed node can never be
  reached from a surviving neighbour's subscriber or dependency list.
- Notification uses a thread-local `NotifyFrame` stack so a node destroyed
  *during* its own notification is unlinked from the in-flight walk, not
  dereferenced after free.
- The whole system is thread-local by design — one graph per thread — so there
  is no shared-mutable-aliasing question to answer in the first place.

The claim "we got the lifetime bookkeeping right" is not asserted — it is
**proven at runtime**, on every CI run, by an AddressSanitizer + UBSan gate.
[`test_signal.cpp`](https://github.com/agentty-org/maya/blob/master/tests/test_signal.cpp)
contains explicit adversarial lifetime cases:

- `test_computed_destroyed_before_signal` — drop a derived node while its
  source is still live and mutating.
- `test_signal_destroyed_before_effect` — drop the source out from under an
  active effect.
- `test_effect_disposes_sibling_effect` — an effect that destroys *another*
  effect while the notification walk is in progress.
- `test_batched_node_destroyed_before_flush` — destroy a node that has a
  pending batched notification queued against it.
- `test_effect_does_not_fire_after_destruction` — the disposed-observer
  guarantee.

These are use-after-free bugs *by design* — each one deliberately builds the
timing window a naive graph would crash in. The CI `sanitizers` job compiles
the entire tree with `-fsanitize=address,undefined` and runs them with
`detect_leaks=1`. They pass with zero errors:

```
$ ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./test_signal
...
--- test_batched_node_destroyed_before_flush ---
PASS
=== ALL 20 TESTS PASSED ===
```

The takeaway is not "C++ is as safe as Rust." It is more specific and more
honest than that: **for the class of data structure where Rust's borrow checker
stops helping and starts costing you** — graphs, back-edges, arbitrary-order
destruction, deliberate re-entrancy — a C++ codebase with intrusive lifetime
discipline and a sanitizer gate ends up in the *same* place a Rust codebase
does (aliasing checked dynamically), except the C++ version never paid the
`Rc<RefCell>` tax, keeps `&T` for the 95% of the program that *is* a tree, and
catches the same bug class — plus leaks and integer UB the borrow checker never
looked at — under one gate. The safety is earned by tooling and discipline
instead of granted by the type system, and for this problem shape that is the
better trade.
