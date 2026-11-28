// syntax_lang.cpp — language identification + per-language token tables.
//
// detect_lang (tag → LangId), classify_word (keyword/type/constant tables
// per language), comment_style_for, features_for. Pure data; consumed by the
// tokeniser in syntax_highlight.cpp through syntax_internal.hpp.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>

#include "maya/widget/agentty/markdown/syntax_internal.hpp"

namespace maya {
namespace syntax_detail {

LangId detect_lang(std::string_view tag) {
    // Normalize: lowercase — use stack buffer to avoid heap allocation
    // (language tags are always short, typically < 16 chars).
    char buf[32];
    size_t len = std::min(tag.size(), sizeof(buf) - 1);
    for (size_t k = 0; k < len; ++k)
        buf[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[k])));
    buf[len] = '\0';
    std::string_view lower{buf, len};

    if (lower == "c" || lower == "h")                return LangId::C;
    if (lower == "cpp" || lower == "c++" ||
        lower == "cxx" || lower == "cc" ||
        lower == "hpp" || lower == "hxx")            return LangId::Cpp;
    if (lower == "python" || lower == "py")          return LangId::Python;
    if (lower == "rust" || lower == "rs")            return LangId::Rust;
    if (lower == "javascript" || lower == "js" ||
        lower == "jsx" || lower == "mjs" ||
        lower == "cjs")                              return LangId::JavaScript;
    if (lower == "typescript" || lower == "ts" ||
        lower == "tsx")                              return LangId::TypeScript;
    if (lower == "go" || lower == "golang")          return LangId::Go;
    if (lower == "java")                             return LangId::Java;
    if (lower == "kotlin" || lower == "kt" ||
        lower == "kts")                              return LangId::Kotlin;
    if (lower == "swift")                            return LangId::Swift;
    if (lower == "ruby" || lower == "rb")            return LangId::Ruby;
    if (lower == "bash" || lower == "sh" ||
        lower == "shell" || lower == "zsh")          return LangId::Shell;
    if (lower == "fish")                             return LangId::Fish;
    if (lower == "sql" || lower == "mysql" ||
        lower == "postgresql" || lower == "sqlite")  return LangId::SQL;
    if (lower == "html" || lower == "htm")           return LangId::HTML;
    if (lower == "xml" || lower == "svg")            return LangId::XML;
    if (lower == "css")                              return LangId::CSS;
    if (lower == "scss" || lower == "sass" ||
        lower == "less")                             return LangId::SCSS;
    if (lower == "json" || lower == "jsonc")         return LangId::JSON;
    if (lower == "yaml" || lower == "yml")           return LangId::YAML;
    if (lower == "toml")                             return LangId::TOML;
    if (lower == "lua")                              return LangId::Lua;
    if (lower == "zig")                              return LangId::Zig;
    if (lower == "haskell" || lower == "hs")         return LangId::Haskell;
    if (lower == "elixir" || lower == "ex" ||
        lower == "exs")                              return LangId::Elixir;
    if (lower == "erlang" || lower == "erl")         return LangId::Erlang;
    if (lower == "php")                              return LangId::PHP;
    if (lower == "perl" || lower == "pl")            return LangId::Perl;
    if (lower == "r")                                return LangId::R;
    if (lower == "diff" || lower == "patch")         return LangId::Diff;
    if (lower == "makefile" || lower == "make")      return LangId::Makefile;
    if (lower == "cmake")                            return LangId::CMake;
    if (lower == "dockerfile" || lower == "docker")  return LangId::Dockerfile;
    if (lower == "markdown" || lower == "md")        return LangId::Markdown;
    return LangId::Unknown;
}

// ── Per-language keyword tables ──────────────────────────────────────────────

