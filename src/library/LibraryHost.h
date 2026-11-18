#pragma once

// The JS runtime's natives (PatternHost) served by the sample library.
#include "library/SampleLibrary.h"
#include "pattern/PatternEngine.h"

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
};

} // namespace concha::lib
