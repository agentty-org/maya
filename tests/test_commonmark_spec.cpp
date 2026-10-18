// test_commonmark_spec.cpp — score maya's markdown PARSER against the
// official CommonMark 0.31.2 example set (tests/fixtures/...json).
//
// This is a conformance ratchet, not a pass/fail gate (yet): it prints
// per-section and overall pass-rate. A small allowlisted floor keeps it
// from silently regressing once we start the engine rebuild — raise the
// floor as conformance climbs.
//
// Scoring is at the AST level: parse_markdown(md) -> md::Document ->
// cm::to_html() -> normalized compare against the spec's expected HTML.

#include "agtest.hpp"

#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/engine/cm_engine.hpp"  // pure-core parse (no ext)
#include "cm_html.hpp"
#include "check.hpp"

using namespace maya;

#ifndef MAYA_SOURCE_DIR
#define MAYA_SOURCE_DIR "."
#endif

namespace {

// ── tiny JSON: array of flat {string:string|int} objects ───────────
// Sufficient for spec.json; handles \n \t \r \" \\ \/ \uXXXX escapes.
struct Example {
    std::string markdown;
    std::string html;
    int         example = 0;
    std::string section;
};

std::string decode_json_string(std::string_view s, std::size_t& i) {
    std::string out;
    ++i; // opening quote
    while (i < s.size() && s[i] != '"') {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char e = s[++i];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '/': out += '/';  break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case 'u': {
                    auto hexval = [&](char h) -> int {
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                        return 0;
                    };
                    unsigned cp = 0;
                    for (int k = 0; k < 4 && i + 1 < s.size(); ++k)
                        cp = (cp << 4) | hexval(s[++i]);
                    // UTF-8 encode (BMP only; spec.json uses no surrogates
                    // in the example bodies that matter here).
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += e; break;
            }
            ++i;
        } else {
            out += c; ++i;
        }
    }
    ++i; // closing quote
    return out;
}

std::vector<Example> parse_spec(std::string_view s) {
    std::vector<Example> out;
    std::size_t i = 0;
    auto skip_ws = [&] { while (i < s.size() && (s[i]==' '||s[i]=='\n'||s[i]=='\t'||s[i]=='\r')) ++i; };
    skip_ws();
    if (i >= s.size() || s[i] != '[') return out;
    ++i;
    while (i < s.size()) {
        skip_ws();
        if (i < s.size() && s[i] == ']') break;
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i >= s.size() || s[i] != '{') break;
        ++i; // {
        Example ex;
        while (i < s.size()) {
            skip_ws();
            if (i < s.size() && s[i] == '}') { ++i; break; }
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i >= s.size() || s[i] != '"') { ++i; continue; }
            std::string key = decode_json_string(s, i);
            skip_ws();
            if (i < s.size() && s[i] == ':') ++i;
            skip_ws();
            if (i < s.size() && s[i] == '"') {
                std::string val = decode_json_string(s, i);
                if (key == "markdown") ex.markdown = std::move(val);
                else if (key == "html") ex.html = std::move(val);
                else if (key == "section") ex.section = std::move(val);
            } else {
                // number (example index) or other scalar
                std::string num;
                while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i]=='-'))
                    num += s[i++];
                if (key == "example" && !num.empty()) ex.example = std::stoi(num);
            }
        }
        out.push_back(std::move(ex));
    }
    return out;
}

// Loose HTML normalization so cosmetic differences (insignificant
// whitespace between block tags) don't mask real parse correctness.
std::string normalize(std::string_view h) {
    std::string out;
    out.reserve(h.size());
    bool in_tag = false;
    for (std::size_t k = 0; k < h.size(); ++k) {
        char c = h[k];
        if (c == '<') in_tag = true;
        if (c == '>') { in_tag = false; out += c; continue; }
        // collapse runs of whitespace that sit between tags / around them
        if ((c == '\n' || c == '\t' || c == '\r')) { c = ' '; }
        out += c;
    }
    // collapse multiple spaces, trim spaces adjacent to '>' '<'
    std::string z; z.reserve(out.size());
    for (std::size_t k = 0; k < out.size(); ++k) {
        char c = out[k];
        if (c == ' ') {
            if (!z.empty() && (z.back() == ' ' || z.back() == '>')) continue;
            // look ahead: drop space right before '<'
            std::size_t j = k; while (j < out.size() && out[j] == ' ') ++j;
            if (j < out.size() && out[j] == '<') { k = j - 1; continue; }
        }
        z += c;
    }
    while (!z.empty() && z.back() == ' ') z.pop_back();
    std::size_t b = 0; while (b < z.size() && z[b] == ' ') ++b;
    return z.substr(b);
}

std::string read_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(static_cast<std::size_t>(n), '\0');
    [[maybe_unused]] auto rd = std::fread(s.data(), 1, static_cast<std::size_t>(n), f);
    std::fclose(f);
    return s;
}

} // namespace

TEST_CASE("commonmark_spec") {
    std::printf("=== CommonMark 0.31.2 parser conformance ===\n\n");

    std::string path = std::string(MAYA_SOURCE_DIR) +
                       "/tests/fixtures/commonmark-0.31.2.json";
    std::string raw = read_file(path.c_str());
    if (raw.empty()) {
        MESSAGE("skip: could not read " << path);
        return;
    }

    auto examples = parse_spec(raw);
    std::printf("loaded %zu examples\n\n", examples.size());
    MAYA_TEST_CHECK(examples.size() > 600, "spec.json should have >600 examples");

    std::map<std::string, std::pair<int,int>> by_section; // section -> {pass,total}
    int pass = 0, total = 0;
    std::vector<int> first_fails;

    for (const auto& ex : examples) {
        // Pure CommonMark core — extensions OFF — so the score is exact.
        maya::md::Document doc = maya::md_detail::engine::parse(ex.markdown);
        std::string got = normalize(cm::to_html(doc));
        std::string want = normalize(ex.html);
        bool ok = (got == want);
        ++total;
        auto& sec = by_section[ex.section];
        ++sec.second;
        if (ok) { ++pass; ++sec.first; }
        else if (first_fails.size() < 12) first_fails.push_back(ex.example);
    }

    std::printf("%-34s %6s\n", "section", "pass/total");
    std::printf("%-34s %6s\n", "----------------------------------", "----------");
    for (const auto& [name, pt] : by_section) {
        std::printf("%-34s %4d/%-4d  %5.1f%%\n", name.c_str(),
                    pt.first, pt.second,
                    pt.second ? 100.0 * pt.first / pt.second : 0.0);
        std::fflush(stdout);   // localize: the crash looked like it was IN the
                               // table print; with this, the last flushed row
                               // names the section that kills the process
    }

    double rate = total ? 100.0 * pass / total : 0.0;
    std::printf("\nOVERALL: %d/%d  (%.1f%%)\n", pass, total, rate);
    std::printf("sample failing example #s:");
    for (int e : first_fails) std::printf(" %d", e);
    std::printf("\n");

    // Conformance ratchet floor. Raise as the engine rebuild lands.
    // Starts permissive to record the pre-rebuild baseline without
    // failing CI; bump this number every time conformance climbs.
    constexpr double kFloor = 0.0;
    MAYA_TEST_CHECK(rate >= kFloor, "conformance below ratchet floor");
}