WordClass classify_word(std::string_view word, LangId lang) {
    // Constants — universal
    if (in_list(word, {"true", "false", "null", "nullptr", "None", "nil",
                       "True", "False", "NULL", "NaN", "Infinity",
                       "undefined", "NUL", "YES", "NO"}))
        return {false, false, true};

    switch (lang) {
    case LangId::C:
    case LangId::Cpp:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "goto", "default",
            "struct", "enum", "union", "typedef", "class", "namespace",
            "template", "typename", "using", "static", "extern", "inline",
            "const", "constexpr", "consteval", "constinit", "volatile",
            "mutable", "register", "thread_local",
            "virtual", "override", "final", "explicit", "noexcept",
            "public", "private", "protected", "friend",
            "new", "delete", "operator", "sizeof", "alignof", "decltype",
            "static_assert", "static_cast", "dynamic_cast", "reinterpret_cast",
            "const_cast", "typeid", "throw", "try", "catch",
            "concept", "requires", "co_await", "co_yield", "co_return",
            "export", "import", "module",
            "auto", "void",
            "#include", "#define", "#ifdef", "#ifndef", "#endif", "#pragma",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "char", "float", "double", "long", "short", "unsigned",
            "signed", "bool", "size_t", "uint8_t", "uint16_t", "uint32_t",
            "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
            "string", "string_view", "vector", "map", "set", "array",
            "optional", "variant", "pair", "tuple", "span", "expected",
            "unique_ptr", "shared_ptr", "weak_ptr",
            "wchar_t", "char8_t", "char16_t", "char32_t", "ptrdiff_t",
        })) return {false, true, false};
        break;

    case LangId::Python:
        if (in_list(word, {
            "if", "elif", "else", "for", "while", "break", "continue",
            "return", "yield", "pass", "raise", "try", "except", "finally",
            "with", "as", "assert", "del",
            "def", "class", "lambda", "async", "await",
            "import", "from", "global", "nonlocal",
            "and", "or", "not", "in", "is",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "float", "str", "bool", "list", "dict", "tuple", "set",
            "bytes", "bytearray", "complex", "frozenset", "type", "object",
            "range", "enumerate", "zip", "map", "filter",
            "Exception", "ValueError", "TypeError", "KeyError", "IndexError",
            "RuntimeError", "StopIteration", "AttributeError", "ImportError",
            "OSError", "IOError", "FileNotFoundError",
        })) return {false, true, false};
        if (in_list(word, {"self", "cls", "super"}))
            return {true, false, false};
        break;

    case LangId::Rust:
        if (in_list(word, {
            "if", "else", "for", "while", "loop", "break", "continue",
            "return", "match", "as",
            "fn", "struct", "enum", "impl", "trait", "type", "where",
            "let", "mut", "const", "static", "ref", "move",
            "pub", "mod", "use", "crate", "super", "self", "Self",
            "async", "await", "unsafe", "extern", "dyn",
        })) return {true, false, false};
        if (in_list(word, {
            "i8", "i16", "i32", "i64", "i128", "isize",
            "u8", "u16", "u32", "u64", "u128", "usize",
            "f32", "f64", "bool", "char", "str",
            "String", "Vec", "Box", "Rc", "Arc", "Cell", "RefCell",
            "Option", "Result", "Ok", "Err", "Some",
            "HashMap", "HashSet", "BTreeMap", "BTreeSet",
            "Iterator", "IntoIterator", "From", "Into",
            "Display", "Debug", "Clone", "Copy", "Send", "Sync",
            "Default", "PartialEq", "Eq", "PartialOrd", "Ord", "Hash",
        })) return {false, true, false};
        break;

    case LangId::JavaScript:
    case LangId::TypeScript:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "throw", "try", "catch", "finally",
            "default", "in", "of", "typeof", "instanceof", "void", "delete",
            "function", "class", "extends", "new", "this", "super",
            "const", "let", "var", "async", "await", "yield",
            "import", "export", "from", "as", "default",
            "with", "debugger",
        })) return {true, false, false};
        if (lang == LangId::TypeScript && in_list(word, {
            "type", "interface", "enum", "namespace", "declare", "abstract",
            "implements", "readonly", "keyof", "infer", "is", "asserts",
            "override", "satisfies",
        })) return {true, false, false};
        if (in_list(word, {
            "string", "number", "boolean", "object", "symbol", "bigint",
            "any", "unknown", "never", "void",
            "Array", "Map", "Set", "Promise", "Date", "RegExp", "Error",
            "Object", "Function", "Symbol", "WeakMap", "WeakSet",
            "Record", "Partial", "Required", "Readonly", "Pick", "Omit",
        })) return {false, true, false};
        break;

    case LangId::Go:
        if (in_list(word, {
            "if", "else", "for", "switch", "case", "break", "continue",
            "return", "goto", "default", "fallthrough", "select",
            "func", "type", "struct", "interface", "map", "chan",
            "var", "const", "package", "import",
            "go", "defer", "range",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64", "uintptr",
            "float32", "float64", "complex64", "complex128",
            "bool", "byte", "rune", "string", "error",
            "any", "comparable",
        })) return {false, true, false};
        if (in_list(word, {"make", "append", "len", "cap", "copy", "close",
                           "new", "delete", "panic", "recover", "print", "println",
                           "iota"}))
            return {false, false, true};
        break;

    case LangId::Java:
    case LangId::Kotlin:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "throw", "try", "catch", "finally",
            "default", "instanceof", "new", "this", "super",
            "class", "interface", "enum", "extends", "implements",
            "abstract", "final", "static", "synchronized", "volatile",
            "transient", "native", "strictfp",
            "public", "private", "protected", "package", "import",
            "assert", "throws", "void",
        })) return {true, false, false};
        if (lang == LangId::Kotlin && in_list(word, {
            "fun", "val", "var", "when", "is", "as", "in", "out",
            "object", "companion", "data", "sealed", "inline", "reified",
            "suspend", "override", "open", "internal", "lateinit",
            "by", "constructor", "init", "typealias",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "long", "short", "byte", "float", "double", "char",
            "boolean", "String", "Integer", "Long", "Double", "Float",
            "Boolean", "Character", "Object", "Void",
            "List", "Map", "Set", "Array", "Collection", "Iterator",
            "Optional", "Stream", "Comparable", "Iterable",
        })) return {false, true, false};
        break;

    case LangId::Ruby:
        if (in_list(word, {
            "if", "elsif", "else", "unless", "while", "until", "for",
            "do", "end", "begin", "rescue", "ensure", "raise", "retry",
            "return", "break", "next", "redo", "yield",
            "def", "class", "module", "include", "extend", "require",
            "require_relative", "attr_reader", "attr_writer", "attr_accessor",
            "self", "super", "then", "when", "case", "in", "and", "or", "not",
            "defined?", "alias", "undef", "private", "protected", "public",
            "lambda", "proc", "block_given?",
        })) return {true, false, false};
        break;

    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:
        if (in_list(word, {
            "if", "then", "else", "elif", "fi", "for", "while", "until",
            "do", "done", "case", "esac", "in", "function", "return",
            "local", "export", "unset", "readonly", "declare", "typeset",
            "source", "eval", "exec", "set", "shift", "trap",
            "echo", "printf", "read", "test", "exit",
            // Dockerfile
            "FROM", "RUN", "CMD", "ENTRYPOINT", "COPY", "ADD", "WORKDIR",
            "ENV", "ARG", "EXPOSE", "VOLUME", "USER", "LABEL", "ONBUILD",
            "HEALTHCHECK", "SHELL", "STOPSIGNAL",
        })) return {true, false, false};
        break;

    case LangId::SQL:
        if (in_list(word, {
            "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE",
            "SET", "DELETE", "CREATE", "ALTER", "DROP", "TABLE", "INDEX",
            "VIEW", "DATABASE", "SCHEMA", "JOIN", "LEFT", "RIGHT", "INNER",
            "OUTER", "FULL", "CROSS", "ON", "AS", "AND", "OR", "NOT", "IN",
            "IS", "LIKE", "BETWEEN", "EXISTS", "HAVING", "GROUP", "BY",
            "ORDER", "ASC", "DESC", "LIMIT", "OFFSET", "UNION", "ALL",
            "DISTINCT", "CASE", "WHEN", "THEN", "ELSE", "END", "IF",
            "BEGIN", "COMMIT", "ROLLBACK", "TRANSACTION", "GRANT", "REVOKE",
            "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "CONSTRAINT",
            "UNIQUE", "CHECK", "DEFAULT", "CASCADE", "RESTRICT",
            // Also match lowercase
            "select", "from", "where", "insert", "into", "values", "update",
            "set", "delete", "create", "alter", "drop", "table", "index",
            "view", "database", "schema", "join", "left", "right", "inner",
            "outer", "full", "cross", "on", "as", "and", "or", "not", "in",
            "is", "like", "between", "exists", "having", "group", "by",
            "order", "asc", "desc", "limit", "offset", "union", "all",
            "distinct", "case", "when", "then", "else", "end", "if",
            "begin", "commit", "rollback", "transaction", "grant", "revoke",
            "primary", "key", "foreign", "references", "constraint",
            "unique", "check", "default", "cascade", "restrict",
        })) return {true, false, false};
        if (in_list(word, {
            "INT", "INTEGER", "BIGINT", "SMALLINT", "TINYINT",
            "VARCHAR", "CHAR", "TEXT", "BLOB", "BOOLEAN", "BOOL",
            "FLOAT", "DOUBLE", "DECIMAL", "NUMERIC", "REAL",
            "DATE", "TIME", "TIMESTAMP", "DATETIME",
            "SERIAL", "BIGSERIAL", "UUID",
            "int", "integer", "bigint", "smallint", "tinyint",
            "varchar", "char", "text", "blob", "boolean", "bool",
            "float", "double", "decimal", "numeric", "real",
            "date", "time", "timestamp", "datetime",
            "serial", "bigserial", "uuid",
        })) return {false, true, false};
        break;

    case LangId::Lua:
        if (in_list(word, {
            "if", "then", "else", "elseif", "end", "for", "while", "do",
            "repeat", "until", "break", "return", "goto",
            "function", "local", "in", "and", "or", "not",
        })) return {true, false, false};
        break;

    case LangId::Zig:
        if (in_list(word, {
            "if", "else", "for", "while", "break", "continue", "return",
            "switch", "orelse", "catch", "unreachable",
            "fn", "pub", "const", "var", "struct", "enum", "union",
            "error", "test", "comptime", "inline", "extern", "export",
            "threadlocal", "defer", "errdefer", "nosuspend",
            "try", "async", "await", "suspend", "resume",
            "align", "allowzero", "volatile", "linksection",
        })) return {true, false, false};
        if (in_list(word, {
            "u8", "u16", "u32", "u64", "u128", "usize",
            "i8", "i16", "i32", "i64", "i128", "isize",
            "f16", "f32", "f64", "f128", "bool", "void", "noreturn",
            "anyerror", "anyframe", "anytype", "anyopaque", "type",
            "comptime_int", "comptime_float",
        })) return {false, true, false};
        break;

    case LangId::Swift:
        if (in_list(word, {
            "if", "else", "for", "while", "repeat", "switch", "case",
            "break", "continue", "return", "throw", "guard", "defer",
            "do", "try", "catch", "where", "in", "as", "is",
            "func", "class", "struct", "enum", "protocol", "extension",
            "typealias", "associatedtype",
            "let", "var", "static", "lazy", "override", "mutating",
            "public", "private", "internal", "fileprivate", "open",
            "import", "init", "deinit", "subscript", "operator",
            "async", "await", "actor",
            "self", "Self", "super",
        })) return {true, false, false};
        if (in_list(word, {
            "Int", "Int8", "Int16", "Int32", "Int64",
            "UInt", "UInt8", "UInt16", "UInt32", "UInt64",
            "Float", "Double", "Bool", "String", "Character",
            "Array", "Dictionary", "Set", "Optional",
            "Any", "AnyObject", "Void", "Never",
        })) return {false, true, false};
        break;

    case LangId::Haskell:
        if (in_list(word, {
            "if", "then", "else", "case", "of", "let", "in", "where",
            "do", "module", "import", "data", "type", "newtype", "class",
            "instance", "deriving", "default", "forall", "infixl", "infixr",
            "infix", "qualified", "as", "hiding",
        })) return {true, false, false};
        if (in_list(word, {
            "Int", "Integer", "Float", "Double", "Char", "String", "Bool",
            "IO", "Maybe", "Either", "Monad", "Functor", "Applicative",
            "Show", "Read", "Eq", "Ord", "Num", "Enum", "Bounded",
        })) return {false, true, false};
        break;

    case LangId::Elixir:
    case LangId::Erlang:
        if (in_list(word, {
            "if", "else", "do", "end", "case", "cond", "when", "with",
            "for", "unless", "fn", "def", "defp", "defmodule", "defstruct",
            "defimpl", "defprotocol", "defmacro", "defguard",
            "import", "require", "use", "alias", "raise", "rescue",
            "try", "catch", "after", "receive", "send", "spawn",
            "and", "or", "not", "in",
        })) return {true, false, false};
        break;

    case LangId::PHP:
        if (in_list(word, {
            "if", "else", "elseif", "for", "foreach", "while", "do",
            "switch", "case", "break", "continue", "return", "throw",
            "try", "catch", "finally", "default",
            "function", "class", "interface", "trait", "extends", "implements",
            "abstract", "final", "static", "public", "private", "protected",
            "new", "instanceof", "as", "use", "namespace", "echo", "print",
            "include", "require", "include_once", "require_once",
            "yield", "fn", "match", "enum", "readonly",
        })) return {true, false, false};
        break;

    case LangId::CSS:
    case LangId::SCSS:
    case LangId::HTML:
    case LangId::XML:
    case LangId::JSON:
    case LangId::YAML:
    case LangId::TOML:
    case LangId::Perl:
    case LangId::R:
    case LangId::CMake:
    case LangId::Markdown:
    // Diff is line-oriented (handled by the line highlighter, not per word),
    // so a word is always plain. Listed explicitly so -Wswitch flags any
    // future LangId that forgets to decide.
    case LangId::Diff:
    case LangId::Unknown:
        break;
    }

    return {false, false, false};
}

