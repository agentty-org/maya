#pragma once
// maya::panel::detail — caret splicing for line-edited items (Text, Path).
//
// The caret is drawn IN the text rather than as a hardware cursor: it then
// survives the scroll viewport's clipping and needs no separate positioning
// pass. But a value longer than its column is truncated at the END, so typing
// past the column width used to push the caret — and everything you were
// typing — off the right edge. You could still edit; you just could not see it.
//
// So the value scrolls horizontally under a fixed-width window, the way every
// single-line editor does. `budget` is the columns the cell may use; 0 means
// "unknown", which keeps the old whole-string behaviour for callers that have
// not measured (the flex layout will still clip it, just without the window).
//
// U+2039/203A single angle quotes mark a horizontal scroll — deliberately not
// the … that TruncateEnd uses, so "there is more text this way" reads
// differently from "this was cut off".

#include <algorithm>
#include <string>

#include "../../../element/text.hpp"   // string_width

namespace maya::panel::detail {

[[nodiscard]] inline std::string with_caret(const std::string& v,
                                            std::size_t caret,
                                            int budget = 0,
                                            std::size_t* caret_at = nullptr) {
    if (caret == std::string::npos) return v;
    const std::size_t at = std::min(caret, v.size());

    static constexpr const char* kBar = "\xe2\x96\x88";   // █ FULL BLOCK
    if (budget <= 2 || string_width(v) + 1 <= budget) {
        if (caret_at) *caret_at = at;
        return v.substr(0, at) + kBar + v.substr(at);
    }

    // Window the value so the caret sits inside it, biased to show what comes
    // BEFORE the caret (you are usually appending, and the text you just typed
    // matters more than the text you have not reached).
    const int win = budget - 2;                     // room for the markers
    std::size_t begin = 0;
    if (static_cast<int>(at) > win) begin = at - static_cast<std::size_t>(win);
    // Never split a UTF-8 sequence.
    while (begin > 0 && (static_cast<unsigned char>(v[begin]) & 0xC0u) == 0x80u) --begin;

    std::size_t end = begin;
    int used = 0;
    while (end < v.size() && used < win) {
        std::size_t next = end + 1;
        while (next < v.size() && (static_cast<unsigned char>(v[next]) & 0xC0u) == 0x80u) ++next;
        used += string_width(v.substr(end, next - end));
        end = next;
    }

    std::string out;
    if (begin > 0)     out += "\xe2\x80\xb9";        // ‹
    out += v.substr(begin, at - begin);
    if (caret_at) *caret_at = out.size();
    out += kBar;
    out += v.substr(at, end - at);
    if (end < v.size()) out += "\xe2\x80\xba";       // ›
    return out;
}

} // namespace maya::panel::detail
