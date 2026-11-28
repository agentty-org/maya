#!/usr/bin/env python3
"""
Generate maya/include/maya/widget/agentty/markdown/engine/cm_unicode_table.hpp from
pinned UCD data.

CommonMark 0.31.2 needs three pieces of Unicode classification its parser
cannot get from ASCII bytes alone:

  * "Unicode whitespace character" (§2.1) — Zs general category plus tab,
    line feed, form feed, carriage return. Drives emphasis flanking.
  * "Unicode punctuation character" (§2.1) — P (punctuation) OR S (symbol)
    general categories. Also drives flanking ($, £, € are punctuation).
  * Reference-label case folding (§6.3) — labels match after a full Unicode
    case fold (ẞ folds to "ss", Α folds to α, …), not an ASCII lower-case.

Inputs (committed under maya/data/):
  - UnicodeData.txt   — general categories (P*/S* → punctuation, Zs → space)
  - CaseFolding.txt   — C (common) + F (full) case-fold mappings

Output:
  - maya/include/maya/widget/agentty/markdown/engine/cm_unicode_table.hpp
        kPunctRanges       sorted/coalesced P*|S* code-point ranges
        kWhitespaceRanges  sorted/coalesced Zs (+ ASCII ws is handled in C++)
        kCaseFold          sorted {code point -> folded UTF-8} rows

Run from anywhere:
    python maya/scripts/gen_cm_unicode.py

The generated header is checked in; re-run only when bumping Unicode.
"""

from __future__ import annotations

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent  # repo / maya
DATA = ROOT / "data"
OUT = (ROOT / "include" / "maya" / "widget" / "markdown" / "engine"
       / "cm_unicode_table.hpp")


def parse_categories(path: pathlib.Path):
    """Yield (codepoint, category) for every assigned code point.

    UnicodeData.txt encodes large blocks as a pair of `<…, First>` / `<…,
    Last>` rows sharing one category; expand those to the full range.
    """
    pending_first = None
    with path.open(encoding="utf-8") as fh:
        for raw in fh:
            f = raw.rstrip("\n").split(";")
            if len(f) < 3:
                continue
            cp = int(f[0], 16)
            name, cat = f[1], f[2]
            if name.endswith(", First>"):
                pending_first = (cp, cat)
                continue
            if name.endswith(", Last>"):
                first, fcat = pending_first
                for c in range(first, cp + 1):
                    yield (c, fcat)
                pending_first = None
                continue
            yield (cp, cat)


def coalesce(cps) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for cp in sorted(cps):
        if out and cp <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(cp, out[-1][1]))
        else:
            out.append((cp, cp))
    return out


def parse_case_folding(path: pathlib.Path) -> dict[int, list[int]]:
    """code point -> list of folded code points, using C and F statuses
    (full case folding; F expands e.g. ẞ -> s s)."""
    folds: dict[int, list[int]] = {}
    with path.open(encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            cp_s, status, mapping = (p.strip() for p in line.split(";")[:3])
            if status not in ("C", "F"):
                continue
            folds[int(cp_s, 16)] = [int(x, 16) for x in mapping.split()]
    return folds


def utf8_escape(cps: list[int]) -> str:
    out = []
    for cp in cps:
        for b in chr(cp).encode("utf-8"):
            out.append(f"\\x{b:02x}")
    return "".join(out)


def header_meta(path: pathlib.Path) -> str:
    with path.open(encoding="utf-8") as fh:
        for raw in fh:
            s = raw.strip()
            if s.startswith("#") and s != "#":
                return s.lstrip("# ").strip()
    return "(unknown)"


def emit_ranges(name: str, ranges: list[tuple[int, int]]) -> str:
    lines = [f"inline constexpr CpRange {name}[] = {{"]
    row = "    "
    for first, last in ranges:
        cell = f"{{0x{first:04X},0x{last:04X}}}, "
        if len(row) + len(cell) > 92:
            lines.append(row.rstrip())
            row = "    "
        row += cell
    if row.strip():
        lines.append(row.rstrip())
    lines.append("};")
    lines.append(f"inline constexpr int {name}Count = "
                 f"static_cast<int>(sizeof({name}) / sizeof({name}[0]));")
    return "\n".join(lines)


def main() -> int:
    ud = DATA / "UnicodeData.txt"
    cf = DATA / "CaseFolding.txt"
    if not ud.exists() or not cf.exists():
        sys.stderr.write(
            f"missing UCD files in {DATA} — drop UnicodeData.txt and "
            "CaseFolding.txt from https://www.unicode.org/Public/<ver>/ucd/ "
            "and re-run.\n")
        return 1

    cats = list(parse_categories(ud))
    punct = coalesce(cp for cp, c in cats if c[0] in ("P", "S"))
    space = coalesce(cp for cp, c in cats if c == "Zs")

    folds = parse_case_folding(cf)
    fold_rows = sorted(folds.items())

    fold_emitted = []
    for cp, mapped in fold_rows:
        fold_emitted.append(
            f'    {{0x{cp:04X}, "{utf8_escape(mapped)}"}},')

    # UnicodeData.txt carries no header comment; borrow the revision tag
    # from CaseFolding.txt (e.g. "CaseFolding-16.0.0.txt") so the generated
    # banner records which Unicode version produced these tables.
    cf_meta = header_meta(cf)
    ver = "?"
    m = __import__("re").search(r"-(\d+\.\d+\.\d+)", cf_meta)
    if m:
        ver = m.group(1)

    body = f"""\
#pragma once
// AUTO-GENERATED — DO NOT EDIT BY HAND.
// Regenerate with: python maya/scripts/gen_cm_unicode.py
//
// Source files (pinned under maya/data/, Unicode {ver}):
//   UnicodeData.txt — general categories (P*/S* punctuation, Zs whitespace)
//   CaseFolding.txt — {cf_meta}
//
// CommonMark 0.31.2 §2.1 / §6.3 Unicode classification for the engine:
//   kPunctRanges       code points in general category P* or S*
//                      ("Unicode punctuation character" — drives flanking)
//   kWhitespaceRanges  general category Zs ("Unicode whitespace"); the
//                      ASCII controls (tab/LF/FF/CR/space) are folded in by
//                      the C++ predicate, not duplicated here.
//   kCaseFold          full case-fold (CaseFolding.txt statuses C + F) used
//                      to normalize reference labels; rows sorted by cp for
//                      binary search, value is the folded UTF-8 sequence.

#include <cstdint>
#include <string_view>

namespace maya::md_detail::engine {{

struct CpRange {{ char32_t first; char32_t last; }};

{emit_ranges('kPunctRanges', punct)}

{emit_ranges('kWhitespaceRanges', space)}

struct FoldRow {{ char32_t cp; std::string_view folded; }};

inline constexpr FoldRow kCaseFold[] = {{
{chr(10).join(fold_emitted)}
}};
inline constexpr int kCaseFoldCount =
    static_cast<int>(sizeof(kCaseFold) / sizeof(kCaseFold[0]));

}} // namespace maya::md_detail::engine
"""

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(body, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT.parent)}: {len(punct)} punct ranges, "
          f"{len(space)} space ranges, {len(fold_rows)} case-fold rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
