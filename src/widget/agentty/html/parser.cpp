// parser.cpp — tolerant DOM tree builder. Turns the token stream into a
// Node tree, applying the implied-close rules that real HTML relies on
// (an open <li> is closed by the next <li>, a block element closes an open
// <p>, <tr>/<td> auto-nest, …). Void and self-closing elements never push a
// scope. Stray end tags are ignored. Anything unbalanced is closed at EOF.

#include "maya/widget/agentty/html/internal.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace maya::html::detail {
namespace {

class TreeBuilder {
public:
    Node build(const std::vector<Token>& tokens) {
        stack_.push_back(Node{.kind = Node::Kind::Document});
        for (const auto& tok : tokens) {
            switch (tok.kind) {
                case Token::Kind::Text:    on_text(tok);  break;
                case Token::Kind::StartTag: on_start(tok); break;
                case Token::Kind::EndTag:  on_end(tok.name); break;
            }
        }
        while (stack_.size() > 1) pop_into_parent();
        return std::move(stack_.front());
    }

private:
    std::vector<Node> stack_;  // stack_[0] == Document; back() == open element

    [[nodiscard]] std::string_view top_tag() const {
        return stack_.size() > 1 ? std::string_view(stack_.back().tag)
                                 : std::string_view{};
    }

    void pop_into_parent() {
        Node done = std::move(stack_.back());
        stack_.pop_back();
        stack_.back().children.push_back(std::move(done));
    }

    void on_text(const Token& tok) {
        stack_.back().children.push_back(
            Node{.kind = Node::Kind::Text, .text = tok.text});
    }

    void on_start(const Token& tok) {
        close_implied(tok.name);
        Node el{.kind = Node::Kind::Element, .tag = tok.name, .attrs = tok.attrs};
        if (tok.self_closing || is_void_element(tok.name)) {
            stack_.back().children.push_back(std::move(el));
        } else {
            stack_.push_back(std::move(el));
        }
    }

    void on_end(std::string_view name) {
        int idx = -1;
        for (int k = static_cast<int>(stack_.size()) - 1; k >= 1; --k) {
            if (stack_[static_cast<std::size_t>(k)].tag == name) { idx = k; break; }
        }
        if (idx < 0) return;  // stray end tag
        while (static_cast<int>(stack_.size()) - 1 >= idx) pop_into_parent();
    }

    // Auto-close open elements that the spec implicitly ends when `name` opens.
    void close_implied(std::string_view name) {
        for (;;) {
            if (stack_.size() <= 1) return;
            std::string_view top = top_tag();
            bool close = false;
            if (name == "li" && top == "li") close = true;
            else if ((name == "td" || name == "th") &&
                     (top == "td" || top == "th")) close = true;
            else if (name == "tr" &&
                     (top == "td" || top == "th" || top == "tr")) close = true;
            else if ((name == "dt" || name == "dd") &&
                     (top == "dt" || top == "dd")) close = true;
            else if (name == "option" && top == "option") close = true;
            else if ((name == "thead" || name == "tbody" || name == "tfoot") &&
                     (top == "td" || top == "th" || top == "tr" ||
                      top == "thead" || top == "tbody" || top == "tfoot"))
                close = true;
            else if (is_block_element(name) && top == "p") close = true;
            if (!close) return;
            pop_into_parent();
        }
    }
};

} // namespace

Node parse(std::string_view src) {
    return TreeBuilder().build(tokenize(src));
}

} // namespace maya::html::detail
