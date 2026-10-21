#pragma once
// maya::render::canvas - Double-buffered 2D cell grid for terminal rendering
//
// Canvas is the rendering surface: a width x height grid of packed 64-bit
// cells. Each cell holds a Unicode code point, a style ID (interned via
// StylePool), a hyperlink ID, and a width marker for CJK/wide characters.
//
// The 64-bit packing enables O(1) cell comparison in the diff algorithm --
// two cells are identical iff their packed representations are equal.
// Dirty-region tracking (damage rect) limits the diff to only the changed
// area, and a clip stack supports overflow:hidden containers.

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <functional>
#include <string_view>
#include <vector>

#include "../core/types.hpp"
#include "../core/simd.hpp"  // also brings in platform/detect.hpp for MAYA_FORCEINLINE
#include "../style/style.hpp"
#include "../style/theme.hpp"  // Theme, held by value in StylePool::sgr_theme_
#include "../element/text.hpp"

namespace maya {

// ============================================================================
// terminal_color_level — the render pipeline's active color capability
// ============================================================================
// 3 = truecolor, 2 = 256-color, 1 = 16-color. Detected once from the
// environment (COLORTERM / TERM / NO_COLOR — see maya::env::color_level)
// and overridable with MAYA_COLOR=truecolor|256|16|auto. This is the SAME
// level StylePool uses to degrade RGB SGR at emit time; widgets that pick
// between an RGB-band design and a plain-fg fallback (e.g. diff row bands,
// which quantize to garish full-bright cells at 16 colors) should branch
// on it so their design intent survives the degrade instead of being
// mangled by it.
[[nodiscard]] int terminal_color_level() noexcept;

// ============================================================================
// AlignedBuffer - 64-byte cache-line aligned buffer for SIMD
// ============================================================================
// AVX2 loads from 32-byte aligned addresses avoid split cache-line penalties.
// We align to 64 bytes (full cache line) so that SIMD diff loops hit aligned
// addresses more often. This replaces std::vector<uint64_t> for cell storage.

class AlignedBuffer {
public:
    AlignedBuffer() = default;

    explicit AlignedBuffer(std::size_t count, uint64_t fill_value = 0);

    ~AlignedBuffer() { free(); }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& o) noexcept
        : data_(o.data_), size_(o.size_), capacity_(o.capacity_)
    { o.data_ = nullptr; o.size_ = o.capacity_ = 0; }

    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) {
            free();
            data_ = o.data_; size_ = o.size_; capacity_ = o.capacity_;
            o.data_ = nullptr; o.size_ = o.capacity_ = 0;
        }
        return *this;
    }

    void resize(std::size_t count, uint64_t fill_value = 0);
    void assign(std::size_t count, uint64_t value);

    /// Release capacity when `count` is dramatically smaller than
    /// currently allocated. Useful after a width oscillation where
    /// the cell buffer grew during a brief wide-window state and is
    /// now permanently stuck at that size. Threshold is 2x — i.e.
    /// shrink only when at most half the current capacity is needed,
    /// to avoid thrashing the allocator on small steady-state
    /// variations.
    void shrink_to_fit_if_oversized(std::size_t count) noexcept;

    [[nodiscard]] uint64_t* data() noexcept { return data_; }
    [[nodiscard]] const uint64_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    uint64_t& operator[](std::size_t i) noexcept { return data_[i]; }
    const uint64_t& operator[](std::size_t i) const noexcept { return data_[i]; }

    uint64_t* begin() noexcept { return data_; }
    uint64_t* end() noexcept { return data_ + size_; }
    const uint64_t* begin() const noexcept { return data_; }
    const uint64_t* end() const noexcept { return data_ + size_; }

private:
    void free() {
        if (data_) ::operator delete(data_, std::align_val_t{64});
        data_ = nullptr;
    }

    uint64_t*   data_     = nullptr;
    std::size_t size_     = 0;
    std::size_t capacity_ = 0;
};

// ============================================================================
// Cell - A single terminal cell (packed into 64 bits)
// ============================================================================
// Memory layout (8 bytes total):
//   [0..3]  char32_t character    (32 bits)
//   [4..5]  uint16_t style_id     (16 bits - index into StylePool)
//   [6]     uint8_t  hyperlink_id (8 bits  - index into a hyperlink pool)
//   [7]     uint8_t  width        (8 bits  - wide-glyph marker; only 0/1/2)
//
// The pack/unpack scheme is designed so that identical cells produce
// identical 64-bit values, enabling the diff to compare cells with a
// single integer comparison.
//
// hyperlink_id is deliberately uint8_t, NOT uint16_t: byte 7 belongs to
// `width`, and the diff/renderer hot paths read it as `packed >> 56`
// (with the top bits assumed zero beyond the 3 width values). A 16-bit
// hyperlink field shifted <<48 would silently overwrite the width byte
// for ids > 255 — the diff would then misclassify those cells as
// wide-glyph halves and skip/overdraw them. The narrow type makes that
// overflow unrepresentable at compile time instead of a runtime surprise
// for whichever future feature first interns >255 hyperlinks (such a
// feature must widen the layout deliberately, touching every `>> 56`
// reader, not by accident).

struct Cell {
    char32_t character    = U' ';
    uint16_t style_id     = 0;
    uint8_t  hyperlink_id = 0;
    uint8_t  width        = 0;  // 0 = normal, 1 = wide first half, 2 = wide second half

    /// Pack all fields into a single 64-bit integer for fast comparison.
    [[nodiscard]] constexpr uint64_t pack() const noexcept {
        return static_cast<uint64_t>(character)
             | (static_cast<uint64_t>(style_id)     << 32)
             | (static_cast<uint64_t>(hyperlink_id) << 48)
             | (static_cast<uint64_t>(width)        << 56);
    }

    /// Reconstruct a Cell from its packed 64-bit representation.
    [[nodiscard]] static constexpr Cell unpack(uint64_t packed) noexcept {
        return Cell{
            .character    = static_cast<char32_t>(packed & 0xFFFFFFFF),
            .style_id     = static_cast<uint16_t>((packed >> 32) & 0xFFFF),
            .hyperlink_id = static_cast<uint8_t>((packed >> 48) & 0xFF),
            .width        = static_cast<uint8_t>((packed >> 56) & 0xFF),
        };
    }

    constexpr bool operator==(const Cell&) const = default;
};

/// Mask selecting the STRUCTURAL half of a packed cell: glyph, hyperlink and
/// width — everything except `style_id`.
///
/// The scrollback gate exists to catch a committed-prefix SHIFT: a row above
/// the viewport moved, so what maya believes is in the terminal's scrollback
/// no longer matches what is there. That is a question about CONTENT.
///
/// A retheme is not that. Swapping colour schemes re-interns every style, so
/// `style_id` changes on essentially every cell while not one glyph moves —
/// the committed prefix is still exactly where it was. Comparing full packed
/// cells cannot tell the two apart, so a theme change looked identical to
/// corruption and forced a scrollback recovery (a full-viewport repaint, ~31
/// KB) on every keystroke of the theme browser.
///
/// Masking style_id out makes the distinction structural rather than
/// heuristic: this answers "did anything MOVE", and a caller that also needs
/// to know "did anything RECOLOUR" asks that separately.
inline constexpr uint64_t cell_structure_mask = ~(static_cast<uint64_t>(0xFFFF) << 32);