// ── Comment style per language ───────────────────────────────────────────────


CommentStyle comment_style_for(LangId lang) {
    switch (lang) {
    case LangId::C:
    case LangId::Cpp:          return {"//", "/*", "*/", false};
    case LangId::Python:       return {nullptr, nullptr, nullptr, true};
    case LangId::Rust:         return {"//", "/*", "*/", false};
    case LangId::JavaScript:
    case LangId::TypeScript:   return {"//", "/*", "*/", false};
    case LangId::Go:           return {"//", "/*", "*/", false};
    case LangId::Java:
    case LangId::Kotlin:       return {"//", "/*", "*/", false};
    case LangId::Swift:        return {"//", "/*", "*/", false};
    case LangId::Zig:          return {"//", nullptr, nullptr, false};
    case LangId::Lua:          return {"--", "--[[", "]]", false};
    case LangId::Haskell:      return {"--", "{-", "-}", false};
    case LangId::SQL:          return {"--", "/*", "*/", false};
    case LangId::Ruby:         return {nullptr, "=begin", "=end", true};
    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:
    case LangId::YAML:
    case LangId::TOML:
    case LangId::R:
    case LangId::CMake:
    case LangId::Elixir:
    case LangId::Erlang:       return {nullptr, nullptr, nullptr, true};
    case LangId::PHP:          return {"//", "/*", "*/", true};
    case LangId::Perl:         return {nullptr, nullptr, nullptr, true};
    case LangId::CSS:
    case LangId::SCSS:         return {"//", "/*", "*/", false};
    case LangId::HTML:
    case LangId::XML:          return {nullptr, "<!--", "-->", false};
    case LangId::JSON:         return {nullptr, nullptr, nullptr, false};
    case LangId::Diff:
    case LangId::Markdown:
    case LangId::Unknown:      return {"//", "/*", "*/", true};
    }
    return {"//", "/*", "*/", true};
}

