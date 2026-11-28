// highlight.cpp — built-in tokenizer for maya::syntax.
//
// A fast, allocation-light lexer that emits canonical highlight Spans. It is
// intentionally *lexical* (not a full parser): it walks the source once,
// classifying strings, comments, numbers, identifiers (keyword/type/constant
// vs. plain), operators and punctuation. That covers the visual 90% of code
// highlighting at a fraction of a real parser's cost — and it produces the
// exact same Span stream a tree-sitter backend would, so the themeable render
// half is shared.
//
// Complexity: O(n) over the source bytes, single pass, no backtracking. The
// only allocation is the caller's output vector (reserved up front).

#include "maya/widget/agentty/markdown/highlight.hpp"

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>

namespace maya::syntax {

// ── language tag → Lang ─────────────────────────────────────────────────────
Lang lang_from_tag(std::string_view tag) noexcept {
    std::string t;
    t.reserve(tag.size());
    for (char c : tag) t += static_cast<char>(std::tolower((unsigned char)c));

    auto is = [&](std::string_view s) { return t == s; };
    if (is("c") || is("h")) return Lang::C;
    if (is("cpp") || is("c++") || is("cc") || is("cxx") || is("hpp") ||
        is("hxx") || is("h++"))
        return Lang::Cpp;
    if (is("py") || is("python") || is("python3")) return Lang::Python;
    if (is("rs") || is("rust")) return Lang::Rust;
    if (is("go") || is("golang")) return Lang::Go;
    if (is("js") || is("javascript") || is("jsx") || is("mjs") || is("cjs"))
        return Lang::JavaScript;
    if (is("ts") || is("typescript") || is("tsx")) return Lang::TypeScript;
    if (is("sh") || is("bash") || is("zsh") || is("shell") || is("console"))
        return Lang::Shell;
    if (is("json") || is("jsonc") || is("json5")) return Lang::Json;
    return Lang::Generic;
}

namespace {

// ── keyword / type / constant tables, per language family ───────────────────
// Built once on first use; lookups are O(1) hash hits.

struct Words {
    std::unordered_set<std::string_view> keywords;
    std::unordered_set<std::string_view> ctrl;       // subset: control-flow
    std::unordered_set<std::string_view> types;
    std::unordered_set<std::string_view> constants;
};

const Words& words_for(Lang lang) {
    static const Words c_family = [] {
        Words w;
        w.keywords = {
            "auto","break","case","const","continue","default","do","else",
            "enum","extern","for","goto","if","inline","register","return",
            "sizeof","static","struct","switch","typedef","union","volatile",
            "while","class","namespace","template","typename","public","private",
            "protected","virtual","override","new","delete","operator","using",
            "friend","explicit","constexpr","consteval","noexcept","nullptr",
            "this","throw","try","catch","mutable","decltype","co_await",
            "co_return","co_yield","concept","requires","static_assert"};
        w.ctrl = {"if","else","for","while","do","switch","case","break",
                  "continue","return","goto","throw","try","catch"};
        w.types = {"int","char","short","long","float","double","void","bool",
                   "unsigned","signed","size_t","uint8_t","uint16_t","uint32_t",
                   "uint64_t","int8_t","int16_t","int32_t","int64_t","wchar_t",
                   "string","vector","map","set","auto"};
        w.constants = {"true","false","NULL","nullptr"};
        return w;
    }();

    static const Words python = [] {
        Words w;
        w.keywords = {"and","as","assert","async","await","class","def","del",
                      "elif","else","except","finally","for","from","global",
                      "if","import","in","is","lambda","nonlocal","not","or",
                      "pass","raise","return","try","while","with","yield","match",
                      "case"};
        w.ctrl = {"if","elif","else","for","while","return","break","continue",
                  "raise","try","except","finally","with","yield"};
        w.types = {"int","float","str","bool","bytes","list","dict","set",
                   "tuple","object","complex","frozenset"};
        w.constants = {"True","False","None","self","cls","__name__"};
        return w;
    }();

    static const Words rust = [] {
        Words w;
        w.keywords = {"as","async","await","break","const","continue","crate",
                      "dyn","else","enum","extern","fn","for","if","impl","in",
                      "let","loop","match","mod","move","mut","pub","ref","return",
                      "self","Self","static","struct","super","trait","type",
                      "unsafe","use","where","while","union"};
        w.ctrl = {"if","else","for","while","loop","match","return","break",
                  "continue"};
        w.types = {"i8","i16","i32","i64","i128","isize","u8","u16","u32","u64",
                   "u128","usize","f32","f64","bool","char","str","String","Vec",
                   "Option","Result","Box","Rc","Arc"};
        w.constants = {"true","false","None","Some","Ok","Err"};
        return w;
    }();

    static const Words go = [] {
        Words w;
        w.keywords = {"break","case","chan","const","continue","default","defer",
                      "else","fallthrough","for","func","go","goto","if","import",
                      "interface","map","package","range","return","select",
                      "struct","switch","type","var"};
        w.ctrl = {"if","else","for","switch","case","return","break","continue",
                  "goto","defer","select","range"};
        w.types = {"bool","string","int","int8","int16","int32","int64","uint",
                   "uint8","uint16","uint32","uint64","byte","rune","float32",
                   "float64","complex64","complex128","error","any"};
        w.constants = {"true","false","nil","iota"};
        return w;
    }();

    static const Words js = [] {
        Words w;
        w.keywords = {"async","await","break","case","catch","class","const",
                      "continue","debugger","default","delete","do","else",
                      "export","extends","finally","for","function","if","import",
                      "in","instanceof","let","new","of","return","super","switch",
                      "this","throw","try","typeof","var","void","while","with",
                      "yield","static","get","set","interface","type","enum",
                      "implements","namespace","declare","readonly","as"};
        w.ctrl = {"if","else","for","while","do","switch","case","break",
                  "continue","return","throw","try","catch","finally","yield"};
        w.types = {"number","string","boolean","object","symbol","bigint","any",
                   "unknown","never","void","undefined","null","Array","Promise",
                   "Map","Set","Record"};
        w.constants = {"true","false","null","undefined","NaN","Infinity","this"};
        return w;
    }();

    static const Words shell = [] {
        Words w;
        w.keywords = {"if","then","else","elif","fi","for","while","until","do",
                      "done","case","esac","function","in","select","return",
                      "break","continue","local","export","readonly","declare",
                      "source","alias","unset","shift","exit"};
        w.ctrl = {"if","then","else","elif","fi","for","while","until","do",
                  "done","case","esac","return","break","continue"};
        w.types = {};
        w.constants = {"true","false"};
        return w;
    }();

    static const Words json = [] {
        Words w;
        w.constants = {"true","false","null"};
        return w;
    }();

    switch (lang) {
        case Lang::Python:                       return python;
        case Lang::Rust:                         return rust;
        case Lang::Go:                           return go;
        case Lang::JavaScript:
        case Lang::TypeScript:                   return js;
        case Lang::Shell:                        return shell;
        case Lang::Json:                         return json;
        case Lang::C:
        case Lang::Cpp:
        case Lang::Generic:
        default:                                 return c_family;
    }
}

[[nodiscard]] bool is_ident_start(unsigned char c) {
    return std::isalpha(c) || c == '_' || c == '$';
}
[[nodiscard]] bool is_ident(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '$';
}

struct Lexer {
    std::string_view s;
    Lang             lang;
    const Words&     w;
    std::vector<Span>& out;
    std::size_t      i = 0;

