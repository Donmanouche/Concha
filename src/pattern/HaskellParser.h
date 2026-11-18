#pragma once

// Tidal in Concha: Strudel's hs2js parses Haskell with web-tree-sitter (WebAssembly), which QuickJS
// cannot run. tree-sitter's C runtime and tree-sitter-haskell (third_party) do the same parse here;
// js/src/tree-sitter-native.mjs stands in for web-tree-sitter on top of this.

#include <string>
#include <string_view>

namespace concha {

// The syntax tree of `code` as JSON: [type, startIndex, endIndex, [children...]] for every node
// (named or not, as web-tree-sitter's `children`), indices in UTF-16 code units like JS strings.
std::string parseHaskellJson(std::string_view utf8Code);

} // namespace concha