// ── Language feature flags ───────────────────────────────────────────────────

LangFeatures features_for(LangId lang) {
    switch (lang) {
    case LangId::C:
    case LangId::Cpp:          return {false, false, true,  false, false, true,  false, false};
    case LangId::Python:       return {true,  false, false, true,  false, false, false, false};
    case LangId::Rust:         return {false, false, false, false, false, false, true,  false};
    case LangId::JavaScript:   return {false, true,  false, true,  false, false, false, false};
    case LangId::TypeScript:   return {false, true,  false, true,  false, false, false, false};
    case LangId::Go:           return {false, true,  false, false, false, true,  false, false};
    case LangId::Java:         return {false, false, false, true,  false, true,  false, false};
    case LangId::Kotlin:       return {false, false, false, true,  false, true,  false, false};
    case LangId::Swift:        return {false, false, false, true,  false, false, false, false};
    case LangId::Ruby:         return {false, false, false, false, false, false, false, true};
    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:   return {false, false, false, false, true,  false, false, false};
    case LangId::PHP:          return {false, false, false, false, true,  true,  false, false};
    case LangId::Perl:         return {false, false, false, false, true,  false, false, false};
    case LangId::Elixir:       return {false, false, false, true,  false, false, false, true};
    case LangId::Erlang:       return {false, false, false, false, false, true,  false, false};
    default:                   return {false, false, false, false, false, false, false, false};
    }
}


} // namespace syntax_detail
} // namespace maya
