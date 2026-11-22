#include "plugin/LibraryPanel.h"

namespace concha {

namespace {
const juce::Colour kPanel(0xf01b1d22), kText(0xffd8d8d8), kDim(0xff8a8f98);
}

void LibraryPanel::Row::resized()
{
    auto r = getLocalBounds().reduced(2);
    remove.setBounds(r.removeFromRight(28));
    autoload.setBounds(r.removeFromRight(120));
    name.setBounds(r.removeFromLeft(160));
    r.removeFromLeft(8);
    path.setBounds(r);
}

void LibraryPanel::DefRow::resized()
{
    auto r = getLocalBounds().reduced(2);
    remove.setBounds(r.removeFromRight(28));
    path.setBounds(r);
}

LibraryPanel::LibraryPanel(std::function<void()> onChanged) : onChanged_(std::move(onChanged))
{
    help_.setText("Named sample folders. In code: samples('Name') or samples('lib:Name'), then s(\"sound\"). "
                  "Tick \"load at start\" to have them without samples(). A sub-folder = a sound; "
                  "Bank/bank-bd/*.wav gives .bank(\"Bank\").",
                  juce::dontSendNotification);
    help_.setColour(juce::Label::textColourId, kDim);
    help_.setJustificationType(juce::Justification::topLeft);
    add_.onClick = [this] { addFolder(); };
    close_.onClick = [this] {
        if (onClose) onClose();
    };
    viewport_.setViewedComponent(&list_, false);
    addAndMakeVisible(help_);
    addAndMakeVisible(viewport_);
    addAndMakeVisible(add_);
    addAndMakeVisible(close_);
    folders_ = lib::SampleLibrary::instance().folders();
    rebuild();
    defsHelp_.setText("Folders of SynthDefs (.scsyndef, e.g. from SynthDef(...).writeDefFile(path) in SuperCollider; "
                      "without a path, or with .store, SuperCollider writes them in its own synthdefs folder, where "
                      "\"Add\" opens). "
                      "Each SynthDef plays with s(\"name\") like StrudelDirt's instruments; its controls take the "
                      "pattern's values of the same name. Changed files are read again at each evaluation.",
                      juce::dontSendNotification);
    defsHelp_.setColour(juce::Label::textColourId, kDim);
    defsHelp_.setJustificationType(juce::Justification::topLeft);
    addDefs_.onClick = [this] { addDefFolder(); };
    defsViewport_.setViewedComponent(&defsList_, false);
    addAndMakeVisible(defsHelp_);
    addAndMakeVisible(defsViewport_);
    addAndMakeVisible(addDefs_);
    defFolders_ = lib::SampleLibrary::instance().synthDefFolders();
    rebuildDefs();
}

void LibraryPanel::paint(juce::Graphics& g)
{
    g.fillAll(kPanel);
    g.setColour(kText);
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("Sample folders", 16, 10, 300, 24, juce::Justification::centredLeft);
    if (folders_.empty()) {
        g.setColour(kDim);
        g.drawText("No folder yet.", viewport_.getBounds().reduced(8), juce::Justification::topLeft);
    }
    g.setColour(kText);
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("SynthDef folders", defsTitle_, juce::Justification::centredLeft);
    if (defFolders_.empty()) {
        g.setColour(kDim);
        g.setFont(juce::Font(juce::FontOptions(15.0f)));
        g.drawText("No folder yet.", defsViewport_.getBounds().reduced(8), juce::Justification::topLeft);
    }
}

void LibraryPanel::resized()
{
    auto r = getLocalBounds().reduced(16);
    r.removeFromTop(30);
    help_.setBounds(r.removeFromTop(40));
    auto bottom = r.removeFromBottom(32);
    close_.setBounds(bottom.removeFromRight(100));
    add_.setBounds(bottom.removeFromLeft(220));
    bottom.removeFromLeft(8);
    addDefs_.setBounds(bottom.removeFromLeft(220));
    r.removeFromBottom(8);
    // SynthDef folders: the lower part
    auto defs = r.removeFromBottom(juce::jmax(150, r.getHeight() * 2 / 5));
    defs.removeFromTop(8);
    defsTitle_ = defs.removeFromTop(26);
    defsHelp_.setBounds(defs.removeFromTop(40));
    defsViewport_.setBounds(defs);
    const int dw = juce::jmax(200, defs.getWidth() - defsViewport_.getScrollBarThickness());
    defsList_.setSize(dw, int(defRows_.size()) * 32);
    for (size_t i = 0; i < defRows_.size(); ++i) defRows_[i]->setBounds(0, int(i) * 32, dw, 30);
    viewport_.setBounds(r);
    const int w = juce::jmax(200, r.getWidth() - viewport_.getScrollBarThickness());
    list_.setSize(w, int(rows_.size()) * 32);
    for (size_t i = 0; i < rows_.size(); ++i) rows_[i]->setBounds(0, int(i) * 32, w, 30);
}

void LibraryPanel::rebuild()
{
    rows_.clear();
    for (size_t i = 0; i < folders_.size(); ++i) {
        auto row = std::make_unique<Row>();
        row->name.setText(juce::String::fromUTF8(folders_[i].name.c_str()), false);
        row->name.onFocusLost = row->name.onReturnKey = [this, i] {
            if (i >= folders_.size() || i >= rows_.size()) return;
            const auto n = rows_[i]->name.getText().trim().toStdString();
            if (!n.empty() && n != folders_[i].name) {
                folders_[i].name = n;
                commit();
            }
        };
        { // the path, and what the folder gives: its sounds, or why none
            std::string error;
            const int count = lib::SampleLibrary::instance().folderSoundCount(folders_[i].name, error);
            const juce::String path = juce::String::fromUTF8(folders_[i].path.c_str());
            row->path.setText(count > 0 ? juce::String(count) + " sounds - " + path : "no sounds: " + juce::String::fromUTF8(error.c_str()) + " - " + path,
                              juce::dontSendNotification);
            row->path.setColour(juce::Label::textColourId, count > 0 ? kDim : juce::Colour(0xffff5370));
            row->path.setTooltip(row->path.getText());
        }
        row->autoload.setToggleState(folders_[i].autoload, juce::dontSendNotification);
        row->autoload.onClick = [this, i] {
            if (i >= folders_.size()) return;
            folders_[i].autoload = rows_[i]->autoload.getToggleState();
            commit();
        };
        row->remove.setTooltip("Remove this folder from the library (its files are not touched)");
        row->remove.onClick = [this, i] {
            if (i >= folders_.size()) return;
            folders_.erase(folders_.begin() + long(i));
            commit();
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<LibraryPanel>(this)] {
                if (safe) safe->rebuild();
            });
        };
        for (juce::Component* c : {static_cast<juce::Component*>(&row->name), static_cast<juce::Component*>(&row->path),
                                   static_cast<juce::Component*>(&row->autoload), static_cast<juce::Component*>(&row->remove)})
            row->addAndMakeVisible(c);
        list_.addAndMakeVisible(*row);
        rows_.push_back(std::move(row));
    }
    resized();
    repaint();
}