// Verify packing round-trips correctly.
static_assert(Cell{U'A', 42, 7, 1}.pack() != 0);
static_assert(Cell::unpack(Cell{U'A', 42, 7, 1}.pack()) == Cell{U'A', 42, 7, 1});
static_assert(Cell{}.pack() == Cell{U' ', 0, 0, 0}.pack());
// Field-isolation proofs: the max value of each field must round-trip
// without perturbing its neighbours (this is what the old uint16_t
// hyperlink_id violated — id 256 packed into the width byte).
static_assert(Cell::unpack(Cell{U'A', 65535, 255, 2}.pack())
              == Cell{U'A', 65535, 255, 2});
static_assert(Cell::unpack(Cell{U'A', 0, 255, 0}.pack()).width == 0);
static_assert(Cell::unpack(Cell{U'A', 0, 0, 2}.pack()).hyperlink_id == 0);

// ============================================================================
// StylePool - Interning pool for Style objects
// ============================================================================
// Assigns each unique Style a compact uint16_t ID. Identical styles always
// receive the same ID, so the canvas stores only 16-bit indices instead of
// full Style objects. This makes cell packing feasible and the diff cheap.
//
// The pool is shared between front and back canvases in the double-buffer
// system so that style IDs are comparable across frames.

// ============================================================================
// StylePool - Open-addressing flat hash map for style interning
// ============================================================================
// Replaces std::unordered_map with a cache-friendly open-addressing table.
// Linear probing on a power-of-2 table gives O(1) amortized lookup with
// zero pointer chasing. For typical TUI workloads (10-50 unique styles),
// this is ~5x faster than std::unordered_map.
//
// Layout:
//   styles_[]  — flat vector of Style objects, indexed by uint16_t ID
//   slots_[]   — open-addressing table: { hash, id } pairs
//   hash == 0  — empty sentinel (real hashes that collide with 0 are mapped to 1)

class StylePool {
public:
    /// Maximum number of unique styles (uint16_t ID space).
    static constexpr std::size_t max_styles = 65535;

    StylePool();

    /// Intern a style, returning its unique ID.
    /// Hot path: called once per cell during painting.
    [[nodiscard]] MAYA_ALWAYS_INLINE uint16_t intern(const Style& s) {
        std::size_t h = hash_style(s);
        std::size_t idx = h & mask_;

        while (true) {
            auto& slot = slots_[idx];
            if (slot.hash == 0) [[unlikely]] {
                // Saturate: the uint16_t id space is full. Collapse
                // every new style to id 0 (the default style) rather
                // than aliasing onto the last-inserted entry —
                // "closest existing style" silently merges every
                // overflowing style into one arbitrary previously-seen
                // style, producing a visible-but-misattributed style
                // (e.g. all overflowed styles render as whatever the
                // 65534th style happened to be). Collapsing to default
                // makes overflow self-announcing: text reverts to
                // plain styling, which is the conservative failure
                // mode and matches what `prev_id == UINT16_MAX` does
                // for unknown terminal state.
                //
                // The bool flag below is set so external diagnostics
                // can surface the condition once (an animation that
                // generates a fresh shade per frame is the practical
                // way to hit this).
                if (styles_.size() >= max_styles) [[unlikely]] {
                    overflow_ = true;
                    return 0;
                }
                // Empty slot — insert new style.
                auto id = static_cast<uint16_t>(styles_.size());
                styles_.push_back(s);
                sgr_cache_.push_back(build_sgr(s));
                looks_.push_back(look_of(s));
                if (s.caret_anchor) [[unlikely]] caret_ids_.push_back(id);
                slot = {h, id};
                ++size_;
                if (size_ * 4 > capacity_ * 3) [[unlikely]] {
                    grow(capacity_ * 2);
                }
                return id;
            }
            if (slot.hash == h && styles_[slot.id] == s) [[likely]] {
                return slot.id;
            }
            idx = (idx + 1) & mask_;
        }
    }

    /// Look up a style by its ID. The caller must ensure the ID is valid.
    [[nodiscard]] MAYA_ALWAYS_INLINE const Style& get(uint16_t id) const noexcept {
        // Clamp a stale/out-of-range id to the always-present default style
        // (id 0) rather than reading past styles_ into freed/uninitialized
        // capacity.  A cell can legitimately carry a style_id interned in a
        // frame BEFORE clear() shrank the pool; grid_emit's StyleTable::write
        // and any other reader must not fault on it.  Mirrors the same guard
        // already present on sgr().
        if (id >= styles_.size()) id = 0;
        return styles_[id];
    }

    /// Pre-built SGR escape sequence for a style ID.
    /// Each string is a complete "\x1b[0;...m" that resets then applies — the diff
    /// loop calls this once per style change instead of computing transitions.
    [[nodiscard]] MAYA_ALWAYS_INLINE std::string_view sgr(uint16_t id) const noexcept {
        // Clamp a stale/out-of-range id (e.g. one carried over from a frame
        // before clear() shrank the pool) to the always-present default
        // style rather than reading past sgr_cache_ into uninitialized
        // capacity. See write_transition_sgr for the full rationale.
        if (id >= sgr_cache_.size()) id = 0;
        return sgr_cache_[id];
    }

    /// Emit the minimal SGR sequence that transitions the terminal from
    /// the SGR state corresponding to `prev_id` to the state for `new_id`,
    /// appended directly to `out`.
    ///
    /// When `prev_id == UINT16_MAX` (the "unknown state" sentinel — no
    /// SGR has been emitted yet this frame) we fall back to the full
    /// reset-and-set sequence via the pre-cached `sgr_cache_[new_id]`,
    /// because the terminal may carry residual SGR from outside maya's
    /// control. Otherwise we emit only the differential — attribute
    /// toggles for the bits that changed (e.g. `\x1b[22m` to turn off
    /// bold) and color setters for the fg / bg that changed. The leading
    /// `0;` reset is omitted, saving 2 bytes per transition; the more
    /// significant win is on transitions that change only a single
    /// attribute (e.g. fg color flip in a syntax-highlighted block),
    /// where the full reset would re-emit every preserved attribute.
    ///
    /// Same prev_id == new_id is a no-op (caller is expected to gate but
    /// we re-check for safety).
    void write_transition_sgr(uint16_t prev_id, uint16_t new_id,
                              std::string& out) const;

    /// Before an SGR reset that drops the style: close its OSC 8 link, if
    /// it has one (the reset doesn't end a link).
    void close_link(uint16_t id, std::string& out) const {
        if (id < styles_.size() && styles_[id].link) out.append("\x1b]8;;\x1b\\");
    }

