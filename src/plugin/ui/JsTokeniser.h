#pragma once

// Syntax colouring of Strudel code, with the token kinds strudel.cc's CodeMirror (lezer JavaScript)
// gives the theme: keywords, definitions (const x), labels ($:), properties and calls, numbers,
// strings (mini-notation included), comments, operators, brackets, punctuation.

#include <juce_gui_extra/juce_gui_extra.h>

namespace concha::ui {

class JsTokeniser final : public juce::CodeTokeniser {
public:
    enum Token {
        Error, Comment, Keyword, Identifier, Definition, Label, Boolean, Number, String, Operator, Bracket,
        Punctuation, ClassName
    };

    int readNextToken(juce::CodeDocument::Iterator& source) override;
    juce::CodeEditorComponent::ColourScheme getDefaultColourScheme() override;

    static bool isIdentifierStart(juce::juce_wchar c);
    static bool isIdentifierChar(juce::juce_wchar c);
};

} // namespace concha::ui
