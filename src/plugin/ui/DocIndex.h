#pragma once

// Strudel's function reference (js/tools/gen-docs.mjs, compiled into the binary): for the
// Reference panel and the completion popup, as strudel.cc's doc.json serves its own.

#include <juce_core/juce_core.h>

#include <vector>

namespace concha::ui {

struct DocEntry {
    juce::String name;
    juce::StringArray synonyms, tags;
    juce::String description;
    struct Param {
        juce::String name, type, description;
    };
    std::vector<Param> params;
    juce::StringArray examples;
    bool completion = true; // @noAutocomplete / @superdirtOnly: kept out of the completions
};

class DocIndex {
public:
    static const DocIndex& instance();

    const std::vector<DocEntry>& entries() const { return entries_; }
    // name or synonym -> entry (nullptr when unknown)
    const DocEntry* find(const juce::String& name) const;
    // completion labels (names and synonyms), sorted, with their entries
    struct Completion {
        juce::String label;
        const DocEntry* entry;
    };
    const std::vector<Completion>& completions() const { return completions_; }
    // Completions for a typed word: prefix matches first, then other matches.
    std::vector<const Completion*> complete(const juce::String& word, int limit) const;

    // The entry as text: name, synonyms, description, parameters, examples.
    static juce::String describe(const DocEntry& e, const juce::String& as = {});

private:
    DocIndex();
    std::vector<DocEntry> entries_;
    std::vector<Completion> completions_;
};

} // namespace concha::ui