    /// Number of interned styles.
    [[nodiscard]] std::size_t size() const noexcept { return styles_.size(); }

    /// Process-wide unique identifier assigned at construction. Stable
    /// for the lifetime of this StylePool — a different pool instance
    /// always gets a different id, even when memory is recycled. Used
    /// by `intern_const<S>(pool)` (render-roadmap B5) to validate that
    /// a thread-local cached style_id is still valid for the current
    /// pool.
    [[nodiscard]] uint64_t pool_id() const noexcept { return pool_id_; }

    /// True if `intern()` has ever been asked for a style after the
    /// uint16_t id space saturated. Once set, stays set until
    /// `clear()` is called — overflow is a one-way condition for the
    /// pool's lifetime. Callers (diagnostics, optional warning UI)
    /// can poll this between frames; we don't surface it from the
    /// hot path to avoid syscalls.
    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }

    /// True if `id` was interned from a style carrying the hardware-
    /// caret anchor meta-bit. O(k) over the (tiny — normally 0 or 1)
    /// set of anchor-carrying ids; used by the paint path to detect
    /// the caret cell without unpacking the Style per cell.
    [[nodiscard]] bool is_caret_anchor(uint16_t id) const noexcept {
        for (uint16_t c : caret_ids_) if (c == id) return true;
        return false;
    }
    /// Any caret-anchor style interned at all? Lets the hot paint path
    /// skip per-cell anchor checks entirely for frames with no caret.
    [[nodiscard]] bool has_caret_anchor() const noexcept {
        return !caret_ids_.empty();
    }

    /// Do two style ids paint the same pixels on THIS terminal?
    ///
    /// True when their colours, resolved against the live theme and
    /// degraded to the terminal's depth, and their wire-visible attributes
    /// are equal. A 256-colour link folds many truecolor shades onto one
    /// palette entry, so a diff that compares ids re-sends cells whose
    /// look didn't change. Out-of-range ids compare as id 0, like
    /// write_transition_sgr treats them.
    [[nodiscard]] bool same_look(uint16_t a, uint16_t b) const noexcept {
        if (a == b) return true;
        const auto n = looks_.size();
        return looks_[a < n ? a : 0] == looks_[b < n ? b : 0];
    }

    /// Reset the pool back to only the default style.
    void clear();

    /// Re-derive cached SGR for the theme now in force.
    ///
    /// build_sgr() bakes the theme's background into every style that does
    /// not name one (see canvas.cpp), and the result is cached per style id
    /// at intern time. A theme swap therefore invalidates every cached
    /// string — without this, bg-less styles would keep emitting the
    /// PREVIOUS canvas colour for the rest of the process, which is the
    /// stale-bytes half of the same bug the default fixes.
    ///
    /// MUST BE CALLED at the top of every frame, before anything is
    /// interned or emitted. It is NOT automatic: sgr() is a const noexcept
    /// hot-path accessor and intern() is MAYA_ALWAYS_INLINE, so neither can
    /// afford — or is allowed — to mutate the cache behind the caller's
    /// back. (An earlier version of this comment claimed they did. They
    /// never have.) Both frame drivers do it for you: RenderPipeline's
    /// clear() stage and Runtime's inline compose. A host that hand-rolls a
    /// frame owns the call.
    ///
    /// Returns true iff the theme actually changed and the cache was
    /// rebuilt. That matters to anyone holding a WIRE-LEVEL SHADOW: a
    /// retheme rewrites what a style id renders as while changing neither
    /// the glyph nor the id, so a cell-diffing emitter sees an identical
    /// frame and sends nothing. Such a caller must invalidate its shadow
    /// when this returns true — see the comment on sgr_theme_.
    bool retheme();

private:
    struct Slot {
        std::size_t hash = 0;  // 0 = empty sentinel
        uint16_t    id   = 0;
    };

    std::vector<Style>       styles_;
    std::vector<std::string> sgr_cache_;  // sgr_cache_[id] = pre-built "\x1b[0;...m"
    // looks_[id]: what the style paints as, on this terminal (see same_look).
    // Rebuilt with sgr_cache_ on retheme, because it bakes the theme in.
    struct Look {
        std::optional<LitColor> fg, bg;
        std::uint8_t attrs = 0;   // bold italic underline inverse strike conceal
        bool operator==(const Look&) const = default;
    };
    std::vector<Look>        looks_;
    static Look look_of(const Style& s) noexcept;
    // The theme the cached SGR strings were built under, held BY VALUE and
    // compared BY VALUE.
    //
    // This used to be a `const void*` identity test, on the stated premise
    // that "the live slot is re-seated rather than mutated". That premise is
    // false: app_set_theme() assigns THROUGH the slot (`*slot = t`), so the
    // address of theme::live() is fixed for the life of the Runtime. The
    // pointer therefore matched on every frame after the first and retheme()
    // was a permanent no-op — every swap kept the previous theme's SGR bytes,
    // which is precisely the "background stays the old colour" bug. A Theme
    // is ~25 Colors; comparing it once per frame is trivially cheap next to
    // painting one, and unlike identity it cannot be silently wrong.
    Theme                    sgr_theme_{};
    bool                     sgr_theme_valid_ = false;
    std::vector<Slot>        slots_;
    std::vector<uint16_t>    caret_ids_;  // ids interned with caret_anchor set
    std::size_t size_     = 0;
    std::size_t capacity_ = 0;
    std::size_t mask_     = 0;
    uint64_t    pool_id_  = 0;  // Set by StylePool() ctor via next_pool_id_.
    bool        overflow_ = false;  // intern() hit the uint16_t cap

    // ── SGR cache builder ────────────────────────────────────────────────
    static char* write_uint_sgr(char* p, unsigned n) noexcept;
    // Resolve a Color against the live theme, then emit. The two halves are
    // separate so a caller holding an already-resolved LitColor can skip the
    // first without the second having to guess whether it still needs doing.
    static char* append_color_sgr(char* p, const Color& c, bool is_fg) noexcept;
    static char* append_resolved_sgr(char* p, const LitColor& c, bool is_fg) noexcept;
    static std::string build_sgr(const Style& s);

    /// FNV-1a hash over the style's packed representation.
    [[nodiscard]] static std::size_t hash_style(const Style& s) noexcept;

    void insert_slot(std::size_t h, uint16_t id);
    void grow(std::size_t new_cap);
};

