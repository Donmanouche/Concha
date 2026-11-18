#pragma once

// The JS runtime's natives (PatternHost) served by the sample library.
#include "library/SampleLibrary.h"
#include "pattern/PatternEngine.h"

#include <functional>

namespace concha::lib {

struct LibraryHost final : PatternHost {
    SampleLibrary& library;
    explicit LibraryHost(SampleLibrary& l) : library(l) {}
    bool fetchText(const std::string& url, std::string& text, std::string& error) override
    {
        return library.fetchText(url, text, error);
    }
    std::string libraryMap(const std::string& name, std::string& error) override
    {
        return library.folderMapJson(name, &error);
    }
    std::string libraryAutoload() override { return library.autoloadJson(); }
    uint32_t publishBuffer(const float* data, size_t frames, double sampleRate) override
    {
        return library.publishTransient(data, frames, sampleRate);
    }
    // MIDI CCs and macros of the plug-in instance (set by the processor; none: 0)
    std::function<double(int, int, int)> controls;
    double control(int kind, int a, int b) override { return controls ? controls(kind, a, b) : 0.0; }
    // .analyze(id)'s audio (set by the processor; none: no analysers)
    std::function<int(const std::string&, int, std::vector<float>&)> analysers;
    int analyser(const std::string& id, int kind, std::vector<float>& out) override { return analysers ? analysers(id, kind, out) : 0; }
};

} // namespace concha::lib
