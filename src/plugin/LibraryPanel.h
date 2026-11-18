#pragma once

// "Samples" panel: the user's named sample folders (none by default). Each folder has the name
// used in code (samples('Name'), .bank("Name")), its path, and "load at start".

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

private:
    struct Row final : juce::Component {
        juce::TextEditor name;
        juce::Label path;
        juce::ToggleButton autoload{"au demarrage"};
        juce::TextButton remove{"x"};
        void resized() override;
    };

    void rebuild();
    void commit();
    void addFolder();

    std::function<void()> onChanged_;
    std::vector<lib::LibraryFolder> folders_;
    std::vector<std::unique_ptr<Row>> rows_;
    juce::Viewport viewport_;
    juce::Component list_;
    juce::TextButton add_{"Ajouter un dossier..."}, close_{"Fermer"};
    juce::Label help_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace concha