// ============================================================================
// intern_const(pool, style) — cached style_id for a stable style value
// ============================================================================
//
// Render-roadmap B5. For a `Style` that's stable at a call site (a
// constexpr global, a theme entry that doesn't change between frames),
// skip the hash + slot-probe of `pool.intern()` on every paint by
// stashing the resolved id in a thread_local cache slot.
//
// Style isn't a structural type — `std::optional<Color>` has private
// base classes — so we can't use it as a non-type template parameter.
// Instead, each call site gets its own cache via a defaulted lambda
// template parameter: every textual occurrence of `intern_const(pool,
// ...)` produces a unique lambda closure type at that source location,
// which instantiates a distinct template specialisation with its own
// static thread_local slot. Two different lines in the same TU never
// share cache state. Two different translation units with the same
// line of code also get distinct instantiations.
//
// Contract: at a given call site, `s` should evaluate to the SAME
// style on every call. A site that toggles between styleA / styleB
// will cache one of them and return that for the other. Use this for
// constants; use plain `pool.intern(s)` for dynamic styles.
//
// Cache invalidation: keyed on `pool.pool_id()`, which bumps on
// StylePool construction AND on `clear()`. A stale slot from a
// previous pool generation falls through to re-intern.
//
// Concurrency: each thread holds its own slot. First call on a new
// thread pays the intern cost; subsequent calls are a load + compare.
template <auto Tag = []{}>
[[nodiscard]] MAYA_ALWAYS_INLINE uint16_t intern_const(StylePool& pool,
                                                       const Style& s) noexcept {
    (void)Tag;  // unused — its only purpose is to make each call site unique
    static thread_local uint16_t cached_id   = UINT16_MAX;
    static thread_local uint64_t cached_pool = 0;
    if (const uint64_t pid = pool.pool_id();
        cached_pool == pid && cached_id != UINT16_MAX) [[likely]]
    {
        return cached_id;
    }
    cached_id   = pool.intern(s);
    cached_pool = pool.pool_id();
    return cached_id;
}

// ============================================================================
// Canvas - A 2D grid of packed cells
// ============================================================================
// The core rendering surface. Two canvases (front and back) form the
// double-buffer system. Elements render into the back canvas; the diff
// algorithm compares back vs front to produce minimal terminal output.
//
// All coordinate systems are 0-based. The origin (0,0) is the top-left
// corner of the canvas.

// ============================================================================
// Canvas Stage — runtime type-state for paint/diff lifecycle
// ============================================================================
//
// A Canvas moves through three stages each frame:
//
//   Drained — every cell is the default_cell(). Entered via the
//             constructor, resize(), clear(), or clear_rows().
//             Painters may write; readers (serialize/diff/compose)
//             may still run but will see only blanks.
//
//   Painted — at least one cell has been written this frame.
//             Subsequent writes stay in Painted. Readers consume
//             this stage to emit the frame.
//
// The shippable form per render-roadmap A2: tracked at runtime as a
// `Stage` member, exposed via `stage()`, transitioned by the existing
// mutators, and asserted by readers in debug builds (MAYA_DEBUG_STAGE).
// The compile-time type-state version would be too API-invasive — it
// would template every Canvas user — but the runtime version still
// gives us the lifecycle contract in one named field plus optional
// runtime checking, which is what the production codepath needs.
enum class CanvasStage : uint8_t {
    Drained,   ///< Cells are all default_cell(); no writes since last drain.
    Painted,   ///< At least one cell has been written since last drain.
};

class Canvas {
public:
    Canvas() = default;

    Canvas(int width, int height, StylePool* pool);

    /// Current paint-lifecycle stage. Mutating ops (set/fill/blit/
    /// write_text/...) transition Drained → Painted; clearing ops
    /// (clear/clear_rows/resize) transition any → Drained.
    [[nodiscard]] CanvasStage stage() const noexcept { return stage_; }

    // -- Write-epoch tracking (renderer whole-entry blit skip) ---------------
    //
    // Every mutation that can change a row's cells (or its last_col_
    // bookkeeping) stamps that row with a fresh, canvas-monotonic epoch
    // value. A reader that records a row's epoch immediately after
    // writing it can later prove "nothing touched this row since my
    // write" with a single integer compare — the foundation of the
    // renderer's whole-entry blit skip, which collapses the per-frame
    // O(content-rows) `bulk_eq` re-verification of the preserved frozen
    // prefix into O(1) epoch compares per row (a 64-bit load vs a
    // width-cells SIMD compare).
    //
    // Soundness direction: a SPURIOUS stamp (stamping a row whose bytes
    // didn't actually change) merely disables a skip — the caller falls
    // back to the byte-compare path. A MISSING stamp would let a stale
    // skip serve wrong bytes, so every mutator stamps LIBERALLY (before
    // fine-grained clip analysis, when cheap). Epochs are strictly
    // increasing per stamp (shared monotonic counter), so a recorded
    // value can never be re-produced by a later, different write (no
    // ABA).
    //
    // The UID distinguishes canvases: a record made against a warmup
    // scratch canvas (or the fullscreen front/back pair) can never
    // match the live inline canvas.
    [[nodiscard]] std::uint64_t uid() const noexcept { return uid_; }
    /// Monotonic per-canvas stamp counter. A snapshot taken after a set
    /// of writes upper-bounds the epoch of every row those writes (and
    /// all earlier ones) stamped; any later mutation stamps strictly
    /// greater. Used with row_write_epoch() to prove a row range is
    /// untouched since the snapshot.
    [[nodiscard]] std::uint64_t write_epoch_counter() const noexcept {
        return epoch_counter_;
    }
    [[nodiscard]] MAYA_ALWAYS_INLINE std::uint64_t row_write_epoch(int y) const noexcept {
        if (static_cast<unsigned>(y) >= static_cast<unsigned>(height_))
            return ~std::uint64_t{0};   // OOB: never matches a recorded value
        return row_epoch_[static_cast<std::size_t>(y)];
    }
    /// Effective clip bounds active right now ([x0,x1) × [y0,y1)); the
    /// full canvas when no clip is pushed. Recorded by the renderer's
    /// blit-skip so a differing outer clip re-verifies bytes.
    struct ClipBounds { int x0, y0, x1, y1;
        bool operator==(const ClipBounds&) const = default; };
    [[nodiscard]] ClipBounds effective_clip() const noexcept {
        if (!has_clip_) return {0, 0, width_, height_};
        return {clip_x0_, clip_y0_, clip_x1_, clip_y1_};
    }

    // -- Cell access ----------------------------------------------------------

