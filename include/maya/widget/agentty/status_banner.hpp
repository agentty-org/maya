#pragma once
// maya::widget::StatusBanner — transient toast row with leading edge mark.
//
// Single-row banner used in the status bar's slot for ephemeral
// status / error messages:
//
//   ▎ status text                   (info  — muted edge mark, italic)
//   ▎⚠  status text                  (warn  — yellow edge + glyph)
//   ▎⚠  status text                  (error — red edge + glyph)
//
// When `text` is empty, renders a 1-cell blank placeholder so the
// surrounding panel's row count stays fixed (no vertical jitter when
// a toast appears or disappears).
//
// `kind` decides both the leading-edge color and the background tint
// used by `maya::StatusBar` when it promotes the banner to the full-
// width toast row. Info → terminal-neutral, Warn → amber, Error → red.
// The legacy `is_error` flag still works (true ⇒ Error) but new code
// should set `kind` directly so retries/awaiting-permission/etc. can
// reach Warn without going through Error's red palette.
//
//   maya::StatusBanner{{
//       .text = m.s.status,
//       .kind = m.s.status.starts_with("error:")
//                   ? maya::StatusBanner::Kind::Error
//                   : maya::StatusBanner::Kind::Info,
//   }}.build();

#include <string>
#include <utility>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"

namespace maya {

class StatusBanner {
public:
    // Severity of the banner — drives both edge color (standalone use)
    // and toast background tint (when promoted by `maya::StatusBar`).
    enum class Kind : uint8_t {
        Info,    // generic transient status: "context compacted", …
        Warn,    // recoverable trouble: retry, awaiting permission, …
        Error,   // failure: rate limit, transport gave up, …
    };

    struct Config {
        std::string text;                 // empty = blank slot
        Kind        kind     = Kind::Info;
        bool        is_error = false;     // legacy: true overrides kind → Error
        Color       muted_color = Color::slot(ThemeSlot::Muted);
        Color       warn_color  = Color::slot(ThemeSlot::Warning);
        Color       error_color = Color::slot(ThemeSlot::Error);

        // Resolves the legacy `is_error` flag against the explicit
        // `kind` so widgets only have to reason about one axis.
        [[nodiscard]] Kind effective_kind() const noexcept {
            return is_error ? Kind::Error : kind;
        }

        [[nodiscard]] Color edge_color() const noexcept {
            switch (effective_kind()) {
                case Kind::Info:  return muted_color;
                case Kind::Warn:  return warn_color;
                case Kind::Error: return error_color;
            }
            return muted_color;
        }
    };

    explicit StatusBanner(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (cfg_.text.empty()) return text(" ");      // blank-but-present

        const Kind k  = cfg_.effective_kind();
        const Color bc = cfg_.edge_color();
        const char* glyph = (k == Kind::Info) ? "  "
                                              : " \xe2\x9a\xa0  ";    // ⚠
        return h(
            text(" "),
            text("\xe2\x96\x8e", Style{}.with_fg(bc)),                       // ▎
            text(glyph, Style{}.with_fg(bc)),
            text(cfg_.text, Style{}.with_fg(bc).with_italic())
        ).build();
    }

private:
    Config cfg_;
};

} // namespace maya
