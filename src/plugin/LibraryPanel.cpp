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

LibraryPanel::LibraryPanel(std::function<void()> onChanged) : onChanged_(std::move(onChanged))
{
    help_.setText("Dossiers de samples nommes. Dans le code : samples('Nom') ou samples('lib:Nom'), puis s(\"son\"). "
                  "Coche \"au demarrage\" pour les avoir sans samples(). Un sous-dossier = un son ; "
                  "Banque/banque-bd/*.wav donne .bank(\"Banque\").",
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
}

void LibraryPanel::paint(juce::Graphics& g)
{
    g.fillAll(kPanel);
    g.setColour(kText);
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("Samples", 16, 10, 300, 24, juce::Justification::centredLeft);
    if (folders_.empty()) {
        g.setColour(kDim);
        g.drawText("Aucun dossier pour l'instant.", viewport_.getBounds().reduced(8), juce::Justification::topLeft);
    }
}

void LibraryPanel::resized()
{
    auto r = getLocalBounds().reduced(16);
    r.removeFromTop(30);
    help_.setBounds(r.removeFromTop(40));
    auto bottom = r.removeFromBottom(32);
    close_.setBounds(bottom.removeFromRight(100));
    add_.setBounds(bottom.removeFromLeft(200));
    r.removeFromBottom(8);
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
        row->path.setText(juce::String::fromUTF8(folders_[i].path.c_str()), juce::dontSendNotification);
        row->path.setColour(juce::Label::textColourId, kDim);
        row->path.setTooltip(row->path.getText());
        row->autoload.setToggleState(folders_[i].autoload, juce::dontSendNotification);
        row->autoload.onClick = [this, i] {
            if (i >= folders_.size()) return;
            folders_[i].autoload = rows_[i]->autoload.getToggleState();
            commit();
        };
        row->remove.setTooltip("Retirer ce dossier de la bibliotheque (les fichiers ne sont pas touches)");
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

void LibraryPanel::commit()
{
    lib::SampleLibrary::instance().setFolders(folders_);
    if (onChanged_) onChanged_();
}

void LibraryPanel::addFolder()
{
    chooser_ = std::make_unique<juce::FileChooser>("Dossier de samples", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
                              const auto dir = fc.getResult();
                              if (!dir.isDirectory()) return;
                              std::string name = dir.getFileName().removeCharacters(" '\"").toStdString();
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
                          });
}

} // namespace concha