    /// Set a cell at (x, y). Clipped or out-of-bounds coordinates are silently ignored.
    ///
    /// Wide-character invariant: when `width == 1` (the lead cell of a
    /// wide glyph), the trail cell at (x+1, y) MUST also be writable — a
    /// lead without its trail is an orphan that the renderer will treat
    /// as a normal-width character (so the trail's previous content
    /// becomes visible). If x+1 would fall outside the canvas or the
    /// current clip rect, refuse the lead too. Same rule for the trail
    /// (`width == 2`): refuse it if its lead would be invisible. This
    /// removes a whole class of "blank cells appear inside text after a
    /// resize" bugs without the caller needing to know about clip math.
    MAYA_ALWAYS_INLINE void set(int x, int y, char32_t ch, uint16_t style_id, uint8_t width = 0) {
        if (__builtin_expect(!in_bounds(x, y), 0)) return;
        // Fast clip check using cached bounds — avoids vector access per cell.
        if (has_clip_ && __builtin_expect(
                x < clip_x0_ || x >= clip_x1_ || y < clip_y0_ || y >= clip_y1_, 0))
            return;

        // Wide-char paired-cell guard. The lead and trail must both pass
        // the clip; otherwise we'd create an orphan that draws stale
        // content from the previous frame. Cheap branch — `width == 0`
        // (the dominant case) skips both checks.
        if (__builtin_expect(width != 0, 0)) {
            if (width == 1) {
                // Lead — trail at (x+1, y) must be writable.
                if (x + 1 >= width_) return;
                if (has_clip_ && (x + 1 >= clip_x1_)) return;
            } else if (width == 2) {
                // Trail — lead at (x-1, y) must have been writable.
                if (x <= 0) return;
                if (has_clip_ && (x - 1 < clip_x0_)) return;
            }
        }

        auto idx = cell_index(x, y);
        uint64_t packed = Cell{ch, style_id, 0, width}.pack();
        // Value-gated write + stamp: a byte-identical set() is a no-op
        // for the canvas AND for the row's epoch. Chrome that repaints
        // the same border/rail/header cells every frame (the settled
        // tool panel) must not disarm the per-row blit skip of the
        // cached bodies sharing those rows — only a write that CHANGES
        // the cell does. Bookkeeping below still runs on the equal path
        // (cheap, and keeps semantics byte-for-byte identical to the
        // unconditional-store version).
        if (cells_[idx] != packed) {
            cells_[idx] = packed;
            stamp_row(y);
        }
        const bool visible = (ch != U' ' || style_id != 0);
        if (visible) {
            if (y > max_y_) max_y_ = y;
            // Per-row last-content column cache. Monotonically grows
            // toward the right edge until clear()/clear_rows()/resize()
            // resets it. Serialize/diff use this in lieu of a per-frame
            // backward linear scan to find row trim points.
            //
            // Wide-lead cells (width=1) reach two columns wide; advance
            // last_col_ to x+1 even though we haven't written the trail
            // cell yet (paired-cell guard above guarantees the trail
            // slot is in bounds, and the caller's very next set() call
            // will fill it with the placeholder). This closes the
            // mid-write window where a reader between the lead set()
            // and the trail set() would see last_col_=x and clip the
            // trail.
            const int new_last = (width == 1) ? x + 1 : x;
            if (new_last > last_col_[static_cast<std::size_t>(y)])
                last_col_[static_cast<std::size_t>(y)] = new_last;
        }
        stage_ = CanvasStage::Painted;
    }

    /// Read the cell at (x, y). Out-of-bounds returns a default cell.
    [[nodiscard]] Cell get(int x, int y) const noexcept;

    /// Direct access to the packed cell value at (x, y) for fast diff.
    [[nodiscard]] MAYA_ALWAYS_INLINE uint64_t get_packed(int x, int y) const noexcept {
        return cells_[static_cast<std::size_t>(y * width_ + x)];
    }

    /// Blit `n` packed cells from `src` into row `y`, starting at column
    /// `x`. Respects the current clip rect and updates max_y_ if the
    /// blitted row carries any non-blank content.
    ///
    /// Why this exists: the renderer's component cache stores pre-painted
    /// cell regions (packed uint64_t per cell) per hash_id entry. The
    /// cache-hit path needs to copy those cells into the live canvas
    /// without re-decoding each one. Per-cell `set()` would work but
    /// would pay style-pool intern + per-cell clip math; a row memcpy
    /// after the clip math is done once amortises both. `row_has_content`
    /// is the host's promise that at least one cell in the row is
    /// non-blank — saves the scan to update max_y_ when the host already
    /// knows from cache-population time.
    ///
    /// Wide-glyph invariant: the source buffer may contain wide-char
    /// lead (width==1) / trail (width==2) cell pairs. Clipping is by
    /// column, so a clip edge that falls between a lead and its trail
    /// would deposit an orphan half — the renderer/diff would then
    /// treat the orphan trail (placeholder) as a skipped cell, and the
    /// orphan lead as a normal-width glyph that overdraws the next
    /// cell. Both produce visible corruption ("phantom characters" or
    /// stale content bleeding out where the trail was clipped). We
    /// neutralise both edges by overwriting any orphan half at
    /// (x0, x1-1) with a default blank cell after the memcpy. The
    /// memcpy stays a single contiguous copy on the dominant
    /// no-wide-char path; the two-cell fix-up is unconditional but
    /// cheap.
    MAYA_ALWAYS_INLINE void blit_packed_row(int x, int y,
                                            const uint64_t* src,
                                            int n,
                                            bool row_has_content) {
        // Sentinel value: caller has no precomputed last-col info, do
        // the right-edge scan ourselves. -1 means "row is blank" only
        // when row_has_content is also false.
        blit_packed_row_impl(x, y, src, n, row_has_content,
                             /*known_last_col=*/INT_MIN);
    }

    /// Blit overload with a precomputed last-content column hint. The
    /// caller (typically the renderer's cells-cache fast path)
    /// captured the rightmost non-blank column when populating its
    /// cache, so we don't need to re-scan the row from the right on
    /// every paint. Pass -1 for a known-blank row; pass the absolute
    /// last-content column (in source-buffer coords, i.e. 0 ≤ c < n)
    /// otherwise.
    MAYA_ALWAYS_INLINE void blit_packed_row(int x, int y,
                                            const uint64_t* src,
                                            int n,
                                            bool row_has_content,
                                            int known_last_col) {
        blit_packed_row_impl(x, y, src, n, row_has_content, known_last_col);
    }

