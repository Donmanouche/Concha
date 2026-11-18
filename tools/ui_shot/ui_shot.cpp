// Renders Concha's editor off screen to a PNG, with a pattern playing (free run), to check the
// UI without a DAW: concha_ui_shot <code.js> <out.png> [width height] [ms] [tab | library] [text to type at the end]
#include "plugin/LibraryPanel.h"
#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ui/CodeView.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <thread>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <code.js> <out.png> [width height] [tab]\n", argv[0]);
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    auto proc = std::make_unique<concha::ConchaProcessor>();
    proc->setPlayConfigDetails(0, 2 * concha::kNumStereoBuses, 48000, 256);
    proc->prepareToPlay(48000, 256);
    if (auto* fr = proc->state().getParameter("freeRun")) fr->setValueNotifyingHost(1.0f);
    const juce::String code = juce::File(juce::String(argv[1])).loadFileAsString();
    proc->setCode(code);
    if (argc > 6) proc->state().state.setProperty("panelTab", juce::String(argv[6]).getIntValue(), nullptr);

    std::atomic<bool> run{true};
    std::thread audio([&] { // the DAW's audio thread, in real time
        juce::AudioBuffer<float> buffer(2 * concha::kNumStereoBuses, 256);
        juce::MidiBuffer midi;
        auto next = std::chrono::steady_clock::now();
        while (run) {
            proc->processBlock(buffer, midi);
            next += std::chrono::microseconds(256 * 1000000 / 48000);
            std::this_thread::sleep_until(next);
        }
    });
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc->createEditor());
    const int w = argc > 4 ? juce::String(argv[3]).getIntValue() : 1100, h = argc > 4 ? juce::String(argv[4]).getIntValue() : 680;
    editor->setSize(w, h);
    // let the engine start, evaluate, play a while: the message loop runs the editor's timer
    const int playMs = argc > 5 ? juce::String(argv[5]).getIntValue() : 2500;
    const juce::String outPath(argv[2]);
    juce::Timer::callAfterDelay(1500, [&] {
        proc->evaluate(code);
        juce::Timer::callAfterDelay(playMs, [&] {
            if (argc > 7) { // type into the code view (completion check)
                std::function<concha::ui::CodeView*(juce::Component*)> find = [&](juce::Component* c) -> concha::ui::CodeView* {
                    if (auto* v = dynamic_cast<concha::ui::CodeView*>(c)) return v;
                    for (auto* ch : c->getChildren())
                        if (auto* v = find(ch)) return v;
                    return nullptr;
                };
                if (auto* view = find(editor.get())) {
                    view->moveCaretToEnd(false);
                    for (auto ch : juce::String::fromUTF8(argv[7])) view->keyPressed(juce::KeyPress(0, {}, ch));
                }
            }
            juce::Timer::callAfterDelay(400, [&] {
                // tab "library": the sample / SynthDef folders panel instead of the window
                std::unique_ptr<concha::LibraryPanel> library;
                if (argc > 6 && juce::String(argv[6]) == "library") {
                    library = std::make_unique<concha::LibraryPanel>([] {});
                    library->setSize(editor->getWidth(), editor->getHeight());
                }
                juce::Component& target = library ? static_cast<juce::Component&>(*library) : *editor;
                const juce::Image shot = target.createComponentSnapshot(target.getLocalBounds(), true, 1.0f);
                juce::File out{outPath};
                out.deleteFile();
                juce::FileOutputStream os{out};
                juce::PNGImageFormat().writeImageToStream(shot, os);
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    run = false;
    audio.join();
    editor.reset();
    proc.reset();
    std::printf("wrote %s\n", argv[2]);
    return 0;
}
