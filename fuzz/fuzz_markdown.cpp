// fuzz_markdown.cpp — libFuzzer harness for the markdown parser/renderer.
//
// markdown(source) parses arbitrary text through the CommonMark engine and
// builds an Element tree. The parser handles deeply nested lists, code
// fences, tables, links, entities, and raw HTML — a large, recursive surface
// that has historically been a hang/overflow magnet in TUI markdown engines.
// Bound the input so pathological nesting can't legitimately OOM the fuzzer.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <maya/widget/agentty/markdown.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap at 64 KiB: beyond that we're testing the allocator, not the parser,
    // and deep-nesting inputs grow super-linearly.
    if (size > 64u * 1024u) size = 64u * 1024u;

    std::string_view src{reinterpret_cast<const char*>(data), size};

    // Parse + build the element tree. We don't render to a canvas here
    // (that needs layout sizing); parsing + tree construction is the part
    // that touches the untrusted bytes.
    maya::Element el = maya::markdown(src);
    (void)el;

    return 0;
}