    /// Cache-fast blit that SKIPS the memcpy when the destination row
    /// already holds byte-identical cells to `src` over the clipped
    /// span. Purpose-built for the renderer's frozen (immutable,
    /// hash-keyed) component blit: a tall settled tool card is
    /// re-blitted every frame to the SAME canvas rows with the SAME
    /// cached cells, so once the canvas holds those bytes the copy is
    /// pure waste. The equality probe (`simd::bulk_eq`, read-only,
    /// short-circuits on first diff) is strictly cheaper than the
    /// memcpy it elides, and the bookkeeping (max_y_ / last_col_)
    /// is still applied so content_height() and the diff scan see the
    /// identical result they would from a full blit.
    ///
    /// SAFETY: when the destination is NOT byte-identical (layout
    /// shifted the entry, the region was cleared, a wide-glyph edge
    /// differs) it falls through to a full blit_packed_row_impl. The
    /// worst case is therefore a full blit plus one wasted compare —
    /// never a stale row. Correctness does not depend on any
    /// cross-frame assumption; only the WORK is saved, and only when
    /// the bytes provably already match.
    ///
    /// Returns true if the fast skip fired (bytes already matched),
    /// false if it fell through to a full blit.
    MAYA_ALWAYS_INLINE bool blit_packed_row_cached(int x, int y,
                                                   const uint64_t* src,
                                                   int n,
                                                   bool row_has_content,
                                                   int known_last_col) {
        if (y < 0 || y >= height_ || n <= 0) return false;
        int x0 = std::max(0, x);
        int x1 = std::min(width_, x + n);
        if (has_clip_) {
            if (y < clip_y0_ || y >= clip_y1_) return false;
            x0 = std::max(x0, clip_x0_);
            x1 = std::min(x1, clip_x1_);
        }
        if (x1 <= x0) return false;
        const std::size_t dst_off = static_cast<std::size_t>(y * width_ + x0);
        const int src_off = x0 - x;
        const int count = x1 - x0;
        // Read-only equality probe. If the destination already equals
        // the source over the clipped span, the memcpy + wide-glyph
        // repair would be a no-op — skip them. max_y_ / last_col_ still
        // need updating (they were reset by the frame's clear), so we
        // fall into the same bookkeeping the full path runs, minus the
        // writes.
        if (!simd::bulk_eq(&cells_[dst_off], src + src_off,
                           static_cast<std::size_t>(count))) {
            blit_packed_row_impl(x, y, src, n, row_has_content, known_last_col);
            return false;
        }
        // Bytes already match. Apply the max_y_ / last_col_ bookkeeping
        // exactly as blit_packed_row_impl would, so downstream readers
        // (content_height, the diff scan) are unaffected by the skip.
        if (row_has_content) {
            const uint64_t blank = default_cell();
            int actual_last = -1;
            if (known_last_col != INT_MIN) {
                if (known_last_col >= 0) {
                    const int abs_col = x + known_last_col;
                    if (abs_col >= x0 && abs_col < x1) {
                        const std::size_t hint_off =
                            dst_off + static_cast<std::size_t>(abs_col - x0);
                        if (cells_[hint_off] != blank) actual_last = abs_col;
                        else {
                            for (int i = static_cast<int>(hint_off - dst_off) - 1;
                                 i >= 0; --i)
                                if (cells_[dst_off + static_cast<std::size_t>(i)]
                                        != blank) { actual_last = x0 + i; break; }
                        }
                    } else if (abs_col >= x1) {
                        for (int i = count - 1; i >= 0; --i)
                            if (cells_[dst_off + static_cast<std::size_t>(i)]
                                    != blank) { actual_last = x0 + i; break; }
                    }
                }
            } else {
                for (int i = count - 1; i >= 0; --i)
                    if (cells_[dst_off + static_cast<std::size_t>(i)] != blank) {
                        actual_last = x0 + i; break;
                    }
            }
            if (actual_last >= 0) {
                if (y > max_y_) max_y_ = y;
                if (actual_last > last_col_[static_cast<std::size_t>(y)])
                    last_col_[static_cast<std::size_t>(y)] = actual_last;
            }
        }
        stage_ = CanvasStage::Painted;
        return true;
    }

private:
    MAYA_ALWAYS_INLINE void blit_packed_row_impl(int x, int y,
                                                 const uint64_t* src,
                                                 int n,
                                                 bool row_has_content,
                                                 int known_last_col) {
        if (y < 0 || y >= height_) return;
        if (n <= 0) return;
        int x0 = std::max(0, x);
        int x1 = std::min(width_, x + n);
        if (has_clip_) {
            if (y < clip_y0_ || y >= clip_y1_) return;
            x0 = std::max(x0, clip_x0_);
            x1 = std::min(x1, clip_x1_);
        }
        if (x1 <= x0) return;
        stamp_row(y);

        std::size_t dst_off = static_cast<std::size_t>(y * width_ + x0);
        int src_off = x0 - x;
        int count = x1 - x0;
        // Plain memcpy of packed cells. cells_ is std::vector<uint64_t>
        // so alignment is guaranteed; src is also uint64_t-aligned by
        // the caller (cache entry storage).
        std::memcpy(&cells_[dst_off], src + src_off,
                    static_cast<std::size_t>(count) * sizeof(uint64_t));

        // Wide-glyph clip-edge repair. Decode the width byte (bits
        // 56-63 of the packed cell) at the two cells we just wrote
        // that sit on the clip boundary. A trail (width==2) at the
        // LEFT edge means we clipped its lead away — replace the
        // orphan with a blank. A lead (width==1) at the RIGHT edge
        // means its trail lies outside the blitted region — same
        // treatment. These are O(1) checks (no loop), unconditional
        // (branch-predicts well: dominant case is width==0), and
        // restore the invariant `set()` maintains for direct writes.
        const uint64_t blank = default_cell();
        {
            const uint64_t left = cells_[dst_off];
            if (__builtin_expect((left >> 56) == 2, 0))
                cells_[dst_off] = blank;
        }
        if (count >= 1) {
            const std::size_t right_off = dst_off + static_cast<std::size_t>(count - 1);
            const uint64_t right = cells_[right_off];
            if (__builtin_expect((right >> 56) == 1, 0))
                cells_[right_off] = blank;
        }

        if (row_has_content) {
            int actual_last = -1;
            if (known_last_col != INT_MIN) {
                // Caller-supplied hint. Map src-relative column to
                // canvas-absolute column, then intersect with the
                // clipped span [x0, x1). known_last_col == -1 means
                // the row is genuinely blank in the source — nothing
                // to update.
                if (known_last_col >= 0) {
                    const int abs_col = x + known_last_col;
                    if (abs_col >= x0 && abs_col < x1) {
                        // Hint is inside the clipped span. Check that
                        // the wide-glyph repair above didn't blank it.
                        const std::size_t hint_off =
                            dst_off + static_cast<std::size_t>(abs_col - x0);
                        if (cells_[hint_off] != blank) {
                            actual_last = abs_col;
                        } else {
                            // Right-edge repair blanked the hint cell.
                            // Fall back to a scan from where the
                            // hint pointed — typically just one or
                            // two cells back.
                            for (int i = static_cast<int>(hint_off - dst_off) - 1;
                                 i >= 0; --i) {
                                if (cells_[dst_off +
                                           static_cast<std::size_t>(i)]
                                        != blank) {
                                    actual_last = x0 + i;
                                    break;
                                }
                            }
                        }
                    } else if (abs_col >= x1) {
                        // Hint sits past the clip. The last visible
                        // cell of our blit is at x1-1; scan back from
                        // there. Most cells in that tail are likely
                        // non-blank (we clipped off the right edge of
                        // a content row), so this is typically a
                        // single-iteration scan.
                        for (int i = count - 1; i >= 0; --i) {
                            if (cells_[dst_off +
                                       static_cast<std::size_t>(i)]
                                    != blank) {
                                actual_last = x0 + i;
                                break;
                            }
                        }
                    }
                    // abs_col < x0: hint sits before our clipped
                    // region. The blitted slice is entirely past the
                    // last content cell — row is blank in our view.
                }
            } else {
                // No hint — scan from the right edge. Same code as
                // before the overload was added.
                for (int i = count - 1; i >= 0; --i) {
                    if (cells_[dst_off + static_cast<std::size_t>(i)] != blank) {
                        actual_last = x0 + i;
                        break;
                    }
                }
            }
            // Only bump max_y_ if we actually have visible content in
            // this row — the wide-glyph clip-edge repair above can have
            // blanked the entire blitted span, in which case the row
            // is genuinely empty and bumping max_y_ would lie to
            // content_height() (claiming content where there is none
            // and forcing the diff path to walk + emit empty rows).
            // Matches the discipline set() upholds: max_y_ tracks
            // visible writes, not attempted writes.
            if (actual_last >= 0) {
                if (y > max_y_) max_y_ = y;
                if (actual_last > last_col_[static_cast<std::size_t>(y)])
                    last_col_[static_cast<std::size_t>(y)] = actual_last;
            }
        }
        stage_ = CanvasStage::Painted;
    }

public:

