#include "plugin/ui/DocIndex.h"

#include <algorithm>
#include <string_view>

namespace concha {
std::string_view embeddedDocs(); // build/js/EmbeddedDocs.cpp
}

namespace concha::ui {

const DocIndex& DocIndex::instance()
{
    static const DocIndex index;
    return index;
}

DocIndex::DocIndex()
{
    const auto data = embeddedDocs();
    const juce::var root = juce::JSON::parse(juce::String::fromUTF8(data.data(), int(data.size())));
    if (const auto* arr = root.getArray()) {
        entries_.reserve(size_t(arr->size()));
        for (const auto& v : *arr) {
            DocEntry e;
            e.name = v["n"].toString();
            if (const auto* s = v["s"].getArray())
                for (const auto& x : *s) e.synonyms.add(x.toString());
            if (const auto* t = v["t"].getArray())
                for (const auto& x : *t) e.tags.add(x.toString());
            e.description = v["d"].toString();
            if (const auto* p = v["p"].getArray())
                for (const auto& x : *p) e.params.push_back({x[0].toString(), x[1].toString(), x[2].toString()});
            if (const auto* ex = v["e"].getArray())
                for (const auto& x : *ex) e.examples.add(x.toString());
            e.completion = !(bool)v["x"];
            entries_.push_back(std::move(e));
        }
    }
    // labels: every name and synonym once (getSynonymDoc in autocomplete.mjs)
    juce::StringArray seen;
    for (const auto& e : entries_) {
        if (!e.completion) continue;
        juce::StringArray labels{e.name};
        labels.addArray(e.synonyms);
        for (const auto& l : labels)
            if (l.isNotEmpty() && !seen.contains(l)) {
                seen.add(l);
                completions_.push_back({l, &e});
            }
    }
    std::sort(completions_.begin(), completions_.end(),
              [](const Completion& a, const Completion& b) { return a.label.compareNatural(b.label) < 0; });
}

const DocEntry* DocIndex::find(const juce::String& name) const
{
    for (const auto& e : entries_)
        if (e.name == name) return &e;
    for (const auto& e : entries_)
        if (e.synonyms.contains(name)) return &e;
    return nullptr;
}

std::vector<const DocIndex::Completion*> DocIndex::complete(const juce::String& word, int limit) const
{
    std::vector<const Completion*> out;
    if (word.isEmpty()) return out;
    for (const auto& c : completions_) // exact case prefix, then any case prefix, then contains
        if (c.label.startsWith(word)) out.push_back(&c);
    for (const auto& c : completions_)
        if (!c.label.startsWith(word) && c.label.startsWithIgnoreCase(word)) out.push_back(&c);
    for (const auto& c : completions_)
        if (!c.label.startsWithIgnoreCase(word) && c.label.containsIgnoreCase(word)) out.push_back(&c);
    if (int(out.size()) > limit) out.resize(size_t(limit));
    return out;
}

juce::String DocIndex::describe(const DocEntry& e, const juce::String& as)
{
    juce::String s;
    const juce::String title = as.isNotEmpty() ? as : e.name;
    s << title << "\n";
    juce::StringArray syn{e.name};
    syn.addArray(e.synonyms);
    syn.removeString(title);
    if (!syn.isEmpty()) s << "Synonyms: " << syn.joinIntoString(", ") << "\n";
    s << "\n";
    if (e.description.isNotEmpty()) s << e.description << "\n\n";
    if (!e.params.empty()) {
        s << "Parameters\n";
        for (const auto& p : e.params) {
            s << "  " << p.name;
            if (p.type.isNotEmpty()) s << "  (" << p.type << ")";
            if (p.description.isNotEmpty()) s << "\n      " << p.description.replace("\n", "\n      ");
            s << "\n";
        }
        s << "\n";
    }
    if (!e.examples.isEmpty()) {
        s << "Examples\n";
        for (const auto& x : e.examples) s << "\n" << x << "\n";
    }
    return s.trimEnd();
}

} // namespace concha::ui