    void push(std::size_t start, std::size_t end, Capture cap) {
        if (end > start)
            out.push_back({static_cast<std::uint32_t>(start),
                           static_cast<std::uint32_t>(end - start), cap});
    }

    [[nodiscard]] char at(std::size_t k) const {
        return k < s.size() ? s[k] : '\0';
    }

    void run() {
        const bool hash_comment =
            lang == Lang::Python || lang == Lang::Shell;
        const bool preproc =
            lang == Lang::C || lang == Lang::Cpp || lang == Lang::Generic;

        while (i < s.size()) {
            char c = s[i];

            // Line comment: // or #
            if (c == '/' && at(i + 1) == '/') { line_comment(); continue; }
            if (c == '#' && hash_comment)      { line_comment(); continue; }
            // Preprocessor: # at column start in C/C++.
            if (c == '#' && preproc && at_line_start()) { preproc_line(); continue; }
            // Block comment: /* ... */
            if (c == '/' && at(i + 1) == '*') { block_comment(); continue; }

            // Strings.
            if (c == '"' || c == '\'' || c == '`') { string_lit(c); continue; }

            // Numbers.
            if (std::isdigit((unsigned char)c) ||
                (c == '.' && std::isdigit((unsigned char)at(i + 1)))) {
                number(); continue;
            }

            // Shell / preproc variables: $VAR, ${VAR}.
            if (c == '$' && (lang == Lang::Shell)) { shell_var(); continue; }

            // Attributes / decorators: @name (py/js/java), #[...] (rust).
            if (c == '@' && is_ident_start((unsigned char)at(i + 1))) {
                attribute(); continue;
            }

            // Identifiers / keywords.
            if (is_ident_start((unsigned char)c)) { identifier(); continue; }

            // Operators.
            if (is_operator(c)) { operator_run(); continue; }

            // Punctuation.
            if (is_punct(c)) { push(i, i + 1, Capture::Punctuation); i++; continue; }

            // Anything else (whitespace etc.) — leave as a gap (None).
            i++;
        }
    }

    [[nodiscard]] bool at_line_start() const {
        // True if everything before i on this line is whitespace.
        std::size_t k = i;
        while (k > 0 && s[k - 1] != '\n') {
            if (!std::isspace((unsigned char)s[k - 1])) return false;
            k--;
        }
        return true;
    }