    // -- Overflow reporting ---------------------------------------------------

    /// A write that did not fit the clip rect, reported at the moment the
    /// cells were dropped.
    struct ClipOverflow {
        int x = 0;             ///< where the write started
        int y = 0;             ///< the row
        int wanted = 0;        ///< display columns the text asked for
        int edge = 0;          ///< the column past which cells were discarded
        std::string_view text; ///< what was being written
    };

    /// Report writes that run past the clip edge.
    ///
    /// Clipping is how a terminal renderer loses information: cells past
    /// the edge are dropped without a word, and the dropped END is where
    /// values live — a number beside a bar, the tail of a path, the right
    /// half of a figure. The reader gets a plausible-looking frame and no
    /// way to know something is missing, which is exactly the condition
    /// under which width bugs survive a green test suite.
    ///
    /// Installing a reporter turns that silence into a message naming the
    /// text and the column. It is opt-in because clipping is also
    /// legitimate and routine — a scroll viewport clips by design, an
    /// ellipsised label has already made its choice — so this is a
    /// debugging instrument and a CI gate, not an invariant.
    void on_clip_overflow(std::function<void(const ClipOverflow&)> fn) {
        clip_report_ = std::move(fn);
    }

    // -- Text rendering -------------------------------------------------------

    /// Write a UTF-8 string starting at (x, y).
    void write_text(int x, int y, std::string_view text, uint16_t style_id);

    // -- Region operations ----------------------------------------------------

    /// Fill a rectangular region with a character and style.
    void fill(Rect region, char32_t ch, uint16_t style_id);

    /// Reset all cells to space with the default style. Clears damage.
    void clear();

    // ── Hardware-cursor hint ─────────────────────────────────────
    // An optional (x, y) the frame's focused caret owner records at
    // paint time. It is a HINT, not a cell: it lives outside the packed
    // cell grid, so the diff, the shadow-of-wire hashes, the component
    // cache and the Witness Chain never see it — identical cells still
    // produce identical frames whether or not a hint is set. The inline
    // serializer consumes it in the frame epilogue: hint present + row
    // on-screen ⇒ the real terminal cursor is moved there and SHOWN
    // (DECTCEM) instead of being parked hidden at the frame's bottom-
    // left corner. This is what lets a composer use the terminal's OWN
    // caret — native blink at the terminal's cadence with zero
    // animation-frame wake-ups, IME candidate windows anchored at the
    // true caret cell, and screen readers tracking the real cursor.
    //
    // Exactly one owner per frame: the LAST set_cursor_hint call wins
    // (matches every immediate-mode precedent — ncurses leaves the
    // cursor at the last move(), ratatui at the last set_cursor). A
    // frame with no focused caret simply never calls it, and the
    // serializer falls back to park-and-hide. clear()/clear_rows()
    // reset the hint with the rest of the frame state.
    struct CursorHint { int x = 0; int y = 0; };
    void set_cursor_hint(int x, int y) noexcept {
        if (in_bounds(x, y)) cursor_hint_ = CursorHint{x, y};
    }
    void clear_cursor_hint() noexcept { cursor_hint_.reset(); }
    [[nodiscard]] const std::optional<CursorHint>& cursor_hint() const noexcept {
        return cursor_hint_;
    }

    /// Clear only rows [0, n). Much faster than clear() for inline mode
    /// where only a small portion of a tall canvas has content.
    void clear_rows(int n);

    /// Clear only rows [keep_top, height), PRESERVING rows
    /// [0, keep_top) intact from the previous frame. Used by the
    /// inline render path to skip re-clearing (and, via
    /// blit_packed_row_cached, re-painting) the immutable frozen
    /// prefix that has already overflowed the viewport into native
    /// scrollback. max_y_ and last_col_ for the preserved rows are
    /// kept as-is; max_y_ is re-derived across the whole canvas so
    /// content_height() stays exact whether the tallest content is
    /// above or below keep_top. `keep_top <= 0` degenerates to a full
    /// clear(); `keep_top >= height` clears nothing.
    ///
    /// `clear_bottom` caps the cleared tail at row `clear_bottom`
    /// (exclusive): rows [max(keep_top, ...), min(height_, clear_bottom))
    /// are blanked, and rows [clear_bottom, height_) are left untouched.
    /// The default (INT_MAX) clears the whole tail. Callers that grow the
    /// canvas with headroom slack (inline auto-height) pass the height
    /// they will actually PAINT this frame, so the never-painted slack
    /// above it isn't re-blanked every frame — that slack scales with the
    /// turn length (25% headroom), so clearing it was an O(rows)/frame
    /// fill. Leaving it dirty is safe: nothing paints there, so last_col_
    /// stays -1 (set by the last resize) and the diff / content_height
    /// never read it.
    void clear_below(int keep_top, int clear_bottom = INT_MAX);

    /// Drain a single row back to default_cell() and reset its
    /// per-row bookkeeping (last_col_[y] → -1, max_y_ rescanned if y
    /// was the previous max). Use this from a widget that performs
    /// multi-pass paint within a single frame and wants to discard
    /// an earlier pass's contribution to that row's trim metadata
    /// before the next pass writes shorter content.
    ///
    /// Without this hook, `set()` / `write_text()` only ever GROW
    /// `last_col_[y]` — a row written wider in pass 1 then
    /// overwritten shorter in pass 2 keeps the wider trim, and the
    /// diff path emits cells out to that stale extent before EL'ing.
    /// The cells in [shorter, wider) on canvas are blank by then so
    /// the visible result is correct, but the diff burns bytes
    /// re-emitting blanks per frame instead of letting the EL
    /// optimisation handle them in one shot.
    void clear_row(int y) noexcept;

    /// The highest row index that received non-space content since last clear.
    /// Returns -1 if nothing was written. O(1) — avoids scanning the canvas.
    [[nodiscard]] int max_content_row() const noexcept { return max_y_; }

    /// The highest column index in row `y` with non-blank+default content
    /// since the last clear. Returns -1 if the row is entirely blank.
    /// O(1) — maintained incrementally by set()/write_text(). Used by
    /// serialize/diff in place of a per-frame backward linear scan.
    [[nodiscard]] int last_content_col(int y) const noexcept {
        if (static_cast<unsigned>(y) >= static_cast<unsigned>(height_)) return -1;
        return last_col_[static_cast<std::size_t>(y)];
    }

    // -- Clip stack -----------------------------------------------------------

