#pragma once

// "Sample folders" panel: the user's named sample folders (none by default). Each folder has the name
// used in code (samples('Name'), .bank("Name")), its path, and "load at start". Below, the
// user's SynthDef folders (.scsyndef files, read again at each evaluation; none by default).

#include "library/SampleLibrary.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace concha {

class LibraryPanel final : public juce::Component {
public:
    explicit LibraryPanel(std::function<void()> onChanged);

    void paint(juce::Graphics&) override;
    void resized() override;
    std::function<void()> onClose;
    // a folder chosen (by the file chooser, or a tool): named after it, loaded at start
    void addFolderPath(const juce::File& dir);

private:
    struct Row final : juce::Component {
        juce::TextEditor name;
        juce::Label path;
        juce::ToggleButton autoload{"load at start"};
        juce::TextButton remove{"x"};
        void resized() override;
    };

    void rebuild();
    void commit();
    void addFolder();
    void rebuildDefs();
    void addDefFolder();

    std::function<void()> onChanged_;
    std::vector<lib::LibraryFolder> folders_;
    std::vector<std::unique_ptr<Row>> rows_;
    juce::Viewport viewport_;
    juce::Component list_;
    juce::TextButton add_{"Add a sample folder..."}, close_{"Close"};
    juce::Label help_;
    struct DefRow final : juce::Component {
        juce::Label path;
        juce::TextButton remove{"x"};
        void resized() override;
    };
    std::vector<std::string> defFolders_;
    std::vector<std::unique_ptr<DefRow>> defRows_;
    juce::Viewport defsViewport_;
    juce::Component defsList_;
    juce::TextButton addDefs_{"Add a SynthDef folder..."};
    juce::Label defsHelp_;
    juce::Rectangle<int> defsTitle_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace concha