    void line_comment() {
        std::size_t start = i;
        while (i < s.size() && s[i] != '\n') i++;
        push(start, i, Capture::Comment);
    }

    void preproc_line() {
        std::size_t start = i;
        // Continuation lines (ending in backslash) extend the directive.
        while (i < s.size()) {
            if (s[i] == '\n') {
                // Was the previous non-space a backslash?
                std::size_t j = i;
                while (j > start && std::isspace((unsigned char)s[j - 1])) j--;
                if (j > start && s[j - 1] == '\\') { i++; continue; }
                break;
            }
            i++;
        }
        push(start, i, Capture::Preproc);
    }

    void block_comment() {
        std::size_t start = i;
        i += 2;
        while (i < s.size() && !(s[i] == '*' && at(i + 1) == '/')) i++;
        if (i < s.size()) i += 2;   // consume closing */
        push(start, i, Capture::Comment);
    }

    void string_lit(char q) {
        std::size_t start = i;
        i++;   // opening quote
        while (i < s.size()) {
            char c = s[i];
            if (c == '\\' && i + 1 < s.size()) { i += 2; continue; }
            if (c == q) { i++; break; }
            // Single-quote char literals don't span newlines; bail to avoid
            // swallowing the rest of a file on an unterminated quote.
            if (c == '\n' && q == '\'') break;
            i++;
        }
        push(start, i, Capture::String);
    }

    void number() {
        std::size_t start = i;
        if (s[i] == '0' && (at(i + 1) == 'x' || at(i + 1) == 'X')) {
            i += 2;
            while (i < s.size() && (std::isxdigit((unsigned char)s[i]) ||
                                    s[i] == '_')) i++;
        } else if (s[i] == '0' && (at(i + 1) == 'b' || at(i + 1) == 'B')) {
            i += 2;
            while (i < s.size() && (s[i] == '0' || s[i] == '1' || s[i] == '_')) i++;
        } else {
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) ||
                                    s[i] == '.' || s[i] == '_' ||
                                    s[i] == 'e' || s[i] == 'E' ||
                                    ((s[i] == '+' || s[i] == '-') &&
                                     (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                i++;
        }
        // Numeric type suffixes: f, L, u, etc.
        while (i < s.size() && std::isalpha((unsigned char)s[i])) i++;
        push(start, i, Capture::Number);
    }

    void shell_var() {
        std::size_t start = i;
        i++;   // $
        if (at(i) == '{') {
            while (i < s.size() && s[i] != '}') i++;
            if (i < s.size()) i++;
        } else {
            while (i < s.size() && is_ident((unsigned char)s[i])) i++;
        }
        push(start, i, Capture::Variable);
    }

    void attribute() {
        std::size_t start = i;
        i++;   // @
        while (i < s.size() && is_ident((unsigned char)s[i])) i++;
        push(start, i, Capture::Attribute);
    }

    void identifier() {
        std::size_t start = i;
        while (i < s.size() && is_ident((unsigned char)s[i])) i++;
        std::string_view word = s.substr(start, i - start);

        Capture cap = Capture::Variable;
        if (w.ctrl.contains(word))            cap = Capture::KeywordCtrl;
        else if (w.keywords.contains(word))   cap = Capture::Keyword;
        else if (w.types.contains(word))      cap = Capture::Type;
        else if (w.constants.contains(word))  cap = Capture::Constant;
        else {
            // Function-call heuristic: identifier immediately followed by '('.
            std::size_t k = i;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k < s.size() && s[k] == '(') cap = Capture::Function;
            // Type heuristic: PascalCase identifier.
            else if (!word.empty() && std::isupper((unsigned char)word[0]) &&
                     word.size() > 1)
                cap = Capture::Type;
        }
        // Identifiers that resolve to plain variables stay as None so the
        // body text uses the theme default (avoids a wall of one colour).
        if (cap == Capture::Variable) return;
        push(start, i, cap);
    }

    void operator_run() {
        std::size_t start = i;
        while (i < s.size() && is_operator(s[i])) i++;
        push(start, i, Capture::Operator);
    }

    static bool is_operator(char c) {
        switch (c) {
            case '+': case '-': case '*': case '/': case '%':
            case '=': case '<': case '>': case '!': case '&':
            case '|': case '^': case '~': case '?':
                return true;
            default: return false;
        }
    }
    static bool is_punct(char c) {
        switch (c) {
            case '(': case ')': case '{': case '}': case '[': case ']':
            case ';': case ',': case '.': case ':':
                return true;
            default: return false;
        }
    }
};

} // namespace

void builtin_highlight(std::string_view src, Lang lang, std::vector<Span>& out) {
    out.clear();
    out.reserve(src.size() / 4 + 8);
    Lexer lex{src, lang, words_for(lang), out};
    lex.run();
}

std::vector<Span> highlight(std::string_view src, Lang lang) {
    std::vector<Span> out;
    highlight(src, lang, out);
    return out;
}

} // namespace maya::syntax