    /// Push a clipping rectangle.
    void push_clip(Rect clip);

    /// Pop the most recent clipping rectangle.
    void pop_clip();

    /// Returns true if (x, y) falls outside the current clip region.
    [[nodiscard]] bool is_clipped(int x, int y) const noexcept;

    /// Number of clip rectangles currently on the stack — exposed so the
    /// renderer can assert "stack is empty between frames" and recover
    /// gracefully if a paint callback threw between push and pop.
    [[nodiscard]] std::size_t clip_depth() const noexcept {
        return clip_stack_.size();
    }

    /// Force-clear the clip stack. Called between frames to recover from
    /// any unmatched push_clip/pop_clip pair (e.g. after a paint callback
    /// threw and unwound past pop_clip). Cheap — empties a vector.
    void reset_clips() noexcept;

    /// RAII clip guard. Pushing/popping in pairs is required for
    /// correctness, and writing the pop manually is fragile in the
    /// presence of early returns or exceptions. Use this instead:
    ///
    ///   if (auto _ = canvas.clip_scope(rect)) {
    ///       paint_children();
    ///   }
    ///
    /// The pop happens deterministically when the guard goes out of scope,
    /// even if `paint_children` throws. The boolean conversion always
    /// returns true so the `if` is just syntactic sugar that scopes the
    /// guard to the controlled statement.
    class [[nodiscard]] ClipScope {
        Canvas* c_;
    public:
        ClipScope(Canvas& c, Rect clip) : c_(&c) { c_->push_clip(clip); }
        ~ClipScope() { if (c_) c_->pop_clip(); }
        ClipScope(const ClipScope&) = delete;
        ClipScope& operator=(const ClipScope&) = delete;
        ClipScope(ClipScope&& o) noexcept : c_(o.c_) { o.c_ = nullptr; }
        ClipScope& operator=(ClipScope&&) = delete;
        explicit operator bool() const noexcept { return true; }
    };

    [[nodiscard]] ClipScope clip_scope(Rect clip) { return {*this, clip}; }

    // -- Sizing ---------------------------------------------------------------

    /// Resize the canvas, clearing all content.
    void resize(int w, int h);

    [[nodiscard]] int width()  const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    // -- Damage tracking ------------------------------------------------------

    void mark_damage(Rect region);
    [[nodiscard]] Rect damage() const noexcept { return damage_; }
    void reset_damage() noexcept { damage_ = Rect{}; }
    void mark_all_damaged();

    // -- Style pool access ----------------------------------------------------

    [[nodiscard]] StylePool* style_pool() const noexcept { return style_pool_; }
    void set_style_pool(StylePool* pool) noexcept { style_pool_ = pool; }

    // -- Raw buffer access (for diff) -----------------------------------------

    [[nodiscard]] const uint64_t* cells() const noexcept { return cells_.data(); }
    [[nodiscard]] std::size_t cell_count() const noexcept { return cells_.size(); }

private:
    [[nodiscard]] MAYA_ALWAYS_INLINE bool in_bounds(int x, int y) const noexcept {
        return static_cast<unsigned>(x) < static_cast<unsigned>(width_)
            && static_cast<unsigned>(y) < static_cast<unsigned>(height_);
    }

    [[nodiscard]] MAYA_ALWAYS_INLINE std::size_t cell_index(int x, int y) const noexcept {
        return static_cast<std::size_t>(y * width_ + x);
    }

    [[nodiscard]] static uint64_t default_cell() noexcept {
        return Cell{U' ', 0, 0, 0}.pack();
    }

    [[nodiscard]] Rect full_rect() const noexcept {
        return {{Columns{0}, Rows{0}}, {Columns{width_}, Rows{height_}}};
    }

    void update_clip_cache() noexcept {
        if (clip_stack_.empty()) {
            has_clip_ = false;
        } else {
            has_clip_ = true;
            const Rect& c = clip_stack_.back();
            clip_x0_ = c.left().value;
            clip_y0_ = c.top().value;
            clip_x1_ = c.right().value;
            clip_y1_ = c.bottom().value;
        }
    }

    AlignedBuffer cells_;
    int width_  = 0;
    int height_ = 0;
    int max_y_  = -1;  // highest row with non-space content (O(1) content_height)
    // Per-row last-content column. `-1` = row is blank-default. Updated
    // by set()/fill()/write_text(); reset by clear()/clear_rows()/resize().
    // Size always equals height_. Reads via last_content_col() are O(1).
    std::vector<int> last_col_;
    // Per-row write epoch (see the public accessors above). Size always
    // equals height_. 0 = never stamped since construction.
    std::vector<std::uint64_t> row_epoch_;
    std::uint64_t epoch_counter_ = 0;
    std::uint64_t uid_ = next_canvas_uid();

    [[nodiscard]] static std::uint64_t next_canvas_uid() noexcept;

    MAYA_ALWAYS_INLINE void stamp_row(int y) noexcept {
        row_epoch_[static_cast<std::size_t>(y)] = ++epoch_counter_;
        ++stamp_row_count_;
    }
    // Stamp rows [y0, y1) with one fresh epoch value (batch mutators —
    // fill/clear*). A shared value within the batch is fine: it is still
    // strictly greater than every previously recorded epoch.
    void stamp_rows(int y0, int y1) noexcept {
        if (y0 < 0) y0 = 0;
        if (y1 > height_) y1 = height_;
        if (y0 >= y1) return;
        const std::uint64_t e = ++epoch_counter_;
        for (int y = y0; y < y1; ++y)
            row_epoch_[static_cast<std::size_t>(y)] = e;
        stamp_row_count_ += static_cast<std::uint64_t>(y1 - y0);
    }
public:
    // Telemetry: total rows stamped over the canvas lifetime (profiler
    // diffs across frames). Not part of any correctness path.
    [[nodiscard]] std::uint64_t stamp_row_count() const noexcept {
        return stamp_row_count_;
    }
private:
    std::uint64_t stamp_row_count_ = 0;
    StylePool* style_pool_ = nullptr;
    Rect damage_{};
    std::vector<Rect> clip_stack_;
    // Cached clip bounds — avoids vector back() on every set() call.
    bool has_clip_ = false;
    int clip_x0_ = 0, clip_y0_ = 0, clip_x1_ = 0, clip_y1_ = 0;
    // Overflow reporter. Null in every normal frame, so the hot path pays
    // one predictable null check and nothing else.
    std::function<void(const ClipOverflow&)> clip_report_;
    // A2 lifecycle stage. Mutators flip to Painted; clear/clear_rows/
    // resize flip back to Drained. Lives next to other small flags so
    // it shares a cache line with has_clip_ / clip bounds — no
    // additional memory traffic in the hot path.
    CanvasStage stage_ = CanvasStage::Drained;
    // Hardware-cursor hint — see the public accessor block. Optional so
    // "no focused caret this frame" is representable; reset by clear()
    // and clear_rows() alongside the rest of the frame state.
    std::optional<CursorHint> cursor_hint_{};
};

} // namespace maya
