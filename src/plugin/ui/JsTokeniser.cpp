#include "plugin/ui/JsTokeniser.h"

#include "plugin/ui/Theme.h"

namespace concha::ui {

namespace {
bool isKeyword(const juce::String& w)
{
    static const juce::StringArray words{"await", "break", "case", "catch", "class", "const", "continue", "debugger",
                                         "default", "delete", "do", "else", "export", "extends", "finally", "for",
                                         "function", "if", "import", "in", "instanceof", "let", "new", "of", "return",
                                         "static", "super", "switch", "this", "throw", "try", "typeof", "var", "void",
                                         "while", "with", "yield", "async"};
    return words.contains(w);
}

// the word before `it` (skipping whitespace), e.g. "const" in `const x`
juce::String previousWord(juce::CodeDocument::Iterator it)
{
    while (!it.isSOF() && juce::CharacterFunctions::isWhitespace(it.peekPreviousChar())) it.previousChar();
    juce::String w;
    while (!it.isSOF() && JsTokeniser::isIdentifierChar(it.peekPreviousChar())) w = juce::String::charToString(it.previousChar()) + w;
    return w;
}

// only whitespace between the line start and `it`
bool atLineStart(juce::CodeDocument::Iterator it)
{
    while (!it.isSOF()) {
        const juce::juce_wchar c = it.peekPreviousChar();
        if (c == '\n') return true;
        if (!juce::CharacterFunctions::isWhitespace(c)) return false;
        it.previousChar();
    }
    return true;
}
} // namespace

bool JsTokeniser::isIdentifierStart(juce::juce_wchar c)
{
    return juce::CharacterFunctions::isLetter(c) || c == '_' || c == '$';
}

bool JsTokeniser::isIdentifierChar(juce::juce_wchar c)
{
    return isIdentifierStart(c) || juce::CharacterFunctions::isDigit(c);
}

int JsTokeniser::readNextToken(juce::CodeDocument::Iterator& source)
{
    source.skipWhitespace();
    const juce::juce_wchar c = source.peekNextChar();
    if (c == 0) return Error;

    if (c == '/') { // comments
        juce::CodeDocument::Iterator next = source;
        next.skip();
        const juce::juce_wchar c2 = next.peekNextChar();
        if (c2 == '/') {
            source.skipToEndOfLine();
            return Comment;
        }
        if (c2 == '*') {
            source.skip();
            source.skip();
            juce::juce_wchar prev = 0;
            while (!source.isEOF()) {
                const juce::juce_wchar ch = source.nextChar();
                if (prev == '*' && ch == '/') break;
                prev = ch;
            }
            return Comment;
        }
    }
    if (c == '"' || c == '\'' || c == '`') { // strings (mini-notation, sample names, template literals)
        source.skip();
        while (!source.isEOF()) {
            const juce::juce_wchar ch = source.nextChar();
            if (ch == '\\') {
                source.skip();
                continue;
            }
            if (ch == c) break;
            if (ch == '\n' && c != '`') break;
        }
        return String;
    }
    if (juce::CharacterFunctions::isDigit(c) ||
        (c == '.' && [&] { auto n = source; n.skip(); return juce::CharacterFunctions::isDigit(n.peekNextChar()); }())) {
        if (c == '0') {
            source.skip();
            const juce::juce_wchar x = source.peekNextChar();
            if (x == 'x' || x == 'X' || x == 'b' || x == 'B' || x == 'o' || x == 'O') {
                source.skip();
                while (juce::CharacterFunctions::isLetterOrDigit(source.peekNextChar())) source.skip();
                return Number;
            }
        }
        while (juce::CharacterFunctions::isDigit(source.peekNextChar()) || source.peekNextChar() == '.' ||
               source.peekNextChar() == '_')
            source.skip();
        if (source.peekNextChar() == 'e' || source.peekNextChar() == 'E') {
            source.skip();
            if (source.peekNextChar() == '-' || source.peekNextChar() == '+') source.skip();
            while (juce::CharacterFunctions::isDigit(source.peekNextChar())) source.skip();
        }
        return Number;
    }
    if (isIdentifierStart(c)) {
        const juce::CodeDocument::Iterator start = source;
        juce::String word;
        while (isIdentifierChar(source.peekNextChar())) word += juce::String::charToString(source.nextChar());
        if (word == "true" || word == "false" || word == "null" || word == "undefined") return Boolean;
        if (isKeyword(word)) return Keyword;
        // a label at the line start: `$: ...` or `name: ...` (not `a ? b : c`, not `x::`)
        if (source.peekNextChar() == ':' && atLineStart(start)) {
            auto n = source;
            n.skip();
            if (n.peekNextChar() != ':') return Label;
        }
        const juce::String before = previousWord(start);
        if (before == "const" || before == "let" || before == "var" || before == "function" || before == "class")
            return Definition;
        if (juce::CharacterFunctions::isUpperCase(word[0]) && word.length() > 1 && !word.containsChar('_') &&
            word.toUpperCase() != word)
            return ClassName;
        return Identifier; // variables, calls and properties share one colour in this theme
    }
    source.skip();
    switch (c) {
        case '(': case ')': case '[': case ']': case '{': case '}': return Bracket;
        case ',': case ';': case '.': return Punctuation;
        case '+': case '-': case '*': case '/': case '%': case '=': case '<': case '>': case '!': case '&':
        case '|': case '^': case '~': case '?': case ':': case '@':
            while (juce::String("+-*/%=<>!&|^?").containsChar(source.peekNextChar())) source.skip();
            return Operator;
        default: return Error;
    }
}

juce::CodeEditorComponent::ColourScheme JsTokeniser::getDefaultColourScheme()
{
    juce::CodeEditorComponent::ColourScheme cs;
    cs.set("Error", theme::foreground);
    cs.set("Comment", theme::comment);
    cs.set("Keyword", theme::keyword);
    cs.set("Identifier", theme::variable);
    cs.set("Definition", theme::definition);
    cs.set("Label", theme::label);
    cs.set("Boolean", theme::boolean);
    cs.set("Number", theme::number);
    cs.set("String", theme::string);
    cs.set("Operator", theme::operatorColour);
    cs.set("Bracket", theme::bracket);
    cs.set("Punctuation", theme::punctuation);
    cs.set("ClassName", theme::className);
    return cs;
}

} // namespace concha::ui
