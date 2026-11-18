// Stands in for web-tree-sitter (bundled in its place by js/build.mjs) for Strudel's hs2js: the parse
// runs in C++ (src/pattern/HaskellParser, tree-sitter + tree-sitter-haskell) through
// __concha_hsparse(code), which returns [type, startIndex, endIndex, [children...]] per node.
// hs2js uses rootNode, and type, text, startIndex, children of the nodes.

class Node {
  constructor(code, raw) {
    this._code = code;
    this._raw = raw;
  }
  get type() {
    return this._raw[0];
  }
  get startIndex() {
    return this._raw[1];
  }
  get endIndex() {
    return this._raw[2];
  }
  get text() {
    return this._code.slice(this._raw[1], this._raw[2]);
  }
  get children() {
    return (this._children ??= this._raw[3].map((c) => new Node(this._code, c)));
  }
  get childCount() {
    return this._raw[3].length;
  }
}

class Language {
  static async load() {
    return new Language();
  }
}

export default class Parser {
  static async init() {}
  static Language = Language;
  setLanguage() {}
  parse(code) {
    if (typeof globalThis.__concha_hsparse !== 'function') throw new Error('Tidal: no Haskell parser in this runtime');
    const raw = JSON.parse(globalThis.__concha_hsparse(code));
    return { rootNode: new Node(code, raw) };
  }
}