void LibraryPanel::rebuildDefs()
{
    defRows_.clear();
    for (size_t i = 0; i < defFolders_.size(); ++i) {
        auto row = std::make_unique<DefRow>();
        row->path.setText(juce::String::fromUTF8(defFolders_[i].c_str()), juce::dontSendNotification);
        row->path.setColour(juce::Label::textColourId, kText);
        row->path.setTooltip(row->path.getText());
        row->remove.setTooltip("Remove this folder (its files are not touched; SynthDefs already read stay until restart)");
        row->remove.onClick = [this, i] {
            if (i >= defFolders_.size()) return;
            defFolders_.erase(defFolders_.begin() + long(i));
            lib::SampleLibrary::instance().setSynthDefFolders(defFolders_);
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<LibraryPanel>(this)] {
                if (safe) safe->rebuildDefs();
            });
        };
        row->addAndMakeVisible(row->path);
        row->addAndMakeVisible(row->remove);
        defsList_.addAndMakeVisible(*row);
        defRows_.push_back(std::move(row));
    }
    resized();
    repaint();
}

void LibraryPanel::addDefFolder()
{
    // where SuperCollider writes them (writeDefFile() without a folder, .store): a hidden folder
    // the dialog would not show from the home folder
    juce::File start = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
#if JUCE_LINUX || JUCE_BSD
    if (auto sc = start.getChildFile(".local/share/SuperCollider/synthdefs"); sc.isDirectory()) start = sc;
#elif JUCE_MAC
    if (auto sc = start.getChildFile("Library/Application Support/SuperCollider/synthdefs"); sc.isDirectory()) start = sc;
#elif JUCE_WINDOWS
    if (auto sc = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("SuperCollider/synthdefs");
        sc.isDirectory())
        start = sc;
#endif
    chooser_ = std::make_unique<juce::FileChooser>("SynthDef folder", start);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
                              const auto dir = fc.getResult();
                              if (!dir.isDirectory()) return;
                              const std::string path = dir.getFullPathName().toStdString();
                              for (const auto& p : defFolders_)
                                  if (p == path) return;
                              defFolders_.push_back(path);
                              lib::SampleLibrary::instance().setSynthDefFolders(defFolders_);
                              rebuildDefs();
                              if (onChanged_) onChanged_(); // evaluate again: the new SynthDefs
                          });
}

void LibraryPanel::commit()
{
    lib::SampleLibrary::instance().setFolders(folders_);
    if (onChanged_) onChanged_();
}

void LibraryPanel::addFolder()
{
    chooser_ = std::make_unique<juce::FileChooser>("Sample folder", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) { addFolderPath(fc.getResult()); });
}

void LibraryPanel::addFolderPath(const juce::File& dir)
{
                              if (!dir.isDirectory()) return;
                              std::string name = dir.getFileName().retainCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-").toStdString(); // usable in code
                              if (name.empty()) name = "samples";
                              // unique name
                              std::string unique = name;
                              for (int k = 2;; ++k) {
                                  bool taken = false;
                                  for (const auto& f : folders_)
                                      if (juce::String(f.name).equalsIgnoreCase(juce::String(unique))) taken = true;
                                  if (!taken) break;
                                  unique = name + std::to_string(k);
                              }
                              folders_.push_back({unique, dir.getFullPathName().toStdString(), true});
                              commit();
                              rebuild();
}

} // namespace concha
