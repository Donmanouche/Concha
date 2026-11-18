#include "pattern/HaskellParser.h"

#include <tree_sitter/api.h>

#include <vector>

extern "C" const TSLanguage* tree_sitter_haskell(void);

namespace concha {

namespace {
void appendEscaped(std::string& out, const char* s)
{
    out += '"';
    for (; *s; ++s) {
        const char c = *s;
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (uint8_t(c) < 0x20) {
            static const char* hex = "0123456789abcdef";
            out += "\\u00";
            out += hex[(c >> 4) & 0xf];
            out += hex[c & 0xf];
        } else
            out += c;
    }
    out += '"';
}

void appendNode(std::string& out, TSNode node)
{
    out += '[';
    appendEscaped(out, ts_node_type(node));
    // UTF-16 input: byte offsets / 2 are JS string indices
    out += ',' + std::to_string(ts_node_start_byte(node) / 2) + ',' + std::to_string(ts_node_end_byte(node) / 2) + ",[";
    const uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ',';
        appendNode(out, ts_node_child(node, i));
    }
    out += "]]";
}

std::u16string toUtf16(std::string_view s)
{
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xf0) cp = c & 0x07, extra = 3;
        else if (c >= 0xe0) cp = c & 0x0f, extra = 2;
        else if (c >= 0xc0) cp = c & 0x1f, extra = 1;
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (uint8_t(s[i]) & 0x3f);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xd800 + (cp >> 10));
            out += char16_t(0xdc00 + (cp & 0x3ff));
        } else
            out += char16_t(cp);
    }
    return out;
}
} // namespace

std::string parseHaskellJson(std::string_view utf8Code)
{
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_haskell());
    const std::u16string code = toUtf16(utf8Code);
    TSTree* tree = ts_parser_parse_string_encoding(parser, nullptr, reinterpret_cast<const char*>(code.data()),
                                                   uint32_t(code.size() * 2), TSInputEncodingUTF16);
    std::string out;
    if (tree) {
        appendNode(out, ts_tree_root_node(tree));
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
    return out;
}

} // namespace concha
